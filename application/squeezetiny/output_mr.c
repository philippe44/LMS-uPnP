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

/*
TODO
- wakeup outputhread only when streambuf has written something ?
- move the that handles adding headers + endianess out of main loop
*/

#include "squeezelite.h"

extern log_level	output_loglevel;
static log_level 	*loglevel = &output_loglevel;

#define BYTE_1(n)	((u8_t) (n >> 24))
#define BYTE_2(n)	((u8_t) (n >> 16))
#define BYTE_3(n)	((u8_t) (n >> 8))
#define BYTE_4(n)	((u8_t) (n))

/*---------------------------------- FLAC ------------------------------------*/

#define FLAC_COMBO(F,N,S) ((u32_t) (((u32_t) F << 12) | ((u32_t) ((N-1) & 0x07) << 9) | ((u32_t) ((S-1) & 0x01f) << 4)))
#define QUAD_BYTE_H(n)	((u32_t) ((u64_t)(n) >> 32))
#define QUAD_BYTE_L(n)	((u32_t) (n))

u32_t	FLAC_CODED_RATES[] = { 0, 88200, 176400, 192000, 8000, 16000, 22050,
							   24000, 32000, 44100, 48000, 96000, 0, 0, 0, 0 };
u8_t	FLAC_CODED_CHANNELS[] = { 1, 2, 3, 4, 5, 6, 7, 8, 2, 2, 2, 0, 0, 0, 0, 0 };
u8_t	FLAC_CODED_SAMPLE_SIZE[] = { 0, 8, 12, 0, 16, 20, 24, 0 };

#if SL_LITTLE_ENDIAN
#define FLAC_TAG	(0xf8ff)	// byte order is reversed because it's treated as a u16
#define FLAC_GET_FRAME_TAG(n)	((u16_t) ((n) & 0xf8ff))
#define FLAC_GET_BLOCK_STRATEGY(n) ((u16_t) ((n) & 0x0100))
#else
#define FLAC_GET_FRAME_TAG(n)	((u16_t) ((n) & 0xfff8))
#define FLAC_TAG	(0xfff8)	// byte order is reversed because it's treated as a u16
#define FLAC_GET_BLOCK_STRATEGY(n) ((u16_t) ((n) & 0x0001))
#endif

#define FLAC_GET_BLOCK_SIZE(n)	((u8_t) ((n) >> 4) & 0x0f)
#define FLAC_GET_FRAME_RATE(n) ((u8_t) ((n) & 0x0f))
#define FLAC_GET_FRAME_CHANNEL(n) ((u8_t) ((n) >> 4) & 0x0f)
#define FLAC_GET_FRAME_SAMPLE_SIZE(n) ((u8_t) ((n) >> 1) & 0x07)

#define FLAC_RECV_MIN	128

typedef struct flac_frame_s {
	u16_t	tag;
	u8_t    bsize_rate;
	u8_t	channels_sample_size;
} flac_frame_t;

typedef struct flac_streaminfo_s {
		u8_t min_block_size[2];
		u8_t max_block_size[2];
		u8_t min_frame_size[3];
		u8_t max_frame_size[3];
		u8_t combo[4];
		u8_t sample_count[4];
		u8_t MD5[16];
} flac_streaminfo_t;


flac_streaminfo_t FLAC_NORMAL_STREAMINFO = {
		{ 0x00, 0x10 },
		{ 0xff, 0xff },
		{ 0x00, 0x00, 0x00 },
		{ 0x00, 0x00, 0x00 },
		{ BYTE_1(FLAC_COMBO(44100, 2, 16)),
		BYTE_2(FLAC_COMBO(44100, 2, 16)),
		BYTE_3(FLAC_COMBO(44100, 2, 16)),
		BYTE_4(FLAC_COMBO(44100, 2, 16)) | BYTE_1(QUAD_BYTE_H(0)) },
		{ BYTE_1(QUAD_BYTE_L(0)),
		BYTE_2(QUAD_BYTE_L(0)),
		BYTE_3(QUAD_BYTE_L(0)),
		BYTE_4(QUAD_BYTE_L(0)) },
		{ 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
		0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00 }
};

#define FLAC_TOTAL_SAMPLES 0xffffffff

