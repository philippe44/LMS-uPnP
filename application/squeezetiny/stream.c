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

// stream thread

#include "squeezelite.h"

#include <fcntl.h>

extern log_level stream_loglevel;
static log_level *loglevel = &stream_loglevel;

#define LOCK_S   mutex_lock(ctx->streambuf->mutex)
#define UNLOCK_S mutex_unlock(ctx->streambuf->mutex)

static void send_header(struct thread_ctx_s *ctx) {
	char *ptr = ctx->stream.header;
	int len = ctx->stream.header_len;

	unsigned try = 0;
	ssize_t n;

	while (len) {
		n = send(ctx->fd, ptr, len, MSG_NOSIGNAL);
		if (n <= 0) {
			if (n < 0 && last_error() == ERROR_WOULDBLOCK && try < 10) {
				LOG_SDEBUG("[%p] retrying (%d) writing to socket", ctx, ++try);
				usleep(1000);
				continue;
			}
			LOG_WARN("[%p] failed writing to socket: %s", ctx, strerror(last_error()));
			ctx->stream.disconnect = LOCAL_DISCONNECT;
			ctx->stream.state = DISCONNECT;
			wake_controller(ctx);
			return;
		}
		LOG_SDEBUG("[%p] wrote %d bytes to socket", ctx, n);
		ptr += n;
		len -= n;
	}
	LOG_SDEBUG("[%p] wrote header", ctx);
}

bool stream_disconnect(struct thread_ctx_s *ctx) {
	bool disc = false;
	LOCK_S;
	if (ctx->fd != -1) {
		closesocket(ctx->fd);
		ctx->fd = -1;
		disc = true;
	}
	ctx->stream.state = STOPPED;
	UNLOCK_S;
	return disc;
}

static void _disconnect(stream_state state, disconnect_code disconnect, struct thread_ctx_s *ctx) {
	ctx->stream.state = state;
	ctx->stream.disconnect = disconnect;
	closesocket(ctx->fd);
	ctx->fd = -1;
	wake_controller(ctx);
}

