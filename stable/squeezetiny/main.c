/*
 *  Squeezelite - lightweight headless squeezebox emulator
 *
 *  (c) Adrian Smith 2012-2014, triode1@btinternet.com
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 *
 */

 /*
 TODO
 - Manage order of destruction fo the machine and potential race condition with
 objects like mutexes that are used in common. typically all other processes should
 wait for proto to be finshed before destroying their contextes
 */

#include "squeezelite.h"

#include <math.h>
#include <signal.h>

#define TITLE "Squeezelite " VERSION ", Copyright 2012-2014 Adrian Smith + Philippe."

#ifndef NO_CODEC
#define CODECS_BASE "flac,pcm,mp3,ogg,aac"
#else
#define CODECS_BASE "pcm,mp3"
#endif

#if FFMPEG
#define CODECS_FF   ",wma,alac"
#else
#define CODECS_FF   ""
#endif
#if DSD
#define CODECS_DSD  ",dsd"
#else
#define CODECS_DSD  ""
#endif
#define CODECS_MP3  " (mad,mpg for specific mp3 codec)"

#define CODECS CODECS_BASE CODECS_FF CODECS_DSD CODECS_MP3

#define LOCK_S   mutex_lock(ctx->streambuf->mutex)
#define UNLOCK_S mutex_unlock(ctx->streambuf->mutex)
#if 0
#define LOCK_O   mutex_lock(ctx->outputbuf->mutex)
#define UNLOCK_O mutex_unlock(ctx->outputbuf->mutex)
#else
#define LOCK_O
#define UNLOCK_O
#endif
#define LOCK_D   mutex_lock(ctx->decode.mutex)
#define UNLOCK_D mutex_unlock(ctx->decode.mutex)
#define LOCK_P   mutex_lock(ctx->mutex)
#define UNLOCK_P mutex_unlock(ctx->mutex)

struct thread_ctx_s thread_ctx[MAX_PLAYER];

/*----------------------------------------------------------------------------*/
/* locals */
/*----------------------------------------------------------------------------*/
static char 			_gl_server[SERVER_NAME_LEN + 1];
static char 			*gl_server = NULL;
static u8_t				gl_last_mac[6];
static unsigned 		gl_rate_delay = 0;
static char 			*gl_resample = NULL;
static bool 			gl_dop	= false;
static unsigned 		gl_dop_delay = 0;
static char				gl_include_codecs[SQ_STR_LENGTH];
static char				gl_exclude_codecs[SQ_STR_LENGTH];

/*----------------------------------------------------------------------------*/
/* locals */
/*----------------------------------------------------------------------------*/
static bool sq_wipe_device(struct thread_ctx_s *ctx);

static log_level	loglevel = lWARN;

/*---------------------------------------------------------------------------*/
void sq_stop(void) {
	int i;

	for (i = 0; i < MAX_PLAYER; i++) {
		if (thread_ctx[i].in_use) {
			sq_wipe_device(&thread_ctx[i]);
		}
	}
#if WIN
	winsock_close();
#endif
}

/*--------------------------------------------------------------------------*/
bool sq_wipe_device(struct thread_ctx_s *ctx) {
	int i;
	bool rc;
	char buf[SQ_STR_LENGTH];

	ctx->callback = NULL;
	ctx->in_use = false;
	rc = slimproto_close(ctx);
	if (!rc) {
		LOG_WARN("[%p] not able to end slimproto (1st)", ctx);
	}
	output_mr_close(ctx);
	decode_close(ctx);
	stream_close(ctx);
	if (!rc) {
		rc = slimproto_close(ctx);
		if (!rc) {
			LOG_ERROR("[%p] not able to end slimproto (2nd)", ctx);
		}
	}
	for (i = 0; i < 2; i++) {
		if (ctx->out_ctx[i].read_file) fclose (ctx->out_ctx[i].read_file);
		if (ctx->out_ctx[i].write_file) fclose (ctx->out_ctx[i].write_file);
		sprintf(buf, "%s/%s", ctx->config.buffer_dir, ctx->out_ctx[i].buf_name);
		remove(buf);
	}
	memset(ctx, 0, sizeof(struct thread_ctx_s));
	return rc;
}

