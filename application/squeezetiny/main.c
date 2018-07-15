/*
 *  Squeezelite - lightweight headless squeezebox emulator
 *
 *  (c) Adrian Smith 2012-2014, triode1@btinternet.com
 *	(c) Philippe 2015-2017, philippe_44@outlook.com
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

#define IMAGEPROXY "/imageproxy/"

#define LOCK_S   mutex_lock(ctx->streambuf->mutex)
#define UNLOCK_S mutex_unlock(ctx->streambuf->mutex)
#define LOCK_O	 mutex_lock(ctx->outputbuf->mutex)
#define UNLOCK_O mutex_unlock(ctx->outputbuf->mutex)
#define LOCK_P   mutex_lock(ctx->mutex)
#define UNLOCK_P mutex_unlock(ctx->mutex)

struct thread_ctx_s thread_ctx[MAX_PLAYER];
char				sq_ip[16];
u16_t				sq_port;

/*----------------------------------------------------------------------------*/
/* locals */
/*----------------------------------------------------------------------------*/
static void sq_wipe_device(struct thread_ctx_s *ctx);

extern log_level	 slimmain_loglevel;
static log_level	*loglevel = &slimmain_loglevel;

/*--------------------------------------------------------------------------*/
void sq_wipe_device(struct thread_ctx_s *ctx) {
	int i;

	mutex_lock(ctx->cli_mutex);
	ctx->callback = NULL;
	ctx->in_use = false;
	mutex_unlock(ctx->cli_mutex);

	slimproto_close(ctx);
	output_flush(ctx);
	output_close(ctx);
#if RESAMPLE
	process_end(ctx);
#endif
	decode_close(ctx);
	stream_close(ctx);

	for (i = 0; ctx->mimetypes[i]; i++) free(ctx->mimetypes[i]);
}

