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
#define ICY_INTERVAL	32000

static ssize_t handle_http(struct thread_ctx_s *ctx, int sock, char *content_type,
						   size_t bytes, u8_t **tbuf, size_t hsize);
static void mirror_header(key_data_t *src, key_data_t *rsp, char *key);

/*---------------------------------------------------------------------------*/
static void output_thread(struct thread_ctx_s *ctx) {
	bool http_ready, done = false;
	int sock = -1;
	char *mimetype = NULL, chunk_frame_buf[16] = "", *chunk_frame = chunk_frame_buf;
	size_t hpos, bytes = 0, hsize = 0, tpos = 0;
	ssize_t chunk_count;
	u8_t *tbuf = NULL;
	u8_t *hbuf = malloc(HEAD_SIZE);
	fd_set rfds, wfds;

	/*
	This function is higly non-linear and painful to read at first, I agree
	but it's also much easier, at the end, than a series of intricated if/else.
	Read it carefully, and then it's pretty simple
	*/

	while (ctx->output_running) {
		struct timeval timeout = {0, 50*1000};
		bool res = true;
		int n;

		if (sock == -1) {
			struct timeval timeout = {0, 50*1000};

			FD_ZERO(&rfds);
			FD_SET(ctx->output.http, &rfds);

			if (select(ctx->output.http + 1, &rfds, NULL, NULL, &timeout) > 0) {
				sock = accept(ctx->output.http, NULL, NULL);
				set_nonblock(sock);
				http_ready = false;
				FD_ZERO(&wfds);
			}

			if (sock != -1 && ctx->running) {
				LOG_INFO("[%p]: got HTTP connection %u", ctx, sock);
			} else {
				// if streaming fails, must exit for slimproto
				LOCK_D; LOCK_O;
				if (!_output_bytes(ctx) && ctx->decode.state == DECODE_COMPLETE) {
					UNLOCK_O; UNLOCK_D;
					break;
				}
				UNLOCK_O; UNLOCK_D;
				continue;
			}
		}

		FD_ZERO(&rfds);
		FD_SET(sock, &rfds);

		n = select(sock + 1, &rfds, &wfds, NULL, &timeout);

		// need to wait till we get a codec, the very unlikely case of "codc"
		if (!mimetype && n > 0) {
			LOCK_D;
			if (ctx->decode.state < DECODE_READY) {
				LOG_INFO("[%p]: need to wait for codec", ctx);
				UNLOCK_D;
				// not very elegant but let's not consume all CPU
				usleep(50*1000);
				continue;
			}
			mimetype = strdup(ctx->output.mimetype);
			UNLOCK_D;
		}

		// should be the HTTP headers (works with non-blocking socket)
		if (n > 0 && FD_ISSET(sock, &rfds)) {
			ssize_t offset = handle_http(ctx, sock, mimetype, bytes, &tbuf, hsize);
			http_ready = res = (offset >= 0 && offset <= bytes + 1);

			// reset chunking and head/tails properly at every new connection
			*chunk_frame = '\0';
			chunk_count = hpos = 0;

			// this is a restart at a given position, send from tail
			if (offset > 0) {
				tpos = offset - 1;
				LOG_INFO("[%p]: tail pos %zd (need %zd)", ctx, tpos, bytes - tpos);
			// this is a restart a 0, send the head (Sonos) - never chunked!
			} else if (!offset && bytes && tbuf) hpos = hsize;
		}

		// something wrong happened or master connection closed
		if (n < 0 || !res) {
			LOG_INFO("[%p]: HTTP close %u (bytes %zd)", ctx, sock, bytes);
			closesocket(sock);
			sock = -1;
			continue;
		}

		// got a connection but a select timeout, so no HTTP headers yet
		if (!http_ready) continue;

		// need to send the header as it's a restart (Sonos!)
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

		// now are surely running - socket is non blocking, so this is fast
		if (_output_bytes(ctx) || tpos < bytes) {
			ssize_t	space;

			// we cannot write, so don't bother
			if (!FD_ISSET(sock, &wfds)) {
				FD_SET(sock, &wfds);
				UNLOCK_O;
				continue;
			}

			if (tpos < bytes) {
				space = min(bytes - tpos, MAX_BLOCK);
				LOG_DEBUG("[%p]: read from tail %zd ", ctx, space);
			} else space = min(_output_cont_bytes(ctx), MAX_BLOCK);

			// if chunked mode start by sending the header
			if (chunk_count) space = min(space, chunk_count);
			else if (ctx->output.chunked) {
				chunk_count = min(space, MAX_CHUNK_SIZE);
				sprintf(chunk_frame_buf, "%x\r\n", chunk_count);
				chunk_frame = chunk_frame_buf;
				UNLOCK_O;
				continue;
			}

			// take data from cache or from normal buffer
			if (tpos < bytes) {
				size_t i = tpos % TAIL_SIZE;
				space = min(space, TAIL_SIZE - i);
				space = send(sock, tbuf + i, space, 0);
				LOG_DEBUG("[%p]: send from tail %zd ", ctx, space);
			} else space = send(sock, (void*) _output_readp(ctx), space, 0);

			if (space > 0) {

				// first transmission, set flags for slimproto
				if (!bytes) _output_boot(ctx);

				if (bytes < HEAD_SIZE) {
					memcpy(hbuf + bytes, _output_readp(ctx), min(space, HEAD_SIZE - bytes));
					hsize += min(space, HEAD_SIZE - bytes);
				}

				// check for end of chunk - space cannot be bigger than chunk!
				if (chunk_count) {
					chunk_count -= space;
					if (!chunk_count) {
						strcpy(chunk_frame_buf, "\r\n");
						chunk_frame = chunk_frame_buf;
					}
				}

				// store new data in tail buffer
				if (tpos == bytes) {
					if (tbuf) {
						size_t i = tpos % TAIL_SIZE;
						ssize_t n = min(space, TAIL_SIZE - i);
						memcpy(tbuf + i, _output_readp(ctx), n);
						memcpy(tbuf, _output_readp(ctx), space - n);
					}
					tpos = bytes += space;
					_output_inc_readp(ctx, space);
				} else tpos += space;

				LOG_SDEBUG("[%p] sent %u bytes (total: %u)", ctx, space, bytes);
			}

			UNLOCK_O;
		} else {
			// check if all sent - LOCK_D shall always be locked before LOCK_O
			// or locked alone
			UNLOCK_O;
			LOCK_D;
			if (ctx->decode.state != DECODE_RUNNING) {
				// sending final empty chunk
				if (ctx->output.chunked) {
					strcpy(chunk_frame_buf, "0\r\n\r\n");
					chunk_frame = chunk_frame_buf;
				}
				done = true;
			}
			UNLOCK_D;
			// we don't have anything to send, let select read or sleep
			FD_ZERO(&wfds);
		}
	}

	LOG_INFO("[%p]: completed: %u bytes (gap %d)", ctx, bytes, ctx->output.length > 0 ? bytes - ctx->output.length : 0);

	LOCK_O;
	if (ctx->output_running == THREAD_RUNNING) ctx->output_running = THREAD_EXITED;
	UNLOCK_O;

	NFREE(mimetype);
	NFREE(tbuf);
	NFREE(hbuf);

	// in chunked mode, a full chunk might not have been sent (due to TCP)
	if (sock != -1) shutdown_socket(sock);
}

