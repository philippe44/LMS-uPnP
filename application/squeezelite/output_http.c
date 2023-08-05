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
#define TAIL_SIZE		(2048*1024)
#define HEAD_SIZE		65536
#define ICY_INTERVAL	16384
#define TIMEOUT			50
#define SLEEP			50
#define DRAIN_MAX		(5000 / TIMEOUT)

struct thread_param_s {
	struct thread_ctx_s *ctx;
	struct output_thread_s *thread;
};

static void 	output_http_thread(struct thread_param_s *param);
static ssize_t 	handle_http(struct thread_ctx_s *ctx, int sock, int thread_index,
						   size_t bytes, struct buffer *obuf, bool *header);
static void 	mirror_header(key_data_t *src, key_data_t *rsp, char *key);
static ssize_t 	send_with_icy(struct thread_ctx_s *ctx, int sock, const void *buf,
							 ssize_t *len, int flags);

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
	int i;

	for (i = 0; i < 2; i++) if (ctx->output_thread[i].index == index) {
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
	bool http_ready = false, done = false;
	int sock = -1;
	char chunk_frame_buf[16] = "", *chunk_frame = chunk_frame_buf;
	bool acquired = false;
	size_t hpos = 0, bytes = 0, hsize = 0;
	ssize_t chunk_count = 0;
	u8_t *hbuf = malloc(HEAD_SIZE);
	fd_set rfds, wfds;
	struct buffer __obuf, *obuf = &__obuf;
	struct output_thread_s *thread = param->thread;
	struct thread_ctx_s *ctx = param->ctx;
	unsigned drain_count = DRAIN_MAX;
	u32_t start = gettime_ms();
	FILE *store = NULL;

	free(param);
	buf_init(obuf, HTTP_STUB_DEPTH + 512*1024);

	if (*ctx->config.store_prefix) {
		char name[STR_LEN];
		snprintf(name, sizeof(name), "%s/" BRIDGE_URL "%u-out#%u#.%s", ctx->config.store_prefix, thread->index, 
			thread->http, mimetype_to_ext(ctx->output.mimetype));
		store = fopen(name, "wb");
	}

	/*
	This function is higly non-linear and painful to read at first, I agree
	but it's also much easier, at the end, than a series of intricated if/else.
	Read it carefully, and then it's pretty simple
	*/

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

		// should be the HTTP headers (works with non-blocking socket)
		if (n > 0 && FD_ISSET(sock, &rfds)) {
			bool header = false;
			ssize_t offset = handle_http(ctx, sock, thread->index, bytes, obuf, &header);

			http_ready = res = (offset >= 0 && offset <= bytes + 1);

			// need to re-send header (Sonos)
			if (http_ready && header) {
				hpos = hsize;
				LOG_INFO("[%p]: re-sending header %u bytes", ctx, hpos);
			} else hpos = 0;

			// reset chunking and
			*chunk_frame = '\0';
			chunk_count = 0;
		}

		// something wrong happened or master connection closed
		if (n < 0 || !res) {
			LOG_INFO("[%p]: HTTP close %d (bytes %zd) (n:%d res:%d)", ctx, sock, bytes, n, res);
			closesocket(sock);
			sock = -1;
			/*
			When streaming fails, decode will be completed but new_stream
			never happened, so output thread is blocked until the player
			closes the connection at which point we must exit and release
			slimproto (case where bytes == 0).
			*/
			LOCK_D;
			if (n < 0 && !bytes && ctx->decode.state == DECODE_COMPLETE) {
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

		// need to send the header as it's a restart (Sonos!) - no ICY
		if (hpos) {
			ssize_t sent = send(sock, hbuf + hsize - hpos, hpos, 0);
			if (sent > 0) hpos -= sent;
			if (!hpos) {
				LOG_INFO("[%p]: finished header re-sent", ctx);
				closesocket(sock);
				sock = -1;
			} else {
				FD_SET(sock, &wfds);
				LOG_DEBUG("[%p]: sending from head %zd", ctx, sent);
			}
			continue;
		}

		// first send any chunk framing (header, footer)
		if (*chunk_frame) {
			if (FD_ISSET(sock, &wfds)) {
				int n = send(sock, chunk_frame, strlen(chunk_frame), 0);
				if (n > 0) chunk_frame += n;
			} else FD_SET(sock, &wfds);
			continue;
		}

		// then exit if needed (must be after footer has been sent - if any)
		if (done) {
			LOG_INFO("[%p]: self-exit ", ctx);
			break;
		}

		LOCK_O;

		// slimproto has not released us yet or we have been stopped
		if (ctx->output.state != OUTPUT_RUNNING) {
			UNLOCK_O;
			continue;
		}

		/*
		Pull some data from outpubuf. In non-flow mode, order of test matters
		as pulling from	outputbuf should stop once draining has	started,
		otherwise it will start reading data of next track. Draining starts as
		soon as decoder	is COMPLETE or ERROR (no LOCK_D, not critical) and STMd
		will only be requested when "complete" has been set, so two different
		tracks will never co-exist in outpufbuf
		In flow mode, STMd will be sent as soon as decode finishes *and* track
		has already started, so two tracks will co-exist in outputbuf and this
		is needed for crossfade. Pulling audio from outputbuf must be continuous
		and draining will self-reset every time a decoding restarts. There is a
		risk that if a player has a very large buffer, the whole next track is
		decoded (COMPLETE), sent in outputbuf, transfered to obuf which is then
		fully sent to the player before that track even starts, so as soon as it
		actually starts, decoder states moves to STOPPED, STMd is sent but new
		data does not arrive before the test below happens, so output thread
		exits. I don't know how to prevent that from happening, except by using
		horrific timers. Note as well that drain_count is not a proper timer,
		but it starts only to decrement when decoder is STOPPED and after all
		outputbuf has been process, so when still sending obuf, the time counted
		depends when the player releases the wfds, which is not predictible.
		Still, as soon as obuf is empty, this is chunks of TIMEOUT, so it's very
		unlikey that while emptying obuf, the decoder has not restarted if there
		is a next track
		*/

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
			LOG_INFO("[%p]: draining (%zu bytes)", ctx, bytes);
		}

		// now are surely running - socket is non blocking, so this is fast
		if (_buf_used(obuf)) {
			ssize_t	sent, space;

			// we cannot write, so don't bother
			if (!FD_ISSET(sock, &wfds)) {
				FD_SET(sock, &wfds);
				UNLOCK_O;
				continue;
			}

			space = min(_buf_cont_read(obuf), MAX_BLOCK);

			// if chunked mode start by sending the header
			if (chunk_count) space = min(space, chunk_count);
			else if (ctx->output.chunked) {
				chunk_count = min(space, MAX_CHUNK_SIZE);
				sprintf(chunk_frame_buf, "%zx\r\n", chunk_count);
				chunk_frame = chunk_frame_buf;
				UNLOCK_O;
				continue;
			}

			sent = send_with_icy(ctx, sock, (void*) obuf->readp, &space, 0);

			if (sent > 0) {
				if (bytes < HEAD_SIZE) {
					memcpy(hbuf + bytes, obuf->readp, min(space, HEAD_SIZE - bytes));
					hsize += min(space, HEAD_SIZE - bytes);
				}

				// check for end of chunk - space cannot be bigger than chunk!
				if (chunk_count) {
					chunk_count -= sent;
					if (!chunk_count) {
						strcpy(chunk_frame_buf, "\r\n");
						chunk_frame = chunk_frame_buf;
					}
				}

				_buf_inc_readp(obuf, space);
				bytes += space;

				LOG_SDEBUG("[%p] sent %u bytes (total: %u)", ctx, space, bytes);
			}
		} else {
			// check if all sent
			if (!drain_count) {
				if (ctx->output.chunked) {
					strcpy(chunk_frame_buf, "0\r\n\r\n");
					chunk_frame = chunk_frame_buf;
				}
				done = true;
			}
			// we don't have anything to send, let select read or sleep
			FD_ZERO(&wfds);
		}

		UNLOCK_O;
	}

	NFREE(hbuf);
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

	LOG_INFO("[%p]: end thread %d (%zu bytes)", ctx, thread == ctx->output_thread ? 0 : 1, bytes);
}

