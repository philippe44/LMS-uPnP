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

// make may define: SELFPIPE, RESAMPLE, RESAMPLE_MP, VISEXPORT, DSD, LINKALL to influence build

// build detection
#include "squeezedefs.h"

#if LINUX && !defined(SELFPIPE)
#define EVENTFD   1
#define SELFPIPE  0
#define WINEVENT  0
#endif
#if (LINUX && !EVENTFD) || OSX || FREEBSD
#define EVENTFD   0
#define SELFPIPE  1
#define WINEVENT  0
#endif
#if WIN
#define EVENTFD   0
#define SELFPIPE  0
#define WINEVENT  1
#endif

#define MAX_HEADER 4096 // do not reduce as icy-meta max is 4080

#include <stdio.h>
#include <stdlib.h>
#include <stdarg.h>
#include <string.h>
#include <errno.h>
#include <limits.h>
#include <math.h>
#include <fcntl.h>
#include <sys/types.h>
#include <sys/stat.h>

#include "squeezeitf.h"
#include "util_common.h"
#include "log_util.h"

#if !defined(MSG_NOSIGNAL)
#define MSG_NOSIGNAL 0
#endif

typedef u32_t frames_t;
typedef int sockfd;

#if EVENTFD
#include <sys/eventfd.h>
#define event_event int
#define event_handle struct pollfd
#define wake_create(e) e = eventfd(0, 0)
#define wake_signal(e) eventfd_write(e, 1)
#define wake_clear(e) eventfd_t val; eventfd_read(e, &val)
#define wake_close(e) close(e)
#endif

#if SELFPIPE
#define event_handle struct pollfd
#define event_event struct wake
#define wake_create(e) pipe(e.fds); set_nonblock(e.fds[0]); set_nonblock(e.fds[1])
#define wake_signal(e) write(e.fds[1], ".", 1)
#define wake_clear(e) char c[10]; read(e, &c, 10)
#define wake_close(e) close(e.fds[0]); close(e.fds[1])
struct wake {
	int fds[2];
};
#endif

#if WINEVENT
#define event_event HANDLE
#define event_handle HANDLE
#define wake_create(e) e = CreateEvent(NULL, FALSE, FALSE, NULL)
#define wake_signal(e) SetEvent(e)
#define wake_close(e) CloseHandle(e)
#endif

// printf/scanf formats for u64_t
#if (LINUX && __WORDSIZE == 64) || (FREEBSD && __LP64__)
#define FMT_u64 "%lu"
#define FMT_x64 "%lx"
#elif __GLIBC_HAVE_LONG_LONG || defined __GNUC__ || WIN
#define FMT_u64 "%llu"
#define FMT_x64 "%llx"
#else
#error can not support u64_t
#endif

#define BYTES_PER_FRAME 4

typedef enum { THREAD_KILLED = 0, THREAD_RUNNING, THREAD_EXITED } thread_state;

// utils.c (non logging)
typedef struct {
	char *key;
	char *data;
} key_data_t;

bool 		http_parse(int sock, char **request, key_data_t *rkd, char **body, int *len);char*		http_send(int sock, char *method, key_data_t *rkd);
int 		read_line(int fd, char *line, int maxlen, int timeout);
int 		send_response(int sock, char *response);

char*		kd_lookup(key_data_t *kd, char *key);
bool 		kd_add(key_data_t *kd, char *key, char *value);
char* 		kd_dump(key_data_t *kd);
void 		kd_free(key_data_t *kd);

typedef enum { EVENT_TIMEOUT = 0, EVENT_READ, EVENT_WAKE } event_type;
struct thread_ctx_s;

char*		next_param(char *src, char c);
u32_t 		gettime_ms(void);
void 		get_mac(u8_t *mac);
void 		set_nonblock(sockfd s);
void 		set_block(sockfd s);
int 		connect_timeout(sockfd sock, const struct sockaddr *addr, socklen_t addrlen, int timeout);
int 		bind_socket(unsigned short *port, int mode);
int 		shutdown_socket(int sd);
void 		server_addr(char *server, in_addr_t *ip_ptr, unsigned *port_ptr);
void 		set_readwake_handles(event_handle handles[], sockfd s, event_event e);
event_type 	wait_readwake(event_handle handles[], int timeout);
void 		packN(u32_t *dest, u32_t val);
void 		packn(u16_t *dest, u16_t val);
u32_t 		unpackN(u32_t *src);
u16_t 		unpackn(u16_t *src);
#if OSX
void set_nosigpipe(sockfd s);
#else
#define set_nosigpipe(s)
#endif
#if WIN
void 		winsock_init(void);
void 		winsock_close(void);
void*		dlopen(const char *filename, int flag);
void*		dlsym(void *handle, const char *symbol);
char*		dlerror(void);
int 		poll(struct pollfd *fds, unsigned long numfds, int timeout);
#endif
#if LINUX || FREEBSD
void 		touch_memory(u8_t *buf, size_t size);
#endif

