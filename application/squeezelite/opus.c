 /*
 *  Squeezelite - lightweight headless squeezebox emulator
 *
 *  (c) Adrian Smith 2012-2015, triode1@btinternet.com
 *      Ralph Irving 2015-2017, ralph_irving@hotmail.com
 *      Philippe, philippe_44@outlook.com for raop/multi-instance modifications
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

// by default on decent OS, we don't need to raw Ogg depacketizer
//#define OGG_ONLY

#include "squeezelite.h"
#ifndef OGG_ONLY
#include <opusfile.h>
#else
#include <ogg/ogg.h>
#include <opus.h>
#endif

// opus maximum output frames is 120ms @ 48kHz
#define MAX_OPUS_FRAMES 5760

#if !LINKALL
#ifdef OGG_ONLY
static struct {
	void *handle;
	int (*ogg_stream_init)(ogg_stream_state* os, int serialno);
	int (*ogg_stream_clear)(ogg_stream_state* os);
	int (*ogg_stream_reset)(ogg_stream_state* os);
	int (*ogg_stream_eos)(ogg_stream_state* os);
	int (*ogg_stream_reset_serialno)(ogg_stream_state* os, int serialno);
	int (*ogg_sync_clear)(ogg_sync_state* oy);
	void (*ogg_packet_clear)(ogg_packet* op);
	char* (*ogg_sync_buffer)(ogg_sync_state* oy, long size);
	int (*ogg_sync_wrote)(ogg_sync_state* oy, long bytes);
	long (*ogg_sync_pageseek)(ogg_sync_state* oy, ogg_page* og);
	int (*ogg_sync_pageout)(ogg_sync_state* oy, ogg_page* og);
	int (*ogg_stream_pagein)(ogg_stream_state* os, ogg_page* og);
	int (*ogg_stream_packetout)(ogg_stream_state* os, ogg_packet* op);
	int (*ogg_page_packets)(const ogg_page* og);
} go;
#endif

static struct {
	void* handle;
#ifndef OGG_ONLY
	// opus symbols to be dynamically loaded
	void (*op_free)(OggOpusFile* _of);
	int  (*op_read)(OggOpusFile* _of, opus_int16* _pcm, int _buf_size, int* _li);
	const struct OpusHead* (*op_head)(OggOpusFile* _of, int _li);
	OggOpusFile* (*op_open_callbacks) (void* _source, OpusFileCallbacks* _cb, unsigned char* _initial_data, size_t _initial_bytes, int* _error);
#else
	OpusDecoder* (*opus_decoder_create)(opus_int32 Fs, int channels, int* error);
	int (*opus_decode)(OpusDecoder* st, const unsigned char* data, opus_int32 len, opus_int16* pcm, int frame_size, int decode_fec);
	void (*opus_decoder_destroy)(OpusDecoder* st);
#endif
} gu;
#endif

struct opus {
	int channels;
#ifndef OGG_ONLY
	struct OggOpusFile *of;
#else
	enum {OGG_SYNC, OGG_ID_HEADER, OGG_COMMENT_HEADER} status;
	ogg_stream_state state;
	ogg_packet packet;
	ogg_sync_state sync;
	ogg_page page;
	OpusDecoder* decoder;
	int rate, gain, pre_skip;
	size_t overframes;
	u8_t *overbuf;
#endif
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

#ifndef OGG_ONLY
#if LINKALL
#define OP(h, fn, ...) (op_ ## fn)(__VA_ARGS__)
#else
#define OP(h, fn, ...) (h)->op_ ## fn(__VA_ARGS__)
#endif
#else
#if LINKALL
#define OG(h, fn, ...) (ogg_ ## fn)(__VA_ARGS__)
#define OP(h, fn, ...) (opus_ ## fn)(__VA_ARGS__)
#else
#define OG(h, fn, ...) (h)->ogg_ ## fn(__VA_ARGS__)
#define OP(h, fn, ...) (h)->opus_ ## fn(__VA_ARGS__)
#endif
#endif

#ifndef OGG_ONLY
// called with mutex locked within vorbis_decode to avoid locking O before S
static int _read_cb(void *datasource, char *ptr, int size) {
	size_t bytes = 0;
	struct thread_ctx_s *ctx = datasource;

	while (1) {
		LOCK_S;
		bytes = min(_buf_used(ctx->streambuf), _buf_cont_read(ctx->streambuf));
		bytes = min(bytes, size);
		if (bytes || ctx->stream.state <= DISCONNECT) break;

		UNLOCK_S;
		usleep(50 * 1000);
	}

	memcpy(ptr, ctx->streambuf->readp, bytes);
	_buf_inc_readp(ctx->streambuf, bytes);
	UNLOCK_S;

	return bytes;
}
#else 
static unsigned parse_uint16(const unsigned char* _data) {
	return _data[0] | _data[1] << 8;
}

static int parse_int16(const unsigned char* _data) {
	return ((_data[0] | _data[1] << 8) ^ 0x8000) - 0x8000;
}

static u32_t parse_uint32(const unsigned char* _data) {
	return _data[0] | (u32_t)_data[1] << 8 |
		(u32_t)_data[2] << 16 | (u32_t)_data[3] << 24;
}

static int get_ogg_packet(struct thread_ctx_s* ctx) {
	int status, packet = -1;
	struct opus* u = ctx->decode.handle;

	LOCK_S;
	size_t bytes = min(_buf_used(ctx->streambuf), _buf_cont_read(ctx->streambuf));

	while (!(status = OG(&go, stream_packetout, &u->state, &u->packet)) && bytes) {

		// if sync_pageout (or sync_pageseek) is not called here, sync buffers build up
		while (!(status = OG(&go, sync_pageout, &u->sync, &u->page)) && bytes) {
			size_t consumed = min(bytes, 4096);
			char* buffer = OG(&go, sync_buffer, &u->sync, consumed);
			memcpy(buffer, ctx->streambuf->readp, consumed);
			OG(&go, sync_wrote, &u->sync, consumed);

			_buf_inc_readp(ctx->streambuf, consumed);
			bytes -= consumed;
		}

		// if we have a new page, put it in
		if (status)	OG(&go, stream_pagein, &u->state, &u->page);
	}

	// we only return a negative value when there is nothing more to proceed
	if (status > 0) packet = status;
	else if (ctx->stream.state > DISCONNECT || _buf_used(ctx->streambuf)) packet = 0;

	UNLOCK_S;
	return packet;
}

static int read_opus_header(struct thread_ctx_s* ctx) {
	struct opus* u = ctx->decode.handle;
	int status = 0;
	bool fetch = true;

	LOCK_S;

	size_t bytes = min(_buf_used(ctx->streambuf), _buf_cont_read(ctx->streambuf));

	while (bytes && !status) {
		// first fetch a page if we need one
		if (fetch) {
			size_t consumed = min(bytes, 4096);
			char* buffer = OG(&go, sync_buffer, &u->sync, consumed);
			memcpy(buffer, ctx->streambuf->readp, consumed);
			OG(&go, sync_wrote, &u->sync, consumed);

			_buf_inc_readp(ctx->streambuf, consumed);
			bytes -= consumed;

			if (!OG(&go, sync_pageseek, &u->sync, &u->page)) continue;
		}

		switch (u->status) {
		case OGG_SYNC:
			u->status = OGG_ID_HEADER;
			OG(&go, stream_init, &u->state, OG(&go, page_serialno, &u->page));
			fetch = false;
			break;
		case OGG_ID_HEADER:
			status = OG(&go, stream_pagein, &u->state, &u->page);
			if (OG(&go, stream_packetout, &u->state, &u->packet)) {
				if (u->packet.bytes < 19 || memcmp(u->packet.packet, "OpusHead", 8)) {
					LOG_ERROR("[%p]: wrong opus header packet (size:%u)", ctx, u->packet.bytes);
					status = -100;
					break;
				}
				u->status = OGG_COMMENT_HEADER;
				u->channels = u->packet.packet[9];
				u->pre_skip = parse_uint16(u->packet.packet + 10);
				u->rate = parse_uint32(u->packet.packet + 12);
				u->gain = parse_int16(u->packet.packet + 16);
				u->decoder = OP(&gu, decoder_create, 48000, u->channels, &status);
				if (!u->decoder || status != OPUS_OK) {
					LOG_ERROR("[%p]: can't create decoder %d (channels:%u)", ctx, status, u->channels);
				}
			}
			fetch = true;
			break;
		case OGG_COMMENT_HEADER:
			// skip packets to consume VorbisComment. Headers are mandatory and align on pages
			status = OG(&go, page_packets, &u->page);
			break;
		default:
			break;
		}
	}

	UNLOCK_S;
	return status;
}
#endif

static decode_state opus_decompress(struct thread_ctx_s *ctx) {
	struct opus *u = ctx->decode.handle;
	frames_t frames;
	u8_t *write_buf = NULL;

	if (ctx->decode.new_stream) {
#ifndef OGG_ONLY
		struct OpusFileCallbacks cbs;
		const struct OpusHead *info;
		int err;

		cbs.read = (op_read_func) _read_cb;
		cbs.seek = NULL; cbs.tell = NULL; cbs.close = NULL;

		if ((u->of = OP(&gu, open_callbacks, ctx, &cbs, NULL, 0, &err)) == NULL) {
			LOG_WARN("open_callbacks error: %d", err);
			return DECODE_COMPLETE;
		}

		info = OP(&gu, head, u->of, -1);
		u->channels = info->channel_count;
#else
		int status = read_opus_header(ctx);

		if (status == 0) {
			return DECODE_RUNNING;
		} else if (status < 0) {
			LOG_WARN("[%p]: can't create codec", ctx);
			return DECODE_ERROR;
		}
#endif
		LOCK_O;
		ctx->output.channels = u->channels;
		ctx->output.direct_sample_rate = 48000;
		ctx->output.sample_rate = decode_newstream(48000, ctx->output.supported_rates, ctx);
		ctx->output.sample_size = 16;
		ctx->output.track_start = ctx->outputbuf->writep;
		if (ctx->output.fade_mode) _checkfade(true, ctx);
		ctx->decode.new_stream = false;
		UNLOCK_O;

		if (u->channels > 2) {
			LOG_WARN("[%p]: too many channels: %d", ctx, u->channels);
			return DECODE_ERROR;
		}

		LOG_INFO("[%p]: setting track_start", ctx);
	}

	LOCK_O_direct;
	IF_DIRECT(
		frames = min(_buf_space(ctx->outputbuf), _buf_cont_write(ctx->outputbuf)) / BYTES_PER_FRAME;
		write_buf = ctx->outputbuf->writep;
	);
	IF_PROCESS(
		write_buf = ctx->process.inbuf;
		frames = ctx->process.max_in_frames;
	);

#ifndef OGG_ONLY
	// write the decoded frames into outputbuf then unpack them (they are 16 bits)
	int n = OP(&gu, read, u->of, (opus_int16*) write_buf, frames * u->channels, NULL);
#else
	int packet, n = 0;

	if (u->overframes) {
		/* use potential leftover from previous encoding. We know that it will fit this time
		 * as min_space is >=MAX_OPUS_FRAMES and we start from the beginning of the buffer */
		memcpy(write_buf, u->overbuf, u->overframes * BYTES_PER_FRAME);
		n = u->overframes;
		u->overframes = 0;
	} else if ((packet = get_ogg_packet(ctx)) > 0) {
		if (frames < MAX_OPUS_FRAMES) {
			// don't have enough contiguous space, use the overflow buffer (still works if n < 0)
			n = OP(&gu, decode, u->decoder, u->packet.packet, u->packet.bytes, (opus_int16*) u->overbuf, MAX_OPUS_FRAMES, 0);
			if (n >= 0) {
				u->overframes = n - min(n, frames);
				n = min(n, frames);
				memcpy(write_buf, u->overbuf, n * BYTES_PER_FRAME);
				memmove(u->overbuf, u->overbuf + n, u->overframes);
			}
		} else {
			/* we just do one packet at a time, although we could loop on packets but that means locking the 
			 * outputbuf and streambuf for maybe a long time while we process it all, so don't do that */
			n = OP(&gu, decode, u->decoder, u->packet.packet, u->packet.bytes, (opus_int16*) write_buf, frames, 0);
		}
	} else if (!packet && !OG(&go, page_eos, &u->page)) {
		UNLOCK_O_direct;
		return DECODE_RUNNING;
	};
