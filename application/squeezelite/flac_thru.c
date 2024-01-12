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

#if WIN
#define lzcnt(x) __lzcnt(x)
#else
#define lzcnt(x) __builtin_clz(x)
#endif

// see https://xiph.org/flac/format.html

static const u32_t	FLAC_RATES[] = { 0, 88200, 176400, 192000, 8000, 16000, 22050,
							   24000, 32000, 44100, 48000, 96000, 0x0c, 0xd, 0x0e, 0x0f };
static const u8_t	FLAC_CHANNELS[] = { 1, 2, 3, 4, 5, 6, 7, 8, 2, 2, 2, 0, 0, 0, 0, 0 };
static const u8_t	FLAC_SAMPLE_SIZE[] = { 0, 8, 12, 0, 16, 20, 24, 0 };

static u8_t crc8[256];
static u16_t crc16[256];

#define FLAC_MAX_SAMPLES 0xfffffffffLL

typedef struct {
	u16_t	tag;
	u8_t    bsize_rate;
	u8_t	channels_sample_size;
	u8_t	body[12];	// enough to include CRC-8
} frame_t;

typedef struct {
	u16_t min_block_size;
	u16_t max_block_size;
	u8_t min_frame_size[3];
	u8_t max_frame_size[3];
	u8_t combo[4];
	u8_t sample_count[4];
	u8_t MD5[16];
} streaminfo_t;

static u8_t flac_header[] = {
	'f', 'L', 'a', 'C',
	0x80,
	sizeof(streaminfo_t) >> 16,
	sizeof(streaminfo_t) >> 8,
	sizeof(streaminfo_t)
};

typedef struct {
	enum { OFF, SYNC, CRC16 } state;
	bool ignore;
	u16_t crc16;
	u64_t offset, position;
	struct settings_s {
		u32_t sample_rate;
		u8_t channels, sample_size;
		size_t block_size;
		bool fixed_block;
	} settings;
	u8_t queue[2];
} flac_t;

#ifdef VORBIS_COMMENT
static u8_t vorbis_comment[] = {
	0x84,0x00,0x00,0x28,0x20,0x00,0x00,0x00,0x72,0x65,0x66,0x65,0x72,0x65,0x6E,
	0x63,0x65,0x20,0x6C,0x69,0x62,0x46,0x4C,0x41,0x43,0x20,0x31,0x2E,0x34,0x2E,
	0x32,0x20,0x32,0x30,0x32,0x32,0x31,0x30,0x32,0x32,0x00,0x00,0x00,0x00
};
#endif

static streaminfo_t *create_streaminfo(flac_t *flac, sq_flac_header_t type, frame_t *frame, u32_t duration);
static size_t read_frame(frame_t* frame, struct settings_s* settings, u64_t *position, size_t *extra);
static size_t create_frame(flac_t* flac, frame_t* frame, size_t* out);
static void bit_slicer(u8_t* data, u64_t* item, size_t bits, size_t* bitpos);
static u8_t read_utf8(u8_t* buf, u64_t* v);
static u8_t write_utf8(u64_t v, u8_t* buf);
static u16_t flac_block_size(u8_t block_size);
static inline u16_t calc_crc16(u8_t* data, size_t n, u16_t crc);
static inline u8_t calc_crc8(u8_t* data, size_t n, u8_t crc);