/*----------------------------------------------------------------------------*/
static ssize_t send_with_icy(struct thread_ctx_s *ctx, int sock, const void *buf, ssize_t *len, int flags) {
	struct outputstate *p = &ctx->output;
	ssize_t bytes = 0;

	// ICY not active, just send
	if (!p->icy.interval) {
		bytes = send(sock, buf, *len, flags);
		if (bytes > 0) *len = bytes;
		else *len = 0;
		return *len;
	}

	/*
	len is what we are authorized to send, due to chunk encoding so don't go
	over even if this is to send ICY metadata, we'll have to do it next time,
	hence this painful "buffer" system
	*/

	// ICY is active
	if (!p->icy.remain && !p->icy.count) {
		int len_16 = 0;

		LOG_SDEBUG("[%p]: ICY checking", ctx);

		if (p->icy.updated) {
			char *format = (p->icy.artwork && *p->icy.artwork) ?
							"NStreamTitle='%s%s%s';StreamURL='%s';" :
							"NStreamTitle='%s%s%s';";
			// there is room for 1 extra byte at the beginning for length
			len_16 = sprintf(p->icy.buffer, format,
							 p->icy.artist, *p->icy.artist ? " - " : "",
							 p->icy.title, p->icy.artwork) - 1;
			LOG_INFO("[%p]: ICY update\n\t%s\n\t%s\n\t%s", ctx, p->icy.artist, p->icy.title, p->icy.artwork);
			len_16 = (len_16 + 15) / 16;
		}

		p->icy.buffer[0] = len_16;
		p->icy.size = p->icy.count = len_16 * 16 + 1;
		p->icy.remain = p->icy.interval;
		p->icy.updated = false;
	}

	// write ICY pending data
	if (p->icy.count) {
		bytes = min(*len, p->icy.count);
		bytes = send(sock, p->icy.buffer + p->icy.size - p->icy.count, bytes, flags);
		// socket is non-blocking (will prevent data send below as well)
		if (bytes <= 0) bytes = *len = 0;
		else p->icy.count -= bytes;
	}

	// write data if remaining space and no icy to send (if icy send was partial)
	if (!p->icy.count && bytes < *len) {
		*len = min(*len - bytes, p->icy.remain);
		*len = send(sock, buf, *len, flags);
		// socket is non-blocking
		if (*len < 0) *len = 0;
		else p->icy.remain -= *len;
	} else *len = 0;

	// return 0 when send fails
	return bytes + *len;
}