// buffer.c
struct buffer {
	u8_t *buf;
	u8_t *readp;
	u8_t *writep;
	u8_t *wrap;
	size_t size;
	size_t base_size;
	mutex_type mutex;
};

// _* called with mutex locked
unsigned 	_buf_used(struct buffer *buf);
unsigned 	_buf_space(struct buffer *buf);
unsigned 	_buf_cont_read(struct buffer *buf);
unsigned 	_buf_cont_write(struct buffer *buf);
void 		_buf_inc_readp(struct buffer *buf, unsigned by);
void 		_buf_inc_writep(struct buffer *buf, unsigned by);
unsigned 	_buf_read(void *dst, struct buffer *src, unsigned btes);
void*		_buf_readp(struct buffer *buf);
int	 		_buf_seek(struct buffer *src, unsigned from, unsigned by);
void 		_buf_move(struct buffer *buf, unsigned by);
unsigned 	_buf_size(struct buffer *src);
void 		buf_flush(struct buffer *buf);
void 		buf_adjust(struct buffer *buf, size_t mod);
void 		_buf_resize(struct buffer *buf, size_t size);
void 		buf_init(struct buffer *buf, size_t size);
void 		buf_destroy(struct buffer *buf);

// slimproto.c
void 		slimproto_close(struct thread_ctx_s *ctx);
void 		slimproto_reset(struct thread_ctx_s *ctx);
void 		slimproto_thread_init(struct thread_ctx_s *ctx);
void 		wake_controller(struct thread_ctx_s *ctx);
void 		send_packet(u8_t *packet, size_t len, sockfd sock);
void 		wake_controller(struct thread_ctx_s *ctx);

// stream.c
typedef enum { STOPPED = 0, DISCONNECT, STREAMING_WAIT,
			   STREAMING_BUFFERING, STREAMING_FILE, STREAMING_HTTP, SEND_HEADERS, RECV_HEADERS } stream_state;
typedef enum { DISCONNECT_OK = 0, LOCAL_DISCONNECT = 1, REMOTE_DISCONNECT = 2, UNREACHABLE = 3, TIMEOUT = 4 } disconnect_code;

#define STREAM_DELAY 30000

struct streamstate {
	stream_state state;
	disconnect_code disconnect;
	char *header;
	size_t header_len;
	int endtok;
	bool sent_headers;
	bool cont_wait;
	u64_t bytes;
	u32_t last_read;
	unsigned threshold;
	u32_t meta_interval;
	u32_t meta_next;
	u32_t meta_left;
	bool  meta_send;
};

void 		stream_thread_init(unsigned buf_size, struct thread_ctx_s *ctx);
void 		stream_close(struct thread_ctx_s *ctx);
void 		stream_file(const char *header, size_t header_len, unsigned threshold, struct thread_ctx_s *ctx);
void 		stream_sock(u32_t ip, u16_t port, const char *header, size_t header_len, unsigned threshold, bool cont_wait, struct thread_ctx_s *ctx);
bool 		stream_disconnect(struct thread_ctx_s *ctx);

// decode.c
typedef enum { DECODE_STOPPED = 0, DECODE_READY, DECODE_RUNNING, DECODE_COMPLETE, DECODE_ERROR } decode_state;

struct decodestate {
	decode_state state;
	bool new_stream;
	mutex_type mutex;
	void *handle;
#if PROCESS
	void *process_handle;
	bool direct;
	bool process;
#endif
};

#if PROCESS
struct processstate {
	u8_t *inbuf, *outbuf;
	unsigned max_in_frames, max_out_frames;
	unsigned in_frames, out_frames;
	unsigned in_sample_rate, out_sample_rate;
	unsigned long total_in, total_out;
};
#endif

struct codec {
	char id;
	char *types;
	unsigned min_read_bytes;
	unsigned min_space;
	void (*open)(u8_t sample_size, u32_t sample_rate, u8_t	channels,
				 u8_t endianness, struct thread_ctx_s *ctx);
	void (*close)(struct thread_ctx_s *ctx);
	decode_state (*decode)(struct thread_ctx_s *ctx);
};

void 		decode_init(void);
void 		decode_end(void);
void 		decode_thread_init(struct thread_ctx_s *ctx);