static void *stream_thread(struct thread_ctx_s *ctx) {

	while (ctx->stream_running) {

		struct pollfd pollinfo;
		size_t space;

		LOCK_S;

		space = min(_buf_space(ctx->streambuf), _buf_cont_write(ctx->streambuf));

		if (ctx->fd < 0 || !space || ctx->stream.state <= STREAMING_WAIT) {
			UNLOCK_S;
			usleep(100000);
			continue;
		}

		if (ctx->stream.state == STREAMING_FILE) {

			int n = read(ctx->fd, ctx->streambuf->writep, space);
			if (n == 0) {
				LOG_INFO("[%p] end of stream", ctx);
				_disconnect(DISCONNECT, DISCONNECT_OK, ctx);
			}
			if (n > 0) {
				_buf_inc_writep(ctx->streambuf, n);
				ctx->stream.bytes += n;
				LOG_SDEBUG("[%p] ctx->streambuf read %d bytes", ctx, n);
			}
			if (n < 0) {
				LOG_WARN("[%p] error reading: %s", ctx, strerror(last_error()));
				_disconnect(DISCONNECT, REMOTE_DISCONNECT, ctx);
			}

			UNLOCK_S;
			continue;

		} else {

			pollinfo.fd = ctx->fd;
			pollinfo.events = POLLIN;
			if (ctx->stream.state == SEND_HEADERS) {
				pollinfo.events |= POLLOUT;
			}
		}

		UNLOCK_S;

		if (poll(&pollinfo, 1, 100)) {

			LOCK_S;

			// check socket has not been closed while in poll
			if (ctx->fd < 0) {
				UNLOCK_S;
				continue;
			}

			if ((pollinfo.revents & POLLOUT) && ctx->stream.state == SEND_HEADERS) {
				send_header(ctx);
				ctx->stream.header_len = 0;
				ctx->stream.state = RECV_HEADERS;
				UNLOCK_S;
				continue;
			}

			if (pollinfo.revents & (POLLIN | POLLHUP)) {

				// get response headers
				if (ctx->stream.state == RECV_HEADERS) {

					// read one byte at a time to catch end of header
					char c;

					int n = recv(ctx->fd, &c, 1, 0);
					if (n <= 0) {
						if (n < 0 && last_error() == ERROR_WOULDBLOCK) {
							UNLOCK_S;
							continue;
						}
						LOG_WARN("[%p] error reading headers: %s", ctx, n ? strerror(last_error()) : "closed");
						_disconnect(STOPPED, LOCAL_DISCONNECT, ctx);
						UNLOCK_S;
						continue;
					}

					*(ctx->stream.header + ctx->stream.header_len) = c;
					ctx->stream.header_len++;

					if (ctx->stream.header_len > MAX_HEADER - 1) {
						LOG_ERROR("[%p] received headers too long: %u", ctx, ctx->stream.header_len);
						_disconnect(DISCONNECT, LOCAL_DISCONNECT, ctx);
					}

					if (ctx->stream.header_len > 1 && (c == '\r' || c == '\n')) {
						ctx->stream.endtok++;
						if (ctx->stream.endtok == 4) {
							*(ctx->stream.header + ctx->stream.header_len) = '\0';
							LOG_INFO("[%p] headers: len: %d\n%s", ctx, ctx->stream.header_len, ctx->stream.header);
							ctx->stream.state = ctx->stream.cont_wait ? STREAMING_WAIT : STREAMING_BUFFERING;
							wake_controller(ctx);
						}
					} else {
						ctx->stream.endtok = 0;
					}

					UNLOCK_S;
					continue;
				}

				// receive icy meta data

				if (ctx->stream.meta_interval && ctx->stream.meta_next == 0) {
					if (ctx->stream.meta_left == 0) {
						// read meta length
						u8_t c;
						int n = recv(ctx->fd, &c, 1, 0);
						if (n <= 0) {
							if (n < 0 && last_error() == ERROR_WOULDBLOCK) {
								UNLOCK_S;
								continue;
							}
							LOG_WARN("[%p] error reading icy meta: %s", ctx, n ? strerror(last_error()) : "closed");
							_disconnect(STOPPED, LOCAL_DISCONNECT, ctx);
							UNLOCK_S;
							continue;
						}
						ctx->stream.meta_left = 16 * c;
						ctx->stream.header_len = 0; // amount of received meta data
						// MAX_HEADER must be more than meta max of 16 * 255
					}

					if (ctx->stream.meta_left) {
						int n = recv(ctx->fd, ctx->stream.header + ctx->stream.header_len, ctx->stream.meta_left, 0);
						if (n <= 0) {
							if (n < 0 && last_error() == ERROR_WOULDBLOCK) {
								UNLOCK_S;
								continue;
							}
							LOG_WARN("[%p] error reading icy meta: %s", ctx, n ? strerror(last_error()) : "closed");
							_disconnect(STOPPED, LOCAL_DISCONNECT, ctx);
							UNLOCK_S;
							continue;
						}
						ctx->stream.meta_left -= n;
						ctx->stream.header_len += n;
					}

					if (ctx->stream.meta_left == 0) {
						if (ctx->stream.header_len) {
							*(ctx->stream.header + ctx->stream.header_len) = '\0';
							LOG_INFO("[%p] icy meta: len: %u\n%s", ctx, ctx->stream.header_len, ctx->stream.header);
							ctx->stream.meta_send = true;
							wake_controller(ctx);
						}
						ctx->stream.meta_next = ctx->stream.meta_interval;
						UNLOCK_S;
						continue;
					}

				// stream body into streambuf
				} else {
					int n;

					space = min(_buf_space(ctx->streambuf), _buf_cont_write(ctx->streambuf));

					if (ctx->stream.meta_interval) {
						space = min(space, ctx->stream.meta_next);
					}

					n = recv(ctx->fd, ctx->streambuf->writep, space, 0);
					if (n == 0) {
						LOG_INFO("[%p] end of stream (t:%Ld)", ctx, ctx->stream.bytes);
						_disconnect(DISCONNECT, DISCONNECT_OK, ctx);
					}
					if (n < 0 && last_error() != ERROR_WOULDBLOCK) {
						LOG_WARN("[%p] error reading: %s", ctx, strerror(last_error()));
						_disconnect(DISCONNECT, REMOTE_DISCONNECT, ctx);
					}

					if (n > 0) {
						_buf_inc_writep(ctx->streambuf, n);
						ctx->stream.bytes += n;
						wake_output(ctx);
						if (ctx->stream.meta_interval) {
							ctx->stream.meta_next -= n;
						}
					}

					if (ctx->stream.state == STREAMING_BUFFERING && ctx->stream.bytes > ctx->stream.threshold) {
						ctx->stream.state = STREAMING_HTTP;
						wake_controller(ctx);
					}

					LOG_SDEBUG("[%p] streambuf read %d bytes", ctx, n);
				}
			}

			UNLOCK_S;

		}
		else {
			LOG_SDEBUG("[%p] poll timeout", ctx);
		}
	}

	return 0;
}


