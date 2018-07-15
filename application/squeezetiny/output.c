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
#include "FLAC/stream_encoder.h"

extern log_level	output_loglevel;
static log_level 	*loglevel = &output_loglevel;

#define LOCK_O 	 mutex_lock(ctx->outputbuf->mutex)
#define UNLOCK_O mutex_unlock(ctx->outputbuf->mutex)

static void 	lpcm_pack(u8_t *dst, u8_t *src, size_t bytes, u8_t channels, int endian);
static void		apply_gain(s32_t *p, u32_t gain, u8_t shift, size_t frames);
size_t 			fade_gain(u32_t *gain, struct thread_ctx_s *ctx);
static void 	scale_and_pack(void *dst, u32_t *src, size_t frames, u8_t channels,
							   u8_t sample_size, int endian);

static FLAC__StreamEncoderWriteStatus flac_write_callback(const FLAC__StreamEncoder *encoder, const FLAC__byte buffer[], size_t bytes, unsigned samples, unsigned current_frame, void *client_data);

static struct {
#if !LINKALL
	// FLAC symbols to be dynamically loaded
	FLAC__StreamEncoder* (*FLAC__stream_encoder_new)(void);
	FLAC__bool (*FLAC__stream_encoder_finish)(FLAC__StreamEncoder *encoder);
	void (*FLAC__stream_encoder_delete)(FLAC__StreamEncoder *encoder);
	FLAC__bool (*FLAC__stream_encoder_set_verify)(FLAC__StreamEncoder*encoder, FLAC__bool value);
	FLAC__bool (*FLAC__stream_encoder_set_compression_level)(FLAC__StreamEncoder *encoder, unsigned value);
	FLAC__bool (*FLAC__stream_encoder_set_channels)(FLAC__StreamEncoder *encoder, unsigned value);
	FLAC__bool (*FLAC__stream_encoder_set_bits_per_sample)(FLAC__StreamEncoder *encoder, unsigned value);
	FLAC__bool (*FLAC__stream_encoder_set_sample_rate)(FLAC__StreamEncoder *encoder, unsigned value);
	FLAC__bool (*FLAC__stream_encoder_set_blocksize)(FLAC__StreamEncoder *encoder, unsigned value);
	FLAC__bool (*FLAC__stream_encoder_set_streamable_subset)(FLAC__StreamEncoder *encoder, FLAC__bool value);
	FLAC__StreamEncoderInitStatus (*FLAC__stream_encoder_init_stream)(FLAC__StreamEncoder *encoder, FLAC__StreamEncoderWriteCallback write_callback, FLAC__StreamEncoderSeekCallback seek_callback, FLAC__StreamEncoderTellCallback tell_callback, FLAC__StreamEncoderMetadataCallback metadata_callback, void *client_data);
	FLAC__bool (*FLAC__stream_encoder_process_interleaved)(FLAC__StreamEncoder *encoder, const FLAC__int32 buffer[], unsigned samples);
#endif
} f;

#define FLAC_BLOCK_SIZE 1024
#define FLAC_MIN_SPACE	(16*1024)

