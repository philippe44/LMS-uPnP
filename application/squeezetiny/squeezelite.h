/*
 *  Squeezelite - lightweight headless squeezebox emulator
 *
 *  (c) Adrian Smith 2012-2014, triode1@btinternet.com
  *	(c) Philippe 2015-2016, philippe_44@outlook.com
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

#if defined(RESAMPLE) || defined(RESAMPLE_MP)
#undef  RESAMPLE
#define RESAMPLE  1 // resampling
#define PROCESS   1 // any sample processing (only resampling at present)
#else
#define RESAMPLE  0
#define PROCESS   0
#endif
#if defined(RESAMPLE_MP)
#undef RESAMPLE_MP
#define RESAMPLE_MP 1
#else
#define RESAMPLE_MP 0
#endif

#if defined(FFMPEG)
#undef FFMPEG
#define FFMPEG    1
#else
#define FFMPEG    0
#endif

#if defined(DSD)
#undef DSD
#define DSD       1
#define IF_DSD(x) { x }
#else
#undef DSD
#define DSD       0
#define IF_DSD(x)
#endif

#if defined(LINKALL)
#undef LINKALL
#define LINKALL   1 // link all libraries at build time - requires all to be available at run time
#else
#define LINKALL   0
#endif


#if !LINKALL

// dynamically loaded libraries at run time

#if LINUX
#define LIBFLAC "libFLAC.so.8"
#define LIBMAD  "libmad.so.0"
#define LIBMPG "libmpg123.so.0"
#define LIBVORBIS "libvorbisfile.so.3"
#define LIBTREMOR "libvorbisidec.so.1"
#define LIBFAAD "libfaad.so.2"
#define LIBAVUTIL   "libavutil.so.%d"
#define LIBAVCODEC  "libavcodec.so.%d"
#define LIBAVFORMAT "libavformat.so.%d"
#define LIBSOXR "libsoxr.so.0"
#endif

#if OSX
#define LIBFLAC "libFLAC.8.dylib"
#define LIBMAD  "libmad.0.dylib"
#define LIBMPG "libmpg123.0.dylib"
#define LIBVORBIS "libvorbisfile.3.dylib"
#define LIBTREMOR "libvorbisidec.1.dylib"
#define LIBFAAD "libfaad.2.dylib"
#define LIBAVUTIL   "libavutil.%d.dylib"
#define LIBAVCODEC  "libavcodec.%d.dylib"
#define LIBAVFORMAT "libavformat.%d.dylib"
#define LIBSOXR "libsoxr.0.dylib"
#endif

#if WIN
#define LIBFLAC "libFLAC.dll"
#define LIBMAD  "libmad-0.dll"
#define LIBMPG "libmpg123-0.dll"
#define LIBVORBIS "libvorbisfile.dll"
#define LIBTREMOR "libvorbisidec.dll"
#define LIBFAAD "libfaad2.dll"
#define LIBAVUTIL   "avutil-%d.dll"
#define LIBAVCODEC  "avcodec-%d.dll"
#define LIBAVFORMAT "avformat-%d.dll"
#define LIBSOXR "libsoxr.dll"
#endif

#if FREEBSD
#define LIBFLAC "libFLAC.so.11"
#define LIBMAD  "libmad.so.2"
#define LIBMPG "libmpg123.so.0"
#define LIBVORBIS "libvorbisfile.so.6"
#define LIBTREMOR "libvorbisidec.so.1"
#define LIBFAAD "libfaad.so.2"
#define LIBAVUTIL   "libavutil.so.%d"
#define LIBAVCODEC  "libavcodec.so.%d"
#define LIBAVFORMAT "libavformat.so.%d"
#endif

#endif // !LINKALL

// config options
#define OUTPUTBUF_SIZE_CROSSFADE (OUTPUTBUF_SIZE * 12 / 10)

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

#define MAX_SILENCE_FRAMES 2048

#define FIXED_ONE 0x10000

#define BYTES_PER_FRAME 8

// utils.c (non logging)
typedef enum { EVENT_TIMEOUT = 0, EVENT_READ, EVENT_WAKE } event_type;
struct thread_ctx_s;

char *next_param(char *src, char c);
u32_t gettime_ms(void);
void get_mac(u8_t *mac);
void set_nonblock(sockfd s);
int connect_timeout(sockfd sock, const struct sockaddr *addr, socklen_t addrlen, int timeout);
void server_addr(char *server, in_addr_t *ip_ptr, unsigned *port_ptr);
void set_readwake_handles(event_handle handles[], sockfd s, event_event e);
event_type wait_readwake(event_handle handles[], int timeout);
void packN(u32_t *dest, u32_t val);
void packn(u16_t *dest, u16_t val);
u32_t unpackN(u32_t *src);
u16_t unpackn(u16_t *src);
#if OSX
void set_nosigpipe(sockfd s);
#else
#define set_nosigpipe(s)
#endif
#if WIN
void winsock_init(void);
void winsock_close(void);
void *dlopen(const char *filename, int flag);
void *dlsym(void *handle, const char *symbol);
char *dlerror(void);
int poll(struct pollfd *fds, unsigned long numfds, int timeout);
#endif
#if LINUX || FREEBSD
void touch_memory(u8_t *buf, size_t size);
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
unsigned _buf_used(struct buffer *buf);
unsigned _buf_space(struct buffer *buf);
unsigned _buf_cont_read(struct buffer *buf);
unsigned _buf_cont_write(struct buffer *buf);
void _buf_inc_readp(struct buffer *buf, unsigned by);
void _buf_inc_writep(struct buffer *buf, unsigned by);
unsigned _buf_read(void *dst, struct buffer *src, unsigned btes);
void	*_buf_readp(struct buffer *buf);
int	 _buf_seek(struct buffer *src, unsigned from, unsigned by);
void _buf_move(struct buffer *buf, unsigned by);
unsigned _buf_size(struct buffer *src);
void buf_flush(struct buffer *buf);
void buf_adjust(struct buffer *buf, size_t mod);
void _buf_resize(struct buffer *buf, size_t size);
void buf_init(struct buffer *buf, size_t size);
void buf_destroy(struct buffer *buf);

// slimproto.c
void slimproto_close(struct thread_ctx_s *ctx);
void slimproto_reset(struct thread_ctx_s *ctx);
void slimproto_thread_init(struct thread_ctx_s *ctx);
void wake_controller(struct thread_ctx_s *ctx);
void send_packet(u8_t *packet, size_t len, sockfd sock);
void wake_controller(struct thread_ctx_s *ctx);

// stream.c
typedef enum { STOPPED = 0, DISCONNECT, STREAMING_WAIT,
			   STREAMING_BUFFERING, STREAMING_FILE, STREAMING_HTTP, SEND_HEADERS, RECV_HEADERS } stream_state;
typedef enum { DISCONNECT_OK = 0, LOCAL_DISCONNECT = 1, REMOTE_DISCONNECT = 2, UNREACHABLE = 3, TIMEOUT = 4 } disconnect_code;

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

void stream_thread_init(unsigned buf_size, struct thread_ctx_s *ctx);
void stream_close(struct thread_ctx_s *ctx);
void stream_file(const char *header, size_t header_len, unsigned threshold, struct thread_ctx_s *ctx);
void stream_sock(u32_t ip, u16_t port, const char *header, size_t header_len, unsigned threshold, bool cont_wait, struct thread_ctx_s *ctx);
bool stream_disconnect(struct thread_ctx_s *ctx);

// decode.c
typedef enum { DECODE_STOPPED = 0, DECODE_RUNNING, DECODE_COMPLETE, DECODE_ERROR } decode_state;

struct decodestate {
	decode_state state;
	bool new_stream;
	mutex_type mutex;
#if PROCESS
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
	void (*open)(u8_t sample_size, u8_t sample_rate, u8_t channels, u8_t endianness);
	void (*close)(void);
	decode_state (*decode)(void);
};

void decode_init(const char *include_codecs, const char *exclude_codecs, bool full);
void decode_thread_init(struct thread_ctx_s *ctx);

void decode_close(struct thread_ctx_s *ctx);
void decode_flush(struct thread_ctx_s *ctx);
unsigned decode_newstream(unsigned sample_rate, unsigned supported_rates[], struct thread_ctx_s *ctx);
void codec_open(u8_t format, u8_t sample_size, u8_t sample_rate, u8_t channels, u8_t endianness, struct thread_ctx_s *ctx);

#if PROCESS
// process.c
void process_samples(void);
void process_drain(void);
void process_flush(void);
unsigned process_newstream(bool *direct, unsigned raw_sample_rate, unsigned supported_rates[]);
void process_init(char *opt);
#endif

#if RESAMPLE
// resample.c
void resample_samples(struct processstate *process);
bool resample_drain(struct processstate *process);
bool resample_newstream(struct processstate *process, unsigned raw_sample_rate, unsigned supported_rates[]);
void resample_flush(void);
bool resample_init(char *opt);
#endif

// output.c output_pack.c
typedef enum { OUTPUT_OFF = -1, OUTPUT_STOPPED = 0, OUTPUT_BUFFER, OUTPUT_RUNNING, 
			   OUTPUT_PAUSE_FRAMES, OUTPUT_SKIP_FRAMES, OUTPUT_START_AT } output_state;

typedef enum { S32_LE, S24_LE, S24_3LE, S16_LE } output_format;

typedef enum { FADE_INACTIVE = 0, FADE_DUE, FADE_ACTIVE } fade_state;
typedef enum { FADE_UP = 1, FADE_DOWN, FADE_CROSS } fade_dir;
typedef enum { FADE_NONE = 0, FADE_CROSSFADE, FADE_IN, FADE_OUT, FADE_INOUT } fade_mode;

struct outputstate {
	output_state state;
	output_format format;
	const char *device;
	bool  track_started;
	int (* write_cb)(frames_t out_frames, bool silence, s32_t gainL, s32_t gainR, s32_t cross_gain_in, s32_t cross_gain_out, s32_t **cross_ptr);
	unsigned start_frames;
	unsigned frames_played;
	unsigned frames_played_dmp;// frames played at the point delay is measured
	unsigned current_sample_rate;
	unsigned supported_rates[MAX_SUPPORTED_SAMPLERATES]; // ordered largest first so [0] is max_rate
	unsigned default_sample_rate;
	bool error_opening;
	unsigned device_frames;
	u32_t updated;
	u32_t track_start_time;
	u32_t current_replay_gain;
	// was union
	struct {
		u32_t pause_frames;
		u32_t skip_frames;
		u32_t start_at;
	};
	unsigned next_sample_rate; // set in decode thread
	u8_t  *track_start;        // set in decode thread
	u32_t gainL;               // set by slimproto
	u32_t gainR;               // set by slimproto
	u32_t next_replay_gain;    // set by slimproto
	unsigned threshold;        // set by slimproto
	fade_state fade;
	u8_t *fade_start;
	u8_t *fade_end;
	fade_dir fade_dir;
	fade_mode fade_mode;       // set by slimproto
	unsigned fade_secs;        // set by slimproto
	unsigned rate_delay;
	bool delay_active;
#if DSD
	bool next_dop;             // set in decode thread
	bool dop;
	bool has_dop;              // set in dop_init - output device supports dop
	unsigned dop_delay;        // set in dop_init - delay in ms switching to/from dop
#endif
};

void output_init_common(char *device, unsigned output_buf_size, unsigned rates[], struct thread_ctx_s *ctx);
void output_close_common(struct thread_ctx_s *ctx);
void output_flush(struct thread_ctx_s *ctx);
// _* called with mutex locked
frames_t _output_frames(frames_t avail, struct thread_ctx_s *ctx);
void _checkfade(bool, struct thread_ctx_s *ctx);
void wake_output(struct thread_ctx_s *ctx);

// output_mr.c
void output_mr_thread_init(unsigned output_buf_size, char *params, sq_rate_t[], unsigned rate_delay, struct thread_ctx_s *ctx);
void output_mr_close(struct thread_ctx_s *ctx);

// output_pack.c
void _scale_and_pack_frames(void *outputptr, s32_t *inputptr, frames_t cnt, s32_t gainL, s32_t gainR, output_format format);
void _apply_cross(struct buffer *outputbuf, frames_t out_frames, s32_t cross_gain_in, s32_t cross_gain_out, s32_t **cross_ptr);
void _apply_gain(struct buffer *outputbuf, frames_t count, s32_t gainL, s32_t gainR);
s32_t gain(s32_t gain, s32_t sample);
s32_t to_gain(float f);

// dop.c
#if DSD
bool is_flac_dop(u32_t *lptr, u32_t *rptr, frames_t frames);
void update_dop_marker(u32_t *ptr, frames_t frames);
void dop_silence_frames(u32_t *ptr, frames_t frames);
void dop_init(bool enable, unsigned delay);
#endif


/***************** main thread context**************/
typedef struct {
	u32_t updated;
	u32_t stream_start;			// vf : now() when stream started
	u32_t stream_full;			// v : unread bytes in stream buf
	u32_t stream_size;			// f : stream_buf_size init param
	u64_t stream_bytes;         // v : bytes received for current stream
	u32_t output_full;			// v : unread bytes in output buf
	u32_t output_size;			// f : output_buf_size init param
	u32_t frames_played;        // number of samples (bytes / sample size) played
	u32_t device_frames;
	u32_t current_sample_rate;
	u32_t last;
	stream_state stream_state;
	//
	u32_t	ms_played;
} status_t;

