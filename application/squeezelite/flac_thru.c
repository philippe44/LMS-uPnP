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
#include "squeezeitf.h"

extern log_level decode_loglevel;
static log_level *loglevel = &decode_loglevel;

#define LOCK_S   mutex_lock(ctx->streambuf->mutex)
#define UNLOCK_S mutex_unlock(ctx->streambuf->mutex)
#define LOCK_O   mutex_lock(ctx->outputbuf->mutex)
#define UNLOCK_O mutex_unlock(ctx->outputbuf->mutex)
#define LOCK_O_direct   mutex_lock(ctx->outputbuf->mutex)
#define UNLOCK_O_direct mutex_unlock(ctx->outputbuf->mutex)
#define IF_DIRECT(x)    { x }
#define IF_PROCESS(x)

#define BYTE_1(n)	((u8_t) (n >> 24))
#define BYTE_2(n)	((u8_t) (n >> 16))
#define BYTE_3(n)	((u8_t) (n >> 8))
#define BYTE_4(n)	((u8_t) (n))

/*---------------------------------- FLAC ------------------------------------*/

#define FLAC_COMBO(F,N,S) ((u32_t) (((u32_t) (F) << 12) | ((u32_t) ((N-1) & 0x07) << 9) | ((u32_t) ((S-1) & 0x01f) << 4)))
#define QUAD_BYTE_H(n)	((u32_t) ((u64_t)(n) >> 32))
#define QUAD_BYTE_L(n)	((u32_t) (n))

u32_t	FLAC_CODED_RATES[] = { 0, 88200, 176400, 192000, 8000, 16000, 22050,
							   24000, 32000, 44100, 48000, 96000, 0, 0, 0, 0 };
u8_t	FLAC_CODED_CHANNELS[] = { 1, 2, 3, 4, 5, 6, 7, 8, 2, 2, 2, 0, 0, 0, 0, 0 };
u8_t	FLAC_CODED_SAMPLE_SIZE[] = { 0, 8, 12, 0, 16, 20, 24, 0 };

#if SL_LITTLE_ENDIAN
#define FLAC_TAG	(0xf8ff)	// byte order is reversed because it's treated as a u16
#define FLAC_GET_FRAME_TAG(n)	((u16_t) ((n) & 0xfcff))
#define FLAC_GET_BLOCK_STRATEGY(n) ((u16_t) ((n) & 0x0100))
#define FLAC_GET_RESERVED(n) ((u16_t) ((n) & 0x0200))
#else
#define FLAC_GET_FRAME_TAG(n)	((u16_t) ((n) & 0xfffc))
#define FLAC_TAG	(0xfff8)	// byte order is reversed because it's treated as a u16
#define FLAC_GET_BLOCK_STRATEGY(n) ((u16_t) ((n) & 0x0001))
#define FLAC_GET_RESERVED(n) ((u16_t) ((n) & 0x0002))
#endif

#define FLAC_GET_BLOCK_SIZE(n)	((u8_t) ((n) >> 4) & 0x0f)
#define FLAC_GET_FRAME_RATE(n) ((u8_t) ((n) & 0x0f))
#define FLAC_GET_FRAME_CHANNEL(n) ((u8_t) ((n) >> 4) & 0x0f)
#define FLAC_GET_FRAME_SAMPLE_SIZE(n) ((u8_t) ((n) >> 1) & 0x07)

#define FLAC_RECV_MIN	128
#define FLAC_MAX_SAMPLES 0xfffffffffLL

typedef struct flac_frame_s {
	u16_t	tag;
	u8_t    bsize_rate;
	u8_t	channels_sample_size;
	u8_t	body;
} flac_frame_t;

typedef struct flac_streaminfo_s {
		u8_t min_block_size[2];
		u8_t max_block_size[2];
		u8_t min_frame_size[3];
		u8_t max_frame_size[3];
		u8_t combo[4];
		u8_t sample_count[4];
		u8_t MD5[16];
} flac_streaminfo_t;

flac_streaminfo_t FLAC_STREAMINFO = {
		{ 0x00, 0x10 },
		{ 0xff, 0xff },
		{ 0x00, 0x00, 0x00 },
		{ 0x00, 0x00, 0x00 },
		{ BYTE_1(FLAC_COMBO(44100, 2, 16)),
		BYTE_2(FLAC_COMBO(44100, 2, 16)),
		BYTE_3(FLAC_COMBO(44100, 2, 16)),
		BYTE_4(FLAC_COMBO(44100, 2, 16)) | BYTE_1(QUAD_BYTE_H(0)) },
		{ BYTE_1(QUAD_BYTE_L(0)),
		BYTE_2(QUAD_BYTE_L(0)),
		BYTE_3(QUAD_BYTE_L(0)),
		BYTE_4(QUAD_BYTE_L(0)) },
		{ 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
		0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00 }
};