/*---------------------------------------------------------------------------*/
static decode_state flac_decode(struct thread_ctx_s *ctx) {
	flac_t *p = ctx->decode.handle;

	LOCK_S;

	if (ctx->stream.state <= DISCONNECT && _buf_used(ctx->streambuf) == 0) {
		if (p->state == CRC16) {
			p->crc16 = htons(p->crc16);
			_buf_write(ctx->outputbuf, &p->crc16, 2);
		}
		UNLOCK_S;
		return DECODE_COMPLETE;
	}

	// need to do that before header increments pointer
	if (ctx->decode.new_stream) {

		// starting with "flAC" or headerless, no need to to anything
		if (ctx->config.flac_header == FLAC_NO_HEADER || !memcmp(ctx->streambuf->readp, "fLaC", 4)) {
			ctx->output.track_start = ctx->outputbuf->writep;
			ctx->decode.new_stream = false;
			LOG_INFO("[%p]: flac thru no header needed", ctx);
		} else {
			// the min in and out are enough to process a full header (and stream has been flushed)
			size_t n, avail = _buf_cont_read(ctx->streambuf);
			u16_t tag = ntohs(0xfff8);

			// search for a flac tab (0xfff8)
			for (n = 0; n < (avail - 2) && *(u16_t*)(ctx->streambuf->readp + n) != tag; n++);
			_buf_inc_readp(ctx->streambuf, n);

			// bail out if not found or not enough data for a full frame
			if (n == avail - 2 || _buf_used(ctx->streambuf) < sizeof(frame_t)) {
				UNLOCK_S;
				return DECODE_RUNNING;
			}

			frame_t frame;

			// acquire a full frame header
			n = min(sizeof(frame), _buf_cont_read(ctx->streambuf));
			memcpy(&frame, ctx->streambuf->readp, n);
			memcpy((u8_t*)&frame + n, ctx->streambuf->buf, sizeof(frame) - n);

			streaminfo_t* streaminfo = create_streaminfo(p, ctx->config.flac_header, &frame, ctx->output.duration);
			if (streaminfo) {
				LOCK_O;
				_buf_write(ctx->outputbuf, flac_header, sizeof(flac_header));
				_buf_write(ctx->outputbuf, streaminfo, sizeof(streaminfo_t));
#ifdef VORBIS_COMMENT
				_buf_write(ctx->outputbuf, vorbis_comment, sizeof(vorbis_comment));
#endif
				ctx->output.track_start = ctx->outputbuf->writep;
				UNLOCK_O;
				free(streaminfo);
				ctx->decode.new_stream = false;
				p->state = SYNC;
				LOG_INFO("[%p]: flac thru header added", ctx);
			} else {
				// not a frame header, consume at least one byte
				_buf_inc_readp(ctx->streambuf, 1);
				UNLOCK_S;
				return DECODE_RUNNING;
			}
		}
	}

	LOCK_O;

	if (p->state == SYNC) {
		frame_t frame;
		size_t in, out;

		// read a header but don't increment streambuf as we don't know yet if we need the data
		in = min(sizeof(frame), _buf_cont_read(ctx->streambuf));
		memcpy(&frame, ctx->streambuf->readp, in);
		memcpy((u8_t*) &frame + in, ctx->streambuf->buf, sizeof(frame) - in);

		bool first_frame = p->position == p->offset;

		// if this is a frame, consume it and create the replacement
		if ((in = create_frame(p, &frame, &out)) != 0) {
			if (!first_frame) {
				p->crc16 = htons(p->crc16);
				_buf_write(ctx->outputbuf, &p->crc16, 2);
			}

			// start a new frame
			p->crc16 = calc_crc16((u8_t*)&frame, out, 0);
			_buf_write(ctx->outputbuf, &frame, out);

			// remove frame and replenish queue
			_buf_inc_readp(ctx->streambuf, in);
			_buf_read(&p->queue, ctx->streambuf, 2);
		} else {
			p->ignore = true;
		}

		// we move back to crc16 no matter what
		p->state = CRC16;
	} 

	size_t consumed = 0, avail = min(_buf_used(ctx->streambuf), _buf_cont_read(ctx->streambuf));
	avail = min(avail, _buf_space(ctx->outputbuf));
	avail = min(avail, _buf_cont_write(ctx->outputbuf));

	// in CRC16 state, copy as much as we can
	for (u8_t* iptr = ctx->streambuf->readp, *optr = ctx->outputbuf->writep; p->state == CRC16 && consumed < avail; consumed++) {
		// if we've reached a potential SYNC word, we want to restart fresh
		if (!p->ignore && *iptr == 0xff && (consumed == avail - 1 || (*(iptr + 1) & 0xf8) == 0xf8)) {
			p->state = SYNC;
			break;
		}
			
		// write data & update crc16 as we now know that it is not part of crc16 itself
		p->crc16 = calc_crc16(p->queue, 1, p->crc16);
		*optr++ = p->queue[0];

		p->queue[0] = p->queue[1];
		p->queue[1] = *iptr++;
		p->ignore = false;
	}

	// no need to re-process flac headers
	if (p->state == OFF) {
		memcpy(ctx->outputbuf->writep, ctx->streambuf->readp, avail);
		consumed = avail;
	}

	_buf_inc_readp(ctx->streambuf, consumed);
	_buf_inc_writep(ctx->outputbuf, consumed);

	UNLOCK_O;
	UNLOCK_S;

	return DECODE_RUNNING;
}

/*---------------------------------------------------------------------------*/
static void flac_open(u8_t sample_size, u32_t sample_rate, u8_t	channels, u8_t endianness, struct thread_ctx_s *ctx) {
	if (!ctx->decode.handle) ctx->decode.handle = malloc(sizeof(flac_t));
	memset(ctx->decode.handle, 0, sizeof(flac_t));
	((flac_t*)(ctx->decode.handle))->state = OFF;
}

