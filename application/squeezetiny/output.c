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

#define BYTE_1(n)	((u8_t) (n >> 24))
#define BYTE_2(n)	((u8_t) (n >> 16))
#define BYTE_3(n)	((u8_t) (n >> 8))
#define BYTE_4(n)	((u8_t) (n))

static void 	lpcm_pack(u8_t *dst, u8_t *src, size_t bytes, u8_t channels, int endian);
static void*	apply_gain(s32_t *p, u32_t gain, size_t frames);
size_t 			fade_gain(u32_t *gain, struct thread_ctx_s *ctx);
static void 	scale_and_pack(void *dst, u32_t *src, size_t frames, u8_t channels,
							   u8_t sample_size, int endian);

/*---------------------------------- WAVE ------------------------------------*/
static struct wave_header_s {
	u8_t 	chunk_id[4];
	u32_t	chunk_size;
	u8_t	format[4];
	u8_t	subchunk1_id[4];
	u8_t	subchunk1_size[4];
	u8_t	audio_format[2];
	u16_t	channels;
	u32_t	sample_rate;
	u32_t   byte_rate;
	u16_t	block_align;
	u16_t	bits_per_sample;
	u8_t	subchunk2_id[4];
	u32_t	subchunk2_size;
} wave_header = {
		{ 'R', 'I', 'F', 'F' },
		0,
		{ 'W', 'A', 'V', 'E' },
		{ 'f','m','t',' ' },
		{ 16, 0, 0, 0 },
		{ 1, 0 },
		0,
		0,
		0,
		0,
		0,
		{ 'd', 'a', 't', 'a' },
		0, 		//chunk_size - sizeof(struct wave_header_s) - 8 - 8
	};


/*---------------------------------- AIFF ------------------------------------*/
static struct aiff_header_s {			// need to use all u8 due to padding
	u8_t 	chunk_id[4];
	u8_t	chunk_size[4];
	u8_t	format[4];
	u8_t	common_id[4];
	u8_t	common_size[4];
	u8_t	channels[2];
	u8_t 	frames[4];
	u8_t	sample_size[2];
	u8_t	sample_rate_exp[2];
	u8_t	sample_rate_num[8];
	u8_t    data_id[4];
	u8_t	data_size[4];
	u32_t	offset;
	u32_t	blocksize;
} aiff_header = {
		{ 'F', 'O', 'R', 'M' },
#ifdef AIFF_MARKER
		{ 0x3B, 0x9A, 0xCA, 0x48 },		// adding comm, mark, ssnd and AIFF sizes
#else
		{ 0x3B, 0x9A, 0xCA, 0x2E },		// adding comm, ssnd and AIFF sizes
#endif
		{ 'A', 'I', 'F', 'F' },
		{ 'C', 'O', 'M', 'M' },
		{ 0x00, 0x00, 0x00, 0x12 },
		{ 0x00, 0x00 },
		{ 0x0E, 0xE6, 0xB2, 0x80 },		// 250x10^6 frames of 2 channels and 2 bytes each
		{ 0x00, 0x00 },
		{ 0x40, 0x0E },
		{ 0x00, 0x00 },
		{ 'S', 'S', 'N', 'D' },
		{ 0x3B, 0x9A, 0xCA, 0x08 },		// one chunk of 10^9 bytes + 8
	};


static void little16(void *dst, u16_t src);
static void little32(void *dst, u32_t src);
static void big16(void *dst, u16_t src);
static void big32(void *dst, u32_t src);