void 		decode_close(struct thread_ctx_s *ctx);
void 		decode_flush(struct thread_ctx_s *ctx);
unsigned 	decode_newstream(unsigned sample_rate, unsigned supported_rates[],
							 struct thread_ctx_s *ctx);
void 		codec_open(u8_t codec, u8_t sample_size, u32_t sample_rate,
					   u8_t	channels, u8_t endianness, struct thread_ctx_s *ctx);

#if PROCESS
// process.c
void 		process_samples(struct thread_ctx_s *ctx);
void 		process_drain(struct thread_ctx_s *ctx);
void 		process_flush(struct thread_ctx_s *ctx);
unsigned 	process_newstream(bool *direct, unsigned raw_sample_rate,
							  unsigned supported_rates[], struct thread_ctx_s *ctx);
void 		process_init(char *opt, struct thread_ctx_s *ctx);
void 		process_end(struct thread_ctx_s *ctx);
#endif

#if RESAMPLE
// resample.c
void 		resample_samples(struct thread_ctx_s *ctx);
bool 		resample_drain(struct thread_ctx_s *ctx);
bool 		resample_newstream(unsigned raw_sample_rate, unsigned supported_rates[],
							   struct thread_ctx_s *ctx);
void 		resample_flush(struct thread_ctx_s *ctx);
bool 		resample_init(char *opt, struct thread_ctx_s *ctx);
void 		resample_end(struct thread_ctx_s *ctx);
#endif

// output.c

#define ICY_LEN_MAX		(255*16+1)
#define ICY_UPDATE_TIME	5000

typedef enum { OUTPUT_OFF = -1, OUTPUT_STOPPED = 0, OUTPUT_WAITING,
			   OUTPUT_RUNNING } output_state;

typedef enum { FADE_INACTIVE = 0, FADE_DUE, FADE_ACTIVE } fade_state;
typedef enum { FADE_UP = 1, FADE_DOWN, FADE_CROSS } fade_dir;
typedef enum { FADE_NONE = 0, FADE_CROSSFADE, FADE_IN, FADE_OUT, FADE_INOUT } fade_mode;

// function starting with _ must be called with mutex locked
void 		output_init(unsigned output_buf_size, struct thread_ctx_s *ctx);
void 		output_close(struct thread_ctx_s *ctx);
size_t		output_pcm_header(void **header, size_t *hsize, struct thread_ctx_s *ctx );
void 		output_free_icy(struct thread_ctx_s *ctx);
void 		swap(u8_t *src, u8_t *dst, size_t bytes, u8_t size);
void 		truncate16(u8_t *src, u8_t *dst, size_t bytes, int in_endian, int out_endian);
void 		lpcm_pack(u8_t *dst, u8_t *src, size_t bytes, u8_t channels, int endian);
void 		apply_gain(void *p, u32_t gain, size_t bytes, u8_t size, int endian);

unsigned	_output_bytes(struct thread_ctx_s *ctx);
unsigned	_output_cont_bytes(struct thread_ctx_s *ctx);
void* 		_output_readp(struct thread_ctx_s *ctx);
void 		_output_inc_readp(struct thread_ctx_s *ctx, unsigned by);
void 		_output_boot(struct thread_ctx_s *ctx);
void 		_checkfade(bool, struct thread_ctx_s *ctx);

// output_http.c
void 		output_flush(struct thread_ctx_s *ctx);
bool 		output_start(struct thread_ctx_s *ctx);
void 		wake_output(struct thread_ctx_s *ctx);

// info for the track being sent to the http renderer (not played)
struct outputstate {
	output_state state;		// license to stream or not
	char	format;			// data sent format (p=pcm, w=wav, i=aif, f=flac)
	bool	thru;			// thru mode, just pass data to player
	int     index;			// track index served by output thread
	int		http;			// listening socket of http server
	u16_t	port;           // listening port of http server
	u8_t 	sample_size, channels, codec; // as name, original stream values
	u32_t 	sample_rate;	// as name, original stream values
	bool	trunc16;		 // true if 24 bits samples must be truncated to 16
	int 	in_endian, out_endian;	// 1 = little (MSFT/INTL), 0 = big (PCM/AAPL)
	u32_t 	duration;       // duration of track in ms, 0 if unknown
	bool  	remote;			// local track or not (if duration == 0 => live)
	ssize_t length;			// HTTP content-length (-1:no chunked, -3 chunked if possible, >0 fake length)
	bool 	chunked;		// chunked mode
	char 	mimetype[_STR_LEN_];	// content-type to send to player
	bool  	track_started;	// track has started to be streamed (trigger, not state)
	u8_t  	*track_start;   // pointer where track starts in buffer, just for legacy compatibility
	unsigned current_sample_rate;	// current in sample rate
	unsigned supported_rates[2];	// moot for now
	// for icy data
	struct {
		u32_t interval, remain;
		u16_t size, count;
		char buffer[ICY_LEN_MAX];
		char *artist, *title, *artwork;
		u32_t hash, last;
		bool  updated;
	} icy;
	// only useful with decode mode
	fade_state fade;		// fading state
	u8_t	   *fade_start;	// pointer to fading start in output buffer
	u8_t 		*fade_end;	// pointer to fading end in output buffer
	fade_dir 	fade_dir;	// fading direction
	fade_mode 	fade_mode;  // fading mode
	unsigned 	fade_secs;  // set by slimproto
	// only used with pcm or decode mode
	u32_t 		replay_gain;
	u32_t		start_at;	// when to start the track, unused
};

