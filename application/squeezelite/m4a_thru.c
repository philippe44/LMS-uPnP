/*
 *  Squeezelite - lightweight headless squeezebox emulator
 *
 *  (c) Adrian Smith 2012-2015, triode1@btinternet.com
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


#define WRAPBUF_LEN 2048

struct m4adts {
	// following used for mp4 only
	u32_t consume;
	u32_t pos;
	u32_t *frames;
	u32_t frame_size, frame_index;
	u8_t freq_index;
	u32_t audio_object_type;
	u8_t channel_config;
	unsigned trak, play;
};

extern log_level decode_loglevel;
static log_level *loglevel = &decode_loglevel;

#define LOCK_S   mutex_lock(ctx->streambuf->mutex)
#define UNLOCK_S mutex_unlock(ctx->streambuf->mutex)
#define LOCK_O   mutex_lock(ctx->outputbuf->mutex)
#define UNLOCK_O mutex_unlock(ctx->outputbuf->mutex)
#if PROCESS
#define LOCK_O_direct   if (ctx->decode.direct) mutex_lock(ctx->outputbuf->mutex)
#define UNLOCK_O_direct if (ctx->decode.direct) mutex_unlock(ctx->outputbuf->mutex)
#define IF_DIRECT(x)    if (ctx->decode.direct) { x }
#define IF_PROCESS(x)   if (!ctx->decode.direct) { x }
#else
#define LOCK_O_direct   mutex_lock(ctx->outputbuf->mutex)
#define UNLOCK_O_direct mutex_unlock(ctx->outputbuf->mutex)
#define IF_DIRECT(x)    { x }
#define IF_PROCESS(x)
#endif


// read mp4 header to extract config data
static int read_mp4_header(struct thread_ctx_s *ctx) {
	struct m4adts *a = ctx->decode.handle;
	size_t bytes = min(_buf_used(ctx->streambuf), _buf_cont_read(ctx->streambuf));
	char type[5];
	u32_t len;

	// assume that mP4 header will not wrap around streambuf!

	while (bytes >= 8) {
		// count trak to find the first playable one
		u32_t consume;

		len = unpackN((u32_t *)ctx->streambuf->readp);
		memcpy(type, ctx->streambuf->readp + 4, 4);
		type[4] = '\0';

		if (!strcmp(type, "moov")) {
			a->trak = 0;
			a->play = 0;
		}
		if (!strcmp(type, "trak")) {
			a->trak++;
		}

		// extract audio config from within esds
		if (!strcmp(type, "esds") && bytes > len) {
			u8_t *ptr = ctx->streambuf->readp + 12;
			u32_t audio_config;

			// handle extension tag if present
			if (*ptr++ != 0x03) return -1;
			if (*ptr == 0x80 || *ptr == 0x81 || *ptr == 0xfe) ptr += 3;
			ptr += 4;
			if (*ptr++ != 0x04) return -1;
			if (*ptr == 0x80 || *ptr == 0x81 || *ptr == 0xfe) ptr += 3;
			ptr += 14;
			if (*ptr++ != 0x05) return -1;
			if (*ptr == 0x80 || *ptr == 0x81 || *ptr == 0xfe) ptr += 3;
			ptr += 1;
			audio_config = unpackN((u32_t*)ptr);
			a->freq_index = (audio_config >> 23)& 0x0f;
			a->audio_object_type = (audio_config >> 27);
			a->channel_config = (audio_config >> 19)& 0x0f;
			LOG_DEBUG("[%p]: playable aac track: %u", ctx, a->trak);
			a->play = a->trak;
		}

		// stash sample to chunk info, assume it comes before stco
		if (!strcmp(type, "stsz") && bytes > len) {
			a->frame_size = unpackN((u32_t*) (ctx->streambuf->readp + 12));
			if (!a->frame_size) {
				u32_t entries = unpackN((u32_t*) (ctx->streambuf->readp + 16));
				u8_t *ptr = ctx->streambuf->readp + 20 + entries * 4;

				a->frames = malloc(entries * 4);
				a->frame_index = 0;
				LOG_INFO("[%p]: frame table of %d entries", ctx, entries);
				while (entries--) {
					ptr -= 4;
					a->frames[entries] = unpackN((u32_t*) ptr);
				}
			}
		}

		// found media data, advance to start of first chunk and return
		if (!strcmp(type, "mdat")) {
			_buf_inc_readp(ctx->streambuf, 8);
			a->pos += 8;
			bytes  -= 8;
			if (a->play) {
				LOG_DEBUG("[%p]: type: mdat len: %u pos: %u", ctx, len, a->pos);
				return 1;
			} else {
				// some file have mdat before moov, but we can't seek
				LOG_ERROR("[%p]: type: mdat len: %u, no playable track found", ctx, len);
				return -1;
			}
		}

		// default to consuming entire box
		consume = len;

		// read into these boxes so reduce consume
		if (!strcmp(type, "moov") || !strcmp(type, "trak") || !strcmp(type, "mdia") || !strcmp(type, "minf") || !strcmp(type, "stbl") ||
			!strcmp(type, "udta") || !strcmp(type, "ilst")) {
			consume = 8;
		}
		// special cases which mix mix data in the enclosing box which we want to read into
		if (!strcmp(type, "stsd")) consume = 16;
		if (!strcmp(type, "mp4a")) consume = 36;
		if (!strcmp(type, "meta")) consume = 12;

		// consume rest of box if it has been parsed (all in the buffer) or is not one we want to parse
		if (bytes >= consume) {
			LOG_DEBUG("[%p]: type: %s len: %u consume: %u", ctx, type, len, consume);
			_buf_inc_readp(ctx->streambuf, consume);
			a->pos += consume;
			bytes -= consume;
		} else if ( !(!strcmp(type, "esds") || !strcmp(type, "stsz") ) ) {
			LOG_DEBUG("[%p]: type: %s len: %u consume: %u - partial consume: %u", ctx, type, len, consume, bytes);
			_buf_inc_readp(ctx->streambuf, bytes);
			a->pos += bytes;
			a->consume = consume - bytes;
			break;
		} else if (len >= ctx->streambuf->size) {
			// can't process an atom larger than streambuf!
			LOG_ERROR("[%p]: atom %s too large for buffer %u %u", ctx, type, len, ctx->streambuf->size);
			return -1;
		} else {
			// make sure we have 'len' contiguous space in streambuf (large headers)
			_buf_unwrap(ctx->streambuf, len);
			break;
		}
	}

	return 0;
}

static decode_state m4adts_decode(struct thread_ctx_s *ctx) {
	struct m4adts *a = ctx->decode.handle;

	LOCK_S;

	if (a->consume) {
		u32_t consume = min(a->consume, _buf_cont_read(ctx->streambuf));
		LOG_DEBUG("[%p]: consume: %u of %u", ctx, consume, a->consume);
		_buf_inc_readp(ctx->streambuf, consume);
		a->pos += consume;
		a->consume -= consume;
		UNLOCK_S;
		return DECODE_RUNNING;
	}

	if (ctx->decode.new_stream) {
		int found = read_mp4_header(ctx);

		if (found == 1) {
			LOG_INFO("[%p]: setting track_start", ctx);
			LOCK_O;
			ctx->output.track_start = ctx->outputbuf->writep;
			ctx->decode.new_stream = false;
			UNLOCK_O;

		} else if (found == -1) {
			LOG_WARN("[%p]: error reading stream header", ctx);
			UNLOCK_S;
			return DECODE_ERROR;

		} else {
			// not finished header parsing come back next time
			UNLOCK_S;
			return DECODE_RUNNING;
		}
	}

	while (1) {
		size_t in, out;
		u32_t frame_size;
		u8_t *iptr;
		u8_t ADTSHeader[] = {0xFF,0xF1,0,0,0,0,0xFC};

		in = _buf_used(ctx->streambuf);
		if (ctx->stream.state <= DISCONNECT && !in) {
			UNLOCK_S;
			return DECODE_COMPLETE;
		}

		frame_size = a->frame_size ? a->frame_size : a->frames[a->frame_index];

		out = _buf_space(ctx->outputbuf);
		if (in < frame_size || out < frame_size + sizeof(ADTSHeader)){
			UNLOCK_S;
			return DECODE_RUNNING;
		}

		a->frame_index++;
		in = min(in, _buf_cont_read(ctx->streambuf));

		// simplify copy by handling wrap case
		if (in < frame_size) {
			u8_t *buffer = malloc(frame_size);
			memcpy(buffer, ctx->streambuf->readp, in);
			memcpy(buffer + in, ctx->streambuf->buf, frame_size - in);
			iptr = buffer;
		} else iptr = ctx->streambuf->readp;

		ADTSHeader[2] = (((a->audio_object_type & 0x03) - 1)  << 6) + (a->freq_index << 2) + (a->channel_config >> 2);
		ADTSHeader[3] = ((a->channel_config & 0x03) << 6) + ((frame_size + sizeof(ADTSHeader)) >> 11);
		ADTSHeader[4] = ((frame_size + sizeof(ADTSHeader)) & 0x7ff) >> 3;
		ADTSHeader[5] = (((frame_size + sizeof(ADTSHeader)) & 0x07) << 5) + 0x1f;

		LOCK_O_direct;

		// first copy header
		out = min(sizeof(ADTSHeader),_buf_cont_write(ctx->outputbuf));
		memcpy(ctx->outputbuf->writep, ADTSHeader, out);
		memcpy(ctx->outputbuf->buf, ADTSHeader + out, sizeof(ADTSHeader) - out);
		_buf_inc_writep(ctx->outputbuf, sizeof(ADTSHeader));

		// then copy data themselves
		out = min(frame_size, _buf_cont_write(ctx->outputbuf));
		memcpy(ctx->outputbuf->writep, iptr, out);
		memcpy(ctx->outputbuf->buf, iptr + out, frame_size - out);
		_buf_inc_writep(ctx->outputbuf, frame_size);

		if (in < frame_size ) free(iptr);
		_buf_inc_readp(ctx->streambuf, frame_size);

		UNLOCK_O_direct;
		UNLOCK_S;

		LOG_SDEBUG("[%p]: write %u bytes", ctx, frame_size + sizeof(ADTSHeader));
	}
}

static void m4adts_open(u8_t sample_size, u32_t sample_rate, u8_t channels, u8_t endianness, struct thread_ctx_s *ctx) {
	struct m4adts *a = ctx->decode.handle;

	if (!a) {
		a = ctx->decode.handle = malloc(sizeof(struct m4adts));
		if (!a) return;
		a->frames = NULL;
	}

	a->play = a->pos = a->consume = 0;
	if (a->frames) free(a->frames);
}

static void m4adts_close(struct thread_ctx_s *ctx) {
	struct m4adts *a = ctx->decode.handle;

	if (a->frames) free(a->frames);
	free(a);
	ctx->decode.handle = NULL;
}

struct codec *register_m4a_thru(void) {
	static struct codec ret = {
		'4',          // id
		"aac",        // types
		WRAPBUF_LEN,  // min read
		20480,        // min space
		m4adts_open,    // open
		m4adts_close,   // close
		m4adts_decode,  // decode
		true,			// thru
	};

	LOG_INFO("using mp4 to aac (ADTS)", NULL);
	return &ret;
}

void deregister_m4a_thru(void) {
}