/*---------------------------------------------------------------------------*/
void _output_fill(struct buffer *buf, struct thread_ctx_s *ctx) {
	size_t bytes = _buf_space(buf);
	struct outputstate *p;

	/*
	The outputbuf contains FRAMES of 32 bits samples and 2 channels. The buf is
	what will be sent to client, so it can be any thing from header, ICY data,
	passthru audio, uncompressed audio in 8,16,24,32 and 1 or 2 channels or
	re-compressed audio.
	The gain, truncation, L24 pack, swap, fade-in/out is applied *from* the
	outputbuf and copied *to* this buf so it really has no specific alignement
	but for simplicity we won't process anything until it has free space for the
	largest block of audio which is BYTES_PER_FRAME
	*/
	if (bytes < BYTES_PER_FRAME) return;

	//just to make code more readable
	p = &ctx->output;

	// write header pending data and exit
	if (p->header.buffer) {
		bytes = min(p->header.count, _buf_cont_write(buf));
		memcpy(buf->writep, p->header.buffer + p->header.size - p->header.count, bytes);
		_buf_inc_writep(buf, bytes);
		p->header.count -= bytes;

		if (!p->header.count) {
			LOG_INFO("[%p] PCM header sent (%u bytes)", ctx, p->header.size);
			NFREE(p->header.buffer);
		}
		return;
	}

	// might be overloaded if there is some ICY or header to be sent
	bytes = min(bytes, _buf_cont_read(ctx->outputbuf));

	// check if ICY sending is active
	if (p->icy.interval && !p->icy.count) {
		if (!p->icy.remain) {
			int len_16 = 0;

			LOG_SDEBUG("[%p]: ICY checking", ctx);

			if (p->icy.updated) {
				// there is room for 1 extra byte at the beginning for length
#if ICY_ARTWORK
				len_16 = sprintf(p->icy.buffer, "NStreamTitle='%s - %s';StreamURL='%s';",
								 p->icy.artist, p->icy.title, p->icy.artwork) - 1;
				LOG_INFO("[%p]: ICY update\n\t%s\n\t%s\n\t%s", ctx, p->icy.artist, p->icy.title, p->icy.artwork);
#else
				len_16 = sprintf(p->icy.buffer, "NStreamTitle='%s - %s'", p->icy.artist, p->icy.title) - 1;
				LOG_INFO("[%p]: ICY update\n\t%s\n\t%s", ctx, p->icy.artist, p->icy.title);
#endif
				len_16 = (len_16 + 15) / 16;
			}

			p->icy.buffer[0] = len_16;
			p->icy.size = p->icy.count = len_16 * 16 + 1;
			p->icy.remain = p->icy.interval;
			p->icy.updated = false;
		} else {
			// do not go over icy interval
			bytes = min(bytes, p->icy.remain);
		}
	}

	// write ICY pending data and exit
	if (p->icy.count) {
		bytes = min(p->icy.count, _buf_cont_write(buf));
		memcpy(buf->writep, p->icy.buffer + p->icy.size - p->icy.count, bytes);
		_buf_inc_writep(buf, bytes);
		p->icy.count -= bytes;
		return;
	}

	// see how audio data shall be processed
	if (p->encode == ENCODE_THRU) {
		//	simple encoded audio, nothing to process, just forward outputbut
		bytes = min(bytes, _buf_cont_write(buf));
		memcpy(buf->writep, ctx->outputbuf->readp, bytes);
		_buf_inc_writep(buf, bytes);
		_buf_inc_readp(ctx->outputbuf, bytes);
		if (p->icy.interval) p->icy.remain -= bytes;
	} else if (p->encode == ENCODE_PCM) {
		// uncompressed audio to be processed
		u8_t *iptr, *optr, ibuf[BYTES_PER_FRAME], obuf[BYTES_PER_FRAME];
		u8_t sample_size = p->trunc16 ? 16 : p->sample_size;
		size_t in, out, frames, bytes_per_frame = (sample_size / 8) * p->channels;
		u32_t gain = 65536L;
		void *scratch;

		in = _buf_used(ctx->outputbuf);

		// need enough room in outputbuf and buf
		if (in < BYTES_PER_FRAME || out < bytes_per_frame) return;

		in = min(in, _buf_cont_read(ctx->outputbuf));

		// no enough cont'd place in input, just process one frame
		if (in < BYTES_PER_FRAME) {
			memcpy(ibuf, ctx->outputbuf->readp, in);
			memcpy(ibuf + in, ctx->outputbuf->buf, BYTES_PER_FRAME - in);
			iptr = ibuf;
			in = BYTES_PER_FRAME;
		} else iptr = ctx->outputbuf->readp;

		// space is less than cont_write when buffer is empty!
		out = min(_buf_space(buf), _buf_cont_write(buf));

		// not enough cont'd place in output, just process one frame
		if (out < bytes_per_frame) {
			optr = obuf;
			out = bytes_per_frame;
		} else optr = buf->writep;

		//FIXME: check that this works even with in < BYTES_PER_FRAME
		frames = fade_gain(&gain, ctx);
		frames = min(frames, in / BYTES_PER_FRAME);
		frames = min(frames, out / bytes_per_frame);

		// apply gain if any (before truncation or packing)

		//apply_gain(optr, iptr, out->replay_gain, bytes, out->sample_size, out->in_endian);

		/*
		// truncate or swap
		if (out->sample_size == 24 && out->trunc16) {
			truncate16(optr, iptr, bytes, out->in_endian, out->out_endian);
			obytes = (obytes * 2) / 3;
		} else if (out->sample_size == 24 && ctx->config.L24_format == L24_PACKED_LPCM) {
			lpcm_pack(optr, iptr, bytes, out->channels, out->in_endian);
		} else if (out->in_endian != out->out_endian) {
			swap(optr, iptr, bytes, out->sample_size);
		} else memcpy(optr, iptr, bytes);
		*/
		/* to be applied
			- gain
			- fade
			- truncation
			- 24 bits packing
		*/

		if (p->replay_gain) gain = ((u64_t) gain * p->replay_gain) >> 16;
		scratch = apply_gain((s32_t*) iptr, gain, frames);

		if (scratch) {
			scale_and_pack(optr, scratch, frames, p->channels, sample_size, p->out_endian);

			free(scratch);

			// take the data from temporary buffer if needed
			if (optr == obuf) {
				size_t out = _buf_cont_write(buf);
				memcpy(buf->writep, optr, out);
				memcpy(buf->buf, optr + out, bytes_per_frame - out);
			}

			_buf_inc_readp(ctx->outputbuf, frames * BYTES_PER_FRAME);
		}

		LOG_SDEBUG("[%p]: processed %u frames", ctx, frames);

		_buf_inc_writep(buf, frames * bytes_per_frame);
	}
}

