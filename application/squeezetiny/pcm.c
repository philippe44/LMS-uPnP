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

/*---------------------------------------------------------------------------*/
static decode_state pcm_decode(struct thread_ctx_s *ctx) {
	size_t bytes, in, out, bytes_per_frame, count;
	frames_t frames;
	u8_t *iptr, buf[3*8];
	u32_t *optr;
	struct pcm *p = ctx->decode.handle;

	LOCK_S;
	LOCK_O_direct;

	iptr = (u8_t *)ctx->streambuf->readp;

	if (ctx->decode.new_stream && ctx->output.in_endian && !(*((u64_t*) iptr)) &&
		   (strstr(ctx->server_version, "7.7") || strstr(ctx->server_version, "7.8"))) {
		/*
		LMS < 7.9 does not remove 8 bytes when sending aiff files but it does
		when it is a transcoding ... so this does not matter for 16 bits samples
		but it is a mess for 24 bits ... so this tries to guess what we are
		receiving
		*/
		_buf_inc_readp(ctx->streambuf, 8);
		LOG_INFO("[%p]: guessing a AIFF extra header", ctx);
	}

	bytes = min(_buf_used(ctx->streambuf), _buf_cont_read(ctx->streambuf));
	bytes_per_frame = (ctx->output.sample_size * ctx->output.channels) / 8;

	if (ctx->stream.state <= DISCONNECT && bytes == 0) {
		UNLOCK_O_direct;
		UNLOCK_S;
		return DECODE_COMPLETE;
	}

	in = bytes / bytes_per_frame;

	IF_DIRECT(
		out = min(_buf_space(ctx->outputbuf), _buf_cont_write(ctx->outputbuf)) / BYTES_PER_FRAME;
	);
	IF_PROCESS(
		out = ctx->process.max_in_frames;
	);

	if (ctx->decode.new_stream) {
		size_t length;

		LOCK_O_not_direct;

		// header must be done here otherwise output might send it too early
		length = _output_pcm_header(ctx);
		if (ctx->config.stream_length > 0) ctx->output.length = length;

		LOG_INFO("[%p]: setting track start, estimated size %zd", ctx, length);

		ctx->output.current_sample_rate = decode_newstream(ctx->output.sample_rate, ctx->output.supported_rates, ctx);
		ctx->output.track_start = ctx->outputbuf->writep;
		if (ctx->output.fade_mode) _checkfade(true, ctx);
		ctx->decode.new_stream = false;

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

	if (in == 0 && bytes > 0 && _buf_used(ctx->streambuf) >= bytes_per_frame) {
		memcpy(buf, iptr, bytes);
		memcpy(buf + bytes, ctx->streambuf->buf, bytes_per_frame - bytes);
		iptr = buf;
		in = 1;
	}

	frames = min(in, out);
	frames = min(frames, MAX_DECODE_FRAMES);

	count = frames * ctx->output.channels;

	if (ctx->output.channels == 2) {
		if (ctx->output.sample_size == 8) {
			while (count--) {
				*optr++ = *iptr++ << 24;
			}
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
			while (count--) {
				*optr = *iptr++ << 24;
				*(optr+1) = *optr;
				optr += 2;
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

	_buf_inc_readp(ctx->streambuf, frames * bytes_per_frame);

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
	ctx->decode.handle = NULL;
}

/*---------------------------------------------------------------------------*/
static void pcm_close(struct thread_ctx_s *ctx) {
}

/*---------------------------------------------------------------------------*/
struct codec *register_pcm(void) {
	static struct codec ret = {
		'p',         // id
		"pcm,wav,aif", 		 // types
		4096,        // min read
		16*1024,     // min space
		pcm_open,   // open
		pcm_close,  // close
		pcm_decode, // decode
	};

	LOG_INFO("using pcm", NULL);
	return &ret;
}

void deregister_pcm(void) {
}


#if 0
		// mono, 16 bits
		if (ctx->output.sample_size == 16 && ctx->output.channels == 1) {
			int count = frames;

			// 2 bytes per sample and mono, expand to stereo, 1 frame per loop
			if (!ctx->output.in_endian) {
				while (count--) {
					*optr++ = *iptr;
					*optr++ = *(iptr + 1);
					*optr++ = *iptr++;
					*optr++ = *iptr++;
				}
			} else {
				while (count--) {
					*optr++ = *(iptr + 1);
					*optr++ = *iptr;
					*optr++ = *(iptr + 1);
					*optr++ = *iptr++;
					iptr++;
				}
			}
		}

		// 24 bits, the tricky one
		if (ctx->output.sample_size == 24 && ctx->output.channels == 2) {
			int count = frames * 2;

			done = true;
			// 3 bytes per sample, shrink to 2 and do 1/2 frame (1 channel) per loop
			if (!ctx->output.in_endian) {
				while (count--) {
					iptr++;
					*optr++ = *iptr++;
					*optr++ = *iptr++;
				}
			} else {
				while (count--) {
					*optr++ = *(iptr + 1);
					*optr++ = *iptr;
					iptr += 3;
				}
			}
		}
#endif



