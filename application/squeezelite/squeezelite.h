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
#include "platform.h"
#include "squeezedefs.h"

#if !defined(LOOPBACK)
#if LINUX && !defined(SELFPIPE)
#define EVENTFD   1
#define SELFPIPE  0
#define WINEVENT  0
#endif
#if (LINUX && !EVENTFD) || OSX || FREEBSD || SUNOS
#define EVENTFD   0
#define SELFPIPE  1
#define WINEVENT  0
#endif
#if WIN
#define EVENTFD   0
#define SELFPIPE  0
#define WINEVENT  1
#endif
#else
#define EVENTFD   0
#define SELFPIPE  0
#define WINEVENT  0
#undef LOOPBACK
#define LOOPBACK  1
#endif

#if !LINKALL

// dynamically loaded libraries at run time
#if LINUX || SUNOS
#define LIBFLAC "libFLAC.so.8"
#define LIBMAD  "libmad.so.0"
#define LIBMPG "libmpg123.so.0"
#define LIBVORBIS "libvorbisfile.so.3"
#define LIBTREMOR "libvorbisidec.so.1"
#define LIBOPUS "libopusfile.so.0"
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
#define LIBOPUS "libopusfile.0.dylib"
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
#define LIBOPUS "libopusfile-0.dll"
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
#define LIBOPUS "libopusfile.so.1"
#define LIBFAAD "libfaad.so.2"
#define LIBAVUTIL   "libavutil.so.%d"
#define LIBAVCODEC  "libavcodec.so.%d"
#define LIBAVFORMAT "libavformat.so.%d"
#endif

#endif // !LINKALL

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

#define SL_LITTLE_ENDIAN (__BYTE_ORDER__ == __ORDER_LITTLE_ENDIAN__)

#include "squeezeitf.h"
#include "mimetypes.h"
#include "cross_log.h"
#include "cross_net.h"
#include "cross_util.h"

// we'll give venerable squeezelite the benefit of "old" int's
typedef uint64_t u64_t;
typedef int64_t  s64_t;
typedef uint32_t u32_t;
typedef int32_t  s32_t;
typedef uint16_t u16_t;
typedef int16_t  s16_t;
typedef uint8_t  u8_t;
typedef int8_t   s8_t;

#define MAX_HEADER 4096 // do not reduce as icy-meta max is 4080

#define STREAM_THREAD_STACK_SIZE (1024 * 64)
#define DECODE_THREAD_STACK_SIZE (1024 * 128)
#define OUTPUT_THREAD_STACK_SIZE (1024 * 64)
#define SLIMPROTO_THREAD_STACK_SIZE  (1024 * 64)
#define DECODE_THREAD_STACK_SIZE (1024 * 128)

#define mutex_type pthread_mutex_t
#define mutex_create(m) pthread_mutex_init(&m, NULL)
#define mutex_lock(m) pthread_mutex_lock(&m)
#define mutex_unlock(m) pthread_mutex_unlock(&m)
#define mutex_trylock(m) pthread_mutex_trylock(&m)
#define mutex_destroy(m) pthread_mutex_destroy(&m)
#define thread_type pthread_t
#define mutex_timedlock(m, t) _mutex_timedlock(&m, t)
#if !WIN
#define mutex_create_p(m) \
	pthread_mutexattr_t attr; \
	pthread_mutexattr_init(&attr); \
	pthread_mutexattr_setprotocol(&attr, PTHREAD_PRIO_INHERIT); \
	pthread_mutex_init(&m, &attr); pthread_mutexattr_destroy(&attr)
#else
#define mutex_create_p mutex_create
#endif	

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

#if LOOPBACK
#define event_handle struct pollfd
#define event_event struct wake
#define wake_create(e) _wake_create(&e)
#define wake_signal(e) send(e.fds[1], ".", 1, 0)
#define wake_clear(e) char c; recv(e, &c, 1, 0)
#define wake_close(e) closesocket(e.mfds); closesocket(e.fds[0]); closesocket(e.fds[1])
struct wake {
	int mfds;
	int fds[2];
};
void _wake_create(event_event*);
#endif

#if WINEVENT
#define event_event HANDLE
#define event_handle HANDLE
#define wake_create(e) e = CreateEvent(NULL, FALSE, FALSE, NULL)
#define wake_signal(e) SetEvent(e)
#define wake_close(e) CloseHandle(e)
#endif

// this is for decoded frames buffers (32 bits * 2 channels)
#define BYTES_PER_FRAME 8

typedef enum { EVENT_TIMEOUT = 0, EVENT_READ, EVENT_WAKE } event_type;
struct thread_ctx_s;