static u8_t flac_header[] = {
			'f', 'L', 'a', 'C',
			0x80,
			(u8_t) ((u32_t) sizeof(flac_streaminfo_t) >> 16),
			(u8_t) ((u32_t) sizeof(flac_streaminfo_t) >> 8),
			(u8_t) ((u32_t) sizeof(flac_streaminfo_t))
		};


struct flac {
	u32_t sample_rate;
};

static u16_t 	flac_block_size(u8_t block_size);
static bool 	create_streaminfo(sq_flac_header_t type, flac_frame_t *frame, flac_streaminfo_t *streaminfo, u32_t *rate, u32_t duration);

/*---------------------------------------------------------------------------*/
decode_state flac_decode(struct thread_ctx_s *ctx) {
	size_t in, out;
	struct flac *p = ctx->decode.handle;

	LOCK_S;
	LOCK_O_direct;

	in = min(_buf_used(ctx->streambuf), _buf_cont_read(ctx->streambuf));

	if (ctx->stream.state <= DISCONNECT && in == 0) {
		UNLOCK_O_direct;
		UNLOCK_S;
		return DECODE_COMPLETE;
	}

	// need to do that before header increments pointer
	if (ctx->decode.new_stream) {
		ctx->output.track_start = ctx->outputbuf->writep;
		ctx->decode.new_stream = false;

		LOG_INFO("[%p]: setting track_start", ctx);

		// the min in and out are enough to process a full header
		if (ctx->config.flac_header != FLAC_NO_HEADER) {
			flac_frame_t frame;
			flac_streaminfo_t streaminfo;
			size_t bytes;

			memcpy(&streaminfo, &FLAC_STREAMINFO, sizeof(flac_streaminfo_t));

			// acquire a full header, do not increment pointer
			bytes = min(in, sizeof(frame));
			memcpy(&frame, ctx->streambuf->readp, bytes);
			memcpy((u8_t*) &frame + bytes, ctx->streambuf->buf, sizeof(frame) - bytes);

			// starting with "flAC", we have a full header, no need to to anything
			if (strncmp((char*) &frame, "fLaC", 4) && create_streaminfo(ctx->config.flac_header, &frame, &streaminfo, &p->sample_rate, ctx->output.duration)) {
				bytes = min(sizeof(flac_header), _buf_cont_write(ctx->outputbuf));
				memcpy(ctx->outputbuf->writep, &flac_header, bytes);
				memcpy(ctx->outputbuf->buf, (u8_t*) &flac_header + bytes, sizeof(flac_header) - bytes);
				_buf_inc_writep(ctx->outputbuf, sizeof(flac_header));

				bytes = min(sizeof(flac_streaminfo_t), _buf_cont_write(ctx->outputbuf));
				memcpy(ctx->outputbuf->writep, &streaminfo, bytes);
				memcpy(ctx->outputbuf->buf, (u8_t*) &streaminfo + bytes, sizeof(flac_streaminfo_t) - bytes);
				_buf_inc_writep(ctx->outputbuf, sizeof(flac_streaminfo_t));

				LOG_INFO("[%p]: FLAC header added", ctx);
			}
		}
	}

	out = min(_buf_space(ctx->outputbuf), _buf_cont_write(ctx->outputbuf));
	out = min(in, out);

	memcpy(ctx->outputbuf->writep, ctx->streambuf->readp, out);

	_buf_inc_readp(ctx->streambuf, out);
	_buf_inc_writep(ctx->outputbuf, out);

	UNLOCK_O_direct;
	UNLOCK_S;

	return DECODE_RUNNING;
}

/*---------------------------------------------------------------------------*/
static void flac_open(u8_t sample_size, u32_t sample_rate, u8_t	channels, u8_t endianness, struct thread_ctx_s *ctx) {
	if (!ctx->decode.handle) ctx->decode.handle = malloc(sizeof(struct flac));
}

/*---------------------------------------------------------------------------*/
static void flac_close(struct thread_ctx_s *ctx) {
	if (ctx->decode.handle) free(ctx->decode.handle);
	ctx->decode.handle = NULL;
}

/*---------------------------------------------------------------------------*/
struct codec *register_flac_thru(void) {
	static struct codec ret = {
		'c',         // id = flac in thru-mode
		"flc", 		 // types
		4096,        // min read
		16*1024,     // min space
		flac_open,   // open
		flac_close,  // close
		flac_decode, // decode
		true,        // thru
	};

	LOG_INFO("using flac", NULL);
	return &ret;
}

