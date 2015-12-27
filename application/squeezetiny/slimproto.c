/*
 *  Squeezelite - lightweight headless squeezebox emulator
 *
 *  (c) Adrian Smith 2012-2014, triode1@btinternet.com
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

/* TODO
- maybe one single CLI socket for all machines
*/

#include "squeezelite.h"
#include "slimproto.h"

#define PORT 3483
#define MAXBUF 4096

#if SL_LITTLE_ENDIAN
#define LOCAL_PLAYER_IP   0x0100007f // 127.0.0.1
#define LOCAL_PLAYER_PORT 0x9b0d     // 3483
#else
#define LOCAL_PLAYER_IP   0x7f000001 // 127.0.0.1
#define LOCAL_PLAYER_PORT 0x0d9b     // 3483
#endif

static log_level loglevel = lWARN;

#define LOCK_S   mutex_lock(ctx->streambuf->mutex)
#define UNLOCK_S mutex_unlock(ctx->streambuf->mutex)
#if 0
#define LOCK_O   mutex_lock(ctx->outputbuf->mutex)
#define UNLOCK_O mutex_unlock(ctx->outputbuf->mutex)
#else
#define LOCK_O
#define UNLOCK_O
#endif
#define LOCK_D   mutex_lock(ctx->decode.mutex)
#define UNLOCK_D mutex_unlock(ctx->decode.mutex)
#define LOCK_P   mutex_lock(ctx->mutex)
#define UNLOCK_P mutex_unlock(ctx->mutex)

static u8_t 	pcm_sample_size[] = { 8, 16, 24, 32 };
static u32_t 	pcm_sample_rate[] = { 11025, 22050, 32000, 44100, 48000,
									  8000, 12000, 16000, 24000, 96000, 88200,
									  176400, 192000, 352800, 384000 };
static u8_t		pcm_channels[] = { 1, 2 };

#if 0
/*---------------------------------------------------------------------------*/
static bool get_header_urn(char *header, int header_len, char *urn)
{
	char *p1, *p2;
	int len;

	p1 = strstr(header, "GET") + 3;
	if (p1) {
		while (*p1++ == ' ');
		p2 = strchr(p1, ' ');
		len = p2-p1;
		strncpy(urn, p1, len);
		urn[len] = '\0';
		return true;
	}

	urn[0] = '\0';
	return false;
}
#endif

/*---------------------------------------------------------------------------*/
bool ctx_callback(struct thread_ctx_s *ctx, sq_action_t action, u8_t *cookie, void *param)
{
	bool rc = false;

	if (ctx->callback) rc = ctx->callback(ctx->self, ctx->MR, action, cookie, param);
	return rc;
}


/*---------------------------------------------------------------------------*/
void send_packet(u8_t *packet, size_t len, sockfd sock) {
	u8_t *ptr = packet;
	unsigned try = 0;
	ssize_t n;

	while (len) {
		n = send(sock, ptr, len, MSG_NOSIGNAL);
		if (n <= 0) {
			if (n < 0 && last_error() == ERROR_WOULDBLOCK && try < 10) {
				LOG_DEBUG("retrying (%d) writing to socket", ++try);
				usleep(1000);
				continue;
			}
			LOG_WARN("failed writing to socket: %s", strerror(last_error()));
			return;
		}
		ptr += n;
		len -= n;
	}
}

/*---------------------------------------------------------------------------*/
static void sendHELO(bool reconnect, const char *fixed_cap, const char *var_cap, u8_t mac[6], struct thread_ctx_s *ctx) {
	const char *base_cap = "Model=squeezelite,ModelName=SqueezeLite,AccuratePlayPoints=0,HasDigitalOut=0";
	struct HELO_packet pkt;

	memset(&pkt, 0, sizeof(pkt));
	memcpy(&pkt.opcode, "HELO", 4);
	pkt.length = htonl(sizeof(struct HELO_packet) - 8 + strlen(base_cap) + strlen(fixed_cap) + strlen(var_cap));
	pkt.deviceid = 12; // squeezeplay
	pkt.revision = 0;
	packn(&pkt.wlan_channellist, reconnect ? 0x4000 : 0x0000);
	packN(&pkt.bytes_received_H, (u64_t)ctx->status.stream_bytes >> 32);
	packN(&pkt.bytes_received_L, (u64_t)ctx->status.stream_bytes & 0xffffffff);
	memcpy(pkt.mac, mac, 6);

	LOG_DEBUG("[%p] mac: %02x:%02x:%02x:%02x:%02x:%02x", ctx, pkt.mac[0], pkt.mac[1], pkt.mac[2], pkt.mac[3], pkt.mac[4], pkt.mac[5]);
	LOG_INFO("[%p] cap: %s%s%s", ctx, base_cap, fixed_cap, var_cap);

	send_packet((u8_t *)&pkt, sizeof(pkt), ctx->sock);
	send_packet((u8_t *)base_cap, strlen(base_cap), ctx->sock);
	send_packet((u8_t *)fixed_cap, strlen(fixed_cap), ctx->sock);
	send_packet((u8_t *)var_cap, strlen(var_cap), ctx->sock);
}

