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

#include "squeezelite.h"

extern log_level	output_loglevel;
static log_level 	*loglevel = &output_loglevel;

#define LOCK_O 	 mutex_lock(ctx->outputbuf->mutex)
#define UNLOCK_O mutex_unlock(ctx->outputbuf->mutex)
#define LOCK_D   mutex_lock(ctx->decode.mutex)
#define UNLOCK_D mutex_unlock(ctx->decode.mutex)

#define MAX_CHUNK_SIZE	(256*1024)
#define MAX_BLOCK		(32*1024)
#define ICY_INTERVAL	16384
#define TIMEOUT			50
#define SLEEP			50
#define DRAIN_MAX		(5000 / TIMEOUT)

struct thread_param_s {
	struct thread_ctx_s *ctx;
	struct output_thread_s *thread;
};

struct http_ctx_s {
	struct buffer header;
	int index;
	size_t total, pos;
};

static void 	output_http_thread(struct thread_param_s *param);
static bool 	handle_http(struct http_ctx_s *http, struct thread_ctx_s *ctx, int sock, struct buffer *obuf);
static void 	mirror_header(key_data_t *src, key_data_t *rsp, char *key);
static ssize_t 	send_with_icy(struct outputstate *out, struct buffer* backlog, int sock, const void* data, size_t bytes, int flags);
static ssize_t  send_chunked(bool chunked, struct buffer* backlog, int sock, const void* data, size_t bytes, int flags);
static ssize_t  send_backlog(struct buffer* backlog, int sock, const void* data, size_t bytes, int flags);

/*---------------------------------------------------------------------------*/
bool output_start(struct thread_ctx_s *ctx) {
	struct thread_param_s *param = malloc(sizeof(struct thread_param_s));
	int i = 0;

	// start the http server thread (get an available one first)
	if (ctx->output_thread[0].running) param->thread = ctx->output_thread + 1;
	else param->thread = ctx->output_thread;

	param->thread->index = ctx->output.index;
	param->thread->running = true;
	param->ctx = ctx;

	// find a free port
	ctx->output.port = sq_local_port;
	do {
		struct in_addr host;
		host.s_addr = INADDR_ANY;
		param->thread->http = bind_socket(host, &ctx->output.port, SOCK_STREAM);
	} while (param->thread->http < 0 && ctx->output.port++ && i++ < 2 * MAX_PLAYER);

	// and listen to it
	if (param->thread->http <= 0 || listen(param->thread->http, 1)) {
		closesocket(param->thread->http);
		param->thread->http = -1;
		free(param);
		return false;
	}

	LOG_INFO("[%p]: start thread %d", ctx, param->thread == ctx->output_thread ? 0 : 1);

	pthread_create(&param->thread->thread, NULL, (void *(*)(void*)) &output_http_thread, param);

	return true;
}

/*---------------------------------------------------------------------------*/
bool output_abort(struct thread_ctx_s *ctx, int index) {
	for (int i = 0; i < 2; i++) if (ctx->output_thread[i].index == index) {
		LOCK_O;
		ctx->output_thread[i].running = false;
		UNLOCK_O;
		pthread_join(ctx->output_thread[i].thread, NULL);
		return true;
	}
	return false;
}

