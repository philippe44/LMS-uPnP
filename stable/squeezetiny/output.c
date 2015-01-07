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

// Common output function
// TODO silencebuf cannot be glocal or shall not be free in _close

#include "squeezelite.h"

static log_level loglevel;
u8_t *silencebuf;

#if DSD
u8_t *silencebuf_dop;
#endif

#define LOCK_O   mutex_lock(ctx->outputbuf->mutex)
#define UNLOCK_O mutex_unlock(ctx->outputbuf->mutex)

// functions starting _* are called with mutex locked

/*---------------------------------------------------------------------------*/
frames_t _output_frames(frames_t avail, struct thread_ctx_s *ctx) {

	frames_t frames, size;
	bool silence;

	s32_t cross_gain_in = 0, cross_gain_out = 0; s32_t *cross_ptr = NULL;

	s32_t gainL = ctx->output.current_replay_gain ? gain(ctx->output.gainL, ctx->output.current_replay_gain) : ctx->output.gainL;
	s32_t gainR = ctx->output.current_replay_gain ? gain(ctx->output.gainR, ctx->output.current_replay_gain) : ctx->output.gainR;

	frames = _buf_used(ctx->outputbuf) / BYTES_PER_FRAME;
	silence = false;

	// start when threshold met
	if (ctx->output.state == OUTPUT_BUFFER && frames > ctx->output.threshold * ctx->output.next_sample_rate / 100 && frames > ctx->output.start_frames) {
		ctx->output.state = OUTPUT_RUNNING;
		LOG_INFO("start buffer frames: %u", frames);
		wake_controller(ctx);
	}

	// skip ahead - consume outputbuf but play nothing
	if (ctx->output.state == OUTPUT_SKIP_FRAMES) {
		if (frames > 0) {
			frames_t skip = min(frames, ctx->output.skip_frames);
			LOG_INFO("skip %u of %u frames", skip, ctx->output.skip_frames);
			frames -= skip;
			ctx->output.frames_played += skip;
			while (skip > 0) {
				frames_t cont_frames = min(skip, _buf_cont_read(ctx->outputbuf) / BYTES_PER_FRAME);
				skip -= cont_frames;
				_buf_inc_readp(ctx->outputbuf, cont_frames * BYTES_PER_FRAME);
			}
		}
		ctx->output.state = OUTPUT_RUNNING;
	}

	// pause frames - play silence for required frames
	if (ctx->output.state == OUTPUT_PAUSE_FRAMES) {
		LOG_INFO("pause %u frames", ctx->output.pause_frames);
		if (ctx->output.pause_frames == 0) {
			ctx->output.state = OUTPUT_RUNNING;
		} else {
			silence = true;
			frames = min(avail, ctx->output.pause_frames);
			frames = min(frames, MAX_SILENCE_FRAMES);
			ctx->output.pause_frames -= frames;
		}
	}

	// start at - play silence until jiffies reached
	if (ctx->output.state == OUTPUT_START_AT) {
		u32_t now = gettime_ms();
		if (now >= ctx->output.start_at || ctx->output.start_at > now + 10000) {
			ctx->output.state = OUTPUT_RUNNING;
		} else {
			u32_t delta_frames = (ctx->output.start_at - now) * ctx->output.current_sample_rate / 1000;
			silence = true;
			frames = min(avail, delta_frames);
			frames = min(frames, MAX_SILENCE_FRAMES);
		}
	}

	// play silence if buffering or no frames
	if (ctx->output.state <= OUTPUT_BUFFER || frames == 0) {
		silence = true;
		frames = min(avail, MAX_SILENCE_FRAMES);
	}

	LOG_SDEBUG("avail: %d frames: %d silence: %d", avail, frames, silence);
	frames = min(frames, avail);
	size = frames;

	while (size > 0) {
		frames_t out_frames;
		frames_t cont_frames = _buf_cont_read(ctx->outputbuf) / BYTES_PER_FRAME;
		int wrote;

		if (ctx->output.track_start && !silence) {
			if (ctx->output.track_start == ctx->outputbuf->readp) {
				unsigned delay = 0;
				if (ctx->output.current_sample_rate != ctx->output.next_sample_rate) {
					delay = ctx->output.rate_delay;
				}
				IF_DSD(
				   if (ctx->output.dop != ctx->output.next_dop) {
					   delay = ctx->output.dop_delay;
				   }
				)
				frames -= size;
				// add silence delay in two halves, before and after track start on rate or pcm-dop change
				if (delay) {
					ctx->output.state = OUTPUT_PAUSE_FRAMES;
					if (!ctx->output.delay_active) {
						ctx->output.pause_frames = ctx->output.current_sample_rate * delay / 2000;
						ctx->output.delay_active = true;  // first delay - don't process track start
						break;
					} else {
						ctx->output.pause_frames = ctx->output.next_sample_rate * delay / 2000;
						ctx->output.delay_active = false; // second delay - process track start
					}
				}
				LOG_INFO("track start sample rate: %u replay_gain: %u", ctx->output.next_sample_rate, ctx->output.next_replay_gain);
				ctx->output.frames_played = 0;
				ctx->output.track_started = true;
				ctx->output.track_start_time = gettime_ms();
				ctx->output.current_sample_rate = ctx->output.next_sample_rate;
				IF_DSD(
				   ctx->output.dop = ctx->output.next_dop;
				)
				if (!ctx->output.fade == FADE_ACTIVE || !ctx->output.fade_mode == FADE_CROSSFADE) {
					ctx->output.current_replay_gain = ctx->output.next_replay_gain;
				}
				ctx->output.track_start = NULL;
				break;
			} else if (ctx->output.track_start > ctx->outputbuf->readp) {
				// reduce cont_frames so we find the next track start at beginning of next chunk
				cont_frames = min(cont_frames, (ctx->output.track_start - ctx->outputbuf->readp) / BYTES_PER_FRAME);
			}
		}

		IF_DSD(
			if (ctx->output.dop) {
				gainL = gainR = FIXED_ONE;
			}
		)

		if (ctx->output.fade && !silence) {
			if (ctx->output.fade == FADE_DUE) {
				if (ctx->output.fade_start == ctx->outputbuf->readp) {
					LOG_INFO("fade start reached", NULL);
					ctx->output.fade = FADE_ACTIVE;
				} else if (ctx->output.fade_start > ctx->outputbuf->readp) {
					cont_frames = min(cont_frames, (ctx->output.fade_start - ctx->outputbuf->readp) / BYTES_PER_FRAME);
				}
			}
			if (ctx->output.fade == FADE_ACTIVE) {
				// find position within fade
				frames_t cur_f = ctx->outputbuf->readp >= ctx->output.fade_start ? (ctx->outputbuf->readp - ctx->output.fade_start) / BYTES_PER_FRAME :
					(ctx->outputbuf->readp + ctx->outputbuf->size - ctx->output.fade_start) / BYTES_PER_FRAME;
				frames_t dur_f = ctx->output.fade_end >= ctx->output.fade_start ? (ctx->output.fade_end - ctx->output.fade_start) / BYTES_PER_FRAME :
					(ctx->output.fade_end + ctx->outputbuf->size - ctx->output.fade_start) / BYTES_PER_FRAME;
				if (cur_f >= dur_f) {
					if (ctx->output.fade_mode == FADE_INOUT && ctx->output.fade_dir == FADE_DOWN) {
						LOG_INFO("fade down complete, starting fade up", NULL);
						ctx->output.fade_dir = FADE_UP;
						ctx->output.fade_start = ctx->outputbuf->readp;
						ctx->output.fade_end = ctx->outputbuf->readp + dur_f * BYTES_PER_FRAME;
						if (ctx->output.fade_end >= ctx->outputbuf->wrap) {
							ctx->output.fade_end -= ctx->outputbuf->size;
						}
						cur_f = 0;
					} else if (ctx->output.fade_mode == FADE_CROSSFADE) {
						LOG_INFO("crossfade complete", NULL);
						if (_buf_used(ctx->outputbuf) >= dur_f * BYTES_PER_FRAME) {
							_buf_inc_readp(ctx->outputbuf, dur_f * BYTES_PER_FRAME);
							LOG_INFO("skipped crossfaded start", NULL);
						} else {
							LOG_WARN("unable to skip crossfaded start", NULL);
						}
						ctx->output.fade = FADE_INACTIVE;
						ctx->output.current_replay_gain = ctx->output.next_replay_gain;
					} else {
						LOG_INFO("fade complete", NULL);
						ctx->output.fade = FADE_INACTIVE;
					}
				}
				// if fade in progress set fade gain, ensure cont_frames reduced so we get to end of fade at start of chunk
				if (ctx->output.fade) {
					if (ctx->output.fade_end > ctx->outputbuf->readp) {
						cont_frames = min(cont_frames, (ctx->output.fade_end - ctx->outputbuf->readp) / BYTES_PER_FRAME);
					}
					if (ctx->output.fade_dir == FADE_UP || ctx->output.fade_dir == FADE_DOWN) {
						// fade in, in-out, out handled via altering standard gain
						s32_t fade_gain;
						if (ctx->output.fade_dir == FADE_DOWN) {
							cur_f = dur_f - cur_f;
						}
						fade_gain = to_gain((float)cur_f / (float)dur_f);
						gainL = gain(gainL, fade_gain);
						gainR = gain(gainR, fade_gain);
					}
					if (ctx->output.fade_dir == FADE_CROSS) {
						// cross fade requires special treatment - performed later based on these values
						// support different replay gain for old and new track by retaining old value until crossfade completes
						if (_buf_used(ctx->outputbuf) / BYTES_PER_FRAME > dur_f + size) {
							cross_gain_in  = to_gain((float)cur_f / (float)dur_f);
							cross_gain_out = FIXED_ONE - cross_gain_in;
							if (ctx->output.current_replay_gain) {
								cross_gain_out = gain(cross_gain_out, ctx->output.current_replay_gain);
							}
							if (ctx->output.next_replay_gain) {
								cross_gain_in = gain(cross_gain_in, ctx->output.next_replay_gain);
							}
							gainL = ctx->output.gainL;
							gainR = ctx->output.gainR;
							cross_ptr = (s32_t *)(ctx->output.fade_end + cur_f * BYTES_PER_FRAME);
						} else {
							LOG_INFO("unable to continue crossfade - too few samples", NULL);
							ctx->output.fade = FADE_INACTIVE;
						}
					}
				}
			}
		}

		out_frames = !silence ? min(size, cont_frames) : size;

		wrote = ctx->output.write_cb(out_frames, silence, gainL, gainR, cross_gain_in, cross_gain_out, &cross_ptr);

		if (wrote <= 0) {
			frames -= size;
			break;
		} else {
			out_frames = (frames_t)wrote;
		}

		size -= out_frames;

		if (!silence) {
			_buf_inc_readp(ctx->outputbuf, out_frames * BYTES_PER_FRAME);
			ctx->output.frames_played += out_frames;
		}
	}

	LOG_SDEBUG("wrote %u frames", frames);

	return frames;
}