/*---------------------------------------------------------------------------*/
static void sendSTAT(const char *event, u32_t server_timestamp, struct thread_ctx_s *ctx) {
	struct STAT_packet pkt;
	u32_t now = gettime_ms();

	memset(&pkt, 0, sizeof(struct STAT_packet));
	memcpy(&pkt.opcode, "STAT", 4);
	pkt.length = htonl(sizeof(struct STAT_packet) - 8);
	memcpy(&pkt.event, event, 4);
	// num_crlf
	// mas_initialized; mas_mode;
	packN(&pkt.stream_buffer_fullness, ctx->status.stream_full);
	packN(&pkt.stream_buffer_size, ctx->status.stream_size);
	packN(&pkt.bytes_received_H, (u64_t)ctx->status.stream_bytes >> 32);
	packN(&pkt.bytes_received_L, (u64_t)ctx->status.stream_bytes & 0xffffffff);
	pkt.signal_strength = 0xffff;
	packN(&pkt.jiffies, now);
	packN(&pkt.output_buffer_size, 10);
	packN(&pkt.output_buffer_fullness, 10);
	packN(&pkt.elapsed_seconds, ctx->status.ms_played / 1000);
	// voltage;
	packN(&pkt.elapsed_milliseconds, ctx->status.ms_played);
	pkt.server_timestamp = server_timestamp; // keep this is server format - don't unpack/pack

	if (strcmp(event, "STMt")) {
		LOG_INFO("[%p]: STAT:[%s] msplayed %d", ctx, event, ctx->status.ms_played);
    }
	LOG_DEBUG("[%p] STAT: %s", ctx, event);
	LOG_SDEBUG("[%p] received bytesL: %u streambuf: %u calc elapsed: %u real elapsed: %u ",
				   ctx, (u32_t)ctx->status.stream_bytes, ctx->status.stream_full, ctx->status.ms_played, now - ctx->status.stream_start);

	send_packet((u8_t *)&pkt, sizeof(pkt), ctx->sock);
}

/*---------------------------------------------------------------------------*/
static void sendDSCO(disconnect_code disconnect, sockfd sock) {
	struct DSCO_packet pkt;

	memset(&pkt, 0, sizeof(pkt));
	memcpy(&pkt.opcode, "DSCO", 4);
	pkt.length = htonl(sizeof(pkt) - 8);
	pkt.reason = disconnect & 0xFF;

	LOG_DEBUG("DSCO: %d", disconnect);

	send_packet((u8_t *)&pkt, sizeof(pkt), sock);
}

/*---------------------------------------------------------------------------*/
static void sendRESP(const char *header, size_t len, sockfd sock) {
	struct RESP_header pkt_header;

	memset(&pkt_header, 0, sizeof(pkt_header));
	memcpy(&pkt_header.opcode, "RESP", 4);
	pkt_header.length = htonl(sizeof(pkt_header) + len - 8);

	LOG_DEBUG("RESP", NULL);

	send_packet((u8_t *)&pkt_header, sizeof(pkt_header), sock);
	send_packet((u8_t *)header, len, sock);
}

/*---------------------------------------------------------------------------*/
static void sendMETA(const char *meta, size_t len, sockfd sock) {
	struct META_header pkt_header;

	memset(&pkt_header, 0, sizeof(pkt_header));
	memcpy(&pkt_header.opcode, "META", 4);
	pkt_header.length = htonl(sizeof(pkt_header) + len - 8);

	LOG_DEBUG("META", NULL);

	send_packet((u8_t *)&pkt_header, sizeof(pkt_header), sock);
	send_packet((u8_t *)meta, len, sock);
}

/*---------------------------------------------------------------------------*/
static void sendSETDName(const char *name, sockfd sock) {
	struct SETD_header pkt_header;

	memset(&pkt_header, 0, sizeof(pkt_header));
	memcpy(&pkt_header.opcode, "SETD", 4);

	pkt_header.id = 0; // id 0 is playername S:P:Squeezebox2
	pkt_header.length = htonl(sizeof(pkt_header) + strlen(name) + 1 - 8);

	LOG_DEBUG("set playername: %s", name);

	send_packet((u8_t *)&pkt_header, sizeof(pkt_header), sock);
	send_packet((u8_t *)name, strlen(name) + 1, sock);
}