/*---------------------------------------------------------------------------*/
static void output_http_thread(struct thread_param_s *param) {
	int sock = -1;
	bool acquired = false, http_ready = false, finished = false;
	fd_set rfds, wfds;
	struct buffer __obuf, *obuf = &__obuf, backlog;
	struct output_thread_s *thread = param->thread;
	struct thread_ctx_s *ctx = param->ctx;
	unsigned drain_count = DRAIN_MAX;
	u32_t start = gettime_ms();
	FILE *store = NULL;

	struct http_ctx_s http_ctx;
	memset(&http_ctx, 0, sizeof(struct http_ctx_s));
	http_ctx.index = thread->index;
	http_ctx.total = http_ctx.pos = 0;
	buf_init(&http_ctx.header, 65536);

	buf_init(obuf, HTTP_STUB_DEPTH + 512*1024);
	buf_init(&backlog, max(ctx->output.icy.interval, MAX_BLOCK) + ICY_LEN_MAX + 2 + 16);

	free(param);

	if (*ctx->config.store_prefix) {
		char name[STR_LEN];
		snprintf(name, sizeof(name), "%s/" BRIDGE_URL "%u-out#%u#.%s", ctx->config.store_prefix, http_ctx.index, 
			thread->http, mimetype_to_ext(ctx->output.mimetype));
		store = fopen(name, "wb");
	}

	while (thread->running) {
		struct timeval timeout = {0, 0};
		bool res = true;
		int n;

		if (sock == -1) {
			struct timeval timeout = {0, TIMEOUT*1000};

			FD_ZERO(&rfds);
			FD_SET(thread->http, &rfds);

			if (select(thread->http + 1, &rfds, NULL, NULL, &timeout) > 0) {
				sock = accept(thread->http, NULL, NULL);
				set_nonblock(sock);
				http_ready = false;
				buf_flush(&backlog);
				FD_ZERO(&wfds);
			}

			if (sock != -1 && ctx->running) {
				LOG_INFO("[%p]: got HTTP connection %u", ctx, sock);
			} else continue;
		}

		FD_ZERO(&rfds);
		FD_SET(sock, &rfds);

		// short wait if obuf has free space and there is something to process
		timeout.tv_usec = _buf_used(ctx->outputbuf) && _buf_space(obuf) > HTTP_STUB_DEPTH ?
									TIMEOUT*1000 / 10 : TIMEOUT*1000;

		n = select(sock + 1, &rfds, &wfds, NULL, &timeout);

		// need to wait till we have an initialized codec
		if (!acquired && n > 0) {
			LOCK_D;
			if (ctx->decode.new_stream) {
				UNLOCK_D;
				// not very elegant but let's not consume all CPU
				usleep(SLEEP*1000);
				continue;
			}
			acquired = true;
			UNLOCK_D;

			LOCK_O;
			_output_new_stream(obuf, store, ctx);
			UNLOCK_O;

			LOG_INFO("[%p]: drain is %u (waited %u)", ctx, obuf->size, gettime_ms() - start);
		}

		// should be the HTTP headers
		if (n > 0 && FD_ISSET(sock, &rfds)) {
			http_ready = res = handle_http(&http_ctx, ctx, sock, obuf);
		}

		// something wrong happened or master connection closed
		if (n < 0 || !res) {
			LOG_INFO("[%p]: HTTP close %d (bytes %zd) (n:%d res:%d)", ctx, sock, http_ctx.total, n, res);
			closesocket(sock);
			sock = -1;
			/* when streaming fails, decode will be completed but new_stream never happened, 
			 * so output thread is blocked until the player closes the connection at which 
			 * point we must exit and release slimproto (case where bytes == 0)	*/
			LOCK_D;
			if (n < 0 && !http_ctx.total && ctx->decode.state == DECODE_COMPLETE) {
				ctx->output.completed = true;
				LOG_ERROR("[%p]: streaming failed, exiting", ctx);
				UNLOCK_D;
				break;
			}
			UNLOCK_D;
			continue;
		}

		// got a connection but a select timeout, so no HTTP headers yet
		if (!http_ready) continue;

		LOCK_O;

		// slimproto has not released us yet or we have been stopped
		if (ctx->output.state != OUTPUT_RUNNING) {
			UNLOCK_O;
			continue;
		}

		/* pull some data from outpubuf. In non-flow mode, order of test matters as pulling 
		 * from outputbuf should stop once draining has started, otherwise it will start 
		 * reading data of next track. Draining starts as soon as decoder is COMPLETE or ERROR
		 * (no LOCK_D, not critical) and STMd will only be requested when "complete" has been
		 * set, so two different	tracks will never co-exist in outpufbuf. In flow mode, STMd 
		 * will be sent as soon as decode finishes *and* track has already started, so two 
		 * tracks will co-exist in outputbuf and this is needed for crossfade. Pulling audio 
		 * from outputbuf must be continuous and draining will self-reset every time a decoding
		 * restarts. There is a risk that if a player has a very large buffer, the whole next 
		 * track is decoded (COMPLETE), sent in outputbuf, transfered to obuf which is then
		 * fully sent to the player before that track even starts, so as soon as it actually 
		 * starts, decoder states moves to STOPPED, STMd is sent but new data does not arrive 
		 * before the test below happens, so output thread exits. I don't know how to prevent 
		 * that from happening, except by using horrific timers. Note as well that drain_count
		 * is not a proper timer, but it starts only to decrement when decoder is STOPPED and 
		 * after all outputbuf has been process, so when still sending obuf, the time counted
		 * depends when the player releases the wfds, which is not predictible. Still, as soon 
		 * as obuf is empty, this is chunks of TIMEOUT, so it's very unlikey that while emptying
		 * obuf, the decoder has not restarted if there	is a next track */

		if (ctx->output.encode.flow) {
			// drain_count is not really time, but close enough
			if (!_output_fill(obuf, store, ctx) && ctx->decode.state == DECODE_STOPPED) drain_count--;
			else drain_count = DRAIN_MAX;
		} else if (drain_count && !_output_fill(obuf, store, ctx) && ctx->decode.state > DECODE_RUNNING) {
			// full track pulled from outputbuf, draining from obuf
			_output_end_stream(obuf, ctx);
			ctx->output.completed = true;
			drain_count = 0;
			wake_controller(ctx);
			LOG_INFO("[%p]: draining (%zu bytes)", ctx, http_ctx.total);
		}

		/* now we are surely running but for the forgetful, we need this backlog mechanism because
		 * we can't use a blocking socket. If we don't, the output buffer gets lock while we block
		 * in send and the whole slimproto state machine is stalled. Still, the reality of the use
		 * of it seems to be limited to writing the trailing \r\n in chunked mode, probably because
		 * we wait for socket to be writable. But in theory, a writable socket does not guarantee
		 * there is enough available space */

		if (!FD_ISSET(sock, &wfds) && (_buf_used(obuf) || _buf_used(&backlog))) {
			// we can't write but we have to, let's wait for select() 
			FD_SET(sock, &wfds);
		} else if (_buf_used(&backlog)) {
			// we have some backlog, give it priority
			send_backlog(&backlog, sock, NULL, 0, 0);
		} else if (_buf_used(obuf)) {
			// only get what we can process (ignore result because all is always sent/backlog'd)
			size_t bytes = min(_buf_cont_read(obuf), ctx->output.icy.interval ? ctx->output.icy.remain : MAX_BLOCK);
			send_with_icy(&ctx->output, &backlog, sock, obuf->readp, bytes, 0);

			// store header (for Sonos) and make accurate HTTP total bytes calculation
			if (http_ctx.pos < http_ctx.total) {
				http_ctx.pos += bytes;
				if (http_ctx.pos >= http_ctx.total) {
					LOG_INFO("[%p]: finished re-sending at %zu", ctx, http_ctx.pos);
				}
			} else {
				http_ctx.total = http_ctx.pos += bytes;
				_buf_write(&http_ctx.header, obuf->readp, min(bytes, _buf_cont_write(&http_ctx.header) - 1));
			}

			// some might be in backlog, but it will be sent later (we never really know anyway what send() does)
			_buf_inc_readp(obuf, bytes);
			LOG_SDEBUG("[%p] sent %u bytes (total: %u)", ctx, bytes, http_ctx.total);

		} else if (!drain_count) {
			// if all has been sent, add a closing empty chunk if needed
			if (ctx->output.chunked) send_backlog(&backlog, sock, "0\r\n\r\n", 5, 0);
			finished = true;
		} else {
			// we don't have anything to send, let select read or sleep
			FD_ZERO(&wfds);
		}

		UNLOCK_O;

		// all have been sent
		if (finished && !_buf_used(&backlog)) {
			LOG_INFO("[%p]: self-exit", ctx);
			break;
		}
	}

	buf_destroy(&http_ctx.header);
	buf_destroy(&backlog);
	buf_destroy(obuf);

	// in chunked mode, a full chunk might not have been sent (due to TCP)
	if (sock != -1) shutdown_socket(sock);
	shutdown_socket(thread->http);
	if (store) fclose(store);

	LOCK_O;

	thread->http = -1;
	if (thread->running) {
		// if we self-terminate, nobody will join us so free resources now
		pthread_detach(thread->thread);
		thread->running = false;
	}

	if (ctx->output.encode.flow) {
		// terminate codec if needed
		_output_end_stream(NULL, ctx);
		// need to have slimproto move on in case of stream failure
		ctx->output.completed = true;
	}

	UNLOCK_O;

	LOG_INFO("[%p]: end thread %d (%zu bytes)", ctx, thread == ctx->output_thread ? 0 : 1, http_ctx.total);
}