/*--------------------------------------------------------------------------*/
bool sq_delete_device(sq_dev_handle_t handle) {
	struct thread_ctx_s *ctx = &thread_ctx[handle - 1];

	return sq_wipe_device(ctx);
}


/*---------------------------------------------------------------------------*/
void main_loglevel(log_level level)
{
	LOG_WARN("main change log", NULL);
	loglevel = level;
}

/*---------------------------------------------------------------------------*/
void *sq_urn2MR(const char *urn)
{
	int i = 0;
	out_ctx_t *out = NULL;

	for (i = 0; i < MAX_PLAYER && !out; i++) {
		if (!thread_ctx[i].in_use) continue;
		if (strstr(urn, thread_ctx[i].out_ctx[0].buf_name)) out = &thread_ctx[i].out_ctx[0];
		if (strstr(urn, thread_ctx[i].out_ctx[1].buf_name)) out = &thread_ctx[i].out_ctx[1];
	}

	return (out) ? thread_ctx[i-1].MR : NULL;
}

bool cli_send_cmd(char *cmd, struct thread_ctx_s *ctx)
{
	// this is still thread safe as there is one socket per device, so seqN can
	// the same between devices
	static u16_t tagN;
	char tag[16];
	char packet[128];
	char *res, *p;
	int wait, len;
	bool rc = false;

	mutex_lock(ctx->cli_mutex);

	wait = ctx->config.max_read_wait;
	sprintf(tag, "%d\n", tagN++);
	len = sprintf(packet, "%s%s\n", tag, cmd);
	send_packet(packet, len, ctx->cli_sock);

	// first receive the tag and then point to the last '\n'
	len = 0;
	p = res = NULL;
	while (wait--)	{
		len += recv(ctx->cli_sock, packet + len, 128-1 - len, 0);
		packet[len] = '\0';
		if (!p) {
			p = strstr(packet, tag);
			if (p) p += strlen(tag);
		}
		if (p) {
			res = strchr(p, '\n');
			if (res) break;
		}
		usleep(50000);
	}

	if (res) {
		*res = '\0';
		while (*--res != ' ' && res > packet);
		strcpy(cmd, res);
		rc = true;
	}

	mutex_unlock(ctx->cli_mutex);
	return rc;
}


/*--------------------------------------------------------------------------*/
u32_t sq_get_time(sq_dev_handle_t handle)
{
	struct thread_ctx_s *ctx = &thread_ctx[handle - 1];
	char cmd[128];

	if (!handle || !ctx->cli_sock) {
		LOG_ERROR("[%p]: no handle or CLI socket %d", ctx, handle);
		return 0;
	}

	sprintf(cmd, "%s time ?", ctx->cli_id);
	if (cli_send_cmd(cmd, ctx)) {
		return (u32_t) (atof(cmd) * 1000);
	}
	else {
		LOG_ERROR("[%p] cannot gettime", ctx);
		return 0;
	}
}

/*---------------------------------------------------------------------------*/
bool sq_set_time(sq_dev_handle_t handle, u32_t time)
{
	struct thread_ctx_s *ctx = &thread_ctx[handle - 1];
	char cmd[128];

	if (!handle || !ctx->cli_sock) {
		LOG_ERROR("[%p]: no handle or cli socket %d", ctx, handle);
		return false;
	}

	sprintf(cmd, "%s time %.1lf", ctx->cli_id, (double) time / 1000);

	if (cli_send_cmd(cmd, ctx)) {
		LOG_INFO("[%p] set time %d", ctx, time);
		return true;
	}
	else {
		LOG_ERROR("[%p] cannot settime %d", ctx, time);
		return false;
	}
}

/*---------------------------------------------------------------------------*/
char *sq_content_type(const char *urn)
{
	int i = 0;
	out_ctx_t *out = NULL;
	char *p;

	for (i = 0; i < MAX_PLAYER && !out; i++) {
		if (!thread_ctx[i].in_use) continue;
		if (strstr(urn, thread_ctx[i].out_ctx[0].buf_name)) out = &thread_ctx[i].out_ctx[0];
		if (strstr(urn, thread_ctx[i].out_ctx[1].buf_name)) out = &thread_ctx[i].out_ctx[1];
	}

	if (out) {
		p = malloc(strlen(out->content_type) + 1);
		strcpy(p, out->content_type);
		return p;
	}
	else
		return strdup("audio/unknown");
}


