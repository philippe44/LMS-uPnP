/*
 *  Squeezelite - lightweight headless squeezebox emulator
 *
 *  (c) Adrian Smith 2012-2015, triode1@btinternet.com
 *  (c) Philippe, philippe_44@outlook.com for multi-instance modifications
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

#if USE_SSL
#include "openssl/ssl.h"
#include "openssl/err.h"
#endif

#if USE_LIBOGG && !LINKALL
static struct {
	void* handle;
	int (*ogg_stream_init)(ogg_stream_state* os, int serialno);
	int (*ogg_stream_clear)(ogg_stream_state* os);
	int (*ogg_stream_reset_serialno)(ogg_stream_state* os, int serialno);
	int (*ogg_stream_pagein)(ogg_stream_state* os, ogg_page* og);
	int (*ogg_stream_packetout)(ogg_stream_state* os, ogg_packet* op);
	int (*ogg_sync_clear)(ogg_sync_state* oy);
	char* (*ogg_sync_buffer)(ogg_sync_state* oy, long size);
	int (*ogg_sync_wrote)(ogg_sync_state* oy, long bytes);
	ogg_int64_t(*ogg_page_granulepos)(const ogg_page* og);
	int  (*ogg_page_serialno)(const ogg_page* og);
	int (*ogg_page_bos)(const ogg_page* og);
	int (*ogg_sync_pageout)(ogg_sync_state* oy, ogg_page* og);
} go;
#endif

#if USE_LIBOGG
#if LINKALL
#define OG(h, fn, ...) (ogg_ ## fn)(__VA_ARGS__)
#else
#define OG(h, fn, ...) (h)->ogg_##fn(__VA_ARGS__)
#endif
#endif

extern log_level	stream_loglevel;
static log_level 	*loglevel = &stream_loglevel;

#define LOCK_S   mutex_lock(ctx->streambuf->mutex)
#define UNLOCK_S mutex_unlock(ctx->streambuf->mutex)

#define PTR_U32(p)	((u32_t) (*(u32_t*)p))

#if USE_SSL

static SSL_CTX *SSLctx = NULL;
static int SSLcount = 0;

static int _last_error(struct thread_ctx_s* ctx) {
	if (!ctx->ssl) return last_error();
	return ctx->ssl_error ? ECONNABORTED : ERROR_WOULDBLOCK;
}

static int _recv(struct thread_ctx_s *ctx, void *buffer, size_t bytes, int options) {
	if (!ctx->ssl) return recv(ctx->fd, buffer, bytes, options);
	int n = SSL_read(ctx->ssl, (u8_t*) buffer, bytes);
	if (n <= 0) {
		int err = SSL_get_error(ctx->ssl, n);
		if (err == SSL_ERROR_ZERO_RETURN) return 0;
		ctx->ssl_error = (err != SSL_ERROR_WANT_READ && err != SSL_ERROR_WANT_WRITE);
	}
	return n;
}

static int _send(struct thread_ctx_s *ctx, void *buffer, size_t bytes, int options) {
	if (!ctx->ssl) return send(ctx->fd, buffer, bytes, options);
	int n = 0;
	do {
		ERR_clear_error();
		if ((n = SSL_write(ctx->ssl, (u8_t*) buffer, bytes)) >= 0) break;
		int err = SSL_get_error(ctx->ssl, n);
		ctx->ssl_error = (err != SSL_ERROR_WANT_READ && err != SSL_ERROR_WANT_WRITE);
	} while (!ctx->ssl_error);
	return n;
}

/*
can't mimic exactly poll as SSL is a real pain. Even if SSL_pending returns
0, there might be bytes to read but when select (poll) return > 0, there might
be no frame available. As well select (poll) < 0 does not mean that there is
no data pending
*/
static int _poll(struct thread_ctx_s *ctx, struct pollfd *pollinfo, int timeout) {
	if (!ctx->ssl) return poll(pollinfo, 1, timeout);
	if (pollinfo->events & POLLIN && SSL_pending(ctx->ssl)) {
		if (pollinfo->events & POLLOUT) poll(pollinfo, 1, 0);
		pollinfo->revents = POLLIN;
		return 1;
	}
	return poll(pollinfo, 1, timeout);
}
#else
#define _recv(ctx, buf, n, opt) recv(ctx->fd, buf, n, opt)
#define _send(ctx, buf, n, opt) send(ctx->fd, buf, n, opt)
#define _poll(ctx, pollinfo, timeout) poll(pollinfo, 1, timeout)
#define _last_error(x) last_error()
#endif