/*--------------------------------------------------------------------------*/
void sq_delete_device(sq_dev_handle_t handle) {
	struct thread_ctx_s *ctx = &thread_ctx[handle - 1];

	if (!handle) return;

	ctx = &thread_ctx[handle - 1];
	sq_wipe_device(ctx);
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
	char *buf = malloc(max(strlen(str), strlen(tag)) + 4);

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
bool cli_open_socket(struct thread_ctx_s *ctx) {
	struct sockaddr_in addr;

	if (!ctx->config.dynamic.use_cli) return false;
	if (ctx->cli_sock > 0) return true;

	ctx->cli_sock = socket(AF_INET, SOCK_STREAM, 0);
	set_nonblock(ctx->cli_sock);
	set_nosigpipe(ctx->cli_sock);

	addr.sin_family = AF_INET;
	addr.sin_addr.s_addr = ctx->slimproto_ip;
	addr.sin_port = htons(9090);

	if (connect_timeout(ctx->cli_sock, (struct sockaddr *) &addr, sizeof(addr), 50))  {
		LOG_ERROR("[%p] unable to connect to server with cli", ctx);
		ctx->cli_sock = -1;
		return false;
	}

	LOG_INFO("[%p]: opened CLI socket %d", ctx, ctx->cli_sock);
	return true;
}

#define CLI_SEND_SLEEP (10000)
#define CLI_SEND_TO (1*500000)
/*---------------------------------------------------------------------------*/
char *cli_send_cmd(char *cmd, bool req, bool decode, struct thread_ctx_s *ctx)
{
#define CLI_LEN 4096
	char *packet;
	int wait;
	size_t len;
	char *rsp = NULL;

	mutex_lock(ctx->cli_mutex);
	if (!cli_open_socket(ctx)) {
		mutex_unlock(ctx->cli_mutex);
		return NULL;
	}
	ctx->cli_timestamp = gettime_ms();

	packet = malloc(CLI_LEN);
	wait = CLI_SEND_TO / CLI_SEND_SLEEP;
	cmd = cli_encode(cmd);
	if (req) len = sprintf(packet, "%s ?\n", cmd);
	else len = sprintf(packet, "%s\n", cmd);

	LOG_SDEBUG("[%p]: cmd %s", ctx, packet);
	send_packet((u8_t*) packet, len, ctx->cli_sock);
	// first receive the tag and then point to the last '\n'
	len = 0;
	while (wait)	{
		int k;
		fd_set rfds;
		struct timeval timeout = {0, CLI_SEND_SLEEP};

		FD_ZERO(&rfds);
		FD_SET(ctx->cli_sock, &rfds);

		k = select(ctx->cli_sock + 1, &rfds, NULL, NULL, &timeout);

		if (!k) {
			wait--;
			continue;
		}

		if (k < 0) break;

		k = recv(ctx->cli_sock, packet + len, CLI_LEN-1 - len, 0);
		if (k <= 0) break;

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

	LOG_SDEBUG("[%p]: rsp %s", ctx, rsp);

	if (rsp && ((rsp = stristr(rsp, cmd)) != NULL)) {
		rsp += strlen(cmd);
		while (*rsp && *rsp == ' ') rsp++;

		if (decode) rsp = cli_decode(rsp);
		else rsp = strdup(rsp);
		*(strrchr(rsp, '\n')) = '\0';
	}

	mutex_unlock(ctx->cli_mutex);

	free(cmd);
	free(packet);

	return rsp;
}

/*--------------------------------------------------------------------------*/
u32_t sq_get_time(sq_dev_handle_t handle)
{
	struct thread_ctx_s *ctx = &thread_ctx[handle - 1];
	char cmd[128];
	char *rsp;
	u32_t time = 0;

	if (!ctx->config.dynamic.use_cli) return 0;

	if (!handle || !ctx->in_use) {
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
bool sq_set_time(sq_dev_handle_t handle, char *pos)
{
	struct thread_ctx_s *ctx = &thread_ctx[handle - 1];
	char cmd[128];
	char *rsp;

	if (!ctx->config.dynamic.use_cli) return false;

	if (!handle || !ctx->in_use) {
		LOG_ERROR("[%p]: no handle or cli socket %d", ctx, handle);
		return false;
	}

	sprintf(cmd, "%s time %s", ctx->cli_id, pos);
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
static void sq_init_metadata(metadata_t *metadata)
{
	metadata->artist 	= NULL;
	metadata->album 	= NULL;
	metadata->title 	= NULL;
	metadata->genre 	= NULL;
	metadata->artwork 	= NULL;
	metadata->remote_title = NULL;

	metadata->track 	= 0;
	metadata->index 	= 0;
	metadata->file_size = 0;
	metadata->duration 	= 0;
	metadata->remote 	= false;
	metadata->bitrate	= 0;
}

/*--------------------------------------------------------------------------*/
void sq_default_metadata(metadata_t *metadata, bool init)
{
	if (init) sq_init_metadata(metadata);

	if (!metadata->title) metadata->title 	= strdup("[LMS]");
	if (!metadata->album) metadata->album 	= strdup("[no album]");
	if (!metadata->artist) metadata->artist = strdup("[no artist]");
	if (!metadata->genre) metadata->genre 	= strdup("[no genre]");
	if (!metadata->remote_title) metadata->remote_title	= strdup("[no remote]");
}


/*--------------------------------------------------------------------------*/
bool sq_get_metadata(sq_dev_handle_t handle, metadata_t *metadata, bool next)
{
	struct thread_ctx_s *ctx = &thread_ctx[handle - 1];
	char cmd[1024];
	char *rsp, *p, *cur;

	if (!handle || !ctx->in_use || !ctx->config.dynamic.use_cli) {
		if (ctx->config.dynamic.use_cli) {
			LOG_ERROR("[%p]: no handle or CLI socket %d", ctx, handle);
		}
		sq_default_metadata(metadata, true);
		return false;
	}

	sq_init_metadata(metadata);

	sprintf(cmd, "%s status - 2 tags:xcfldatgrKN", ctx->cli_id);
	rsp = cli_send_cmd(cmd, false, false, ctx);

	if (!rsp || !*rsp) {
		sq_default_metadata(metadata, false);
		LOG_WARN("[%p]: cannot get metadata", ctx);
		return true;
	}

	// find the current index
	if ((p = cli_find_tag(rsp, "playlist_cur_index")) != NULL) {
		metadata->index = atoi(p);
		if (next) metadata->index++;
		free(p);
	}

	// need to make sure we rollover if end of list
	if ((p = cli_find_tag(rsp, "playlist_tracks")) != NULL) {
		metadata->index %= atoi(p);
	}

	sprintf(cmd, "playlist%%20index%%3a%d ", metadata->index);

	if ((cur = stristr(rsp, cmd)) != NULL) {
		metadata->title = cli_find_tag(cur, "title");
		metadata->artist = cli_find_tag(cur, "artist");
		metadata->album = cli_find_tag(cur, "album");
		metadata->genre = cli_find_tag(cur, "genre");
		metadata->remote_title = cli_find_tag(cur, "remote_title");

		if ((p = cli_find_tag(cur, "duration")) != NULL) {
			metadata->duration = 1000 * atof(p);
			free(p);
		}

		/*
		at this point, LMS sends the original filesize, not the transcoded
		so it simply does not work
		if ((p = cli_find_tag(rsp, "filesize")) != NULL) {
			metadata->file_size = atol(p);
			free(p);
		}
		*/

		if ((p = cli_find_tag(cur, "bitrate")) != NULL) {
			metadata->bitrate = atol(p);
			free(p);
		}

		if ((p = cli_find_tag(cur, "tracknum")) != NULL) {
			metadata->track = atol(p);
			free(p);
		}

		if ((p = cli_find_tag(cur, "remote")) != NULL) {
			metadata->remote = (atoi(p) == 1);
			free(p);
		}

		metadata->artwork = cli_find_tag(cur, "artwork_url");
		if (!metadata->artwork || !strlen(metadata->artwork)) {
			NFREE(metadata->artwork);
			if ((p = cli_find_tag(cur, "coverid")) != NULL) {
				metadata->artwork = malloc(_STR_LEN_);
				snprintf(metadata->artwork, _STR_LEN_, "http://%s:%s/music/%s/cover.jpg", ctx->server_ip, ctx->server_port, p);
				free(p);
			}
		}

		if (metadata->artwork && !strncmp(metadata->artwork, IMAGEPROXY, strlen(IMAGEPROXY))) {
			char *artwork = malloc(_STR_LEN_);

			snprintf(artwork, _STR_LEN_, "http://%s:%s%s", ctx->server_ip, ctx->server_port, metadata->artwork);
			// why the f... does LMS use .png extension in image proxy, where IT IS jpeg?
			if ((p = strstr(artwork, ".png")) != NULL) strcpy(p, ".jpg");
			free(metadata->artwork);
			metadata->artwork = artwork;
		}
	} else {
		LOG_ERROR("[%p]: track not found %u %s", ctx, metadata->index, rsp);
	}

	if (!next && metadata->duration && ((p = cli_find_tag(rsp, "time")) != NULL)) {
		metadata->duration -= (u32_t) (atof(p) * 1000);
		free(p);
	}

	NFREE(rsp);

	sq_default_metadata(metadata, false);

	LOG_DEBUG("[%p]: idx %d\n\tartist:%s\n\talbum:%s\n\ttitle:%s\n\tgenre:%s\n\tduration:%d.%03d\n\tsize:%d\n\tcover:%s", ctx, metadata->index,
				metadata->artist, metadata->album, metadata->title,
				metadata->genre, div(metadata->duration, 1000).quot,
				div(metadata->duration,1000).rem, metadata->file_size,
				metadata->artwork ? metadata->artwork : "");

	return true;
}


/*--------------------------------------------------------------------------*/
void sq_free_metadata(metadata_t *metadata)
{
	NFREE(metadata->artist);
	NFREE(metadata->album);
	NFREE(metadata->title);
	NFREE(metadata->genre);
	NFREE(metadata->artwork);
	NFREE(metadata->remote_title);
}


/*--------------------------------------------------------------------------*/
u32_t sq_self_time(sq_dev_handle_t handle)
{
	struct thread_ctx_s *ctx = &thread_ctx[handle - 1];
	u32_t time;
	u32_t now = gettime_ms();

	LOCK_O;

	if (ctx->render.index != -1 && ctx->render.state != RD_STOPPED) {
		time = now - ctx->render.track_start_time - ctx->render.ms_paused;
		if (ctx->render.state == RD_PAUSED) time -= now - ctx->render.track_pause_time;
	} else time = 0;

	UNLOCK_O;

	return time;
}


/*---------------------------------------------------------------------------*/
void sq_notify(sq_dev_handle_t handle, void *caller_id, sq_event_t event, u8_t *cookie, void *param)
{
	struct thread_ctx_s *ctx = &thread_ctx[handle - 1];
	char cmd[128], *rsp;

	LOG_SDEBUG("[%p] notif %d", ctx, event);

	// squeezelite device has not started yet or is off ...
	if (!ctx->running || !ctx->on || !handle || !ctx->in_use) return;

	switch (event) {
		case SQ_TRANSITION:
			/*
			 not all players send or have time to send a transition between two
			 tracks. If they don't we'll simply detect track start while detecting
			 index change. Otherwise we'll detect it with the play event
			*/
			LOCK_O;
			ctx->render.state = RD_TRANSITION;
			UNLOCK_O;
			break;
		case SQ_PLAY: {
			// don't need to lock to read as we are the only one to change the state
			if (ctx->render.state == RD_PAUSED) {
				LOCK_O;
				ctx->render.ms_paused += gettime_ms() - ctx->render.track_pause_time;
				ctx->render.state = RD_PLAYING;
				UNLOCK_O;
				LOG_INFO("[%p] resume notification (paused time %u)", ctx, ctx->render.ms_paused);
			} else if (ctx->render.state != RD_PLAYING) {
				LOCK_O;
				ctx->render.state = RD_PLAYING;
				// PLAY event can happen before render index has been captured
				if (ctx->render.index == ctx->output.index) {
					ctx->output.track_started = true;
					ctx->render.track_start_time = gettime_ms();
					LOG_INFO("[%p] track %u started at %u", ctx, ctx->render.index, ctx->render.track_start_time);
            } else {
					LOG_INFO("[%p] play notification", ctx );
				}
				UNLOCK_O;
				wake_controller(ctx);
			} else {
				LOG_INFO("[%p] play ignored", ctx );
			}

			if (*(bool*) param) {
				// unsollicited PLAY done on the player direclty
				LOG_WARN("[%p] unsollicited play", ctx);
				sprintf(cmd, "%s play", ctx->cli_id);
				rsp = cli_send_cmd(cmd, false, true, ctx);
				NFREE(rsp);
			}
			break;
		}
		case SQ_PAUSE: {
			// put a marker when we paused
			LOCK_O;
			ctx->render.state = RD_PAUSED;
			ctx->render.track_pause_time = gettime_ms();
			LOG_INFO("[%p] track paused at %u", ctx, ctx->render.track_pause_time);
			UNLOCK_O;

			if (*(bool*) param) {
				LOG_WARN("[%p] unsollicited pause", ctx);
				sprintf(cmd, "%s pause", ctx->cli_id);
				rsp = cli_send_cmd(cmd, false, true, ctx);
				NFREE(rsp);
			}
			break;
		}
		case SQ_STOP:
			if (*(bool*) param) {
				// stop if the renderer side is sure or if we had 2 stops in a row
				LOG_INFO("[%p] forced STOP", ctx);
				sprintf(cmd, "%s stop", ctx->cli_id);
				rsp = cli_send_cmd(cmd, false, true, ctx);
				NFREE(rsp);
			/* FIXME: not sure anymore what this tries to cover
			} else if (ctx->stream.state <= DISCONNECT && !ctx->output.completed) {
				// happens if streaming fails (spotty)
				LOG_INFO("[%p] un-managed STOP, re-starting", ctx);
				sprintf(cmd, "%s time -5.00", ctx->cli_id);
				rsp = cli_send_cmd(cmd, false, true, ctx);
				NFREE(rsp);
			*/
			} else {
				// might be a STMu or a STMo, let slimproto decide
				LOCK_O;
				ctx->render.state = RD_STOPPED;
				UNLOCK_O;
				LOG_INFO("[%p] notify STOP", ctx);
				wake_controller(ctx);
			}
			break;
		case SQ_SEEK:
			break;
		case SQ_VOLUME: {
			sprintf(cmd, "%s mixer volume %d", ctx->cli_id, *((u16_t*) param));
			rsp = cli_send_cmd(cmd, false, true, ctx);
			NFREE(rsp);
			break;
		}
		case SQ_TIME: {
			u32_t time = *((u32_t*) param);
			LOG_DEBUG("[%p] time %d %d", ctx, ctx->render.ms_played, time);
			LOCK_O;
			if (ctx->render.index != -1) ctx->render.ms_played = time;
			UNLOCK_O;
			break;
		}
		case SQ_TRACK_INFO: {
			char *uri= (char*) param;
			u32_t index;

			uri = strstr(uri, BRIDGE_URL);
			if (!uri) break;
			/*
			if we detect a change of track then update render context. Still,
			we have to wait	for PLAY status before claiming track has started.
			make sure as well that renderer track number is not from an old
			context
			*/
			sscanf(uri, BRIDGE_URL "%u", &index);
			if (ctx->render.index != index && ctx->output.index == index) {
				LOCK_O;
				ctx->render.index = index;
				ctx->render.ms_paused = ctx->render.ms_played = 0;
				ctx->render.duration = ctx->output.duration;
				if (ctx->render.state == RD_PLAYING) {
					ctx->output.track_started = true;
					ctx->render.track_start_time = gettime_ms();
					UNLOCK_O;
					LOG_INFO("[%p] track %u started at %u", ctx, index, ctx->render.track_start_time);
					wake_controller(ctx);
				} else UNLOCK_O;
			}
			break;
		}
		default:
			LOG_WARN("[%p]: unknown notification %u", event);
			break;
	 }
 }


/*---------------------------------------------------------------------------*/
void sq_init(char *ip, u16_t port)
{
	strcpy(sq_ip, ip);
	sq_port = port;

	decode_init();
}

/*---------------------------------------------------------------------------*/
void sq_stop() {
	int i;

	for (i = 0; i < MAX_PLAYER; i++) {
		if (thread_ctx[i].in_use) {
			sq_wipe_device(&thread_ctx[i]);
		}
	}

	decode_end();
}

/*---------------------------------------------------------------------------*/
void sq_release_device(sq_dev_handle_t handle)
{
	if (handle) {
		struct thread_ctx_s *ctx = thread_ctx + handle - 1;
		int i;

		ctx->in_use = false;
		for (i = 0; ctx->mimetypes[i]; i++) free(ctx->mimetypes[i]);
	}
}

/*---------------------------------------------------------------------------*/
sq_dev_handle_t sq_reserve_device(void *MR, bool on, char *mimetypes[], sq_callback_t callback)
{
	int idx, i;
	struct thread_ctx_s *ctx;

	/* find a free thread context - this must be called in a LOCKED context */
	for  (idx = 0; idx < MAX_PLAYER; idx++)
		if (!thread_ctx[idx].in_use) break;

	if (idx < MAX_PLAYER) 	{
		// this sets a LOT of data to proper defaults (NULL, false ...)
		memset(&thread_ctx[idx], 0, sizeof(struct thread_ctx_s));
		thread_ctx[idx].in_use = true;
	}
	else return false;

	ctx = thread_ctx + idx;
	ctx->self = idx + 1;
	ctx->on = on;
	ctx->callback = callback;
	ctx->MR = MR;

	// copy the content-type capabilities of the player
	for (i = 0; i < MAX_MIMETYPES && mimetypes[i]; i++) ctx->mimetypes[i] = strdup(mimetypes[i]);

	return idx + 1;
}

/*---------------------------------------------------------------------------*/
bool sq_run_device(sq_dev_handle_t handle, sq_dev_param_t *param)
{
	struct thread_ctx_s *ctx = &thread_ctx[handle - 1];

	memcpy(&ctx->config, param, sizeof(sq_dev_param_t));

	sprintf(ctx->cli_id, "%02x:%02x:%02x:%02x:%02x:%02x",
										  ctx->config.mac[0], ctx->config.mac[1], ctx->config.mac[2],
										  ctx->config.mac[3], ctx->config.mac[4], ctx->config.mac[5]);

	if (!stream_thread_init(ctx->config.stream_buf_size, ctx)) return false;

	if (output_init(ctx->config.output_buf_size, ctx)) {
		decode_thread_init(ctx);
		slimproto_thread_init(ctx);
#if RESAMPLE
		process_init(param->resample_options, ctx);
#endif
		return true;
	} else {
		stream_close(ctx);
		return false;
	}
}

/*--------------------------------------------------------------------------*/
void *sq_get_ptr(sq_dev_handle_t handle)
{
	if (!handle) return NULL;
	else return thread_ctx + handle - 1;
}

