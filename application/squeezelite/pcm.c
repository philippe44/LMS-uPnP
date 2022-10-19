/*
 *  Squeezelite - lightweight headless squeezebox emulator
 *
 *  (c) Philippe, philippe_44@outlook.com for raop/multi-instance modifications
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

extern log_level decode_loglevel;
static log_level *loglevel = &decode_loglevel;

#define LOCK_S   mutex_lock(ctx->streambuf->mutex)
#define UNLOCK_S mutex_unlock(ctx->streambuf->mutex)
#define LOCK_O   mutex_lock(ctx->outputbuf->mutex)
#define UNLOCK_O mutex_unlock(ctx->outputbuf->mutex)
#if PROCESS
#define LOCK_O_direct   if (ctx->decode.direct) mutex_lock(ctx->outputbuf->mutex)
#define UNLOCK_O_direct if (ctx->decode.direct) mutex_unlock(ctx->outputbuf->mutex)
#define LOCK_O_not_direct   if (!ctx->decode.direct) mutex_lock(ctx->outputbuf->mutex)
#define UNLOCK_O_not_direct if (!ctx->decode.direct) mutex_unlock(ctx->outputbuf->mutex)
#define IF_DIRECT(x)    if (ctx->decode.direct) { x }
#define IF_PROCESS(x)   if (!ctx->decode.direct) { x }
#else
#define LOCK_O_direct   mutex_lock(ctx->outputbuf->mutex)
#define UNLOCK_O_direct mutex_unlock(ctx->outputbuf->mutex)
#define LOCK_O_not_direct
#define UNLOCK_O_not_direct
#define IF_DIRECT(x)    { x }
#define IF_PROCESS(x)
#endif

#define MAX_DECODE_FRAMES 4096
#define MIN_READ 	4096
#define MIN_SPACE	(16*1024)

struct pcm {
	unsigned bytes_per_frame;
};

/*---------------------------------------------------------------------------*/
static unsigned check_header(struct thread_ctx_s *ctx) {
	u8_t *ptr = ctx->streambuf->readp;
	unsigned bytes = 0;

	// we can safely parse the buffer as we start at the top
	if (!memcmp(ptr, "RIFF", 4) && !memcmp(ptr+8, "WAVE", 4) && !memcmp(ptr+12, "fmt ", 4)) {
		LOG_INFO("[%p]: WAVE", ctx);
		// override the server parsed values with our own
		ctx->output.channels    	=  *(u16_t*) (ptr+22);
		ctx->output.sample_rate 	= *(u32_t*) (ptr+24);
		ctx->output.sample_size 	= *(u16_t*) (ptr+34);
		ctx->output.in_endian   = 1;
		bytes = (12+8+4+4) + *(u32_t*) (ptr+16);
		LOG_INFO("[%p]: pcm size: %u rate: %u chan: %u endian:1): %u", ctx, ctx->output.sample_size, ctx->output.sample_rate, ctx->output.channels);
	} else if (!memcmp(ptr, "FORM", 4) && (!memcmp(ptr+8, "AIFF", 4) || !memcmp(ptr+8, "AIFC", 4))) {
		ptr += 12;
		LOG_INFO("[%p]: AIFF", ctx);

		// we explore as far as 22 bytes ahead of parse pointer
		while (ctx->streambuf->readp - ptr < MIN_READ - 22) {
			unsigned len = htonl(*(u32_t*) (ptr+4));
			LOG_INFO("[%p]: AIFF header: %4s len: %d", ctx, ptr, len);

			if (!memcmp(ptr, "COMM", 4)) {
				int exponent;
				// override the server parsed values with our own
				ctx->output.channels    = htons(*(u16_t*) (ptr + 8));
				ctx->output.sample_size = htons(*(u16_t*) (ptr + 14));
				ctx->output.in_endian   = 0;
				// sample rate is encoded as IEEE 80 bit extended format
				// make some assumptions to simplify processing - only use first 32 bits of mantissa
				exponent = ((*(ptr+16) & 0x7f) << 8 | *(ptr+17)) - 16383 - 31;
				ctx->output.sample_rate  = htonl(*(u32_t*) (ptr+18));
				while (exponent < 0) { ctx->output.sample_rate >>= 1; ++exponent; }
				while (exponent > 0) { ctx->output.sample_rate <<= 1; --exponent; }
				LOG_INFO("[%p]: pcm size: %u rate: %u chan: %u endian:0): %u", ctx, ctx->output.sample_size, ctx->output.sample_rate, ctx->output.channels);
			}

			if (!memcmp(ptr, "SSND", 4)) {
				unsigned offset = htonl(*(u32_t*) (ptr+8));
				bytes = ptr - ctx->streambuf->readp + offset + (8+8);
				break;
			}

			ptr += len + 8;
		}
	} else if (ctx->output.in_endian && !(*(u64_t*) ptr) && (strstr(ctx->server_version, "7.7") || strstr(ctx->server_version, "7.8"))) {
		/*
		LMS < 7.9 does not remove 8 bytes when sending aiff files but it does
		when it is a transcoding ... so this does not matter for 16 bits samples
		but it is a mess for 24 bits ... so this tries to guess what we are
		receiving
		*/
		LOG_INFO("[%p]: guessing a AIFF extra header", ctx);
		bytes = 8;
	} else {
		LOG_WARN("[%p]: unknown format - can't parse header", ctx);
	}

	return bytes;
}