typedef enum {TRACK_STOPPED = 0, TRACK_STARTED, TRACK_PAUSED} track_status_t;

#define PLAYER_NAME_LEN 64
#define SERVER_VERSION_LEN	32
#define MAX_PLAYER		32

typedef struct out_ctx_s {
	FILE 				*read_file, *write_file;
	char 				buf_name[SQ_STR_LENGTH];
	s32_t				file_size;
	u32_t				duration;
	s32_t				raw_size;
	bool				remote;
	bool  				live;
	u32_t				track_hash;
	struct {
		bool 	update;
		u32_t	interval;
		u32_t	remain;
		char 	*artist;
		char 	*title;
		char 	*artwork;
		u32_t 	last;
	} icy;
	struct thread_ctx_s *owner;
	unsigned 			idx;
	char				ext[5];
	u8_t				src_format;
	u8_t				codec;
	u8_t				sample_size;
	u32_t				sample_rate;
	bool				endianness;
	u32_t				replay_gain;
	u8_t				channels;
	char				content_type[SQ_STR_LENGTH];
	char				proto_info[SQ_STR_LENGTH];
	u32_t				read_count, write_count;
	u64_t				read_count_t, write_count_t;
	u32_t				close_count;
	mutex_type			mutex;
} out_ctx_t;