/*---------------------------------------------------------------------------*/
static void process_strm(u8_t *pkt, int len, struct thread_ctx_s *ctx) {
	struct strm_packet *strm = (struct strm_packet *)pkt;

	if (strm->command != 't' && strm->command != 'q') {
		LOG_INFO("[%p] strm command %c", ctx, strm->command);
	}
	else {
		LOG_DEBUG("[%p] strm command %c", ctx, strm->command);
    }

	switch(strm->command) {
	case 't':
		sendSTAT("STMt", strm->replay_gain, ctx); // STMt replay_gain is no longer used to track latency, but support it
		break;
	case 'f':
	case 'q':
		decode_flush(ctx);
		output_flush(ctx);
		ctx->play_running = ctx-> track_ended = false;
		ctx->track_status = TRACK_STOPPED;
		ctx->status.ms_played = ctx->ms_played = 0;
		if (stream_disconnect(ctx))
			if (strm->command == 'f') sendSTAT("STMf", 0, ctx);
		buf_flush(ctx->streambuf);
		ctx_callback(ctx, SQ_STOP, NULL, NULL);
		break;
	case 'p':
		{
			unsigned interval = unpackN(&strm->replay_gain);

			ctx->ms_pause = interval;
			ctx->start_at = (interval) ? gettime_ms() + interval : 0;
			ctx->track_status = TRACK_PAUSED;
			ctx_callback(ctx, SQ_PAUSE, NULL, NULL);
			if (!interval) {
				sendSTAT("STMp", 0, ctx);
			}
			LOG_INFO("[%p] pause interval: %u", ctx, interval);
		}
		break;
	case 'a':
		{
			unsigned interval = unpackN(&strm->replay_gain);
			ctx_callback(ctx, SQ_SEEK, NULL, &interval);
			LOG_INFO("[%p]skip ahead interval: %u", ctx, interval);
		}
		break;
	case 'u':
		{
			unsigned jiffies = unpackN(&strm->replay_gain);

			LOG_INFO("[%p] unpause at: %u now: %u", ctx, jiffies, gettime_ms());
			ctx->start_at = jiffies;
			if (!jiffies) {
				// this is an unpause after for an autostart = 0 or 2
				if (ctx->track_status == TRACK_STOPPED) {
					ctx->track_new = true;
					ctx->track_start_time = gettime_ms();
				}
				if (ctx->track_status != TRACK_STARTED) ctx_callback(ctx, SQ_UNPAUSE, NULL, NULL);
				ctx->track_status = TRACK_STARTED;
			}
			sendSTAT("STMr", 0, ctx);
		}
		break;
	case 's':
		{
			sq_seturi_t	uri;
			unsigned header_len = len - sizeof(struct strm_packet);
			char *header = (char *)(pkt + sizeof(struct strm_packet));
			in_addr_t ip = (in_addr_t)strm->server_ip; // keep in network byte order
			u16_t port = strm->server_port; // keep in network byte order
			if (ip == 0) ip = ctx->slimproto_ip;

			LOG_INFO("[%p], strm s autostart: %c transition period: %u transition type: %u codec: %c",
					  ctx, strm->autostart, strm->transition_period, strm->transition_type - '0', strm->format);

			ctx->autostart = strm->autostart - '0';
			sendSTAT("STMf", 0, ctx);
			if (header_len > MAX_HEADER -1) {
				LOG_WARN("[%p] header too long: %u", ctx, header_len);
				break;
			}

			if (strm->format != '?') {
				codec_open(strm->format, strm->pcm_sample_size, strm->pcm_sample_rate, strm->pcm_channels, strm->pcm_endianness, ctx);
			} else if (ctx->autostart >= 2) {
				// extension to slimproto to allow server to detect codec from response header and send back in codc message
				LOG_WARN("[%p] streaming unknown codec", ctx);
			} else {
				LOG_WARN("[%p] unknown codec requires autostart >= 2", ctx);
				break;
			}

			 uri.sample_size = (strm->pcm_sample_size != '?') ? pcm_sample_size[strm->pcm_sample_size - '0'] : 0xff;
			 uri.sample_rate = (strm->pcm_sample_rate != '?') ? pcm_sample_rate[strm->pcm_sample_rate - '0'] : 0xff;
			 uri.channels = (strm->pcm_channels != '?') ? pcm_channels[strm->pcm_channels - '1'] : 0xff;
			 uri.endianness = (strm->pcm_endianness != '?') ? strm->pcm_endianness - '0' : 0;
			 uri.codec = strm->format;

			 if (ctx->config.mode == SQ_STREAM)
			 {
				unsigned idx;
				bool rc;
				char buf[SQ_STR_LENGTH];

				// stream is proxied and then forwared to the renderer
				stream_sock(ip, port, header, header_len, strm->threshold * 1024, ctx->autostart >= 2, ctx);

				LOCK_S;LOCK_O;

				idx = ctx->out_idx = (ctx->out_idx + 1) & 0x01;

				if (ctx->out_ctx[idx].read_file) {
					LOG_WARN("[%p]: read file left open", ctx, ctx->out_ctx[idx].buf_name);
					fclose(ctx->out_ctx[idx].read_file);
					ctx->out_ctx[idx].read_file = NULL;
				}
				ctx->out_ctx[idx].read_count = ctx->out_ctx[idx].read_count_t =
											   ctx->out_ctx[idx].close_count = 0;

				if (ctx->out_ctx[idx].write_file) {
					LOG_WARN("[%p]: write file left open", ctx, ctx->out_ctx[idx].buf_name);
					fclose(ctx->out_ctx[idx].write_file);
				}

				// open the write_file here as some players react very fast
				sprintf(buf, "%s/%s", ctx->config.buffer_dir, ctx->out_ctx[idx].buf_name);
				ctx->out_ctx[idx].write_file = fopen(buf, "wb");
				ctx->out_ctx[idx].write_count = ctx->out_ctx[idx].write_count_t = 0;

				ctx->out_ctx[idx].sample_size = uri.sample_size;
				ctx->out_ctx[idx].sample_rate = uri.sample_rate;
				ctx->out_ctx[idx].endianness = uri.endianness;
				ctx->out_ctx[idx].channels = uri.channels;
				ctx->out_ctx[idx].codec = uri.codec;
				ctx->out_ctx[idx].replay_gain = unpackN(&strm->replay_gain);
				strcpy(uri.name, ctx->out_ctx[idx].buf_name);

				if (ctx->play_running || ctx->track_status != TRACK_STOPPED) {
					rc = ctx_callback(ctx, SQ_SETNEXTURI, NULL, &uri);
				}
				else {
					rc = ctx_callback(ctx, SQ_SETURI, NULL, &uri);
					ctx->track_ended = false;
					ctx->track_status = TRACK_STOPPED;
					ctx->track_new = true;
					ctx->status.ms_played = ctx->ms_played = 0;
					ctx->read_to = ctx->read_ended = false;
				}

				if (rc) {
					strcpy(ctx->out_ctx[idx].content_type, uri.content_type);
					strcpy(ctx->out_ctx[idx].proto_info, uri.proto_info);
					strcpy(ctx->out_ctx[idx].ext, uri.ext);
					ctx->out_ctx[idx].file_size = uri.file_size;
					ctx->out_ctx[idx].duration = uri.duration;
					ctx->out_ctx[idx].src_format = uri.src_format;
					ctx->out_ctx[idx].remote = uri.remote;
					ctx->out_ctx[idx].live = uri.remote && (uri.duration == 0);
					ctx->out_ctx[idx].track_hash = uri.track_hash;
					sq_set_sizes(ctx->out_ctx + idx);
				}
				else ctx->decode.state = DECODE_ERROR;

				UNLOCK_S;UNLOCK_O;
			}

			sendSTAT("STMc", 0, ctx);
			ctx->sentSTMu = ctx->sentSTMo = ctx->sentSTMl = ctx->sentSTMd = false;
		}
		break;
	default:
		LOG_WARN("[%p] unhandled strm %c", ctx, strm->command);
		break;
	}
}

