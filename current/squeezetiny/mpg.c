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

#include "squeezelite.h"

#include <mpg123.h>

#define READ_SIZE  512
#define WRITE_SIZE 32 * 1024

struct mpg {
	mpg123_handle *h;
	bool use16bit;
};

static struct mpg *m;

extern log_level loglevel;

extern struct buffer *streambuf;
extern struct buffer *outputbuf;
extern struct streamstate stream;
extern struct outputstate output;
extern struct decodestate decode;
extern struct processstate process;

#define LOCK_S   mutex_lock(streambuf->mutex)
#define UNLOCK_S mutex_unlock(streambuf->mutex)
#define LOCK_O   mutex_lock(outputbuf->mutex)
#define UNLOCK_O mutex_unlock(outputbuf->mutex)

#define LOCK_O_not_direct
#define UNLOCK_O_not_direct
#define IF_DIRECT(x)    { x }
#define IF_PROCESS(x)


static decode_state mpg_decode(void) {
	size_t bytes, space;
	int ret;

	LOCK_S;
	LOCK_O;

	bytes = min(_buf_used(streambuf), _buf_cont_read(streambuf));
	space = min(_buf_space(outputbuf), _buf_cont_write(outputbuf));

	bytes = min(bytes, READ_SIZE);
	space = min(space, WRITE_SIZE);
	bytes = min(bytes, space);

	memcpy(outputbuf->writep, streambuf->readp, bytes);

	// is this needed in case of direct copy ?
	if (decode.new_stream) {
		output.next_sample_rate = output.sample_rate
		output.track_start = outputbuf->writep;
		decode.new_stream = false;
	}

	_buf_inc_readp(streambuf, bytes);
	_buf_inc_writep(outputbuf, bytes);

	UNLOCK_O;

	LOG_SDEBUG("writethru %u frames", space / BYTES_PER_FRAME);

	if (bytes == 0 && stream.state <= DISCONNECT) {
		UNLOCK_S;
		LOG_INFO("stream complete", NULL);
		return DECODE_COMPLETE;
	}

	UNLOCK_S;

	// OK and NEED_MORE keep running
	return DECODE_RUNNING;
}
