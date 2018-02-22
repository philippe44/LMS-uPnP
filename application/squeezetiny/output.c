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
unsigned _output_bytes(struct thread_ctx_s *ctx) {
	return ctx->output.thru ? _buf_used(ctx->outputbuf) : _buf_used(ctx->encodebuf);
}

/*---------------------------------------------------------------------------*/
unsigned _output_cont_bytes(struct thread_ctx_s *ctx) {
	if (ctx->output.icy.count) return ctx->output.icy.count;
	else {
		unsigned bytes = ctx->output.thru ? _buf_cont_read(ctx->outputbuf) :_buf_cont_read(ctx->encodebuf);
		if (ctx->output.icy.interval) return min(bytes, ctx->output.icy.remain);
		else return bytes;
	}
}

/*---------------------------------------------------------------------------*/
void* _output_readp(struct thread_ctx_s *ctx) {
	if (ctx->output.icy.count)
		return ctx->output.icy.buffer + ctx->output.icy.size - ctx->output.icy.count;
	else
		return ctx->output.thru ? ctx->outputbuf->readp : ctx->encodebuf->readp;
}

/*---------------------------------------------------------------------------*/
void _output_inc_readp(struct thread_ctx_s *ctx, unsigned by) {
	if (ctx->output.icy.count) ctx->output.icy.count -= by;
	else {
		if (ctx->output.thru) _buf_inc_readp(ctx->outputbuf, by);
		else _buf_inc_readp(ctx->encodebuf, by);

		// check if time to insert ICY metadata
		if (!ctx->output.icy.interval) return;

		ctx->output.icy.remain -= by;
		if (!ctx->output.icy.remain) {
			struct outputstate *out = &ctx->output;
			int len_16 = 0;

			if (ctx->output.icy.updated) {
				// there is room for 1 extra byte at the beginning for length
#if ICY_ARTWORK
				len_16 = sprintf(out->icy.buffer, "NStreamTitle='%s - %s';StreamURL='%s';",
								 out->icy.artist, out->icy.title, out->icy.artwork) - 1;

				LOG_INFO("[%p]: ICY update\n\t%s\n\t%s\n\t%s", ctx, out->icy.artist, out->icy.title, out->icy.artwork);
#else
				len_16 = sprintf(out->icy.buffer, "NStreamTitle='%s - %s'",
								 out->icy.artist, out->icy.title) - 1;

				LOG_INFO("[%p]: ICY update\n\t%s\n\t%s", ctx, out->icy.artist, out->icy.title);
#endif

				len_16 = (len_16 + 15) / 16;
			}

			out->icy.buffer[0] = len_16;
			out->icy.size = out->icy.count = len_16 * 16 + 1;
			out->icy.remain = out->icy.interval;
			out->icy.updated = false;
		}
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

	ctx->outputbuf = &ctx->__o_buf;
	buf_init(ctx->outputbuf, outputbuf_size);

	if (!ctx->outputbuf->buf) {
		LOG_ERROR("[%p]: unable to malloc output buffer", ctx);
		exit(0);
	}

	if (strcasecmp(ctx->config.encode, "thru")) {
		ctx->encodebuf = &ctx->__e_buf;
		buf_init(ctx->encodebuf, outputbuf_size);

		if (!ctx->encodebuf->buf) {
			LOG_ERROR("[%p]: unable to malloc output buffer", ctx);
			exit(0);
		}
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

	buf_destroy(ctx->outputbuf);
	if (ctx->encodebuf) buf_destroy(ctx->encodebuf);
}

/*---------------------------------------------------------------------------*/
void output_free_icy(struct thread_ctx_s *ctx) {
	NFREE(ctx->output.icy.artist);
	NFREE(ctx->output.icy.title);
	NFREE(ctx->output.icy.artwork);
}

/*---------------------------------------------------------------------------*/
void _checkfade(bool fade, struct thread_ctx_s *ctx) {
}

/*---------------------------------------------------------------------------*/
size_t output_pcm_header(void **header, size_t *hsize, struct thread_ctx_s *ctx ) {
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
		*header = h;
		*hsize = sizeof(struct wave_header_s);
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
		*header = h;
		// can't count on structure due to alignment
		*hsize = 54;
		}
		break;
   case 'p':
   default:
		*header = NULL;
		*hsize = 0;
		break;
	}

	return length;
}