/*---------------------------------------------------------------------------*/
void *sq_open(const char *urn)
{
	int i = 0;
	out_ctx_t *out = NULL;

	for (i = 0; i < MAX_PLAYER && !out; i++) {
		if (!thread_ctx[i].in_use) continue;
		if (strstr(urn, thread_ctx[i].out_ctx[0].buf_name)) out = &thread_ctx[i].out_ctx[0];
		if (strstr(urn, thread_ctx[i].out_ctx[1].buf_name)) out = &thread_ctx[i].out_ctx[1];
	}

	if (out) {
		char buf[SQ_STR_LENGTH];
		struct thread_ctx_s *ctx = out->owner; 		// for the macro to work ... ugh

		LOCK_S;LOCK_O;
		if (!out->read_file) {
			sprintf(buf, "%s/%s", thread_ctx[i-1].config.buffer_dir, out->buf_name);
			out->read_file = fopen(buf, "rb");
			// the read_count_t is only set at the setURI, not at every close/open !
			out->read_count = 0;
			LOG_INFO("[%p]: open", out->owner);
			if (!out->read_file) out = NULL;
		}
		// Some clients try to open 2 sessions : do not allow that
		else out = NULL;
		UNLOCK_S;UNLOCK_O;
	}

	return out;
}

/*---------------------------------------------------------------------------*/
bool sq_close(void *desc)
{
	out_ctx_t *p = (out_ctx_t*) desc;

	if (&p->owner->out_ctx[p->idx] != p) {
		LOG_ERROR("[%p]: unknow output context %p", p->owner, p);
		return false;
	}
	else {
		struct thread_ctx_s *ctx = p->owner; 		// for the macro to work ... ugh
		LOCK_S;LOCK_O;
		if (p->read_file) fclose(p->read_file);
		p->read_file = NULL;
		p->read_count_t -= p->read_count;
		LOG_INFO("[%p]: read total:%Ld", p->owner, p->read_count_t);
		UNLOCK_S;UNLOCK_O;
	}

	return true;
}

/*---------------------------------------------------------------------------*/
int sq_seek(void *desc, off_t bytes, int from)
{
	out_ctx_t *p = (out_ctx_t*) desc;
	int rc = -1;

	if (&p->owner->out_ctx[p->idx] != p) {
		LOG_ERROR("[%p]: unknow output context %p", p->owner, p);
	}
	else {
		struct thread_ctx_s *ctx = p->owner; 		// for the macro to work ... ugh
		LOCK_S;LOCK_O;

		// not clear what happen during a SEEK_SET vs a SEEK_CUR
		bytes -= p->read_count_t - p->read_count;
		if (bytes < 0) bytes = 0;
		LOG_INFO("[%p]: adjusting %d", p->owner, bytes);
		rc = fseek(p->read_file, bytes, from);
		p->read_count += bytes;
		p->read_count_t += bytes;
		UNLOCK_S;UNLOCK_O;
	}

	return rc;
}