/*---------------------------------------------------------------------------*/
void _output_boot(struct thread_ctx_s *ctx) {
	if (ctx->output.track_start != ctx->outputbuf->readp) {
		LOG_ERROR("[%p] not a track boundary %p:%p",
		ctx, ctx->output.track_start, ctx->outputbuf->readp);
	}
	ctx->output.track_start = NULL;
}

/*---------------------------------------------------------------------------*/
void output_init(unsigned outputbuf_size, struct thread_ctx_s *ctx) {
	LOG_DEBUG("[%p] init output media renderer", ctx);

	outputbuf_size = max(outputbuf_size, 256*1024);
	ctx->outputbuf = &ctx->__o_buf;
	buf_init(ctx->outputbuf, outputbuf_size);

	if (!ctx->outputbuf->buf) {
		LOG_ERROR("[%p]: unable to malloc output buffer", ctx);
		exit(0);
	}

	ctx->output.track_started = false;
	ctx->output.track_start = NULL;
	ctx->output_running = THREAD_KILLED;
	ctx->output.http = -1;
	ctx->output.icy.artist = ctx->output.icy.title = ctx->output.icy.artwork = NULL;

	ctx->render.index = -1;
}

/*---------------------------------------------------------------------------*/
void output_close(struct thread_ctx_s *ctx) {
	bool running;

	LOG_INFO("[%p] close media renderer", ctx);

	LOCK_O;
	running = ctx->output_running;
	ctx->output_running = THREAD_KILLED;
	UNLOCK_O;

	// still a race condition if a stream just ended a bit before ...
	if (running) pthread_join(ctx->output_thread, NULL);

	output_free_icy(ctx);
	NFREE(ctx->output.header.buffer);

	buf_destroy(ctx->outputbuf);
}