flac_streaminfo_t FLAC_FULL_STREAMINFO = {
		{ 0x00, 0x10 },
		{ 0xff, 0xff },
		{ 0x00, 0x00, 0x00 },
		{ 0x00, 0x00, 0x00 },
		{ BYTE_1(FLAC_COMBO(44100, 2, 16)),
		BYTE_2(FLAC_COMBO(44100, 2, 16)),
		BYTE_3(FLAC_COMBO(44100, 2, 16)),
		BYTE_4(FLAC_COMBO(44100, 2, 16)) | BYTE_1(QUAD_BYTE_H(FLAC_TOTAL_SAMPLES)) },
		{ BYTE_1(QUAD_BYTE_L(FLAC_TOTAL_SAMPLES)),
		BYTE_2(QUAD_BYTE_L(FLAC_TOTAL_SAMPLES)),
		BYTE_3(QUAD_BYTE_L(FLAC_TOTAL_SAMPLES)),
		BYTE_4(QUAD_BYTE_L(FLAC_TOTAL_SAMPLES)) },
		{ 0xaa, 0xaa, 0xaa, 0xaa, 0xaa, 0xaa, 0xaa, 0xaa,
		0xaa, 0xaa, 0xaa, 0xaa, 0xaa, 0xaa, 0xaa, 0xaa }
};

static u8_t flac_header[] = {
			'f', 'L', 'a', 'C',
			0x80,
			(u8_t) ((u32_t) sizeof(flac_streaminfo_t) >> 16),
			(u8_t) ((u32_t) sizeof(flac_streaminfo_t) >> 8),
			(u8_t) ((u32_t) sizeof(flac_streaminfo_t))
		};


/*---------------------------------- WAVE ------------------------------------*/
static struct wave_header_s {
	u8_t 	chunk_id[4];
	u32_t	chunk_size;
	u8_t	format[4];
	u8_t	subchunk1_id[4];
	u8_t	subchunk1_size[4];
	u8_t	audio_format[2];
	u16_t	channels;
	u32_t	sample_rate;
	u32_t   byte_rate;
	u16_t	block_align;
	u16_t	bits_per_sample;
	u8_t	subchunk2_id[4];
	u32_t	subchunk2_size;
} wave_header = {
		{ 'R', 'I', 'F', 'F' },
		0,
		{ 'W', 'A', 'V', 'E' },
		{ 'f','m','t',' ' },
		{ 16, 0, 0, 0 },
		{ 1, 0 },
		0,
		0,
		0,
		0,
		0,
		{ 'd', 'a', 't', 'a' },
		0, 		//chunk_size - sizeof(struct wave_header_s) - 8 - 8
	};


/*---------------------------------- AIFF ------------------------------------*/
static struct aiff_header_s {			// need to use all u8 due to padding
	u8_t 	chunk_id[4];
	u8_t	chunk_size[4];
	u8_t	format[4];
	u8_t	common_id[4];
	u8_t	common_size[4];
	u8_t	channels[2];
	u8_t 	frames[4];
	u8_t	sample_size[2];
	u8_t	sample_rate_exp[2];
	u8_t	sample_rate_num[8];
	u8_t    data_id[4];
	u8_t	data_size[4];
	u32_t	offset;
	u32_t	blocksize;
#define AIFF_PAD_SIZE	2				// C compiler adds a padding to that structure, need to discount it
	u8_t	pad[2];
} aiff_header = {
		{ 'F', 'O', 'R', 'M' },
#ifdef AIFF_MARKER
		{ 0x3B, 0x9A, 0xCA, 0x48 },		// adding comm, mark, ssnd and AIFF sizes
#else
		{ 0x3B, 0x9A, 0xCA, 0x2E },		// adding comm, ssnd and AIFF sizes
#endif
		{ 'A', 'I', 'F', 'F' },
		{ 'C', 'O', 'M', 'M' },
		{ 0x00, 0x00, 0x00, 0x12 },
		{ 0x00, 0x00 },
		{ 0x0E, 0xE6, 0xB2, 0x80 },		// 250x10^6 frames of 2 channels and 2 bytes each
		{ 0x00, 0x00 },
		{ 0x40, 0x0E },
		{ 0x00, 0x00 },
		{ 'S', 'S', 'N', 'D' },
		{ 0x3B, 0x9A, 0xCA, 0x08 },		// one chunk of 10^9 bytes + 8
		0,
		0
	};