char*		next_param(char *src, char c);
void 		server_addr(char *server, in_addr_t *ip_ptr, unsigned *port_ptr);
void 		set_readwake_handles(event_handle handles[], sockfd s, event_event e);
event_type 	wait_readwake(event_handle handles[], int timeout);
void 		packN(u32_t *dest, u32_t val);
void 		packn(u16_t *dest, u16_t val);
u32_t 		unpackN(u32_t *src);
u16_t 		unpackn(u16_t *src);

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
unsigned 	_buf_write(struct buffer *buf, void *src, unsigned size);
void 		buf_flush(struct buffer *buf);
void 		buf_adjust(struct buffer *buf, size_t mod);
void 		_buf_resize(struct buffer *buf, size_t size);
void 		_buf_unwrap(struct buffer *buf, size_t cont);
void 		buf_init(struct buffer *buf, size_t size);
void 		buf_destroy(struct buffer *buf);
bool 		_buf_reset(struct buffer *buf);

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

struct streamstate {
	stream_state state;
	FILE* store;
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
	size_t header_mlen;
	struct sockaddr_in addr;
	char host[256];
	struct {
		char header[2048];
		unsigned len, threshold;
		in_addr_t ip;
		u16_t port;
		unsigned flags;
	} strm;
};

bool 		stream_thread_init(unsigned streambuf_size, struct thread_ctx_s *ctx);
void 		stream_close(struct thread_ctx_s *ctx);
void 		stream_file(const char *header, size_t header_len, unsigned threshold, struct thread_ctx_s *ctx);
void 		stream_sock(u32_t ip, u16_t port, bool use_ssl, const char *header, size_t header_len, unsigned threshold, 
						bool cont_wait, struct thread_ctx_s *ctx);
bool 		stream_disconnect(struct thread_ctx_s *ctx);

// decode.c
typedef enum { DECODE_STOPPED = 0, DECODE_READY, DECODE_RUNNING, DECODE_COMPLETE, DECODE_ERROR } decode_state;

struct decodestate {
	decode_state state;
	bool new_stream;
	u32_t frames;
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
	bool thru;
};

void 		decode_init(void);
void 		decode_end(void);
void 		decode_thread_init(struct thread_ctx_s *ctx);

void 		decode_close(struct thread_ctx_s *ctx);
void 		decode_flush(struct thread_ctx_s *ctx);
unsigned 	decode_newstream(unsigned sample_rate, int supported_rates[],
							 struct thread_ctx_s *ctx);
bool 		codec_open(u8_t codec, u8_t sample_size, u32_t sample_rate,
					   u8_t	channels, u8_t endianness, struct thread_ctx_s *ctx);

#if PROCESS
// process.c
void 		process_samples(struct thread_ctx_s *ctx);
void 		process_drain(struct thread_ctx_s *ctx);
void 		process_flush(struct thread_ctx_s *ctx);
unsigned 	process_newstream(bool *direct, unsigned raw_sample_rate,
							  int supported_rates[], struct thread_ctx_s *ctx);
void 		process_init(char *opt, struct thread_ctx_s *ctx);
void 		process_end(struct thread_ctx_s *ctx);
#endif

#if RESAMPLE
// resample.c

void 		resample_samples(struct thread_ctx_s *ctx);
bool 		resample_drain(struct thread_ctx_s *ctx);
bool 		resample_newstream(unsigned raw_sample_rate, int supported_rates[],
							   struct thread_ctx_s *ctx);
void 		resample_flush(struct thread_ctx_s *ctx);
bool 		resample_init(char *opt, struct thread_ctx_s *ctx);
void 		resample_end(struct thread_ctx_s *ctx);
#endif

// output.c

#define	OUTPUTBUF_IDLE_SIZE (256*1024)
#define HTTP_STUB_DEPTH		(2048*1024)

#define ICY_LEN_MAX		(255*16+1)
#define METADATA_UPDATE_TIME	5000

// real value is 576x2=1152 samples@44100kHz = 26.122 ms but we want a bit more blocks
#define MP3_SILENCE_DURATION 26

typedef enum { OUTPUT_OFF = -1, OUTPUT_STOPPED = 0, OUTPUT_WAITING,
			   OUTPUT_RUNNING } output_state;

typedef enum { FADE_INACTIVE = 0, FADE_DUE, FADE_ACTIVE, FADE_PENDING } fade_state;
typedef enum { FADE_UP = 1, FADE_DOWN, FADE_CROSS } fade_dir;
typedef enum { FADE_NONE = 0, FADE_CROSSFADE, FADE_IN, FADE_OUT, FADE_INOUT } fade_mode;

typedef enum { ENCODE_THRU, ENCODE_NULL, ENCODE_PCM, ENCODE_FLAC, ENCODE_MP3 } encode_mode;