#endif
	if (n > 0) {
		frames_t count;
		s16_t *iptr;
		s32_t *optr;

		frames = n;
		count = frames * u->channels;

		// work backward to unpack samples to 4 bytes per sample
		iptr = (s16_t *)write_buf + count;
		optr = (s32_t *)write_buf + frames * 2;

		if (u->channels == 2) {
			while (count--) {
				*--optr = *--iptr << 16;
			}
		} else if (u->channels == 1) {
			while (count--) {
				*--optr = *--iptr << 16;
				*--optr = *iptr << 16;
			}
		}

		ctx->decode.frames += frames;

		IF_DIRECT(
			_buf_inc_writep(ctx->outputbuf, frames * BYTES_PER_FRAME);
		);
		IF_PROCESS(
			ctx->process.in_frames = frames;
		);

		LOG_SDEBUG("[%p]: wrote %u frames", ctx, frames);

	} else if (n == 0) {

#ifndef OGG_ONLY
		if (ctx->stream.state <= DISCONNECT) {
#else
		if (packet < 0) {
#endif
			LOG_INFO("[%p]: end of decode", ctx);
			UNLOCK_O_direct;
			return DECODE_COMPLETE;
		} else {
			LOG_INFO("[%p]: no frame decoded", ctx);
		}

#ifndef OGG_ONLY
	} else if (n == OP_HOLE) {
		// recoverable hole in stream, seen when skipping
		LOG_DEBUG("[%p]: hole in stream", ctx);
#endif
	} else {
		LOG_INFO("[%p]: op_read / opus_decoder error: %d", ctx, n);
		UNLOCK_O_direct;
		return DECODE_COMPLETE;
	}

	UNLOCK_O_direct;
	return DECODE_RUNNING;
}