/*----------------------------------------------------------------------------*/
ssize_t send_backlog(struct buffer *backlog, int sock, const void* data, size_t bytes, int flags) {
	ssize_t sent = 0;

	// try to flush backlog if any
	while (_buf_used(backlog) && sent >= 0) {
		sent = send(sock, backlog->readp, _buf_cont_read(backlog), flags);
		if (sent > 0) _buf_inc_readp(backlog, sent);
	}

	sent = 0;

	// try to send data if backlog is flushed
	if (!_buf_used(backlog)) {
		sent = send(sock, data, bytes, flags);
		if (sent < 0) sent = 0;
	}

	// store what we have not sent
	_buf_write(backlog, (u8_t*) data + sent, bytes - sent);
	return sent;
}

/*----------------------------------------------------------------------------*/
static ssize_t send_chunked(bool chunked, struct buffer *backlog, int sock, const void *data, size_t bytes, int flags) {
	if (!chunked) return send_backlog(backlog, sock, data, bytes, flags);
	
	char chunk[16];
	itoa(bytes, chunk, 16);
	strcat(chunk, "\r\n");

	send_backlog(backlog, sock, chunk, strlen(chunk), flags);
	bytes = send_backlog(backlog, sock, data, bytes, flags);
	send_backlog(backlog, sock, "\r\n", 2, flags);

	return bytes;
}