/*---------------------------------------------------------------------------*/
void _checkfade(bool start, struct thread_ctx_s *ctx) {
	frames_t bytes;

	LOG_INFO("fade mode: %u duration: %u %s", ctx->output.fade_mode, ctx->output.fade_secs, start ? "track-start" : "track-end");

	bytes = ctx->output.next_sample_rate * BYTES_PER_FRAME * ctx->output.fade_secs;
	if (ctx->output.fade_mode == FADE_INOUT) {
		bytes /= 2;
	}

	if (start && (ctx->output.fade_mode == FADE_IN || (ctx->output.fade_mode == FADE_INOUT && _buf_used(ctx->outputbuf) == 0))) {
		bytes = min(bytes, ctx->outputbuf->size - BYTES_PER_FRAME); // shorter than full buffer otherwise start and end align
		LOG_INFO("fade IN: %u frames", bytes / BYTES_PER_FRAME);
		ctx->output.fade = FADE_DUE;
		ctx->output.fade_dir = FADE_UP;
		ctx->output.fade_start = ctx->outputbuf->writep;
		ctx->output.fade_end = ctx->output.fade_start + bytes;
		if (ctx->output.fade_end >= ctx->outputbuf->wrap) {
			ctx->output.fade_end -= ctx->outputbuf->size;
		}
	}

	if (!start && (ctx->output.fade_mode == FADE_OUT || ctx->output.fade_mode == FADE_INOUT)) {
		bytes = min(_buf_used(ctx->outputbuf), bytes);
		LOG_INFO("fade %s: %u frames", ctx->output.fade_mode == FADE_INOUT ? "IN-OUT" : "OUT", bytes / BYTES_PER_FRAME);
		ctx->output.fade = FADE_DUE;
		ctx->output.fade_dir = FADE_DOWN;
		ctx->output.fade_start = ctx->outputbuf->writep - bytes;
		if (ctx->output.fade_start < ctx->outputbuf->buf) {
			ctx->output.fade_start += ctx->outputbuf->size;
		}
		ctx->output.fade_end = ctx->outputbuf->writep;
	}

	if (start && ctx->output.fade_mode == FADE_CROSSFADE) {
		if (_buf_used(ctx->outputbuf) != 0) {
			if (ctx->output.next_sample_rate != ctx->output.current_sample_rate) {
				LOG_INFO("crossfade disabled as sample rates differ", NULL);
				return;
			}
			bytes = min(bytes, _buf_used(ctx->outputbuf));               // max of current remaining samples from previous track
			bytes = min(bytes, (frames_t)(ctx->outputbuf->size * 0.9));  // max of 90% of outputbuf as we consume additional buffer during crossfade
			LOG_INFO("CROSSFADE: %u frames", bytes / BYTES_PER_FRAME);
			ctx->output.fade = FADE_DUE;
			ctx->output.fade_dir = FADE_CROSS;
			ctx->output.fade_start = ctx->outputbuf->writep - bytes;
			if (ctx->output.fade_start < ctx->outputbuf->buf) {
				ctx->output.fade_start += ctx->outputbuf->size;
			}
			ctx->output.fade_end = ctx->outputbuf->writep;
			ctx->output.track_start = ctx->output.fade_start;
		} else if (ctx->outputbuf->size == OUTPUTBUF_SIZE && ctx->outputbuf->readp == ctx->outputbuf->buf) {
			// if default setting used and nothing in buffer attempt to resize to provide full crossfade support
			LOG_INFO("resize outputbuf for crossfade", NULL);
			_buf_resize(ctx->outputbuf, OUTPUTBUF_SIZE_CROSSFADE);
#if LINUX || FREEBSD
			touch_memory(ctx->outputbuf->buf, ctx->outputbuf->size);
#endif
		}
	}
}


