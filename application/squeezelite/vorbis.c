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

#include "squeezelite.h"

//#define OGG_ONLY

// automatically select between floating point (preferred) and fixed point libraries:
// NOTE: works with Tremor version here: http://svn.xiph.org/trunk/Tremor, not vorbisidec.1.0.2 currently in ubuntu

// we take common definations from <vorbis/vorbisfile.h> even though we can use tremor at run time
// tremor's OggVorbis_File struct is normally smaller so this is ok, but padding added to malloc in case it is bigger
#define OV_EXCLUDE_STATIC_CALLBACKS

#ifndef OGG_ONLY
#ifdef TREMOR_ONLY
#include <ivorbisfile.h>
#else
#include <vorbis/vorbisfile.h>
#endif
#else
#include <ogg/ogg.h>
#ifdef TREMOR_ONLY
#include <ivorbiscodec.h>
#else
#include <vorbis/codec.h>
#endif
#endif

#ifdef OGG_ONLY
#define DEPTH 24
#endif

#if !LINKALL && !defined(TREMOR_ONLY)
static bool tremor = false;
#endif

#if !LINKALL
static struct {
	void* handle;
#ifdef OGG_ONLY
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
#endif
} go;

static struct {
	void* handle;
#ifndef OGG_ONLY
	// vorbis symbols to be dynamically loaded - from either vorbisfile or vorbisidec (tremor) version of library
	vorbis_info* (*ov_info)(OggVorbis_File* vf, int link);
	int (*ov_clear)(OggVorbis_File* vf);
	long (*ov_read)(OggVorbis_File* vf, char* buffer, int length, int bigendianp, int word, int sgned, int* bitstream);
	long (*ov_read_tremor)(OggVorbis_File* vf, char* buffer, int length, int* bitstream);
	int (*ov_open_callbacks)(void* datasource, OggVorbis_File* vf, const char* initial, long ibytes, ov_callbacks callbacks);
#else
	void (*vorbis_info_init)(vorbis_info* vi);
	void (*vorbis_info_clear)(vorbis_info* vi);
	void (*vorbis_comment_init)(vorbis_comment* vc);
	int (*vorbis_block_init)(vorbis_dsp_state* v, vorbis_block* vb);
	int (*vorbis_block_clear)(vorbis_block* vb);
	void (*vorbis_dsp_clear)(vorbis_dsp_state* v);
	int (*vorbis_synthesis_headerin)(vorbis_info* vi, vorbis_comment* vc, ogg_packet* op);
	int (*vorbis_synthesis_init)(vorbis_dsp_state* v, vorbis_info* vi);
	int (*vorbis_synthesis_read)(vorbis_dsp_state* v, int samples);
	int (*vorbis_synthesis_pcmout)(vorbis_dsp_state* v, float*** pcm);
	int (*vorbis_synthesis_blockin)(vorbis_dsp_state* v, vorbis_block* vb);
	int (*vorbis_synthesis)(vorbis_block* vb, ogg_packet* op);
#endif
} gv;
#endif

