/*
 *  Slimproto bridge
 *
 *  (c) Adrian Smith 2012-2014, triode1@btinternet.com
 *	(c) Philippe, philippe_44@outlook.com
 *
 *  See LICENSE
 *
 */

#pragma once

#include <limits.h>
#include "squeezedefs.h"
#include "metadata.h"

#define BRIDGE_URL	 	"bridge-"

#define OUTPUTBUF_SIZE	(4*1024*1024)
#define STREAMBUF_SIZE	(1024*1024)

typedef enum {SQ_NONE, SQ_SET_TRACK, SQ_PLAY, SQ_TRANSITION, SQ_PAUSE, SQ_UNPAUSE,
			  SQ_STOP, SQ_VOLUME, SQ_MUTE, SQ_TIME, SQ_TRACK_INFO, SQ_ONOFF, SQ_NEW_METADATA,
			  SQ_NEXT, SQ_SETNAME, SQ_SETSERVER, SQ_BATTERY, SQ_NEXT_FAILED, SQ_FLUSH} sq_action_t;
typedef enum { ICY_NONE, ICY_FULL, ICY_TEXT } sq_icy_e;
typedef enum { L24_PACKED, L24_PACKED_LPCM, L24_TRUNC16, L24_TRUNC16_PCM, L24_UNPACKED_HIGH, L24_UNPACKED_LOW } sq_L24_pack_t;
typedef enum { FLAC_NO_HEADER = 0, FLAC_DEFAULT_HEADER = 1, FLAC_MAX_HEADER = 2, FLAC_ADJUST_HEADER } sq_flac_header_t;

typedef	sq_action_t sq_event_t;
typedef	int	sq_dev_handle_t;

// make sure compiler uses 64 bits for these
#define HTTP_LENGTH_NONE	((int64_t) -1) 
#define HTTP_LENGTH_IFKNOWN ((int64_t) -2) 
#define HTTP_LENGTH_CHUNKED ((int64_t) -3) 
#define HTTP_LENGTH_LARGE   ((int64_t) UINT32_MAX - 8192)

typedef	struct sq_dev_param_s {
	int64_t		stream_length;
	unsigned 	streambuf_size;
	unsigned 	outputbuf_size;
	char		codecs[STR_LEN];
	char		mode[STR_LEN];
	enum { HTTP_CACHE_MEMORY = 0, HTTP_CACHE_INFINITE = 1, HTTP_CACHE_DISK = 2 } cache;
	bool		force_aac;
	int         next_delay;
	char 		raw_audio_format[STR_LEN];
	char		server[STR_LEN];
	uint32_t 	sample_rate;
	sq_L24_pack_t		L24_format;
	sq_flac_header_t	flac_header;
	char		name[STR_LEN];
	uint8_t		mac[6];
#ifdef RESAMPLE
	char		resample_options[STR_LEN];
#endif
	bool		roon_mode;
	char		store_prefix[STR_LEN];
	char		coverart[STR_LEN];
	// set at runtime, not from config
	struct {
		bool	 use_cli;
		char 	 set_server[STR_LEN];
		sq_icy_e send_icy;
	};
} sq_dev_param_t;

struct track_param
{
	unsigned	index;
	metadata_t	metadata;
	char		uri[STR_LEN];
	char		mimetype[STR_LEN];
	char		format;
	bool		flow;
};

typedef bool (*sq_callback_t)(void *caller, sq_action_t action, ...);

void				sq_init(struct in_addr host, uint16_t port, char *model_name);
void				sq_stop(void);

// only name cannot be NULL
bool			 	sq_run_device(sq_dev_handle_t handle, sq_dev_param_t *param);
void				sq_delete_device(sq_dev_handle_t);
sq_dev_handle_t		sq_reserve_device(void *caller_id, bool on, char *mimecaps[], sq_callback_t callback);
void				sq_release_device(sq_dev_handle_t);

void				sq_notify(sq_dev_handle_t handle, sq_event_t event, ...);
uint32_t			sq_get_time(sq_dev_handle_t handle);
uint32_t			sq_self_time(sq_dev_handle_t handle);
uint32_t			sq_get_metadata(sq_dev_handle_t handle, struct metadata_s *metadata, int token);
void				sq_default_metadata(struct metadata_s *metadata, bool init);
void 				sq_free_metadata(struct metadata_s *metadata);
bool 				sq_set_time(sq_dev_handle_t handle, char *pos);
bool				sq_close(void *desc);
bool 				sq_is_remote(const char *urn);
void*				sq_get_ptr(sq_dev_handle_t handle);
bool				sq_icy_active(sq_dev_handle_t handle);