static void opus_open(u8_t size, u32_t rate, u8_t chan, u8_t endianness, struct thread_ctx_s *ctx) {
	struct opus *u = ctx->decode.handle;

#ifndef OGG_ONLY
	if (!u) {
		u = ctx->decode.handle = malloc(sizeof(struct opus));
		u->of = NULL;
	} else if (u->of) {
		OP(&gu, free, u->of);
		u->of = NULL;
	}
#else
	if (!u) {
		u = ctx->decode.handle = calloc(1, sizeof(struct opus));
		u->overbuf = malloc(MAX_OPUS_FRAMES * BYTES_PER_FRAME);
		OG(&go, sync_init, &u->sync);
		OG(&go, stream_init, &u->state, -1);
	} else {
		if (u->decoder) OP(&gu, decoder_destroy, u->decoder);
		u->decoder = NULL;
		OG(&go, stream_clear, &u->state);
		OG(&go, sync_clear, &u->sync);
	}
	u->status = OGG_SYNC;
	u->overframes = 0;
#endif
}

static void opus_close(struct thread_ctx_s *ctx) {
	struct opus *u = ctx->decode.handle;

#ifndef OGG_ONLY
	if (u->of) {
		OP(&gu, free, u->of);
	}
#else
	if (u->decoder) OP(&gu, decoder_destroy, u->decoder);
	free(u->overbuf);
	OG(&go, stream_clear, &u->state);
	OG(&go, sync_clear, &u->sync);
#endif
	free(u);
	ctx->decode.handle = NULL;
}