/*---------------------------------------------------------------------------*/
static void flac_close(struct thread_ctx_s *ctx) {
	if (ctx->decode.handle) free(ctx->decode.handle);
	ctx->decode.handle = NULL;
}

/*---------------------------------------------------------------------------*/
struct codec *register_flac_thru(void) {
	static struct codec ret = {
		'F',         // id = flac in thru-mode
		"flc", 		 // types
		4096,        // min read
		16*1024,     // min space
		flac_open,   // open
		flac_close,  // close
		flac_decode, // decode
		true,        // thru
	};

	// x^8 + x^2 + x^1 + x^0 = 0x07
	for (int i = 0; i < 256; i++) {
		crc8[i] = i;
		for (int j = 0; j < 8; j++) crc8[i] = (crc8[i] & 0x80) ? (crc8[i] << 1) ^ 0x07 : (crc8[i] << 1);
	}

	// x^16 + x^15 + x^2 + x^0 = 0x8005
	for (int i = 0; i < 256; i++) {
		crc16[i] = i << 8;
		for (int j = 0; j < 8; j++) crc16[i] = (crc16[i] & 0x8000) ? (crc16[i] << 1) ^ 0x8005 : (crc16[i] << 1);
	}

	LOG_INFO("using flac thru", NULL);
	return &ret;
}

void deregister_flac_thru(void) {
}

/*---------------------------------------------------------------------------*/
static streaminfo_t *create_streaminfo(flac_t *flac, sq_flac_header_t type, frame_t *frame, u32_t duration) {
	if (!read_frame(frame, &flac->settings, &flac->position, NULL)) return 0;

	streaminfo_t* streaminfo = calloc(sizeof(streaminfo_t), 1);
	u64_t total_samples = 0;
	flac->offset = flac->position;

	if (flac->settings.fixed_block) {
		streaminfo->min_block_size = streaminfo->max_block_size = htons(flac->settings.block_size);
	} else {
		streaminfo->min_block_size = htons(0x10);
		streaminfo->max_block_size = 0xffff;
		LOG_INFO("flac is variable blocksize, header insertion might not work");
	}

	if (type == FLAC_MAX_HEADER) total_samples = FLAC_MAX_SAMPLES;
	else if (type == FLAC_ADJUST_HEADER) total_samples = (flac->settings.sample_rate * (u64_t) duration) / 1000;

	u32_t combo = (flac->settings.sample_rate << 12) | 
			       ((((u32_t)flac->settings.channels - 1) & 0x07) << 9) | 
		           ((((u32_t)flac->settings.sample_size - 1) & 0x01f) << 4) |
				   ((total_samples >> 32) & 0x0f);

	for (size_t i = 0; i < 4; i++) streaminfo->combo[i] = combo >> (3 - i) * 8;
	for (size_t i = 0; i < 4; i++) streaminfo->sample_count[i] = total_samples >> (3 - i) * 8;

	LOG_INFO("flac header ch:%d, s:%d, r:%d, b:%d, samples:%" PRId64, flac->settings.channels, flac->settings.sample_size, 
															 flac->settings.sample_rate, flac->settings.block_size, 
															 total_samples);
	
	return streaminfo;
}

/*---------------------------------------------------------------------------*/
static size_t read_frame(frame_t* frame, struct settings_s *settings, u64_t *position, size_t *extra) {
	if ((ntohs(frame->tag) & 0xfff8) != 0xfff8) return 0;

	size_t bitpos = 15;
	u64_t bits;
	u8_t* data = (u8_t*)frame;

	bit_slicer(data, &bits, 1, &bitpos);
	settings->fixed_block = bits == 0;

	bit_slicer(data, &bits, 4, &bitpos);
	settings->block_size = flac_block_size(bits);

	bit_slicer(data, &bits, 4, &bitpos);
	settings->sample_rate = FLAC_RATES[bits];

	bit_slicer(data, &bits, 4, &bitpos);
	settings->channels = FLAC_CHANNELS[bits];

	bit_slicer(data, &bits, 3, &bitpos);
	settings->sample_size = FLAC_SAMPLE_SIZE[bits];

	bitpos++;
	bitpos += read_utf8(data + bitpos / 8, position) * 8;
	if (extra) *extra = bitpos;

	// is blocksize at the end of header
	if (settings->block_size == 0x06) {
		bit_slicer(data, &bits, 8, &bitpos);
		settings->block_size = bits + 1;
	} else if (settings->block_size == 0x07) {
		bit_slicer(data, &bits, 16, &bitpos);
		settings->block_size = bits + 1;
	}

	// is sample rate at the end of header
	if (settings->sample_rate == 0x0c) {
		bit_slicer(data, &bits, 8, &bitpos);
		settings->sample_rate = bits * 1000;
	} else if (settings->sample_rate == 0x0d) {
		bit_slicer(data, &bits, 16, &bitpos);
		settings->sample_rate = bits;
	} else if (settings->sample_rate == 0x0e) {
		bit_slicer(data, &bits, 16, &bitpos);
		settings->sample_rate = bits * 10;
	}
	if (extra) *extra = (bitpos - *extra) / 8;

	u8_t crc = calc_crc8(data, bitpos / 8, 0);

	// get the actual crc
	bit_slicer(data, &bits, 8, &bitpos);

	return bits == crc ? bitpos / 8 : 0;
}