#define LOCK_S   mutex_lock(ctx->streambuf->mutex)
#define UNLOCK_S mutex_unlock(ctx->streambuf->mutex)
#define LOCK_O
#define UNLOCK_O

/*---------------------------------------------------------------------------*/
static void little16(void *dst, u16_t src)
{
	u8_t *p = (u8_t*) dst;

	*p++ = (u8_t) (src);
	*p = (u8_t) (src >> 8);
}

/*---------------------------------------------------------------------------*/
static void little32(void *dst, u32_t src)
{
	u8_t *p = (u8_t*) dst;

	*p++ = (u8_t) (src);
	*p++ = (u8_t) (src >> 8);
	*p++ = (u8_t) (src >> 16);
	*p = (u8_t) (src >> 24);

}
/*---------------------------------------------------------------------------*/
static void big16(void *dst, u16_t src)
{
	u8_t *p = (u8_t*) dst;

	*p++ = (u8_t) (src >> 8);
	*p = (u8_t) (src);
}

/*---------------------------------------------------------------------------*/
static void big32(void *dst, u32_t src)
{
	u8_t *p = (u8_t*) dst;

	*p++ = (u8_t) (src >> 24);
	*p++ = (u8_t) (src >> 16);
	*p++ = (u8_t) (src >> 8);
	*p = (u8_t) (src);
}

/*---------------------------------------------------------------------------*/
static u16_t flac_block_size(u8_t block_size)
{
	if (block_size == 0x01) return 192;
	if (block_size <= 0x05) return 576 * (1 << (block_size - 2));
	if (block_size == 0x06) return 0;
	if (block_size == 0x07) return 0;
	if (block_size <= 0xf) return 256 * (1 << (block_size - 8));
	return 0;
}

/*---------------------------------------------------------------------------*/
void wake_output(struct thread_ctx_s *ctx) {
	return;
}

/*---------------------------------------------------------------------------*/
size_t truncate16(u8_t *p, size_t *space, bool src_endianness, bool dst_endianness)
{
	u8_t *q;
	size_t i;

	*space = (*space / 3) * 3;
	q = (src_endianness) ? p + 1 : p;

	if (src_endianness == dst_endianness) {
		for (i = 0; i < *space; i += 3) {
			*(p++) = *(q++);
			*(p++) = *(q++);
			q++;
		}
	}
	else {
		for (i = 0; i < *space; i += 3) {
			*(p++) = *(q + 1);
			*(p++) = *(q);
			q += 3;
		}
	}

	return (*space * 2) / 3;
}

#define MAX_VAL16 0x7fffffffLL
#define MAX_VAL24 0x7fffffffffLL
#define MAX_VAL32 0x7fffffffffffLL
/*---------------------------------------------------------------------------*/
size_t apply_gain(void *p, size_t *space, u8_t inc, bool endianness, u32_t gain)
{
	size_t i;

	if (!gain || gain == 65536) return *space;


	*space = (*space / inc) * inc;

	switch (inc) {
		case 2: {
			s16_t *buf = p;
			s64_t sample;

			if (endianness) {
				for (i = 0; i < *space/inc; i++, buf++) {
					sample = *buf * (s64_t) gain;
					if (sample > MAX_VAL16) sample = MAX_VAL16;
					else if (sample < -MAX_VAL16) sample = -MAX_VAL16;
					*buf = sample >> 16;
			   }
			}
			else {
				for (i = 0; i < *space/inc; i++, buf++) {
					sample = (s16_t) (((*buf << 8) & 0xff00) | ((*buf >> 8) & 0xff)) * (s64_t) gain;
					if (sample > MAX_VAL16) sample = MAX_VAL16;
					else if (sample < -MAX_VAL16) sample = -MAX_VAL16;
					sample >>= 16;
					*buf = ((sample >> 8) & 0xff) | ((sample << 8) & 0xff00);
				}
			}
			break;
		}
		case 3: {
			u8_t *buf = p;
			s64_t sample;

			// for 24 bits samples, first put the sample in the 3 upper bytes
			if (endianness) {
				for (i = 0; i < *space/inc; i++, buf += 3) {
					sample = (s32_t) ((((u32_t) *buf) << 8) | ((u32_t) *(buf+1) << 16) | ((u32_t) *(buf+2) << 24)) * (s64_t) gain;
					if (sample > MAX_VAL32) sample = MAX_VAL32;
					else if (sample < -MAX_VAL32) sample = -MAX_VAL32;
					sample >>= 16+8;
					*buf = sample;
					*(buf+1) = sample >> 8;
					*(buf+2) = sample >> 16;
				}
			}
			else {
				for (i = 0; i < *space/inc; i++, buf += 3) {
					sample = (s32_t) ((((u32_t) *buf) << 24) | ((u32_t) *(buf+1) << 16) | ((u32_t) *(buf+2) << 8)) * (s64_t) gain;
					if (sample > MAX_VAL32) sample = MAX_VAL32;
					else if (sample < -MAX_VAL32) sample = -MAX_VAL32;
					sample >>= 16+8;
					*buf = sample >> 16;
					*(buf+1) = sample >> 8;
					*(buf+2) = sample;
				}
		   }
			break;
		}
		case 4: {
			s32_t *buf = p;
			s64_t sample;

			if (endianness) {
				for (i = 0; i < *space/inc; i++, buf++) {
					sample = *buf * (s64_t) gain;
					if (sample > MAX_VAL32) sample = MAX_VAL32;
					else if (sample < -MAX_VAL32) sample = -MAX_VAL32;
					*buf = sample >> 16;
				}
			}
			else {
				for (i = 0; i < *space/inc; i++, buf++) {
					sample = *buf * (s64_t) gain;
					if (sample > MAX_VAL32) sample = MAX_VAL32;
					else if (sample < -MAX_VAL32) sample = -MAX_VAL32;
					sample >>= 16;
					*buf = ((sample >> 24) & 0xff) | ((sample >> 8) & 0xff00) | ((sample << 8) & 0xff0000) | ((sample << 24) & 0xff000000);
				}
			}
			break;
		}
	}

	return *space;
}