/*---------------------------------------------------------------------------*/
void output_loglevel(log_level level) {
	LOG_ERROR("output change loglevel %d", level);
	loglevel = level;
}

/*---------------------------------------------------------------------------*/
void output_init_common(char *device, unsigned output_buf_size, unsigned rates[], struct thread_ctx_s *ctx) {
	unsigned i;

	ctx->outputbuf = &ctx->__o_buf;

	output_buf_size = output_buf_size - (output_buf_size % BYTES_PER_FRAME);
	LOG_DEBUG("outputbuf size: %u", output_buf_size);

	buf_init(ctx->outputbuf, output_buf_size);
	if (!ctx->outputbuf->buf) {
		LOG_ERROR("unable to malloc output buffer", NULL);
		exit(0);
	}

	silencebuf = malloc(MAX_SILENCE_FRAMES * BYTES_PER_FRAME);
	if (!silencebuf) {
		LOG_ERROR("unable to malloc silence buffer", NULL);
		exit(0);
	}
	memset(silencebuf, 0, MAX_SILENCE_FRAMES * BYTES_PER_FRAME);

	IF_DSD(
		silencebuf_dop = malloc(MAX_SILENCE_FRAMES * BYTES_PER_FRAME);
		if (!silencebuf_dop) {
			LOG_ERROR("unable to malloc silence dop buffer");
			exit(0);
		}
		dop_silence_frames((u32_t *)silencebuf_dop, MAX_SILENCE_FRAMES);
	)

	ctx->output.state = OUTPUT_STOPPED;
	ctx->output.device = device;
	ctx->output.fade = FADE_INACTIVE;
	ctx->output.error_opening = false;

	for (i = 0; i < MAX_SUPPORTED_SAMPLERATES; ++i) {
		ctx->output.supported_rates[i] = rates[i];
	}

	// set initial sample rate, preferring 44100
	for (i = 0; i < MAX_SUPPORTED_SAMPLERATES; ++i) {
		if (ctx->output.supported_rates[i] == 44100) {
			ctx->output.default_sample_rate = 44100;
			break;
		}
	}
	if (!ctx->output.default_sample_rate) {
		ctx->output.default_sample_rate = rates[0];
	}

	ctx->output.current_sample_rate = ctx->output.default_sample_rate;

	if (loglevel >= lINFO) {
		char rates_buf[10 * MAX_SUPPORTED_SAMPLERATES] = "";
		for (i = 0; ctx->output.supported_rates[i]; ++i) {
			char s[10];
			sprintf(s, "%d ", ctx->output.supported_rates[i]);
			strcat(rates_buf, s);
		}
		LOG_INFO("supported rates: %s", rates_buf);
	}
}

/*---------------------------------------------------------------------------*/
void output_close_common(struct thread_ctx_s *ctx) {
	buf_destroy(ctx->outputbuf);
	free(silencebuf);
	IF_DSD(
		free(silencebuf_dop);
	)
}

/*---------------------------------------------------------------------------*/
void output_flush(struct thread_ctx_s *ctx) {
	LOG_INFO("flush output buffer", NULL);
	buf_flush(ctx->outputbuf);
	LOCK_O;
	ctx->output.fade = FADE_INACTIVE;
	if (ctx->output.state != OUTPUT_OFF) {
		ctx->output.state = OUTPUT_STOPPED;
		if (ctx->output.error_opening) {
			ctx->output.current_sample_rate = ctx->output.default_sample_rate;
		}
		ctx->output.delay_active = false;
	}
	ctx->output.frames_played = ctx->output.frames_played_dmp = 0;
	UNLOCK_O;
}