struct vorbis {
	bool opened, eos;
	int channels;
#ifndef OGG_ONLY
	OggVorbis_File* vf;
#else
	enum { OGG_SYNC, OGG_ID_HEADER, OGG_COMMENT_HEADER, OGG_SETUP_HEADER } status;
	struct {
		ogg_stream_state state;
		ogg_packet packet;
		ogg_sync_state sync;
		ogg_page page;
	};
	struct {
		vorbis_dsp_state decoder;
		vorbis_info info;
		vorbis_comment comment;
		vorbis_block block;
	};
	uint32_t overflow;
	int rate;
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
#define OV(h, fn, ...) (ov_ ## fn)(__VA_ARGS__)
#else
#define OV(h, fn, ...) (h)->ov_##fn(__VA_ARGS__)
#endif
#else
#if LINKALL
#define OV(h, fn, ...) (vorbis_ ## fn)(__VA_ARGS__)
#define OG(h, fn, ...) (ogg_ ## fn)(__VA_ARGS__)
#else
#define OV(h, fn, ...) (h)->vorbis_##fn(__VA_ARGS__)
#define OG(h, fn, ...) (h)->ogg_##fn(__VA_ARGS__)
#endif
#endif

#ifndef OGG_ONLY
// called with mutex locked within vorbis_decode to avoid locking O before S
static size_t _read_cb(void *ptr, size_t size, size_t nmemb, void *datasource) {
	size_t bytes;
	struct thread_ctx_s *ctx = datasource;

	while (1) {
		LOCK_S;
		bytes = min(_buf_used(ctx->streambuf), _buf_cont_read(ctx->streambuf));
		bytes = min(bytes, size * nmemb);
		if (bytes || ctx->stream.state <= DISCONNECT) break;

		UNLOCK_S;
		usleep(50 * 1000);
	}

	memcpy(ptr, ctx->streambuf->readp, bytes);
	_buf_inc_readp(ctx->streambuf, bytes);
	UNLOCK_S;

	return bytes / size;
}

// these are needed for older versions of tremor, later versions and libvorbis allow NULL to be used
static int _seek_cb(void* datasource, ogg_int64_t offset, int whence) { return -1; }
static int _close_cb(void* datasource) { return 0; }
static long _tell_cb(void* datasource) { return 0; }

int _ov_read(OggVorbis_File* vf, char* pcm, size_t bytes, int *out) {
// if LINKALL is set, then it's either TREMOR or VORBIS at compile-time
#ifndef TREMOR_ONLY
#if !LINKALL
	if (!tremor)
#endif
#if SL_LITTLE_ENDIAN
	return OV(&gv, read, vf, pcm, bytes, 0, 2, 1, out);
#else
	return OV(&gv, read, vf, pcm, bytes, 1, 2, 1, out);
#endif
#else
	return OV(&gv, read, vf, pcm, bytes, out);
#endif
}
#else 
static int get_ogg_packet(struct thread_ctx_s* ctx) {
	int status, packet = -1;
	struct vorbis* v = ctx->decode.handle;

	LOCK_S;
	size_t bytes = min(_buf_used(ctx->streambuf), _buf_cont_read(ctx->streambuf));

	while (!(status = OG(&go, stream_packetout, &v->state, &v->packet)) && bytes) {
		
		// if sync_pageout (or sync_pageseek) is not called here, sync buffers build up
		while (!(status = OG(&go, sync_pageout, &v->sync, &v->page)) && bytes) {
			size_t consumed = min(bytes, 4096);
			char* buffer = OG(&go, sync_buffer, &v->sync, consumed);
			memcpy(buffer, ctx->streambuf->readp, consumed);
			OG(&go, sync_wrote, &v->sync, consumed);

			_buf_inc_readp(ctx->streambuf, consumed);
			bytes -= consumed;
		}

		// if we have a new page, put it in
		if (status)	OG(&go, stream_pagein, &v->state, &v->page);
	}

	// we only return a negative value when there is nothing more to proceed
	if (status > 0) packet = status;
	else if (ctx->stream.state > DISCONNECT || _buf_used(ctx->streambuf)) packet = 0;

	UNLOCK_S;
	return packet;
}

static int read_vorbis_header(struct thread_ctx_s* ctx) {
	struct vorbis* v = ctx->decode.handle;
	int status = 0;
	bool fetch = true;

	LOCK_S;

	size_t bytes = min(_buf_used(ctx->streambuf), _buf_cont_read(ctx->streambuf));

	while (bytes && !status) {
		// first fetch a page if we need one
		if (fetch) {
			size_t consumed = min(bytes, 4096);
			char* buffer = OG(&go, sync_buffer, &v->sync, consumed);
			memcpy(buffer, ctx->streambuf->readp, consumed);
			OG(&go, sync_wrote, &v->sync, consumed);

			_buf_inc_readp(ctx->streambuf, consumed);
			bytes -= consumed;

			if (!OG(&go, sync_pageseek, &v->sync, &v->page)) continue;
		}

		switch (v->status) {
		case OGG_SYNC:
			v->status = OGG_ID_HEADER;
			OG(&go, stream_init, &v->state, OG(&go, page_serialno, &v->page));
			fetch = false;
			break;
		case OGG_ID_HEADER:
			status = OG(&go, stream_pagein, &v->state, &v->page);
			if (!OG(&go, stream_packetout, &v->state, &v->packet)) break;
		
			OV(&gv, info_init, &v->info);
			status = OV(&gv, synthesis_headerin, &v->info, &v->comment, &v->packet);

			if (status) {
				LOG_ERROR("[%p]: vorbis id header packet error %d", ctx, status);
				status = -1;
			} else {
				v->channels = v->info.channels;
				v->rate = v->info.rate;
				v->status = OGG_COMMENT_HEADER;

				// only fetch if no other packet already in (they should not)
				fetch = OG(&go, page_packets, &v->page) <= 1;
				if (!fetch) LOG_INFO("[%p]: id packet should terminate page", ctx);
				LOG_INFO("[%p]: id acquired", ctx);
			}
			break;
		case OGG_SETUP_HEADER:
			// header packets don't align with pages on Vorbis (contrary to Opus)
			if (fetch) OG(&go, stream_pagein, &v->state, &v->page);

			// finally build a codec if we have the packet
			status = OG(&go, stream_packetout, &v->state, &v->packet);
			if (status && ((status = OV(&gv, synthesis_headerin, &v->info, &v->comment, &v->packet)) ||
				(status = OV(&gv, synthesis_init, &v->decoder, &v->info)))) {
				LOG_ERROR("[%p]: vorbis setup header packet error %d", ctx, status);
				// no need to free comment, it's fake
				OV(&gv, info_clear, &v->info);
				status = -1;
			} else {
				OV(&gv, block_init, &v->decoder, &v->block);
				v->opened = true;
				LOG_INFO("[%p]: codec up and running", ctx);
				status = 1;
			}
			//@FIXME: can we have audio on that page as well?
			break;
		case OGG_COMMENT_HEADER: {
			// don't consume VorbisComment, just skip it
			int packets = OG(&go, page_packets, &v->page);
			if (packets) {
				v->status = OGG_SETUP_HEADER;
				OG(&go, stream_pagein, &v->state, &v->page);
				OG(&go, stream_packetout, &v->state, &v->packet);

				OV(&gv, comment_init, &v->comment);
				v->comment.vendor = "N/A";

				// because of lack of page alignment, we might have the setup page already fully in
				if (packets > 1) fetch = false;
				LOG_INFO("[%p]: comment skipped succesfully", ctx);
			}
			break;
		}
		default:
			break;
		}
	}

	UNLOCK_S;
	return status;
}

static inline int pcm_out(vorbis_dsp_state* decoder, void*** pcm) {
#ifndef TREMOR_ONLY                
#if !LINKALL
	if (!tremor) 
#endif
	return OV(&gv, synthesis_pcmout, decoder, (float***) pcm);
#else
	return OV(&gv, synthesis_pcmout, decoder, (int32_t***) pcm);
#endif
}

static inline int32_t clip15(int32_t x) {
	int ret = x;
	ret -= ((x <= 32767) - 1) & (x - 32767);
	ret -= ((x >= -32768) - 1) & (x + 32768);
	return ret;
}

static inline int32_t clip15f(float x) {
	int32_t ret = x * (1 << (DEPTH-1)) + 0.5f;
	if (ret > (1 << (DEPTH-1)) - 1) ret = (1 << (DEPTH-1)) - 1;
	else if (ret < -(1 << (DEPTH-1))) ret = -(1 << (DEPTH-1));
	return ret;

}
#endif

static decode_state vorbis_decode( struct thread_ctx_s *ctx) {
	struct vorbis *v = ctx->decode.handle;
	frames_t frames;
	int n = 0;
	u8_t *write_buf = NULL;

	if (ctx->decode.new_stream) {
#ifndef OGG_ONLY
		ov_callbacks cbs;
		int err;
		struct vorbis_info *info;

		cbs.read_func = _read_cb;

#ifndef TREMOR_ONLY
#if !LINKALL
		if (!tremor) {
#endif	
			cbs.seek_func = NULL; cbs.close_func = NULL; cbs.tell_func = NULL;
#if !LINKALL
		} else
#endif
		{
			cbs.seek_func = _seek_cb; cbs.close_func = _close_cb; cbs.tell_func = _tell_cb;
		}
#endif

		if ((err = OV(&gv, open_callbacks, ctx, v->vf, NULL, 0, cbs)) < 0) {
			LOG_WARN("[%p]: open_callbacks error: %d", ctx, err);
			return DECODE_COMPLETE;
		}

		info = OV(&gv, info, v->vf, -1);
		v->opened = true;
		v->channels = info->channels;

		LOCK_O;
		ctx->output.direct_sample_rate = info->rate;
#else
		int status = read_vorbis_header(ctx);

		if (status == 0) {
			return DECODE_RUNNING;
		}
		else if (status < 0) {
			LOG_WARN("[%p]: can't create codec", ctx);
			return DECODE_ERROR;
		}

		LOCK_O;
		ctx->output.direct_sample_rate = v->rate;
#endif
		ctx->output.channels = v->channels;
		ctx->output.sample_rate = decode_newstream(ctx->output.direct_sample_rate, ctx->output.supported_rates, ctx);
		ctx->output.sample_size = 16;
		ctx->output.track_start = ctx->outputbuf->writep;
		if (ctx->output.fade_mode) _checkfade(true, ctx);
		ctx->decode.new_stream = false;
		UNLOCK_O;

		if (v->channels > 2) {
			LOG_WARN("[%p]: too many channels: %d", ctx, v->channels);
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
		frames = ctx->process.max_in_frames;
		write_buf = ctx->process.inbuf;
	);

#ifndef OGG_ONLY
	int s;
	int bytes = frames * 2 * v->channels; // samples returned are 16 bits
	n = _ov_read(v->vf, (char*)write_buf, bytes, &s);
#else
	void** pcm = NULL;
	int packet = 0;

	if (v->overflow) {
		n = pcm_out(&v->decoder, &pcm);
		v->overflow = n - min(n, frames);
	} else if ((packet = get_ogg_packet(ctx)) > 0) {
		n = OV(&gv, synthesis, &v->block, &v->packet);
		if (n == 0) n = OV(&gv, synthesis_blockin, &v->decoder, &v->block);
		if (n == 0) n = pcm_out(&v->decoder, &pcm);
		v->overflow = n - min(n, frames);
	} else if (!packet && !OG(&go, page_eos, &v->page)) {
		UNLOCK_O_direct;
		return DECODE_RUNNING;
	};
#endif

	if (n > 0) {

#ifndef OGG_ONLY
		frames = n / 2 / v->channels;
		frames_t count = frames * v->channels;

		// work backward to unpack samples to 4 bytes per sample
		s32_t* optr = (s32_t*)write_buf + frames * 2;
		s16_t* iptr = (s16_t*)write_buf + count;

		if (v->channels == 2) {
			while (count--) {
				*--optr = *--iptr << 16;
			}
		}
		else if (v->channels == 1) {
			while (count--) {
				*--optr = *--iptr << 16;
				*--optr = *iptr << 16;
			}
		}
#else
		// if tremor linkall or compile, we have intergers
#if !LINKALL
		if (!tremor)
#endif
#ifndef TREMOR_ONLY
		{
			frames = min(n, frames);
			frames_t count = frames;
			s32_t* optr = (s32_t*)write_buf;

			if (v->channels == 2) {
				float* iptr_l = pcm[0];
				float* iptr_r = pcm[1];

				while (count--) {
					*optr++ = clip15f(*iptr_l++) << (32 - DEPTH);
					*optr++ = clip15f(*iptr_r++) << (32 - DEPTH);
				}
			}
			else if (v->channels == 1) {
				float* iptr = pcm[0];
				while (count--) {
					*optr++ = clip15f(*iptr) << (32 - DEPTH);
					*optr++ = clip15f(*iptr++) << (32 - DEPTH);
				}
			}
		}
#if !LINKALL
		else 
#endif
#else
		{
			if (v->channels == 2) {
				s32_t* iptr_l = (s32_t*)pcm[0];
				s32_t* iptr_r = (s32_t*)pcm[1];

				while (count--) {
					*optr++ = clip15(*iptr_l++);
					*optr++ = clip15(*iptr_r++);
				}
			}
			else if (v->channels == 1) {
				s32_t* iptr = (s32_t*)pcm[0];
				while (count--) {
					*optr++ = clip15(*iptr);
					*optr++ = clip15(*iptr++);
				}
			}
		}
#endif
		// return samples to vorbis decoder
		OV(&gv, synthesis_read, &v->decoder, frames);
#endif

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
	} else if (n == OV_HOLE) {

		// recoverable hole in stream, seen when skipping
		LOG_DEBUG("[%p]: hole in stream", ctx);
#endif
	} else {

		LOG_INFO("[%p]: ov_read error: %d", ctx, n);
		UNLOCK_O_direct;
		return DECODE_COMPLETE;
	}

	UNLOCK_O_direct;
	return DECODE_RUNNING;
}

static void vorbis_open(u8_t size, u32_t rate, u8_t chan, u8_t endianness, struct thread_ctx_s *ctx) {
	struct vorbis *v = ctx->decode.handle;

#ifndef OGG_ONLY
	if (!v) {
		v = ctx->decode.handle = calloc(1, sizeof(struct vorbis));
		v->opened = false;
		v->vf = malloc(sizeof(OggVorbis_File) + 128); // add some padding as struct size may be larger
		memset(v->vf, 0, sizeof(OggVorbis_File) + 128);
	} else if (v->opened) {
		OV(&gv, clear, v->vf);
		v->opened = false;
	}
	v->eos = true;
#else
	if (!v) {
		v = ctx->decode.handle = calloc(1, sizeof(struct vorbis));
		OG(&go, sync_init, &v->sync);
	} else {
		if (v->opened) {
			OV(&go, block_clear, &v->block);
			OV(&go, info_clear, &v->info);
			OV(&go, dsp_clear, &v->decoder);
		}
		OG(&go, stream_clear, &v->state);
		OG(&go, sync_clear, &v->sync);
	}
	OG(&go, stream_init, &v->state, -1);
	v->eos = false;
	v->opened = false;
	v->overflow = 0;
	v->status = OGG_SYNC;
#endif
}

static void vorbis_close(struct thread_ctx_s *ctx) {
	struct vorbis *v = ctx->decode.handle;

#ifndef OGG_ONLY
	if (v->opened) {
		OV(&gv, clear, v->vf);
	}
	free(v->vf);
#else
	if (v->opened) {
		OV(&go, block_clear, &v->block);
		OV(&go, info_clear, &v->info);
		OV(&go, dsp_clear, &v->decoder);
	}
	OG(&go, stream_clear, &v->state);
	OG(&go, sync_clear, &v->sync);
#endif
	free(v);
	ctx->decode.handle = NULL;
}

static bool load_vorbis(void) {
#if !LINKALL
	char *err;
	gv.handle = NULL;

#ifndef TREMOR_ONLY
	gv.handle = dlopen(LIBVORBIS, RTLD_NOW);
#endif
	if (!gv.handle) {
		gv.handle = dlopen(LIBTREMOR, RTLD_NOW);
		if (gv.handle) {
			tremor = true;
		}
		else {
			LOG_INFO("vorbis dlerror: %s", dlerror());
			return false;
		}
	}

#ifndef OGG_ONLY
	gv.ov_read = dlsym(handle, "ov_read");
	gv.ov_info = dlsym(gv.handle, "ov_info");
	gv.ov_clear = dlsym(gv.handle, "ov_clear");
	gv.ov_open_callbacks = dlsym(gv.handle, "ov_open_callbacks");
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

	gv.vorbis_info_init = dlsym(gv.handle, "vorbis_info_init");
	gv.vorbis_info_clear = dlsym(gv.handle, "vorbis_info_clear");
	gv.vorbis_comment_init = dlsym(gv.handle, "vorbis_comment_init");
	gv.vorbis_block_clear = dlsym(gv.handle, "vorbis_block_clear");
	gv.vorbis_synthesis_headerin = dlsym(gv.handle, "vorbis_synthesis_headerin");
	gv.vorbis_synthesis_init = dlsym(gv.handle, "vorbis_synthesis_init");
	gv.vorbis_synthesis_read = dlsym(gv.handle, "vorbis_synthesis_read");
	gv.vorbis_synthesis_pcmout = dlsym(gv.handle, "vorbis_synthesis_pcmout");
	gv.vorbis_synthesis_blockin = dlsym(gv.handle, "vorbis_synthesis_blockin");
	gv.vorbis_synthesis = dlsym(gv.handle, "vorbis_synthesis");
#endif

	if ((err = dlerror()) != NULL) {
		LOG_INFO("dlerror: %s", err);
		return false;
	}

	LOG_INFO("loaded %s", tremor ? LIBTREMOR : LIBVORBIS);
#endif

	return true;
}

struct codec *register_vorbis(void) {
	static struct codec ret = {
		'o',          // id
		"ogg",        // types
		8192,         // min read
		32*1024,      // min space
		vorbis_open,  // open
		vorbis_close, // close
		vorbis_decode,// decode
	};

	if (!load_vorbis()) {
		return NULL;
	}

	LOG_INFO("using vorbis to decode ogg", NULL);
	return &ret;
}

void deregister_vorbis(void) {
#if !LINKALL
	if (go.handle) dlclose(go.handle);
	if (gv.handle) dlclose(gv.handle);
#endif
}