/*---------------------------------------------------------------------------*/
int sq_read(void *desc, void *dst, unsigned bytes)
{
	unsigned wait, read_b = 0;
	out_ctx_t *p = (out_ctx_t*) desc;
	struct thread_ctx_s *ctx = p->owner;

	if (&p->owner->out_ctx[p->idx] != p) {
		LOG_ERROR("[%p]: unknow output context %p", p->owner, p);
		return -1;
	}

	switch (ctx->config.max_get_bytes) {
		case 0:
			bytes = ctx->stream.threshold ? min(ctx->stream.threshold, bytes) : bytes;
			break;
		case -1:
			break;
		default:
			bytes= min((unsigned) ctx->config.max_get_bytes, bytes);
			break;
	}

	wait = ctx->config.max_read_wait;
	if (ctx->config.mode == SQ_STREAM) {
		do
		{
			LOCK_S;LOCK_O;
			if (p->read_file) read_b += fread(dst, 1, bytes, p->read_file);
			UNLOCK_S;LOCK_O;
			LOG_SDEBUG("[%p] read %u bytes at %d", ctx, read_b, wait);
			usleep(50000);
		} while (!read_b && p->write_file && wait--);
	}

	LOCK_S;LOCK_O;

	p->read_count += read_b;
	p->read_count_t += read_b;

	/*
	stream disconnected and not full data request served ==> end of stream
	but ... only inform the controller when a last read with 0 has been
	made, otherwise the upnp device will make another read attempt, read in
	the nextURI buffer and miss the end of the current track
	*/
	if (wait && !read_b && !p->write_file) {
		ctx->read_ended = true;
		LOG_INFO("[%p]: read (end of track) w:%d", ctx, wait);
	}

	// exit on timeout and not read enough data ==> underrun
	if (!wait && !read_b) {
		ctx->read_to = true;
		LOG_ERROR("[%p]: underrun read:%d (r:%d)", ctx, read_b, bytes);
	}
	UNLOCK_S;UNLOCK_O;

	LOG_INFO("[%p]: read %d (r:%d w:%d)", ctx, bytes, read_b, wait);

	return read_b;
}


/*---------------------------------------------------------------------------*/
void sq_notify(sq_dev_handle_t handle, void *caller_id, sq_event_t event, int cookie, void *param)
{
	struct thread_ctx_s *ctx = &thread_ctx[handle - 1];

	LOG_SDEBUG("[%p] notif %d", ctx, event);

	// squeezelite device has not started yet or is off ...
	if (!ctx->running || !ctx->on || !handle) return;

	switch (event) {
		case SQ_SETURI: break;
		case SQ_UNPAUSE:
		case SQ_PLAY:
			// received play confirmation ==> enable time adjustement
			LOCK_S;
			LOG_INFO("[%p] uPNP playing notif", ctx);
			ctx->play_running = true;
			UNLOCK_S;
			wake_controller(ctx);
			break;
		case SQ_PAUSE:
			LOG_INFO("[%p] uPNP pause notif", ctx);
			break;
		case SQ_STOP:
			LOG_INFO("[%p] uPNP notify STOP", ctx);
			LOCK_S;
			if (ctx->play_running) {
				ctx->track_ended = true;
				if (ctx->config.mode == SQ_DIRECT) ctx->read_ended = true;
			 }
			ctx->play_running = false;
			UNLOCK_S;
			wake_controller(ctx);
			break;
		case SQ_SEEK: break;
		case SQ_VOLUME: break;
		case SQ_TIME: {
			int time = *((unsigned*) param);

			LOG_DEBUG("[%p] time %d %d", ctx, ctx->ms_played, time*1000);

			LOCK_S;
			if (ctx->play_running && ctx->config.mode == SQ_DIRECT) {
				int delta = (int) ctx->ms_played - (int) time*1000;
				if (delta > 10) {
					LOG_INFO("[%p] End of track by time rollover %d", ctx, delta);
					ctx->read_ended = true;
					ctx->ms_played = 0;
					ctx->track_new = true;
					wake_controller(ctx);
				}
			}
			UNLOCK_S;
			ctx->ms_played = time * 1000;
			break;
		}
		case SQ_TRACK_CHANGE:
			LOCK_S;
			if (ctx->play_running) {
				LOG_INFO("[%p] End of track by track change", ctx);
				if (ctx->config.mode == SQ_DIRECT) ctx->read_ended = true;
				ctx->ms_played = 0;
				ctx->track_new = true;
				wake_controller(ctx);
			}
			UNLOCK_S;
			break;

		default: break;
	 }
 }

/*---------------------------------------------------------------------------*/
void sq_reset(sq_dev_handle_t handle)
{
	 slimproto_reset(&thread_ctx[handle-1]);
}

