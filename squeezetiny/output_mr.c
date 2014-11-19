/*
 *  Squeezelite - lightweight headless squeezebox emulator
 *
 *  (c) Adrian Smith 2012-2014, triode1@btinternet.com
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

/*
TODO
- wakeup outputhread only when streambuf has written something ?
- move the that handles adding headers + endianess out of main loop
*/

#include "squeezelite.h"

#define FRAME_BLOCK MAX_SILENCE_FRAMES

static log_level loglevel = lWARN;

#define BYTE_1(n)	((u8_t) (n >> 24))
#define BYTE_2(n)	((u8_t) (n >> 16))
#define BYTE_3(n)	((u8_t) (n >> 8))
#define BYTE_4(n)	((u8_t) (n))


#define FLAC_COMBO(F,N,S) ((u32_t) (((u32_t) F << 12) | ((u32_t) ((N-1) & 0x07) << 9) | ((u32_t) ((S-1) & 0x01f) << 4)))
#define QUAD_BYTE_H(n)	((u32_t) ((u64_t)(n) >> 32))
#define QUAD_BYTE_L(n)	((u32_t) (n))

u32_t	FLAC_CODED_RATES[] = { 0, 88200, 176400, 192000, 8000, 16000, 22050,
							   24000, 32000, 44100, 48000, 96000, 0, 0, 0, 0 };
u8_t	FLAC_CODED_CHANNELS[] = { 1, 2, 3, 4, 5, 6, 7, 8, 2, 2, 2, 0, 0, 0, 0, 0 };
u8_t	FLAC_CODED_SAMPLE_SIZE[] = { 0, 8, 12, 0, 16, 20, 24, 0 };

#define FLAC_TAG	(0xf8ff)	// byte order is reversed because it's treated as a u16
#define FLAC_GET_FRAME_TAG(n)	((u16_t) ((n) & 0xf8ff))
#define FLAC_GET_FRAME_RATE(n) ((u8_t) ((n) & 0x0f))
#define FLAC_GET_FRAME_CHANNEL(n) ((u8_t) ((n) >> 4) & 0x0f)
#define FLAC_GET_FRAME_SAMPLE_SIZE(n) ((u8_t) ((n) >> 1) & 0x07)

#define FLAC_RECV_MIN	128

typedef struct flac_frame_s {
	u16_t	tag;
	u8_t    bsize_rate;
	u8_t	channels_sample_size;
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
		0x10, 0x00,
		0xff, 0xff,
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
		{ 0xaa, 0xaa, 0xaa, 0xaa, 0xaa, 0xaa, 0xaa, 0xaa,
		0xaa, 0xaa, 0xaa, 0xaa, 0xaa, 0xaa, 0xaa, 0xaa }
};

static u8_t flac_header[] = {
			'f', 'L', 'a', 'C',
			0x80,
			(u8_t) ((u32_t) sizeof(flac_streaminfo_t) << 16),
			(u8_t) ((u32_t) sizeof(flac_streaminfo_t) << 8),
			(u8_t) ((u32_t) sizeof(flac_streaminfo_t))
		};


#define LOCK_S   mutex_lock(ctx->streambuf->mutex)
#define UNLOCK_S mutex_unlock(ctx->streambuf->mutex)
#if 0
#define LOCK_O   mutex_lock(ctx->outputbuf->mutex)
#define UNLOCK_O mutex_unlock(ctx->outputbuf->mutex)
#else
#define LOCK_O
#define UNLOCK_O
#endif
#define LOCK_D   mutex_lock(ctx->decode.mutex);
#define UNLOCK_D mutex_unlock(ctx->decode.mutex);

extern u8_t *silencebuf;
#if DSD
extern u8_t *silencebuf_dop;
#endif

/*---------------------------------------------------------------------------*/
#if 0
static int _mr_write_frames(frames_t out_frames, bool silence, s32_t gainL, s32_t gainR,
								s32_t cross_gain_in, s32_t cross_gain_out, s32_t **cross_ptr, struct thread_ctx_s *ctx) {

	u8_t *obuf;

	if (!silence) {

		if (ctx->output.fade == FADE_ACTIVE && ctx->output.fade_dir == FADE_CROSS && *cross_ptr) {
			_apply_cross(ctx->outputbuf, out_frames, cross_gain_in, cross_gain_out, cross_ptr);
		}

		obuf = ctx->outputbuf->readp;

	} else {

		obuf = silencebuf;
	}

	IF_DSD(
		   if (ctx->output.dop) {
			   if (silence) {
				   obuf = silencebuf_dop;
			   }
			   update_dop_marker((u32_t *)obuf, out_frames);
		   }
	)

#if 0
	_scale_and_pack_frames(ctx->buf + ctx->buffill * ctx->bytes_per_frame, (s32_t *)(void *)obuf, out_frames, gainL, gainR, ctx->output.format);
	ctx->buffill += out_frames;
#endif

	return (int)out_frames;
}
#endif