#if LINKALL
#define FLAC(h, fn, ...) (FLAC__ ## fn)(__VA_ARGS__)
#define FLAC_A(h, a)     (FLAC__ ## a)
#else
#define FLAC(h, fn, ...) (h).FLAC__##fn(__VA_ARGS__)
#define FLAC_A(h, a)     (h).FLAC__ ## a
#endif


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
bool _output_fill(struct buffer *buf, struct thread_ctx_s *ctx) {
	size_t bytes = _buf_space(buf);
	struct outputstate *p;

	/*
	The outputbuf contains FRAMES of 32 bits samples and 2 channels. The buf is
	what will be sent to client, so it can be anything amongst header, ICY data,
	passthru audio, uncompressed audio in 8,16,24,32 and 1 or 2 channels or
	re-compressed audio.
	The gain, truncation, L24 pack, swap, fade-in/out is applied *from* the
	outputbuf and copied *to* this buf so it really has no specific alignement
	but for simplicity we won't process anything until it has free space for the
	largest block of audio which is BYTES_PER_FRAME
	Input buffer (outputbuf) is a multiple of BYTES_PER_FRAME and the writep is
	always aligned to a multiple of BYTES_PER_FRAME when starting a new track
	Output buffer (buf) cannot have an alignement due to additon of header  for
	wav and aif files
	*/
	if (bytes < BYTES_PER_FRAME) return true;

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
		return true;
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
		return true;
	}

	// see how audio data shall be processed
	if (p->encode.mode == ENCODE_THRU) {
		//	simple encoded audio, nothing to process, just forward outputbut
		bytes = min(bytes, _buf_cont_write(buf));
		memcpy(buf->writep, ctx->outputbuf->readp, bytes);
		_buf_inc_writep(buf, bytes);
		_buf_inc_readp(ctx->outputbuf, bytes);
		if (p->icy.interval) p->icy.remain -= bytes;
	} else {
		// uncompressed audio to be processed
		size_t in, out, frames, bytes_per_frame = (p->encode.sample_size / 8) * p->encode.channels;
		u8_t *optr, obuf[BYTES_PER_FRAME];
		u32_t gain = 65536L;

		// outputbuf is processed by BYTES_PER_FRAMES multiples => aligns fine
		in = min(_buf_used(ctx->outputbuf), _buf_cont_read(ctx->outputbuf));

		// no bytes may mean end of audio data, let caller decide
		if (!in) {
			if (p->encode.mode != ENCODE_FLAC) return false;
			// make sure that FLAC finishing call will have enough space
			if (_buf_space(buf) < FLAC_MIN_SPACE) return true;
			else return false;
		}

		frames = fade_gain(&gain, ctx);
		if (p->replay_gain) gain = ((u64_t) gain * p->replay_gain) >> 16;

		/*
		} else if (out->sample_size == 24 && ctx->config.L24_format == L24_PACKED_LPCM) {
			lpcm_pack(optr, iptr, bytes, out->encode.channels, out->in_endian);
		*/

		if (p->encode.mode == ENCODE_PCM) {
			// buf cannot count on alignement because of headers (wav /aif)
			out = min(_buf_space(buf), _buf_cont_write(buf));

			// not enough cont'd place in output, just process one frame
			if (out < bytes_per_frame) {
				optr = obuf;
				out = bytes_per_frame;
			} else optr = buf->writep;

			frames = min(frames, in / BYTES_PER_FRAME);
			frames = min(frames, out / bytes_per_frame);

			apply_gain((s32_t*) ctx->outputbuf->readp, gain, 0, frames);
			scale_and_pack(optr, (u32_t*) ctx->outputbuf->readp, frames,
						   p->encode.channels, p->encode.sample_size, p->out_endian);

			// take the data from temporary buffer if needed
			if (optr == obuf) {
				size_t out = _buf_cont_write(buf);
				memcpy(buf->writep, optr, out);
				memcpy(buf->buf, optr + out, bytes_per_frame - out);
			}

			_buf_inc_writep(buf, frames * bytes_per_frame);
		} else if (p->encode.mode == ENCODE_FLAC) {
			out = _buf_space(buf);
			if (out < FLAC_MIN_SPACE) return true;

			frames = min(frames, in / BYTES_PER_FRAME);
			frames = min(frames, out / bytes_per_frame);

			apply_gain((s32_t*) ctx->outputbuf->readp, gain, 32 - p->sample_size, frames);
			FLAC(f, stream_encoder_process_interleaved, p->encode.codec, ctx->outputbuf->readp, frames);
		}

		_buf_inc_readp(ctx->outputbuf, frames * BYTES_PER_FRAME);

		LOG_SDEBUG("[%p]: processed %u frames", ctx, frames);
	}

	return (bytes != 0);
}

