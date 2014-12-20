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

/*---------------------------------- FLAC ------------------------------------*/

#define FLAC_COMBO(F,N,S) ((u32_t) (((u32_t) F << 12) | ((u32_t) ((N-1) & 0x07) << 9) | ((u32_t) ((S-1) & 0x01f) << 4)))
#define QUAD_BYTE_H(n)	((u32_t) ((u64_t)(n) >> 32))
#define QUAD_BYTE_L(n)	((u32_t) (n))

u32_t	FLAC_CODED_RATES[] = { 0, 88200, 176400, 192000, 8000, 16000, 22050,
							   24000, 32000, 44100, 48000, 96000, 0, 0, 0, 0 };
u8_t	FLAC_CODED_CHANNELS[] = { 1, 2, 3, 4, 5, 6, 7, 8, 2, 2, 2, 0, 0, 0, 0, 0 };
u8_t	FLAC_CODED_SAMPLE_SIZE[] = { 0, 8, 12, 0, 16, 20, 24, 0 };

#define FLAC_TAG	(0xf8ff)	// byte order is reversed because it's treated as a u16
#define FLAC_GET_FRAME_TAG(n)	((u16_t) ((n) & 0xf8ff))
#define FLAC_GET_BLOCK_SIZE(n)	((u8_t) ((n) >> 4) & 0x0f)
#define FLAC_GET_FRAME_RATE(n) ((u8_t) ((n) & 0x0f))
#define FLAC_GET_FRAME_CHANNEL(n) ((u8_t) ((n) >> 4) & 0x0f)
#define FLAC_GET_FRAME_SAMPLE_SIZE(n) ((u8_t) ((n) >> 1) & 0x07)
#define FLAC_GET_BLOCK_STRATEGY(n) ((u16_t) ((n) & 0x0100))

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