/*---------------------------------------------------------------------------*/
static size_t create_frame(flac_t* flac, frame_t* frame, size_t* out) {
	struct settings_s settings;
	u64_t position;
	size_t extra, n = read_frame(frame, &settings, &position, &extra);

	// make sure that this is a valid frame
	if (n && flac->position == position && 
		(!flac->settings.fixed_block || flac->settings.block_size >= settings.block_size) &&
		flac->settings.sample_rate == settings.sample_rate &&
		flac->settings.channels == settings.channels &&
		flac->settings.sample_size == settings.sample_size) {

		flac->position = flac->settings.fixed_block ? position + 1 : position + settings.block_size;
		position -= flac->offset;

		// calculate position in utf8 and snap variable length items
		u8_t* p = (u8_t*)frame;
		*out = offsetof(frame_t, body) + write_utf8(position, frame->body);
		memmove(p + *out, p + n - extra - 1, extra);
		*out += extra;

		// re-calculate crc and copy optional variable length items
		p[*out] = calc_crc8(p, *out, 0);
		
		*out += 1;
		return n;
	}

	return 0;
}

/*---------------------------------------------------------------------------*/
static void bit_slicer(u8_t* data, u64_t* item, size_t bits, size_t* bitpos) {
	u8_t* p = data + *bitpos / 8;
	size_t offset = *bitpos % 8;
	*item = 0;


	for (u8_t c = *p << offset, n = bits; n;) {
		*item |= c >> 7;
		if (++offset % 8 == 0) c = *p++;
		else c <<= 1;
		if (--n) *item <<= 1;
	}

	*bitpos += bits;
	*item &= (1 << bits) - 1;
}

/*---------------------------------------------------------------------------*/
static u8_t read_utf8(u8_t* buf, u64_t* v) {
	u8_t len = lzcnt(~*buf & 0xff) - 24;

	// creates a bit mask that works for len = 0,2,3...
	*v = *buf++ & ((1 << (8 - len)) - 1);

	if (len) for (int i = --len; i; i--) {
		*v <<= 6;
		*v |= *buf++ & 0x3f;
	}

	return len + 1;
}

/*---------------------------------------------------------------------------*/
static u8_t write_utf8(u64_t v, u8_t* buf) {
	if (v < 0x80) {
		*buf = v;
		return 1;
	}

	// calculate number of bytes-1 required
	u8_t len = v >> 32 ? 64 - lzcnt(v >> 32) : 32 - lzcnt(v);
	len = (len - 2) / 5;

	*buf++ = ~((1 << (7 - len)) - 1) | (v >> len * 6);

	for (int i = len - 1; i >= 0; i--) {
		*buf++ = 0x80 | ((v >> i * 6) & 0x3f);
	}

	return len + 1;
}

/*---------------------------------------------------------------------------*/
static u16_t flac_block_size(u8_t block_size)
{
	if (block_size == 0x01) return 192;
	if (block_size <= 0x05) return 576 * (1 << (block_size - 2));
	if (block_size == 0x06 || block_size == 0x07) return block_size;
	if (block_size <= 0xf) return 256 * (1 << (block_size - 8));
	return 0;
}

/*---------------------------------------------------------------------------*/
static inline u16_t calc_crc16(u8_t* data, size_t n, u16_t crc) {
	while (n--) crc = (crc << 8) ^ crc16[*data++ ^ (crc >> 8)];
	return crc;
}

/*---------------------------------------------------------------------------*/
static inline u8_t calc_crc8(u8_t* data, size_t n, u8_t crc) {
	while (n--) crc = crc8[*data++ ^ crc];
	return crc;
}