/*---------------------------------------------------------------------------*/
size_t _change_endianness(u8_t *p, size_t *space, u8_t inc)
{
	int j;
	size_t i;
	u8_t buf[4];

	*space = (*space / inc) * inc;
	for (i = 0; i < *space; i += inc) {
			for (j = 0; j < inc; j++) buf[inc-1-j] = *(p+j);
			for (j = 0; j < inc; j++) *(p++) = buf[j];
	}

	return *space;
}

/*---------------------------------------------------------------------------*/
size_t change_endianness(u8_t *p, size_t *space, u8_t inc)
{
	size_t i;
	u8_t buf, buf2;

	*space = (*space / inc) * inc;
	i = *space;

	switch (inc) {
		case 1: break;
		case 2: {
			while (i) {
				buf = *p;
				*p = *(p+1);
				*(p+1) = buf;
				i -= 2;
				p += 2;
			}
		}
		case 3: {
			while (i) {
				buf = *p;
				*p = *(p+2);
				*(p+2) = buf;
				i -= 3;
				p += 3;
			}
		}
		 case 4: {
			while (i)	{
				buf = *p;
				buf2 = *(p+1);
				*p = *(p+3);
				*(p+1) = *(p+2);
				*(p+2) = buf2;
				*(p+3) = buf;
				i -= 4;
				p += 4;
			}
		}
	}

	return *space;
}