static bool send_header(struct thread_ctx_s *ctx) {
	char *ptr = ctx->stream.header;
	int len = ctx->stream.header_len;

	unsigned try = 0;
	ssize_t n;

	while (len) {
		n = _send(ctx, ptr, len, MSG_NOSIGNAL);
		if (n <= 0) {
			if (n < 0 && _last_error(ctx) == ERROR_WOULDBLOCK && try < 10) {
				LOG_SDEBUG("[%p]: retrying (%d) writing to socket", ctx, ++try);
				usleep(1000);
				continue;
			}
			LOG_WARN("[%p]: failed writing to socket: %s", ctx, strerror(_last_error(ctx)));
			ctx->stream.disconnect = LOCAL_DISCONNECT;
			ctx->stream.state = DISCONNECT;
			wake_controller(ctx);
			return false;
		}
		LOG_SDEBUG("[%p]: wrote %d bytes to socket", ctx, n);
		ptr += n;
		len -= n;
	}
	LOG_SDEBUG("[%p]: wrote header", ctx);
	return true;
}

static void _disconnect(stream_state state, disconnect_code disconnect, struct thread_ctx_s *ctx) {
	ctx->stream.state = state;
	ctx->stream.disconnect = disconnect;
#if USE_SSL
	if (ctx->ssl) {
		SSL_shutdown(ctx->ssl);
		SSL_free(ctx->ssl);
		ctx->ssl = NULL;
	}
#endif
	closesocket(ctx->fd);
	ctx->fd = -1;
#if USE_LIBOGG
	if (ctx->stream.ogg.active) {
		OG(&go, stream_clear, &ctx->stream.ogg.state);
		OG(&go, sync_clear, &ctx->stream.ogg.sync);
	}
#else
	if (ctx->stream.ogg.state == STREAM_OGG_PAGE && ctx->stream.ogg.data) free(ctx->stream.ogg.data);
	ctx->stream.ogg.data = NULL;
#endif
	if (ctx->stream.store) fclose(ctx->stream.store);
	wake_controller(ctx);
}

static int connect_socket(bool use_ssl, struct thread_ctx_s *ctx) {
	int sock = socket(AF_INET, SOCK_STREAM, 0);

	LOG_INFO("[%p]: connecting to %s:%d", ctx, inet_ntoa(ctx->stream.addr.sin_addr), ntohs(ctx->stream.addr.sin_port));

	if (sock < 0) {
		LOG_ERROR("[%p]: failed to create socket", ctx);
		return sock;
	}

	/* This is to force at least Windows to not have gigantic TCP buffer that cause a
	   FIN_WAIT_2 timeout on low bitrate files of a certain size. With no set of the value,
	   Windows uses a different amount than what it reports... oh well
    */
	unsigned int opt, len = sizeof(opt);
	getsockopt(sock, SOL_SOCKET, SO_RCVBUF, (void*)&opt, &len);
	setsockopt(sock, SOL_SOCKET, SO_RCVBUF, (void*) &opt, sizeof(opt));
	LOG_INFO("[%p]: set SO_RCVBUF at %d bytes", ctx, opt);

	set_nonblock(sock);
	set_nosigpipe(sock);

	if (tcp_connect_timeout(sock, ctx->stream.addr, 10*1000) < 0) {
		LOG_WARN("[%p]: unable to connect to server", ctx);
		closesocket(sock);
		return -1;
	}

#if USE_SSL
	if (use_ssl) {
		ctx->ssl = SSL_new(SSLctx);
		SSL_set_fd(ctx->ssl, sock);

		// add SNI
		if (*ctx->stream.host) SSL_set_tlsext_host_name(ctx->ssl, ctx->stream.host);

		// try to connect (socket is non-blocking)
		while (1) {
			int status, err = 0;

			ERR_clear_error();
			status = SSL_connect(ctx->ssl);

			// successful negotiation
			if (status == 1) break;

			// error or non-blocking requires more time
			if (status < 0) {
				err = SSL_get_error(ctx->ssl, status);
				if (err == SSL_ERROR_WANT_READ || err == SSL_ERROR_WANT_WRITE) continue;
			}

			LOG_WARN("[%p]: unable to open SSL socket %d (%d)", ctx, status, err);

			closesocket(sock);
			SSL_free(ctx->ssl);
			ctx->ssl = NULL;

			return -1;
		}
		LOG_INFO("[%p]: streaming with SSL", ctx);
	} else ctx->ssl = NULL;
#endif

	return sock;
}