/*----------------------------------------------------------------------------*/
/*
So far, the diversity of behavior of UPnP devices is too large to do anything
that work for enough of them and handle byte seeking. So, we are either with
chunking or not and that's it. All this works very well with player that simply
suspend the connection using TCP, but if they close it and want to resume (i.e.
they request a range, we'll restart from where we were and mostly it will not be
acceptable by the player, so then use the option seek_after_pause
*/
static ssize_t handle_http(struct thread_ctx_s *ctx, int sock, int thread_index,
						   size_t bytes, struct buffer *obuf, bool *header)
{
	char *body = NULL, *request = NULL, *str = NULL;
	key_data_t headers[64], resp[16] = { { NULL, NULL } };
	char *head = "HTTP/1.1 200 OK";
	int len, index;
	ssize_t res = 0;
	char format;
	enum { ANY, SONOS, CHROMECAST } type;

	if (!http_parse_simple(sock, &request, headers, &body, &len)) {
		LOG_WARN("[%p]: http parsing error %s", ctx, request);
		res = -1;
		goto cleanup;
	}

	LOG_INFO("[%p]: received %s", ctx, request);
	sscanf(request, "%*[^/]/" BRIDGE_URL "%d", &index);

	LOG_INFO("[%p]: HTTP headers\n%s", ctx, str = kd_dump(headers));
	NFREE(str);

	if (((str = kd_lookup(headers, "USER-AGENT")) != NULL) && strcasestr(str, "sonos")) {
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
	format = mimetype_to_format(ctx->output.mimetype);
	ctx->output.icy.count = 0;
	if (ctx->output.icy.allowed && (format == 'm' || format == 'a') &&
	    ((str = kd_lookup(headers, "Icy-MetaData")) != NULL) && atol(str)) {
		kd_vadd(resp, "icy-metaint", "%u", ICY_INTERVAL);
		LOCK_O;
		ctx->output.icy.interval = ctx->output.icy.remain = ICY_INTERVAL;
		ctx->output.icy.updated = true;
		UNLOCK_O;
	} else ctx->output.icy.interval = 0;

	// are we opening the expected file
	if (index != thread_index) {
		LOG_WARN("wrong file requested, refusing %u %d", index, thread_index);
		head = "HTTP/1.1 410 Gone";
		res = -1;
	} else {
		//kd_add(resp, "Accept-Ranges", "none");
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
			// a range request - might happen even when we said NO RANGE !!!
			if ((str = kd_lookup(headers, "Range")) != NULL) {
				int offset = 0;
				sscanf(str, "bytes=%u", &offset);
				// when range cannot be satisfied, just continue where we were
				if (offset < bytes) {
					head = "HTTP/1.1 206 Partial Content";
					if (type != SONOS) kd_vadd(resp, "Content-Range", "bytes %u-%zu/*", offset, bytes);
					res = offset + 1;
					obuf->readp = obuf->buf + offset % obuf->size;
				} else if (offset) {
                    LOG_INFO("[%p]: range cannot be satisfied %zu/%zu", ctx, offset, bytes);
                }
			} else if (bytes && type == SONOS && !ctx->output.icy.interval) {
				// Sonos client re-opening the connection, so make it believe we
				// have a 2G length - thus it will sent a range-request
				if (ctx->output.length < 0) kd_vadd(resp, "Content-Length", "%zu", INT_MAX);
				chunked = false;
				*header = true;
			} else if (bytes) {
				// re-opening an existing connection, resend from beginning
				obuf->readp = obuf->buf;
				LOG_INFO("[%p]: re-opening a connection at %zu", ctx, bytes);
				if (bytes > HTTP_STUB_DEPTH) {
					LOG_WARN("[%p]: head is lost %zu", ctx, bytes);
				}
			}

			// set chunked mode
			if (strstr(request, "1.1") && ctx->output.length == -3 && chunked) {
				ctx->output.chunked = true;
				kd_add(resp, "Transfer-Encoding", "chunked");
			}
		} else {
			// do not send body if request is HEAD
			res = -1;
		}
	}

	str = http_send(sock, head, resp);

	LOG_INFO("[%p]: responding:\n%s", ctx, str);

cleanup:
	NFREE(body);
	NFREE(str);
	NFREE(request);
	kd_free(resp);
	kd_free(headers);

	return res;
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