/*---------------------------------------------------------------------------*/
static void process_cont(u8_t *pkt, int len, struct thread_ctx_s *ctx) {
	struct cont_packet *cont = (struct cont_packet *)pkt;

	cont->metaint = unpackN(&cont->metaint);

	LOG_DEBUG("cont metaint: %u loop: %u", cont->metaint, cont->loop);

	if (ctx->autostart > 1) {
		ctx->autostart -= 2;
		LOCK_S;
		if (ctx->stream.state == STREAMING_WAIT) {
			ctx->stream.state = STREAMING_BUFFERING;
			ctx->stream.meta_interval = ctx->stream.meta_next = cont->metaint;
		}
		UNLOCK_S;
		wake_controller(ctx);
	}
}

/*---------------------------------------------------------------------------*/
static void process_codc(u8_t *pkt, int len, struct thread_ctx_s *ctx) {
	struct codc_packet *codc = (struct codc_packet *)pkt;

	LOG_DEBUG("[%p] codc: %c", ctx, codc->format);
	codec_open(codc->format, codc->pcm_sample_size, codc->pcm_sample_rate, codc->pcm_channels, codc->pcm_endianness, ctx);
}

/*---------------------------------------------------------------------------*/
static void process_aude(u8_t *pkt, int len, struct thread_ctx_s *ctx) {
	struct aude_packet *aude = (struct aude_packet *)pkt;

	LOCK_O;
	ctx->on = (aude->enable_spdif) ? true : false;
	LOG_DEBUG("[%p] on/off using aude %d", ctx, ctx->on);

#if 0
	decode_flush(ctx);
	output_flush(ctx);
	ctx->play_running = ctx-> track_ended = false;
	ctx->track_status = TRACK_STOPPED;
	ctx->status.ms_played = ctx->ms_played = 0;
	stream_disconnect(ctx);
	buf_flush(ctx->streambuf);
#endif
	UNLOCK_O;

#if 0
	ctx_callback(ctx, SQ_STOP, NULL, NULL);
#endif
	ctx_callback(ctx, SQ_ONOFF, NULL, &ctx->on);
}

/*---------------------------------------------------------------------------*/
static void process_audg(u8_t *pkt, int len, struct thread_ctx_s *ctx) {
	struct audg_packet *audg = (struct audg_packet *)pkt;
	u16_t  gain;

	audg->old_gainL = unpackN(&audg->old_gainL);
	audg->old_gainR = unpackN(&audg->old_gainR);

	LOG_DEBUG("[%p] (old) audg gainL: %u gainR: %u", ctx, audg->old_gainL, audg->old_gainR);

	gain = (audg->old_gainL + audg->old_gainL) / 2;
	ctx_callback(ctx, SQ_VOLUME, NULL, (void*) &gain);
}