/*---------------------------------------------------------------------------*/
void _output_new_stream(struct buffer *obuf, struct thread_ctx_s *ctx) {
	struct outputstate *out = &ctx->output;

	if (!out->encode.sample_rate) out->encode.sample_rate = out->sample_rate;
	if (!out->encode.sample_size)
		if (ctx->config.L24_format == L24_TRUNC16 && out->sample_size == 24) out->encode.sample_size = 16;
		else out->encode.sample_size = out->sample_size;
	if (!out->encode.channels) out->encode.channels = out->channels;

	if (out->encode.mode == ENCODE_PCM) {
		size_t length;

		buf_init(obuf, out->encode.sample_rate * out->encode.channels * out->encode.sample_size / 8 * 3);

		/*
		do not create a size (content-length) when we really don't know it but
		when we set a content-length (http_mode > 0) then at least the headers
		should be consistent
		*/
		if (!out->duration) {
			if (ctx->config.stream_length < 0) length = MAX_FILE_SIZE;
			else length = ctx->config.stream_length;
			length = (length / ((u64_t) out->encode.sample_rate * out->encode.channels * out->encode.sample_size /8)) *
					 (u64_t) out->encode.sample_rate * out->encode.channels * out->encode.sample_size / 8;
		} else length = (((u64_t) out->duration * out->encode.sample_rate) / 1000) * out->encode.channels * (out->encode.sample_size / 8);

		switch (out->format) {
		case 'w': {
			struct wave_header_s *h = malloc(sizeof(struct wave_header_s));

			memcpy(h, &wave_header, sizeof(struct wave_header_s));
			little16(&h->channels, out->encode.channels);
			little16(&h->bits_per_sample, out->encode.sample_size);
			little32(&h->sample_rate, out->encode.sample_rate);
			little32(&h->byte_rate, out->encode.sample_rate * out->encode.channels * (out->encode.sample_size / 8));
			little16(&h->block_align, out->encode.channels * (out->encode.sample_size / 8));
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
			big16(h->channels, out->encode.channels);
			big16(h->sample_size, out->encode.sample_size);
			big16(h->sample_rate_num, out->encode.sample_rate);
			big32(&h->data_size, length + 8);
			big32(&h->chunk_size, (length+8+8) + (18+8) + 4);
			big32(&h->frames, length / (out->encode.channels * (out->encode.sample_size / 8)));
			length += (8+8) + (18+8) + 4 + 8;
			out->header.buffer = (u8_t*) h;
			out->header.size = out->header.count = 54; // can't count on structure due to alignment
			}
			break;
		case 'p':
			default:
			out->header.buffer = NULL;
			out->header.size = out->header.count = 0;
			//FIXME: why setting length here at 0?
			// length = 0;
			break;
		}

		if (ctx->config.stream_length > 0) ctx->output.length = length;
		//FIXME ctx->output.length = length;
		LOG_INFO("[%p]: PCM estimated size %zu", ctx, length);
	} else if (out->encode.mode == ENCODE_FLAC) {
		FLAC__StreamEncoder *codec;
		size_t size;
		bool ok;

		size = max((out->encode.sample_rate * out->encode.channels * out->encode.sample_size / 8 * 3) / 2, 2*FLAC_MIN_SPACE);
		buf_init(obuf, size);

		//FIXME: veryfy that there is no conflic with DLL load
		codec = FLAC(f, stream_encoder_new);
		ok = FLAC(f, stream_encoder_set_verify,codec, false);
		ok &= FLAC(f, stream_encoder_set_compression_level, codec, 5);
		ok &= FLAC(f, stream_encoder_set_channels, codec, out->encode.channels);
		ok &= FLAC(f, stream_encoder_set_bits_per_sample, codec, out->encode.sample_size);
		ok &= FLAC(f, stream_encoder_set_sample_rate, codec, out->encode.sample_rate);
		ok &= FLAC(f, stream_encoder_set_blocksize, codec, FLAC_BLOCK_SIZE);
		ok &= FLAC(f, stream_encoder_set_streamable_subset, codec, true);
		ok &= !FLAC(f, stream_encoder_init_stream, codec, flac_write_callback, NULL, NULL, NULL, obuf);
		out->encode.codec = (void*) codec;

		LOG_INFO("[%p]: encoding using FLAC (%u)", ctx, ok);
	} else {
		buf_init(obuf, max((out->bitrate * 3) / 8, 128*1024));
	}
}