/*---------------------------------------------------------------------------*/
static decode_state pcm_decode(struct thread_ctx_s *ctx) {
	size_t bytes = 0, in, out, count;
	frames_t frames;
	struct pcm *p = ctx->decode.handle;
	u8_t *iptr, ibuf[BYTES_PER_FRAME];
	u32_t *optr = NULL;

	LOCK_S;
	LOCK_O_direct;

	if (ctx->stream.state <= DISCONNECT && _buf_used(ctx->streambuf) < p->bytes_per_frame) {
		UNLOCK_O_direct;
		UNLOCK_S;
		return DECODE_COMPLETE;
	}

	IF_DIRECT(
		out = min(_buf_space(ctx->outputbuf), _buf_cont_write(ctx->outputbuf)) / BYTES_PER_FRAME;
	);
	IF_PROCESS(
		out = ctx->process.max_in_frames;
	);

	if (ctx->decode.new_stream) {
		// check headers and consume bytes if needed
		if (!ctx->config.roon_mode) bytes = check_header(ctx);
		_buf_inc_readp(ctx->streambuf, bytes);

		LOCK_O_not_direct;

		ctx->output.direct_sample_rate = ctx->output.sample_rate;
		ctx->output.sample_rate = decode_newstream(ctx->output.sample_rate, ctx->output.supported_rates, ctx);
		ctx->output.track_start = ctx->outputbuf->writep;
		if (ctx->output.fade_mode) _checkfade(true, ctx);
		ctx->decode.new_stream = false;
		p->bytes_per_frame = (ctx->output.sample_size * ctx->output.channels) / 8;

		UNLOCK_O_not_direct;

		IF_PROCESS(
			out = ctx->process.max_in_frames;
		);
	}

	IF_DIRECT(
		optr = (u32_t*) ctx->outputbuf->writep;
	);
	IF_PROCESS(
		optr = (u32_t*) ctx->process.inbuf;
	);

	bytes = min(_buf_used(ctx->streambuf), _buf_cont_read(ctx->streambuf));

	iptr = (u8_t *)ctx->streambuf->readp;
	in = bytes / p->bytes_per_frame;

	if (in == 0 && bytes > 0 && _buf_used(ctx->streambuf) >= p->bytes_per_frame) {
		memcpy(ibuf, iptr, bytes);
		memcpy(ibuf + bytes, ctx->streambuf->buf, p->bytes_per_frame - bytes);
		iptr = ibuf;
		in = 1;
	}

	frames = min(in, out);
	frames = min(frames, MAX_DECODE_FRAMES);

	ctx->decode.frames += frames;

	count = frames * ctx->output.channels;

	if (ctx->output.channels == 2) {
		if (ctx->output.sample_size == 8) {
			if (!ctx->output.in_endian)	while (count--) *optr++ = *iptr++ << 24;
			else while (count--) *optr++ = (*iptr++ ^ 0x80) << 24;
		} else if (ctx->output.sample_size == 16) {
			if (!ctx->output.in_endian) {
				while (count--) {
					*optr++ = *(iptr) << 24 | *(iptr+1) << 16;
					iptr += 2;
				}
			} else {
				while (count--) {
					*optr++ = *(iptr) << 16 | *(iptr+1) << 24;
					iptr += 2;
				}
			}
		} else if (ctx->output.sample_size == 24) {
			if (!ctx->output.in_endian) {
				while (count--) {
					*optr++ = *(iptr) << 24 | *(iptr+1) << 16 | *(iptr+2) << 8;
					iptr += 3;
				}
			} else {
				while (count--) {
					*optr++ = *(iptr) << 8 | *(iptr+1) << 16 | *(iptr+2) << 24;
					iptr += 3;
				}
			}
		} else if (ctx->output.sample_size == 32) {
			if (!ctx->output.in_endian) {
				while (count--) {
					*optr++ = *(iptr) << 24 | *(iptr+1) << 16 | *(iptr+2) << 8 | *(iptr+3);
					iptr += 4;
				}
			} else {
				while (count--) {
					*optr++ = *(iptr) | *(iptr+1) << 8 | *(iptr+2) << 16 | *(iptr+3) << 24;
					iptr += 4;
				}
			}
		}
	} else if (ctx->output.channels == 1) {
		if (ctx->output.sample_size == 8) {
			if (!ctx->output.in_endian) {
				while (count--) {
					*optr = *iptr++ << 24;
					*(optr+1) = *optr;
					optr += 2;
				}
			} else {
				while (count--) {
					*optr = (*iptr++ ^ 0x80) << 24;
					*(optr+1) = *optr;
					optr += 2;
				}
			}
		} else if (ctx->output.sample_size == 16) {
			if (!ctx->output.in_endian) {
				while (count--) {
					*optr = *(iptr) << 24 | *(iptr+1) << 16;
					*(optr+1) = *optr;
					iptr += 2;
					optr += 2;
				}
			} else {
				while (count--) {
					*optr = *(iptr) << 16 | *(iptr+1) << 24;
					*(optr+1) = *optr;
					iptr += 2;
					optr += 2;
				}
			}
		} else if (ctx->output.sample_size == 24) {
			if (!ctx->output.in_endian) {
				while (count--) {
					*optr = *(iptr) << 24 | *(iptr+1) << 16 | *(iptr+2) << 8;
					*(optr+1) = *optr;
					iptr += 3;
					optr += 2;
				}
			} else {
				while (count--) {
					*optr = *(iptr) << 8 | *(iptr+1) << 16 | *(iptr+2) << 24;
					*(optr+1) = *optr;
					iptr += 3;
					optr += 2;
				}
			}
		} else if (ctx->output.sample_size == 32) {
			if (!ctx->output.in_endian) {
				while (count--) {
					*optr++ = *(iptr) << 24 | *(iptr+1) << 16 | *(iptr+2) << 8 | *(iptr+3);
					*(optr+1) = *optr;
					iptr += 4;
					optr += 2;
				}
			} else {
				while (count--) {
					*optr++ = *(iptr) | *(iptr+1) << 8 | *(iptr+2) << 16 | *(iptr+3) << 24;
					*(optr+1) = *optr;
					iptr += 4;
					optr += 2;
				}
			}
		}
	} else {
		LOG_ERROR("[%p]: unsupported channels", ctx, ctx->output.channels);
	}

	LOG_SDEBUG("[%p]: decoded %u frames", ctx, frames);

	_buf_inc_readp(ctx->streambuf, frames * p->bytes_per_frame);

	IF_DIRECT(
		_buf_inc_writep(ctx->outputbuf, frames * BYTES_PER_FRAME);
	);
	IF_PROCESS(
		ctx->process.in_frames = frames;
	);

	UNLOCK_O_direct;
	UNLOCK_S;

	return DECODE_RUNNING;
}

/*---------------------------------------------------------------------------*/
static void pcm_open(u8_t sample_size, u32_t sample_rate, u8_t channels, u8_t endianness, struct thread_ctx_s *ctx) {
	struct pcm *p = ctx->decode.handle;
	if (!p)	p = ctx->decode.handle = malloc(sizeof(struct pcm));
	p->bytes_per_frame = BYTES_PER_FRAME;
}

/*---------------------------------------------------------------------------*/
static void pcm_close(struct thread_ctx_s *ctx) {
	if (ctx->decode.handle) free(ctx->decode.handle);
	ctx->decode.handle = NULL;
}

/*---------------------------------------------------------------------------*/
struct codec *register_pcm(void) {
	static struct codec ret = {
		'p',         // id
		"pcm,wav,aif", 		 // types
		MIN_READ,        // min read
		MIN_SPACE,     // min space
		pcm_open,   // open
		pcm_close,  // close
		pcm_decode, // decode
	};

	LOG_INFO("using pcm", NULL);
	return &ret;
}

void deregister_pcm(void) {
}

