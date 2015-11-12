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
#include <ctype.h>

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
#if 0
static unsigned 		gl_rate_delay = 0;
#endif
#if RESAMPLE
static char 			*gl_resample = NULL;
#endif
#if DSD
static bool 			gl_dop	= false;
static unsigned 		gl_dop_delay = 0;
#endif
static char				gl_include_codecs[SQ_STR_LENGTH];
static char				gl_exclude_codecs[SQ_STR_LENGTH];

/*----------------------------------------------------------------------------*/
/* locals */
/*----------------------------------------------------------------------------*/
static void sq_wipe_device(struct thread_ctx_s *ctx);

static log_level	loglevel = lWARN;

/*---------------------------------------------------------------------------*/
void sq_stop() {
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
void sq_wipe_device(struct thread_ctx_s *ctx) {
	int i;

	ctx->callback = NULL;
	ctx->in_use = false;

	slimproto_close(ctx);
	output_mr_close(ctx);
	decode_close(ctx);
	stream_close(ctx);

	for (i = 0; i < 2; i++) {
		if (ctx->out_ctx[i].read_file) fclose (ctx->out_ctx[i].read_file);
		if (ctx->out_ctx[i].write_file) fclose (ctx->out_ctx[i].write_file);
		if (!ctx->config.keep_buffer_file) {
			char buf[SQ_STR_LENGTH];

			sprintf(buf, "%s/%s", ctx->config.buffer_dir, ctx->out_ctx[i].buf_name);
			remove(buf);
		}
		ctx->out_ctx[i].read_file = ctx->out_ctx[i].write_file = NULL;
		ctx->out_ctx[i].owner = NULL;
		mutex_destroy(ctx->out_ctx[i].mutex);
	}
}

/*--------------------------------------------------------------------------*/
void sq_delete_device(sq_dev_handle_t handle) {
	struct thread_ctx_s *ctx = &thread_ctx[handle - 1];
	sq_wipe_device(ctx);
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

/*---------------------------------------------------------------------------*/
static char from_hex(char ch) {
  return isdigit(ch) ? ch - '0' : tolower(ch) - 'a' + 10;
}

/*---------------------------------------------------------------------------*/
static char to_hex(char code) {
  static char hex[] = "0123456789abcdef";
  return hex[code & 15];
}

/*---------------------------------------------------------------------------*/
/* IMPORTANT: be sure to free() the returned string after use */
static char *cli_encode(char *str) {
  char *pstr = str, *buf = malloc(strlen(str) * 3 + 1), *pbuf = buf;
  while (*pstr) {
	if ( isalnum(*pstr) || *pstr == '-' || *pstr == '_' || *pstr == '.' ||
						  *pstr == '~' || *pstr == ' ' || *pstr == ')' ||
						  *pstr == '(' )
	  *pbuf++ = *pstr;
	else if (*pstr == '%') {
	  *pbuf++ = '%',*pbuf++ = '2', *pbuf++ = '5';
	}
	else
	  *pbuf++ = '%', *pbuf++ = to_hex(*pstr >> 4), *pbuf++ = to_hex(*pstr & 15);
	pstr++;
  }
  *pbuf = '\0';
  return buf;
}

/*---------------------------------------------------------------------------*/
/* IMPORTANT: be sure to free() the returned string after use */
static char *cli_decode(char *str) {
  char *pstr = str, *buf = malloc(strlen(str) + 1), *pbuf = buf;
  while (*pstr) {
	if (*pstr == '%') {
	  if (pstr[1] && pstr[2]) {
		*pbuf++ = from_hex(pstr[1]) << 4 | from_hex(pstr[2]);
		pstr += 2;
	  }
	} else {
	  *pbuf++ = *pstr;
	}
	pstr++;
  }
  *pbuf = '\0';
  return buf;
}

/*---------------------------------------------------------------------------*/
/* IMPORTANT: be sure to free() the returned string after use */
static char *cli_find_tag(char *str, char *tag)
{
	char *p, *res = NULL;
	char *buf = malloc(strlen(str));

	strcpy(buf, tag);
	strcat(buf, "%3a");
	if ((p = stristr(str, buf)) != NULL) {
		int i = 0;
		p += strlen(buf);
		while (*(p+i) != ' ' && *(p+i) != '\n' && *(p+i)) i++;
		if (i) {
			strncpy(buf, p, i);
			buf[i] = '\0';
			res = url_decode(buf);
		}
	}
	free(buf);
	return res;
}

/*---------------------------------------------------------------------------*/
char *cli_send_cmd(char *cmd, bool req, bool decode, struct thread_ctx_s *ctx)
{
#define CLI_LEN 2048
	char packet[CLI_LEN];
	int wait;
	size_t len;
	char *rsp = NULL;

	mutex_lock(ctx->cli_mutex);
	wait = ctx->config.max_read_wait;

	cmd = cli_encode(cmd);
	if (req) len = sprintf(packet, "%s ?\n", cmd);
	else len = sprintf(packet, "%s\n", cmd);
	send_packet((u8_t*) packet, len, ctx->cli_sock);

	// first receive the tag and then point to the last '\n'
	len = 0;
	while (wait--)	{
		int k;
		usleep(10000);
		k = recv(ctx->cli_sock, packet + len, CLI_LEN-1 - len, 0);
		if (k < 0) continue;
		len += k;
		packet[len] = '\0';
		if (strchr(packet, '\n') && stristr(packet, cmd)) {
			rsp = packet;
			break;
		}
	}

	if (!wait) {
		LOG_WARN("[%p]: Timeout waiting for CLI reponse (%s)", ctx, cmd);
	}

	if (rsp) {
		for (rsp += strlen(cmd); *rsp == ' '; rsp++);
		if (decode) rsp = cli_decode(rsp);
		else rsp = strdup(rsp);
		*(strrchr(rsp, '\n')) = '\0';
	}

	NFREE(cmd);
	mutex_unlock(ctx->cli_mutex);
	return rsp;
}

/*--------------------------------------------------------------------------*/
u32_t sq_get_time(sq_dev_handle_t handle)
{
	struct thread_ctx_s *ctx = &thread_ctx[handle - 1];
	char cmd[128];
	char *rsp;
	u32_t time = 0;

	if (!handle || !ctx->cli_sock) {
		LOG_ERROR("[%p]: no handle or CLI socket %d", ctx, handle);
		return 0;
	}

	sprintf(cmd, "%s time", ctx->cli_id);
	rsp = cli_send_cmd(cmd, true, true, ctx);
	if (rsp && *rsp) {
		time = (u32_t) (atof(rsp) * 1000);
	}
	else {
		LOG_ERROR("[%p] cannot gettime", ctx);
	}

	NFREE(rsp);
	return time;
}

/*---------------------------------------------------------------------------*/
bool sq_set_time(sq_dev_handle_t handle, u32_t time)
{
	struct thread_ctx_s *ctx = &thread_ctx[handle - 1];
	char cmd[128];
	char *rsp;

	if (!handle || !ctx->cli_sock) {
		LOG_ERROR("[%p]: no handle or cli socket %d", ctx, handle);
		return false;
	}

	sprintf(cmd, "%s time %.1f", ctx->cli_id, (double) time / 1000);
	LOG_INFO("[%p] time cmd %s", ctx, cmd);

	rsp = cli_send_cmd(cmd, false, true, ctx);
	if (!rsp) {
		LOG_ERROR("[%p] cannot settime %d", ctx, time);
		return false;
	}

	NFREE(rsp);
	return true;
}

/*--------------------------------------------------------------------------*/
static void sq_init_metadata(sq_metadata_t *metadata)
{
	metadata->artist 	= NULL;
	metadata->album 	= NULL;
	metadata->title 	= NULL;
	metadata->genre 	= NULL;
	metadata->path 		= NULL;
	metadata->artwork 	= NULL;

	metadata->track 	= 0;
	metadata->index 	= 0;
	metadata->file_size = 0;
	metadata->duration 	= 0;
	metadata->remote 	= false;
}

/*--------------------------------------------------------------------------*/
void sq_default_metadata(sq_metadata_t *metadata, bool init)
{
	if (init) sq_init_metadata(metadata);

	if (!metadata->title) metadata->title 	= strdup("[LMS to uPnP]");
	if (!metadata->album) metadata->album 	= strdup("[no album]");
	if (!metadata->artist) metadata->artist = strdup("[no artist]");
	if (!metadata->genre) metadata->genre 	= strdup("[no genre]");
	/*
	if (!metadata->path) metadata->path = strdup("[no path]");
	if (!metadata->artwork) metadata->artwork = strdup("[no artwork]");
	*/
}


/*--------------------------------------------------------------------------*/
void sq_update_icy(struct out_ctx_s *p)
{
	char cmd[1024];
	char *rsp, *artist, *title, *artwork;
	u16_t idx;
	u32_t now = gettime_ms();
	struct thread_ctx_s *ctx = p->owner;

	if ((now - p->icy.last < 5000) || !p->icy.interval) return;
	p->icy.last = now;

	sprintf(cmd, "%s playlist index", ctx->cli_id);
	rsp = cli_send_cmd(cmd, true, true, ctx);

	if (!rsp || (rsp && !*rsp)) {
		LOG_ERROR("[%p]: missing index", ctx);
		NFREE(rsp);
		return;
	}

	idx = atol(rsp);
	NFREE(rsp);

	sprintf(cmd, "%s playlist path %d", ctx->cli_id, idx);
	rsp = cli_send_cmd(cmd, true, true, ctx);
	if (!rsp || hash32(rsp) != p->track_hash) {
		NFREE(rsp);
		return;
	}
	NFREE(rsp);

	sprintf(cmd, "%s playlist artist %d", ctx->cli_id, idx);
	artist = cli_send_cmd(cmd, true, true, ctx);
	if (artist && (!p->icy.artist || strcmp(p->icy.artist, artist))) {
		NFREE(p->icy.artist);
		p->icy.artist = strdup(artist);
		p->icy.update = true;
	}
	NFREE(artist);

	sprintf(cmd, "%s playlist title %d", ctx->cli_id, idx);
	title = cli_send_cmd(cmd, true, true, ctx);
	if (title && (!p->icy.title || strcmp(p->icy.title, title))) {
		NFREE(p->icy.title);
		p->icy.title = strdup(title);
		p->icy.update = true;
	}
	NFREE(title);

	sprintf(cmd, "%s status %d 1 tags:K", ctx->cli_id, idx);
	rsp = cli_send_cmd(cmd, false, false, ctx);
	if (rsp && *rsp) artwork = cli_find_tag(rsp, "artwork_url");
	else artwork = NULL;
	NFREE(rsp);

	if (artwork && (!p->icy.artwork || strcmp(p->icy.artwork, artwork)))  {
		NFREE(p->icy.artwork);
		p->icy.artwork = strdup(artwork);
		p->icy.update = true;
	}
	NFREE(artwork);
}


/*--------------------------------------------------------------------------*/
bool sq_get_metadata(sq_dev_handle_t handle, sq_metadata_t *metadata, bool next)
{
	struct thread_ctx_s *ctx = &thread_ctx[handle - 1];
	char cmd[1024];
	char *rsp, *p;
	u16_t idx;

	if (!handle || !ctx->cli_sock) {
		LOG_ERROR("[%p]: no handle or CLI socket %d", ctx, handle);
		sq_default_metadata(metadata, true);
		return false;
	}

	sprintf(cmd, "%s playlist index", ctx->cli_id);
	rsp = cli_send_cmd(cmd, true, true, ctx);

	if (!rsp || (rsp && !*rsp)) {
		LOG_ERROR("[%p]: missing index", ctx);
		NFREE(rsp);
		sq_default_metadata(metadata, true);
		return false;
	}

	sq_init_metadata(metadata);

	idx = atol(rsp);
	NFREE(rsp);
	metadata->index = idx;

	if (next) {
		sprintf(cmd, "%s playlist tracks", ctx->cli_id);
		rsp = cli_send_cmd(cmd, true, true, ctx);
		if (rsp && atol(rsp)) idx = (idx + 1) % atol(rsp);
		else idx = 0;
		NFREE(rsp);
	}

	sprintf(cmd, "%s playlist path %d", ctx->cli_id, idx);
	metadata->path = cli_send_cmd(cmd, true, true, ctx);
	metadata->track_hash = hash32(metadata->path);

	sprintf(cmd, "%s playlist remote %d", ctx->cli_id, idx);
	rsp  = cli_send_cmd(cmd, true, true, ctx);
	if (rsp && *rsp == '1') metadata->remote = true;
	else metadata->remote = false;
	NFREE(rsp)

	sprintf(cmd, "songinfo 0 10 url:%s tags:cfldatgrK", metadata->path);
	rsp = cli_send_cmd(cmd, false, false, ctx);

	if (rsp && *rsp) {
		metadata->title = cli_find_tag(rsp, "title");
		metadata->artist = cli_find_tag(rsp, "artist");
		metadata->album = cli_find_tag(rsp, "album");
		metadata->genre = cli_find_tag(rsp, "genre");

		if ((p = cli_find_tag(rsp, "duration")) != NULL) {
			metadata->duration = 1000 * atof(p);
			free(p);
		}

		if ((p = cli_find_tag(rsp, "filesize")) != NULL) {
			metadata->file_size = atol(p);
			/*
			at this point, LMS sends the original filesize, not the transcoded
			so it simply does not work
			*/
			metadata->file_size = 0;
			free(p);
		}

		if ((p = cli_find_tag(rsp, "tracknum")) != NULL) {
			metadata->track = atol(p);
			free(p);
		}

		metadata->artwork = cli_find_tag(rsp, "artwork_url");
		if (!metadata->artwork || !strlen(metadata->artwork)) {
			NFREE(metadata->artwork);
			if ((p = cli_find_tag(rsp, "coverid")) != NULL) {
				metadata->artwork = malloc(SQ_STR_LENGTH);
				snprintf(metadata->artwork, SQ_STR_LENGTH, "http://%s:%s/music/%s/cover.jpg", ctx->server_ip, ctx->server_port, p);
				free(p);
			}
		}
	}
	else {
		LOG_INFO("[%p]: no metadata using songinfo", ctx, idx);
		NFREE(rsp);

		sprintf(cmd, "%s playlist title %d", ctx->cli_id, idx);
		metadata->title = cli_send_cmd(cmd, true, true, ctx);

		sprintf(cmd, "%s playlist album %d", ctx->cli_id, idx);
		metadata->album = cli_send_cmd(cmd, true, true, ctx);

		sprintf(cmd, "%s playlist artist %d", ctx->cli_id, idx);
		metadata->artist = cli_send_cmd(cmd, true, true, ctx);

		sprintf(cmd, "%s playlist genre %d", ctx->cli_id, idx);
		metadata->genre = cli_send_cmd(cmd, true, true, ctx);

		sprintf(cmd, "%s status %d 1 tags:K", ctx->cli_id, idx);
		rsp = cli_send_cmd(cmd, false, false, ctx);
		if (rsp && *rsp) metadata->artwork = cli_find_tag(rsp, "artwork_url");
		NFREE(rsp);

		sprintf(cmd, "%s playlist duration %d", ctx->cli_id, idx);
		rsp = cli_send_cmd(cmd, true, true, ctx);
		if (rsp) metadata->duration = 1000 * atof(rsp);
	}
	NFREE(rsp);

	if (!next) {
		sprintf(cmd, "%s time", ctx->cli_id);
		rsp = cli_send_cmd(cmd, true, true, ctx);
		if (rsp && *rsp) metadata->duration -= (u32_t) (atof(rsp) * 1000);
		NFREE(rsp);
	}

	sq_default_metadata(metadata, false);

	LOG_INFO("[%p]: idx %d\n\tartist:%s\n\talbum:%s\n\ttitle:%s\n\tgenre:%s\n\tduration:%d.%03d\n\tsize:%d\n\tcover:%s", ctx, idx,
				metadata->artist, metadata->album, metadata->title,
				metadata->genre, div(metadata->duration, 1000).quot,
				div(metadata->duration,1000).rem, metadata->file_size,
				metadata->artwork);

	return true;
}

/*--------------------------------------------------------------------------*/
void sq_free_metadata(sq_metadata_t *metadata)
{
	NFREE(metadata->artist);
	NFREE(metadata->album);
	NFREE(metadata->title);
	NFREE(metadata->genre);
	NFREE(metadata->path);
	NFREE(metadata->artwork);
}


/*---------------------------------------------------------------------------*/
sq_dev_handle_t sq_urn2handle(const char *urn)
{
	int i;

	for (i = 0; i < MAX_PLAYER; i++) {
		if (!thread_ctx[i].in_use) continue;
		if (strstr(urn, thread_ctx[i].out_ctx[0].buf_name)) return i+1;
		if (strstr(urn, thread_ctx[i].out_ctx[1].buf_name)) return i+1;;
	}

	return 0;
}


/*---------------------------------------------------------------------------*/
void *sq_get_info(const char *urn, s32_t *size, char **content_type, u16_t interval)
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
		i--;
		p = malloc(strlen(out->content_type) + 1);
		strcpy(p, out->content_type);
		*size = out->file_size;
		*content_type = p;

		if (out->remote) out->icy.interval = interval;
		else out->icy.interval = 0;

		return &thread_ctx[i];
	}
	else {
		*content_type = strdup("audio/unknown");
		return NULL;
	}
}