/*---------------------------------------------------------------------------*/
static void output_thru_thread(struct thread_ctx_s *ctx) {

	while (ctx->mr_running) {
		size_t	space, nb_write;
		unsigned sleep_time;
		out_ctx_t *out;

		LOCK_S;
		out = &ctx->out_ctx[ctx->out_idx];

		if (_buf_used(ctx->streambuf)) {
			bool ready = true;
			nb_write = space = _buf_cont_read(ctx->streambuf);

			// open the buffer if needed (should be opened in slimproto)
			if (!out->write_file) {
				char buf[SQ_STR_LENGTH];

				sprintf(buf, "%s/%s", ctx->config.buffer_dir, out->buf_name);
				out->write_file = fopen(buf, "wb");
				out->write_count = out->write_count_t = 0;
				LOG_ERROR("[%p]: write file not opened %s", ctx, buf);
			}

			// LMS will need to wait for the player to consume data ...
			if (out->remote && ctx->config.stream_pacing_size != -1 && (out->write_count - out->read_count) > (u32_t) ctx->config.stream_pacing_size) {
				UNLOCK_S;
				LOG_DEBUG("[%p] pacing (%u)", ctx, out->write_count - out->read_count);
				usleep(100000);
				continue;
			}

			/*
			Re-size buffer if needed. This is disabled if the streaming mode is
			to require the whole file to be stored. Note that out->file_size is
			set *after* the whole file is acquired at the end of this routine,
			which does not conflict with this test or is set in sq_get_info if
			duration is 0 (live stream)	which is the desired behavior
			*/
			if (out->file_size != HTTP_BUFFERED && ctx->config.buffer_limit != -1 && out->write_count > (u32_t) ctx->config.buffer_limit) {
				u8_t *buf;
				u32_t n;
				int rc;
				struct stat Status;

				// LMS will need to wait for the player to consume data ...
				if ((out->write_count - out->read_count) > (u32_t) ctx->config.buffer_limit / 2) {
					UNLOCK_S;
					usleep(100000);
					continue;
				}

				LOCK_O;
				out->write_count -= ctx->config.buffer_limit / 4;
				out->read_count -= ctx->config.buffer_limit / 4;

				buf = malloc(2L*1024L*1024L);
				fseek(out->write_file, 0, SEEK_SET);
				fseek(out->read_file, ctx->config.buffer_limit / 4, SEEK_SET);
				for (n = 0; n < out->write_count;) {
					u32_t b;
					b = fread(buf, 1, 2L*1024L*1024L, out->read_file);
					fwrite(buf, 1, b, out->write_file);
					n += b;
				}
				free(buf);
				fflush(out->write_file);

				fseek(out->read_file, out->read_count, SEEK_SET);
				rc = fresize(out->write_file, out->write_count);
				fstat(fileno(out->write_file), &Status);
				LOG_INFO("[%p]: re-sizing w:%d r:%d rp:%d ws:%d rc:%d", ctx,
						  out->write_count + ctx->config.buffer_limit /4,
						  out->read_count + ctx->config.buffer_limit /4,
						  ftell(out->read_file), Status.st_size, rc );
				UNLOCK_O;
			}

			// some file format require the re-insertion of headers
			if (!out->write_count) {
				// flac case
				if (!strcmp(out->ext, "flac")) {
					if (space >= FLAC_RECV_MIN) {
						if (strncmp(_buf_readp(ctx->streambuf), "fLaC", 4) && ctx->config.flac_header != FLAC_NO_HEADER) {
							flac_frame_t *frame;
							flac_streaminfo_t *streaminfo;
							u32_t rate;
							u8_t sample_size, channels;
							u16_t block_size = 0;

							frame = (flac_frame_t*) _buf_readp(ctx->streambuf);
							if (FLAC_GET_FRAME_TAG(frame->tag) != FLAC_TAG) {
								LOG_ERROR("[%p]: no header and not a frame ...", ctx);
								ready = true;
							}
							else {
								rate = FLAC_CODED_RATES[FLAC_GET_FRAME_RATE(frame->bsize_rate)];
								sample_size = FLAC_CODED_SAMPLE_SIZE[FLAC_GET_FRAME_SAMPLE_SIZE(frame->channels_sample_size)];
								channels = FLAC_CODED_CHANNELS[FLAC_GET_FRAME_CHANNEL(frame->channels_sample_size)];
								streaminfo = malloc(sizeof(flac_streaminfo_t));
								if (ctx->config.flac_header == FLAC_NORMAL_HEADER)
									memcpy(streaminfo, &FLAC_NORMAL_STREAMINFO, sizeof(flac_streaminfo_t));
								else
									memcpy(streaminfo, &FLAC_FULL_STREAMINFO, sizeof(flac_streaminfo_t));

								if (!FLAC_GET_BLOCK_STRATEGY(frame->tag)) {
									block_size = flac_block_size(FLAC_GET_BLOCK_SIZE(frame->bsize_rate));
									if (block_size) {
										streaminfo->min_block_size[0] = streaminfo->max_block_size[0] = BYTE_3(block_size);
										streaminfo->min_block_size[1] = streaminfo->max_block_size[1] = BYTE_4(block_size);
									}
									else {
										LOG_WARN("[%p]: unhandled blocksize %d, using variable", ctx, frame->tag);
									}
								}

								streaminfo->combo[0] = BYTE_1(FLAC_COMBO(rate, channels, sample_size));
								streaminfo->combo[1] = BYTE_2(FLAC_COMBO(rate, channels, sample_size));
								streaminfo->combo[2] = BYTE_3(FLAC_COMBO(rate, channels, sample_size));
								streaminfo->combo[3] = BYTE_4(FLAC_COMBO(rate, channels, sample_size));
								out->write_count = fwrite(&flac_header, 1, sizeof(flac_header), out->write_file);
								out->write_count += fwrite(streaminfo, 1, sizeof(flac_streaminfo_t), out->write_file);
								out->write_count_t = out->write_count;
								LOG_INFO("[%p]: flac header ch:%d, s:%d, r:%d, b:%d", ctx, channels, sample_size, rate, block_size);
								if (!rate || !sample_size || !channels) {
									LOG_ERROR("[%p]: wrong header %d %d %d", rate, channels, sample_size);
								}
								free(streaminfo);
							}
						}
					}
					else ready = false;
				}
			}

			/*
			PCM selected, if source format is endian = 1 (wav), then byte
			re-ordering	is needed as PCM is endian = 0
			There might be a need for 24 bits compacting as well
			*/
			if (!strcmp(out->ext, "pcm")) {
				u8_t *p;

				/*
				!!! only if LMS < 7.9.x !!!
				if the source file is an AIF, there is a blocksize + offset of
				8 bytes, just skip them	but also need to realign buffer to
				sample_size boundary.
				NB: assumes there are at least 8 bytes in the buffer
				!!! only if LMS < 7.9.x !!!
				*/
				if (!out->write_count && !out->endianness && ctx->aiff_header && (out->src_format == 'i')) {
					_buf_move(ctx->streambuf, 8);
					 nb_write = space -= 8;
				}

				p = _buf_readp(ctx->streambuf);

				// apply gain if any
				nb_write = apply_gain(p, &space, out->sample_size / 8, out->endianness, out->replay_gain);

				// 3 bytes but truncate that to 2 bytes
				if (out->sample_size == 24 && (ctx->config.L24_format == L24_TRUNC_16 ||
											   ctx->config.L24_format == L24_TRUNC_16_PCM)) {
					nb_write = truncate16(p, &space, out->endianness, 0);
				}
				// 2 or 4 bytes or 3 bytes with no packing, but change endianness
				else if (out->endianness && (out->sample_size != 24 || (out->sample_size == 24 && ctx->config.L24_format == L24_PACKED))) {
						nb_write = change_endianness(p, &space, out->sample_size / 8);
				}

				// 3 bytes with packing required, 2 channels
				if (out->sample_size == 24 && ctx->config.L24_format == L24_PACKED_LPCM && out->channels == 2) {
					u32_t i;
					u8_t j, buf[12];

					nb_write = space = (space / 12) * 12;
					for (i = 0; i < space; i += 12) {
						// order after that should be L0T,L0M,L0B,R0T,R0M,R0B,L1T,L1M,L1B,R1T,R1M,R1B
						if (out->endianness) for (j = 0; j < 12; j += 3) {
							buf[j] = *(p+j+2);
							buf[j+1] = *(p+j+1);
							buf[j+2] = *(p+j);
						}
						else for (j = 0; j < 12; j++) buf[j] = *(p+j);
						// L0T,L0M & R0T,R0M
						*p++ = buf[0]; *p++ = buf[1];
						*p++ = buf[3]; *p++ = buf[4];
						// L1T,L1M & R1T,R1M
						*p++ = buf[6]; *p++ = buf[7];
						*p++ = buf[9]; *p++ = buf[10];
						// L0B, R0B, L1B, R1B
						*p++ = buf[2]; *p++ = buf[5]; *p++ = buf[8]; *p++ = buf[11];
						// after that R0T,R0M,L0T,L0M,R1T,R1M,L1T,L1M,R0B,L0B,R1B,L1B
					}
				}

				// 3 bytes with packing required, 1 channel
				if (out->sample_size == 24 && ctx->config.L24_format == L24_PACKED_LPCM && out->channels == 1) {
					u32_t i;
					u8_t j, buf[6];

					nb_write = space = (space / 6) * 6;
					for (i = 0; i < space; i += 6) {
						// order after that should be C0T,C0M,C0B,C1T,C1M,C1B
						if (out->endianness) for (j = 0; j < 6; j += 3) {
							buf[j] = *(p+j+2);
							buf[j+1] = *(p+j+1);
							buf[j+2] = *(p+j);
						}
						else for (j = 0; j < 6; j++) buf[j] = *(p+j);
						// C0T,C0M,C1,C1M
						*p++ = buf[0]; *p++ = buf[1];
						*p++ = buf[3]; *p++ = buf[4];
						// C0B, C1B
						*p++ = buf[2]; *p++ = buf[5];
						// after that C0T,C0M,C1T,C1M,C0B,C1B
					}
				}

			}

			/*
			WAV selected, then a header must be added. Also, if source format
			is endian = 1 (wav), then byte ordering is correct otherwise, byte
			re-ordering is needed (opposite of PCM case) and offset mist be skipped
			*/
			if (!strcmp(out->ext, "wav")) {

				if (!out->write_count) {
					u8_t sample_size = (out->sample_size == 24 && ctx->config.L24_format == L24_TRUNC_16) ? 16 : out->sample_size;
					struct wave_header_s *header = malloc(sizeof(struct wave_header_s));

					memcpy(header, &wave_header, sizeof(struct wave_header_s));
					little16(&header->channels, out->channels);
					little16(&header->bits_per_sample, sample_size);
					little32(&header->sample_rate, out->sample_rate);
					little32(&header->byte_rate, out->sample_rate * out->channels * (sample_size / 8));
					little16(&header->block_align, out->channels * (sample_size / 8));
					little32(&header->subchunk2_size, out->raw_size);
					little32(&header->chunk_size, 36 + header->subchunk2_size);

					out->write_count = fwrite(header, 1, sizeof(struct wave_header_s), out->write_file);
					out->write_count_t = out->write_count;
					LOG_INFO("[%p]: wave header (r:%d f:%d)", ctx, out->raw_size, out->file_size);
					free(header);

					/*
					!!! only if LMS < 7.9.x !!!
					if the source file is an AIF, there is a blocksize + offset of
					8 bytes, just skip them	but also need to realign buffer to
					sample_size boundary.
					NB: assumes there are at least 8 bytes in the buffer
					!!! only if LMS < 7.9.x !!!
					*/
					if (!out->endianness && ctx->aiff_header && (out->src_format == 'i')) {
						LOG_INFO("[%p]: stripping AIFF un-needed data", ctx);
						_buf_move(ctx->streambuf, 8);
						 nb_write = space -= 8;
					}
				}

				// apply gain if any
				nb_write = apply_gain(_buf_readp(ctx->streambuf), &space, out->sample_size / 8, out->endianness, out->replay_gain);

				// 3 bytes truncated to 2 bytes or simply change endianness
				if (out->sample_size == 24 && ctx->config.L24_format == L24_TRUNC_16) {
					nb_write = truncate16(_buf_readp(ctx->streambuf), &space, out->endianness, 1);
				}
				else if (!out->endianness) {
					nb_write = change_endianness(_buf_readp(ctx->streambuf), &space, out->sample_size / 8);
				}
			}

			/*
			AIF selected, then a header must be added. Also, if source format
			is endian = 0 (aiff), then byte ordering is correct otherwise, byte
			re-ordering is needed (same as PCM)
			*/
			if (!strcmp(out->ext, "aif")) {
				if (!out->write_count) {
					u8_t sample_size = (out->sample_size == 24 && ctx->config.L24_format == L24_TRUNC_16) ? 16 : out->sample_size;
					struct aiff_header_s *header = malloc(sizeof(struct aiff_header_s));
					div_t duration = div(out->duration, 1000);

					memcpy(header, &aiff_header, sizeof(struct aiff_header_s));
					big16(header->channels, out->channels);
					big16(header->sample_size, sample_size);
					big16(header->sample_rate_num, out->sample_rate);
					big32(&header->data_size, out->raw_size + 8);
					big32(&header->chunk_size, (out->raw_size+8+8) + (18+8) + 4);
					big32(&header->frames, duration.quot * out->sample_rate + (duration.rem * out->sample_rate) / 1000);

					out->write_count = fwrite(header, 1, sizeof(struct aiff_header_s) - AIFF_PAD_SIZE, out->write_file);
					out->write_count_t = out->write_count;
					LOG_INFO("[%p]: aiff header (r:%d f:%d)", ctx, out->raw_size, out->file_size);
					free(header);

					/*
					!!! only if LMS < 7.9.x !!!
					if the source file is an AIF, there is a blocksize + offset of
					8 bytes, just skip them	but also need to realign buffer to
					sample_size boundary.
					NB: assumes there are at least 8 bytes in the buffer
					!!! only if LMS < 7.9.x !!!
					*/
					if (!out->endianness && ctx->aiff_header && (out->src_format == 'i')) {
						LOG_INFO("[%p]: stripping AIFF un-needed data", ctx);
						_buf_move(ctx->streambuf, 8);
						 nb_write = space -= 8;
					}
				}

				// apply gain if any
				nb_write = apply_gain(_buf_readp(ctx->streambuf), &space, out->sample_size / 8, out->endianness, out->replay_gain);

				// 3 bytes truncated to 2 bytes or simply change indianess
				if (out->sample_size == 24 && ctx->config.L24_format == L24_TRUNC_16) {
					nb_write = truncate16(_buf_readp(ctx->streambuf), &space, out->endianness, 0);
				}
				else if (out->endianness) {
					nb_write = change_endianness(_buf_readp(ctx->streambuf), &space, out->sample_size / 8);
				}
			}

			// write in the file
			if (ready) {
				fwrite(_buf_readp(ctx->streambuf), 1, nb_write, out->write_file);
				fflush(out->write_file);
				_buf_inc_readp(ctx->streambuf, space);
				out->write_count += nb_write;
				out->write_count_t += nb_write;
			}

			sleep_time = 10000;
		} else sleep_time = 100000;

		// all done, time to close the file
		if (out->write_file && ctx->stream.state <= DISCONNECT &&
			(!_buf_used(ctx->streambuf) ||
			(out->sample_size == 24 && _buf_used(ctx->streambuf) < 6*out->channels) ||
			(out->file_size > 0 && (out->write_count_t >= out->file_size)))) {
			LOG_INFO("[%p] wrote total %Ld", ctx, out->write_count_t);
			fclose(out->write_file);
			if (out->file_size == HTTP_BUFFERED) out->file_size = out->write_count_t;
			out->write_file = NULL;

			UNLOCK_S;
			buf_flush(ctx->streambuf);
		}
		else {
			UNLOCK_S;
		}

		usleep(sleep_time);
	}
}

