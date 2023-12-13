/*
 *  Cache - crude caching solution for HTTP
 *
 *	(c) Philippe, philippe_44@outlook.com
 *
 *  This software is released under the MIT License.
 *  https://opensource.org/licenses/MIT
 *
 */

#pragma once

#include <stdint.h>
#include <stdio.h>
#include <stdbool.h>

#include "platform.h"

/****************************************************************************************
 * Base buffer
 */

typedef struct cache_buffer_s {
	// public
	size_t total, size;
	uint8_t* buffer;
	bool infinite;
	enum cache_type_e { CACHE_RING, CACHE_INFINITE, CACHE_FILE } type;

	/* the private part should be a ptr to an anonymous struct but as it does 
	 * not contain anything that drag exotic include files into client, we'll 
	 * leave it like that for now */
	union {
		struct {
			FILE* fd;
			size_t read_offset;
		} file;
		struct {
			uint8_t* read_p, * write_p, * wrap;
		} ring;
	};

	size_t (*pending)(struct cache_buffer_s* self);
	size_t (*level)(struct cache_buffer_s* self);
	ssize_t (*scope)(struct cache_buffer_s* self, size_t offset);
	size_t(*read)(struct cache_buffer_s* self, uint8_t* dst, size_t size, size_t min);
	uint8_t* (*read_inner)(struct cache_buffer_s* self, size_t* size);
	void (*set_offset)(struct cache_buffer_s* self, size_t offset);
	void (*write)(struct cache_buffer_s* self, const uint8_t* src, size_t size);
	void (*flush)(struct cache_buffer_s* self);
	void (*destruct)(struct cache_buffer_s* self);
} cache_buffer;

// buffer_size is either the memory buffer for RING and INFINITE or the internal buffer for DISK. Leave to 0 for default
cache_buffer* cache_create(enum cache_type_e type, size_t buffer_size);
void cache_delete(cache_buffer* cache);