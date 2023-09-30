/*
 *  Squeeze2upnp - LMS to uPNP bridge
 *
 *	(c) Philippe, philippe_44@outlook.com
 *
 *  See LICENSE
 *
 */

#include <math.h>
#include <fcntl.h>
#include <sys/types.h>
#include <sys/stat.h>

#include "platform.h"

#if WIN
#include <process.h>
#endif

#include "squeezedefs.h"

#if USE_SSL
#include <openssl/ssl.h>
#include "cross_ssl.h"
#endif

#include "squeeze2upnp.h"
#include "upnpdebug.h"
#include "upnptools.h"
#include "ixmlextra.h"
#include "avt_util.h"
#include "mr_util.h"
#include "mimetypes.h"
#include "config_upnp.h"
#include "cross_util.h"
#include "cross_log.h"
#include "cross_net.h"
#include "cross_ssl.h"
#include "cross_thread.h"

#define	AV_TRANSPORT 			"urn:schemas-upnp-org:service:AVTransport"
#define	RENDERING_CTRL 			"urn:schemas-upnp-org:service:RenderingControl"
#define	CONNECTION_MGR 			"urn:schemas-upnp-org:service:ConnectionManager"
#define TOPOLOGY				"urn:schemas-upnp-org:service:ZoneGroupTopology"
#define GROUP_RENDERING_CTRL	"urn:schemas-upnp-org:service:GroupRenderingControl"

#define DISCOVERY_TIME 		30
#define PRESENCE_TIMEOUT	(DISCOVERY_TIME * 6)

#define TRACK_POLL  	(1000)
#define STATE_POLL  	(500)
#define INFOEX_POLL 	(60*1000)
#define MIN_POLL 		(min(TRACK_POLL, STATE_POLL))
#define MAX_ACTION_ERRORS (5)

#define SHORT_TRACK		(10*1000)

#define MODEL_NAME_STRING	"UPnPBridge"

enum { NEXT_FORCE = -1, NEXT_GAPPED = 0, NEXT_GAPLESS = 1 };

/*----------------------------------------------------------------------------*/
/* globals initialized */
/*----------------------------------------------------------------------------*/
int32_t				glLogLimit = -1;
char				glBinding[128] = "?";
struct sMR			glMRDevices[MAX_RENDERERS];
pthread_mutex_t 	glMRMutex;
UpnpClient_Handle 	glControlPointHandle;
char				glCustomDiscovery[STR_LEN * 8];

log_level	slimproto_loglevel = lINFO;
log_level	slimmain_loglevel = lWARN;
log_level	stream_loglevel = lWARN;
log_level	decode_loglevel = lWARN;
log_level	output_loglevel = lINFO;
log_level	main_loglevel = lINFO;
log_level	util_loglevel = lWARN;
log_level	upnp_loglevel = lINFO;

tMRConfig			glMRConfig = {
							false,      	// SeekAfterPause
							true,			// LivePause
							false,			// ByteSeek
							true,			// Enabled
							PRESENCE_TIMEOUT, // Removal timeout
							1,         		// VolumeOnPlay
							true,			// VolumeFeedback
							NEXT_GAPLESS,	// AcceptNextURI
							true,			// SendMetaData
							true,			// SendCoverArt
							ICY_FULL,		// SendIcy
							100,			// MaxVolume
							false,			// AutoPlay
							"",      		// ForcedMimeTypes
					};

static uint8_t LMSVolumeMap[129] = {
			0, 3, 6, 7, 8, 10, 12, 13, 14, 16, 17, 18, 19, 20,
			21, 22, 24, 25, 26, 27, 28, 28, 29, 30, 31, 32, 33,
			34, 35, 36, 37, 37, 38, 39, 40, 41, 41, 42, 43, 44,
			45, 45, 46, 47, 48, 48, 49, 50, 51, 51, 52, 53, 53,
			54, 55, 55, 56, 57, 57, 58, 59, 60, 60, 61, 61, 62,
			63, 63, 64, 65, 65, 66, 67, 67, 68, 69, 69, 70, 70,
			71, 72, 72, 73, 73, 74, 75, 75, 76, 76, 77, 78, 78,
			79, 79, 80, 80, 81, 82, 82, 83, 83, 84, 84, 85, 86,
			86, 87, 87, 88, 88, 89, 89, 90, 91, 91, 92, 92, 93,
			93, 94, 94, 95, 95, 96, 96, 97, 98, 99, 100
		};

sq_dev_param_t glDeviceParam = {
					HTTP_CHUNKED, 	 		// stream_length
					STREAMBUF_SIZE,			// stream_buffer_size
					OUTPUTBUF_SIZE,			// output_buffer_size
					"aac,ogg,ops,ogf,flc,alc,wav,aif,pcm,mp3",		// codecs
					"thru",					// mode
					30,						// next_delay
					"raw,wav,aif",			// raw_audio_format
					"?",                    // server
					96000,			        // sample_rate
					L24_PACKED_LPCM,        // L24_mode
					FLAC_DEFAULT_HEADER,	// flac_header
					"",						// name
					{ 0x00,0x00,0x00,0x00,0x00,0x00 },
#ifdef RESAMPLE
					"",						// resample_options
#endif
					false,      			// roon_mode
					"",						// store_prefix
					"",						// coveart resolution
					// parameters not from read from config file
#if !WIN
					{
#endif
						true,				// use_cli
						"",     			// server
						ICY_FULL,			// send_icy;
#if !WIN
					},
#endif
				} ;

/*----------------------------------------------------------------------------*/
/* local typedefs															  */
/*----------------------------------------------------------------------------*/
typedef struct sUpdate {
	enum { DISCOVERY, BYE_BYE, SEARCH_TIMEOUT } Type;
	char *Data;
} tUpdate;

/*----------------------------------------------------------------------------*/
/* consts or pseudo-const*/
/*----------------------------------------------------------------------------*/
static char* glDiscoveryPatterns[8] = { "urn:schemas-upnp-org:device:MediaRenderer:1",
										"urn:schemas-upnp-org:device:MediaRenderer:2" };

static const struct cSearchedSRV_s
{
 char 	name[RESOURCE_LENGTH];
 int	idx;
 uint32_t  TimeOut;
} cSearchedSRV[NB_SRV] = {	{AV_TRANSPORT, AVT_SRV_IDX, 0},
						{RENDERING_CTRL, REND_SRV_IDX, 120},
						{CONNECTION_MGR, CNX_MGR_IDX, 0},
						{TOPOLOGY, TOPOLOGY_IDX, 0},
						{GROUP_RENDERING_CTRL, GRP_REND_SRV_IDX, 0},
				   };

/*----------------------------------------------------------------------------*/
/* locals */
/*----------------------------------------------------------------------------*/
static log_level 	  	*loglevel = &main_loglevel;
#if LINUX || FREEBSD || SUNOS
bool					glDaemonize = false;
#endif
pthread_t				glUpdateMRThread;
static bool				glMainRunning = true;
static bool				glInteractive = true;
static pthread_mutex_t 	glUpdateMutex;

static pthread_cond_t  	glUpdateCond;
static pthread_t 		glMainThread, glUpdateThread;
static cross_queue_t	glUpdateQueue;
static char				*glLogFile;

static char				*glPidFile = NULL;
static bool				glAutoSaveConfigFile = false;
static bool				glGracefullShutdown = true;
static bool				glDiscovery;
static void				*glConfigID = NULL;
static char				glConfigName[STR_LEN] = "./config.xml";
static char				*glExcluded = "Squeezebox";
static char				glModelName[STR_LEN] = MODEL_NAME_STRING;

static char usage[] =
			VERSION "\n"
		   "See -t for license terms\n"
		   "Usage: [options]\n"
		   "  -s <server>[:<port>]\tConnect to specified server, otherwise uses autodiscovery to find server\n"
		   "  -b <address|iface>]\tNetwork address (or interface name) to bind to\n"
		   "  -x <config file>\tread config from file (default is ./config.xml)\n"
		   "  -i <config file>\tdiscover players, save <config file> and exit\n"
		   "  -I \t\t\tauto save config at every network scan\n"
		   "  -f <logfile>\t\tWrite debug to logfile\n"
		   "  -p <pid file>\t\twrite PID in file\n"
		   "  -o [thru|pcm|flc[:<q>]|mp3[:<r>]][,r:[-]<rate>][,s:<8:16:24>][,flow]\tTranscode mode\n"
		   "  -d <log>=<level>\tSet logging level, logs: all|slimproto|slimmain|stream|decode|output|web|main|util|upnp, level: error|warn|info|debug|sdebug\n"
		   "  -M <modelname>\tSet the squeezelite player model name sent to the server (default: " MODEL_NAME_STRING ")\n"
#if LINUX || FREEBSD || SUNOS
		   "  -z \t\t\tDaemonize\n"
#endif
		   "  -Z \t\t\tNOT interactive\n"
		   "  -k \t\t\tImmediate exit on SIGQUIT and SIGTERM\n"
		   "  -t \t\t\tLicense terms\n"
		   "\n"
		   "Build options:"