#if 0
/*---------------------------------------------------------------------------*/
static void output_thread(struct thread_ctx_s *ctx) {

	LOCK_O;

	switch (ctx->output.format) {
	case S32_LE:
		ctx->bytes_per_frame = 4 * 2; break;
	case S24_3LE:
		ctx->bytes_per_frame = 3 * 2; break;
	case S16_LE:
		ctx->bytes_per_frame = 2 * 2; break;
	default:
		ctx->bytes_per_frame = 4 * 2; break;
	}

	UNLOCK_O;

	while (ctx->mr_running) {

		LOCK_O;

		ctx->output.device_frames = 0;
		ctx->output.updated = gettime_ms();
		ctx->output.frames_played_dmp = ctx->output.frames_played;

		_output_frames(FRAME_BLOCK, ctx);

		UNLOCK_O;

		if (ctx->buffill) {
			fwrite(ctx->buf, bytes_per_frame, buffill, stdout);
			ctx->buffill = 0;
		}
	}
}
#endif

/*---------------------------------------------------------------------------*/
void output_mr_loglevel(log_level level) {
	LOG_ERROR("output_mr init %d", level);
	loglevel = level;
}

/*---------------------------------------------------------------------------*/
void wake_output(struct thread_ctx_s *ctx) {
	return;
}

/*---------------------------------------------------------------------------*/
static void output_thru_thread(struct thread_ctx_s *ctx) {
	while (ctx->mr_running) {
		size_t	space;
		unsigned sleep_time;
		out_ctx_t *out;

		LOCK_S;
		out = &ctx->out_ctx[ctx->out_idx];

		if (_buf_used(ctx->streambuf)) {
			bool ready = true;
			space = _buf_cont_read(ctx->streambuf);

			// first thing first, opens the buffer if needed
			if (!out->write_file) {
				char buf[SQ_STR_LENGTH];

				sprintf(buf, "%s/%s", ctx->config.buffer_dir, out->buf_name);
				out->write_file = fopen(buf, "wb");
				out->write_count = 0;
			}

			// some file format require the re-insertion of headers
			if (!out->write_count) {
				// flac case
				if (!strcmp(out->ext, "flac")) {
					if (space >= FLAC_RECV_MIN) {
						if (strncmp(_buf_readp(ctx->streambuf), "fLaC", 4)) {
							flac_frame_t *frame;
							flac_streaminfo_t *streaminfo;
							u32_t rate;
							u8_t sample_size;
							u8_t channels;

							frame = (flac_frame_t*) _buf_readp(ctx->streambuf);
							if (FLAC_GET_FRAME_TAG(frame->tag) != FLAC_TAG) {
								LOG_ERROR("[%p]: no header and not a frame ...", ctx);
								ready = true;
							}
							else {
								rate = FLAC_CODED_RATES[FLAC_GET_FRAME_RATE(frame->bsize_rate)];
								sample_size = FLAC_CODED_SAMPLE_SIZE[FLAC_GET_FRAME_SAMPLE_SIZE(frame->channels_sample_size)];
								channels = FLAC_CODED_CHANNELS[FLAC_GET_FRAME_CHANNEL(frame->channels_sample_size)];
								streaminfo = malloc(sizeof(flac_streaminfo_t));
								memcpy(streaminfo, &FLAC_STREAMINFO, sizeof(flac_streaminfo_t));
								streaminfo->combo[0] = BYTE_1(FLAC_COMBO(rate, channels, sample_size));
								streaminfo->combo[1] = BYTE_2(FLAC_COMBO(rate, channels, sample_size));
								streaminfo->combo[2] = BYTE_3(FLAC_COMBO(rate, channels, sample_size));
								streaminfo->combo[3] = BYTE_4(FLAC_COMBO(rate, channels, sample_size));
								out->write_count = fwrite(&flac_header, 1, sizeof(flac_header), out->write_file);
								out->write_count += fwrite(streaminfo, 1, sizeof(flac_streaminfo_t), out->write_file);
								LOG_INFO("[%p]: flac header ch:%d, s:%d, r:%d", ctx, channels, sample_size, rate);
								if (!rate || !sample_size || !channels) {
									LOG_ERROR("[%p]: wrong header %d %d %d", rate, channels, sample_size);
								}
								free(streaminfo);
							}
						}
					}
					else ready = false;
				}
			}

			if (!strcmp(out->ext, "pcm")) {
				u32_t i;
				u8_t j, buf[4];
				u8_t *p, inc = out->sample_size/8;
				space = (space / inc) * inc;
				p = _buf_readp(ctx->streambuf);
				for (i = 0; i < space; i += inc) {
					for (j = 0; j < inc; j++) buf[inc-1-j] = *(p+j);
					for (j = 0; j < inc; j++) *(p++) = buf[j];
				}
			}

			// write in the file
			if (ready) {
				fwrite(_buf_readp(ctx->streambuf), 1, space, out->write_file);
				fflush(out->write_file);
				_buf_inc_readp(ctx->streambuf, space);
				out->write_count += space;
			}

			sleep_time = 10000;
		} else sleep_time = 100000;

		// all done, time to close the file
		if (ctx->stream.state <= DISCONNECT & !_buf_used(ctx->streambuf) && out->write_file)
		{
			LOG_INFO("[%p] wrote total %d", ctx, out->write_count);
			fclose(out->write_file);
			out->write_file = NULL;
		}
		UNLOCK_S;

		usleep(sleep_time);
	}
}

