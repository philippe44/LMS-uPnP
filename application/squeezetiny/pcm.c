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
#define LOCK_O_direct   mutex_lock(ctx->outputbuf->mutex)
#define UNLOCK_O_direct mutex_unlock(ctx->outputbuf->mutex)
#define IF_DIRECT(x)    { x }
#define IF_PROCESS(x)

struct pcm {
	u32_t sample_rate;
	size_t bytes_per_frame;
	u8_t *header;
	size_t header_size;
};

/*---------------------------------------------------------------------------*/
static decode_state pcm_decode(struct thread_ctx_s *ctx) {
	size_t in, out;
	struct pcm *p = ctx->decode.handle;
	u8_t *iptr, *optr, ibuf[3*8*2], obuf[3*8*2];

	LOCK_S;
	LOCK_O_direct;

	in = min(_buf_used(ctx->streambuf), _buf_cont_read(ctx->streambuf));

	if (ctx->stream.state <= DISCONNECT && in == 0) {
		UNLOCK_O_direct;
		UNLOCK_S;
		return DECODE_COMPLETE;
	}

	iptr = (u8_t *)ctx->streambuf->readp;

	if (ctx->decode.new_stream) {
		ctx->output.track_start = ctx->outputbuf->writep;

		if (!ctx->output.in_endian && !(*((u64_t*) iptr)) &&
		   (strstr(ctx->server_version, "7.7") || strstr(ctx->server_version, "7.8"))) {
			/*
			LMS < 7.9 does not remove 8 bytes when sending aiff files but it does
			when it is a transcoding ... so this does not matter for 16 bits samples
			but it is a mess for 24 bits ... so this tries to guess what we are
			receiving
			*/
			_buf_inc_readp(ctx->streambuf, 8);
			in = min(_buf_used(ctx->streambuf), _buf_cont_read(ctx->streambuf));

			LOG_INFO("[%p]: guessing a AIFF extra header", ctx);
		}
	}

	// the min in and out are enough to process a full header
	if (p->header) {
		out = min(p->header_size, _buf_cont_write(ctx->outputbuf));
		memcpy(ctx->outputbuf->writep, p->header, out);
		memcpy(ctx->outputbuf->buf, p->header + out, p->header_size - out);
		_buf_inc_writep(ctx->outputbuf, p->header_size);

		free(p->header);
		p->header = NULL;
	}

	if (ctx->decode.new_stream) {
		LOG_INFO("[%p]: setting track_start", ctx);

		//FIXME: not in use for now, sample rate always same how to know starting rate when resamplign will be used
		ctx->output.current_sample_rate = decode_newstream(p->sample_rate, ctx->output.supported_rates, ctx);
		if (ctx->output.fade_mode) _checkfade(true, ctx);
		ctx->decode.new_stream = false;
	}

	out = min(_buf_space(ctx->outputbuf), _buf_cont_write(ctx->outputbuf));

	// no enough cont'd place in input
	if (in < p->bytes_per_frame) {
		memcpy(ibuf, iptr, in);
		memcpy(ibuf + in, ctx->streambuf->buf, p->bytes_per_frame - in);
		iptr = ibuf;
		in = p->bytes_per_frame;
	}

	// not enough cont'd place in output
	if (out < p->bytes_per_frame) {
		optr = obuf;
		out = p->bytes_per_frame;
	} else optr = ctx->outputbuf->writep;

	// might be just bytes_per_frames
	in = out = (min(in, out) / p->bytes_per_frame) * p->bytes_per_frame;

	// apply gain if any (before truncation or packing)
	apply_gain(iptr, ctx->output.replay_gain, in, ctx->output.sample_size, ctx->output.in_endian);

	// truncate or swap
	if (ctx->output.trunc16) {
		truncate16(optr, iptr, in, ctx->output.in_endian, ctx->output.out_endian);
		out = (out * 2) / 3;
	} else if (ctx->output.format == 'p' && ctx->output.sample_size == 24 && ctx->config.L24_format == L24_PACKED_LPCM) {
		lpcm_pack(optr, iptr, in, ctx->output.channels, ctx->output.in_endian);
	} else if (ctx->output.in_endian != ctx->output.out_endian) {
		swap(optr, iptr, in, ctx->output.sample_size);
	} else memcpy(optr, iptr, in);

	// take the data from temporary buffer if needed
	if (optr == obuf) {
		size_t out = _buf_cont_write(ctx->outputbuf);
		memcpy(ctx->outputbuf->writep, optr, out);
		memcpy(ctx->outputbuf->buf, optr + out, p->bytes_per_frame - out);
	}

	_buf_inc_readp(ctx->streambuf, in);
	_buf_inc_writep(ctx->outputbuf, out);

	UNLOCK_O_direct;
	UNLOCK_S;

	return DECODE_RUNNING;
}

/*---------------------------------------------------------------------------*/
static void pcm_open(u8_t sample_size, u32_t sample_rate, u8_t channels, u8_t endianness, struct thread_ctx_s *ctx) {
	struct pcm *p = ctx->decode.handle;
	size_t length;

	if (!p)	p = ctx->decode.handle = malloc(sizeof(struct pcm));
	if (!p) return;

	p->sample_rate = sample_rate;
	if (sample_size == 24 && ctx->config.L24_format == L24_PACKED_LPCM) {
		p->bytes_per_frame = 2 * (sample_size * channels) / 8;
  	} else p->bytes_per_frame = (sample_size * channels) / 8;

	length = output_pcm_header((void*) &p->header, &p->header_size, ctx);
	if (ctx->config.stream_length > 0) ctx->output.length = length;

	LOG_INFO("[%p]: estimated size %zd", ctx, length);
}

/*---------------------------------------------------------------------------*/
static void pcm_close(struct thread_ctx_s *ctx) {
	struct pcm *p = ctx->decode.handle;

	if (p) free(p);
	ctx->decode.handle = NULL;
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