#if LINUX
		   " LINUX"
#endif
#if WIN
		   " WIN"
#endif
#if OSX
		   " OSX"
#endif
#if FREEBSD
		   " FREEBSD"
#endif
#if SUNOS
	" SUNOS"
#endif
#if EVENTFD
		   " EVENTFD"
#endif
#if SELFPIPE
		   " SELFPIPE"
#endif
#if LOOPBACK
		   " LOOPBACK"
#endif
#if WINEVENT
		   " WINEVENT"
#endif
#if FFMPEG
		   " FFMPEG"
#endif
#if RESAMPLE
		   " RESAMPLE"
#endif
#if CODECS
		   " CODECS"
#endif
#if USE_SSL
		   " SSL"
#endif
#if LINKALL
		   " LINKALL"
#endif
		   "\n\n";
static char license[] =
		   "This program is free software: you can redistribute it and/or modify\n"
		   "it under the terms of the GNU General Public License as published by\n"
		   "the Free Software Foundation, either version 3 of the License, or\n"
		   "(at your option) any later version.\n\n"
		   "This program is distributed in the hope that it will be useful,\n"
		   "but WITHOUT ANY WARRANTY; without even the implied warranty of\n"
		   "MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the\n"
		   "GNU General Public License for more details.\n\n"
		   "You should have received a copy of the GNU General Public License\n"
		   "along with this program.  If not, see <http://www.gnu.org/licenses/>.\n\n";
