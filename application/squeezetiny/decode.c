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

// decode thread

#include "squeezelite.h"

#define READ_SIZE  512
#define WRITE_SIZE 32 * 1024

log_level 		loglevel = lWARN;
struct codec	*codecs[MAX_CODECS];

#define LOCK_S   mutex_lock(ctx->streambuf->mutex)
#define UNLOCK_S mutex_unlock(ctx->streambuf->mutex)
#define LOCK_O   mutex_lock(ctx->outputbuf->mutex)
#define UNLOCK_O mutex_unlock(ctx->outputbuf->mutex)
#define LOCK_D   mutex_lock(ctx->decode.mutex);
#define UNLOCK_D mutex_unlock(ctx->decode.mutex);

#if PROCESS
#error 1
#define IF_DIRECT(x)    if (decode.direct) { x }
#define IF_PROCESS(x)   if (!decode.direct) { x }
#define MAY_PROCESS(x)  { x }
#else
#define IF_DIRECT(x)    { x }
#define IF_PROCESS(x)
#define MAY_PROCESS(x)
#endif

/*---------------------------------------------------------------------------*/
static void *decode_thread(struct thread_ctx_s *ctx) {
	while (ctx->decode_running) {
		size_t bytes, space, min_space;
		bool toend;
		bool ran = false;

		LOCK_S;
		bytes = _buf_used(ctx->streambuf);
		toend = (ctx->stream.state <= DISCONNECT);
		UNLOCK_S;
		LOCK_O;
		space = _buf_space(ctx->outputbuf);
		UNLOCK_O;

		LOCK_D;

		if (ctx->decode.state == DECODE_RUNNING && ctx->codec) {

			LOG_SDEBUG("streambuf bytes: %u outputbuf space: %u", bytes, space);

			IF_DIRECT(
				min_space = ctx->codec->min_space;
			);
			IF_PROCESS(
				min_space = ctx->process.max_out_frames * BYTES_PER_FRAME;
			);

			if (space > min_space && (bytes > ctx->codec->min_read_bytes || toend)) {

				ctx->decode.state = ctx->codec->decode();

				IF_PROCESS(
					if (ctx->process.in_frames) {
						ctx->process_samples();
					}

					if (ctx->decode.state == DECODE_COMPLETE) {
						ctx->process_drain();
					}
				);

				if (ctx->decode.state != DECODE_RUNNING) {

					LOG_INFO("decode %s", ctx->decode.state == DECODE_COMPLETE ? "complete" : "error");

					LOCK_O;
//					if (ctx->output.fade_mode) _checkfade(false, ctx);
					UNLOCK_O;

					wake_controller(ctx);
				}

				ran = true;
			}
		}

		UNLOCK_D;

		if (!ran) {
			usleep(100000);
		}
	}

	return 0;
}


/*---------------------------------------------------------------------------*/
void decode_init(log_level level, const char *include_codecs, const char *exclude_codecs, bool full) {
#if DSD || FFMPEG || !defined(NO_CODEC)
	int i = 0;
#endif

	loglevel = level;

	LOG_DEBUG("init decode, include codecs: %s exclude codecs: %s", include_codecs ? include_codecs : "", exclude_codecs);

	// register codecs
	// dsf,dff,alc,wma,wmap,wmal,aac,spt,ogg,ogf,flc,aif,pcm,mp3
#if DSD
	if (!strstr(exclude_codecs, "dsd")  && (!include_codecs || strstr(include_codecs, "dsd")))  codecs[i++] = register_dsd();
#endif
#if FFMPEG
	if (!strstr(exclude_codecs, "alac") && (!include_codecs || strstr(include_codecs, "alac")))  codecs[i++] = register_ff("alc");
	if (!strstr(exclude_codecs, "wma")  && (!include_codecs || strstr(include_codecs, "wma")))   codecs[i++] = register_ff("wma");
#endif
#ifndef NO_CODEC
	if (!strstr(exclude_codecs, "aac")  && (!include_codecs || strstr(include_codecs, "aac")))  codecs[i++] = register_faad();
	if (!strstr(exclude_codecs, "ogg")  && (!include_codecs || strstr(include_codecs, "ogg")))  codecs[i++] = register_vorbis();
	if (!strstr(exclude_codecs, "flac") && (!include_codecs || strstr(include_codecs, "flac"))) codecs[i++] = register_flac();
	if (!strstr(exclude_codecs, "pcm")  && (!include_codecs || strstr(include_codecs, "pcm")))  codecs[i++] = register_pcm();

	// try mad then mpg for mp3 unless command line option passed
	if (!(strstr(exclude_codecs, "mp3") || strstr(exclude_codecs, "mad")) &&
		(!include_codecs || strstr(include_codecs, "mp3") || strstr(include_codecs, "mad")))	codecs[i] = register_mad();
	if (!(strstr(exclude_codecs, "mp3") || strstr(exclude_codecs, "mpg")) && !codecs[i] &&
		(!include_codecs || strstr(include_codecs, "mp3") || strstr(include_codecs, "mpg")))    codecs[i] = register_mpg();
#endif
}

