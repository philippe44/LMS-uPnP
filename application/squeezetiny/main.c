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
#define LOCK_O
#define UNLOCK_O
#define LOCK_P   mutex_lock(ctx->mutex)
#define UNLOCK_P mutex_unlock(ctx->mutex)

struct thread_ctx_s thread_ctx[MAX_PLAYER];

/*----------------------------------------------------------------------------*/
/* locals */
/*----------------------------------------------------------------------------*/
static void sq_wipe_device(struct thread_ctx_s *ctx);

extern log_level	 main_loglevel;
static log_level	*loglevel = &main_loglevel;

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

	if (connect(ctx->cli_sock, (struct sockaddr *) &addr, sizeof(addr)) < 0) {
#if !WIN
		if (last_error() != EINPROGRESS) {
#else
		if (last_error() != WSAEWOULDBLOCK) {
#endif
			LOG_ERROR("[%p] unable to connect to server with cli", ctx);
			ctx->cli_sock = -1;
			return false;
		}
	}

	LOG_INFO("[%p]: opened CLI socket %d", ctx, ctx->cli_sock);
	return true;
}

#define CLI_SEND_SLEEP (10000)
#define CLI_SEND_TO (1*1000000)
/*---------------------------------------------------------------------------*/
char *cli_send_cmd(char *cmd, bool req, bool decode, struct thread_ctx_s *ctx)
{
#define CLI_LEN 2048
	char packet[CLI_LEN];
	int wait;
	size_t len;
	char *rsp = NULL;

	mutex_lock(ctx->cli_mutex);
	if (!cli_open_socket(ctx)) {
		mutex_unlock(ctx->cli_mutex);
		return NULL;
	}
	ctx->cli_timestamp = gettime_ms();

	wait = CLI_SEND_TO / CLI_SEND_SLEEP;
	cmd = cli_encode(cmd);
	if (req) len = sprintf(packet, "%s ?\n", cmd);
	else len = sprintf(packet, "%s\n", cmd);

	LOG_SDEBUG("[%p]: cmd %s", ctx, packet);
	send_packet((u8_t*) packet, len, ctx->cli_sock);
	// first receive the tag and then point to the last '\n'
	len = 0;
	while (wait--)	{
		int k;
		usleep(CLI_SEND_SLEEP);
		k = recv(ctx->cli_sock, packet + len, CLI_LEN-1 - len, 0);
		if (k < 0) {
			if (last_error() == ERROR_WOULDBLOCK) continue;
			else break;
		}
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
bool sq_set_time(sq_dev_handle_t handle, u32_t time)
{
	struct thread_ctx_s *ctx = &thread_ctx[handle - 1];
	char cmd[128];
	char *rsp;

	if (!ctx->config.dynamic.use_cli) return false;

	if (!handle || !ctx->in_use) {
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
static void sq_init_metadata(metadata_t *metadata)
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
void sq_default_metadata(metadata_t *metadata, bool init)
{
	if (init) sq_init_metadata(metadata);

	if (!metadata->title) metadata->title 	= strdup("[LMS]");
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

	if ((now - p->icy.last - 5000 > 0x7fffffff) || !p->icy.interval) return;
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

	if (artwork && (!p->icy.artwork || !strstr(p->icy.artwork, artwork)))  {
		NFREE(p->icy.artwork);

		if (!strncmp(artwork, IMAGEPROXY, strlen(IMAGEPROXY))) {
			p->icy.artwork = malloc(SQ_STR_LENGTH);
			snprintf(p->icy.artwork, SQ_STR_LENGTH, "http://%s:%s%s", ctx->server_ip, ctx->server_port, artwork);
		} else {
			p->icy.artwork = strdup(artwork);
		}

		p->icy.update = true;
	}

	NFREE(artwork);
}


/*--------------------------------------------------------------------------*/
bool sq_get_metadata(sq_dev_handle_t handle, metadata_t *metadata, bool next)
{
	struct thread_ctx_s *ctx = &thread_ctx[handle - 1];
	char cmd[1024];
	char *rsp, *p;
	u16_t idx;

	if (!handle || !ctx->in_use || !ctx->config.dynamic.use_cli) {
		if (ctx->config.dynamic.use_cli) {
			LOG_ERROR("[%p]: no handle or CLI socket %d", ctx, handle);
		}
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

	sprintf(cmd, "%s playlist remote %d", ctx->cli_id, idx);
	rsp  = cli_send_cmd(cmd, true, true, ctx);
	if (rsp && *rsp == '1') metadata->remote = true;
	else metadata->remote = false;
	NFREE(rsp)

	sprintf(cmd, "%s playlist path %d", ctx->cli_id, idx);
	rsp = cli_send_cmd(cmd, true, true, ctx);
	if (rsp && *rsp) {
		metadata->path = rsp;
		metadata->track_hash = hash32(metadata->path);
		sprintf(cmd, "%s songinfo 0 10 url:%s tags:cfldatgrK", ctx->cli_id, metadata->path);
		rsp = cli_send_cmd(cmd, false, false, ctx);
	}

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

	if (!next && metadata->duration) {
		sprintf(cmd, "%s time", ctx->cli_id);
		rsp = cli_send_cmd(cmd, true, true, ctx);
		if (rsp && *rsp) metadata->duration -= (u32_t) (atof(rsp) * 1000);
		NFREE(rsp);
	}

	if (metadata->artwork && !strncmp(metadata->artwork, IMAGEPROXY, strlen(IMAGEPROXY))) {
		char *artwork = malloc(SQ_STR_LENGTH);

		snprintf(artwork, SQ_STR_LENGTH, "http://%s:%s%s", ctx->server_ip, ctx->server_port, metadata->artwork);
		free(metadata->artwork);
		metadata->artwork = artwork;
	}

	sq_default_metadata(metadata, false);

	LOG_INFO("[%p]: idx %d\n\tartist:%s\n\talbum:%s\n\ttitle:%s\n\tgenre:%s\n\tduration:%d.%03d\n\tsize:%d\n\tcover:%s", ctx, idx,
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
void *sq_get_info(const char *urn, s32_t *size, char **content_type, char **dlna_content, u16_t interval)
{
	int i = 0;
	out_ctx_t *out = NULL;
	char *p;


	for (i = 0; i < MAX_PLAYER && !out; i++) {
		if (!thread_ctx[i].in_use) continue;
		if (strstr(urn, thread_ctx[i].out_ctx[0].buf_name)) out = &thread_ctx[i].out_ctx[0];
		if (strstr(urn, thread_ctx[i].out_ctx[1].buf_name)) out = &thread_ctx[i].out_ctx[1];
	}

	if (!out) {
		*content_type = strdup("audio/unknown");
		LOG_ERROR("Unknown URN %s", urn);
		return NULL;
	}

	/*
	When using "fake length" some players (Sonos) will try to re-open the same
	(old) file after they receive the "setnexturi" command. It's because they
	have not received the expected amount of data, so they try another time. The
	solution is to lock access to the current file and refuse to reopen it or
	getinfo from it once the full content has been set, until the player has
	requested the next file
	*/
	if (out->read_complete) {
		*content_type = strdup("audio/unknown");
		LOG_INFO("[%p]: full file already send, can't re-open", out->owner);
		return NULL;
	}

	// point to the right context !
	i--;

	/*
	Some players send 2 GET for the same URL - this is not supported and a
	HTTP NOT FOUND (404) shall be answered. But some others send a GET after
	the SETURI and then, when they receive the PLAY, they close the socket and
	send another GET, but the close might be received after the 2nd GET (due to
	threading) and this would be confused with a dual GET. A mutex with timeout
	is used to solve this, but the actual lock shall be done in the GET (open)
	request not in the INFO request.
	NB: this has an impact if a player requests an INFO on an opened file, but
	it is assumed that this case does not happen in UPnP
	*/

	if (out->read_file) {
		if (mutex_timedlock(out->mutex, 1000)) {
			LOG_WARN("[%p]: File opened twice %s (returning NOT FOUND)", out->owner, urn);
			return NULL;
		}
		else mutex_unlock(out->mutex);
	}

	// in case the whole file must be buffered
	if (out->file_size == HTTP_BUFFERED) {
		LOG_INFO("[%p]: waiting for whole file buffering", out->owner);
		if (out->duration) while (out->write_file) usleep(50000);
		else out->file_size = 1000000000;
		LOG_INFO("[%p]: file buffered", out->owner);
	}

	*size = out->file_size;
	*content_type = strdup(out->content_type);

	if (dlna_content) {
		if ((p = stristr(out->proto_info, ":DLNA")) != NULL) *dlna_content = strdup(p + 1);
		else *dlna_content = strdup("");
	}

	if (out->remote) out->icy.interval = interval;
	else out->icy.interval = 0;

	LOG_INFO("[%p]: GetInfo %s %d %s", &thread_ctx[i], urn, out->file_size, out->content_type);
	return &thread_ctx[i];
}


/*---------------------------------------------------------------------------*/
bool sq_is_remote(const char *urn)
{
	int i = 0;

	for (i = 0; i < MAX_PLAYER; i++) {
		if (!thread_ctx[i].in_use) continue;
		if (strstr(urn, thread_ctx[i].out_ctx[0].buf_name))
			return thread_ctx[i].config.send_icy && thread_ctx[i].out_ctx[0].live;
		if (strstr(urn, thread_ctx[i].out_ctx[1].buf_name))
			return thread_ctx[i].config.send_icy && thread_ctx[i].out_ctx[1].live;
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
		if (strstr(urn, thread_ctx[i].out_ctx[0].buf_name)) {
			out = &thread_ctx[i].out_ctx[0];
			thread_ctx[i].out_ctx[1].read_complete = false;
		}
		else if (strstr(urn, thread_ctx[i].out_ctx[1].buf_name)) {
			out = &thread_ctx[i].out_ctx[1];
			thread_ctx[i].out_ctx[0].read_complete = false;
		}
	}

	if (out) {
		char buf[SQ_STR_LENGTH];
		struct thread_ctx_s *ctx = out->owner; 		// for the macro to work ... ugh

		sq_reset_icy(out, true);

		LOCK_S;LOCK_O;

		// read counters are not set here. they are set
		sprintf(buf, "%s/%s", thread_ctx[i-1].config.buffer_dir, out->buf_name);
		mutex_lock(out->mutex);
		out->read_file = fopen(buf, "rb");
		LOG_INFO("[%p]:[%u] open", out->owner, out->idx);
		if (!out->read_file) out = NULL;

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

	// HTTP streaming using no size, nothing else to change, no need for CONTENT LENGTH
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
		LOG_INFO("[%p]:[%u] read total:%Ld", p->owner, p->idx, p->read_count_t);
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

		LOG_INFO("[%p]: seek %zu (c:%Ld)", ctx, bytes, (u64_t) (bytes - (p->write_count_t - p->write_count)));
		bytes -= p->write_count_t - p->write_count;
		if (bytes < 0 || bytes > p->write_count_t) {
			LOG_WARN("[%p]: seek unreachable b:%Ld t:%Ld r:%d", p->owner, bytes, p->write_count_t, p->write_count);
			bytes = 0;
		}
		rc = fseek(p->read_file, bytes, SEEK_SET);
		p->read_count += bytes;
		p->read_count_t += bytes;

		UNLOCK_S;UNLOCK_O;
	}
	return rc;
}

#define SQ_READ_SLEEP (50000)
#define SQ_READ_TO (60*1000000)
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

	wait = SQ_READ_TO / SQ_READ_SLEEP;
	do
	{
		LOCK_S;LOCK_O;
		if (p->read_file) {
			read_b = fread(dst, 1, bytes, p->read_file);
#if OSX
			// to reset EOF pointer
			fseek(p->read_file, 0, SEEK_CUR);
#endif
		}
		UNLOCK_S;LOCK_O;
		LOG_SDEBUG("[%p]:[%u] read %u bytes at %d", ctx, p->idx, read_b, wait);
		if (!read_b) usleep(SQ_READ_SLEEP);
	} while (!read_b && p->write_file && (wait == -1 || wait--) && p->owner);

	/*
	there is tiny chance for a race condition where the device is deleted
	while sleeping, so check that otherwise LOCK will create a fault
	*/
	if (!p->owner) {
		LOG_ERROR("[%p]: device stopped during wait %p", p, ctx);
		return 0;
	}

	LOG_INFO("[%p]:[%u] read %d (a:%d r:%d w:%d)", ctx, p->idx, bytes, req_bytes, read_b, wait);

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
	If there is a write_file, then we are still filling it, just wait more
	It's necessary to check read_file to avoid setting the ready-to-buffer flag
	when LMS has stopped playback. But, read_file can be NULL and this is still
	not a LMS stop request if size was indicated as the closing of the connection
	is made by the peer, not
	*/
	if (((!read_b && p->read_file) || ((p->file_size > 0 ) && (p->read_count_t >= p->file_size))) && wait && !p->write_file) {

		// see getinfo comment about locking context after full read
		p->read_complete = true;
		ctx->ready_buffering = true;
		wake_controller(ctx);

		LOG_INFO("[%p]:[%u] read (end of track) w:%d", ctx, p->idx, wait);
	}

	// exit on timeout and not read enough data ==> underrun
	if (!wait && !read_b) {
		ctx->read_to = true;
		LOG_ERROR("[%p]:[%u] underrun read:%d (r:%d)", ctx, p->idx, read_b, bytes);
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
	if (!ctx->running || !ctx->on || !handle || !ctx->in_use) return;

	switch (event) {
		case SQ_SETURI: break;
		case SQ_UNPAUSE:
		case SQ_PLAY:
			if (* (bool*) param) {
				// unsollicited PLAY done on the player direclty
				char cmd[128], *rsp;

				LOG_WARN("[%p] unsollicited play", ctx);
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
				LOG_INFO("[%p] playing notif", ctx);
				ctx->play_running = true;
				UNLOCK_S;
				wake_controller(ctx);
			}
			break;
		case SQ_PAUSE: {
			char cmd[128], *rsp;

			LOG_WARN("[%p] unsollicited pause", ctx);
			sprintf(cmd, "%s pause", ctx->cli_id);
			rsp = cli_send_cmd(cmd, false, true, ctx);
			NFREE(rsp);
			break;
		}
		case SQ_STOP:
			if (* (bool*) param) {
				char cmd[128], *rsp;

				LOG_INFO("[%p] forced STOP", ctx);
				sprintf(cmd, "%s stop", ctx->cli_id);
				rsp = cli_send_cmd(cmd, false, true, ctx);
				NFREE(rsp);
			}
			else {
				LOG_INFO("[%p] notify STOP", ctx);
				LOCK_S;
				if (ctx->play_running) {
					ctx->track_ended = true;
				 }
				ctx->play_running = false;
				UNLOCK_S;
				wake_controller(ctx);
			}
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
void sq_init(void)
{
#if WIN
	winsock_init();
#endif
}

/*---------------------------------------------------------------------------*/
void sq_release_device(sq_dev_handle_t handle)
{
	if (handle) thread_ctx[handle - 1].in_use = false;
}

/*---------------------------------------------------------------------------*/
sq_dev_handle_t sq_reserve_device(void *MR, bool on, sq_callback_t callback)
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
	ctx->on = on;
	ctx->callback = callback;
	ctx->MR = MR;

	return ctx_i + 1;
}


/*---------------------------------------------------------------------------*/
bool sq_run_device(sq_dev_handle_t handle, sq_dev_param_t *param)
{
	int i;
	struct thread_ctx_s *ctx = &thread_ctx[handle - 1];

	memcpy(&ctx->config, param, sizeof(sq_dev_param_t));

	if (strstr(ctx->config.buffer_dir, "?")) {
		GetTempPath(SQ_STR_LENGTH, ctx->config.buffer_dir);
	}

	LOG_INFO("Buffer path %s", ctx->config.buffer_dir);

	if (access(ctx->config.buffer_dir, 2)) {
		LOG_ERROR("[%p]: cannot access %s", ctx, ctx->config.buffer_dir);
		return false;
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
		ctx->out_ctx[i].read_complete = false;
		strcpy(ctx->out_ctx[i].content_type, "audio/unknown");
	}

	stream_thread_init(ctx->config.stream_buf_size, ctx);
	output_mr_thread_init(ctx->config.output_buf_size, ctx);
	slimproto_thread_init(ctx);

	return true;
}