/*---------------------------------------------------------------------------*/
void output_free_icy(struct thread_ctx_s *ctx) {
	NFREE(ctx->output.icy.artist);
	NFREE(ctx->output.icy.title);
	NFREE(ctx->output.icy.artwork);
}


/*---------------------------------------------------------------------------*/
void _output_new_stream(struct thread_ctx_s *ctx) {
	size_t length;

	length = _output_pcm_header(ctx);
	if (ctx->config.stream_length > 0 && length) ctx->output.length = length;
	//FIXME if (length) ctx->output.length = length;
}


/*---------------------------------------------------------------------------*/
size_t _output_pcm_header(struct thread_ctx_s *ctx ) {
	struct outputstate *out = &ctx->output;
	u8_t sample_size;
	size_t length;

	// do we need to truncate 24 bits?
	sample_size = out->trunc16 ? 16 : out->sample_size;

	/*
	do not create a size (content-length) when we really don't know it but
	when we set a content-length (http_mode > 0) then at least the headers
	should be consistent
	*/
	if (!out->duration) {
		if (ctx->config.stream_length < 0) length = MAX_FILE_SIZE;
		else length = ctx->config.stream_length;
		length = (length / ((u64_t) out->sample_rate * out->channels * sample_size /8)) *
				 (u64_t) out->sample_rate * out->channels * sample_size / 8;
	} else length = (((u64_t) out->duration * out->sample_rate) / 1000) * out->channels * (sample_size / 8);

	switch (out->format) {
	case 'w': {
		struct wave_header_s *h = malloc(sizeof(struct wave_header_s));

		memcpy(h, &wave_header, sizeof(struct wave_header_s));
		little16(&h->channels, out->channels);
		little16(&h->bits_per_sample, sample_size);
		little32(&h->sample_rate, out->sample_rate);
		little32(&h->byte_rate, out->sample_rate * out->channels * (sample_size / 8));
		little16(&h->block_align, out->channels * (sample_size / 8));
		little32(&h->subchunk2_size, length);
		little32(&h->chunk_size, 36 + length);
		length += 36 + 8;
		out->header.buffer = (u8_t*) h;
		out->header.size = out->header.count = sizeof(struct wave_header_s);
	   }
	   break;
   case 'i': {
		struct aiff_header_s *h = malloc(sizeof(struct aiff_header_s));

		memcpy(h, &aiff_header, sizeof(struct aiff_header_s));
		big16(h->channels, out->channels);
		big16(h->sample_size, sample_size);
		big16(h->sample_rate_num, out->sample_rate);
		big32(&h->data_size, length + 8);
		big32(&h->chunk_size, (length+8+8) + (18+8) + 4);
		big32(&h->frames, length / (out->channels * (sample_size / 8)));
		length += (8+8) + (18+8) + 4 + 8;
		out->header.buffer = (u8_t*) h;
		out->header.size = out->header.count = 54; // can't count on structure due to alignment
		}
		break;
   case 'p':
   default:
		out->header.buffer = NULL;
		out->header.size = out->header.count = 0;
		length = 0;
		break;
	}

	return length;
}