/*---------------------------------------------------------------------------*/
void output_mr_thread_init(unsigned output_buf_size, struct thread_ctx_s *ctx) {

	LOG_DEBUG("[%p] init output media renderer", ctx);

	ctx->mr_running = true;

#if LINUX || OSX || FREEBSD
	pthread_attr_t attr;
	pthread_attr_init(&attr);
	pthread_attr_setstacksize(&attr, PTHREAD_STACK_MIN + OUTPUT_THREAD_STACK_SIZE);
	pthread_create(&ctx->mr_thread, &attr, (void *(*)(void*)) &output_thru_thread, ctx);
	pthread_attr_destroy(&attr);
#endif
#if WIN
	ctx->mr_thread = CreateThread(NULL, OUTPUT_THREAD_STACK_SIZE, (LPTHREAD_START_ROUTINE)&output_thru_thread, ctx, 0, NULL);
#endif
}

/*---------------------------------------------------------------------------*/
void output_mr_close(struct thread_ctx_s *ctx) {
	LOG_INFO("[%p] close media renderer", ctx);

	LOCK_S;LOCK_O;
	ctx->mr_running = false;
	UNLOCK_S;UNLOCK_O;

#if LINUX || OSX || FREEBSD
	pthread_join(ctx->mr_thread, NULL);
#endif
}

/*---------------------------------------------------------------------------*/
void output_flush(struct thread_ctx_s *ctx) {
	int i;

	LOG_DEBUG("[%p]: flush output buffer", ctx);

	LOCK_S;LOCK_O;
	for (i = 0; i < 2; i++) {
		char buf[SQ_STR_LENGTH];

		if (ctx->out_ctx[i].read_file) {
			fclose(ctx->out_ctx[i].read_file);
			ctx->out_ctx[i].read_file = NULL;
		}

		if (ctx->out_ctx[i].write_file) {
			fclose(ctx->out_ctx[i].write_file);
			ctx->out_ctx[i].write_file = NULL;
		}

		if (!ctx->config.keep_buffer_file) {
			sprintf(buf, "%s/%s", ctx->config.buffer_dir, ctx->out_ctx[i].buf_name);
			remove(buf);
		}

		ctx->out_ctx[i].completed = false;

	}
	UNLOCK_S;UNLOCK_O;
}