/*---------------------------------------------------------------------------*/
void _output_end_stream(bool finish, struct thread_ctx_s *ctx) {
	if (ctx->output.encode.mode == ENCODE_FLAC && ctx->output.encode.codec) {
		// FLAC is a pain and requires a last encode call
		LOG_INFO("[%p]: finishing FLAC", ctx);
		if (finish) FLAC(f, stream_encoder_finish, ctx->output.encode.codec);
		FLAC(f, stream_encoder_delete, ctx->output.encode.codec);
		ctx->output.encode.codec = NULL;
	}
}

/*---------------------------------------------------------------------------*/
void output_flush(struct thread_ctx_s *ctx) {
	int i;

	LOCK_O;

	ctx->render.ms_played = 0;
	/*
	Don't know actually if it's stopped or not but it will be and that stop event
	does not matter as we are flushing the whole thing. But if we want the next
	playback to work, better force that status to RD_STOPPED
	*/
	if (ctx->output.state != OUTPUT_OFF) ctx->output.state = OUTPUT_STOPPED;

	for (i = 0; i < 2; i++) if (ctx->output_thread[i].running) {
		ctx->output_thread[i].running = false;
		UNLOCK_O;
		pthread_join(ctx->output_thread[i].thread, NULL);
		LOCK_O;
	}

	ctx->output.track_started = false;
	ctx->output.track_start = NULL;
	ctx->output.completed = false;
	NFREE(ctx->output.header.buffer);
	ctx->output.header.count = 0;
	output_free_icy(ctx);
	_output_end_stream(false, ctx);
	ctx->render.index = -1;

	UNLOCK_O;

	LOG_DEBUG("[%p]: flush output buffer", ctx);
	buf_flush(ctx->outputbuf);
}

/*---------------------------------------------------------------------------*/
bool output_init(unsigned outputbuf_size, struct thread_ctx_s *ctx) {
	LOG_DEBUG("[%p] init output media renderer", ctx);

	outputbuf_size = max(outputbuf_size, 256*1024);
	ctx->outputbuf = &ctx->__o_buf;
	buf_init(ctx->outputbuf, outputbuf_size);

	if (!ctx->outputbuf->buf) {
		LOG_ERROR("[%p]: unable to malloc output buffer", ctx);
		return false;
	}

	ctx->output.track_started = false;
	ctx->output.track_start = NULL;
	ctx->output.completed = false;
	ctx->output_thread[0].running = ctx->output_thread[1].running = false;
	ctx->output_thread[0].http = ctx->output_thread[1].http = -1;
	ctx->output.icy.artist = ctx->output.icy.title = ctx->output.icy.artwork = NULL;

	ctx->render.index = -1;

#if !LINKALL
	// load share dlibrary and symbols if necessary
	if (!f.FLAC__stream_encoder_new) {
		void *handle = dlopen(LIBFLAC, RTLD_NOW);
		char *err;

		if (handle) {
			f.FLAC__stream_encoder_new = dlsym(handle, "FLAC__stream_encoder_new");
			f.FLAC__stream_encoder_finish = dlsym(handle, "FLAC__stream_encoder_finish");
			f.FLAC__stream_encoder_delete = dlsym(handle, "FLAC__stream_encoder_delete");
			f.FLAC__stream_encoder_set_verify = dlsym(handle, "FLAC__stream_encoder_set_verify");
			f.FLAC__stream_encoder_set_compression_level = dlsym(handle, "FLAC__stream_encoder_set_compression_level");
			f.FLAC__stream_encoder_set_channels = dlsym(handle, "FLAC__stream_encoder_set_channels");
			f.FLAC__stream_encoder_set_bits_per_sample = dlsym(handle, "FLAC__stream_encoder_set_bits_per_sample");
			f.FLAC__stream_encoder_set_sample_rate = dlsym(handle, "FLAC__stream_encoder_set_sample_rate");
			f.FLAC__stream_encoder_set_blocksize = dlsym(handle, "FLAC__stream_encoder_set_blocksize");
			f.FLAC__stream_encoder_set_streamable_subset = dlsym(handle, "FLAC__stream_encoder_set_streamable_subset");
			f.FLAC__stream_encoder_init_stream = dlsym(handle, "FLAC__stream_encoder_init_stream");
			f.FLAC__stream_encoder_process_interleaved = dlsym(handle, "FLAC__stream_encoder_process_interleaved");
		} else {
			LOG_INFO("loading flac: %s", dlerror());
		}
	}
#endif

	return true;
}