/*---------------------------------------------------------------------------*/
void lpcm_pack(u8_t *dst, u8_t *src, size_t bytes, u8_t channels, int endian) {
	size_t i;

	// bytes are always a multiple of 12 (and 6 ...)
	// 3 bytes with packing required, 2 channels
	if (channels == 2) {
		if (endian) for (i = 0; i < bytes; i += 12) {
			// L0T,L0M & R0T,R0M
			*dst++ = src[2]; *dst++ = src[1];
			*dst++ = src[5]; *dst++ = src[4];
			// L1T,L1M & R1T,R1M
			*dst++ = src[8]; *dst++ = src[7];
			*dst++ = src[11]; *dst++ = src[10];
			// L0B, R0B, L1B, R1B
			*dst++ = src[0]; *dst++ = src[3]; *dst++ = src[6]; *dst++ = src[9];
			src += 12;
		} else for (i = 0; i < bytes; i += 12) {
			// L0T,L0M & R0T,R0M
			*dst++ = src[0]; *dst++ = src[1];
			*dst++ = src[3]; *dst++ = src[4];
			// L1T,L1M & R1T,R1M
			*dst++ = src[6]; *dst++ = src[7];
			*dst++ = src[9]; *dst++ = src[10];
			// L0B, R0B, L1B, R1B
			*dst++ = src[2]; *dst++ = src[5]; *dst++ = src[8]; *dst++ = src[11];
			src += 12;
		}
		// after that R0T,R0M,L0T,L0M,R1T,R1M,L1T,L1M,R0B,L0B,R1B,L1B
	}

	// 3 bytes with packing required, 1 channel
	if (channels == 1) {
		if (endian) for (i = 0; i < bytes; i += 6) {
			// C0T,C0M,C1,C1M
			*dst++ = src[2]; *dst++ = src[1];
			*dst++ = src[5]; *dst++ = src[4];
			// C0B, C1B
			*dst++ = src[0]; *dst++ = src[3];
			src += 6;
		} else for (i = 0; i < bytes; i += 6) {
			// C0T,C0M,C1,C1M
			*dst++ = src[0]; *dst++ = src[1];
			*dst++ = src[3]; *dst++ = src[4];
			// C0B, C1B
			*dst++ = src[2]; *dst++ = src[5];
			src += 6;
		}
		// after that C0T,C0M,C1T,C1M,C0B,C1B
	}
}

/*---------------------------------------------------------------------------*/
void scale_and_pack(void *dst, u32_t *src, size_t frames, u8_t channels, u8_t sample_size, int endian) {
	size_t count = frames * channels;

	if (channels == 2) {
		if (sample_size == 8) {
			u8_t *optr = (u8_t*) dst;
			if (endian) while (count--) *optr++ = (*src++ >> 24) ^ 0x80;
			else while (count--) *optr++ = *optr++ = *src++ >> 24;
		} else if (sample_size == 16) {
			u16_t *optr = (u16_t*) dst;
			if (endian) while (count--) *optr++ = *src++ >> 16;
			else while (count--) {
				*optr++ = ((*src >> 24) & 0xff) | ((*src >> 8) & 0xff00);
				src++;
			}
		} else if (sample_size == 24) {
			u8_t *optr = (u8_t*) dst;
			if (endian) while (count--) {
				*optr++ = *src >> 8;
				*optr++ = *src >> 16;
				*optr++ = *src++ >> 24;
			} else while (count--) {
				*optr++ = *src >> 24;
				*optr++ = *src >> 16;
				*optr++ = *src++ >> 8;
			}
		} else if (sample_size == 32) {
			u32_t *optr = (u32_t*) dst;
			if (endian) memcpy(dst, src, count * 4);
			else while (count--) {
				*optr++ = ((*src >> 24) & 0xff)     | ((*src >> 8)  & 0xff00) |
						  ((*src << 8)  & 0xff0000) | ((*src << 24) & 0xff000000);
				src++;
			}
		}

	} else if (channels == 1) {
		if (sample_size == 8) {
			u8_t *optr = (u8_t*) dst;
			if (endian) while (count--) {
				*optr++ = (*src >> 24) ^ 0x80;
				src += 2;
			} else while (count--) {
				*optr++ = *src >> 24;
				src += 2;
			}
		} else if (sample_size == 16) {
			u16_t *optr = (u16_t*) dst;
			if (endian) while (count--) {
				*optr++ = *src >> 16;
				src += 2;
			}
			else while (count--) {
				*optr++ = ((*src >> 24) & 0xff) | ((*src >> 8) & 0xff00);
				src += 2;
			}
		} else if (sample_size == 24) {
			u8_t *optr = (u8_t*) dst;
			if (endian) while (count--) {
				*optr++ = *src >> 8;
				*optr++ = *src >> 16;
				*optr++ = *src >> 24;
				src += 2;
			} else while (count--) {
				*optr++ = *src >> 24;
				*optr++ = *src >> 16;
				*optr++ = *src++ >> 8;
				src += 2;
			}
		} else if (sample_size == 32) {
			u32_t *optr = (u32_t*) dst;
			if (endian) while (count--) {
				*optr++ = *src;
				src += 2;
			} else while (count--) {
				*optr++ = ((*src >> 24) & 0xff)     | ((*src >> 8)  & 0xff00) |
						  ((*src << 8)  & 0xff0000) | ((*src << 24) & 0xff000000);
				src += 2;
			}
		}
	}
}