/*----------------------------------------------------------------------------*/
static ssize_t send_with_icy(struct outputstate* out, struct buffer *backlog, int sock, const void *data, size_t bytes, int flags) {
	// first send remaining data bytes wich are always smaller or equal to icy.remain
	bytes = send_chunked(out->chunked, backlog, sock, data, bytes, flags);

	// ICY not active, just send
	if (!out->icy.interval) return bytes;

	out->icy.remain -= bytes;

	// ICY is active
	if (!out->icy.remain) {
		int len_16 = 0;
		char* buffer = calloc(ICY_LEN_MAX, 1);

		LOG_DEBUG("[%p]: ICY remains", out, out->icy.remain);

		if (out->icy.updated) {
			char *format = (out->icy.artwork && *out->icy.artwork) ?
							"NStreamTitle='%s%s%s';StreamURL='%s';" :
							"NStreamTitle='%s%s%s';";
			// there is room for 1 extra byte at the beginning for length
			int len = snprintf(buffer, ICY_LEN_MAX, format,
							 out->icy.artist, *out->icy.artist ? " - " : "",
							 out->icy.title, out->icy.artwork) - 1;
			LOG_INFO("[%p]: ICY update\n\t%s\n\t%s\n\t%s", out, out->icy.artist, out->icy.title, out->icy.artwork);

			len_16 = (len + 15) / 16;
			memset(buffer + len + 1, 0, len_16 * 16 - len);
			buffer[0] = len_16;
		}

		send_chunked(out->chunked, backlog, sock, buffer, len_16 * 16 + 1, flags);
		free(buffer);

		out->icy.remain = out->icy.interval;
		out->icy.updated = false;
	}

	return bytes;
}

/* so far, the diversity of behavior of UPnP devices is too large to do anything
 * that works well for enough with all of them and handle byte seeking. All this 
 * works very well with players that simply suspend the connection using TCP, but
 * if they close it and want to resume (i.e. they request a range, we'll restart 
 * from as far as we can but we only have obuf size in memory. The seek_after_pause 
 * option can the be used as well to handle that within LMS */

 /*----------------------------------------------------------------------------*/