/*---------------------------------------------------------------------------*/
static void process_setd(u8_t *pkt, int len,struct thread_ctx_s *ctx) {
	struct setd_packet *setd = (struct setd_packet *)pkt;

	// handle player name query and change
	if (setd->id == 0) {
		if (len == 5) {
			if (strlen(ctx->player_name)) {
				sendSETDName(ctx->player_name, ctx->sock);
			}
		} else if (len > 5) {
			strncpy(ctx->player_name, setd->data, PLAYER_NAME_LEN);
			ctx->player_name[PLAYER_NAME_LEN] = '\0';
			LOG_DEBUG("[%p] set name: %s", ctx, setd->data);
			// confirm change to server
			sendSETDName(setd->data, ctx->sock);
		}
	}
}

/*---------------------------------------------------------------------------*/
static void process_ledc(u8_t *pkt, int len,struct thread_ctx_s *ctx) {
	LOG_DEBUG("[%p] ledc", ctx);
}


#define SYNC_CAP ",SyncgroupID="
#define SYNC_CAP_LEN 13

/*---------------------------------------------------------------------------*/
static void process_serv(u8_t *pkt, int len,struct thread_ctx_s *ctx) {
	struct serv_packet *serv = (struct serv_packet *)pkt;

	LOG_INFO("[%p] switch server", ctx);

	ctx->new_server = serv->server_ip;

	if (len - sizeof(struct serv_packet) == 10) {
		if (!ctx->new_server_cap) {
			ctx->new_server_cap = malloc(SYNC_CAP_LEN + 10 + 1);
		}
		ctx->new_server_cap[0] = '\0';
		strcat(ctx->new_server_cap, SYNC_CAP);
		strncat(ctx->new_server_cap, (const char *)(pkt + sizeof(struct serv_packet)), 10);
	} else {
		if (ctx->new_server_cap) {
			free(ctx->new_server_cap);
			ctx->new_server_cap = NULL;
		}
	}
}

/*---------------------------------------------------------------------------*/
static void process_vers(u8_t *pkt, int len,struct thread_ctx_s *ctx) {
	struct vers_packet *vers = (struct vers_packet *)pkt;

	LOG_DEBUG("[%p] version %s", ctx, vers->version);
}

struct handler {
	char opcode[6];
	void (*handler)(u8_t *, int, struct thread_ctx_s *);
};

static struct handler handlers[] = {
	{ "strm", process_strm },
	{ "cont", process_cont },
	{ "codc", process_codc },
	{ "aude", process_aude },
	{ "audg", process_audg },
	{ "setd", process_setd },
	{ "serv", process_serv },
	{ "ledc", process_ledc },
	{ "vers", process_vers },
	{ "",     NULL  },
};

/*---------------------------------------------------------------------------*/
static void process(u8_t *pack, int len, struct thread_ctx_s *ctx) {
	struct handler *h = handlers;
	while (h->handler && strncmp((char *)pack, h->opcode, 4)) { h++; }

	if (h->handler) {
		LOG_DEBUG("[%p] %s", ctx, h->opcode);
		h->handler(pack, len, ctx);
	} else {
		pack[4] = '\0';
		LOG_WARN("[%p] unhandled %s", ctx, (char *)pack);
	}
}