#define MAX_VAL32 0x7fffffffffffLL
/*---------------------------------------------------------------------------*/
void *apply_gain(s32_t *p, u32_t gain, size_t frames) {
	s32_t *buf = malloc(frames * BYTES_PER_FRAME);
	size_t count = frames * 2;
	s64_t sample;

	if (!buf) return NULL;

	if (gain == 65536) {
		memcpy(buf, p, frames * BYTES_PER_FRAME);
	} else {
		s32_t *iptr = buf;

		while (count--) {
			sample = *p++ * (s64_t) gain;
			if (sample > MAX_VAL32) sample = MAX_VAL32;
			else if (sample < -MAX_VAL32) sample = -MAX_VAL32;
			*iptr++ = sample >> 16;
		}
	}

	return buf;
}

/*---------------------------------------------------------------------------*/
void _checkfade(bool start, struct thread_ctx_s *ctx) {
	frames_t bytes;

	LOG_INFO("[%p]: fade mode: %u duration: %u %s", ctx, ctx->output.fade_mode, ctx->output.fade_secs, start ? "track-start" : "track-end");

	bytes = ctx->output.current_sample_rate * BYTES_PER_FRAME * ctx->output.fade_secs;
	if (ctx->output.fade_mode == FADE_INOUT) {
		bytes /= 2;
	}

	if (start && (ctx->output.fade_mode == FADE_IN || (ctx->output.fade_mode == FADE_INOUT && _buf_used(ctx->outputbuf) == 0))) {
		bytes = min(bytes, ctx->outputbuf->size - BYTES_PER_FRAME); // shorter than full buffer otherwise start and end align
		LOG_INFO("[%p]: fade IN: %u frames", ctx, bytes / BYTES_PER_FRAME);
		ctx->output.fade = FADE_DUE;
		ctx->output.fade_dir = FADE_UP;
		ctx->output.fade_start = ctx->outputbuf->writep;
		ctx->output.fade_end = ctx->output.fade_start + bytes;
		if (ctx->output.fade_end >= ctx->outputbuf->wrap) {
			ctx->output.fade_end -= ctx->outputbuf->size;
		}
	}

	if (!start && (ctx->output.fade_mode == FADE_OUT || ctx->output.fade_mode == FADE_INOUT)) {
		if (!ctx->output.fade) {
			bytes = min(_buf_used(ctx->outputbuf), bytes);
			LOG_INFO("[%p]: fade %s: %u frames", ctx, ctx->output.fade_mode == FADE_INOUT ? "IN-OUT" : "OUT", bytes / BYTES_PER_FRAME);
			ctx->output.fade = FADE_DUE;
			ctx->output.fade_dir = FADE_DOWN;
			ctx->output.fade_start = ctx->outputbuf->writep - bytes;
			if (ctx->output.fade_start < ctx->outputbuf->buf) {
				ctx->output.fade_start += ctx->outputbuf->size;
			}
			ctx->output.fade_end = ctx->outputbuf->writep;
		} else {
			// optionally, writep should be memorized for when continuous streaming is available
			ctx->output.fade = FADE_PENDING;
			LOG_INFO("[%p]: fade OUT delayed as IN still processing", ctx);
		}
	}
}