/*---------------------------------------------------------------------------*/
void stream_thread_init(unsigned stream_buf_size, struct thread_ctx_s *ctx) {

	LOG_DEBUG("[%p] streambuf size: %u", ctx, stream_buf_size);

	stream_buf_size = (stream_buf_size / (4*3)) * (4*3);
	ctx->streambuf = &ctx->__s_buf;
	buf_init(ctx->streambuf, stream_buf_size);
	if (ctx->streambuf->buf == NULL) {
		LOG_ERROR("[%p] unable to malloc buffer", ctx);
		exit(0);
	}

	ctx->stream_running = true;
	ctx->stream.state = STOPPED;
	ctx->stream.header = malloc(MAX_HEADER);
	*ctx->stream.header = '\0';

	ctx->fd = -1;

#if LINUX || FREEBSD
	touch_memory(ctx->streambuf->buf, ctx->streambuf->size);
#endif

#if LINUX || OSX || FREEBSD
	pthread_attr_t attr;
	pthread_attr_init(&attr);
	pthread_attr_setstacksize(&attr, PTHREAD_STACK_MIN + STREAM_THREAD_STACK_SIZE);
	pthread_create(&ctx->stream_thread, &attr, (void *(*)(void*)) stream_thread, ctx);
	pthread_attr_destroy(&attr);
#endif
#if WIN
	ctx->stream_thread = CreateThread(NULL, STREAM_THREAD_STACK_SIZE, (LPTHREAD_START_ROUTINE)&stream_thread, ctx, 0, NULL);
#endif
}

void stream_close(struct thread_ctx_s *ctx) {
	LOG_INFO("[%p] close stream", ctx);
	LOCK_S;
	ctx->stream_running = false;
	UNLOCK_S;
#if LINUX || OSX || FREEBSD
	pthread_join(ctx->stream_thread, NULL);
#endif
	free(ctx->stream.header);
	buf_destroy(ctx->streambuf);
}

void stream_file(const char *header, size_t header_len, unsigned threshold, struct thread_ctx_s *ctx) {
	buf_flush(ctx->streambuf);

	LOCK_S;

	ctx->stream.header_len = header_len;
	memcpy(ctx->stream.header, header, header_len);
	*(ctx->stream.header+header_len) = '\0';

	LOG_INFO("[%p] opening local file: %s", ctx, ctx->stream.header);

#if WIN
	ctx->fd = open(ctx->stream.header, O_RDONLY | O_BINARY);
#else
	ctx->fd = open(ctx->stream.header, O_RDONLY);
#endif

	ctx->stream.state = STREAMING_FILE;
	if (ctx->fd < 0) {
		LOG_WARN("[%p] can't open file: %s", ctx, ctx->stream.header);
		ctx->stream.state = DISCONNECT;
	}
	wake_controller(ctx);

	ctx->stream.cont_wait = false;
	ctx->stream.meta_interval = 0;
	ctx->stream.meta_next = 0;
	ctx->stream.meta_left = 0;
	ctx->stream.meta_send = false;
	ctx->stream.sent_headers = false;
	ctx->stream.bytes = 0;
	ctx->stream.threshold = threshold;

	UNLOCK_S;
}

void stream_sock(u32_t ip, u16_t port, const char *header, size_t header_len, unsigned threshold, bool cont_wait, struct thread_ctx_s *ctx) {
	struct sockaddr_in addr;

	int sock = socket(AF_INET, SOCK_STREAM, 0);

	if (sock < 0) {
		LOG_ERROR("[%p] failed to create socket", ctx);
		return;
	}

	memset(&addr, 0, sizeof(addr));
	addr.sin_family = AF_INET;
	addr.sin_addr.s_addr = ip;
	addr.sin_port = port;

	LOG_INFO("[%p] connecting to %s:%d", ctx, inet_ntoa(addr.sin_addr), ntohs(addr.sin_port));

	set_nonblock(sock);
	set_nosigpipe(sock);

	if (connect_timeout(sock, (struct sockaddr *) &addr, sizeof(addr), 10*1000) < 0) {
		LOG_WARN("[%p] unable to connect to server", ctx);
		LOCK_S;
		ctx->stream.state = DISCONNECT;
		ctx->stream.disconnect = UNREACHABLE;
		UNLOCK_S;
		return;
	}

	buf_flush(ctx->streambuf);

	LOCK_S;

	ctx->fd = sock;
	ctx->stream.state = SEND_HEADERS;
	ctx->stream.cont_wait = cont_wait;
	ctx->stream.meta_interval = 0;
	ctx->stream.meta_next = 0;
	ctx->stream.meta_left = 0;
	ctx->stream.meta_send = false;
	ctx->stream.header_len = header_len;
	memcpy(ctx->stream.header, header, header_len);
	*(ctx->stream.header+header_len) = '\0';

	LOG_INFO("[%p] header: %s", ctx, ctx->stream.header);

	ctx->stream.sent_headers = false;
	ctx->stream.bytes = 0;
	ctx->stream.threshold = threshold;

	UNLOCK_S;
}


