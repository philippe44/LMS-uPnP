/*
 *  Squeeze2xxx - squeezebox emulator gateway
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

#ifndef __SQUEEZEITF_H
#define __SQUEEZEITF_H

#include "squeezedefs.h"
#include "util_common.h"

#define MAX_FILE_SIZE 	0xffff0000
#define	MAX_MIMETYPES 	128
#define BRIDGE_URL	 	"bridge-"

typedef enum {SQ_NONE, SQ_SET_TRACK, SQ_PLAY, SQ_TRANSITION, SQ_PAUSE, SQ_UNPAUSE,
			  SQ_STOP, SQ_SEEK, SQ_VOLUME, SQ_TIME, SQ_TRACK_INFO, SQ_ONOFF,
			  SQ_NEXT, SQ_SETNAME, SQ_SETSERVER} sq_action_t;
typedef enum {SQ_STREAM = 2, SQ_FULL = 3} sq_mode_t;
typedef	sq_action_t sq_event_t;

typedef enum { SQ_RATE_384000 = 384000, SQ_RATE_352000 = 352000,
			   SQ_RATE_192000 = 192000, SQ_RATE_176400 = 176400,
			   SQ_RATE_96000 = 96000, SQ_RATE_48000 = 48000, SQ_RATE_44100 = 44100,
			   SQ_RATE_32000 = 32000, SQ_RATE_24000 = 24000, SQ_RATE_22500 = 22500,
			   SQ_RATE_16000 = 16000, SQ_RATE_12000 = 12000, SQ_RATE_11025 = 11025,
			   SQ_RATE_8000 = 8000, SQ_RATE_DEFAULT = 0} sq_rate_e;
typedef enum { L24_PACKED, L24_PACKED_LPCM, L24_TRUNC16, L24_TRUNC16_PCM, L24_UNPACKED_HIGH, L24_UNPACKED_LOW } sq_L24_pack_t;
typedef enum { FLAC_NO_HEADER = 0, FLAC_NORMAL_HEADER = 1, FLAC_FULL_HEADER = 2 } sq_flac_header_t;
typedef	int	sq_dev_handle_t;
typedef unsigned sq_rate_t;

typedef struct metadata_s {
	char *artist;
	char *album;
	char *title;
	char *genre;
	char *artwork;
	char *remote_title;
	u32_t index;
	u32_t track;
	u32_t duration;
	u32_t file_size;
	bool  remote;
} metadata_t;

typedef	struct sq_dev_param_s {
	enum { HTTP_INFINITE = -1, HTTP_CHUNKED = -3, HTTP_LARGE = MAX_FILE_SIZE } stream_length;
	unsigned 	stream_buf_size;
	unsigned 	output_buf_size;
	char		codecs[_STR_LEN_];
	char		encode[8];
	char 		raw_audio_format[_STR_LEN_];
	char		server[_STR_LEN_];
	sq_rate_e	sample_rate;
	sq_L24_pack_t		L24_format;
	sq_flac_header_t	flac_header;
	char		name[_STR_LEN_];
	u8_t		mac[6];
	bool		send_icy;
	// set at runtime, not from config
	struct {
		bool		use_cli;
		char 	server[_STR_LEN_];
	} dynamic;
} sq_dev_param_t;


struct track_param
{
	bool 		next;
	metadata_t	metadata;
	char		uri[_STR_LEN_];
	char		mimetype[_STR_LEN_];
};

typedef bool (*sq_callback_t)(sq_dev_handle_t handle, void *caller_id, sq_action_t action, u8_t *cookie, void *param);

void				sq_init(char *ip, u16_t port);
void				sq_stop(void);

// only name cannot be NULL
bool			 	sq_run_device(sq_dev_handle_t handle, sq_dev_param_t *param);
void				sq_delete_device(sq_dev_handle_t);
sq_dev_handle_t		sq_reserve_device(void *caller_id, bool on, char *mimecaps[], sq_callback_t callback);
void				sq_release_device(sq_dev_handle_t);

void				sq_notify(sq_dev_handle_t handle, void *caller_id, sq_event_t event, u8_t *cookie, void *param);
u32_t 				sq_get_time(sq_dev_handle_t handle);
u32_t 				sq_self_time(sq_dev_handle_t handle);
bool				sq_get_metadata(sq_dev_handle_t handle, struct metadata_s *metadata, bool next);
void				sq_default_metadata(struct metadata_s *metadata, bool init);
void 				sq_free_metadata(struct metadata_s *metadata);
bool 				sq_set_time(sq_dev_handle_t handle, char *pos);
bool				sq_close(void *desc);
bool 				sq_is_remote(const char *urn);
void*				sq_get_ptr(sq_dev_handle_t handle);

#endif