/*---------------------------------------------------------------------------*/
size_t fade_gain(u32_t *gain, struct thread_ctx_s *ctx) {
	size_t frames = _buf_cont_read(ctx->outputbuf) / BYTES_PER_FRAME;
	struct outputstate *out = &ctx->output;
	bool checkfade = false;

	if (!out->fade) return frames;

	if (out->fade == FADE_DUE) {
		if (out->fade_start == ctx->outputbuf->readp) {
			LOG_INFO("[%p]: fade start reached", ctx);
			out->fade = FADE_ACTIVE;
		} else if (out->fade_start > ctx->outputbuf->readp) {
			frames = min(frames, (out->fade_start - ctx->outputbuf->readp) / BYTES_PER_FRAME);
		}
	}

	if (out->fade == FADE_ACTIVE || out->fade == FADE_PENDING) {
		// find position within fade
		frames_t cur_f = ctx->outputbuf->readp >= out->fade_start ? (ctx->outputbuf->readp - out->fade_start) / BYTES_PER_FRAME :
			(ctx->outputbuf->readp + ctx->outputbuf->size - out->fade_start) / BYTES_PER_FRAME;
		frames_t dur_f = out->fade_end >= out->fade_start ? (out->fade_end - out->fade_start) / BYTES_PER_FRAME :
			(out->fade_end + ctx->outputbuf->size - out->fade_start) / BYTES_PER_FRAME;

		if (cur_f >= dur_f) {
			// so far this part is useless as we don't support a continuous flow
			if (out->fade_mode == FADE_INOUT && out->fade_dir == FADE_DOWN) {
				LOG_INFO("[%p]: fade down complete, starting fade up", ctx);
				out->fade_dir = FADE_UP;
				out->fade_start = ctx->outputbuf->readp;
				out->fade_end = ctx->outputbuf->readp + dur_f * BYTES_PER_FRAME;
				if (out->fade_end >= ctx->outputbuf->wrap) out->fade_end -= ctx->outputbuf->size;
				cur_f = 0;
			} else {
				LOG_INFO("[%p]: fade complete", ctx);
				if (out->fade == FADE_PENDING) checkfade = true;
				out->fade = FADE_INACTIVE;
			}
		}

		// if fade in progress set fade gain, ensure cont_frames reduced so we get to end of fade at start of chunk
		if (out->fade) {
			if (out->fade_end > ctx->outputbuf->readp) {
				frames = min(frames, (out->fade_end - ctx->outputbuf->readp) / BYTES_PER_FRAME);
			}
			if (out->fade_dir == FADE_UP || out->fade_dir == FADE_DOWN) {
				if (out->fade_dir == FADE_DOWN) cur_f = dur_f - cur_f;
				*gain = ((u64_t) cur_f << 16) / dur_f;
			}
		}

		// we know there is at least one frame, but it might be in alignment buffer
		if (!frames) frames = 1;

		// to force smooth scale up/down, don't do too many frames
		frames = min(frames, out->sample_rate / 10);

		// a fade-down was pending ac codec ended while fade-up was still going
		if (checkfade) _checkfade(false, ctx);
	}

	return frames;
}

/*---------------------------------------------------------------------------*/
static void little16(void *dst, u16_t src)
{
	u8_t *p = (u8_t*) dst;

	*p++ = (u8_t) (src);
	*p = (u8_t) (src >> 8);
}

/*---------------------------------------------------------------------------*/
static void little32(void *dst, u32_t src)
{
	u8_t *p = (u8_t*) dst;

	*p++ = (u8_t) (src);
	*p++ = (u8_t) (src >> 8);
	*p++ = (u8_t) (src >> 16);
	*p = (u8_t) (src >> 24);

}

/*---------------------------------------------------------------------------*/
static void big16(void *dst, u16_t src)
{
	u8_t *p = (u8_t*) dst;

	*p++ = (u8_t) (src >> 8);
	*p = (u8_t) (src);
}

/*---------------------------------------------------------------------------*/
static void big32(void *dst, u32_t src)
{
	u8_t *p = (u8_t*) dst;

	*p++ = (u8_t) (src >> 24);
	*p++ = (u8_t) (src >> 16);
	*p++ = (u8_t) (src >> 8);
	*p = (u8_t) (src);
}