/*---------------------------------------------------------------------------*/
void sq_init(char *server, u8_t mac[6], sq_log_level_t *log)
{
	if (server) {
		strcpy(_gl_server, server);
		gl_server = _gl_server;
	}
	else gl_server = NULL;

#if 0
	if (gl_resample) {
		unsigned scale = 8;

		scale = gl_rate[0] / 44100;
		if (scale > 8) scale = 8;
		if (scale < 1) scale = 1;
		gl_output_buf_size *= scale;
	 }
#endif

	// mac cannot be NULL
	memcpy(gl_last_mac, mac, 6);

#if WIN
	winsock_init();
#endif

	loglevel = log->general;
	slimproto_loglevel(log->slimproto);
	stream_loglevel(log->stream);
//	output_init(gl_log.output, true);
	output_mr_loglevel(log->output);
	decode_init(log->decode, gl_include_codecs, gl_exclude_codecs, true);
}

/*---------------------------------------------------------------------------*/
sq_dev_handle_t sq_reserve_device(void *MR, sq_callback_t callback)
{
	int i, ctx_i;
	struct thread_ctx_s *ctx;

	/* find a free thread context - this must be called in a LOCKED context */
	for  (ctx_i = 0; ctx_i < MAX_PLAYER; ctx_i++)
		if (!thread_ctx[ctx_i].in_use) break;

	if (ctx_i < MAX_PLAYER)
	{
		memset(&thread_ctx[ctx_i], 0, sizeof(struct thread_ctx_s));
		thread_ctx[ctx_i].in_use = true;
	}
	else return false;

	ctx = thread_ctx + ctx_i;
	ctx->self = ctx_i + 1;
	ctx->on = false;
	ctx->callback = callback;
	ctx->MR = MR;

	return ctx_i + 1;
}


/*---------------------------------------------------------------------------*/
bool sq_run_device(sq_dev_handle_t handle, char *name, u8_t *mac, sq_dev_param_t *param)
{
	int i;
	struct thread_ctx_s *ctx = &thread_ctx[handle - 1];
	u8_t l_mac[6];
	u16_t mac_sum;
	char buf[SQ_STR_LENGTH];

#if 0
	// set the output buffer size if not specified on the command line, take account of resampling
	if (gl_resample) {
		unsigned scale = 8;

		scale = gl_rate[0] / 44100;
		if (scale > 8) scale = 8;
		if (scale < 1) scale = 1;
		l_output_buf_size *= scale;
	 }
#endif

	for (i = 0, mac_sum =0; i < 5; i++) mac_sum += mac[i];
	if (mac && mac_sum) memcpy(l_mac, mac, 6);
	else	{
		gl_last_mac[5] = (gl_last_mac[5] + 1) &0xFF;
		memcpy(l_mac, gl_last_mac, 6);
	}

	sprintf(ctx->cli_id, "%02x:%02x:%02x:%02x:%02x:%02x",
										  l_mac[0], l_mac[1], l_mac[2],
										  l_mac[3], l_mac[4], l_mac[5]);

	for (i = 0; i < 2; i++) {
		sprintf(ctx->out_ctx[i].buf_name, "%02x-%02x-%02x-%02x-%02x-%02x-idx-%d",
										  l_mac[0], l_mac[1], l_mac[2],
										  l_mac[3], l_mac[4], l_mac[5], i);
		sprintf(buf, "%s/%s", ctx->config.buffer_dir, ctx->out_ctx[i].buf_name);
		remove(buf);
		ctx->out_ctx[i].owner = ctx;
		ctx->out_ctx[i].idx = i;
		strcpy(ctx->out_ctx[i].content_type, "audio/unknown");
	}

	memcpy(&ctx->config, param, sizeof(sq_dev_param_t));

	stream_thread_init(ctx->config.stream_buf_size, ctx);
	output_mr_thread_init(ctx->config.output_buf_size, NULL, ctx->config.rate, 0, ctx);

#if DSD
	dop_thread_init(dop, dop_delay, ctx);
#endif

	decode_thread_init(ctx);
#if 0
	ctx->decode.new_stream = true;
	ctx->decode.state = DECODE_STOPPED;
#endif

#if RESAMPLE
	if (gl_resample) {
		process_init(resample);
	}
#endif

	slimproto_thread_init(gl_server, l_mac, name, "", ctx);

	return true;
}