static u32_t inline itohl(u32_t littlelong) {
#if SL_LITTLE_ENDIAN
	return littlelong;
#else
	return __builtin_bswap32(littlelong);
#endif
}

/* https://xiph.org/ogg/doc/framing.html
 * https://xiph.org/flac/ogg_mapping.html
 * https://xiph.org/vorbis/doc/Vorbis_I_spec.html#x1-610004.2 */

#if !USE_LIBOGG
static size_t memfind(const u8_t* haystack, size_t n, const char* needle, size_t len, size_t* offset) {
	size_t i;
	for (i = 0; i < n && *offset != len; i++) *offset = (haystack[i] == needle[*offset]) ? *offset + 1 : 0;
	return i;
}

 /* this mode is made to save memory and CPU by not calling ogg decoding function and never having
  * full packets (as a vorbis_comment can have a very large artwork. It works only at the page
  * level, which means there is a risk of missing the searched comment if they are not on the
  * first page of the vorbis_comment packet... nothing is perfect */
static void stream_ogg(struct thread_ctx_s* ctx, size_t n) {
	if (ctx->stream.ogg.state == STREAM_OGG_OFF) return;
	u8_t* p = ctx->streambuf->writep;

	while (n) {
		size_t consumed = min(ctx->stream.ogg.miss, n);

		// copy as many bytes as possible and come back later if we don't have enough
		if (ctx->stream.ogg.data) {
			memcpy(ctx->stream.ogg.data + ctx->stream.ogg.want - ctx->stream.ogg.miss, p, consumed);
			ctx->stream.ogg.miss -= consumed;
			if (ctx->stream.ogg.miss) return;
		}

		// we have what we want, let's parse
		switch (ctx->stream.ogg.state) {
		case STREAM_OGG_SYNC:
			ctx->stream.ogg.miss -= consumed;
			if (consumed) break;

			// we have to memorize position in case any of last 3 bytes match...
			size_t pos = memfind(p, n, "OggS", 4, &ctx->stream.ogg.match);
			if (ctx->stream.ogg.match == 4) {
				consumed = pos - ctx->stream.ogg.match;
				ctx->stream.ogg.state = STREAM_OGG_HEADER;
				ctx->stream.ogg.miss = ctx->stream.ogg.want = sizeof(ctx->stream.ogg.header);
				ctx->stream.ogg.data = (u8_t*)&ctx->stream.ogg.header;
				ctx->stream.ogg.match = 0;
			}
			else {
				if (!ctx->stream.ogg.match) LOG_INFO("[%p]: OggS not at expected position", ctx);
				return;
			}
			break;
		case STREAM_OGG_HEADER:
			if (!memcmp(ctx->stream.ogg.header.pattern, "OggS", 4)) {
				ctx->stream.ogg.miss = ctx->stream.ogg.want = ctx->stream.ogg.header.count;
				ctx->stream.ogg.data = ctx->stream.ogg.segments;
				ctx->stream.ogg.state = STREAM_OGG_SEGMENTS;
				// granule and page are also in little endian but that does not matter
				ctx->stream.ogg.header.serial = itohl(ctx->stream.ogg.header.serial);
			} else {
				ctx->stream.ogg.state = STREAM_OGG_SYNC;
				ctx->stream.ogg.data = NULL;
			}
			break;
		case STREAM_OGG_SEGMENTS:
			// calculate size of page using lacing values
			for (size_t i = 0; i < ctx->stream.ogg.want; i++) ctx->stream.ogg.miss += ctx->stream.ogg.data[i];
			ctx->stream.ogg.want = ctx->stream.ogg.miss;

			// acquire serial number when we are looking for headers and hit a bos
			if (ctx->stream.ogg.serial == ULLONG_MAX && (ctx->stream.ogg.header.type & 0x02)) ctx->stream.ogg.serial = ctx->stream.ogg.header.serial;

			// we have overshot and missed header, reset serial number to restart search (O and -1 are le/be)
			if (ctx->stream.ogg.header.serial == ctx->stream.ogg.serial && ctx->stream.ogg.header.granule && ctx->stream.ogg.header.granule != -1) ctx->stream.ogg.serial = ULLONG_MAX;

			// not our serial (the above protected us from granule > 0)
			if (ctx->stream.ogg.header.serial != ctx->stream.ogg.serial) {
				// otherwise, jump over data
				ctx->stream.ogg.state = STREAM_OGG_SYNC;
				ctx->stream.ogg.data = NULL;
			} else {
				ctx->stream.ogg.state = STREAM_OGG_PAGE;
				ctx->stream.ogg.data = malloc(ctx->stream.ogg.want);
			}
			break;
		case STREAM_OGG_PAGE: {
			char** tag = (char* []){ "\x3vorbis", "OpusTags", NULL };
			size_t ofs = 0;

			/* with OggFlac, we need the next page (packet) - VorbisComment is wrapped into a FLAC_METADATA
			 * and except with vorbis, comment packet starts a new page but even in vorbis, it won't span
			 * accross multiple pages */
			if (ctx->stream.ogg.flac) ofs = 4;
			else if (!memcmp(ctx->stream.ogg.data, "\x7f""FLAC", 5)) ctx->stream.ogg.flac = true;
			else for (size_t n = 0; *tag; tag++, ofs = 0) if ((ofs = memfind(ctx->stream.ogg.data, ctx->stream.ogg.want, *tag, strlen(*tag), &n)) && n == strlen(*tag)) break;

			if (ofs) {
				// u32:len,char[]:vendorId, u32:N, N x (u32:len,char[]:comment)
				char* p = (char*)ctx->stream.ogg.data + ofs;
				p += itohl(PTR_U32(p)) + 4;
				u32_t count = itohl(PTR_U32(p));
				p += 4;

				// LMS metadata format for Ogg is "Ogg", N x (u16:len,char[]:comment)
				memcpy(ctx->stream.header, "Ogg", 3);
				ctx->stream.header_len = 3;

				for (u32_t len; count--; p += len) {
					len = itohl(PTR_U32(p));
					p += 4;

					// only report what we use and don't overflow (network byte order)
					if (!strncasecmp(p, "TITLE=", 6) || !strncasecmp(p, "ARTIST=", 7) || !strncasecmp(p, "ALBUM=", 6)) {
						if (ctx->stream.header_len + len > MAX_HEADER) break;
						ctx->stream.header[ctx->stream.header_len++] = len >> 8;
						ctx->stream.header[ctx->stream.header_len++] = len;
						memcpy(ctx->stream.header + ctx->stream.header_len, p, len);
						ctx->stream.header_len += len;
						LOG_INFO("[%p]: metadata: %.*s", ctx, len, p);
					}
				}

				ctx->stream.ogg.flac = false;
				ctx->stream.meta_send = true;
				ctx->stream.ogg.serial = ULLONG_MAX;
				wake_controller(ctx);
				LOG_INFO("[%p]: metadata length: %u", ctx, ctx->stream.header_len);
			}
			free(ctx->stream.ogg.data);
			ctx->stream.ogg.data = NULL;
			ctx->stream.ogg.state = STREAM_OGG_SYNC;
			break;
		}
		default:
			break;
		}

		p += consumed;
		n -= consumed;
	}
}
#else
static void stream_ogg(struct thread_ctx_s* ctx, size_t n) {
	if (!ctx->stream.ogg.active) return;

	// fill sync buffer with all what we have
	char* buffer = OG(&go, sync_buffer, &ctx->stream.ogg.sync, n);
	memcpy(buffer, ctx->streambuf->writep, n);
	OG(&go, sync_wrote, &ctx->stream.ogg.sync, n);

	// extract a page from sync buffer
	while (OG(&go, sync_pageout, &ctx->stream.ogg.sync, &ctx->stream.ogg.page) > 0) {
		uint32_t serial = OG(&go, page_serialno, &ctx->stream.ogg.page);

		// set stream serialno if we wait for a new one (no multiplexed streams)
		if (ctx->stream.ogg.serial == ULLONG_MAX && OG(&go, page_bos, &ctx->stream.ogg.page)) {
			ctx->stream.ogg.serial = serial;
			OG(&go, stream_reset_serialno, &ctx->stream.ogg.state, serial);
		}

		// if we overshot, restart searching for headers
		int64_t granule = OG(&go.dl, page_granulepos, &ctx->stream.ogg.page);
		if (ctx->stream.ogg.serial == serial && granule && granule != -1) ctx->stream.ogg.serial = ULLONG_MAX;

		// if we don't have a serial number or it's not us, don't bring page in to avoid build-up
		if (ctx->stream.ogg.serial != serial) continue;

		// bring new page in (there should be one but multiplexed streams are not supported)
		if (OG(&go, stream_pagein, &ctx->stream.ogg.state, &ctx->stream.ogg.page)) continue;

		// get a packet (there might be more than one in a page)
		while (OG(&go, stream_packetout, &ctx->stream.ogg.state, &ctx->stream.ogg.packet) > 0) {
			size_t ofs = 0;

			// if case of OggFlac, VorbisComment is a flac METADATA_BLOC as 2nd packet (4 bytes in)
			if (ctx->stream.ogg.flac) ofs = 4;
			else if (!memcmp(ctx->stream.ogg.packet.packet, "\x7f""FLAC", 5)) ctx->stream.ogg.flac = true;
			else for (char** tag = (char* []){ "\x3vorbis", "OpusTags", NULL }; *tag && !ofs; tag++) if (!memcmp(ctx->stream.ogg.packet.packet, *tag, strlen(*tag))) ofs = strlen(*tag);

			if (!ofs) continue;

			// u32:len,char[]:vendorId, u32:N, N x (u32:len,char[]:comment)
			char* p = (char*)ctx->stream.ogg.packet.packet + ofs;
			p += itohl(PTR_U32(p)) + 4;
			u32_t count = itohl(PTR_U32(p));
			p += 4;

			// LMS metadata format for Ogg is "Ogg", N x (u16:len,char[]:comment)
			memcpy(ctx->stream.header, "Ogg", 3);
			ctx->stream.header_len = 3;

			for (u32_t len; count--; p += len) {
				len = itohl(PTR_U32(p));
				p += 4;

				// only report what we use and don't overflow (network byte order)
				if (!strncasecmp(p, "TITLE=", 6) || !strncasecmp(p, "ARTIST=", 7) || !strncasecmp(p, "ALBUM=", 6)) {
					if (ctx->stream.header_len + len > MAX_HEADER) break;
					ctx->stream.header[ctx->stream.header_len++] = len >> 8;
					ctx->stream.header[ctx->stream.header_len++] = len;
					memcpy(ctx->stream.header + ctx->stream.header_len, p, len);
					ctx->stream.header_len += len;
					LOG_INFO("[% p]: metadata: %.*s", ctx, len, p);
				}
			}

			// ogg_packet_clear does not need to be called
			ctx->stream.ogg.flac = false;
			ctx->stream.ogg.serial = ULLONG_MAX;
			ctx->stream.meta_send = true;
			wake_controller(ctx);
			LOG_INFO("[%p]: metadata length: %u", ctx, ctx->stream.header_len - 3);

			// return as we might have more than one metadata set but we want the first one
			return;
		}
	}
}
#endif

