/*
 *  Squeezelite - lightweight headless squeezebox emulator
 *
 *  (c) Adrian Smith 2012-2014, triode1@btinternet.com
 *	(c) Philippe 2015-2023, philippe_44@outlook.com
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
#include "cache.h"

extern log_level	output_loglevel;
static log_level 	*loglevel = &output_loglevel;

#define LOCK_O 	 mutex_lock(ctx->outputbuf->mutex)
#define UNLOCK_O mutex_unlock(ctx->outputbuf->mutex)
#define LOCK_D   mutex_lock(ctx->decode.mutex)
#define UNLOCK_D mutex_unlock(ctx->decode.mutex)

#define MAX_BLOCK		(32*1024)
#define ICY_INTERVAL	16384
#define TIMEOUT			50
#define DRAIN_MAX		(5000 / TIMEOUT)

struct thread_param_s {
	struct thread_ctx_s* ctx;
	struct output_thread_s* thread;
};

static void     output_http_thread(struct thread_param_s *param);
static bool     handle_http(struct thread_ctx_s* ctx, cache_buffer* cache, bool* use_cache, bool lingering, int index, int sock);
static ssize_t 	send_with_icy(struct outputstate *out, struct buffer* backlog, int sock, const void* data, size_t bytes, int flags);
static ssize_t  send_chunked(bool chunked, struct buffer* backlog, int sock, const void* data, size_t bytes, int flags);
static ssize_t  send_backlog(struct buffer* backlog, int sock, const void* data, size_t bytes, int flags);

/*---------------------------------------------------------------------------*/
bool output_start(struct thread_ctx_s *ctx) {
	struct thread_param_s *param = calloc(sizeof(struct thread_param_s), 1);
	size_t slot;

	LOCK_O;

	// first try to find a non-running thread
	for (slot = 0; slot < ARRAY_COUNT(ctx->output_thread) && ctx->output_thread[slot].running; slot++);

	// none found, then use the lowest index lingering one
	if (slot == ARRAY_COUNT(ctx->output_thread)) for (size_t i = 0; i < ARRAY_COUNT(ctx->output_thread); i++) {
		if (ctx->output_thread[i].lingering &&
			(slot == ARRAY_COUNT(ctx->output_thread) || ctx->output_thread[i].index <= ctx->output_thread[slot].index)) {
			slot = i;
		}
	}

	// this should never happen as all threads can't be running and none is lingering
	if (slot == ARRAY_COUNT(ctx->output_thread)) {
		LOG_ERROR("[%p]: can't find a free thread, we should not be there!!!", ctx);
		return false;
	}

	// found something, now need to terminate it if it lingers
	param->thread = ctx->output_thread + slot;

	if (param->thread->lingering) {
		param->thread->running = false;
		UNLOCK_O;
		LOG_INFO("[%p]: joining thread index:%d (slot:%d)", ctx, param->thread->index, param->thread->slot);
		pthread_join(param->thread->thread, NULL);
	} else {
		UNLOCK_O;
	}

	// it's calloc
	param->thread->index = ctx->output.index;
	param->thread->running = true;
	param->ctx = ctx;

	// find a free port
	ctx->output.port = sq_local_port;
	for (int i = 0; i < 2 * MAX_PLAYER && param->thread->http <= 0; i++) {
		struct in_addr host;
		host.s_addr = INADDR_ANY;
		param->thread->http = bind_socket(host, &ctx->output.port, SOCK_STREAM);
		if (param->thread->http <= 0) ctx->output.port++;
	}

	// and listen to it
	if (param->thread->http <= 0 || listen(param->thread->http, 1)) {
		closesocket(param->thread->http);
		param->thread->http = -1;
		free(param);
		return false;
	}

	LOG_INFO("[%p]: start thread index:%d (slot:%d)", ctx, param->thread->index, param->thread->slot);
	pthread_create(&param->thread->thread, NULL, (void *(*)(void*)) &output_http_thread, param);

	return true;
}