/*---------------------------------------------------------------------------*/
static void slimproto_run(struct thread_ctx_s *ctx) {
	int  expect = 0;
	int  got    = 0;
	u32_t now;
	event_handle ehandles[2];
	int timeouts = 0;

	ctx->aiff_header = false;
	if ((strstr(ctx->server_version, "7.7")) || strstr(ctx->server_version, "7.8")) ctx->aiff_header = true;

	set_readwake_handles(ehandles, ctx->sock, ctx->wake_e);

	while (ctx->running && !ctx->new_server) {

		bool wake = false;
		event_type ev;

		if ((ev = wait_readwake(ehandles, 1000)) != EVENT_TIMEOUT) {

			if (ev == EVENT_READ) {

				if (expect > 0) {
					int n = recv(ctx->sock, ctx->slim_run.buffer + got, expect, 0);
					if (n <= 0) {
						if (n < 0 && last_error() == ERROR_WOULDBLOCK) {
							continue;
						}
						LOG_WARN("[%p] error reading from socket: %s", ctx, n ? strerror(last_error()) : "closed");
						return;
					}
					expect -= n;
					got += n;
					if (expect == 0) {
						process(ctx->slim_run.buffer, got, ctx);
						got = 0;
					}
				} else if (expect == 0) {
					int n = recv(ctx->sock, ctx->slim_run.buffer + got, 2 - got, 0);
					if (n <= 0) {
						if (n < 0 && last_error() == ERROR_WOULDBLOCK) {
							continue;
						}
						LOG_WARN("[%p] error reading from socket: %s", ctx, n ? strerror(last_error()) : "closed");
						return;
					}
					got += n;
					if (got == 2) {
						expect = ctx->slim_run.buffer[0] << 8 | ctx->slim_run.buffer[1]; // length pack 'n'
						got = 0;
						if (expect > MAXBUF) {
							LOG_ERROR("[%p] FATAL: slimproto packet too big: %d > %d", ctx, expect, MAXBUF);
							return;
						}
					}
				} else {
					LOG_ERROR("[%p] FATAL: negative expect", ctx);
					return;
				}

			}

			if (ev == EVENT_WAKE) {
				wake = true;
			}

			timeouts = 0;

		} else if (++timeouts > 35) {

			// expect message from server every 5 seconds, but 30 seconds on mysb.com so timeout after 35 seconds
			LOG_WARN("[%p] No messages from server - connection dead", ctx);
			return;
		}

		// update playback state when woken or every 100ms
		now = gettime_ms();

		if (wake || now - ctx->slim_run.last > 100 || ctx->slim_run.last > now) {
			bool _sendSTMs = false;
			bool _sendDSCO = false;
			bool _sendRESP = false;
			bool _sendMETA = false;
			bool _sendSTMd = false;
			bool _sendSTMt = false;
			bool _sendSTMl = false;
			bool _sendSTMu = false;
			bool _sendSTMo = false;
			bool _sendSTMn = false;
			bool _stream_disconnect = false;
			disconnect_code disconnect_code;
			size_t header_len = 0;
			ctx->slim_run.last = now;

			LOCK_S;
			ctx->status.stream_full = _buf_used(ctx->streambuf);
			ctx->status.stream_size = ctx->streambuf->size;
			ctx->status.stream_bytes = ctx->stream.bytes;
			ctx->status.stream_state = ctx->stream.state;
			ctx->status.ms_played = ctx->ms_played;

			if (ctx->stream.state == DISCONNECT) {
				disconnect_code = ctx->stream.disconnect;
				ctx->stream.state = STOPPED;
				_sendDSCO = true;
			}

			if (!ctx->stream.sent_headers &&
				(ctx->stream.state == STREAMING_HTTP || ctx->stream.state == STREAMING_WAIT || ctx->stream.state == STREAMING_BUFFERING)) {
				header_len = ctx->stream.header_len;
				memcpy(ctx->slim_run.header, ctx->stream.header, header_len);
				_sendRESP = true;
				ctx->stream.sent_headers = true;
			}
			if (ctx->stream.meta_send) {
				header_len = ctx->stream.header_len;
				memcpy(ctx->slim_run.header, ctx->stream.header, header_len);
				_sendMETA = true;
				ctx->stream.meta_send = false;
			}
			UNLOCK_S;

			if (ctx->start_at && now > ctx->start_at) {
				LOG_INFO("[%p] start time elapsed %d %d", ctx, ctx->start_at, now);
				ctx->start_at = 0;
				if (ctx->track_status != TRACK_PAUSED) ctx->track_new = true;
				ctx->track_status = TRACK_STARTED;
				ctx_callback(ctx, SQ_PLAY, NULL, NULL);
			}

			// end of streaming
			if (!ctx->sentSTMu && ctx->status.stream_state <= DISCONNECT && ctx->track_ended) {
				_sendSTMu = true;
				ctx->sentSTMu = true;
				// not normal end can be error or user, can't know that
				if (!ctx->sentSTMd) {
					_sendSTMn = true;
					LOG_WARN("[%p]: unwanted stop, reporting error", ctx);
				}
				ctx->track_ended = false;
			}

			// should not happen in SQ_PROXY
			if (!ctx->sentSTMo && ctx->status.stream_state == STREAMING_HTTP && ctx->read_to) {
				_sendSTMo = true;
				ctx->sentSTMo = true;
				_stream_disconnect = true;
			}

			if ((ctx->status.stream_state == STREAMING_HTTP || ctx->status.stream_state == STREAMING_FILE) && !ctx->sentSTMl) {
				// autostart 2 and 3 require cont to be received first
				if (ctx->autostart == 0) {
					_sendSTMl = true;
					ctx->sentSTMl = true;
				 }
				 else if (ctx->track_status == TRACK_STOPPED) {
					 ctx_callback(ctx, SQ_PLAY, NULL, NULL);
					 LOCK_S;
					 ctx->track_status = TRACK_STARTED;
					 ctx->track_new = true;
					 ctx->track_start_time = now;
					 UNLOCK_S;
				 }
			}

			// few thing need to wait for the running to be confirmed by player
			if (ctx->play_running) {

				// either 1st track or end of track detected by the player
				if (ctx->track_new) {
					_sendSTMs = true;
					ctx->track_new = false;
					ctx->status.stream_start = ctx->track_start_time;
				}

				// send regular status update
				if (now - ctx->status.last > 1000) {
					_sendSTMt = true;
					ctx->status.last = now;
				}

				// last byte from streambuf has been sent, so time to request
				// another potential "next"
				if (ctx->read_ended) {
					_sendSTMd = true;
					ctx->sentSTMd = true;
					ctx->read_ended = false;
				}
			}

			if (ctx->decode.state == DECODE_ERROR) {
				_sendSTMn = true;
				_stream_disconnect = true;
			}

			if (_stream_disconnect) stream_disconnect(ctx);

			// send packets once locks released as packet sending can block
			if (_sendDSCO) sendDSCO(disconnect_code, ctx->sock);
			if (_sendSTMs) sendSTAT("STMs", 0, ctx);
			if (_sendSTMt) sendSTAT("STMt", 0, ctx);
			if (_sendSTMl) sendSTAT("STMl", 0, ctx);
			// 'd' BEFORE 'u'
			if (_sendSTMd) sendSTAT("STMd", 0, ctx);
			if (_sendSTMu) sendSTAT("STMu", 0, ctx);
			if (_sendSTMo) sendSTAT("STMo", 0, ctx);
			if (_sendSTMn) sendSTAT("STMn", 0, ctx);
			if (_sendRESP) sendRESP(ctx->slim_run.header, header_len, ctx->sock);
			if (_sendMETA) sendMETA(ctx->slim_run.header, header_len, ctx->sock);
		}
	}
}

 /*---------------------------------------------------------------------------*/