static void *stream_thread(struct thread_ctx_s *ctx) {
	while (ctx->stream_running) {

		struct pollfd pollinfo;
		size_t space;

		LOCK_S;

		/*
		It is required to use min with buf_space as it is the full space - 1,
		otherwise, a write to full would be authorized and the write pointer
		would wrap to the read pointer, making impossible to know if the buffer
		is full or empty. This has the consequence, though, that the buffer can
		never be totally full and can only wrap once the read pointer has moved
		so it is impossible to count on having a proper multiply of any number
		of bytes in the buffer
		*/
		space = min(_buf_space(ctx->streambuf), _buf_cont_write(ctx->streambuf));

		if (ctx->fd < 0 || !space || ctx->stream.state <= STREAMING_WAIT) {
			UNLOCK_S;
			usleep(100 * 1000);
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
				LOG_WARN("[%p] error reading: %s", ctx, strerror(_last_error(ctx)));
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

		if (_poll(ctx, &pollinfo, 100)) {

			LOCK_S;

			// check socket has not been closed while in poll
			if (ctx->fd < 0) {
				UNLOCK_S;
				continue;
			}

			if ((pollinfo.revents & POLLOUT) && ctx->stream.state == SEND_HEADERS) {
				if (send_header(ctx)) ctx->stream.state = RECV_HEADERS;
				ctx->stream.header_mlen = ctx->stream.header_len;
				ctx->stream.header_len = 0;
				UNLOCK_S;
				continue;
			}

			if (pollinfo.revents & (POLLIN | POLLHUP)) {

				// get response headers
				if (ctx->stream.state == RECV_HEADERS) {

					// read one byte at a time to catch end of header
					char c;

					int n = _recv(ctx, &c, 1, 0);
					if (n <= 0) {
						if (n < 0 && _last_error(ctx) == ERROR_WOULDBLOCK) {
							UNLOCK_S;
							continue;
						}
						LOG_WARN("[%p] error reading headers: %s", ctx, n ? strerror(_last_error(ctx)) : "closed");
#if USE_SSL
						if (!ctx->ssl && !ctx->stream.header_len) {
							int sock;

							// let's restart with SSL this time
							ctx->stream.header_len = ctx->stream.header_mlen;
							closesocket(ctx->fd);
							ctx->fd = -1;
							LOG_INFO("[%p] now attempting with SSL", ctx);

							// stay locked for slimproto (I know it can be long)
							sock = connect_socket(true, ctx);

							if (sock >= 0) {
								ctx->fd = sock;
								ctx->stream.state = SEND_HEADERS;
								UNLOCK_S;
								continue;
							}
						}
#endif
						_disconnect(STOPPED, LOCAL_DISCONNECT, ctx);
						UNLOCK_S;
						continue;
					}

					*(ctx->stream.header + ctx->stream.header_len) = c;
					ctx->stream.header_len++;

					if (ctx->stream.header_len > MAX_HEADER - 1) {
						LOG_ERROR("[%p]: received headers too long: %u", ctx, ctx->stream.header_len);
						_disconnect(DISCONNECT, LOCAL_DISCONNECT, ctx);
					}

					if (ctx->stream.header_len > 1 && (c == '\r' || c == '\n')) {
						ctx->stream.endtok++;
						if (ctx->stream.endtok == 4) {
							*(ctx->stream.header + ctx->stream.header_len) = '\0';
							LOG_INFO("[%p]: headers: len: %d\n%s", ctx, ctx->stream.header_len, ctx->stream.header);
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
						int n = _recv(ctx, &c, 1, 0);
						if (n <= 0) {
							if (n < 0 && _last_error(ctx) == ERROR_WOULDBLOCK) {
								UNLOCK_S;
								continue;
							}
							LOG_WARN("[%p]: error reading icy meta: %s", ctx, n ? strerror(_last_error(ctx)) : "closed");
							_disconnect(STOPPED, LOCAL_DISCONNECT, ctx);
							UNLOCK_S;
							continue;
						}
						ctx->stream.meta_left = 16 * c;
						ctx->stream.header_len = 0; // amount of received meta data
						// MAX_HEADER must be more than meta max of 16 * 255
					}

					if (ctx->stream.meta_left) {
						int n = _recv(ctx, ctx->stream.header + ctx->stream.header_len, ctx->stream.meta_left, 0);
						if (n <= 0) {
							if (n < 0 && _last_error(ctx) == ERROR_WOULDBLOCK) {
								UNLOCK_S;
								continue;
							}
							LOG_WARN("[%p]: error reading icy meta: %s", ctx, n ? strerror(_last_error(ctx)) : "closed");
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
							LOG_INFO("[%p]: icy meta: len: %u\n%s", ctx, ctx->stream.header_len, ctx->stream.header);
							ctx->stream.meta_send = true;
							wake_controller(ctx);
						}
						ctx->stream.meta_next = ctx->stream.meta_interval;
						UNLOCK_S;
						continue;
					}

				// stream body into streambuf
				} else {
					space = min(_buf_space(ctx->streambuf), _buf_cont_write(ctx->streambuf));

					if (ctx->stream.meta_interval) {
						space = min(space, ctx->stream.meta_next);
					}

					int n = _recv(ctx, ctx->streambuf->writep, space, 0);
					if (n == 0) {
						LOG_INFO("[%p]: end of stream (t:%" PRId64 ")", ctx, ctx->stream.bytes);
						_disconnect(DISCONNECT, DISCONNECT_OK, ctx);
					}
					if (n < 0 && _last_error(ctx) != ERROR_WOULDBLOCK) {
						LOG_WARN("[%p]: error reading: %s (%d)", ctx, strerror(_last_error(ctx)), _last_error(ctx));
						_disconnect(DISCONNECT, REMOTE_DISCONNECT, ctx);
					}

					if (n > 0) {
						if (ctx->stream.store) fwrite(ctx->streambuf->writep, 1, n, ctx->stream.store);
						stream_ogg(ctx, n);
						_buf_inc_writep(ctx->streambuf, n);
						ctx->stream.bytes += n;
						if (ctx->stream.meta_interval) {
							ctx->stream.meta_next -= n;
						}
					} else {
						UNLOCK_S;
						continue;
					}

					if (ctx->stream.state == STREAMING_BUFFERING && ctx->stream.bytes > ctx->stream.threshold) {
						ctx->stream.state = STREAMING_HTTP;
						wake_controller(ctx);
					}

					LOG_DEBUG("[%p]: streambuf read %d bytes", ctx, n);
				}
			}

			UNLOCK_S;

		}
		else {
			LOG_SDEBUG("[%p]: poll timeout", ctx);
		}
	}

#if USE_SSL
	if (!--SSLcount) {
		SSL_CTX_free(SSLctx);
		SSLctx = NULL;
	}
#endif

	return 0;
}

/*---------------------------------------------------------------------------*/
void stream_init(void) {
#if USE_LIBOGG && !LINKALL
	go.handle = dlopen(LIBOGG, RTLD_NOW);
	if (!go.handle) {
		LOG_INFO("ogg dlerror: %s", dlerror());
	}
	go.ogg_stream_init = dlsym(go.handle, "ogg_stream_init");
	go.ogg_stream_clear = dlsym(go.handle, "ogg_stream_clear");
	go.ogg_stream_reset_serialno = dlsym(go.handle, "ogg_stream_reset_serialno");
	go.ogg_stream_pagein = dlsym(go.handle, "ogg_stream_pagein");
	go.ogg_stream_packetout = dlsym(go.handle, "ogg_stream_packetout");
	go.ogg_sync_clear = dlsym(go.handle, "ogg_sync_clear");
	go.ogg_sync_buffer = dlsym(go.handle, "ogg_sync_buffer");
	go.ogg_sync_wrote = dlsym(go.handle, "ogg_sync_wrote");
	go.ogg_sync_pageout = dlsym(go.handle, "ogg_sync_pageout");
	go.ogg_page_bos = dlsym(go.handle, "ogg_page_bos");
	go.ogg_page_serialno = dlsym(go.handle, "ogg_page_serialno");
	go.ogg_page_granulepos = dlsym(go.handle, "ogg_page_granulepos");
#endif
}

/*---------------------------------------------------------------------------*/
void stream_end(void) {
#if USE_LIBOGG && !LINKALL
	if (go.handle) dl_close(go.Handle);
#endif
}

/*---------------------------------------------------------------------------*/
bool stream_thread_init(unsigned streambuf_size, struct thread_ctx_s *ctx) {
	pthread_attr_t attr;

	LOG_DEBUG("[%p]: streambuf size: %u", ctx, streambuf_size);
	ctx->streambuf = &ctx->__s_buf;

	buf_init(ctx->streambuf, ((streambuf_size / (BYTES_PER_FRAME * 3)) * BYTES_PER_FRAME * 3));
	if (ctx->streambuf->buf == NULL) {
		LOG_ERROR("[%p]: unable to malloc buffer", ctx);
		return false;
	}

#if USE_SSL
	if (!SSLctx) {
		SSLctx = SSL_CTX_new(SSLv23_client_method());
		if (SSLctx) SSL_CTX_set_options(SSLctx, SSL_OP_NO_SSLv2);
	}
	SSLcount++;
	ctx->ssl = NULL;
#endif

	ctx->stream_running = true;
	ctx->stream.state = STOPPED;
	ctx->stream.header = malloc(MAX_HEADER);
	ctx->stream.header[0] = '\0';
	ctx->fd = -1;

	touch_memory(ctx->streambuf->buf, ctx->streambuf->size);

	pthread_attr_init(&attr);
	pthread_attr_setstacksize(&attr, PTHREAD_STACK_MIN + STREAM_THREAD_STACK_SIZE);
	pthread_create(&ctx->stream_thread, &attr, (void *(*)(void*)) stream_thread, ctx);
	pthread_attr_destroy(&attr);

	return true;
}

void stream_close(struct thread_ctx_s *ctx) {
	LOG_INFO("[%p]: close stream", ctx);
	LOCK_S;
	ctx->stream_running = false;
	UNLOCK_S;
	pthread_join(ctx->stream_thread, NULL);
	free(ctx->stream.header);
	buf_destroy(ctx->streambuf);
}

void stream_file(const char *header, size_t header_len, unsigned threshold, struct thread_ctx_s *ctx) {
	buf_flush(ctx->streambuf);

	LOCK_S;

	ctx->stream.header_len = header_len;
	memcpy(ctx->stream.header, header, header_len);
	*(ctx->stream.header+header_len) = '\0';

	LOG_INFO("[%p]: opening local file: %s", ctx, ctx->stream.header);

#if WIN
	ctx->fd = open(ctx->stream.header, O_RDONLY | O_BINARY);
#else
	ctx->fd = open(ctx->stream.header, O_RDONLY);
#endif

	ctx->stream.state = STREAMING_FILE;
	if (ctx->fd < 0) {
		LOG_WARN("[%p]: can't open file: %s", ctx, ctx->stream.header);
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

void stream_sock(u32_t ip, u16_t port, bool use_ssl, bool use_ogg, const char *header, size_t header_len, unsigned threshold, bool cont_wait, struct thread_ctx_s *ctx) {
	int sock;
	char *p;

	memset(&ctx->stream.addr, 0, sizeof(ctx->stream.addr));
	ctx->stream.addr.sin_family = AF_INET;
	ctx->stream.addr.sin_addr.s_addr = ip;
	ctx->stream.addr.sin_port = port;

	*ctx->stream.host = '\0';
	if ((p = strcasestr(header,"Host:")) != NULL) {
		sscanf(p, "Host:%255s", ctx->stream.host);
		if ((p = strchr(ctx->stream.host, ':')) != NULL) *p = '\0';
	}

	port = ntohs(port);
	sock = connect_socket(use_ssl || port == 443, ctx);

	// try one more time with plain socket
	if (sock < 0 && port == 443 && !use_ssl) sock = connect_socket(false, ctx);

	if (sock < 0) {
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

	LOG_INFO("[%p]: header: %s", ctx, ctx->stream.header);

	ctx->stream.sent_headers = false;
	ctx->stream.bytes = 0;
	ctx->stream.threshold = threshold;

#if USE_LIBOGG
#if !LINKALL
	ctx->stream.ogg.active = use_ogg && ogg.dl.handle;
#else 
	ctx->stream.ogg.active = use_ogg;
#endif
	if (use_ogg) {
		OG(&go, stream_clear, &ctx->stream.ogg.state);
		OG(&go, sync_clear, &ctx->stream.ogg.sync);
		OG(&go, stream_init, &ctx->stream.ogg.state, -1);
	}
#else
	ctx->stream.ogg.miss = ctx->stream.ogg.match = 0;
	ctx->stream.ogg.state = use_ogg ? STREAM_OGG_SYNC : STREAM_OGG_OFF;
#endif
	ctx->stream.ogg.flac = false;
	ctx->stream.ogg.serial = ULLONG_MAX;

	if (*ctx->config.store_prefix) {
		char name[STR_LEN];
		snprintf(name, sizeof(name), "%s/" BRIDGE_URL "%u-in#%u#.%s", ctx->config.store_prefix, ctx->output.index, sock, ctx->codec->types);
		ctx->stream.store = fopen(name, "wb");
	} else {
		ctx->stream.store = NULL;
	}

	UNLOCK_S;
}

bool stream_disconnect(struct thread_ctx_s* ctx) {
	bool disc = false;
	LOCK_S;
#if USE_SSL
	if (ctx->ssl) {
		SSL_shutdown(ctx->ssl);
		SSL_free(ctx->ssl);
		ctx->ssl = NULL;
	}
#endif
	if (ctx->fd != -1) {
		closesocket(ctx->fd);
		ctx->fd = -1;
		disc = true;
	}
	ctx->stream.state = STOPPED;
#if USE_LIBOGG
	if (ctx->stream.ogg.active) {
		OG(&go, stream_clear, &ctx->stream.ogg.state);
		OG(&go, sync_clear, &ctx->stream.ogg.sync);
	}
#else
	if (ctx->stream.ogg.state == STREAM_OGG_PAGE && ctx->stream.ogg.data) free(ctx->stream.ogg.data);
	ctx->stream.ogg.data = NULL;
#endif
	if (ctx->stream.store) fclose(ctx->stream.store);

	UNLOCK_S;
	return disc;
}