#define SET_LOGLEVEL(log) 			  \
	if (!strcmp(resp, #log"dbg")) { \
		char level[20];           \
		i = scanf("%s", level);   \
		log ## _loglevel = debug2level(level); \
	}

/*----------------------------------------------------------------------------*/
/* prototypes */
/*----------------------------------------------------------------------------*/
static void 	*MRThread(void *args);
static 	void*	UpdateThread(void *args);
static bool 	AddMRDevice(struct sMR *Device, char * UDN, IXML_Document *DescDoc,	const char *location);
static bool		isExcluded(char *Model);
static void 	NextTrack(struct sMR *Device);
static void		DeltaOptions(char* ref, char* src);

// functions with _ prefix means that the device mutex is expected to be locked
static bool 	_ProcessQueue(struct sMR *Device);
static void 	_SyncNotifState(char *State, struct sMR* Device);
static void 	_ProcessVolume(char *Volume, struct sMR* Device);


/*----------------------------------------------------------------------------*/
bool sq_callback(void *caller, sq_action_t action, ...)
{
	struct sMR *Device = caller;
	bool rc = true;

	// this is async, so need to check context validity
	if (!CheckAndLock(Device)) return false;

	va_list args;
	va_start(args, action);

	if (action == SQ_ONOFF) {
		Device->on = va_arg(args, int);

		// this is probably now safe against inter-domains deadlock
		if (Device->on && Device->Config.AutoPlay)
			sq_notify(Device->SqueezeHandle, SQ_PLAY, (int) Device->on);

		LOG_DEBUG("[%p]: device set on/off %d", caller, Device->on);
	}

	if (!Device->on && action != SQ_SETNAME && action != SQ_SETSERVER && Device->sqState != SQ_PLAY) {
		LOG_DEBUG("[%p]: device off or not controlled by LMS", caller);
		pthread_mutex_unlock(&Device->Mutex);
		va_end(args);
		return false;
	}

	LOG_SDEBUG("[%p]: callback for %s", Device, Device->friendlyName);

	switch (action) {

		case SQ_SET_TRACK: {
			struct track_param *p = va_arg(args, struct track_param*);
			char *ProtoInfo, *uri;
			char format = mimetype_to_format(p->mimetype);

			// when this is received the next track has been processed
			NFREE(Device->NextURI);
			NFREE(Device->ExpectedURI);
			NFREE(Device->NextProtoInfo);
			Device->ElapsedLast = Device->ElapsedOffset = 0;
			metadata_free(&Device->NextMetaData);
			if (!Device->Config.SendCoverArt) NFREE(p->metadata.artwork);

			LOG_INFO("[%p]:\n\tartist:%s\n\talbum:%s\n\ttitle:%s\n\tgenre:%s\n\t"
					 "duration:%d.%03d\n\tsize:%d\n\tcover:%s\n\toffset:%u", Device,
					p->metadata.artist, p->metadata.album, p->metadata.title,
					p->metadata.genre, div(p->metadata.duration, 1000).quot,
					div(p->metadata.duration,1000).rem, p->metadata.size,
					p->metadata.artwork ? p->metadata.artwork : "", p->offset);

			// passing DLNAfeatures back is really ugly
			ProtoInfo = MakeProtoInfo(p->mimetype, p->metadata.duration);

			// Sonos requires special prefix for ICY but it can't be a repeating stream (must have no duration)
			if ((!p->metadata.duration || p->metadata.repeating != -1) && (*Device->Service[TOPOLOGY_IDX].ControlURL) &&
				(format == 'm' || format == 'a')) {
				(void) !asprintf(&uri, "x-rincon-mp3radio://%s", p->uri);
				if (format == 'a') (void)! asprintf(&Device->ExpectedURI, "aac://%s", p->uri);
				LOG_INFO("[%p]: Sonos live stream", Device);
			} else uri = strdup(p->uri);

			if (!Device->ExpectedURI) Device->ExpectedURI = strdup(uri);

			 if (p->offset) {
				if (Device->State == STOPPED) {
					// could not get next URI before track stopped, restart
					LOG_WARN("[%p]: set current URI (*) (s:%u) %s", Device, Device->ShortTrack, uri);
					Device->ShortTrackWait = 0;
					Device->Duration = p->metadata.duration;
					if (p->metadata.duration && p->metadata.duration < SHORT_TRACK) Device->ShortTrack = true;
					AVTSetURI(Device, uri, &p->metadata, ProtoInfo);
					AVTPlay(Device);
				 } else if (Device->Config.AcceptNextURI != NEXT_GAPLESS || Device->ShortTrack || (p->metadata.duration && p->metadata.duration < SHORT_TRACK)) {
					// can't use UPnP NextURI capability
					LOG_INFO("[%p]: next URI gapped (s:%u) %s", Device, Device->ShortTrack, uri);
					Device->NextURI = uri;
					Device->NextProtoInfo = ProtoInfo;
					// this is a structure copy, pointers within remains valid
					Device->NextMetaData = p->metadata;
				} else {
					// nominal case, use UPnP NextURI feature
					LOG_INFO("[%p]: next URI gapless %s", Device, uri);
					AVTSetNextURI(Device, uri, &p->metadata, ProtoInfo);
				}
			} else {
				if (p->metadata.duration && p->metadata.duration < SHORT_TRACK) Device->ShortTrack = true;
				Device->Duration = p->metadata.duration;
				LOG_INFO("[%p]: set current URI (s:%u) %s", Device, Device->ShortTrack, uri);
				AVTSetURI(Device, uri, &p->metadata, ProtoInfo);
			}

			// Gapless or direct URI used, free ressources
			if (!Device->NextURI) {
				free(ProtoInfo);
				free(uri);
				metadata_free(&p->metadata);
			}

			break;
		}
		case SQ_UNPAUSE:
			// should not be in stopped mode at this point unless it's a short track
			if (Device->sqState == SQ_PLAY) break;

			if (Device->Config.SeekAfterPause) {
				sq_set_time(Device->SqueezeHandle, "-0.01");
				break;
			}

			AVTPlay(Device);
			Device->sqState = SQ_PLAY;
			break;
		case SQ_PLAY: {
			// should not be in stopped mode at this point unless it's a short track
			if (Device->sqState == SQ_PLAY) break;

			AVTSetPlayMode(Device);
			AVTPlay(Device);
			Device->sqState = SQ_PLAY;

			// we need to wakeup that player from long sleep
			crossthreads_wake();

			// send volume to master + slaves
			if (Device->Config.VolumeOnPlay == 1 && Device->Volume != -1) {
				int i;

				// don't want echo, even if sending onPlay
				Device->VolumeStampTx = gettime_ms();

				// update all devices (master & slaves) and set Master's volume if we don't have any
				for (i = 0; i < MAX_RENDERERS; i++) {
					struct sMR *p = glMRDevices + i;
					if (p->Running && (p->Master == Device || p == Device)) CtrlSetVolume(p, p->Volume != -1 ? p->Volume : Device->Volume, p->seqN++);
				}
			}

			break;
		}
		case SQ_STOP:
			AVTStop(Device);
			NFREE(Device->NextURI);
			NFREE(Device->NextProtoInfo);
			NFREE(Device->ExpectedURI);
			metadata_free(&Device->NextMetaData);
			Device->sqState = action;
			Device->ShortTrack = false;
			Device->ShortTrackWait = 0;
			break;
		case SQ_PAUSE:
			if (Device->Config.LivePause || Device->Duration) AVTBasic(Device, "Pause");
			else AVTBasic(Device, "Stop");
			Device->sqState = action;
			break;
		case SQ_VOLUME: {
			int Volume = va_arg(args, int), now = gettime_ms();

			// some controllers send a volume < 0 to mute
			if (Volume < 0) {
				Device->Muted = true;
				Volume = -Volume;
				CtrlSetMute(Device, true, Device->seqN++);
			} else if (Device->Muted) {
				Device->Muted = false;
				CtrlSetMute(Device, false, Device->seqN++);
		   	}

			Volume = LMSVolumeMap[Volume];

			// discard echo commands
			if (now < Device->VolumeStampRx + 1000) break;

			// calculate volume and check for change
			if ((int) Device->Volume == (Volume * Device->Config.MaxVolume) / 100) break;

			// Sonos group volume API is unreliable, need to create our own
			int GroupVolume = CalcGroupVolume(Device);

			/* Volume is kept as a double in device's context to avoid relative
			values going to 0 and being stuck there. This works because although
			volume is echoed from UPnP event as an integer, timing check allows
			that echo to be discarded, so until volume is changed locally, it
			remains a floating value */

			// we will send now
			if (Device->Config.VolumeOnPlay != -1 && (!Device->Config.VolumeOnPlay || Device->sqState == SQ_PLAY))
				Device->VolumeStampTx = now;

			// update context, and set volume only if authorized
			if (GroupVolume < 0) {
				Device->Volume = (Volume * Device->Config.MaxVolume) / 100;
				if (Device->VolumeStampTx == now) CtrlSetVolume(Device, Device->Volume, Device->seqN++);
			} else {
				double Ratio = GroupVolume ? (double) Volume / GroupVolume : 0;

				// for standalone master, GroupVolume equals Device->Volume
				for (int i = 0; i < MAX_RENDERERS; i++) {
					struct sMR *p = glMRDevices + i;
					if (!p->Running || (p != Device && p->Master != Device)) continue;

					// must set a volume for slave if we have not acquired it already
					if (p->Volume && p->Volume != -1 && GroupVolume) p->Volume = min(p->Volume * Ratio, p->Config.MaxVolume);
					else p->Volume = (Volume * p->Config.MaxVolume) / 100;

					if (Device->VolumeStampTx == now) CtrlSetVolume(p, p->Volume, p->seqN++);
				}
			}
			break;
		}
		case SQ_SETNAME:
			strcpy(Device->sq_config.name, va_arg(args, char*));
			if (glAutoSaveConfigFile) {
				pthread_mutex_lock(&glUpdateMutex);
				SaveConfig(glConfigName, glConfigID, false);
				pthread_mutex_unlock(&glUpdateMutex);
			}
			break;
		case SQ_SETSERVER:
			strcpy(Device->sq_config.set_server, inet_ntoa(*va_arg(args, struct in_addr*)));
			break;
		default:
			break;
	}

	va_end(args);
	pthread_mutex_unlock(&Device->Mutex);
	return rc;
}


/*----------------------------------------------------------------------------*/
static void *MRThread(void *args)
{
	int elapsed, wakeTimer = MIN_POLL;
	unsigned last;
	struct sMR *p = (struct sMR*) args;

	last = gettime_ms();

	for (; p->Running; crossthreads_sleep(wakeTimer)) {
		elapsed = gettime_ms() - last;
		pthread_mutex_lock(&p->Mutex);

		if (p->ShortTrack) wakeTimer = MIN_POLL / 2;
		else wakeTimer = (p->sqState != SQ_STOP && p->on) ? MIN_POLL : MIN_POLL * 10;

		LOG_SDEBUG("[%p]: UPnP thread timer %d %d", p, elapsed, wakeTimer);

		p->StatePoll += elapsed;
		p->TrackPoll += elapsed;

		if (p->InfoExPoll != -1) p->InfoExPoll += elapsed;

		// do nothing if we are a slave
		if (p->Master) goto sleep;

		// was just waiting for a short track to end
		if (p->ShortTrackWait > 0 && ((p->ShortTrackWait -= elapsed) < 0)) {
			LOG_WARN("[%p]: stopping on short track timeout", p);
			p->ShortTrack = false;
			sq_notify(p->SqueezeHandle, SQ_STOP, p->ShortTrack);
		}

		// hack to deal with players that do not report end of track
		if (p->Duration < 0 && ((p->Duration += elapsed) >= 0)) {
			if (p->NextProtoInfo) {
				LOG_INFO("[%p] overtime next track", p);
				NextTrack(p);
			} else {
				LOG_INFO("[%p] overtime last track", p);
				p->Duration= 0;
				AVTBasic(p, "Stop");
			}
		}

		/*
		should not request any status update if we are stopped, off or waiting
		for an action to be performed
		*/
		// exception is to poll extended informations if any for battery
		if (p->on && !p->WaitCookie && p->InfoExPoll >= INFOEX_POLL) {
			p->InfoExPoll = 0;
			AVTCallAction(p, "GetInfoEx", p->seqN++);
		}

		if (!p->on || (p->sqState == SQ_STOP && p->State == STOPPED) ||
			 p->ErrorCount < 0 || p->ErrorCount > MAX_ACTION_ERRORS || p->WaitCookie) goto sleep;

		// get track position & CurrentURI
		if (p->TrackPoll >= TRACK_POLL) {
			p->TrackPoll = 0;
			if (p->sqState != SQ_STOP && p->sqState != SQ_PAUSE) {
				AVTCallAction(p, "GetPositionInfo", p->seqN++);
			}
		}

		// do polling as event is broken in many uPNP devices
		if (p->StatePoll >= STATE_POLL) {
			p->StatePoll = 0;
			AVTCallAction(p, "GetTransportInfo", p->seqN++);
		}
sleep:
		pthread_mutex_unlock(&p->Mutex);
		last = gettime_ms();
	}


	AVTActionFlush(&p->ActionQueue);
	metadata_free(&p->NextMetaData);
	NFREE(p->NextProtoInfo);
	NFREE(p->NextURI);
	NFREE(p->ExpectedURI);
	NFREE(p->Sink);
	return NULL;
}


/*----------------------------------------------------------------------------*/
static void _SyncNotifState(char *State, struct sMR* Device)
{
	sq_event_t Event = SQ_NONE;
	bool Param = false;

	/*
	ASSUMING DEVICE'S MUTEX LOCKED
	*/

	// in transitioning mode, do nothing, just wait
	if (!strcmp(State, "TRANSITIONING") && Device->State != TRANSITIONING) {
		LOG_INFO("[%p]: uPNP transition", Device);
		Event = SQ_TRANSITION;
		Device->State = TRANSITIONING;
	}

	if (!strcmp(State, "STOPPED") && Device->State != STOPPED) {
		if (Device->sqState == SQ_PAUSE) {
			// this is a remote forced stop: a pause then a stop
			Param = true;
			Event = SQ_STOP;
		} else if (!Device->NextURI) {
			if (Device->ShortTrack) {
				// might not even have received next LMS's request, wait a bit
				Device->ShortTrackWait = 5000;
				LOG_WARN("[%p]: stop on short track (wait %hd ms for next URI)", Device, Device->ShortTrackWait);
			} else if (Device->sqState == SQ_PLAY && Device->ExpectedURI && Device->Config.AcceptNextURI == NEXT_GAPLESS) {
				// gapless player but something went wrong, requesting LMS to got to next track
				Event = SQ_NEXT_FAILED;
				LOG_INFO("[%p]: nextURI %s failed, requesting LMS to go next", Device, Device->ExpectedURI);
			} else {
				// could generate an overrun event or an underrun
				Event = SQ_STOP;
				LOG_INFO("[%p]: uPNP stop", Device);
			}
		} else if (Device->NextProtoInfo) {
			// non-gapless player or gapped track, manually set next track
			NextTrack(Device);
			LOG_INFO("[%p]: gapped transition %s", Device, Device->NextURI);
		}

		Device->State = STOPPED;
	}
	if (!strcmp(State, "PLAYING") && (Device->State != PLAYING)) {
		switch (Device->sqState) {
			case SQ_PAUSE:
				Param = true;
			case SQ_PLAY:
				Event = SQ_PLAY;
				break;
			default: {
				/*
				can be a local playing after stop or a N-1 playing after a quick
				sequence of "next" when a N stop has been sent ==> ignore it
				*/
				LOG_ERROR("[%s]: unhandled playing", Device->friendlyName);
				break;
			}
		}
		LOG_INFO("%s: uPNP playing", Device->friendlyName);
		Device->State = PLAYING;
	}

	if (!strcmp(State, "PAUSED_PLAYBACK") && Device->State != PAUSED) {
		// detect unsollicited pause, but do not confuse it with a fast pause/play
		if (Device->sqState != SQ_PAUSE) Param = true;
		LOG_INFO("%s: uPNP pause", Device->friendlyName);
		Event = SQ_PAUSE;
		Device->State = PAUSED;
	}

	// seems that now the inter-domain lock does not exist anymore
	if (Event != SQ_NONE)
		sq_notify(Device->SqueezeHandle, Event, (int) Param);
}

/*----------------------------------------------------------------------------*/
static bool _ProcessQueue(struct sMR *Device)
{
	struct sService *Service = &Device->Service[AVT_SRV_IDX];
	tAction *Action;
	int rc = 0;

	/*
	ASSUMING DEVICE'S MUTEX LOCKED
	*/

	Device->WaitCookie = 0;
	if ((Action = queue_extract(&Device->ActionQueue)) == NULL) return false;

	Device->WaitCookie = Device->seqN++;
	rc = UpnpSendActionAsync(glControlPointHandle, Service->ControlURL, Service->Type,
							 NULL, Action->ActionNode, ActionHandler, Device->WaitCookie);

	if (rc != UPNP_E_SUCCESS) {
		LOG_ERROR("Error in queued UpnpSendActionAsync -- %d", rc);
	}

	ixmlDocument_free(Action->ActionNode);
	free(Action);

	return (rc == 0);
}

/*----------------------------------------------------------------------------*/
static void _ProcessVolume(char *Volume, struct sMR* Device)
{
	int UPnPVolume = atoi(Volume), GroupVolume;
	uint32_t now = gettime_ms();
	struct sMR *Master = Device->Master ? Device->Master : Device;

	/*
	ASSUMING DEVICE'S MUTEX LOCKED
	*/

	if (UPnPVolume != (int) Device->Volume && now > Master->VolumeStampTx + 1000) {
		Device->Volume = UPnPVolume;
		Master->VolumeStampRx = now;
		GroupVolume = CalcGroupVolume(Master);

		LOG_INFO("[%p]: UPnP Volume local change %d:%d (%s)", Device, UPnPVolume, GroupVolume, Device->Master ? "slave": "master");

		int ScaledVolume = GroupVolume < 0 ? (UPnPVolume * 100) / Device->Config.MaxVolume : GroupVolume;
		sq_notify(Master->SqueezeHandle, SQ_VOLUME, ScaledVolume);
	}
}

/*----------------------------------------------------------------------------*/
static void NextTrack(struct sMR *Device) {
	if (Device->NextMetaData.duration && Device->NextMetaData.duration < SHORT_TRACK) Device->ShortTrack = true;
	else Device->ShortTrack = false;

	Device->Duration = Device->NextMetaData.duration;
	AVTSetURI(Device, Device->NextURI, &Device->NextMetaData, Device->NextProtoInfo);

	NFREE(Device->NextProtoInfo);
	NFREE(Device->NextURI);

	metadata_free(&Device->NextMetaData);
	AVTPlay(Device);
}

/*----------------------------------------------------------------------------*/
uint64_t ConvertTime(char* time)
{
	uint64_t elapsed = 0;
	uint32_t item;

	while (time) {
		(void)! sscanf(time, "%u", &item);
		elapsed = elapsed * 60 + item;
		time = strchr(time, ':');
		if (time) time++;
	}

	return elapsed;
}

/*----------------------------------------------------------------------------*/
static void ProcessEvent(Upnp_EventType EventType, const void* _Event, void* Cookie) {
	UpnpEvent* Event = (UpnpEvent*)_Event;
	struct sMR* Device = SID2Device(UpnpEvent_get_SID(Event));
	IXML_Document* VarDoc = UpnpEvent_get_ChangedVariables(Event);
	char* r = NULL;
	char* LastChange = NULL;

	// this is async, so need to check context's validity
	if (!CheckAndLock(Device)) return;

	LastChange = XMLGetFirstDocumentItem(VarDoc, "LastChange", true);

	if (((!Device->on || !Device->SqueezeHandle) && !Device->Master) || !LastChange) {
		LOG_SDEBUG("device off, no squeezebox device (yet) or not change for %s", UpnpString_get_String(UpnpEvent_get_SID(Event)));
		pthread_mutex_unlock(&Device->Mutex);
		NFREE(LastChange);
		return;
	}

	// Feedback volume to LMS if authorized
	if (Device->Config.VolumeFeedback) {
		r = XMLGetChangeItem(VarDoc, "Volume", "channel", "Master", "val");
		if (r) _ProcessVolume(r, Device);
		NFREE(r);
	}

	pthread_mutex_unlock(&Device->Mutex);
	NFREE(LastChange);
}

/*----------------------------------------------------------------------------*/
int ActionHandler(Upnp_EventType EventType, const void* Event, void* Cookie) {
	struct sMR* p = NULL;
	static int recurse = 0;

	LOG_SDEBUG("action: %i [%s] [%p] [%u]", EventType, uPNPEvent2String(EventType), Cookie, recurse);
	recurse++;

	switch (EventType) {
		case UPNP_CONTROL_ACTION_COMPLETE: {
			char* r;
			const char* Resp = NULL;

			p = CURL2Device(UpnpActionComplete_get_CtrlUrl(Event));
			if (!CheckAndLock(p)) return 0;

			LOG_SDEBUG("[%p]: ac %i %s (cookie %p)", p, EventType, UpnpString_get_String(UpnpActionComplete_get_CtrlUrl(Event)));

			// If waited action has been completed, proceed to next one if any
			if (p->WaitCookie) {
				Resp = XMLGetLocalName(UpnpActionComplete_get_ActionResult(Event), 1);

				LOG_DEBUG("[%p]: Waited action %s", p, Resp ? Resp : "<none>");

				// discard everything else except waiting action
				if (Cookie != p->WaitCookie) break;

				p->StartCookie = p->WaitCookie;
				_ProcessQueue(p);

				/*
				when certain waited action has been completed, the state need
				to be re-acquired because a 'stop' state might be missed when
				(eg) repositionning where two consecutive status update will
				give 'playing', the 'stop' in the middle being unseen
				*/
				if (Resp && ((!strcasecmp(Resp, "StopResponse") && p->State == STOPPED) ||
					(!strcasecmp(Resp, "PlayResponse") && p->State == PLAYING) ||
					(!strcasecmp(Resp, "PauseResponse") && p->State == PAUSED))) {
					p->State = UNKNOWN;
				}

				break;
			}

			// don't proceed anything that is too old
			if (Cookie < p->StartCookie) break;

			IXML_Document* Result = UpnpActionComplete_get_ActionResult(Event);

			// extended informations, don't do anything else
			if (Resp && !strcasecmp(Resp, "GetInfoExResponse")) {
				// Battery information for devices that have one
				if (*loglevel == lDEBUG) {
					char *s = ixmlDocumenttoString(Result);
					LOG_DEBUG("[%p]: extended info %s", p, s);
					NFREE(s);
				}
				r = XMLGetFirstDocumentItem(Result, "BatteryFlag", true);
				if (r) {
					uint32_t Level = atoi(r) << 8;
					NFREE(r);
					r = XMLGetFirstDocumentItem(Result, "BatteryPercent", true);
					if (r) {
						Level |= (uint8_t) atoi(r);
						sq_notify(p->SqueezeHandle, SQ_BATTERY, Level);
					}
				}
				NFREE(r);
				break;
			}

			// transport state response
			r = XMLGetFirstDocumentItem(Result, "CurrentTransportState", true);
			if (r) _SyncNotifState(r, p);
			NFREE(r);

			if (p->State == PLAYING) {
				// When not playing, position is not reliable
				r = XMLGetFirstDocumentItem(Result, "RelTime", true);
				if (r) {
					uint32_t Elapsed = ConvertTime(r) * 1000;
					if (p->Config.AcceptNextURI == NEXT_FORCE && p->Duration > 0 && p->Duration - Elapsed <= 2000) p->Duration = Elapsed - p->Duration;
					if (!p->Duration) {
						if (p->ElapsedLast > Elapsed) p->ElapsedOffset += p->ElapsedLast;
						p->ElapsedLast = Elapsed;
						Elapsed += p->ElapsedOffset;
					}
					sq_notify(p->SqueezeHandle, SQ_TIME, Elapsed);
					LOG_DEBUG("[%p]: position %d (cookie %p)", p, Elapsed, Cookie);
				}

				NFREE(r);

				// URI detection response
				r = XMLGetFirstDocumentItem(Result, "TrackURI", true);
				if (r) {
					if (*r == '\0' || !strstr(r, BRIDGE_URL)) {
						NFREE(r);
						char* s = XMLGetFirstDocumentItem(Result, "TrackMetaData", true);
						IXML_Document* doc = ixmlParseBuffer(s);
						NFREE(s);

						IXML_Node* node = (IXML_Node*)ixmlDocument_getElementById(doc, "res");
						if (node) node = (IXML_Node*)ixmlNode_getFirstChild(node);
						if (node) r = strdup(ixmlNode_getNodeValue(node));

						LOG_DEBUG("[%p]: no Current URI, use MetaData %s", p, r);
						if (doc) ixmlDocument_free(doc);
					}

					if (p->ExpectedURI && !strcasecmp(r, p->ExpectedURI)) NFREE(p->ExpectedURI);
					if (r) sq_notify(p->SqueezeHandle, SQ_TRACK_INFO, r);
				}

				NFREE(r);
			}

			LOG_SDEBUG("Action complete : %i (cookie %p)", EventType, Cookie);

			if (UpnpActionComplete_get_ErrCode(Event) != UPNP_E_SUCCESS) {
				if (UpnpActionComplete_get_ErrCode(Event) == UPNP_E_SOCKET_CONNECT) p->ErrorCount = -1;
				else if (p->ErrorCount >= 0) p->ErrorCount++;
				LOG_ERROR("[%p]: Error %d in action callback (count:%d cookie:%p)", p, UpnpActionComplete_get_ErrCode(Event), p->ErrorCount, Cookie);
			}
			else {
				p->ErrorCount = 0;
			}

			break;
		}
		default:
			break;
	}

	if (p) {
		pthread_mutex_unlock(&p->Mutex);
	}

	recurse--;

	return 0;
}

/*----------------------------------------------------------------------------*/
int MasterHandler(Upnp_EventType EventType, const void *_Event, void *Cookie)
{
	// this variable is not thread_safe and not supposed to be
	static int recurse = 0;
	// libupnp makes this highly re-entrant so callees must protect themselves
	LOG_SDEBUG("event: %i [%s] [%p] (recurse %u)", EventType, uPNPEvent2String(EventType), Cookie, recurse);
	if (!glMainRunning) return 0;
	recurse++;
	switch ( EventType ) {
	case UPNP_DISCOVERY_ADVERTISEMENT_ALIVE:
		// probably not needed now as the search happens often enough and alive comes from many other devices
		break;
	case UPNP_DISCOVERY_SEARCH_RESULT: {
		tUpdate* Update = malloc(sizeof(tUpdate));

		Update->Type = DISCOVERY;
		Update->Data = strdup(UpnpString_get_String(UpnpDiscovery_get_Location(_Event)));
		LOG_DEBUG("received UPnP discover response %s", Update->Data);
		queue_insert(&glUpdateQueue, Update);
		pthread_cond_signal(&glUpdateCond);

		break;
	}
	case UPNP_DISCOVERY_ADVERTISEMENT_BYEBYE: {
		tUpdate* Update = malloc(sizeof(tUpdate));

		Update->Type = BYE_BYE;
		Update->Data = strdup(UpnpString_get_String(UpnpDiscovery_get_DeviceID(_Event)));
		queue_insert(&glUpdateQueue, Update);
		pthread_cond_signal(&glUpdateCond);

		break;
	}
	case UPNP_DISCOVERY_SEARCH_TIMEOUT: {
		tUpdate* Update = malloc(sizeof(tUpdate));

		Update->Type = SEARCH_TIMEOUT;
		Update->Data = NULL;
		queue_insert(&glUpdateQueue, Update);
		pthread_cond_signal(&glUpdateCond);

		// if there is a cookie, it's a targeted Sonos search
		if (!Cookie) {
			static int Index;
			if (!glDiscoveryPatterns[Index]) Index = 0;
			LOG_INFO("UPnP search for %s", glDiscoveryPatterns[Index]);
			UpnpSearchAsync(glControlPointHandle, DISCOVERY_TIME, glDiscoveryPatterns[Index++], NULL);
		}

		break;
	}
	case UPNP_EVENT_RECEIVED:
		ProcessEvent(EventType, _Event, Cookie);
		break;
	case UPNP_EVENT_SUBSCRIPTION_EXPIRED:
	case UPNP_EVENT_AUTORENEWAL_FAILED: {
		struct sService* s;
		struct sMR* Device = SID2Device(UpnpEventSubscribe_get_SID(_Event));

		free(Cookie);

		if (!CheckAndLock(Device)) break;

		s = EventURL2Service(UpnpEventSubscribe_get_PublisherUrl(_Event), Device->Service);
		if (s != NULL) {
			UpnpSubscribeAsync(glControlPointHandle, s->EventURL, s->TimeOut,
				MasterHandler, (void*) strdup(Device->UDN));
			LOG_INFO("[%p]: Auto-renewal failed, re-subscribing", Device);
		}

		pthread_mutex_unlock(&Device->Mutex);
		break;
	}
	case UPNP_EVENT_RENEWAL_COMPLETE:
	case UPNP_EVENT_SUBSCRIBE_COMPLETE: {
		struct sMR* Device = UDN2Device((char*)Cookie);
		struct sService* s;

		free(Cookie);

		if (!CheckAndLock(Device)) break;

		s = EventURL2Service(UpnpEventSubscribe_get_PublisherUrl(_Event), Device->Service);
		if (s != NULL) {
			if (UpnpEventSubscribe_get_ErrCode(_Event) == UPNP_E_SUCCESS) {
				s->Failed = 0;
				strcpy(s->SID, UpnpString_get_String(UpnpEventSubscribe_get_SID(_Event)));
				s->TimeOut = UpnpEventSubscribe_get_TimeOut(_Event);
				LOG_INFO("[%p]: subscribe success", Device);
			} else if (s->Failed++ < 3) {
				LOG_INFO("[%p]: subscribe fail, re-trying %u", Device, s->Failed);
				UpnpSubscribeAsync(glControlPointHandle, s->EventURL, s->TimeOut, MasterHandler, (void*)strdup(Device->UDN));
			} else {
				LOG_WARN("[%p]: subscribe fail, volume feedback will not work", Device);
			}
		}

		pthread_mutex_unlock(&Device->Mutex);
		break;
	}
	case UPNP_EVENT_SUBSCRIPTION_REQUEST:
	case UPNP_CONTROL_ACTION_REQUEST:
	case UPNP_EVENT_UNSUBSCRIBE_COMPLETE:
	case UPNP_CONTROL_GET_VAR_REQUEST:
	case UPNP_CONTROL_ACTION_COMPLETE:
	case UPNP_CONTROL_GET_VAR_COMPLETE:
		break;
	}

	recurse--;
	return 0;
}

/*----------------------------------------------------------------------------*/
static void FreeUpdate(void *_Item)
{
	tUpdate *Item = (tUpdate*) _Item;
	NFREE(Item->Data);
	free(Item);
}

/*----------------------------------------------------------------------------*/
static void *UpdateThread(void *args)
{
	while (glMainRunning) {
		tUpdate *Update;
		bool updated = false;
		pthread_mutex_lock(&glUpdateMutex);
		pthread_cond_wait(&glUpdateCond, &glUpdateMutex);
		for (; glMainRunning && (Update = queue_extract(&glUpdateQueue)) != NULL; FreeUpdate(Update)) {
			struct sMR *Device;
			uint32_t now = gettime_ms() / 1000;

			// UPnP end of search timer
			if (Update->Type == SEARCH_TIMEOUT) {
				LOG_DEBUG("Presence checking", NULL);

				for (int i = 0; i < MAX_RENDERERS; i++) {
					Device = glMRDevices + i;
					if (Device->Running && (((Device->sqState != SQ_PLAY || Device->State != PLAYING) &&
						((Device->Config.RemoveTimeout != -1 && now - Device->LastSeen > Device->Config.RemoveTimeout) || 
						 Device->ErrorCount > MAX_ACTION_ERRORS)) || Device->ErrorCount < 0)) {
						pthread_mutex_lock(&Device->Mutex);
						LOG_INFO("[%p]: removing unresponsive player (%s)", Device, Device->friendlyName);
						sq_delete_device(Device->SqueezeHandle);
						// device's mutex returns unlocked
						DelMRDevice(Device);
					}
				}

			// device removal request
			} else if (Update->Type == BYE_BYE) {
				Device = UDN2Device(Update->Data);

				// Multiple bye-bye might be sent
				if (!CheckAndLock(Device)) continue;

				LOG_INFO("[%p]: renderer bye-bye: %s", Device, Device->friendlyName);
				sq_delete_device(Device->SqueezeHandle);
				// device's mutex returns unlocked
				DelMRDevice(Device);
			// device keepalive or search response
			} else if (Update->Type == DISCOVERY) {
				IXML_Document *DescDoc = NULL;
				char *UDN = NULL, *ModelName = NULL;

				// it's a Sonos group announce, just do a targeted search and exit
				if (strstr(Update->Data, "group_description")) {
					for (int i = 0; i < MAX_RENDERERS; i++) {
						Device = glMRDevices + i;
						if (Device->Running && *Device->Service[TOPOLOGY_IDX].ControlURL)
							UpnpSearchAsync(glControlPointHandle, 5, Device->UDN, Device);
					}
					continue;
				}

				// existing device ?
				for (int i = 0; i < MAX_RENDERERS; i++) {
					Device = glMRDevices + i;
					if (Device->Running && !strcmp(Device->DescDocURL, Update->Data)) {
						char *friendlyName = NULL;
						struct sMR *Master = GetMaster(Device, &friendlyName);
						Device->LastSeen = now;
						LOG_DEBUG("[%p] UPnP keep alive: %s", Device, Device->friendlyName);
						// check for name change
						UpnpDownloadXmlDoc(Update->Data, &DescDoc);
						if (!friendlyName) friendlyName = XMLGetFirstDocumentItem(DescDoc, "friendlyName", true);
						if (friendlyName && strcmp(friendlyName, Device->friendlyName)) {
							// only update if LMS has not set its own name
							if (!strcmp(Device->sq_config.name, Device->friendlyName)) {
								// by notifying LMS, we'll get an update later
								sq_notify(Device->SqueezeHandle, SQ_SETNAME, friendlyName);
							}

							updated = true;
							LOG_INFO("[%p]: Name update %s => %s (LMS:%s)", Device, Device->friendlyName, friendlyName, Device->sq_config.name);
							strcpy(Device->friendlyName, friendlyName);
						}

						NFREE(friendlyName);

						// we are a master (or not a Sonos)
						if (!Master && Device->Master) {
							// leaving a group
							char **MimeTypes = ParseProtocolInfo(Device->Sink, Device->Config.ForcedMimeTypes);
							LOG_INFO("[%p]: Sonos %s is now master", Device, Device->friendlyName);
							pthread_mutex_lock(&Device->Mutex);
							Device->Master = NULL;
							Device->SqueezeHandle = sq_reserve_device(Device, Device->on, MimeTypes, &sq_callback);
							if (!*(Device->sq_config.name)) strcpy(Device->sq_config.name, Device->friendlyName);
							sq_run_device(Device->SqueezeHandle, &Device->sq_config);

							for (i = 0; MimeTypes[i]; i++) free(MimeTypes[i]);
							free(MimeTypes);
							pthread_mutex_unlock(&Device->Mutex);
						} else if (Master && (!Device->Master || Device->Master == Device)) {
							// joining a group as slave
							LOG_INFO("[%p]: Sonos %s is now slave", Device, Device->friendlyName);
							pthread_mutex_lock(&Device->Mutex);
							Device->Master = Master;
							sq_delete_device(Device->SqueezeHandle);
							Device->SqueezeHandle = 0;
							pthread_mutex_unlock(&Device->Mutex);
						}
						goto cleanup;
					}
				}

				// this can take a very long time, too bad for the queue...
				int rc;
				if ((rc = UpnpDownloadXmlDoc(Update->Data, &DescDoc)) != UPNP_E_SUCCESS) {
					LOG_DEBUG("Error obtaining description %s -- error = %d\n", Update->Data, rc);
					goto cleanup;
				}

				// not a media renderer but maybe a Sonos group update
				bool ValidRenderer = false;
				for (size_t i = 0; !ValidRenderer && glDiscoveryPatterns[i]; i++) {
					ValidRenderer = XMLMatchDocumentItem(DescDoc, "deviceType", glDiscoveryPatterns[i], false);
				}
				if (!ValidRenderer) goto cleanup;

				ModelName = XMLGetFirstDocumentItem(DescDoc, "modelName", true);
				UDN = XMLGetFirstDocumentItem(DescDoc, "UDN", true);
				// excluded device
				if (ModelName && isExcluded(ModelName)) {
					goto cleanup;
				}

				// new device so search a free spot - as this function is not called
				// recursively, no need to lock the device's mutex
				for (Device = glMRDevices; Device->Running && Device < glMRDevices + MAX_RENDERERS; Device++);

				// no more room !
				if (Device == glMRDevices + MAX_RENDERERS) {
					LOG_ERROR("Too many uPNP devices (max:%u)", MAX_RENDERERS);
					goto cleanup;
				}

				updated = true;

				if (AddMRDevice(Device, UDN, DescDoc, Update->Data) && !glDiscovery) {
					char **MimeTypes = ParseProtocolInfo(Device->Sink, Device->Config.ForcedMimeTypes);
					// create a new slimdevice
					Device->SqueezeHandle = sq_reserve_device(Device, Device->on, MimeTypes, &sq_callback);
					if (!*(Device->sq_config.name)) strcpy(Device->sq_config.name, Device->friendlyName);
					if (!Device->SqueezeHandle || !sq_run_device(Device->SqueezeHandle, &Device->sq_config)) {
						sq_release_device(Device->SqueezeHandle);
						Device->SqueezeHandle = 0;
						LOG_ERROR("[%p]: cannot create squeezelite instance (%s)", Device, Device->friendlyName);
						DelMRDevice(Device);
					}
					for (int i = 0; MimeTypes[i]; i++) free(MimeTypes[i]);
					free(MimeTypes);
				}

cleanup:
				if (updated && (glAutoSaveConfigFile || glDiscovery)) {
					LOG_DEBUG("Updating configuration %s", glConfigName);
					SaveConfig(glConfigName, glConfigID, false);
				}

				NFREE(UDN);
				NFREE(ModelName);
				if (DescDoc) ixmlDocument_free(DescDoc);
			}
		}
		// now release the update mutex (will be locked/unlocked)
		pthread_mutex_unlock(&glUpdateMutex);
	}
	return NULL;
}

/*----------------------------------------------------------------------------*/
static void *MainThread(void *args) {
	while (glMainRunning) {

		crossthreads_sleep(30*1000);
		if (!glMainRunning) break;

		if (glLogFile && glLogLimit != -1) {
			uint32_t size = ftell(stderr);

			if (size > glLogLimit*1024*1024) {
				uint32_t Sum, BufSize = 16384;
				uint8_t *buf = malloc(BufSize);

				FILE *rlog = fopen(glLogFile, "rb");
				FILE *wlog = fopen(glLogFile, "r+b");
				LOG_DEBUG("Resizing log", NULL);
				for (Sum = 0, fseek(rlog, size - (glLogLimit*1024*1024) / 2, SEEK_SET);
					 (BufSize = fread(buf, 1, BufSize, rlog)) != 0;
					 Sum += BufSize, fwrite(buf, 1, BufSize, wlog));

				Sum = fresize(wlog, Sum);
				fclose(wlog);
				fclose(rlog);
				NFREE(buf);
				if (!freopen(glLogFile, "a", stderr)) {
					LOG_ERROR("re-open error while truncating log", NULL);
				}
			}
		}
	}
	return NULL;
}

/*----------------------------------------------------------------------------*/
static bool AddMRDevice(struct sMR *Device, char *UDN, IXML_Document *DescDoc, const char *location) {
	char *friendlyName = NULL;
	
	// read parameters from default then config file
	memcpy(&Device->Config, &glMRConfig, sizeof(tMRConfig));
	memcpy(&Device->sq_config, &glDeviceParam, sizeof(sq_dev_param_t));

	LoadMRConfig(glConfigID, UDN, &Device->Config, &Device->sq_config);
	if (!Device->Config.Enabled) return false;

	DeltaOptions(glDeviceParam.codecs, Device->sq_config.codecs);
	DeltaOptions(glDeviceParam.raw_audio_format, Device->sq_config.raw_audio_format);

	// Read key elements from description document
	friendlyName = XMLGetFirstDocumentItem(DescDoc, "friendlyName", true);
	if (!friendlyName || !*friendlyName) friendlyName = strdup(UDN);

	LOG_SDEBUG("UDN:\t%s\nFriendlyName:\t%s", UDN, friendlyName);

	Device->Magic 			= MAGIC;
	Device->sqState 		= SQ_STOP;
	Device->State			= STOPPED;
	Device->InfoExPoll 		= -1;
	Device->Volume 			= -1;
	Device->VolumeStampRx 	= Device->VolumeStampTx = gettime_ms() - 2000;
	Device->LastSeen		= gettime_ms() / 1000;
	// all this is set to 0 by memset ...
	Device->SqueezeHandle 	= 0;
	Device->ErrorCount 		= 0;
	Device->WaitCookie 		= Device->StartCookie = NULL;
	Device->seqN			= NULL;
	Device->TrackPoll 		= Device->StatePoll = 0;
	Device->Actions 		= NULL;
	Device->NextURI 		= Device->NextProtoInfo = NULL;
	Device->Master			= NULL;
	Device->Sink 			= NULL;
	if (Device->sq_config.roon_mode) {
		Device->on = true;
		Device->sq_config.use_cli = false;
	}

	Device->sq_config.send_icy = Device->Config.SendMetaData ? Device->Config.SendIcy : ICY_NONE;
	if (Device->sq_config.send_icy && !Device->Config.SendCoverArt) Device->sq_config.send_icy = ICY_TEXT;

	strcpy(Device->UDN, UDN);
	strcpy(Device->DescDocURL, location);

	memset(&Device->NextMetaData, 0, sizeof(metadata_t));
	memset(&Device->Service, 0, sizeof(struct sService) * NB_SRV);

	/* find the different services */
	for (int i = 0; i < NB_SRV; i++) {
		char *ServiceId = NULL, *ServiceType = NULL;
		char *EventURL = NULL, *ControlURL = NULL;
		char *ServiceURL = NULL;
		strcpy(Device->Service[i].Id, "");
		if (XMLFindAndParseService(DescDoc, location, cSearchedSRV[i].name, &ServiceType, &ServiceId, &EventURL, &ControlURL, &ServiceURL)) {
			struct sService *s = &Device->Service[cSearchedSRV[i].idx];
			LOG_SDEBUG("\tservice [%s] %s %s, %s, %s", cSearchedSRV[i].name, ServiceType, ServiceId, EventURL, ControlURL);

			strncpy(s->Id, ServiceId, RESOURCE_LENGTH-1);
			strncpy(s->ControlURL, ControlURL, RESOURCE_LENGTH-1);
			strncpy(s->EventURL, EventURL, RESOURCE_LENGTH - 1);
			strncpy(s->Type, ServiceType, RESOURCE_LENGTH - 1);
			s->TimeOut = cSearchedSRV[i].TimeOut;
		}
		NFREE(ServiceId);
		NFREE(ServiceType);
		NFREE(EventURL);
		NFREE(ControlURL);

		if (ServiceURL && cSearchedSRV[i].idx == AVT_SRV_IDX && !XMLFindAction(location, ServiceURL, "SetNextAVTransportURI") && Device->Config.AcceptNextURI == NEXT_GAPLESS) {
			LOG_INFO("[%p]: player can't do gapless or gapless disabled by config (%d)", Device, Device->Config.AcceptNextURI);
			Device->Config.AcceptNextURI = NEXT_GAPPED;
		}

		if (ServiceURL && cSearchedSRV[i].idx == AVT_SRV_IDX && XMLFindAction(location, ServiceURL, "GetInfoEx")) {
			LOG_INFO("[%p]: player has extended information", Device);
			Device->InfoExPoll = INFOEX_POLL;
		}

		NFREE(ServiceURL);
	}
	Device->Master = GetMaster(Device, &friendlyName);

	// set remaining items now that we are sure
	Device->Running = true;
	strcpy(Device->friendlyName, friendlyName);
	NFREE(friendlyName);
		
	if (!memcmp(Device->sq_config.mac, "\0\0\0\0\0\0", 6)) {
		char ip[32];
		uint32_t mac_size = 6;

		sscanf(location, "http://%[^:]", ip);
		if (SendARP(inet_addr(ip), INADDR_ANY, Device->sq_config.mac, &mac_size)) {
			*(uint32_t*)(Device->sq_config.mac + 2) = hash32(Device->UDN);
			LOG_INFO("[%p]: creating MAC", Device);
		}
		memset(Device->sq_config.mac, 0xbb, 2);
	}

	// virtual players duplicate mac address
	for (int i = 0; i < MAX_RENDERERS; i++) {
		if (glMRDevices[i].Running && Device != glMRDevices + i && !memcmp(&glMRDevices[i].sq_config.mac, &Device->sq_config.mac, 6)) {
			memset(Device->sq_config.mac, 0xbb, 2);
			*(uint32_t*)(Device->sq_config.mac + 2) = hash32(Device->UDN);
			LOG_INFO("[%p]: duplicated mac ... updating", Device);
		}
	}

	if (Device->Master) {
		LOG_INFO("[%p] skipping Sonos slave %s", Device, Device->friendlyName);
	} else {
		LOG_INFO("[%p]: adding renderer (%s) with mac %hX-%X", Device, Device->friendlyName, *(uint16_t*)Device->sq_config.mac, *(uint32_t*)(Device->sq_config.mac + 2));
	}

	// get the protocol info
	if ((Device->Sink = GetProtocolInfo(Device)) == NULL) {
		LOG_WARN("[%p] unable to get protocol info, set <forced_mimetypes>", Device);
		Device->Sink = strdup("");
	}

	// only check codecs in thru mode
	if (strcasestr(Device->sq_config.mode, "thru"))
		CheckCodecs(Device->sq_config.codecs, Device->Sink, Device->Config.ForcedMimeTypes);

	pthread_create(&Device->Thread, NULL, &MRThread, Device);

	/* subscribe here, not before */
	for (int i = 0; i < NB_SRV; i++) if (Device->Service[i].TimeOut)
		UpnpSubscribeAsync(glControlPointHandle, Device->Service[i].EventURL,
						   Device->Service[i].TimeOut, MasterHandler,
						   (void*) strdup(UDN));

	return (Device->Master == NULL);
}

/*----------------------------------------------------------------------------*/
static bool isExcluded(char *Model)
{
	char item[STR_LEN];
	char *p = glExcluded;
	if (!glExcluded) return false;
	do {
		sscanf(p, "%[^,]", item);
		if (strcasestr(Model, item)) return true;
		p += strlen(item);
	} while (*p++);
	return false;
}

/*----------------------------------------------------------------------------*/
static bool Start(void) {
	struct in_addr Host;
	unsigned short Port = 0;
	char addr[128] = "";

#if USE_SSL
	if (!cross_ssl_load()) {
		LOG_ERROR("Cannot load SSL libraries", NULL);
		return false;
	}
#endif

	// sscanf does not capture empty strings
	if (!strchr(glBinding, '?') && !sscanf(glBinding, "%[^:]:%hu", addr, &Port)) sscanf(glBinding, ":%hu", &Port);

	char* iface = NULL;
	Host = get_interface(addr, &iface, NULL);

	// can't find a suitable interface
	if (Host.s_addr == INADDR_NONE) {
		NFREE(iface);
		return false;
	}

	UpnpSetLogLevel(UPNP_CRITICAL);
	int rc = UpnpInit2(iface, Port);

	LOG_INFO("Binding to iface %s:%hu [%s] (http port %u)", inet_ntoa(Host), (unsigned short)UpnpGetServerPort(), iface, Port);
	NFREE(iface);

	if (rc != UPNP_E_SUCCESS) {
		LOG_ERROR("UPnP init failed: %d %s\n", rc, rc == UPNP_E_SOCKET_BIND ? "cannot bind socket(s)" : "");
		UpnpFinish();
		return false;
	}

	// must be set after initialization
	UpnpSetMaxContentLength(60000);

	// device mutexes are always initialized
	memset(&glMRDevices, 0, sizeof(glMRDevices));
	for (int i = 0; i < MAX_RENDERERS; i++) pthread_mutex_init(&glMRDevices[i].Mutex, 0);
	
	//if (!*glIPaddress) strcpy(glIPaddress, UpnpGetServerIpAddress());
	sq_init(Host, Port ? UpnpGetServerPort() : 0, glModelName);
	rc = UpnpRegisterClient(MasterHandler, NULL, &glControlPointHandle);

	if (rc != UPNP_E_SUCCESS) {
		LOG_ERROR("Error registering ControlPoint: %d", rc);
		UpnpFinish();
		return false;
	}

	// init mutex & cond no matter what
	pthread_mutex_init(&glUpdateMutex, 0);
	pthread_cond_init(&glUpdateCond, 0);
	queue_init(&glUpdateQueue, true, FreeUpdate);

	// start the main thread
	pthread_create(&glMainThread, NULL, &MainThread, NULL);
	pthread_create(&glUpdateThread, NULL, &UpdateThread, NULL);

	
	for (size_t i = 0; glDiscoveryPatterns[i]; i++) {
		LOG_INFO("UPnP search for %s", glDiscoveryPatterns[i]);
		UpnpSearchAsync(glControlPointHandle, DISCOVERY_TIME, glDiscoveryPatterns[i], NULL);
	}

	return true;
}

/*----------------------------------------------------------------------------*/
static bool Stop(void) {
	LOG_INFO("stopping squeezelite devices ...", NULL);
	sq_stop();

	// once main is finished, no risk to have new players created
	LOG_INFO("terminate update thread ...", NULL);
	pthread_cond_signal(&glUpdateCond);
	pthread_join(glUpdateThread, NULL);

	// simple log size management thread ... should be remove done day
	LOG_INFO("terminate main thread ...", NULL);
	crossthreads_wake();
	pthread_join(glMainThread, NULL);
	LOG_INFO("stopping UPnP devices ...", NULL);
	FlushMRDevices();
	LOG_DEBUG("un-register libupnp callbacks ...", NULL);
	UpnpUnRegisterClient(glControlPointHandle);
	LOG_DEBUG("end libupnp ...", NULL);
	UpnpFinish();

	// wait for UPnP to terminate to not have callbacks issues
	pthread_mutex_destroy(&glUpdateMutex);
	pthread_cond_destroy(&glUpdateCond);
	for (int i = 0; i < MAX_RENDERERS; i++)	{
		pthread_mutex_destroy(&glMRDevices[i].Mutex);
	}

	// remove discovered items
	queue_flush(&glUpdateQueue);
	if (glConfigID) ixmlDocument_free(glConfigID);

	netsock_close();
#if USE_SSL
	cross_ssl_free();
#endif

	return true;
}
/*---------------------------------------------------------------------------*/
static void sighandler(int signum) {
	int i;
	static bool quit = false;
	// give it some time to finish ...
	if (quit) {
		LOG_INFO("Please wait for clean exit!", NULL);
		return;
	}
	quit = true;
	glMainRunning = false;
	if (!glGracefullShutdown) {
		for (i = 0; i < MAX_RENDERERS; i++) {
			struct sMR *p = &glMRDevices[i];
			if (p->Running && p->sqState == SQ_PLAY) AVTStop(p);
		}
		LOG_INFO("forced exit", NULL);
		exit(EXIT_SUCCESS);
	}
	Stop();
	exit(EXIT_SUCCESS);
}

/*---------------------------------------------------------------------------*/
static void DeltaOptions(char* ref, char* src) {
	char item[4], * p;

	if (!strchr(src, '+') && !strchr(src, '-')) return;
	ref = strdup(ref);

	for (p = src; *p && (p = strchr(p, '+')) != NULL; p++) {
		if (sscanf(p, "+%3[^,]", item) <= 0) break;
		if (!strstr(ref, item)) {
			strcat(ref, ",");
			strcat(ref, item);
		}
	}

	for (p = src; *p && (p = strchr(p, '-')) != NULL; p++) {
		char* pos;
		if (sscanf(p, "-%3[^,]", item) <= 0) break;
		if ((pos = strstr(ref, item)) != NULL) {
			int n = strlen(item);
			if (pos[n]) n++;
			memmove(pos, pos + n, strlen(pos + n) + 1);
		}
	}

	strcpy(src, ref);
	free(ref);
}

/*---------------------------------------------------------------------------*/
bool ParseArgs(int argc, char **argv) {
	char *optarg = NULL;
	int i, optind = 1;
	char cmdline[256] = "";
	for (i = 0; i < argc && (strlen(argv[i]) + strlen(cmdline) + 2 < sizeof(cmdline)); i++) {
		strcat(cmdline, argv[i]);
		strcat(cmdline, " ");
	}
	while (optind < argc && strlen(argv[optind]) >= 2 && argv[optind][0] == '-') {
		char *opt = argv[optind] + 1;
		if (strstr("stxdfpibcMo", opt) && optind < argc - 1) {
			optarg = argv[optind + 1];
			optind += 2;
		} else if (strstr("tzZIk", opt)) {
			optarg = NULL;
			optind += 1;
		} else {
			printf("%s", usage);
			return false;
		}
		switch (opt[0]) {
		case 'c':
			strcpy(glDeviceParam.store_prefix, optarg);
			break;
		case 'o':
			strcpy(glDeviceParam.mode, optarg);
			break;
		case 's':
			strcpy(glDeviceParam.server, optarg);
			break;
		case 'M':
			strcpy(glModelName, optarg);
			break;
		case 'b':
			strcpy(glBinding, optarg);
			break;
		case 'f':
			glLogFile = optarg;
			break;
		case 'i':
			strcpy(glConfigName, optarg);
			glDiscovery = true;
			break;
		case 'I':
			glAutoSaveConfigFile = true;
			break;
		case 'p':
			glPidFile = optarg;
			break;
		case 'Z':
			glInteractive = false;
			break;
		case 'k':
			glGracefullShutdown = false;
			break;
#if LINUX || FREEBSD || SUNOS
		case 'z':
			glDaemonize = true;
			break;
#endif
		case 'd':
			{
				char *l = strtok(optarg, "=");
				char *v = strtok(NULL, "=");
				log_level new = lWARN;
				if (l && v) {
					if (!strcmp(v, "error"))  new = lERROR;
					if (!strcmp(v, "warn"))   new = lWARN;
					if (!strcmp(v, "info"))   new = lINFO;
					if (!strcmp(v, "debug"))  new = lDEBUG;
					if (!strcmp(v, "sdebug")) new = lSDEBUG;
					if (!strcmp(l, "all") || !strcmp(l, "slimproto"))	slimproto_loglevel = new;
					if (!strcmp(l, "all") || !strcmp(l, "slimmain"))	slimmain_loglevel = new;
					if (!strcmp(l, "all") || !strcmp(l, "stream"))    	stream_loglevel = new;
					if (!strcmp(l, "all") || !strcmp(l, "decode"))    	decode_loglevel = new;
					if (!strcmp(l, "all") || !strcmp(l, "output"))    	output_loglevel = new;
					if (!strcmp(l, "all") || !strcmp(l, "main"))     	main_loglevel = new;
					if (!strcmp(l, "all") || !strcmp(l, "util"))    	util_loglevel = new;
					if (!strcmp(l, "all") || !strcmp(l, "upnp"))    	upnp_loglevel = new;
				} else {
					printf("%s", usage);
					return false;
				}
			}
			break;
		case 't':
			printf("%s", license);
			return false;
		default:
			break;
		}
	}
	return true;
}

/*----------------------------------------------------------------------------*/
/*																			  */
/*----------------------------------------------------------------------------*/
int main(int argc, char *argv[])
{
	int i;
	char resp[20] = "";
	signal(SIGINT, sighandler);
	signal(SIGTERM, sighandler);
#if defined(SIGQUIT)
	signal(SIGQUIT, sighandler);
#endif
#if defined(SIGHUP)
	signal(SIGHUP, sighandler);
#endif

	netsock_init();

	// first try to find a config file on the command line
	for (i = 1; i < argc; i++) {
		if (!strcmp(argv[i], "-x")) {
			strcpy(glConfigName, argv[i+1]);
		}
	}
	// load config from xml file
	glConfigID = (void*) LoadConfig(glConfigName, &glMRConfig, &glDeviceParam);

	// parse custom pattern of SSDP discovery
	char* token = strtok(glCustomDiscovery, ",;");
	for (int i = 0; token && i < sizeof(glDiscoveryPatterns) / sizeof(char*); i++) {
		if (glDiscoveryPatterns[i]) continue;
		LOG_INFO("adding SSDP discovery %s", token);
		// we don't need to co^py the tokens, they are valid runtime-wide
		glDiscoveryPatterns[i] = token;
		token = strtok(NULL, ",;");	
	}

	// potentially overwrite with some cmdline parameters
	if (!ParseArgs(argc, argv)) exit(1);
	if (glLogFile) {
		if (!freopen(glLogFile, "a", stderr)) {
			fprintf(stderr, "error opening logfile %s: %s\n", glLogFile, strerror(errno));
		}
	}

	LOG_WARN("Starting squeeze2upnp version: %s", VERSION);

	if (strtod("0.30", NULL) != 0.30) {
		LOG_WARN("weird GLIBC, try -static build in case of failure");
	}

	if (!glConfigID) {
		LOG_ERROR("\n\n!!!!!!!!!!!!!!!!!! ERROR LOADING CONFIG FILE !!!!!!!!!!!!!!!!!!!!!\n", NULL);
	}
	// just do discovery and exit
	if (glDiscovery) {
		Start();
		sleep(DISCOVERY_TIME + 1);
		glMainRunning = false;
		Stop();
		return(0);
	}

#if LINUX || FREEBSD || SUNOS
	if (glDaemonize) {
		if (daemon(1, glLogFile ? 1 : 0)) {
			fprintf(stderr, "error daemonizing: %s\n", strerror(errno));
		}
	}
#endif
	if (glPidFile) {
		FILE *pid_file;
		pid_file = fopen(glPidFile, "wb");
		if (pid_file) {
			fprintf(pid_file, "%d", getpid());
			fclose(pid_file);
		} else {
			LOG_ERROR("Cannot open PID file %s", glPidFile);
		}
	}
	if (!Start()) {

		LOG_ERROR("Cannot start, exiting", NULL);
		exit(0);
	}

	while (strcmp(resp, "exit")) {

#if LINUX || FREEBSD || SUNOS
		if (!glDaemonize && glInteractive)
			i = scanf("%s", resp);
		else pause();
#else
		if (glInteractive)
			i = scanf("%s", resp);
		else
#if OSX
			pause();
#else
			Sleep(INFINITE);
#endif
#endif

		SET_LOGLEVEL(stream);
		SET_LOGLEVEL(output);
		SET_LOGLEVEL(decode);
		SET_LOGLEVEL(slimproto);
		SET_LOGLEVEL(slimmain);
		SET_LOGLEVEL(main);
		SET_LOGLEVEL(util);
		SET_LOGLEVEL(upnp);

		if (!strcmp(resp, "save"))	{
			char name[128];
			i = scanf("%s", name);
			SaveConfig(name, glConfigID, true);
		}

		if (!strcmp(resp, "dump") || !strcmp(resp, "dumpall"))	{
			uint32_t now = gettime_ms() / 1000;
			bool all = !strcmp(resp, "dumpall");

			for (i = 0; i < MAX_RENDERERS; i++) {
				struct sMR *p = &glMRDevices[i];
				bool Locked = pthread_mutex_trylock(&p->Mutex);

				if (!Locked) pthread_mutex_unlock(&p->Mutex);
				if (!p->Running && !all) continue;
				printf("%20.20s [r:%u] [l:%u] [s:%u] Last:%u eCnt:%u [%p::%p]\n",
						p->friendlyName, p->Running, Locked, p->State,
						now - p->LastSeen, p->ErrorCount,
						p, sq_get_ptr(p->SqueezeHandle));
			}
		}

	}

	// must be protected in case this interrupts a UPnPEventProcessing
	glMainRunning = false;
	Stop();
	LOG_INFO("all done", NULL);
	return true;
}