// called from other threads to wake state machine above
void wake_controller(struct thread_ctx_s *ctx) {
	wake_signal(ctx->wake_e);
}

void discover_server(struct thread_ctx_s *ctx) {
	struct sockaddr_in d;
	struct sockaddr_in s;
	char buf[32], vers[] = "VERS", port[] = "JSON";
	struct pollfd pollinfo;
	u8_t len;

	int disc_sock = socket(AF_INET, SOCK_DGRAM, 0);

	socklen_t enable = 1;
	setsockopt(disc_sock, SOL_SOCKET, SO_BROADCAST, (const void *)&enable, sizeof(enable));

	len = sprintf(buf,"e%s\xff%s", vers, port) + 1;
	*strchr(buf, 0xff) = '\0';

	memset(&d, 0, sizeof(d));
	d.sin_family = AF_INET;
	d.sin_port = htons(PORT);
	if (!ctx->slimproto_ip) d.sin_addr.s_addr = htonl(INADDR_BROADCAST);
	else d.sin_addr.s_addr = ctx->slimproto_ip;

	pollinfo.fd = disc_sock;
	pollinfo.events = POLLIN;

	do {
		LOG_DEBUG("[%p] sending discovery", ctx);
		memset(&s, 0, sizeof(s));

		if (sendto(disc_sock, buf, len, 0, (struct sockaddr *)&d, sizeof(d)) < 0) {
			LOG_WARN("[%p] error sending discovery", ctx);
		}

		if (poll(&pollinfo, 1, 5000) == 1) {
			char readbuf[32], *p;

			socklen_t slen = sizeof(s);
			memset(readbuf, 0, 32);
			recvfrom(disc_sock, readbuf, 32 - 1, 0, (struct sockaddr *)&s, &slen);

			if ((p = strstr(readbuf, vers)) != NULL) {
				p += strlen(vers);
				len = *p;
				strncpy(ctx->server_version, p + 1, min(SERVER_VERSION_LEN, *p));
				ctx->server_version[min(SERVER_VERSION_LEN, *p)] = '\0';
			}

			 if ((p = strstr(readbuf, port)) != NULL) {
				p += strlen(port);
				len = *p;
				strncpy(ctx->server_port, p + 1, min(5, *p));
				ctx->server_port[min(6, *p)] = '\0';
			}

			strcpy(ctx->server_ip, inet_ntoa(s.sin_addr));
			LOG_DEBUG("[%p] got response from: %s:%d", ctx, inet_ntoa(s.sin_addr), ntohs(s.sin_port));
		}
	} while (s.sin_addr.s_addr == 0 && ctx->running);

	closesocket(disc_sock);

	ctx->slimproto_ip = ctx->serv_addr.sin_addr.s_addr = s.sin_addr.s_addr;
}