/*---------------------------------------------------------------------------*/
void output_close(struct thread_ctx_s *ctx) {
	LOG_INFO("[%p] close media renderer", ctx);
	buf_destroy(ctx->outputbuf);
}

/*---------------------------------------------------------------------------*/
void output_free_icy(struct thread_ctx_s *ctx) {
	NFREE(ctx->output.icy.artist);
	NFREE(ctx->output.icy.title);
	NFREE(ctx->output.icy.artwork);
}

/*---------------------------------------------------------------------------*/
static FLAC__StreamEncoderWriteStatus flac_write_callback(const FLAC__StreamEncoder *encoder, const FLAC__byte buffer[], size_t bytes, unsigned samples, unsigned current_frame, void *client_data) {
	struct buffer *obuf = (struct buffer*) client_data;
	unsigned out = _buf_space(obuf);

	if (out < bytes) {
		LOG_ERROR("[%p]: not enough space for FLAC buffer %u %u", obuf, out, bytes);
		return FLAC__STREAM_ENCODER_WRITE_STATUS_FATAL_ERROR;
	}

	out = min(_buf_cont_write(obuf), bytes);
	memcpy(obuf->writep, buffer, out);
	memcpy(obuf->buf, buffer + out, bytes - out);
	_buf_inc_writep(obuf, bytes);

	return FLAC__STREAM_ENCODER_WRITE_STATUS_OK;
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
static void apply_gain(s32_t *iptr, u32_t gain, u8_t shift, size_t frames) {
	size_t count = frames * 2;
	s64_t sample;

	if (gain == 65536 && !shift) return;

	if (gain == 65536) {
		if (shift == 8) while (count--) *iptr++ = *iptr >> 8;
		else if (shift == 16) while (count--) *iptr++ = *iptr >> 16;
		else if (shift == 24) while (count--) *iptr++ = *iptr >> 24;
	} else {
		if (!shift) while (count--) {
			sample = *iptr * (s64_t) gain;
			if (sample > MAX_VAL32) sample = MAX_VAL32;
			else if (sample < -MAX_VAL32) sample = -MAX_VAL32;
			*iptr++ = sample >> 16;
		} else if (shift == 8) while (count--) {
			sample = *iptr * (s64_t) gain;
			if (sample > MAX_VAL32) sample = MAX_VAL32;
			else if (sample < -MAX_VAL32) sample = -MAX_VAL32;
			*iptr++ = sample >> 24;
		} else if (shift == 16) while (count--) {
			sample = *iptr * (s64_t) gain;
			if (sample > MAX_VAL32) sample = MAX_VAL32;
			else if (sample < -MAX_VAL32) sample = -MAX_VAL32;
			*iptr++ = sample >> 32;
		} else if (shift == 24) while (count--) {
			sample = *iptr * (s64_t) gain;
			if (sample > MAX_VAL32) sample = MAX_VAL32;
			else if (sample < -MAX_VAL32) sample = -MAX_VAL32;
			*iptr++ = sample >> 40;
		}
	}
}

/*---------------------------------------------------------------------------*/
void _checkfade(bool start, struct thread_ctx_s *ctx) {
	frames_t bytes;

	LOG_INFO("[%p]: fade mode: %u duration: %u %s", ctx, ctx->output.fade_mode, ctx->output.fade_secs, start ? "track-start" : "track-end");

	bytes = ctx->output.sample_rate * BYTES_PER_FRAME * ctx->output.fade_secs;
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