/*---------------------------------------------------------------------------*/
void decode_thread_init(struct thread_ctx_s *ctx) {
	LOG_DEBUG("[%p]: init decode", ctx);
	mutex_create(ctx->decode.mutex);

#if LINUX || OSX || FREEBSD
	pthread_attr_t attr;
	pthread_attr_init(&attr);
	pthread_attr_setstacksize(&attr, PTHREAD_STACK_MIN + DECODE_THREAD_STACK_SIZE);
	if (ctx->config.mode == SQ_FULL)
		pthread_create(&ctx->decode_thread, &attr, (void *(*)(void*)) decode_thread, ctx);
	pthread_attr_destroy(&attr);
#endif
#if WIN
	if (ctx->config.mode == SQ_FULL)
		ctx->decode_thread = CreateThread(NULL, DECODE_THREAD_STACK_SIZE, (LPTHREAD_START_ROUTINE)&decode_thread, ctx, 0, NULL);
#endif

	ctx->decode.new_stream = true;
	ctx->decode.state = DECODE_STOPPED;

	MAY_PROCESS(
		ctx->decode.direct = true;
		ctx->decode.process = false;
	);
}

/*---------------------------------------------------------------------------*/
void decode_close(struct thread_ctx_s *ctx) {

	LOG_DEBUG("close decode", NULL);
	LOCK_D;
	if (ctx->codec) {
		ctx->codec->close();
		ctx->codec = NULL;
	}
	ctx->decode_running = false;
	UNLOCK_D;
#if LINUX || OSX || FREEBSD
	pthread_join(ctx->decode_thread, NULL);
#endif
	mutex_destroy(ctx->decode.mutex);
}

/*---------------------------------------------------------------------------*/
void decode_flush(struct thread_ctx_s *ctx) {

	LOG_DEBUG("[%p]: decode flush", ctx);
	LOCK_D;
	ctx->decode.state = DECODE_STOPPED;
	IF_PROCESS(
		process_flush();
	);
	UNLOCK_D;
}

/*---------------------------------------------------------------------------*/
unsigned decode_newstream(unsigned sample_rate, unsigned supported_rates[], struct thread_ctx_s *ctx) {
	// called with O locked to get sample rate for potentially processed output stream
	// release O mutex during process_newstream as it can take some time

	MAY_PROCESS(
		if (ctx->decode.process) {
			UNLOCK_O;
			sample_rate = ctx->process_newstream(&ctx->decode.direct, sample_rate, supported_rates);
			LOCK_O;
		}
	);

	return sample_rate;
}

/*---------------------------------------------------------------------------*/
void codec_open(u8_t format, u8_t sample_size, u8_t sample_rate, u8_t channels, u8_t endianness, struct thread_ctx_s *ctx) {
	int i;

	LOG_DEBUG("codec open: '%c'", format);

	LOCK_D;

	ctx->decode.new_stream = true;
	ctx->decode.state = DECODE_STOPPED;

	MAY_PROCESS(
		ctx->decode.direct = true; // potentially changed within codec when processing enabled
	);

	if (ctx->config.mode != SQ_FULL) {
		UNLOCK_D;
		return;
    }

	// find the required codec
	for (i = 0; i < MAX_CODECS; ++i) {

		if (codecs[i] && codecs[i]->id == format) {

			if (ctx->codec && ctx->codec != codecs[i]) {
				LOG_DEBUG("closing codec: '%c'", ctx->codec->id);
				ctx->codec->close();
			}

			ctx->codec = codecs[i];

			ctx->codec->open(sample_size, sample_rate, channels, endianness);

			UNLOCK_D;
			return;
		}
	}

	UNLOCK_D;

	LOG_ERROR("codec not found", NULL);
}

