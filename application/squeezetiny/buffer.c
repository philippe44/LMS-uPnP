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

// fifo bufffers

#include "squeezelite.h"

// _* called with muxtex locked


bool _buf_wrap(struct buffer *buf) {
	return buf->writep <= buf->readp ? true : false;
}

unsigned _buf_used(struct buffer *buf) {
	return buf->writep >= buf->readp ? buf->writep - buf->readp : buf->size - (buf->readp - buf->writep);
}

unsigned _buf_space(struct buffer *buf) {
	return buf->size - _buf_used(buf) - 1; // reduce by one as full same as empty otherwise
}

unsigned _buf_cont_read(struct buffer *buf) {
	return buf->writep >= buf->readp ? buf->writep - buf->readp : buf->wrap - buf->readp;
}

unsigned _buf_cont_write(struct buffer *buf) {
	return buf->writep >= buf->readp ? buf->wrap - buf->writep : buf->readp - buf->writep;
}

unsigned _buf_size(struct buffer *buf) {
	return buf->size;
}

void *_buf_readp(struct buffer *buf) {
	return buf->readp;
}

void _buf_inc_readp(struct buffer *buf, unsigned by) {
	buf->readp += by;
	if (buf->readp >= buf->wrap) {
		buf->readp -= buf->size;
	}
}

void _buf_inc_writep(struct buffer *buf, unsigned by) {
	buf->writep += by;
	if (buf->writep >= buf->wrap) {
		buf->writep -= buf->size;
	}
}

void buf_flush(struct buffer *buf) {
	mutex_lock(buf->mutex);
	buf->readp  = buf->buf;
	buf->writep = buf->buf;
	mutex_unlock(buf->mutex);
}

bool _buf_reset(struct buffer *buf) {
	if (buf->readp != buf->writep) return false;
	buf->readp  = buf->buf;
	buf->writep = buf->buf;
	return true;
}

// adjust buffer to multiple of mod bytes so reading in multiple always wraps on frame boundary
void buf_adjust(struct buffer *buf, size_t mod) {
	size_t size;
	mutex_lock(buf->mutex);
	size = ((unsigned)(buf->base_size / mod)) * mod;
	buf->readp  = buf->buf;
	buf->writep = buf->buf;
	buf->wrap   = buf->buf + size;
	buf->size   = size;
	mutex_unlock(buf->mutex);
}

// called with mutex locked to resize, does not retain contents, reverts to original size if fails
void _buf_resize(struct buffer *buf, size_t size) {
	if (buf->size == size) return;
	free(buf->buf);
	buf->buf = malloc(size);
	if (!buf->buf) {
		size    = buf->size;
		buf->buf= malloc(size);
		if (!buf->buf) {
			size = 0;
		}
	}
	buf->readp  = buf->buf;
	buf->writep = buf->buf;
	buf->wrap   = buf->buf + size;
	buf->size   = size;
	buf->base_size = size;
}

void buf_init(struct buffer *buf, size_t size) {
	buf->buf    = malloc(size);
	buf->readp  = buf->buf;
	buf->writep = buf->buf;
	buf->wrap   = buf->buf + size;
	buf->size   = size;
	buf->base_size = size;
	mutex_create_p(buf->mutex);
}

void buf_destroy(struct buffer *buf) {
	if (buf->buf) {
		free(buf->buf);
		buf->buf = NULL;
		buf->size = 0;
		buf->base_size = 0;
		mutex_destroy(buf->mutex);
	}
}

unsigned _buf_read(void *dst, struct buffer *src, unsigned size) {
	unsigned low, a_size = min(size, _buf_used(src));

	low = min(a_size, _buf_cont_read(src));
	memcpy(dst, src->readp, low);
	memcpy((u8_t*) dst+low, src->buf, a_size - low);
	_buf_inc_readp(src, a_size);

	return a_size;
}

unsigned _buf_write(struct buffer *buf, void *src, unsigned size) {
	unsigned bytes;

	size = min(size, _buf_space(buf));
	bytes = min(size, _buf_cont_write(buf));
	memcpy(buf->writep, src, bytes);
	memcpy(buf->buf, (u8_t*) src + bytes, size - bytes);
	_buf_inc_writep(buf, size);

	return size;
}