static bool load_opus(void) {
#if !LINKALL
	char *err;

	gu.handle = dlopen(LIBOPUS, RTLD_NOW);
	if (!!gu.handle) {
		LOG_INFO("opus dlerror: %s", dlerror());
		return false;
	}

#ifndef OGG_ONLY
	gu.op_free = dlsym(gu.handle, "op_free");
	gu.op_read = dlsym(gu.handle, "op_read");
	gu.op_head = dlsym(gu.handle, "op_head");
	gu.op_open_callbacks = dlsym(gu.handle, "op_open_callbacks");
#else
	go.handle = dlopen(LIBOGG, RTLD_NOW);
	if (!go.handle) {
		LOG_INFO("ogg dlerror: %s", dlerror());
		dlclose(gu.handle);
		return false;
	}

	go.ogg_stream_clear = dlsym(go.handle, "ogg_stream_clear");
	go.ogg_stream_reset = dlsym(go.handle, "ogg_stream_reset");
	go.ogg_stream_eos = dlsym(go.handle, "ogg_stream_eos");
	go.ogg_stream_reset_serialno = dlsym(go.handle, "ogg_stream_reset_serialno");
	go.ogg_sync_clear = dlsym(go.handle, "ogg_sync_clear");
	go.ogg_packet_clear = dlsym(go.handle, "ogg_packet_clear");
	go.ogg_sync_buffer = dlsym(go.handle, "ogg_sync_buffer");
	go.ogg_sync_wrote = dlsym(go.handle, "ogg_sync_wrote");
	go.ogg_sync_pageseek = dlsym(go.handle, "ogg_sync_pageseek");
	go.ogg_sync_pageout = dlsym(go.handle, "ogg_sync_pageout");
	go.ogg_stream_pagein = dlsym(go.handle, "ogg_stream_pagein");
	go.ogg_stream_packetout = dlsym(go.handle, "ogg_stream_packetout");
	go.ogg_page_packets = dlsym(go.handle, "ogg_page_packets");

	gu.opus_decoder_create = dlsym(gu.handle, "opus_decoder_create");
	gu.opus_decoder_destroy = dlsym(gu.handle, "opus_decoder_destroy");
	gu.opus_decode = dlsym(gu.handle, "opus_decode");
#endif

	if ((err = dlerror()) != NULL) {
		LOG_INFO("dlerror: %s", err);
		return false;
	}

	LOG_INFO("loaded "LIBOPUS, NULL);
#endif

	return true;
}

struct codec *register_opus(void) {
	static struct codec ret = {
		'u',          // id
		"ops",        // types
		8192,         // min read
		MAX_OPUS_FRAMES*BYTES_PER_FRAME*2,    // min space
		opus_open,    // open
		opus_close,   // close
		opus_decompress,  // decode
	};

	if (!load_opus()) {
		return NULL;
	}

	LOG_INFO("using opus to decode ops", NULL);
	return &ret;
}

void deregister_opus(void) {
#if !LINKALL
	if (go.handle) dlclose(go.handle);
	if (gu.handle) dlclose(gu.handle);
#endif
}