/*---------------------------------------------------------------------------*/
bool output_start(struct thread_ctx_s *ctx) {
	// already running, stop the thread but at least we can re-use the port
	if (ctx->output_running) {
		LOCK_O;
		ctx->output_running = THREAD_KILLED;
		// new thread must wait for decoder to give greenlight
		ctx->output.state = OUTPUT_WAITING;
		UNLOCK_O;
		pthread_join(ctx->output_thread, NULL);
	} else {
		int i = 0;

		// find a free port
		ctx->output.port = sq_port;
		ctx->output.http = bind_socket(&ctx->output.port, SOCK_STREAM);
		do {
			ctx->output.http = bind_socket(&ctx->output.port, SOCK_STREAM);
		} while (ctx->output.http < 0 && ctx->output.port++ && i++ < MAX_PLAYER);

		// and listen to it
		if (ctx->output.http <= 0 || listen(ctx->output.http, 1)) {
			closesocket(ctx->output.http);
			ctx->output.http = -1;
			return false;
		}
	}

	ctx->output_running = THREAD_RUNNING;
	pthread_create(&ctx->output_thread, NULL, (void *(*)(void*)) &output_thread, ctx);

	return true;
}

/*---------------------------------------------------------------------------*/
void output_flush(struct thread_ctx_s *ctx) {
	thread_state running;

	LOCK_O;
	running = ctx->output_running;
	ctx->output_running = THREAD_KILLED;
	ctx->render.ms_played = 0;
	/*
	Don't know actually if it's stopped or not but it will be and that stop event
	does not matter as we are flushing the whole thing. But if we want the next
	playback to work, better force that status to RD_STOPPED
	*/
	if (ctx->output.state != OUTPUT_OFF) ctx->output.state = OUTPUT_STOPPED;
	UNLOCK_O;

	if (running) {
		pthread_join(ctx->output_thread, NULL);
		LOG_INFO("[%p]: terminating output thread", ctx);
		if (ctx->output.http) {
			shutdown_socket(ctx->output.http);
			ctx->output.http = -1;
		}
	}

	LOCK_O;
	ctx->output.track_started = false;
	ctx->output.track_start = NULL;
	ctx->render.index = -1;
	UNLOCK_O;

	LOG_DEBUG("[%p]: flush output buffer", ctx);
	buf_flush(ctx->outputbuf);

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
static ssize_t handle_http(struct thread_ctx_s *ctx, int sock, char *mimetype,
						   size_t bytes, u8_t **tbuf, size_t hsize)
{
	char *body = NULL, *request = NULL, *str = NULL;
	key_data_t headers[64], resp[16] = { { NULL, NULL } };
	char *head = "HTTP/1.1 200 OK";
	int len, index;
	ssize_t res = 0;
	bool chunked = true, Sonos = false;
	char format;

	if (!http_parse(sock, &request, headers, &body, &len)) {
		LOG_WARN("[%p]: http parsing error %s", ctx, request);
		res = -1;
		goto cleanup;
	}

	LOG_INFO("[%p]: received %s", ctx, request);
	sscanf(request, "%*[^/]/" BRIDGE_URL "%d", &index);

	LOG_INFO("[%p]: HTTP headers\n%s", ctx, str = kd_dump(headers));
	NFREE(str);

	// for sonos devices, then need to have a tail of sent bytes ... crap
	if (((str = kd_lookup(headers, "USER-AGENT")) != NULL) && !strcasecmp(str, "sonos")) {
		Sonos = true;
		if (!*tbuf) {
		 *tbuf = malloc(TAIL_SIZE);
		 LOG_INFO("[%p]: Entering Sonos mode", ctx);
		}
	} else if (*tbuf ) {
		NFREE(*tbuf);
		LOG_INFO("[%p]: Exiting Sonos mode", ctx);
	}

	kd_add(resp, "Server", "squeezebox-bridge");
	kd_add(resp, "Connection", "close");

	// check if add ICY metadata is needed (only on live stream)
	ctx->output.icy.interval = ctx->output.icy.count = 0;
	format = mimetype2format(ctx->output.mimetype);
	if (ctx->config.send_icy && !ctx->output.duration &&
		(format == 'm' || format == 'a') &&
		((str = kd_lookup(headers, "Icy-MetaData")) != NULL) && atol(str)) {
		asprintf(&str, "%u", ICY_INTERVAL);
		kd_add(resp, "icy-metaint", str);
		LOCK_O;
		ctx->output.icy.interval = ctx->output.icy.remain = ICY_INTERVAL;
		// just to make sure icy will be resent
		ctx->output.icy.hash++;
		UNLOCK_O;
	}

	// are we opening the expected file
	if (index != ctx->output.index) {
		LOG_WARN("wrong file requested, refusing %u %d", index, ctx->output.index);
		head = "HTTP/1.1 410 Gone";
		res = -1;
	} else {
		kd_add(resp, "Content-Type", mimetype);
		kd_add(resp, "Accept-Ranges", "none");
		if (ctx->output.length > 0) {
			asprintf(&str, "%u", ctx->output.length);
			kd_add(resp, "Content-Length", str);
			free(str);
		}

		mirror_header(headers, resp, "TransferMode.DLNA.ORG");

		// a range request - might happen even when we said NO RANGE !!!
		if ((str = kd_lookup(headers, "Range")) != NULL) {
			int offset = 0;
			sscanf(str, "bytes=%u", &offset);
			if (offset) {
				head = "HTTP/1.1 206 Partial Content";
				res = offset + 1;
			}
		} else if (bytes && Sonos) {
			// Sonos client re-opening the connection, so make it believe we
			// have a 1G length - thus it will sent a range-request
			asprintf(&str, "%zu", hsize);
			if (ctx->output.length < 0) kd_add(resp, "Content-Length", "2048000000");
			NFREE(str);
			chunked = false;
		}
	}

	// set chunked mode
	if (strstr(request, "1.1") && ctx->output.length == -3 && chunked) {
		ctx->output.chunked = true;
		kd_add(resp, "Transfer-Encoding", "chunked");
	} else ctx->output.chunked = false;

	// do not send body if request is HEAD
	if (strstr(request, "HEAD")) res = -1;

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