// http renderer state (track being played)
struct renderstate {
	enum { RD_TRANSITION, RD_STOPPED, RD_PLAYING, RD_PAUSED } state; // player last known state
	u32_t	ms_played;   	// elapsed time in ms as reported by player
	u32_t	ms_paused;      // total puased time in ùs
	u32_t	duration;  		// duration of the *current* track
	u32_t 	track_pause_time; // timestamp when the track was paused
	u32_t	track_start_time; // timestamp when the track started
	int     index;    		// current track index in player (-1 = unknown)
};

/***************** main thread context**************/
typedef struct {
	u32_t updated;
	u32_t stream_full;			// v : unread bytes in stream buf
	u32_t stream_size;			// f : stream_buf_size init param
	u64_t stream_bytes;         // v : bytes received for current stream
	u32_t output_full;			// v : unread bytes in output buf
	u32_t output_size;			// f : output_buf_size init param
	u32_t current_sample_rate;
	u32_t last;
	stream_state stream_state;
	u32_t	ms_played;
	u32_t	duration;
	thread_state output_running;
} status_t;

typedef enum {TRACK_STOPPED = 0, TRACK_STARTED, TRACK_PAUSED} track_status_t;

#define PLAYER_NAME_LEN 64
#define SERVER_VERSION_LEN	32
#define MAX_PLAYER		32

struct thread_ctx_s {
	int 	self;
	int 	autostart;
	bool	running;
	bool	in_use;
	bool	on;
	sq_dev_param_t	config;
	char 		*mimetypes[MAX_MIMETYPES + 1];
	mutex_type 	mutex;
	bool 		sentSTMu, sentSTMo, sentSTMl, sentSTMd, canSTMdu;
	u32_t 		new_server;
	char 		*new_server_cap;
	char		fixed_cap[128], var_cap[128];
	status_t	status;
	struct streamstate	stream;
	struct outputstate 	output;
	struct decodestate 	decode;
	struct renderstate	render;
#if PROCESS
	struct processstate	process;
#endif
	struct codec		*codec;
	struct buffer		__s_buf;
	struct buffer		__o_buf;
	struct buffer		__e_buf;
	struct buffer		*streambuf;
	struct buffer		*outputbuf;
	struct buffer		*encodebuf;
	in_addr_t 	slimproto_ip;
	unsigned 	slimproto_port;
	char		server_version[SERVER_VERSION_LEN + 1];
	char		server_port[5+1];
	char		server_ip[4*(3+1)+1];
	sockfd 		sock, fd, cli_sock;
	u8_t 		mac[6];
	char		cli_id[18];		// (6*2)+(5*':')+NULL
	mutex_type	cli_mutex;
	u32_t		cli_timestamp;
	thread_state output_running;
	bool	stream_running;
	bool	decode_running;
	thread_type output_thread;
	thread_type stream_thread;
	thread_type	thread;
	thread_type decode_thread;
	struct sockaddr_in serv_addr;
	#define MAXBUF 4096
	event_event	wake_e;
	struct 	{				// scratch memory for slimprot_run (was static)
		 u8_t 	buffer[MAXBUF];
		 u32_t	last;
		 char	header[MAX_HEADER];
	} slim_run;
	sq_callback_t	callback;
	void			*MR;
	u8_t 	last_command;
};

extern struct thread_ctx_s 	thread_ctx[MAX_PLAYER];
extern u16_t 				sq_port;
extern char  				sq_ip[16];

#define MAX_CODECS 16
extern struct codec *codecs[MAX_CODECS];
struct codec*	register_thru(void);
void		 	deregister_thru(void);
struct codec*	register_flac(void);
void		 	deregister_flac(void);
struct codec*	register_pcm(void);
void		 	deregister_pcm(void);

#if RESAMPLE
bool register_soxr(void);
void deregister_soxr(void);
#endif