/*---------------------------------------------------------------------------*/
void output_mr_thread_init(unsigned output_buf_size, char *params, unsigned rate[], unsigned rate_delay, struct thread_ctx_s *ctx) {

	LOG_INFO("[%p] init output media renderer", ctx);

#if 0
	ctx->buf = malloc(FRAME_BLOCK * BYTES_PER_FRAME);
	if (!ctx->buf) {
		LOG_ERROR("unable to malloc buf", NULL);
		return;
	}
	ctx->buffill = 0;

	memset(&ctx->output, 0, sizeof(ctx->output));

	ctx->output.format = S16_LE;
	ctx->output.start_frames = FRAME_BLOCK * 2;
	ctx->output.write_cb = &_mr_write_frames;
	ctx->output.rate_delay = rate_delay;
	ctx->output.format = S16_LE;

	// ensure output rate is specified to avoid test open
	if (!rate[0]) rate[0] = 44100;

	output_init_common("", output_buf_size, rate, ctx);
#endif

	ctx->mr_running = true;

#if LINUX || OSX || FREEBSD
	pthread_attr_t attr;
	pthread_attr_init(&attr);
	pthread_attr_setstacksize(&attr, PTHREAD_STACK_MIN + OUTPUT_THREAD_STACK_SIZE);
	switch (ctx->config.mode) {
	case SQ_FULL:
			pthread_create(&ctx->mr_thread, &attr, output_thru_thread, ctx);
			break;
	case SQ_DIRECT:
	case SQ_STREAM:
			pthread_create(&ctx->mr_thread, &attr, output_thru_thread, ctx);
			break;
	default:
		break;
	}
	pthread_attr_destroy(&attr);
#endif
#if WIN
	switch (ctx->config.mode) {
	case SQ_FULL:
		ctx->mr_thread = CreateThread(NULL, OUTPUT_THREAD_STACK_SIZE, (LPTHREAD_START_ROUTINE) NULL, ctx, 0, NULL);
		break;
	case SQ_DIRECT:
	case SQ_STREAM:
		ctx->mr_thread = CreateThread(NULL, OUTPUT_THREAD_STACK_SIZE, (LPTHREAD_START_ROUTINE)&output_thru_thread, ctx, 0, NULL);
		break;
	default:
		break;
	}
#endif
}

/*---------------------------------------------------------------------------*/
void output_mr_close(struct thread_ctx_s *ctx) {
	LOG_INFO("[%p] close media renderer", ctx);

	LOCK_O;
	ctx->mr_running = false;
	UNLOCK_O;

#if 0
	free(ctx->buf);
	output_close_common(ctx);
#endif

#if LINUX || OSX || FREEBSD
	pthread_join(ctx->mr_thread, NULL);
#endif
}

/*---------------------------------------------------------------------------*/
void output_flush(struct thread_ctx_s *ctx) {
	int i;

	LOG_INFO("[%p] flush output buffer", ctx);

	LOCK_S;LOCK_O;
	for (i = 0; i < 2; i++) {
		if (ctx->out_ctx[i].read_file) {
			fclose(ctx->out_ctx[i].read_file);
			ctx->out_ctx[i].read_file = NULL;
		}

		if (ctx->out_ctx[i].write_file) {
			fclose(ctx->out_ctx[i].write_file);
			ctx->out_ctx[i].write_file = NULL;
		}
	}
	UNLOCK_S;UNLOCK_O;
}