/*---------------------------------------------------------------------------*/
static void slimproto(struct thread_ctx_s *ctx) {
	bool reconnect = false;
	unsigned failed_connect = 0;
	struct sockaddr_in cli_addr;

	mutex_create(ctx->mutex);
	mutex_create(ctx->cli_mutex);

	while (ctx->running) {

		if (ctx->new_server) {
			ctx->slimproto_ip = ctx->new_server;
			LOG_INFO("[%p] switching server to %s:%d", ctx, inet_ntoa(ctx->serv_addr.sin_addr), ntohs(ctx->serv_addr.sin_port));
			ctx->new_server = 0;
			reconnect = false;
		}

		ctx->sock = socket(AF_INET, SOCK_STREAM, 0);

		set_nonblock(ctx->sock);
		set_nosigpipe(ctx->sock);

		if (connect_timeout(ctx->sock, (struct sockaddr *) &ctx->serv_addr, sizeof(ctx->serv_addr), 5) != 0) {

			LOG_WARN("[%p] unable to connect to server %u", ctx, failed_connect);
			sleep(5);

			// rediscover server if it was not set at startup
			if (!ctx->server && ++failed_connect > 5) discover_server(ctx);

		} else {

			LOG_INFO("[%p] connected", ctx);

			ctx->var_cap[0] = '\0';
			failed_connect = 0;

			// add on any capablity to be sent to the new server
			if (ctx->new_server_cap) {
				strcat(ctx->var_cap, ctx->new_server_cap);
				free(ctx->new_server_cap);
				ctx->new_server_cap = NULL;
			}

			sendHELO(reconnect, ctx->fixed_cap, ctx->var_cap, ctx->mac, ctx);

			/*
			there could be a global CLIn socket for all the devices, but this
			would require a mutex to handle it to make sure command / response
			are sent in the right sequence ... at that time, let's use a CLI
			socket per machine
			*/
			// open CLI socket
			ctx->cli_sock = socket(AF_INET, SOCK_STREAM, 0);
			set_nonblock(ctx->cli_sock);
			set_nosigpipe(ctx->cli_sock);

			cli_addr.sin_family = AF_INET;
			cli_addr.sin_addr.s_addr = ctx->slimproto_ip;
			cli_addr.sin_port = htons(9090);

			if (connect_timeout(ctx->cli_sock, (struct sockaddr *) &cli_addr, sizeof(cli_addr), 1) != 0) {
				LOG_ERROR("[%p] unable to connect to server with cli %u", ctx, failed_connect);
			}

			slimproto_run(ctx);

			if (!reconnect) {
				reconnect = true;
			}

			usleep(100000);
		}

		closesocket(ctx->sock);
		closesocket(ctx->cli_sock);
		if (ctx->new_server_cap)	{
			free(ctx->new_server_cap);
			ctx->new_server_cap = NULL;
		}
	}

	mutex_destroy(ctx->mutex);
	mutex_destroy(ctx->cli_mutex);
}


/*---------------------------------------------------------------------------*/
void slimproto_close(struct thread_ctx_s *ctx) {
	LOG_INFO("[%p] slimproto stop for %s", ctx, ctx->player_name);
  	ctx->running = false;
	wake_controller(ctx);
#if LINUX || OSX || FREEBSD
	pthread_detach(ctx->thread);
#endif
}

/*---------------------------------------------------------------------------*/
void slimproto_loglevel(log_level level) {
	LOG_ERROR("slimproto change log", level);
	loglevel = level;
}

/*---------------------------------------------------------------------------*/
void slimproto_thread_init(char *server, u8_t mac[6], const char *name, const char *namefile, struct thread_ctx_s *ctx) {
	wake_create(ctx->wake_e);

	ctx->running = true;
	ctx->slimproto_ip = 0;
	ctx->slimproto_port = 0;
	ctx->sock = -1;

	if (server) {
		server_addr(server, &ctx->slimproto_ip, &ctx->slimproto_port);
		strncpy(ctx->server, server, SERVER_NAME_LEN);
	}

	discover_server(ctx);

	if (!ctx->slimproto_port) {
		ctx->slimproto_port = PORT;
	}

	if (name) {
		strncpy(ctx->player_name, name, PLAYER_NAME_LEN);
		ctx->player_name[PLAYER_NAME_LEN] = '\0';
	}

	/* could be avoided as whole context is reset at init ...*/
	strcpy(ctx->var_cap, "");
	ctx->new_server_cap = NULL;

	LOCK_O;
	sprintf(ctx->fixed_cap, ",MaxSampleRate=%u", ctx->config.sample_rate);

	/* codecs are amongst the few system-wide items */
	if (ctx->config.mode != SQ_FULL) {
		strcat(ctx->fixed_cap, ",");
		strcat(ctx->fixed_cap, ctx->config.codecs);
    }
	else
	{
#if 0
		for (i = 0; i < MAX_CODECS; i++) {
			if (codecs[i] && codecs[i]->id && strlen(ctx->fixed_cap) < 128 - 10) {
				strcat(ctx->fixed_cap, ",");
				strcat(ctx->fixed_cap, codecs[i]->types);
			}
		}
#endif
	}
	UNLOCK_O;

	ctx->serv_addr.sin_family = AF_INET;
	ctx->serv_addr.sin_addr.s_addr = ctx->slimproto_ip;
	ctx->serv_addr.sin_port = htons(ctx->slimproto_port);

	memcpy(ctx->mac, mac, 6);

	LOG_INFO("[%p] connecting to %s:%d", ctx, inet_ntoa(ctx->serv_addr.sin_addr), ntohs(ctx->serv_addr.sin_port));

	ctx->new_server = 0;
	ctx->play_running = ctx-> track_ended = false;
	ctx->track_status = TRACK_STOPPED;

#if LINUX || OSX || FREEBSD
	pthread_attr_t attr;
	pthread_attr_init(&attr);
	pthread_attr_setstacksize(&attr, PTHREAD_STACK_MIN + SLIMPROTO_THREAD_STACK_SIZE);
	pthread_create(&ctx->thread, &attr, (void *(*)(void*)) slimproto, ctx);
	pthread_attr_destroy(&attr);
#endif
#if WIN
	ctx->thread = CreateThread(NULL, SLIMPROTO_THREAD_STACK_SIZE, (LPTHREAD_START_ROUTINE)&slimproto, ctx, 0, NULL);
#endif
}



