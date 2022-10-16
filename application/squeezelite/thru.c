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


struct thru {
	u32_t sample_rate;
};

/*---------------------------------------------------------------------------*/
decode_state thru_decode(struct thread_ctx_s *ctx) {
	unsigned int in, out;

	LOCK_S;
	LOCK_O_direct;

	in = min(_buf_used(ctx->streambuf), _buf_cont_read(ctx->streambuf));

	if (ctx->stream.state <= DISCONNECT && in == 0) {
		UNLOCK_O_direct;
		UNLOCK_S;
		return DECODE_COMPLETE;
	}

	if (ctx->decode.new_stream) {
		LOG_INFO("[%p]: setting track_start", ctx);
		ctx->output.track_start = ctx->outputbuf->writep;
		ctx->decode.new_stream = false;
	}

	out = min(_buf_space(ctx->outputbuf), _buf_cont_write(ctx->outputbuf));
	out = min(in, out);

	memcpy(ctx->outputbuf->writep, ctx->streambuf->readp, out);

	_buf_inc_readp(ctx->streambuf, out);
	_buf_inc_writep(ctx->outputbuf, out);

	UNLOCK_O_direct;
	UNLOCK_S;

	return DECODE_RUNNING;
}

/*---------------------------------------------------------------------------*/
static void thru_open(u8_t sample_size, u32_t sample_rate, u8_t	channels, u8_t endianness, struct thread_ctx_s *ctx) {
	struct thru *p = ctx->decode.handle;

	if (!p)	p = ctx->decode.handle = malloc(sizeof(struct thru));

	if (!p) return;

	p->sample_rate = sample_rate;

	LOG_INFO("[%p]: thru codec", ctx);
}

/*---------------------------------------------------------------------------*/
static void thru_close(struct thread_ctx_s *ctx) {
	if (ctx->decode.handle) free(ctx->decode.handle);
	ctx->decode.handle = NULL;
}

/*---------------------------------------------------------------------------*/
struct codec *register_thru(void) {
	static struct codec ret = {
		'*',         // id
		"*",   		 // types
		4096,        // min read
		16*1024,     // min space
		thru_open,   // open
		thru_close,  // close
		thru_decode, // decode
		true,        // thru
	};

	LOG_INFO("using thru", NULL);
	return &ret;
}


void deregister_thru(void) {
}