/*---------------------------------------------------------------------------*/
bool output_abort(struct thread_ctx_s *ctx, int index) {
	for (int i = 0; i < ARRAY_COUNT(ctx->output_thread); i++) if (ctx->output_thread[i].index == index) {
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
	bool use_cache = false, acquired = false, http_ready = false, finished = false;
	fd_set rfds, wfds;
	struct buffer __obuf, *obuf = &__obuf, backlog;
	struct output_thread_s *thread = param->thread;
	struct thread_ctx_s *ctx = param->ctx;
	unsigned drain_count = DRAIN_MAX;
	u32_t start = gettime_ms();
	FILE *store = NULL;

	enum cache_type_e cache_type = CACHE_INFINITE;
	if (ctx->config.cache == HTTP_CACHE_MEMORY) cache_type = CACHE_RING;
	else if (ctx->config.cache == HTTP_CACHE_DISK && ctx->output.duration) cache_type = CACHE_FILE;
	cache_buffer *cache = cache_create(cache_type, 0);

	buf_init(obuf, 128*1024);
	buf_init(&backlog, max(ctx->output.icy.interval, MAX_BLOCK) + ICY_LEN_MAX + 2 + 16);

	free(param);

	if (*ctx->config.store_prefix) {
		char name[STR_LEN];
		snprintf(name, sizeof(name), "%s/" BRIDGE_URL "%u-out#%u#.%s", ctx->config.store_prefix, thread->index, 
			thread->http, mimetype_to_ext(ctx->output.mimetype));
		store = fopen(name, "wb");
	}

	LOG_INFO("[%p]: thread index:%d (slot:%d) started, listening socket %u (cache:%d)", ctx, thread->index, thread->slot, thread->http, cache_type);

	while (thread->running) {
		if (sock == -1) {
			struct timeval timeout = { 0, 50 * 1000 };

			FD_ZERO(&rfds);
			FD_SET(thread->http, &rfds);

			if (select(thread->http + 1, &rfds, NULL, NULL, &timeout) > 0) {
				sock = accept(thread->http, NULL, NULL);
				set_nonblock(sock);
				http_ready = finished = false;
				buf_flush(&backlog);
				FD_ZERO(&wfds);
				FD_ZERO(&rfds);
			}

			if (sock != -1 && ctx->running) {
				LOG_INFO("[%p]: got HTTP connection %u", ctx, sock);
			} else continue;
		}

		// don't need to loop too fast as we use a write fd
		FD_SET(sock, &rfds);
		bool res = true;
		struct timeval timeout = { 0, TIMEOUT * 1000 };
		int n = select(sock + 1, &rfds, &wfds, NULL, &timeout);
		
		// need to wait till we have an initialized codec
		if (!acquired && n > 0) {
			// don't bother locking decoder, there is no race condition
			if (ctx->decode.new_stream) {
				// and yes, Windows is so bad that we can't use select() as a timer...
				usleep(25 * 1000);
				continue;
			}
			acquired = true;

			LOCK_O;
			_output_new_stream(obuf, store, ctx);
			UNLOCK_O;

			LOG_INFO("[%p]: got codec, drain is %u (waited %u)", ctx, obuf->size, gettime_ms() - start);
		}

		// should be the HTTP headers
		if (n > 0 && FD_ISSET(sock, &rfds)) {
			http_ready = res = handle_http(ctx, cache, &use_cache, thread->lingering, thread->index, sock);
		}
	
		// something wrong happened or master connection closed
		if (n < 0 || !res) {
			LOG_INFO("[%p]: HTTP close %d (bytes %zd) (n:%d res:%d)", ctx, sock, cache->total, n, res);
			closesocket(sock);
			sock = -1;
			/* when streaming fails, decode will be completed but new_stream never happened, 
			 * so output thread is blocked until the player closes the connection at which 
			 * point we must exit and release slimproto (case where bytes == 0)	*/
			if (n < 0 && !cache->total && ctx->decode.state == DECODE_COMPLETE) {
				ctx->output.completed = true;
				LOG_ERROR("[%p]: streaming failed, exiting", ctx);
				break;
			}
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

		/* _output_fill pulls some data from outpubuf and make it ready in obuf for HTTP layer. It
		 * returns true if there is still data to process, whether it actually produces bytes or 
		 * not. So it is important to call and test it before checking decoder state as we should 
		 * only bother when there is nothing more to do (for the current track) as we don't want to
		 * consume next track's data. Draining starts as soon as decoder is COMPLETE or ERROR (no 
		 * LOCK_D, not critical) and STMd will only be requested when "complete" has been set, so 2
		 * different tracks will never co-exist in outpufbuf. In flow mode, STMd will be sent as soon
		 * as decode finishes *and* track has already started, so two tracks will co-exist in outputbuf
		 * and this is needed for crossfade. Pulling audio from outputbuf must be continuous and 
		 * draining will self-reset every time a decoding restarts. There is a risk that if a player 
		 * has a very large buffer, the whole next track is decoded (COMPLETE), sent in outputbuf, 
		 * transfered to obuf which is then fully sent to the player before that track even starts, so
		 * as soon as it actually starts, decoder states moves to STOPPED, STMd is sent but new data 
		 * does not arrive before the test below happens, so output closes the socks and lingers. Note
		 * as well that drain_count is not a proper timer, but it starts only to decrement when decoder
		 * is STOPPED and after all outputbuf has been process, so when still sending obuf, the time 
		 * counted depends when the player releases the wfds, which is not predictible. Still, as soon 
		 * as obuf is empty, this is chunks of TIMEOUT, so it's very unlikey that while emptying
		 * obuf, the decoder has not restarted if there	is a next track. The lingering mode is here so
		 * that players that re-open the connection even after everything has been sent (Sonos during a
		 * pause) can be served */

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
			LOG_INFO("[%p]: draining (%zu bytes)", ctx, cache->total);
		}

		/* now we are surely running but for the forgetful, we need this backlog mechanism because
		 * we can't use a blocking socket. If we don't, the output buffer gets lock while we block
		 * in send and the whole slimproto state machine is stalled. Still, the reality of the use
		 * of it seems to be limited to writing the trailing \r\n in chunked mode, probably because
		 * we wait for socket to be writable. But in theory, a writable socket does not guarantee
		 * there is enough available space */

		if (!FD_ISSET(sock, &wfds) && (use_cache || _buf_used(obuf) || _buf_used(&backlog))) {
			// we can't write but we have to, let's wait for select() 
			FD_SET(sock, &wfds);
		} else if (_buf_used(&backlog)) {
			// we have some backlog, give it priority
			send_backlog(&backlog, sock, NULL, 0, 0);
		} else if (use_cache || _buf_used(obuf)) {
			// only get what we can process (ignore result because all is always sent/backlog'd)
			size_t chunk = ctx->output.icy.interval ? ctx->output.icy.remain : MAX_BLOCK, bytes = chunk;
			uint8_t* readp = NULL;

			// try to source from cache first if we have to
			if (use_cache) {
				readp = cache->read_inner(cache, &bytes);
				if (!readp) use_cache = false;
			}

			// if nothing in cache, then we are (back to) normal source
			if (!readp && _buf_used(obuf)) {
				bytes = min(_buf_cont_read(obuf), chunk);
				cache->write(cache, obuf->readp, bytes);
				readp = obuf->readp;
				_buf_inc_readp(obuf, bytes);
			}

			// some might be in backlog, but it will be sent later (we never really know anyway what send() does)
			if (readp) send_with_icy(&ctx->output, &backlog, sock, readp, bytes, 0);
			else FD_ZERO(&wfds);
	
			LOG_SDEBUG("[%p] sent %u bytes (total: %u)", ctx, bytes, cache->total);
		} else if (finished) {
			LOG_INFO("[%p]: socket %d closed, now lingering", ctx, sock);
			thread->lingering = true;
			shutdown_socket(sock);
			sock = -1;
		} else if (!drain_count) {
			if (ctx->output.chunked) send_backlog(&backlog, sock, "0\r\n\r\n", 5, 0);
			finished = true;
			LOG_INFO("[%p]: full data sent (%zu)", ctx, cache->total);
		} else {
			// we don't have anything to send, let select read or sleep
			FD_ZERO(&wfds);
		}

		UNLOCK_O;
	}

	LOG_INFO("[%p]: finishing thread index:%d (slot:%d) - sent %zu bytes", ctx, thread->index, thread->slot, cache->total);

	buf_destroy(&backlog);
	buf_destroy(obuf);
	cache_delete(cache);

	// in chunked mode, a full chunk might not have been sent (due to TCP)
	if (sock != -1) shutdown_socket(sock);
	shutdown_socket(thread->http);
	if (store) fclose(store);

	LOCK_O;

	thread->http = -1;
	thread->lingering = false;

	if (thread->running) {
		// if we self-terminate, nobody will join us so free resources now
		pthread_detach(thread->thread);
		thread->running = false;
	}

	if (ctx->output.encode.flow) {
		_output_end_stream(NULL, ctx);
		// need to have slimproto move on in case of stream failure
		ctx->output.completed = true;
	}

	UNLOCK_O;
	LOG_INFO("[%p]: exited thread index:%d (slot:%d)", ctx, thread->index, thread->slot);
}

/*----------------------------------------------------------------------------*/
ssize_t send_backlog(struct buffer *backlog, int sock, const void* data, size_t bytes, int flags) {
	ssize_t sent = 0;

	// if there is no backlog buffer, it should be a blocking socket so just send
	if (!backlog) return send(sock, data, bytes, flags);

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

/*----------------------------------------------------------------------------*/
static bool handle_http(struct thread_ctx_s *ctx, cache_buffer* cache, bool *use_cache, bool lingering, int index, int sock)
{
	char* body = NULL, * request = NULL, * p = NULL;
	key_data_t headers[64], resp[16] = { { NULL, NULL } };
	int len, id;

	if (!http_parse_simple(sock, &request, headers, &body, &len)) {
		LOG_WARN("[%p]: http parsing error %s", ctx, request);
		NFREE(body);
		NFREE(request);
		kd_free(headers);
		return false;
	}

	// we could always claim to be 1.1 though
	char* head = NULL, *response = NULL;
	enum { ANY, SONOS, CHROMECAST } type;
	bool send_body = strstr(request, "HEAD") == NULL;
	
	LOG_INFO("[%p]: received %s", ctx, request);
	sscanf(request, "%*[^/]/" BRIDGE_URL "%d", &id);

	LOG_INFO("[%p]: HTTP headers\n%s", ctx, p = kd_dump(headers));
	NFREE(p);

	if (((p = kd_lookup(headers, "USER-AGENT")) != NULL) && strcasestr(p, "sonos")) {
		type = SONOS;
		LOG_INFO("[%p]: Sonos mode", ctx);
	} else if (kd_lookup(headers, "CAST-DEVICE-CAPABILITIES")) {
		type = CHROMECAST;
		LOG_INFO("[%p]: Chromecast mode", ctx);
	} else type = ANY;

	ctx->output.chunked = strstr(request, "HTTP/1.1") != NULL && ctx->output.length == HTTP_LENGTH_CHUNKED;

	// check if add ICY metadata is needed (only on live stream)
	if (ctx->output.icy.allowed && ((p = kd_lookup(headers, "Icy-MetaData")) != NULL) && atol(p)) {
		kd_vadd(resp, "icy-metaint", "%u", ICY_INTERVAL);
		LOCK_O;
		ctx->output.icy.interval = ctx->output.icy.remain = ICY_INTERVAL;
		ctx->output.icy.updated = true;
		UNLOCK_O;
	} else ctx->output.icy.interval = 0;

	// handle various DLNA headers
	if ((p = kd_lookup(headers, "transferMode.dlna.org")) != NULL) kd_add(resp, "transferMode.dlna.org", p);
	if (kd_lookup(headers, "getcontentFeatures.dlna.org")) {
		char* dlna_features = format_to_dlna(ctx->output.format, cache->infinite, !ctx->output.duration && !ctx->output.encode.flow);
		kd_add(resp, "contentFeatures.dlna.org", dlna_features);
		free(dlna_features);
	}
	if (kd_lookup(headers, "getAvailableSeekRange.dlna.org")) {
		kd_vadd(resp, "availableSeekRange.dlna.org", "0 bytes=%zu-%zu", 
			    cache->total - cache->level(cache),  cache->total - 1);
	}

	// are we opening the expected file
	if (id != index) {
		LOG_WARN("wrong file requested, refusing %u %d", id, index);
		head = "HTTP/1.0 410 Gone";
		kd_free(resp);
		send_body = false;
	} else {
		// by defautl use cache and restart from 0 (will be changed below if needed)
		*use_cache = true;
		cache->set_offset(cache, 0);
		int64_t length = ctx->output.length;

		/* Sonos is a pile of crap: when paused (on mp3 or flac) it will leave the connection open, 
		 * then closes and and re-opens it on resume but request the whole file. Still, even if given
		 * properly the whole resource, it fails once it has received about what he already got. We 
		 * can fool it by sending ANYTHING with a content-length (required) of an insane value. Then 
		 * we close the connection which forces it to re-open it again and this time it asks for a 
		 * proper range request but we need to answer 206 without a content-range (which is not 
		 * compliant) or it fails as well */

		if ((p = kd_lookup(headers, "Range")) != NULL && cache->total) {
			size_t offset = 0;
			(void) !sscanf(p, "bytes=%zu", &offset);

			// this is not an initial request (there is cache), so if offset is 0, we are all set
			if (offset) {
				if (!lingering && cache->total == offset) {
					// special case where we just continue so we'll do a 200 with no cache
					*use_cache = false;
				} else if (cache->scope(cache, offset) == 0) {
					// resend range with proper range header except for Sonos...
					head = ctx->output.chunked ? "HTTP/1.1 206 Partial Content" : "HTTP/1.0 206 Partial Content";
					if (type != SONOS) kd_vadd(resp, "Content-Range", "bytes %zu-%zu/*", offset, cache->total - 1);
					cache->set_offset(cache, offset);
					LOG_INFO("[%p]: serving partial content %zu->%zu", ctx, offset, cache->total - 1);
					length = 0;
				} else if (lingering && offset >= cache->total) {
					// there is an offset out of scope and we are drained, we are tapping in estimated length
					send_body = false;
					head = "HTTP/1.0 416 Range Not Satisfiable";
					kd_free(resp);
					kd_vadd(resp, "Content-Range", "*/%zu", cache->total);
					LOG_INFO("[%p]: range cannot be satisfied %zu->%zu", ctx, offset, cache->total);
				} else {
					// this likely means we are being probed toward the end of the file (which we don't have)
					head = ctx->output.chunked ? "HTTP/1.1 206 Partial Content" : "HTTP/1.0 206 Partial Content";
					size_t avail = min(cache->total, length - offset);
					cache->set_offset(cache, cache->total - avail);
					kd_vadd(resp, "Content-Range", "bytes %zu-%zu/%zu", offset, offset + avail - 1, (size_t) length);
					LOG_INFO("[%p]: being probed at %zu but have %zu/%" PRIu64 ", using offset at %zu", ctx, offset,
							 cache->total, length, cache->total - avail);
					length = 0;
				}
			}
		} else if (cache->total) {
			// see Sonos above but also Sonos re-opens stream when icy to add it (not a resume!)
			if (type == SONOS && !ctx->output.icy.interval) length = UINT32_MAX;
			LOG_INFO("[%p]: serving with cache from %zu (cached:%zu)", ctx, cache->total - cache->level(cache), cache->total);
		} else {
			// normal request, don't use cache
			*use_cache = false;
		}

		// add normal headers
		kd_add(resp, "Server", "squeezebox-bridge");
		kd_add(resp, "Accept-Ranges", "bytes");
		kd_add(resp, "Content-Type", ctx->output.mimetype);
		kd_add(resp, "Connection", "close");

		if (send_body) {
			// size might have been updated, last chance to update chunked mode
			if (length > 0) {
				ctx->output.chunked = false;
				kd_vadd(resp, "Content-Length", "%" PRId64, length);
			} else if (ctx->output.chunked) {
				kd_add(resp, "Transfer-Encoding", "chunked");
			}
		}
	}

	// unless instructed otherwise use a 200 with the correct HTTP version
	if (!head) head = ctx->output.chunked ? "HTTP/1.1 200 OK" : "HTTP/1.0 200 OK";
	response = http_send(sock, head, resp);
	LOG_INFO("[%p]: responding:\n%s", ctx, response);

	NFREE(body);
	NFREE(response);
	NFREE(request);
	kd_free(resp);
	kd_free(headers);

	return send_body;
}