void deregister_flac_thru(void) {
}

/*---------------------------------------------------------------------------*/
u32_t read_utf8_u32(u8_t *buf)
{
  u32_t v = 0;
  u32_t x;
  unsigned i;

  x = *buf++;

  if(!(x & 0x80)) { /* 0xxxxxxx */
	v = x;
	i = 0;
  } else if(x & 0xC0 && !(x & 0x20)) { /* 110xxxxx */
	v = x & 0x1F;
	i = 1;
  } else if(x & 0xE0 && !(x & 0x10)) { /* 1110xxxx */
	v = x & 0x0F;
	i = 2;
  } else if(x & 0xF0 && !(x & 0x08)) { /* 11110xxx */
	v = x & 0x07;
	i = 3;
  } else if(x & 0xF8 && !(x & 0x04)) { /* 111110xx */
	v = x & 0x03;
	i = 4;
  } else if(x & 0xFC && !(x & 0x02)) { /* 1111110x */
	v = x & 0x01;
	i = 5;
  } else {
	return 0xffffffff;
  }

  for( ; i; i--) {
	x = *buf++;
	if(!(x & 0x80) || (x & 0x40)) { /* 10xxxxxx */
	  return 0xffffffff;
	}
	v <<= 6;
	v |= (x & 0x3F);
  }

  return v;
}

/*---------------------------------------------------------------------------*/
bool create_streaminfo(sq_flac_header_t type, flac_frame_t *frame, flac_streaminfo_t *streaminfo, u32_t *rate, u32_t duration) {
	u8_t sample_size, channels;
	u16_t block_size = 0;
	u64_t sample_count = 0;
	int i;

	if (FLAC_GET_FRAME_TAG(frame->tag) != FLAC_TAG) {
		LOG_ERROR("flac no header and not a frame ...", NULL);
		return false;
	}

	if (FLAC_GET_RESERVED(frame->tag)) {
		LOG_ERROR("flac reserved bit set, cannot add header ...", NULL);
		return false;
	}

	*rate = FLAC_CODED_RATES[FLAC_GET_FRAME_RATE(frame->bsize_rate)];
	sample_size = FLAC_CODED_SAMPLE_SIZE[FLAC_GET_FRAME_SAMPLE_SIZE(frame->channels_sample_size)];
	channels = FLAC_CODED_CHANNELS[FLAC_GET_FRAME_CHANNEL(frame->channels_sample_size)];

	if (!FLAC_GET_BLOCK_STRATEGY(frame->tag)) {
		block_size = flac_block_size(FLAC_GET_BLOCK_SIZE(frame->bsize_rate));
		if (block_size) {
			streaminfo->min_block_size[0] = streaminfo->max_block_size[0] = BYTE_3(block_size);
			streaminfo->min_block_size[1] = streaminfo->max_block_size[1] = BYTE_4(block_size);
		}
		else {
			LOG_WARN("flac unhandled blocksize %d, using variable", frame->tag);
		}
	}

	if (type == FLAC_ADJUST_HEADER) sample_count = read_utf8_u32(&frame->body);
	if (sample_count == 0xffffffff || type == FLAC_MAX_HEADER) sample_count = FLAC_MAX_SAMPLES;
	else if (sample_count) sample_count = sample_count * block_size + (*rate * (u64_t) duration) / 1000;

	streaminfo->combo[0] = BYTE_1(FLAC_COMBO(*rate, channels, sample_size));
	streaminfo->combo[1] = BYTE_2(FLAC_COMBO(*rate, channels, sample_size));
	streaminfo->combo[2] = BYTE_3(FLAC_COMBO(*rate, channels, sample_size));
	streaminfo->combo[3] = BYTE_4(FLAC_COMBO(*rate, channels, sample_size) | BYTE_1(QUAD_BYTE_H(sample_count)));
	for (i = 3; i >= 0; i--) streaminfo->sample_count[3-i] = sample_count >> 8*i;

	LOG_INFO("flac header ch:%d, s:%d, r:%d, b:%d cnt:%lld", channels, sample_size, *rate, block_size, sample_count);
	if (!*rate || !sample_size || !channels) {
		LOG_ERROR("flac wrong header %d %d %d", *rate, channels, sample_size);
	}

	return true;
}

/*---------------------------------------------------------------------------*/
static u16_t flac_block_size(u8_t block_size)
{
	if (block_size == 0x01) return 192;
	if (block_size <= 0x05) return 576 * (1 << (block_size - 2));
	if (block_size == 0x06) return 0;
	if (block_size == 0x07) return 0;
	if (block_size <= 0xf) return 256 * (1 << (block_size - 8));
	return 0;
}