static bool handle_http(struct http_ctx_s *http, struct thread_ctx_s *ctx, int sock, struct buffer *obuf)
{
	char* body = NULL, * request = NULL, * p = NULL;
	key_data_t headers[64], resp[16] = { { NULL, NULL } };
	int len, index;

	if (!http_parse_simple(sock, &request, headers, &body, &len)) {
		LOG_WARN("[%p]: http parsing error %s", ctx, request);
		NFREE(body);
		NFREE(request);
		kd_free(headers);
		return false;
	}

	// we could always claim to be 1.1 though
	bool http_11 = strstr(request, "HTTP/1.1") != NULL;
	char* head = NULL, *response = NULL;
	enum { ANY, SONOS, CHROMECAST } type;
	bool success = true;
	
	LOG_INFO("[%p]: received %s", ctx, request);
	sscanf(request, "%*[^/]/" BRIDGE_URL "%d", &index);

	LOG_INFO("[%p]: HTTP headers\n%s", ctx, p = kd_dump(headers));
	NFREE(p);

	if (((p = kd_lookup(headers, "USER-AGENT")) != NULL) && strcasestr(p, "sonos")) {
		type = SONOS;
		LOG_INFO("[%p]: Sonos mode", ctx);
	} else if (kd_lookup(headers, "CAST-DEVICE-CAPABILITIES")) {
		type = CHROMECAST;
		LOG_INFO("[%p]: Chromecast mode", ctx);
	} else type = ANY;

	kd_add(resp, "Server", "squeezebox-bridge");
	kd_add(resp, "Connection", "close");
	ctx->output.chunked = false;

	// check if add ICY metadata is needed (only on live stream)
	if (ctx->output.icy.allowed && ((p = kd_lookup(headers, "Icy-MetaData")) != NULL) && atol(p) &&
		(ctx->output.format == 'm' || ctx->output.format == 'a' || ctx->output.format == '4')) {    
		kd_vadd(resp, "icy-metaint", "%u", ICY_INTERVAL);
		LOCK_O;
		ctx->output.icy.interval = ctx->output.icy.remain = ICY_INTERVAL;
		ctx->output.icy.updated = true;
		UNLOCK_O;
	} else ctx->output.icy.interval = 0;

	// are we opening the expected file
	if (index != http->index) {
		LOG_WARN("wrong file requested, refusing %u %d", index, http->index);
		head = "HTTP/1.0 410 Gone";
		kd_free(resp);
		success = false;
	} else {
		kd_add(resp, "Content-Type", ctx->output.mimetype);
		if (abs(ctx->output.length) > abs(HTTP_CHUNKED)) kd_vadd(resp, "Content-Length", "%zu", ctx->output.length);
		mirror_header(headers, resp, "transferMode.dlna.org");

		if (kd_lookup(headers, "getcontentFeatures.dlna.org")) {
			char *dlna_features = mimetype_to_dlna(ctx->output.mimetype, ctx->output.duration);
			kd_add(resp, "contentFeatures.dlna.org", dlna_features);
			free(dlna_features);
		}

		if (!strstr(request, "HEAD")) {
			bool chunked = true;
			if ((p = kd_lookup(headers, "Range")) != NULL) {
				size_t offset = 0;
				(void) !sscanf(p, "bytes=%zu", &offset);
				// when range cannot be satisfied, just continue where we were
				if (offset && offset < http->total) {
					head = (http_11 && ctx->output.length == -3) ? "HTTP/1.1 206 Partial Content" : "HTTP/1.0 206 Partial Content";
					if (type != SONOS) kd_vadd(resp, "Content-Range", "bytes %u-%zu/*", offset, http->total);
					obuf->readp = obuf->buf + offset % obuf->size;
					http->pos = offset;
					LOG_INFO("[%p]: resending %zu->%zu (%zu bytes)", ctx, offset, http->total, http->total - offset);
				} else if (offset) {
                    LOG_INFO("[%p]: range cannot be satisfied %zu->%zu", ctx, offset, http->total);
                }
			} else if (http->total && type == SONOS && !ctx->output.icy.interval) {
				/* Sonos is a pile of crap: when paused (on mp3) it will close the connection, re-open 
				 * it on resume and wants the whole file but of course we don't have it. We fool it by
				 * just sending the track's first N bytes we have stored and so that he sees it's the 
				 * same file. Then we close the connection which forces it to re-open it again with a 
				 * proper range request this time but we need to answer with no content-length (which 
				 * is not compliant) or if fails */
				kd_vadd(resp, "Content-Length", "%zu", UINT32_MAX);
				response = http_send(sock, "HTTP/1.0 200 OK", resp);
				ssize_t sent = send(sock, http->header.buf, _buf_used(&http->header), 0);
				LOG_INFO("[%p]: Sonos header resend %zd\n%s", ctx, sent, response);
				success = false;
			} else if (http->total) {
				// re-opening an existing connection, resend from the oldest we have
				if (http->total > obuf->size) {
					_buf_inc_readp(obuf, _buf_used(obuf) + 1);
					http->pos = http->total - obuf->size - 1;
					LOG_WARN("[%p]: re-opening but head is lost (resent from: %zu, sent: %zu)", ctx, http->pos, http->total);
				} else {
					obuf->readp = obuf->buf;
					http->pos = 0;
					LOG_INFO("[%p]: re-opening from zero (sent %zu)", ctx, http->total);
				}
			}

			// set chunked mode
			if (http_11 && ctx->output.length == -3 && chunked) {
				ctx->output.chunked = true;
				kd_add(resp, "Transfer-Encoding", "chunked");
			}
		} else {
			// do not send body if request is HEAD
			success = false;
		}
	}

	if (!response) {
		// unless instructed otherwise use a 200 with the correct HTTP version
		if (!head) head = ctx->output.chunked ? "HTTP/1.1 200 OK" : "HTTP/1.0 200 OK";
		response = http_send(sock, head, resp);
		LOG_INFO("[%p]: responding:\n%s", ctx, response);
	}

	NFREE(body);
	NFREE(response);
	NFREE(request);
	kd_free(resp);
	kd_free(headers);

	return success;
}


/*----------------------------------------------------------------------------*/
static void mirror_header(key_data_t *src, key_data_t *rsp, char *key) {
	char *data;

	data = kd_lookup(src, key);
	if (data) kd_add(rsp, key, data);
}

/*---------------------------------------------------------------------------*/
void wake_output(struct thread_ctx_s *ctx) {
	return;
}
