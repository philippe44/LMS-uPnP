#ifndef __SQUEEZEITF_H
#define __SQUEEZEITF_H

#include "squeezedefs.h"
#include "util_common.h"

#define SQ_STR_LENGTH	256

typedef enum {SQ_NONE, SQ_SETFORMAT, SQ_SETURI, SQ_SETNEXTURI, SQ_PLAY, SQ_PAUSE, SQ_UNPAUSE, SQ_STOP, SQ_SEEK,
			  SQ_VOLUME, SQ_TIME, SQ_TRACK_CHANGE, SQ_ONOFF, SQ_NEXT} sq_action_t;
typedef enum {SQ_STREAM = 2, SQ_FULL = 3} sq_mode_t;
typedef	sq_action_t sq_event_t;

#define MAX_SUPPORTED_SAMPLERATES 16
#define TEST_RATES = { 384000, 352000, 192000, 176400, 96000, 88200, 48000, 44100, 32000, 24000, 22500, 16000, 12000, 11025, 8000, 0 }

typedef enum { SQ_RATE_384000 = 384000, SQ_RATE_352000 = 352000,
			   SQ_RATE_192000 = 192000, SQ_RATE_176400 = 176400,
			   SQ_RATE_96000 = 96000, SQ_RATE_48000 = 48000, SQ_RATE_44100 = 44100,
			   SQ_RATE_32000 = 32000, SQ_RATE_24000 = 24000, SQ_RATE_22500 = 22500,
			   SQ_RATE_16000 = 16000, SQ_RATE_12000 = 12000, SQ_RATE_11025 = 11025,
			   SQ_RATE_8000 = 8000, SQ_RATE_DEFAULT = 0} sq_rate_e;
typedef enum { L24_PACKED, L24_PACKED_LPCM, L24_TRUNC_16, L24_TRUNC_16_PCM, L24_UNPACKED_HIGH, L24_UNPACKED_LOW } sq_L24_pack_t;
typedef enum { FLAC_NO_HEADER = 0, FLAC_NORMAL_HEADER = 1, FLAC_FULL_HEADER = 2 } sq_flac_header_t;
typedef	int	sq_dev_handle_t;
typedef unsigned sq_rate_t;

typedef struct sq_metadata_s {
	char *artist;
	char *album;
	char *title;
	char *genre;
	char *path;
	char *artwork;
	u32_t index;
	u32_t track;
	u32_t duration;
	u32_t file_size;
	bool  remote;
	u32_t track_hash;
} sq_metadata_t;

typedef	struct sq_dev_param_s {
	unsigned 	stream_buf_size;
	unsigned 	output_buf_size;
	sq_mode_t	mode;
	sq_rate_t	rate[MAX_SUPPORTED_SAMPLERATES];
	int			max_get_bytes;		 // max size allowed in a single read
	int			max_read_wait;
	char		codecs[SQ_STR_LENGTH];
	sq_rate_e	sample_rate;
	sq_L24_pack_t		L24_format;
	sq_flac_header_t	flac_header;
	char		buffer_dir[SQ_STR_LENGTH];
	s32_t		buffer_limit;
	int			keep_buffer_file;
	u8_t		mac[6];
	bool		send_icy;
} sq_dev_param_t;

typedef struct sq_log_level_s {		// must be one of lERROR, lINFO, lDEBUG or lSDEBUG
	log_level	general;
	log_level	slimproto;
	log_level	output;
	log_level	stream;
	log_level	decode;
	log_level	main;
	log_level	upnp;
	log_level	sq2mr;
	log_level	web;
} sq_log_level_t;

typedef struct
{
	char	name[SQ_STR_LENGTH];
	char	ext[5];
	u8_t	channels;
	u8_t	sample_size;
	u32_t	sample_rate;
	u8_t	endianness;
	u8_t	src_format;
	u8_t	codec;
	u32_t	duration;
	char	content_type[SQ_STR_LENGTH];
	char	proto_info[SQ_STR_LENGTH];
	off_t	file_size;
	bool	remote;
	u32_t	track_hash;
} sq_seturi_t;

extern unsigned gl_slimproto_stream_port;

typedef bool (*sq_callback_t)(sq_dev_handle_t handle, void *caller_id, sq_action_t action, u8_t *cookie, void *param);

char*				sq_parse_args(int argc, char**argv);
// all params can be NULL
void				sq_init(char *server, u8_t mac[6], sq_log_level_t *);
void				sq_stop(void);

// only name cannot be NULL
bool			 	sq_run_device(sq_dev_handle_t handle, char *name, sq_dev_param_t *param);
void				sq_delete_device(sq_dev_handle_t);
sq_dev_handle_t		sq_reserve_device(void *caller_id, sq_callback_t callback);
void				sq_release_device(sq_dev_handle_t);

bool				sq_call(sq_dev_handle_t handle, sq_action_t action, void *param);
void				sq_notify(sq_dev_handle_t handle, void *caller_id, sq_event_t event, u8_t *cookie, void *param);
u32_t 				sq_get_time(sq_dev_handle_t handle);
bool				sq_get_metadata(sq_dev_handle_t handle, struct sq_metadata_s *metadata, bool next);
void				sq_default_metadata(struct sq_metadata_s *metadata, bool init);
void 				sq_free_metadata(struct sq_metadata_s *metadata);
bool 				sq_set_time(sq_dev_handle_t handle, u32_t time);
void*				sq_urn2MR(const char *urn);
sq_dev_handle_t 	sq_urn2handle(const char *urn);
void*				sq_get_info(const char *urn, s32_t *filesize, char **content_type, u16_t interval);	// string must be released by caller
void*				sq_open(const char *urn);
void*				sq_isopen(const char *urn);
void				sq_set_sizes(void *desc);
bool				sq_close(void *desc);
int					sq_read(void *desc, void *dst, unsigned bytes);
int					sq_seek(void *desc, off_t bytes, int from);
bool 				sq_is_remote(const char *urn);

void stream_loglevel(log_level level);
void slimproto_loglevel(log_level level);
void output_loglevel(log_level level);
void output_mr_loglevel(log_level level);
void decode_loglevel(log_level level);
void main_loglevel(log_level level);
#endif