// parameters for the output management thread
struct output_thread_s {
		bool			running;
		thread_type 	thread;
		int				http;			// listening socket of http server
		int 			index;
};

// info for the track being sent to the http renderer (not played)
struct outputstate {
	output_state state;		// license to stream or not
	bool	completed;	 	// whole track has been pulled from outputbuf
	char	format;			// data sent format (p=pcm, w=wav, i=aif, f=flac)
	u8_t 	sample_size, channels;	// from original stream
	u8_t	codec;			 // from original stream
	u32_t 	sample_rate;	// sample rate after optional resampling
	u32_t	direct_sample_rate;		// original sample rate;
	int 	in_endian, out_endian;	// 1 = little (MSFT/INTL), 0 = big (PCM/AAPL)
	u32_t 	duration;       // duration of track in ms, 0 if unknown
	u32_t	offset;			// offset of track in ms (for flow mode)
	u32_t	bitrate;	  	// as per name
	ssize_t length;			// HTTP content-length (-1:no chunked, -3 chunked if possible, >0 fake length)
	u16_t  	index;			// 16 bits track counter(see output_thread)
	u16_t	port;			// port of latest thread (mainy used for codc)
	bool 	chunked;		// chunked mode
	char 	mimetype[STR_LEN];	// content-type to send to player
	bool  	track_started;	// track has started to be streamed (trigger, not state)
	u8_t  	*track_start;   // pointer where track starts in buffer, just for legacy compatibility
	int		supported_rates[2];	// for resampling (0 = use raw)
	// for regular metadata update
	struct metadata_s metadata;
	struct {
		u32_t hash, last;
		bool enabled;
	} live_metadata;
	// for icy data
	struct {
		bool allowed;
		size_t interval, remain;
		size_t size, count;
		char buffer[ICY_LEN_MAX];
		char *artist, *title, *artwork;
		bool  updated;
	} icy;
	// for format that requires headers
	struct {
		size_t size, count;
		u8_t *buffer;
	} header;
	// only useful with decode mode
	fade_state  fade; 		// fading state
	unsigned 	fade_secs;  // set by slimproto
	fade_mode 	fade_mode;  // fading mode
	u8_t	    *fade_start;// pointer to fading start in output buffer
	u8_t 		*fade_end;	// pointer to fading end in output buffer
	u8_t		*fade_writep;	// pointer to pending fade position
	fade_dir 	fade_dir;	// fading direction
	// only used with pcm or decode mode
	u32_t 		replay_gain, next_replay_gain;
	u32_t		start_at;	// when to start the track, unused
	struct {
		u32_t	sample_rate;
		u8_t 	sample_size;
		u8_t 	channels;
		encode_mode mode;	// thru, pcm, flac
		bool  	flow;		// thread do not exit when track ends
		void 	*codec; 	// re-encoding codec
		u16_t  	level;      // in flac, compression level, in mp3 bitrate
		u8_t	*buffer;	// interim codec buffer (optional)
		size_t	count;		// # of *frames* in buffer or # of silence blocks to send (null mode)
	} encode;				// format of what being sent to player
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

// function starting with _ must be called with mutex locked
bool		output_thread_init(struct thread_ctx_s *ctx);
void 		output_close(struct thread_ctx_s *ctx);
bool		output_init(void);
void		output_end(void);
void		output_set_icy(struct metadata_s* metadata, struct thread_ctx_s* ctx);
void 		output_free_icy(struct thread_ctx_s *ctx);

bool		_output_fill(struct buffer *buf, FILE *store, struct thread_ctx_s *ctx);
void 		_output_new_stream(struct buffer *buf, FILE *store, struct thread_ctx_s *ctx);
void 		_output_end_stream(struct buffer *buf, struct thread_ctx_s *ctx);
void 		_checkfade(bool, struct thread_ctx_s *ctx);
void 		_checkduration(u32_t frames, struct thread_ctx_s *ctx);

// output_http.c
void 		output_flush(struct thread_ctx_s *ctx);
bool		output_start(struct thread_ctx_s *ctx);
bool 		output_abort(struct thread_ctx_s *ctx, int index);
void 		wake_output(struct thread_ctx_s *ctx);

/***************** main thread context**************/
typedef struct {
	u32_t updated;
	u32_t stream_full;			// v : unread bytes in stream buf
	u32_t stream_size;			// f : stream_buf_size init param
	u64_t stream_bytes;         // v : bytes received for current stream
	u32_t output_full;			// v : unread bytes in output buf
	u32_t output_size;			// f : output_buf_size init param
	u32_t sample_rate;
	u32_t last;
	stream_state stream_state;
	u32_t	ms_played;
	u32_t	duration;
	u16_t	voltage;
} status_t;

typedef enum {TRACK_STOPPED = 0, TRACK_STARTED, TRACK_PAUSED} track_status_t;

#define PLAYER_NAME_LEN 64
#define SERVER_VERSION_LEN	32
#define MAX_PLAYER		32
#define MAX_MIMETYPES	128

struct thread_ctx_s {
	int 		self;
	int 		autostart;
	bool		running;
	thread_type	thread;
	bool		in_use;
	bool		on;
	sq_dev_param_t	config;
	char 		*mimetypes[MAX_MIMETYPES + 1];
	mutex_type 	mutex;
	bool 		sentSTMu, sentSTMo, sentSTMl, sendSTMd, canSTMdu;
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
	struct buffer		*streambuf;
	struct buffer		*outputbuf;
	in_addr_t 	slimproto_ip;
	unsigned 	slimproto_port;
	char		server_version[SERVER_VERSION_LEN + 1];
	char		server_port[5+1];
	char		server_ip[4*(3+1)+1];
	u16_t		cli_port;
	sockfd 		sock, fd, cli_sock;
#if USE_SSL
	void		*ssl;  			// void to no include openssl headers
	bool		ssl_error;
#endif
	u16_t		voltage;
	char		cli_id[18];		// (6*2)+(5*':')+NULL
	mutex_type	cli_mutex;
	u32_t		cli_timeout;
	struct output_thread_s output_thread[2];
	bool 		decode_running, stream_running;
	thread_type	decode_thread, stream_thread;
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
extern struct in_addr		sq_local_host;
extern u16_t 				sq_local_port;
extern char  				sq_model_name[];

#define MAX_CODECS 16
extern struct codec *codecs[MAX_CODECS];
struct codec*	register_thru(void);
void		 	deregister_thru(void);
struct codec*	register_flac(void);
void		 	deregister_flac(void);
struct codec*	register_flac_thru(void);
void		 	deregister_flac_thru(void);
struct codec*	register_m4a_thru(void);
void		 	deregister_m4a_thru(void);
struct codec*	register_pcm(void);
void		 	deregister_pcm(void);
struct codec*	register_vorbis(void);
void		 	deregister_vorbis(void);
struct codec*	register_faad(void);
void		 	deregister_faad(void);
struct codec*	register_mad(void);
void		 	deregister_mad(void);
struct codec*	register_alac(void);
void		 	deregister_alac(void);
struct codec*	register_opus(void);
void		 	deregister_opus(void);

#if RESAMPLE
bool register_soxr(void);
void deregister_soxr(void);
#endif

/*
The whole process includes receiving from LMS into streambuf, then either
passthrough to outputbuf when no decoding is involved or decoding into outpufbuf
in 32 bits 2 channels samples in platform's native endianness
There a single outputbuf and state, but up to two thread to send it to the
player due UPnP gapless feature ( two track in semi-parallel)
The outputbuf is transferred to a local small http buffer before it's actually
send to the player. When decoder / re-encoding is used, that buffer contains
the reformated audio (truncation, coding, swapping, gain ...)
The other main reason for that last buffer is to benefit from UPnP gapless
features which allows a "next track" request to be sent while playing the
current track, but that request must be sent long before current track finishes.
Long before is a buffering size problem, which means that with compressed
formats it can be sent late but with PCM, some players with smaller memory may
require that command to be send pretty early of the player stops, so to some
extend that http buffer acts like an extension of UPnP player's own buffer.
Still, due to the streaming nature of LMS (one single outputbuf) it is not
possible to send the "current" and "next" track fully in parallel, so the trick
is to use that http buffer so that as soon as the last byte of a current track
has been put in it, then LMS can be requested with the next track and so the
UPnP request will come a few seconds earlier (depends on http buffer size) than
it would normally do. Again, for players with larger buffers, this is not really
needed, but some have really small buffers.
The output thread then have two states, the first one (running state) which does
complete processing from outputbuf, gain, trucation, swapping, coding then write
to the http buffer then send the audio to the buffer. The second one, (draining
state) which just sends from http buffer. When entering draining state, data
must not be pulled anymore from outputbuf.
The two output threads cannot be both in running or draining state and they
co-exist during limited period of time. When the running thread has emptied the
outpufbuf (and decoder has stopped) then it moves to draining state and LMS is
informed that the next track can be sent (STMd). If the UPnP player suppports
gapeless, upon reception of LMS' next track request, another output thread is
started (in running state) and the UPnP request is sent to the player which
will, at some point (unknown, depends on the implementation) get the data using
http. Depending how fast the UPnP player implements gapless, then there might be
a small period of time where both thread will work in parallel, once doing full
processing, the other one just sending what's left (draining) in http buffer.
*/

