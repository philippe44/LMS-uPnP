/*
 *  Cache - crude caching solution for HTTP
 *
 *	(c) Philippe, philippe_44@outlook.com
 *
 *  This software is released under the MIT License.
 *  https://opensource.org/licenses/MIT
 *
 */

#include <stdlib.h>
#include <string.h>

#include "platform.h"
#include "cache.h"

static bool ring_construct(cache_buffer* self);
static bool file_construct(cache_buffer* self);

cache_buffer* cache_create(enum cache_type_e type, size_t buffer_size) {
	bool success;

	cache_buffer* cache = calloc(sizeof(cache_buffer), 1);
	cache->type = type;
	cache->size = buffer_size;
	cache->infinite = cache->type != CACHE_RING;

	if (type == CACHE_FILE) success = file_construct(cache);
	else success = ring_construct(cache);

	if (success && !cache->buffer) {
		cache->buffer = malloc(cache->size);
		if (!cache->buffer) success = false;
	}

	if (!success) {
		free(cache);
		cache = NULL;
	}

	return cache;;
}

void cache_delete(cache_buffer* cache) {
	cache->destruct(cache);
	free(cache->buffer);
	free(cache);
}

/****************************************************************************************
 * Ring buffer
 */

static void ring_destruct(cache_buffer* self) { }
static size_t ring_level(cache_buffer* self) { return self->total < self->size ? self->total : self->size - 1; }
static void ring_flush(cache_buffer* self) { self->ring.read_p = self->ring.write_p = self->buffer; self->total = 0; }

static size_t ring_pending(cache_buffer* self) { 
	return self->ring.write_p >= self->ring.read_p ? 
		   self->ring.write_p - self->ring.read_p : 
		   self->ring.wrap - self->ring.read_p; 
}

static ssize_t ring_scope(cache_buffer* self, size_t offset) { 
	if (offset >= self->total) return offset - self->total + 1;
	else if (offset >= self->total - self->level(self)) return 0;
	else return offset - self->total + self->level(self);
}

static void ring_set_offset(cache_buffer* self, size_t offset) {
	if (offset >= self->total) self->ring.read_p = self->ring.write_p;
	else if (offset < self->total - self->level(self)) self->ring.read_p = (self->ring.write_p + 1) == self->ring.wrap ? self->buffer : self->ring.write_p + 1;
	else self->ring.read_p = self->buffer + offset % self->size;
}

static size_t ring_read(cache_buffer* self, uint8_t* dst, size_t size, size_t min) {
	size = min(size, self->pending(self));
	if (size < min) return 0;

	size_t cont = min(size, (size_t)(self->ring.wrap - self->ring.read_p));
	memcpy(dst, self->ring.read_p, cont);
	memcpy(dst + cont, self->buffer, size - cont);

	self->ring.read_p += size;
	if (self->ring.read_p >= self->ring.wrap) self->ring.read_p -= self->size;
	return size;
}

static uint8_t* ring_read_inner(cache_buffer* self, size_t* size) {
	// caller *must* consume ALL data
	*size = min(*size, self->pending(self));
	*size = min(*size, (size_t)(self->ring.wrap - self->ring.read_p));

	uint8_t* p = self->ring.read_p;
	self->ring.read_p += *size;
	if (self->ring.read_p >= self->ring.wrap) self->ring.read_p -= self->size;

	return *size ? p : NULL;
}

static void ring_write(cache_buffer* self, const uint8_t* src, size_t size) {
	size_t cont = min(size, (size_t)(self->ring.wrap - self->ring.write_p));
	memcpy(self->ring.write_p, src, cont);
	memcpy(self->buffer, src + cont, size - cont);

	self->ring.write_p += size;
	self->total += size;

	if (self->ring.write_p >= self->ring.wrap) self->ring.write_p -= self->size;
	if (self->level(self) == self->size - 1) self->ring.read_p = self->ring.write_p + 1 == self->ring.wrap ? self->ring.wrap : self->ring.write_p + 1;
}

static bool ring_construct(cache_buffer* self) {
	if (!self->size) self->size = 8 * 1024 * 1024;

	self->buffer = malloc(self->size);
	if (!self->buffer) return false;

	self->ring.read_p = self->ring.write_p = self->buffer;
	self->ring.wrap = self->buffer + self->size;

	self->pending = ring_pending;
	self->scope = ring_scope;
	self->level = ring_level;
	self->read = ring_read;
	self->read_inner = ring_read_inner;
	self->set_offset= ring_set_offset;
	self->write = ring_write;
	self->flush = ring_flush;
	self->destruct = ring_destruct;

	return true;
}

/****************************************************************************************
 * File buffer
 */

static void file_destruct(cache_buffer* self) { if (self->file.fd) fclose(self->file.fd); }
static size_t file_pending(cache_buffer *self) { return self->total - self->file.read_offset; }
static size_t file_level(cache_buffer* self) { return self->total; }
static void file_flush(cache_buffer* self) { self->file.read_offset = self->total = 0; }
static ssize_t file_scope(cache_buffer* self, size_t offset) { return offset >= self->total ? offset - self->total + 1 : 0; }
static void file_set_offset(cache_buffer* self, size_t offset) { self->file.read_offset = offset; }

static size_t file_read(cache_buffer* self, uint8_t* dst, size_t size, size_t min) {
	size = min(self->size, self->total);
	if (size < min) return 0;

	fseek(self->file.fd, self->file.read_offset, SEEK_SET);
	size_t bytes = fread(dst, 1, size, self->file.fd);
	self->file.read_offset += bytes;

	return bytes;
}

static uint8_t* file_read_inner(cache_buffer* self, size_t* size) {
	if (*size > self->size) {
		free(self->buffer);
		self->buffer = malloc(*size);
		self->size = *size;
	}

	// caller *must* consume ALL data
	*size = min(*size, self->total);

	fseek(self->file.fd, self->file.read_offset, SEEK_SET);
	*size = fread(self->buffer, 1, *size, self->file.fd);
	self->file.read_offset += *size;

	return *size ? self->buffer : NULL;
}

static void file_write(cache_buffer* self, const uint8_t* src, size_t size) {
	fseek(self->file.fd, 0, SEEK_END);
	fwrite(src, 1, size, self->file.fd);
	self->total += size;
}

static bool file_construct(cache_buffer* self) {
	if (!self->size) self->size = 128 * 1024;
	self->file.fd = tmpfile();
	if (!self->file.fd) return false;

	self->pending = file_pending;
	self->scope = file_scope;
	self->level = file_level;
	self->read = file_read;
	self->read_inner = file_read_inner;
	self->set_offset = file_set_offset;
	self->write = file_write;
	self->flush = file_flush;
	self->destruct = file_destruct;

	return true;
}