struct thread_ctx_s {
	int 	self;
	int 	autostart;
	bool	running;
	bool	in_use;
	bool	on;
	sq_dev_param_t	config;
	mutex_type mutex;
	bool 	sentSTMu, sentSTMo, sentSTMl, sentSTMd;
	u32_t 	new_server;
	char 	*new_server_cap;
	char	fixed_cap[128], var_cap[128];
	status_t			status;
	struct streamstate	stream;
	struct outputstate 	output;
	struct decodestate 	decode;
#if PROCESS
	struct processtate	process;
#endif
	struct codec		*codec;
	struct buffer		__s_buf;
	struct buffer		__o_buf;
	struct buffer		*streambuf;
	struct buffer		*outputbuf;
	unsigned			out_idx;
	out_ctx_t			out_ctx[2];
	in_addr_t 	slimproto_ip;
	unsigned 	slimproto_port;
	char		server_version[SERVER_VERSION_LEN + 1];
	char		server_port[5+1];
	char		server_ip[4*(3+1)+1];
	sockfd 		sock, fd, cli_sock;
	u8_t 		mac[6];
	char		cli_id[18];		// (6*2)+(5*':')+NULL
	mutex_type	cli_mutex;
	bool		aiff_header;
//	u8_t *buf;					// for output_mr
//	unsigned buffill;			// for output_mr
	int bytes_per_frame;		// for output_mr
	bool	mr_running;			// for output_mr.c
	bool	stream_running;		// for stream.c
	bool	decode_running;		// for decode.c
	thread_type mr_thread;		// outputmr.c child thread
	thread_type stream_thread;	// stream.c child thread
	thread_type decode_thread;	// decode.c child thread
	thread_type	thread;			// main instance thread
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
	u32_t	ms_played;
	u32_t	start_at;
	u32_t	ms_pause;
	track_status_t	track_status;
	bool	track_ended;
	bool	track_new;
	bool	play_running;
	u32_t	track_start_time;
	bool	read_to;
	bool	read_ended;
};

extern struct thread_ctx_s thread_ctx[MAX_PLAYER];

// codecs
#define MAX_CODECS 9

extern struct codec *codecs[MAX_CODECS];

struct codec *register_flac(void);
struct codec *register_pcm(void);
struct codec *register_mad(void);
struct codec *register_mpg(void);
struct codec *register_vorbis(void);
struct codec *register_faad(void);
struct codec *register_dsd(void);
struct codec *register_ff(const char *codec);