/*---------------------------------------------------------------------------*/
void swap(u8_t *dst, u8_t *src, size_t bytes, u8_t size)
{
	switch (size) {
	case 8:
		memcpy(src, dst, bytes);
		break;
	case 16:
		bytes /= 2;
		src += 1;
		while (bytes--) {
			*dst++ = *src--;
			*dst++ = *src;
			src += 3;
		}
		break;
	case 24:
		bytes /= 3;
		src += 2;
		while (bytes--) {
			*dst++ = *src--;
			*dst++ = *src--;
			*dst++ = *src;
			src += 5;
		}
		break;
	 case 32:
		bytes /= 4;
		src += 3;
		while (bytes--) {
			*dst++ = *src--;
			*dst++ = *src--;
			*dst++ = *src--;
			*dst++ = *src;
			src += 7;
		}
		break;
	}
}


/*---------------------------------------------------------------------------*/
void truncate16(u8_t *dst, u8_t *src, size_t bytes, int in_endian, int out_endian)
{
	bool swap = (in_endian != out_endian);

	bytes /= 3;
	if (out_endian) src++;

	if (swap) {
		if (!out_endian) src += 2;
		while (bytes--) {
			*dst++ = *src--;
			*dst++ = *src;
			src += 4;
		}
	}
	else {
		while (bytes--) {
			*dst++ = *src++;
			*dst++ = *src++;
			src++;
		}
	}
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


#define MAX_VAL8  0x7fffffLL
#define MAX_VAL16 0x7fffffffLL
#define MAX_VAL24 0x7fffffffffLL
#define MAX_VAL32 0x7fffffffffffLL
// this probably does not work for little-endian CPU
/*---------------------------------------------------------------------------*/
void apply_gain(void *p, u32_t gain, size_t bytes, u8_t size, int endian) {
	size_t i = bytes / (size / 8);
	s64_t sample;

	if (!gain || gain == 65536) return;

#if !SL_LITTLE_ENDIAN
	endian = !endian;
#endif

	if (size == 8) {
		u8_t *buf = p;
		while (i--) {
			sample = *buf * (s64_t) gain;
			if (sample > MAX_VAL8) sample = MAX_VAL8;
			else if (sample < -MAX_VAL8) sample = -MAX_VAL8;
			*buf++ = sample >> 16;
		}
		return;
	}

	// big endian target on big endian CPU or the opposite
	if (endian) {

		if (size == 16) {
			s16_t *buf = p;
			while (i--) {
				sample = *buf * (s64_t) gain;
				if (sample > MAX_VAL16) sample = MAX_VAL16;
				else if (sample < -MAX_VAL16) sample = -MAX_VAL16;
				*buf++ = sample >> 16;
		   }
		   return;
		}

		if (size == 24) {
			u8_t *buf = p;
			while (i--) {
				// for 24 bits samples, first put the sample in the 3 upper bytes
				sample = (s32_t) ((((u32_t) *buf) << 8) | ((u32_t) *(buf+1) << 16) | ((u32_t) *(buf+2) << 24)) * (s64_t) gain;
				if (sample > MAX_VAL24) sample = MAX_VAL24;
				else if (sample < -MAX_VAL24) sample = -MAX_VAL24;
				sample >>= 16;
				*buf++ = sample;
				*buf++ = sample >> 8;
				*buf++ = sample >> 16;
			}
			return;
		}

		if (size == 32) {
			s32_t *buf = p;
			while (i--) {
				sample = *buf * (s64_t) gain;
				if (sample > MAX_VAL32) sample = MAX_VAL32;
				else if (sample < -MAX_VAL32) sample = -MAX_VAL32;
				*buf++ = sample >> 16;
			}
			return;
		}

	} else {

		if (size == 16) {
			s16_t *buf = p;
			while (i--) {
				sample = (s16_t) (((*buf << 8) & 0xff00) | ((*buf >> 8) & 0xff)) * (s64_t) gain;
				if (sample > MAX_VAL16) sample = MAX_VAL16;
				else if (sample < -MAX_VAL16) sample = -MAX_VAL16;
				sample >>= 16;
				*buf++ = ((sample >> 8) & 0xff) | ((sample << 8) & 0xff00);
			}
			return;
		}

		if (size == 24) {
			u8_t *buf = p;
			while (i--) {
				sample = (s32_t) ((((u32_t) *buf) << 24) | ((u32_t) *(buf+1) << 16) | ((u32_t) *(buf+2) << 8)) * (s64_t) gain;
				if (sample > MAX_VAL24) sample = MAX_VAL24;
				else if (sample < -MAX_VAL24) sample = -MAX_VAL24;
				sample >>= 16;
				*buf++ = sample >> 16;
				*buf++ = sample >> 8;
				*buf++ = sample;
		   }
		   return;
		}

		if (size == 32) {
			s32_t *buf = p;
			while (i--) {
				sample = *buf * (s64_t) gain;
				if (sample > MAX_VAL32) sample = MAX_VAL32;
				else if (sample < -MAX_VAL32) sample = -MAX_VAL32;
				*buf++ = sample >> 16;
			}
			return;
		}
	}
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