flac_streaminfo_t FLAC_NORMAL_STREAMINFO = {
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

#define FLAC_TOTAL_SAMPLES 0xffffffff

flac_streaminfo_t FLAC_FULL_STREAMINFO = {
		{ 0x00, 0x10 },
		{ 0xff, 0xff },
		{ 0x00, 0x00, 0x00 },
		{ 0x00, 0x00, 0x00 },
		{ BYTE_1(FLAC_COMBO(44100, 2, 16)),
		BYTE_2(FLAC_COMBO(44100, 2, 16)),
		BYTE_3(FLAC_COMBO(44100, 2, 16)),
		BYTE_4(FLAC_COMBO(44100, 2, 16)) | BYTE_1(QUAD_BYTE_H(FLAC_TOTAL_SAMPLES)) },
		{ BYTE_1(QUAD_BYTE_L(FLAC_TOTAL_SAMPLES)),
		BYTE_2(QUAD_BYTE_L(FLAC_TOTAL_SAMPLES)),
		BYTE_3(QUAD_BYTE_L(FLAC_TOTAL_SAMPLES)),
		BYTE_4(QUAD_BYTE_L(FLAC_TOTAL_SAMPLES)) },
		{ 0xaa, 0xaa, 0xaa, 0xaa, 0xaa, 0xaa, 0xaa, 0xaa,
		0xaa, 0xaa, 0xaa, 0xaa, 0xaa, 0xaa, 0xaa, 0xaa }
};


static u8_t flac_vorbis_block[] = { 0x84,0x00,0x00,0x28,0x20,0x00,0x00,0x00,0x72,
									0x65,0x66,0x65,0x72,0x65,0x6E,0x63,0x65,0x20,
									0x6C,0x69,0x62,0x46,0x4C,0x41,0x43,0x20,0x31,
									0x2E,0x32,0x2E,0x31,0x20,0x32,0x30,0x30,0x37,
									0x30,0x39,0x31,0x37,0x00,0x00,0x00,0x00 };

static u8_t flac_header[] = {
			'f', 'L', 'a', 'C',
			0x00,
			(u8_t) ((u32_t) sizeof(flac_streaminfo_t) >> 16),
			(u8_t) ((u32_t) sizeof(flac_streaminfo_t) >> 8),
			(u8_t) ((u32_t) sizeof(flac_streaminfo_t))
		};


/*---------------------------------- WAVE ------------------------------------*/

static struct wave_header_s {
	u8_t 	chunk_id[4];
	u32_t	chunk_size;
	u8_t	format[4];
	u8_t	subchunk1_id[4];
	u32_t	subchunk1_size;
	u16_t	audio_format;
	u16_t	channels;
	u32_t	sample_rate;
	u32_t   byte_rate;
	u16_t	block_align;
	u16_t	bits_per_sample;
	u8_t	subchunk2_id[4];
	u32_t	subchunk2_size;

} wave_header = {
		{ 'R', 'I', 'F', 'F' },
		1000000000 + 8,
		{ 'W', 'A', 'V', 'E' },
		{ 'f','m','t',' ' },
		16,
		1,
		0,
		0,
		0,
		0,
		0,
		{ 'd', 'a', 't', 'a' },
		1000000000 - sizeof(struct wave_header_s) - 8 - 8
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
static u16_t flac_block_size(u8_t block_size)
{
	if (block_size == 0x01) return 192;
	if (block_size <= 0x05) return 576 * (1 << (block_size - 2));
	if (block_size == 0x06) return 0;
	if (block_size == 0x07) return 0;
	if (block_size <= 0xf) return 256 * (1 << (block_size - 8));
	return 0;
}

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

	_scale_and_pack_frames(ctx->buf + ctx->buffill * ctx->bytes_per_frame, (s32_t *)(void *)obuf, out_frames, gainL, gainR, ctx->output.format);
	ctx->buffill += out_frames;

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
#else
static void output_thread(struct thread_ctx_s *ctx) {
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

			// open the buffer if needed (should be opened in slimproto)
			if (!out->write_file) {
				char buf[SQ_STR_LENGTH];

				sprintf(buf, "%s/%s", ctx->config.buffer_dir, out->buf_name);
				out->write_file = fopen(buf, "wb");
				out->write_count = out->write_count_t = 0;
				LOG_ERROR("[%p]: write file not opened %s", ctx, buf);
			}

			// re-size buffer if needed
			if (ctx->config.buffer_limit != -1 && out->write_count > (u32_t) ctx->config.buffer_limit) {
				u8_t *buf;
				u32_t n;
				struct stat Status;

				// LMS will need to wait for the player to consume data ...
				if ((out->write_count - out->read_count) > (u32_t) ctx->config.buffer_limit / 2) {
					UNLOCK_S;
					usleep(100000);
					continue;
				}

				LOCK_O;
				out->write_count -= ctx->config.buffer_limit / 4;
				out->read_count -= ctx->config.buffer_limit / 4;

				buf = malloc(2L*1024L*1024L);
				fseek(out->write_file, 0, SEEK_SET);
				fseek(out->read_file, ctx->config.buffer_limit / 4, SEEK_SET);
				for (n = 0; n < out->write_count;) {
					u32_t b;
					b = fread(buf, 1, 2L*1024L*1024L, out->read_file);
					fwrite(buf, 1, b, out->write_file);
					n += b;
				}
				free(buf);
				fflush(out->write_file);

				fseek(out->read_file, out->read_count, SEEK_SET);
				fresize(out->write_file, out->write_count);
				fstat(fileno(out->write_file), &Status);
				LOG_INFO("[%p]: re-sizing w:%d r:%d rp:%d ws:%d", ctx,
						  out->write_count + ctx->config.buffer_limit /4,
						  out->read_count + ctx->config.buffer_limit /4,
						  ftell(out->read_file), Status.st_size);
				UNLOCK_O;
			}

			// some file format require the re-insertion of headers
			if (!out->write_count) {
				// flac case
				if (!strcmp(out->ext, "flac")) {
					if (space >= FLAC_RECV_MIN) {
						if (strncmp(_buf_readp(ctx->streambuf), "fLaC", 4) && ctx->config.flac_header != FLAC_NO_HEADER) {
							flac_frame_t *frame;
							flac_streaminfo_t *streaminfo;
							u32_t rate;
							u8_t sample_size, channels;
							u16_t block_size = 0;

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
								if (ctx->config.flac_header == FLAC_NORMAL_HEADER)
									memcpy(streaminfo, &FLAC_NORMAL_STREAMINFO, sizeof(flac_streaminfo_t));
								else
									memcpy(streaminfo, &FLAC_FULL_STREAMINFO, sizeof(flac_streaminfo_t));

								if (!FLAC_GET_BLOCK_STRATEGY(frame->tag)) {
									block_size = flac_block_size(FLAC_GET_BLOCK_SIZE(frame->bsize_rate));
									if (block_size) {
										streaminfo->min_block_size[0] = streaminfo->max_block_size[0] = BYTE_3(block_size);
										streaminfo->min_block_size[1] = streaminfo->max_block_size[1] = BYTE_4(block_size);
									}
									else {
										LOG_WARN("[%p]: unhandled blocksize %d, using variable", ctx, frame->tag);
									}
								}

								streaminfo->combo[0] = BYTE_1(FLAC_COMBO(rate, channels, sample_size));
								streaminfo->combo[1] = BYTE_2(FLAC_COMBO(rate, channels, sample_size));
								streaminfo->combo[2] = BYTE_3(FLAC_COMBO(rate, channels, sample_size));
								streaminfo->combo[3] = BYTE_4(FLAC_COMBO(rate, channels, sample_size));
								out->write_count = fwrite(&flac_header, 1, sizeof(flac_header), out->write_file);
								out->write_count += fwrite(streaminfo, 1, sizeof(flac_streaminfo_t), out->write_file);
								out->write_count += fwrite(flac_vorbis_block, 1, sizeof(flac_vorbis_block), out->write_file);
								out->write_count_t = out->write_count;
								LOG_INFO("[%p]: flac header ch:%d, s:%d, r:%d, b:%d", ctx, channels, sample_size, rate, block_size);
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

			/*
			endianness re-ordering for PCM (1 = little endian)
			this could be highly optimized ... and is similar to the functions
			found in output_pack of original squeezelite
			*/
			if (!strcmp(out->ext, "pcm")) {
				u32_t i;
				u8_t j, *p;
				p = _buf_readp(ctx->streambuf);
				// 2 or 4 bytes or 3 bytes with no packing, but changed endianness
				if ((out->sample_size == 16 || out->sample_size == 32 || (out->sample_size == 24 && ctx->config.L24_format == L24_PACKED)) && out->endianness) {
					u8_t buf[4];
					u8_t inc = out->sample_size/8;
					space = (space / inc) * inc;
					for (i = 0; i < space; i += inc) {
						for (j = 0; j < inc; j++) buf[inc-1-j] = *(p+j);
						for (j = 0; j < inc; j++) *(p++) = buf[j];
					}
				}
				// 3 bytes with packing required and endianness changed
				if (out->sample_size == 24 && ctx->config.L24_format == L24_PACKED_LPCM) {
					u8_t buf[12];
					space = (space / 12) * 12;
					for (i = 0; i < space; i += 12) {
						// order after that should be L0T,L0M,L0B,R0T,R0M,R0B,L1T,L1M,L1B,R1T,R1M,R1B
						if (out->endianness) for (j = 0; j < 12; j += 3) {
							buf[j] = *(p+j+3-1);
							buf[j+1] = *(p+j+1);
							buf[j+2] = *(p+j);
                        }
						else for (j = 0; j < 12; j++) buf[j] = *(p+j);
 						// L0T,L0M & R0T,R0M
						*p++ = buf[0]; *p++ = buf[1];
						*p++ = buf[3]; *p++ = buf[4];
						// L1T,L1M & R1T,R1M
						*p++ = buf[6]; *p++ = buf[7];
						*p++ = buf[9]; *p++ = buf[10];
						// L0B, R0B, L1B, R1B
						*p++ = buf[2]; *p++ = buf[5]; *p++ = buf[8]; *p++ = buf[11];
						// after that R0T,R0M,L0T,L0M,R1T,R1M,L1T,L1M,R0B,L0B,R1B,L1B
					}
				}
			}

			if (!strcmp(out->ext, "wav")) {
				if (!out->write_count) {
					wave_header.channels = out->channels;
					wave_header.bits_per_sample = out->sample_size;
					wave_header.sample_rate = out->sample_rate;
					wave_header.byte_rate = out->sample_rate * out->channels * (out->sample_size / 8);
					wave_header.block_align = out->channels * (out->sample_size / 8);
					out->write_count = fwrite(&wave_header, 1, sizeof(struct wave_header_s), out->write_file);
					out->write_count_t = out->write_count;
					LOG_INFO("[%p]: wave header", ctx);
				}
			}

			// write in the file
			if (ready) {
				fwrite(_buf_readp(ctx->streambuf), 1, space, out->write_file);
				fflush(out->write_file);
				_buf_inc_readp(ctx->streambuf, space);
				out->write_count += space;
				out->write_count_t += space;
			}

			sleep_time = 10000;
		} else sleep_time = 100000;

		// all done, time to close the file
		if (out->write_file && ctx->stream.state <= DISCONNECT && (!_buf_used(ctx->streambuf) || (out->sample_size == 24 && _buf_used(ctx->streambuf) < 12)))
		{
			LOG_INFO("[%p] wrote total %d", ctx, out->write_count_t);
			fclose(out->write_file);
			out->write_file = NULL;
#ifdef __EARLY_STMd__
			ctx->read_ended = true;
			wake_controller(ctx);
#endif

			UNLOCK_S;
			buf_flush(ctx->streambuf);
		}
		else {
			UNLOCK_S;
		}

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
			pthread_create(&ctx->mr_thread, &attr, &output_thread, ctx);
			break;
	case SQ_STREAM:
			pthread_create(&ctx->mr_thread, &attr, &output_thru_thread, ctx);
			break;
	default:
		break;
	}
	pthread_attr_destroy(&attr);
#endif
#if WIN
	switch (ctx->config.mode) {
	case SQ_FULL:
		ctx->mr_thread = CreateThread(NULL, OUTPUT_THREAD_STACK_SIZE, (LPTHREAD_START_ROUTINE) &output_thread, ctx, 0, NULL);
		break;
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