/*---------------------------------------------------------------------------*/
bool sq_is_remote(const char *urn)
{
	int i = 0;

	for (i = 0; i < MAX_PLAYER; i++) {
		if (!thread_ctx[i].in_use) continue;
		if (strstr(urn, thread_ctx[i].out_ctx[0].buf_name))
			return thread_ctx[i].config.send_icy && thread_ctx[i].out_ctx[0].remote && !thread_ctx[i].out_ctx[0].duration;
		if (strstr(urn, thread_ctx[i].out_ctx[1].buf_name))
			return thread_ctx[i].config.send_icy && thread_ctx[i].out_ctx[1].remote && !thread_ctx[i].out_ctx[1].duration;
	}

	return true;
}


/*---------------------------------------------------------------------------*/
void sq_reset_icy(struct out_ctx_s *p, bool init)
{
	if (init) {
		p->icy.last = gettime_ms();
		p->icy.remain = p->icy.interval;
		p->icy.update = false;
	}

	NFREE(p->icy.title);
	NFREE(p->icy.artist);
	NFREE(p->icy.artwork);
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

		/*
		Some players send 2 GET for the same URL - this is not supported. But
		others send a GET after the SETURI and then, when they receive the PLAY,
		they close the socket and send another GET, but the close might be
		received after the GET (threading) and might be confused with a 2nd GET
		so a mutex with timeout should solve this
		*/
		if (mutex_timedlock(out->mutex, 1000) && out->read_file) {
			LOG_WARN("[%p]: cannot open file twice %s", out->owner, urn);
			return NULL;
		}

		sq_reset_icy(out, true);

		LOCK_S;LOCK_O;
		if (!out->read_file) {
			// read counters are not set here. they are set
			sprintf(buf, "%s/%s", thread_ctx[i-1].config.buffer_dir, out->buf_name);
			out->read_file = fopen(buf, "rb");
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
void *sq_isopen(const char *urn)
{
	int i = 0;
	out_ctx_t *out = NULL;


	for (i = 0; i < MAX_PLAYER && !out; i++) {
		if (!thread_ctx[i].in_use) continue;
		if (strstr(urn, thread_ctx[i].out_ctx[0].buf_name)) out = &thread_ctx[i].out_ctx[0];
		if (strstr(urn, thread_ctx[i].out_ctx[1].buf_name)) out = &thread_ctx[i].out_ctx[1];
	}

	if (out) return out->read_file;
	else return NULL;
}


/*---------------------------------------------------------------------------*/
void sq_set_sizes(void *desc)
{
	out_ctx_t *p = (out_ctx_t*) desc;
	u8_t sample_size;
	div_t duration;

	p->raw_size = p->file_size;

	// if the track is remote and infinite, this is a live stream
	if (p->remote && !p->duration) return;

	// if not a raw format, then duration and raw size cannot be altered
	if (strcmp(p->ext, "wav") && strcmp(p->ext, "aif") && strcmp(p->ext, "pcm")) return;

	sample_size = (p->sample_size == 24 && p->owner->config.L24_format == L24_TRUNC_16) ? 16 : p->sample_size;

	// duration is missing from metadata but using a HTTP no size format, need to take a guess
	if (!p->duration) {
		p->duration =  (p->file_size < 0) ?
						(1 << 31) / ((u32_t) p->sample_rate * (u32_t) (sample_size/8) * (u32_t) p->channels) :
						(p->file_size) / ((u32_t) p->sample_rate * (u32_t) (sample_size/8) * (u32_t) p->channels);
		p->duration *= 1000;
	}

	duration = div(p->duration, 1000);
	p->raw_size = duration.quot * (u32_t) p->sample_rate * (u32_t) (sample_size/8) * (u32_t) p->channels;
	p->raw_size += (duration.rem * (u32_t) p->sample_rate * (u32_t) (sample_size/8) * (u32_t) p->channels) / 1000;

	// HTTP streaming using no size, nothing else to change
	if (p->file_size < 0) return;

	if (!strcmp(p->ext, "wav")) p->file_size = p->raw_size + 36 + 8;
	if (!strcmp(p->ext, "aif")) p->file_size = p->raw_size + (8+8) + (18+8) + 4 + 8;
	if (!strcmp(p->ext, "pcm")) p->file_size = p->raw_size;
}


/*---------------------------------------------------------------------------*/
bool sq_close(void *desc)
{
	out_ctx_t *p = (out_ctx_t*) desc;

	// reject any pending request after the device has been stopped
	if (!p->owner || &p->owner->out_ctx[p->idx] != p) {
		LOG_ERROR("[%p]: unknow output context %p", p->owner, p);
		return false;
	}
	else {
		struct thread_ctx_s *ctx = p->owner; 		// for the macro to work ... ugh
		LOCK_S;LOCK_O;
		if (p->read_file) fclose(p->read_file);
		p->read_file = NULL;
		LOG_INFO("[%p]: read total:%Ld", p->owner, p->read_count_t);
		p->close_count = p->read_count;
		p->read_count_t -= p->read_count;
		p->read_count = 0;
		mutex_unlock(p->mutex);
		sq_reset_icy(p, false);
		UNLOCK_S;UNLOCK_O;
	}

	return true;
}

/*---------------------------------------------------------------------------*/
int sq_seek(void *desc, off_t bytes, int from)
{
	out_ctx_t *p = (out_ctx_t*) desc;
	int rc = -1;

	if (!p->owner || &p->owner->out_ctx[p->idx] != p) {
		LOG_ERROR("[%p]: unknow output context %p", p->owner, p);
	}
	else {
		struct thread_ctx_s *ctx = p->owner; 		// for the macro to work ... ugh
		LOCK_S;LOCK_O;

		/*
		Although a SEEK_CUR is sent, because this is a response to a GET with a
		range xx-yy, this must be treated as a SEEK_SET. An HTTP range -yy is
		an illegal request, so SEEK_CUR does not make sense (see httpreadwrite.c)
		*/

		LOG_INFO("[%p]: seek %d (c:%d)", ctx, bytes - (p->write_count_t - p->write_count));
		bytes -= p->write_count_t - p->write_count;
		if (bytes < 0) {
			LOG_WARN("[%p]: seek unreachable b:%d t:%d r:%d", p->owner, bytes, p->write_count_t, p->write_count);
			bytes = 0;
		}
		rc = fseek(p->read_file, bytes, SEEK_SET);
		p->read_count += bytes;
		p->read_count_t += bytes;

		UNLOCK_S;UNLOCK_O;
	}
	return rc;
}

/*---------------------------------------------------------------------------*/
int sq_read(void *desc, void *dst, unsigned bytes)
{
	unsigned wait, read_b = 0, req_bytes = bytes;
	out_ctx_t *p = (out_ctx_t*) desc;
	struct thread_ctx_s *ctx = p->owner;

	if (!p->owner || &p->owner->out_ctx[p->idx] != p) {
		LOG_ERROR("[%p]: unknow output context %p", p->owner, p);
		return -1;
	}

	sq_update_icy(p);

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

	if (p->icy.interval) bytes = min(bytes, p->icy.remain);

	wait = ctx->config.max_read_wait;
	if (ctx->config.mode == SQ_STREAM) {
		do
		{
			LOCK_S;LOCK_O;
			if (p->read_file) {
				read_b += fread(dst, 1, bytes, p->read_file);
#if OSX
				// to reset EOF pointer
				fseek(p->read_file, 0, SEEK_CUR);
#endif
			}
			UNLOCK_S;LOCK_O;
			LOG_SDEBUG("[%p] read %u bytes at %d", ctx, read_b, wait);
			if (!read_b) usleep(50000);
		} while (!read_b && p->write_file && wait-- && p->owner);
	}

	/*
	there is tiny chance for a race condition where the device is deleted
	while sleeping, so check that otherwise LOCK will create a fault
	*/
	if (!p->owner) {
		LOG_ERROR("[%p]: device stopped during wait %p", p, ctx);
		return 0;
	}

	LOG_INFO("[%p]: read %d (a:%d r:%d w:%d)", ctx, bytes, req_bytes, read_b, wait);

	LOCK_S;LOCK_O;

	p->read_count += read_b;
	p->read_count_t += read_b;

	if (p->icy.interval) {
		p->icy.remain -= read_b;
		if (!p->icy.remain) {
			u16_t len_16 = 0;
			char *buf = (char *) dst;

			if (p->icy.update) {
				len_16 = sprintf(buf + read_b + 1, "StreamTitle='%s - %s';StreamUrl='%s';",
							(p->icy.artist) ? p->icy.artist : "",
							(p->icy.title) ? p->icy.title : "",
							(p->icy.artwork) ? p->icy.artwork : "");

				LOG_INFO("[%p]: ICY update\n\t%s\n\t%s\n\t%s", ctx, p->icy.artist, p->icy.title, p->icy.artwork);
				len_16 = (len_16 + 15) / 16;
			}

			buf[read_b] = len_16;
			read_b += (len_16 * 16) + 1;
			p->icy.remain = p->icy.interval;
			p->icy.update = false;
		}
	}

	/*
	Stream disconnected and not full data request served ==> end of stream
	but ... only inform the controller when a last read with 0 has been
	made, otherwise the upnp device will make another read attempt, read in
	the nextURI buffer and miss the end of the current track
	Starting 2.5.x with real size, take also into account when player closes
	the connection
	*/
	if ((!read_b || ((p->file_size > 0 ) && (p->read_count_t >= p->file_size))) && wait && !p->write_file) {
#ifndef __EARLY_STMd__
		ctx->read_ended = true;
		wake_controller(ctx);
#endif
		LOG_INFO("[%p]: read (end of track) w:%d", ctx, wait);
	}

	// exit on timeout and not read enough data ==> underrun
	if (!wait && !read_b) {
		ctx->read_to = true;
		LOG_ERROR("[%p]: underrun read:%d (r:%d)", ctx, read_b, bytes);
	}
	UNLOCK_S;UNLOCK_O;

	return read_b;
}


/*---------------------------------------------------------------------------*/
void sq_notify(sq_dev_handle_t handle, void *caller_id, sq_event_t event, u8_t *cookie, void *param)
{
	struct thread_ctx_s *ctx = &thread_ctx[handle - 1];

	LOG_SDEBUG("[%p] notif %d", ctx, event);

	// squeezelite device has not started yet or is off ...
	if (!ctx->running || !ctx->on || !handle) return;

	switch (event) {
		case SQ_SETURI: break;
		case SQ_UNPAUSE:
		case SQ_PLAY:
			if (* (bool*) param) {
				// unsollicited PLAY done on the player direclty
				char cmd[128], *rsp;

				LOG_WARN("[%p] uPNP unsollicited play", ctx);
				sprintf(cmd, "%s play", ctx->cli_id);
				rsp = cli_send_cmd(cmd, false, true, ctx);
				NFREE(rsp);
			}
			else {
				/*
				Be careful of what is done here in case the "playing" event if
				an extra one generated by an unwanted stop or a lack of NextURI cap
				*/
				LOCK_S;
				LOG_INFO("[%p] uPNP playing notif", ctx);
				ctx->play_running = true;
				UNLOCK_S;
				wake_controller(ctx);
			}
			break;
		case SQ_PAUSE: {
			char cmd[128], *rsp;

			LOG_WARN("[%p] uPNP unsollicited pause", ctx);
			sprintf(cmd, "%s pause", ctx->cli_id);
			rsp = cli_send_cmd(cmd, false, true, ctx);
			NFREE(rsp);
			break;
		}
		case SQ_STOP:
			LOG_INFO("[%p] uPNP notify STOP", ctx);
			LOCK_S;
			if (ctx->play_running) {
				ctx->track_ended = true;
			 }
			ctx->play_running = false;
			UNLOCK_S;
			wake_controller(ctx);
			break;
		case SQ_SEEK: break;
		case SQ_VOLUME: {
			char cmd[128], *rsp;

			sprintf(cmd, "%s mixer volume %d", ctx->cli_id, *((u16_t*) param));
			rsp = cli_send_cmd(cmd, false, true, ctx);
			NFREE(rsp);
			break;
		}
		case SQ_TIME: {
			int time = *((unsigned*) param);

			LOG_DEBUG("[%p] time %d %d", ctx, ctx->ms_played, time);
			ctx->ms_played = time;
			break;
		}
		case SQ_TRACK_CHANGE:
			LOCK_S;
			if (ctx->play_running) {
				LOG_INFO("[%p] End of track by track change", ctx);
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
void sq_release_device(sq_dev_handle_t handle)
{
	if (handle) thread_ctx[handle - 1].in_use = false;
}

/*---------------------------------------------------------------------------*/
sq_dev_handle_t sq_reserve_device(void *MR, sq_callback_t callback)
{
	int ctx_i;
	struct thread_ctx_s *ctx;

	/* find a free thread context - this must be called in a LOCKED context */
	for  (ctx_i = 0; ctx_i < MAX_PLAYER; ctx_i++)
		if (!thread_ctx[ctx_i].in_use) break;

	if (ctx_i < MAX_PLAYER)
	{
		// this sets a LOT of data to proper defaults (NULL, false ...)
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
bool sq_run_device(sq_dev_handle_t handle, char *name, sq_dev_param_t *param)
{
	int i;
	struct thread_ctx_s *ctx = &thread_ctx[handle - 1];

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

	memcpy(&ctx->config, param, sizeof(sq_dev_param_t));

	if (strstr(ctx->config.buffer_dir, "?")) {
		GetTempPath(SQ_STR_LENGTH, ctx->config.buffer_dir);
	}

	if (access(ctx->config.buffer_dir, 2)) {
		LOG_ERROR("[%p]: cannot access %s", ctx, ctx->config.buffer_dir);
		return false;
	}

	if (!memcmp(ctx->config.mac, "\0\0\0\0\0\0", 6)) {
		gl_last_mac[5] = (gl_last_mac[5] + 1) &0xFF;
		memcpy(ctx->config.mac, gl_last_mac, 6);
	}

	if ((u32_t) ctx->config.buffer_limit < max(ctx->config.stream_buf_size, ctx->config.output_buf_size) * 4) {
		LOG_ERROR("[%p]: incorrect buffer limit %d", ctx, ctx->config.buffer_limit);
		ctx->config.buffer_limit = max(ctx->config.stream_buf_size, ctx->config.output_buf_size) * 4;
	}

	sprintf(ctx->cli_id, "%02x:%02x:%02x:%02x:%02x:%02x",
										  ctx->config.mac[0], ctx->config.mac[1], ctx->config.mac[2],
										  ctx->config.mac[3], ctx->config.mac[4], ctx->config.mac[5]);

	for (i = 0; i < 2; i++) {
		sprintf(ctx->out_ctx[i].buf_name, "%02x-%02x-%02x-%02x-%02x-%02x-idx-%d",
										  ctx->config.mac[0], ctx->config.mac[1], ctx->config.mac[2],
										  ctx->config.mac[3], ctx->config.mac[4], ctx->config.mac[5], i);
		if (!ctx->config.keep_buffer_file) {
			char buf[SQ_STR_LENGTH];

			sprintf(buf, "%s/%s", ctx->config.buffer_dir, ctx->out_ctx[i].buf_name);
			remove(buf);
		}

		mutex_create(ctx->out_ctx[i].mutex);
		ctx->out_ctx[i].owner = ctx;
		ctx->out_ctx[i].idx = i;
		strcpy(ctx->out_ctx[i].content_type, "audio/unknown");
	}

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

	slimproto_thread_init(gl_server, ctx->config.mac, name, "", ctx);

	return true;
}


