/*
 *  Squeeze2upnp - LMS to uPNP gateway
 *
 *  Squeezelite : (c) Adrian Smith 2012-2014, triode1@btinternet.com
 *  Additions & gateway : (c) Philippe 2014, philippe_44@outlook.com
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

#include <math.h>
#include <fcntl.h>
#include <sys/types.h>
#include <sys/stat.h>

#include "squeezedefs.h"
#if WIN
#include <process.h>
#endif
#include "squeeze2upnp.h"
#include "upnpdebug.h"
#include "upnptools.h"
#include "webserver.h"
#include "util_common.h"
#include "util.h"
#include "avt_util.h"
#include "mr_util.h"

/*
TODO :
- for no pause, the solution will be to send the elapsed time to LMS through CLI so that it does take care of the seek
- samplerate management will have to be reviewed when decode will be used
*/

#define	AV_TRANSPORT 	"urn:schemas-upnp-org:service:AVTransport"
#define	RENDERING_CTRL 	"urn:schemas-upnp-org:service:RenderingControl"
#define	CONNECTION_MGR 	"urn:schemas-upnp-org:service:ConnectionManager"

/*----------------------------------------------------------------------------*/
/* globals initialized */
/*----------------------------------------------------------------------------*/
char				glBaseVDIR[] = "LMS2UPNP";

#if LINUX || FREEBSD
bool				glDaemonize = false;
#endif
bool				glInteractive = true;
char				*glLogFile;
s32_t				glLogLimit = -1;
static char			*glPidFile = NULL;
static char			*glSaveConfigFile = NULL;
bool				glAutoSaveConfigFile = false;
bool				glGracefullShutdown = true;

log_level	slimproto_loglevel = lWARN;
log_level	stream_loglevel = lWARN;
log_level	decode_loglevel = lWARN;
log_level	output_loglevel = lWARN;
log_level	web_loglevel = lWARN;
log_level	main_loglevel = lINFO;
log_level	slimmain_loglevel = lINFO;
log_level	util_loglevel = lWARN;
log_level	upnp_loglevel = lINFO;

tMRConfig			glMRConfig = {
							"-3",
							SQ_STREAM,
							false,
							false,
							false,
							0,
							true,
							"",
							1,
							true,
							true,
							0,
							true,
							true,
							100,
							1,
							"raw",
							1,
							false,
							false,
					};

static u8_t LMSVolumeMap[101] = {
				0, 1, 1, 1, 2, 2, 2, 3,  3,  4,
				5, 5, 6, 6, 7, 8, 9, 9, 10, 11,
				12, 13, 14, 15, 16, 16, 17, 18, 19, 20,
				22, 23, 24, 25, 26, 27, 28, 29, 30, 32,
				33, 34, 35, 37, 38, 39, 40, 42, 43, 44,
				46, 47, 48, 50, 51, 53, 54, 56, 57, 59,
				60, 61, 63, 65, 66, 68, 69, 71, 72, 74,
				75, 77, 79, 80, 82, 84, 85, 87, 89, 90,
				92, 94, 96, 97, 99, 101, 103, 104, 106, 108, 110,
				112, 113, 115, 117, 119, 121, 123, 125, 127, 128
			};

sq_dev_param_t glDeviceParam = {
					 // both are multiple of 3*4(2) for buffer alignement on sample
					(200 * 1024 * (4*3)),
					(200 * 1024 * (4*3)),
					SQ_STREAM,
					{ 	SQ_RATE_384000, SQ_RATE_352000, SQ_RATE_192000, SQ_RATE_176400,
						SQ_RATE_96000, SQ_RATE_48000, SQ_RATE_44100,
						SQ_RATE_32000, SQ_RATE_24000, SQ_RATE_22500, SQ_RATE_16000,
						SQ_RATE_12000, SQ_RATE_11025, SQ_RATE_8000, 0 },
					-1,         // get can retrun as much as required by UPnP
					"pcm,flc,mp3",
					"?",
					SQ_RATE_48000,
					L24_PACKED_LPCM,
					FLAC_NORMAL_HEADER,
					"?",
					"",
					-1L,
					1024*1024,
					0,
					{ 0x00,0x00,0x00,0x00,0x00,0x00 },
					false,
					true,
					{ "" },
				} ;

/*----------------------------------------------------------------------------*/
/* globals */
/*----------------------------------------------------------------------------*/
static ithread_t 	glMainThread;
char				glUPnPSocket[128] = "?";
unsigned int 		glPort;
char 				glIPaddress[128] = "";
UpnpClient_Handle 	glControlPointHandle;
void				*glConfigID = NULL;
char				glConfigName[SQ_STR_LENGTH] = "./config.xml";
static bool			glDiscovery = false;
u32_t				gluPNPScanInterval = SCAN_INTERVAL;
u32_t				gluPNPScanTimeout = SCAN_TIMEOUT;
struct sMR			glMRDevices[MAX_RENDERERS];
ithread_mutex_t		glMRFoundMutex;

/*----------------------------------------------------------------------------*/
/* consts or pseudo-const*/
/*----------------------------------------------------------------------------*/
static const char 	MEDIA_RENDERER[] 	= "urn:schemas-upnp-org:device:MediaRenderer:1";

static const char 	cLogitech[] 		= "Logitech";
static const struct cSearchedSRV_s
{
 char 	name[RESOURCE_LENGTH];
 int	idx;
 u32_t  TimeOut;
} cSearchedSRV[NB_SRV] = {	{AV_TRANSPORT, AVT_SRV_IDX, 0},
						{RENDERING_CTRL, REND_SRV_IDX, 30},
						{CONNECTION_MGR, CNX_MGR_IDX, 0}
				   };

/*----------------------------------------------------------------------------*/
/* locals */
/*----------------------------------------------------------------------------*/
static log_level 	*loglevel = &main_loglevel;
ithread_t			glUpdateMRThread;
static bool			glMainRunning = true;
static struct sLocList {
	char 			*Location;
	struct sLocList *Next;
} *glMRFoundList = NULL;

static char usage[] =
			VERSION "\n"
		   "See -t for license terms\n"
		   "Usage: [options]\n"
		   "  -s <server>[:<port>]\tConnect to specified server, otherwise uses autodiscovery to find server\n"
		   "  -b <address>]\tNetwork address to bind to\n"
		   "  -x <config file>\tread config from file (default is ./config.xml)\n"
		   "  -i <config file>\tdiscover players, save <config file> and exit\n"
		   "  -I \t\t\tauto save config at every network scan\n"
//		   "  -c <codec1>,<codec2>\tRestrict codecs to those specified, otherwise load all available codecs; known codecs: " CODECS "\n"
//		   "  -e <codec1>,<codec2>\tExplicitly exclude native support of one or more codecs; known codecs: " CODECS "\n"
		   "  -f <logfile>\t\tWrite debug to logfile\n"
  		   "  -p <pid file>\t\twrite PID in file\n"
		   "  -d <log>=<level>\tSet logging level, logs: all|slimproto|stream|decode|output|web|main|util|upnp, level: error|warn|info|debug|sdebug\n"
#if RESAMPLE
		   "  -R -u [params]\tResample, params = <recipe>:<flags>:<attenuation>:<precision>:<passband_end>:<stopband_start>:<phase_response>,\n"
		   "  \t\t\t recipe = (v|h|m|l|q)(L|I|M)(s) [E|X], E = exception - resample only if native rate not supported, X = async - resample to max rate for device, otherwise to max sync rate\n"
		   "  \t\t\t flags = num in hex,\n"
		   "  \t\t\t attenuation = attenuation in dB to apply (default is -1db if not explicitly set),\n"
		   "  \t\t\t precision = number of bits precision (NB. HQ = 20. VHQ = 28),\n"
		   "  \t\t\t passband_end = number in percent (0dB pt. bandwidth to preserve. nyquist = 100%%),\n"
		   "  \t\t\t stopband_start = number in percent (Aliasing/imaging control. > passband_end),\n"
		   "  \t\t\t phase_response = 0-100 (0 = minimum / 50 = linear / 100 = maximum)\n"
#endif
#if DSD
		   "  -D [delay]\t\tOutput device supports DSD over PCM (DoP), delay = optional delay switching between PCM and DoP in ms\n"
#endif
#if LINUX || FREEBSD
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
#if EVENTFD
		   " EVENTFD"
#endif
#if SELFPIPE
		   " SELFPIPE"
#endif
#if WINEVENT
		   " WINEVENT"
#endif
#if RESAMPLE_MP
		   " RESAMPLE_MP"
#else
#if RESAMPLE
		   " RESAMPLE"
#endif
#endif
#if FFMPEG
		   " FFMPEG"
#endif
#if DSD
		   " DSD"
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
		   "along with this program.  If not, see <http://www.gnu.org/licenses/>.\n\n"
#if DSD
		   "Contains dsd2pcm library Copyright 2009, 2011 Sebastian Gesemann which\n"
		   "is subject to its own license.\n\n"
#endif
	;

/*----------------------------------------------------------------------------*/
/* prototypes */
/*----------------------------------------------------------------------------*/
static void *MRThread(void *args);
static void *UpdateMRThread(void *args);
static bool AddMRDevice(struct sMR *Device, char * UDN, IXML_Document *DescDoc,	const char *location);
static int	uPNPTerminate(void);

/*----------------------------------------------------------------------------*/
bool sq_callback(sq_dev_handle_t handle, void *caller, sq_action_t action, u8_t *cookie, void *param)
{
	struct sMR *device = caller;
	char *p = (char*) param;
	bool rc = true;

	if (!device)	{
		LOG_ERROR("No caller ID in callback", NULL);
		return false;
	}

	if (action == SQ_ONOFF) {
		device->on = *((bool*) param);

		if (device->on && device->Config.AutoPlay)
			sq_notify(device->SqueezeHandle, device, SQ_PLAY, NULL, &device->on);

		LOG_DEBUG("[%p]: device set on/off %d", caller, device->on);
	}

	if (!device->on && action != SQ_SETNAME && action != SQ_SETSERVER) {
		LOG_DEBUG("[%p]: device off or not controlled by LMS", caller);
		return false;
	}

	LOG_SDEBUG("callback for %s", device->FriendlyName);
	ithread_mutex_lock(&device->Mutex);

	switch (action) {

		case SQ_SETURI:
		case SQ_SETNEXTURI: {
			sq_seturi_t *p = (sq_seturi_t*) param;
			char uri[SQ_STR_LENGTH];

			LOG_INFO("[%p]: codec:%c, ch:%d, s:%d, r:%d", device, p->codec,
										p->channels, p->sample_size, p->sample_rate);

			sq_get_metadata(device->SqueezeHandle, &device->MetaData, (action == SQ_SETURI) ? false : true);
			p->file_size = device->MetaData.file_size ?
						   device->MetaData.file_size :
						   Codec2Length(p->codec, device->Config.StreamLength);

			p->duration 	= device->MetaData.duration;
			p->src_format 	= ext2format(device->MetaData.path);
			p->remote 		= device->MetaData.remote;
			p->track_hash	= device->MetaData.track_hash;
			if (!device->Config.SendCoverArt) NFREE(device->MetaData.artwork);

			// must be done *after* duration has been set
			if (!SetContentType(device, p)) {
				LOG_ERROR("[%p]: no matching codec in player", caller);
				sq_free_metadata(&device->MetaData);
				rc = false;
				break;
			}

			sprintf(uri, "http://%s:%d/%s/%s.%s", glIPaddress, glPort, glBaseVDIR, p->name, p->ext);

			// to detect properly transition
			NFREE(device->NextURI);
			strcpy(device->ProtoInfo, p->proto_info);

			if (action == SQ_SETNEXTURI) {
				device->NextURI = strdup(uri);
				device->NextDuration = p->duration;

				if (device->Config.AcceptNextURI && device->NextDuration > device->Config.MinGapless * 1000){
					AVTSetNextURI(device);
					sq_free_metadata(&device->MetaData);
				}

				LOG_INFO("[%p]: next URI set %s", device, device->NextURI);
			}
			else {
				NFREE(device->CurrentURI);
				device->CurrentURI = strdup(uri);
				device->Duration = p->duration;

				AVTSetURI(device);
				sq_free_metadata(&device->MetaData);

				LOG_INFO("[%p]: current URI set %s", device, device->CurrentURI);
			}

			break;
		}
		case SQ_UNPAUSE:
			if (device->Config.SeekAfterPause) {
				u32_t PausedTime = sq_get_time(device->SqueezeHandle);
				sq_set_time(device->SqueezeHandle, PausedTime);
				break;
			}
			if (device->CurrentURI) {
				if (device->Muted && device->Config.VolumeOnPlay != -1) {
					CtrlSetMute(device, false, device->seqN++);
					device->Muted = false;
				}

				AVTPlay(device);
				device->sqState = SQ_PLAY;
			}
			break;
		case SQ_PLAY:
			if (device->CurrentURI) {
				AVTSetPlayMode(device);
				AVTPlay(device);
				device->sqState = SQ_PLAY;

				if (device->Muted && device->Config.VolumeOnPlay != -1) {
					CtrlSetMute(device, false, device->seqN++);
					device->Muted = false;
				}

				if (device->Config.VolumeOnPlay == 1)
					CtrlSetVolume(device, device->Volume, device->seqN++);
			}
			else rc = false;
			break;
		case SQ_STOP:
			AVTStop(device);
			NFREE(device->CurrentURI);
			NFREE(device->NextURI);
			sq_free_metadata(&device->MetaData);
			device->sqState = action;
			device->ExpectStop = true;
			break;
		case SQ_PAUSE:
			AVTBasic(device, "Pause");
			device->sqState = action;
			break;
		case SQ_NEXT:
			AVTBasic(device, "Next");
			break;
		case SQ_SEEK:
			break;
		case SQ_VOLUME: {
			u32_t Volume = *(u16_t*)p;
			int i;

			// calculate volume and check for change
			for (i = 100; Volume < LMSVolumeMap[i] && i; i--);
			Volume = (i * device->Config.MaxVolume) / 100;
			if (device->Volume == Volume) break;
			device->Volume = Volume;

			// memorise but do not transmit so that we can compare
			if (device->Config.VolumeOnPlay == -1) break;

			// only transmit while playing
			if (!device->Config.VolumeOnPlay || device->sqState == SQ_PLAY) {
				if (device->Volume) {
					if (device->Muted) CtrlSetMute(device, false, device->seqN++);
					CtrlSetVolume(device, device->Volume, device->seqN++);
				}
				else CtrlSetMute(device, true, device->seqN++);
				device->Muted = (device->Volume == 0);
			}

			break;
		}
		case SQ_SETNAME:
			strcpy(device->sq_config.name, param);
			break;
		case SQ_SETSERVER:
			strcpy(device->sq_config.dynamic.server, inet_ntoa(*(struct in_addr*) param));
			break;
		default:
			break;
	}

	ithread_mutex_unlock(&device->Mutex);
	return rc;
}


/*----------------------------------------------------------------------------*/
void SyncNotifState(char *State, struct sMR* Device)
{
	sq_event_t Event = SQ_NONE;
	bool Param = false;

	// an update can have happended that has destroyed the device
	if (!Device->InUse) return;

	ithread_mutex_lock(&Device->Mutex);

	// in transitioning mode, do nothing, just wait
	if (!strcmp(State, "TRANSITIONING")) {
		if (Device->State != TRANSITIONING) {
			LOG_INFO("%s: uPNP transition", Device->FriendlyName);
		}
		Device->State = TRANSITIONING;
		ithread_mutex_unlock(&Device->Mutex);
		return;
	}

	if (!strcmp(State, "STOPPED") && Device->State != STOPPED) {
		if (Device->NextURI) {
			if (!Device->Config.AcceptNextURI || Device->NextDuration < Device->Config.MinGapless) {
				// fake a "SETURI" and a "PLAY" request
				NFREE(Device->CurrentURI);
				Device->CurrentURI = malloc(strlen(Device->NextURI) + 1);
				strcpy(Device->CurrentURI, Device->NextURI);
				NFREE(Device->NextURI);
				Device->Duration = Device->NextDuration;
				Device->NextDuration = 0;

				AVTSetURI(Device);
				sq_free_metadata(&Device->MetaData);
				AVTPlay(Device);

				Event = SQ_TRACK_CHANGE;
				LOG_INFO("[%p]: no gapless %s", Device, Device->CurrentURI);
			}
			else  {
				/*
				This can happen if the SetNextURI has been sent too late and
				the device did not have time to buffer next track data. In
				this case, give it a nudge
				*/
				AVTBasic(Device, "Next");
				LOG_WARN("[%p]: nudge next track required %s", Device, Device->NextURI);
			}
		}
		// This is an end of track and nothing else to play or a LMS stop
		else {
			LOG_INFO("%s: uPNP stop", Device->FriendlyName);
			if (Device->State == PAUSED || !Device->ExpectStop) Param = true;
			Event = SQ_STOP;
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
				LOG_ERROR("[%s]: unhandled playing", Device->FriendlyName);
				break;
			}
		}

		LOG_INFO("%s: uPNP playing", Device->FriendlyName);
		Device->State = PLAYING;
		Device->ExpectStop = false;
		// avoid double play (causes a restart) in case of unsollicited play
	}

	if (!strcmp(State, "PAUSED_PLAYBACK") && Device->State != PAUSED) {
		// detect unsollicited pause, but do not confuse it with a fast pause/play
		if (Device->sqState != SQ_PAUSE) {
			Event = SQ_PAUSE;
			Param = true;
		}
		LOG_INFO("%s: uPNP pause", Device->FriendlyName);
		Device->State = PAUSED;
	}

	ithread_mutex_unlock(&Device->Mutex);
	/*
	Squeeze "domain" execution has the right to consume own mutexes AND callback
	upnp "domain" function that will consume upnp "domain" mutex, but the reverse
	cannot be true otherwise deadlocks will occur
	*/
	if (Event != SQ_NONE)
		sq_notify(Device->SqueezeHandle, Device, Event, NULL, &Param);
}


/*----------------------------------------------------------------------------*/
bool ProcessQueue(struct sMR *Device)
{
	struct sService *Service = &Device->Service[AVT_SRV_IDX];
	tAction *Action;
	int rc = 0;

	Device->WaitCookie = 0;
	if ((Action = QueueExtract(&Device->ActionQueue)) == NULL) return false;

	Device->WaitCookie = Device->seqN++;
	rc = UpnpSendActionAsync(glControlPointHandle, Service->ControlURL, Service->Type,
							 NULL, Action->ActionNode, CallbackActionHandler, Device->WaitCookie);

	if (rc != UPNP_E_SUCCESS) {
		LOG_ERROR("Error in queued UpnpSendActionAsync -- %d", rc);
	}

	free(Action);
	ixmlDocument_free(Action->ActionNode);

	return (rc == 0);
}


/*----------------------------------------------------------------------------*/
void ProcessVolume(char *Volume, struct sMR* Device)
{
	u16_t UPnPVolume = atoi(Volume);

	LOG_SDEBUG("[%p]: Volume %s", Device, Volume);

	if (UPnPVolume != Device->Volume) {
		LOG_INFO("[%p]: UPnP Volume local change %d", Device, UPnPVolume);
		UPnPVolume =  (UPnPVolume * 100) / Device->Config.MaxVolume;
		sq_notify(Device->SqueezeHandle, Device, SQ_VOLUME, NULL, &UPnPVolume);
	}
}


/*----------------------------------------------------------------------------*/
void HandleStateEvent(struct Upnp_Event *Event, void *Cookie)
{
	struct sMR *Device;
	IXML_Document *VarDoc = Event->ChangedVariables;
	char  *r = NULL;
	char  *LastChange = NULL;

	Device = SID2Device(Event->Sid);

	if (!Device) {
		LOG_SDEBUG("no Squeeze device (yet) for %s", Event->Sid);
		return;
	}

	if (Device->Magic != MAGIC) {
		LOG_ERROR("[%p]: Wrong magic ", Device);
		return;
	}

	if (!Device->on) {
		LOG_DEBUG("[%p]: device off, ignored ", Device);
		return;
	}

	LastChange = XMLGetFirstDocumentItem(VarDoc, "LastChange");
	LOG_SDEBUG("Data event %s %u %s", Event->Sid, Event->EventKey, LastChange);
	if (!LastChange) return;
	NFREE(LastChange);

	// Feedback volume to LMS if authorized
	if (Device->Config.VolumeFeedback) {
		r = XMLGetChangeItem(VarDoc, "Volume", "channel", "Master", "val");
		if (r) ProcessVolume(r, Device);
		NFREE(r);
	}
}


/*----------------------------------------------------------------------------*/
int CallbackActionHandler(Upnp_EventType EventType, void *Event, void *Cookie)
{
	LOG_SDEBUG("action: %i [%s] [%p]", EventType, uPNPEvent2String(EventType), Cookie);

	switch ( EventType ) {
		case UPNP_CONTROL_ACTION_COMPLETE: 	{
			struct Upnp_Action_Complete *Action = (struct Upnp_Action_Complete *)Event;
			struct sMR *p;
			char   *r;

			p = CURL2Device(Action->CtrlUrl);
			if (!p) break;

			LOG_SDEBUG("[%p]: ac %i %s (cookie %p)", p, EventType, Action->CtrlUrl, Cookie);

			// If waited action has been completed, proceed to next one if any
			if (p->WaitCookie)  {
				const char *Resp = XMLGetLocalName(Action->ActionResult, 1);

				LOG_DEBUG("[%p]: Waited action %s", p, Resp ? Resp : "<none>");

				// discard everything else except waiting action
				if (Cookie != p->WaitCookie) break;

				ithread_mutex_lock(&p->Mutex);

				p->StartCookie = p->WaitCookie;
				ProcessQueue(p);

				/*
				when certain waited action has been completed, the state need
				to be re-acquired because a 'stop' state might be missed when
				(eg) repositionning where two consecutive status update will
				give 'playing', the 'stop' in the middle being unseen
				*/
				if (Resp && (!strcasecmp(Resp, "StopResponse") ||
							 !strcasecmp(Resp, "PlayResponse") ||
							 !strcasecmp(Resp, "PauseResponse"))) {
					p->State = UNKNOWN;
				}

				ithread_mutex_unlock(&p->Mutex);
				break;
			}

			// don't proceed anything that is too old
			if (Cookie < p->StartCookie) break;

			// transport state response
			r = XMLGetFirstDocumentItem(Action->ActionResult, "CurrentTransportState");
			if (r) SyncNotifState(r, p);
			NFREE(r);

			// When not playing, position is not reliable
			if (p->State == PLAYING) {
				// Get position in current track
				r = XMLGetFirstDocumentItem(Action->ActionResult, "RelTime");
				if (r) {
					p->Elapsed = 1000L * Time2Int(r);
					if (!p->NextURI && p->Duration && (p->Elapsed + 5000 > p->Duration)) p->ExpectStop = true;
					LOG_SDEBUG("[%p]: position %d (cookie %p)", p, p->Elapsed, Cookie);
					// discard any time info unless we are confirmed playing
					sq_notify(p->SqueezeHandle, p, SQ_TIME, NULL, &p->Elapsed);
				}
				NFREE(r);
			}

			// URI detection response
			r = XMLGetFirstDocumentItem(Action->ActionResult, "TrackURI");
			if (r && (*r == '\0' || !strstr(r, "-idx-"))) {
				char *s;
				IXML_Document *doc;
				IXML_Node *node;

				NFREE(r);
				s = XMLGetFirstDocumentItem(Action->ActionResult, "TrackMetaData");
				doc = ixmlParseBuffer(s);
				NFREE(s);

				node = (IXML_Node*) ixmlDocument_getElementById(doc, "res");
				if (node) node = (IXML_Node*) ixmlNode_getFirstChild(node);
				if (node) r = strdup(ixmlNode_getNodeValue(node));

				LOG_DEBUG("[%p]: no Current URI, use MetaData %s", p, r);
				if (doc) ixmlDocument_free(doc);
			}

			if (r && p->CurrentURI) {
				// mutex has to be set BEFORE test and unset BEFORE notification
				ithread_mutex_lock(&p->Mutex);
				/*
				an URI change detection is only valid if there is a nextURI
				pending, otherwise this is false alarm due to de-sync between
				the two players
				*/
				if (strcmp(r, p->CurrentURI) && (p->State == PLAYING) && p->NextURI && strstr(r, "-idx-")) {
					LOG_INFO("Detected URI change %s %s", p->CurrentURI, r);
					NFREE(p->CurrentURI);
					NFREE(p->NextURI);
					p->Duration = p->NextDuration;
					p->CurrentURI = malloc(strlen(r) + 1);
					strcpy(p->CurrentURI, r);
					ithread_mutex_unlock(&p->Mutex);
					sq_notify(p->SqueezeHandle, p, SQ_TRACK_CHANGE, NULL, NULL);
				}
				else ithread_mutex_unlock(&p->Mutex);
			}
			NFREE(r);

			LOG_SDEBUG("Action complete : %i (cookie %p)", EventType, Cookie);

			if (Action->ErrCode != UPNP_E_SUCCESS) {
				p->ErrorCount++;
				LOG_ERROR("Error in action callback -- %d (cookie %p)",	Action->ErrCode, Cookie);
			}
			else p->ErrorCount = 0;
			break;
		}
		default:
			break;
	}

	Cookie = Cookie;
	return 0;
}

/*----------------------------------------------------------------------------*/
int CallbackEventHandler(Upnp_EventType EventType, void *Event, void *Cookie)
{
	LOG_SDEBUG("event: %i [%s] [%p]", EventType, uPNPEvent2String(EventType), Cookie);

	switch ( EventType ) {
		case UPNP_DISCOVERY_ADVERTISEMENT_ALIVE:
		break;
		case UPNP_DISCOVERY_SEARCH_RESULT: {
			struct Upnp_Discovery *d_event = (struct Upnp_Discovery *) Event;
			struct sLocList **p, *prev = NULL;

			LOG_DEBUG("Answer to uPNP search %d", d_event->Location);
			if (d_event->ErrCode != UPNP_E_SUCCESS) {
				LOG_SDEBUG("Error in Discovery Callback -- %d", d_event->ErrCode);
				break;
			}

			ithread_mutex_lock(&glMRFoundMutex);
			p = &glMRFoundList;
			while (*p) {
				prev = *p;
				p = &((*p)->Next);
			}
			(*p) = (struct sLocList*) malloc(sizeof (struct sLocList));
			(*p)->Location = strdup(d_event->Location);
			(*p)->Next = NULL;
			if (prev) prev->Next = *p;
			ithread_mutex_unlock(&glMRFoundMutex);
			break;
		}
		case UPNP_DISCOVERY_SEARCH_TIMEOUT:	{
			pthread_attr_t attr;

			pthread_attr_init(&attr);
			pthread_attr_setstacksize(&attr, PTHREAD_STACK_MIN + 32*1024);
			pthread_create(&glUpdateMRThread, &attr, &UpdateMRThread, NULL);
			pthread_detach(glUpdateMRThread);
			pthread_attr_destroy(&attr);
			break;
		}
		case UPNP_EVENT_RECEIVED:
			HandleStateEvent(Event, Cookie);
			break;
		case UPNP_CONTROL_GET_VAR_COMPLETE:
			LOG_ERROR("Unexpected GetVarComplete", NULL);
			break;
		case UPNP_CONTROL_ACTION_COMPLETE: {
			struct Upnp_Action_Complete *Action = (struct Upnp_Action_Complete *)Event;
			struct sMR *p;
			char   *r;

			p = CURL2Device(Action->CtrlUrl);
			if (!p) break;

			r = XMLGetFirstDocumentItem(Action->ActionResult, "Sink");
			if (r) {
				LOG_DEBUG("[%p]: ProtocolInfo %s (cookie %p)", p, r, Cookie);
				ParseProtocolInfo(p, r);
			}
			NFREE(r);
			break;
		}
		case UPNP_DISCOVERY_ADVERTISEMENT_BYEBYE: {
			struct Upnp_Discovery *d_event = (struct Upnp_Discovery *) Event;
			struct sMR *p = UDN2Device(d_event->DeviceId);

			if (!p) break;

			ithread_mutex_lock(&p->Mutex);

			if (!*d_event->ServiceType && p->UPnPConnected) {
				p->UPnPConnected = false;
				LOG_INFO("[%p]: Player BYE-BYE", p);
			}

			ithread_mutex_unlock(&p->Mutex);

			break;
		}
		case UPNP_EVENT_AUTORENEWAL_FAILED: {
			struct Upnp_Event_Subscribe *d_Event = (struct Upnp_Event_Subscribe *)Event;
			struct sMR *p = SID2Device(d_Event->Sid);

			if (!p) break;

			ithread_mutex_lock(&p->Mutex);
			p->UPnPConnected = false;
			ithread_mutex_unlock(&p->Mutex);

			LOG_WARN("[%p]: Auto-renewal failed", p);

			break;
		}
		case UPNP_EVENT_SUBSCRIPTION_EXPIRED: {
			struct Upnp_Event_Subscribe *d_Event = (struct Upnp_Event_Subscribe *)Event;
			struct sMR *p;
			int i;

			p = SID2Device(d_Event->Sid);
			if (!p) break;

			// renew service subscribtion if needed
			for (i = 0; i < NB_SRV; i++) {
				struct sService *s = &p->Service[cSearchedSRV[i].idx];
				if ((s->TimeOut = cSearchedSRV[i].TimeOut) != 0)
					UpnpSubscribe(glControlPointHandle, s->EventURL, &s->TimeOut, s->SID);
			}

			LOG_WARN("[%p]: Subscription manually renewal", p);
			break;
        }
		case UPNP_EVENT_RENEWAL_COMPLETE:
		case UPNP_EVENT_SUBSCRIBE_COMPLETE: {
			LOG_INFO("event: %i [%s] [%p]", EventType, uPNPEvent2String(EventType), Cookie);
			break;
		}
		case UPNP_EVENT_SUBSCRIPTION_REQUEST:
		case UPNP_CONTROL_ACTION_REQUEST:
		case UPNP_EVENT_UNSUBSCRIBE_COMPLETE:
		case UPNP_CONTROL_GET_VAR_REQUEST:
		break;
	}

	Cookie = Cookie;
	return 0;
}


/*----------------------------------------------------------------------------*/
#define TRACK_POLL  (1000)
#define STATE_POLL  (500)
#define VOLUME_POLL (10000)
#define METADATA_POLL (10000)
#define MAX_ACTION_ERRORS (5)
static void *MRThread(void *args)
{
	int elapsed;
	unsigned last;
	struct sMR *p = (struct sMR*) args;

	last = gettime_ms();

	for (; p->Running;  usleep(500000)) {
		elapsed = gettime_ms() - last;
		p->StatePoll += elapsed;
		p->TrackPoll += elapsed;

		ithread_mutex_lock(&p->Mutex);

		/*
		should not request any status update if we are stopped, off or waiting
		for an action to be performed
		*/
		if (!p->on || (p->sqState == SQ_STOP && p->State == STOPPED) ||
			 p->ErrorCount > MAX_ACTION_ERRORS ||
			 p->WaitCookie) {
			ithread_mutex_unlock(&p->Mutex);
			last = gettime_ms();
			continue;
		}

		// get track position & CurrentURI
		if (p->TrackPoll > TRACK_POLL) {
			p->TrackPoll = 0;
			if (p->State != STOPPED && p->State != PAUSED) {
				AVTCallAction(p, "GetPositionInfo", p->seqN++);
			}
		}

		// do polling as event is broken in many uPNP devices

		if (p->StatePoll > STATE_POLL) {
			p->StatePoll = 0;
			AVTCallAction(p, "GetTransportInfo", p->seqN++);
		}

				// do polling as event is broken in many uPNP devices
		ithread_mutex_unlock(&p->Mutex);
		last = gettime_ms();
	}

	return NULL;
}


/*----------------------------------------------------------------------------*/
static bool RefreshTO(char *UDN)
{
	int i;

	for (i = 0; i < MAX_RENDERERS; i++) {
		if (glMRDevices[i].InUse && !strcmp(glMRDevices[i].UDN, UDN)) {
			ithread_mutex_lock(&glMRDevices[i].Mutex);
			glMRDevices[i].UPnPTimeOut = false;
			glMRDevices[i].UPnPMissingCount = glMRDevices[i].Config.UPnPRemoveCount;
			glMRDevices[i].ErrorCount = 0;
			ithread_mutex_unlock(&glMRDevices[i].Mutex);
			return true;
		}
	}
	return false;
}

/*----------------------------------------------------------------------------*/
static void *UpdateMRThread(void *args)
{
	struct sLocList *p, *m;
	struct sMR *Device = NULL;
	int i, TimeStamp;

	LOG_DEBUG("Begin UPnP devices update", NULL);
	TimeStamp = gettime_ms();

	// first add any newly found uPNP renderer
	ithread_mutex_lock(&glMRFoundMutex);
	m = p = glMRFoundList;
	glMRFoundList = NULL;
	ithread_mutex_unlock(&glMRFoundMutex);

	if (!glMainRunning) {
		LOG_DEBUG("Aborting ...", NULL);
		while (p) {
			m = p->Next;
			free(p->Location); free(p);
			p = m;
		}
		return NULL;
	}

	while (p) {
		IXML_Document *DescDoc = NULL;
		char *UDN = NULL, *Manufacturer = NULL;
		int rc;
		void *n = p->Next;

		rc = UpnpDownloadXmlDoc(p->Location, &DescDoc);
		if (rc != UPNP_E_SUCCESS) {
			LOG_DEBUG("Error obtaining description %s -- error = %d\n", p->Location, rc);
			if (DescDoc) ixmlDocument_free(DescDoc);
			p = n;
			continue;
		}

		Manufacturer = XMLGetFirstDocumentItem(DescDoc, "manufacturer");
		UDN = XMLGetFirstDocumentItem(DescDoc, "UDN");
		if (!strstr(Manufacturer, cLogitech) && !RefreshTO(UDN)) {
			// new device so search a free spot.
			for (i = 0; i < MAX_RENDERERS && glMRDevices[i].InUse; i++);

			// no more room !
			if (i == MAX_RENDERERS) {
				LOG_ERROR("Too many uPNP devices", NULL);
				NFREE(UDN); NFREE(Manufacturer);
				break;
			}

			Device = &glMRDevices[i];
			if (AddMRDevice(Device, UDN, DescDoc, p->Location) && !glSaveConfigFile) {
				// create a new slimdevice
				Device->SqueezeHandle = sq_reserve_device(Device, Device->on, &sq_callback);
				if (!*(Device->sq_config.name)) strcpy(Device->sq_config.name, Device->FriendlyName);
				if (!Device->SqueezeHandle || !sq_run_device(Device->SqueezeHandle, &Device->sq_config)) {
					sq_release_device(Device->SqueezeHandle);
					Device->SqueezeHandle = 0;
					LOG_ERROR("[%p]: cannot create squeezelite instance (%s)", Device, Device->FriendlyName);
					DelMRDevice(Device);
				}
			}
		}

		if (DescDoc) ixmlDocument_free(DescDoc);
		NFREE(UDN);	NFREE(Manufacturer);
		p = n;
	}

	// free the list of discovered location URL's
	p = m;
	while (p) {
		m = p->Next;
		free(p->Location); free(p);
		p = m;
	}

	// then walk through the list of devices to remove missing ones
	for (i = 0; i < MAX_RENDERERS; i++) {
		Device = &glMRDevices[i];
		if (!Device->InUse || !Device->Config.UPnPRemoveCount) continue;
		if (Device->UPnPTimeOut && Device->UPnPMissingCount) Device->UPnPMissingCount--;
		if (Device->UPnPConnected || Device->UPnPMissingCount) continue;

		LOG_INFO("[%p]: removing renderer (%s)", Device, Device->FriendlyName);
		if (Device->SqueezeHandle) sq_delete_device(Device->SqueezeHandle);
		DelMRDevice(Device);
	}

	glDiscovery = true;
	if (glAutoSaveConfigFile && !glSaveConfigFile) {
		LOG_DEBUG("Updating configuration %s", glConfigName);
		SaveConfig(glConfigName, glConfigID, false);
	}

	LOG_DEBUG("End UPnP devices update %d", gettime_ms() - TimeStamp);
	return NULL;
}

/*----------------------------------------------------------------------------*/
static void *MainThread(void *args)
{
	unsigned last = gettime_ms();
	int ScanPoll = 0;

	while (glMainRunning) {
		int i, rc;
		int elapsed = gettime_ms() - last;

		// reset timeout and re-scan devices
		ScanPoll += elapsed;
		if (gluPNPScanInterval && ScanPoll > gluPNPScanInterval*1000) {
			ScanPoll = 0;

			for (i = 0; i < MAX_RENDERERS; i++) {
				glMRDevices[i].UPnPTimeOut = true;
				glDiscovery = false;
			}

			// launch a new search for Media Render
			rc = UpnpSearchAsync(glControlPointHandle, gluPNPScanTimeout, MEDIA_RENDERER, NULL);
			if (UPNP_E_SUCCESS != rc) LOG_ERROR("Error sending search update%d", rc);
		}

		if (glLogFile && glLogLimit != - 1) {
			u32_t size = ftell(stderr);

			if (size > glLogLimit*1024*1024) {
				u32_t Sum, BufSize = 16384;
				u8_t *buf = malloc(BufSize);

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

		last = gettime_ms();
		sleep(1);
	}
	return NULL;
}



/*----------------------------------------------------------------------------*/
int uPNPInitialize(void)
{
	int rc;
	struct UpnpVirtualDirCallbacks VirtualDirCallbacks;

	if (gluPNPScanInterval) {
		if (gluPNPScanInterval < SCAN_INTERVAL) gluPNPScanInterval = SCAN_INTERVAL;
		if (gluPNPScanTimeout < SCAN_TIMEOUT) gluPNPScanTimeout = SCAN_TIMEOUT;
		if (gluPNPScanTimeout > gluPNPScanInterval - SCAN_TIMEOUT) gluPNPScanTimeout = gluPNPScanInterval - SCAN_TIMEOUT;
	}

	ithread_mutex_init(&glMRFoundMutex, 0);
	memset(&glMRDevices, 0, sizeof(glMRDevices));

	UpnpSetLogLevel(UPNP_ALL);
	UpnpSetMaxContentLength(60000);

	if (!strstr(glUPnPSocket, "?")) sscanf(glUPnPSocket, "%[^:]:%u", glIPaddress, &glPort);

	if (*glIPaddress) rc = UpnpInit(glIPaddress, glPort);
	else rc = UpnpInit(NULL, glPort);

	if (rc != UPNP_E_SUCCESS) {
		LOG_ERROR("UPnP init failed: %d\n", rc);
		UpnpFinish();
		return false;
	}

	if (!*glIPaddress) strcpy(glIPaddress, UpnpGetServerIpAddress());
	if (!glPort) glPort = UpnpGetServerPort();

	if (rc != UPNP_E_SUCCESS) {
		LOG_ERROR("uPNP init failed: %d\n", rc);
		UpnpFinish();
		return false;
	}

	if (!*glIPaddress) strcpy(glIPaddress, UpnpGetServerIpAddress());
	if (!glPort) glPort = UpnpGetServerPort();

	LOG_INFO("uPNP init success - %s:%u", glIPaddress, glPort);

	rc = UpnpRegisterClient(CallbackEventHandler,
				&glControlPointHandle, &glControlPointHandle);

	if (rc != UPNP_E_SUCCESS) {
		LOG_ERROR("Error registering ControlPoint: %d", rc);
		UpnpFinish();
		return false;
	}
	else {
		LOG_DEBUG("ControlPoint registered", NULL);
	}

	rc = UpnpEnableWebserver(true);

	if (rc != UPNP_E_SUCCESS) {
		LOG_ERROR("Error initalizing WebServer: %d", rc);
		UpnpFinish();
		return false;
	}
	else {
		LOG_DEBUG("WebServer enabled", NULL);
	}

	rc = UpnpAddVirtualDir(glBaseVDIR);

	if (rc != UPNP_E_SUCCESS) {
		LOG_ERROR("Error setting VirtualDir: %d", rc);
		UpnpFinish();
		return false;
	}
	else {
		LOG_DEBUG("VirtualDir set for Squeezelite", NULL);
	}

	VirtualDirCallbacks.get_info = WebGetInfo;
	VirtualDirCallbacks.open = WebOpen;
	VirtualDirCallbacks.read  = WebRead;
	VirtualDirCallbacks.seek = WebSeek;
	VirtualDirCallbacks.close = WebClose;
	VirtualDirCallbacks.write = WebWrite;
	rc = UpnpSetVirtualDirCallbacks(&VirtualDirCallbacks);

	if (rc != UPNP_E_SUCCESS) {
		LOG_ERROR("Error registering VirtualDir callbacks: %d", rc);
		UpnpFinish();
		return false;
	}
	else {
		LOG_DEBUG("Callbacks registered for VirtualDir", NULL);
	}

	/* start the main thread */
	ithread_create(&glMainThread, NULL, &MainThread, NULL);
	return true;
}

/*----------------------------------------------------------------------------*/
int uPNPTerminate(void)
{
	LOG_DEBUG("terminate main thread ...", NULL);
	ithread_join(glMainThread, NULL);
	LOG_DEBUG("un-register libupnp callbacks ...", NULL);
	UpnpUnRegisterClient(glControlPointHandle);
	LOG_DEBUG("disable webserver ...", NULL);
	UpnpRemoveVirtualDir(glBaseVDIR);
	UpnpEnableWebserver(false);
	LOG_DEBUG("end libupnp ...", NULL);
	UpnpFinish();

	return true;
}

/*----------------------------------------------------------------------------*/
int uPNPSearchMediaRenderer(void)
{
	int rc;

	/* search for (Media Render and wait 15s */
	glDiscovery = false;
	rc = UpnpSearchAsync(glControlPointHandle, SCAN_TIMEOUT, MEDIA_RENDERER, NULL);

	if (UPNP_E_SUCCESS != rc) {
		LOG_ERROR("Error sending uPNP search request%d", rc);
		return false;
	}
	return true;
}


/*----------------------------------------------------------------------------*/
static bool AddMRDevice(struct sMR *Device, char *UDN, IXML_Document *DescDoc, const char *location)
{
	char *deviceType = NULL;
	char *friendlyName = NULL;
	char *URLBase = NULL;
	char *presURL = NULL;
	char *ServiceId = NULL;
	char *ServiceType = NULL;
	char *EventURL = NULL;
	char *ControlURL = NULL;
	char *manufacturer = NULL;
	int i;
	pthread_attr_t attr;
	u32_t mac_size = 6;

	// read parameters from default then config file
	memset(Device, 0, sizeof(struct sMR));
	memcpy(&Device->Config, &glMRConfig, sizeof(tMRConfig));
	memcpy(&Device->sq_config, &glDeviceParam, sizeof(sq_dev_param_t));
	LoadMRConfig(glConfigID, UDN, &Device->Config, &Device->sq_config);
	if (!Device->Config.Enabled) return false;

	// Read key elements from description document
	deviceType = XMLGetFirstDocumentItem(DescDoc, "deviceType");
	friendlyName = XMLGetFirstDocumentItem(DescDoc, "friendlyName");
	if (!friendlyName || !*friendlyName) friendlyName = strdup(UDN);
	URLBase = XMLGetFirstDocumentItem(DescDoc, "URLBase");
	presURL = XMLGetFirstDocumentItem(DescDoc, "presentationURL");
	manufacturer = XMLGetFirstDocumentItem(DescDoc, "manufacturer");

	LOG_SDEBUG("UDN:\t%s\nDeviceType:\t%s\nFriendlyName:\t%s", UDN, deviceType, friendlyName);

	if (presURL) {
		char UsedPresURL[200] = "";
		UpnpResolveURL((URLBase ? URLBase : location), presURL, UsedPresURL);
		strcpy(Device->PresURL, UsedPresURL);
	}
	else strcpy(Device->PresURL, "");

	LOG_INFO("[%p]: adding renderer (%s)", Device, friendlyName);

	ithread_mutex_init(&Device->Mutex, 0);
	Device->Magic = MAGIC;
	Device->UPnPTimeOut = false;
	Device->UPnPConnected = true;
	Device->UPnPMissingCount = Device->Config.UPnPRemoveCount;
	Device->Muted = true;	//assume device is muted
	Device->SqueezeHandle = 0;
	Device->ErrorCount = 0;
	Device->Running = true;
	Device->InUse = true;
	Device->sqState = SQ_STOP;
	Device->State = STOPPED;
	Device->WaitCookie = Device->StartCookie = NULL;
	if (Device->Config.RoonMode) {
		Device->on = true;
		Device->sq_config.use_cli = false;
    }
	strcpy(Device->UDN, UDN);
	strcpy(Device->DescDocURL, location);
	strcpy(Device->FriendlyName, friendlyName);
	strcpy(Device->Manufacturer, manufacturer);
	QueueInit(&Device->ActionQueue);

	ExtractIP(location, &Device->ip);
	if (!memcmp(Device->sq_config.mac, "\0\0\0\0\0\0", mac_size)) {
		if (SendARP(*((in_addr_t*) &Device->ip), INADDR_ANY, Device->sq_config.mac, &mac_size)) {
			u32_t hash = hash32(UDN);

			LOG_ERROR("[%p]: cannot get mac %s, creating fake %x", Device, Device->FriendlyName, hash);
			memcpy(Device->sq_config.mac + 2, &hash, 4);
		}
		memset(Device->sq_config.mac, 0xbb, 2);
	}

	/* find the different services */
	for (i = 0; i < NB_SRV; i++) {
		strcpy(Device->Service[i].Id, "");
		if (XMLFindAndParseService(DescDoc, location, cSearchedSRV[i].name, &ServiceType, &ServiceId, &EventURL, &ControlURL)) {
			struct sService *s = &Device->Service[cSearchedSRV[i].idx];
			LOG_SDEBUG("\tservice [%s] %s %s, %s, %s", cSearchedSRV[i].name, ServiceType, ServiceId, EventURL, ControlURL);

			strncpy(s->Id, ServiceId, RESOURCE_LENGTH-1);
			strncpy(s->ControlURL, ControlURL, RESOURCE_LENGTH-1);
			strncpy(s->EventURL, EventURL, RESOURCE_LENGTH - 1);
			strncpy(s->Type, ServiceType, RESOURCE_LENGTH - 1);
			if ((s->TimeOut = cSearchedSRV[i].TimeOut) != 0)
				UpnpSubscribe(glControlPointHandle, s->EventURL, &s->TimeOut, s->SID);
		}
		NFREE(ServiceId);
		NFREE(ServiceType);
		NFREE(EventURL);
		NFREE(ControlURL);
	}

	// send a request for "sink" (will be returned in a callback)
	Device->ProtocolCapReady = false;
	GetProtocolInfo(Device, Device->seqN++);

	NFREE(deviceType);
	NFREE(friendlyName);
	NFREE(URLBase);
	NFREE(presURL);
	NFREE(manufacturer);

	pthread_attr_init(&attr);
	pthread_attr_setstacksize(&attr, PTHREAD_STACK_MIN + 32*1024);
	pthread_create(&Device->Thread, &attr, &MRThread, Device);
	pthread_attr_destroy(&attr);

	/*
	Wait for protocol cap to be filled. This is really ugly but safe as this is
	executed in a different thread than the GetProtocolInfo
	*/
	for (i = 0; i < 50 && !Device->ProtocolCapReady; i++) usleep(10000);
	if (!Device->ProtocolCapReady) {
		LOG_ERROR("[%p]: timeout waiting ProtocolInfo, cannot adjust codecs", Device);
	}
	else {
		LOG_DEBUG("[%p]: waited %d ms for protocolinfo", Device, i*10);
		CheckCodecs(Device);
	}

	return true;
}

/*----------------------------------------------------------------------------*/
static bool Start(void)
{
	if (!uPNPInitialize()) return false;
	uPNPSearchMediaRenderer();
	return true;
}

static bool Stop(void)
{
	struct sLocList *p, *m;

	LOG_DEBUG("flush renderers ...", NULL);
	FlushMRDevices();
	LOG_DEBUG("terminate libupnp ...", NULL);
	uPNPTerminate();

	ithread_mutex_lock(&glMRFoundMutex);
	m = p = glMRFoundList;
	glMRFoundList = NULL;
	ithread_mutex_unlock(&glMRFoundMutex);
	while (p) {
		m = p->Next;
		free(p->Location); free(p);
		p = m;
	}

	return true;
}

/*---------------------------------------------------------------------------*/
static void sighandler(int signum) {
	int i;

	glMainRunning = false;

	if (!glGracefullShutdown) {
		for (i = 0; i < MAX_RENDERERS; i++) {
			struct sMR *p = &glMRDevices[i];
			if (p->InUse && p->sqState == SQ_PLAY) AVTStop(p);
		}
		LOG_INFO("forced exit", NULL);
		exit(EXIT_SUCCESS);
	}

	sq_stop();
	Stop();
	exit(EXIT_SUCCESS);
}


/*---------------------------------------------------------------------------*/
bool ParseArgs(int argc, char **argv) {
	char *optarg = NULL;
	int optind = 1;
	int i;

#define MAXCMDLINE 256
	char cmdline[MAXCMDLINE] = "";

	for (i = 0; i < argc && (strlen(argv[i]) + strlen(cmdline) + 2 < MAXCMDLINE); i++) {
		strcat(cmdline, argv[i]);
		strcat(cmdline, " ");
	}

	while (optind < argc && strlen(argv[optind]) >= 2 && argv[optind][0] == '-') {
		char *opt = argv[optind] + 1;
		if (strstr("stxdfpib", opt) && optind < argc - 1) {
			optarg = argv[optind + 1];
			optind += 2;
		} else if (strstr("tzZIk"
#if RESAMPLE
						  "uR"
#endif
#if DSD
						  "D"
#endif
		  , opt)) {
			optarg = NULL;
			optind += 1;
		}
		else {
			printf("%s", usage);
			return false;
		}

		switch (opt[0]) {
		case 's':
			strcpy(glDeviceParam.server, optarg);
			break;
		case 'b':
			strcpy(glUPnPSocket, optarg);
			break;
#if RESAMPLE
		case 'u':
		case 'R':
			if (optind < argc && argv[optind] && argv[optind][0] != '-') {
				gl_resample = argv[optind++];
			} else {
				gl_resample = "";
			}
			break;
#endif
		case 'f':
			glLogFile = optarg;
			break;
		case 'i':
			glSaveConfigFile = optarg;
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

#if LINUX || FREEBSD
		case 'z':
			glDaemonize = true;
			break;
#endif
#if DSD
		case 'D':
			gl_dop = true;
			if (optind < argc && argv[optind] && argv[optind][0] != '-') {
				gl_dop_delay = atoi(argv[optind++]);
			}
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
					if (!strcmp(l, "all") || !strcmp(l, "stream"))    	stream_loglevel = new;
					if (!strcmp(l, "all") || !strcmp(l, "decode"))    	decode_loglevel = new;
					if (!strcmp(l, "all") || !strcmp(l, "output"))    	output_loglevel = new;
					if (!strcmp(l, "all") || !strcmp(l, "web")) 	  	web_loglevel = new;
					if (!strcmp(l, "all") || !strcmp(l, "main"))     	main_loglevel = new;
					if (!strcmp(l, "all") || !strcmp(l, "util"))    	util_loglevel = new;
					if (!strcmp(l, "all") || !strcmp(l, "upnp"))    	upnp_loglevel = new;
					if (!strcmp(l, "all") || !strcmp(l, "slimmain"))    slimmain_loglevel = new;
				}
				else {
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

	// first try to find a config file on the command line
	for (i = 1; i < argc; i++) {
		if (!strcmp(argv[i], "-x")) {
			strcpy(glConfigName, argv[i+1]);
		}
	}

	// load config from xml file
	glConfigID = (void*) LoadConfig(glConfigName, &glMRConfig, &glDeviceParam);

	// potentially overwrite with some cmdline parameters
	if (!ParseArgs(argc, argv)) exit(1);

	if (glLogFile) {
		if (!freopen(glLogFile, "a", stderr)) {
			fprintf(stderr, "error opening logfile %s: %s\n", glLogFile, strerror(errno));
		}
	}

	LOG_ERROR("Starting squeeze2upnp version: %s", VERSION);

	if (strtod("0.30", NULL) != 0.30) {
		LOG_ERROR("Wrong GLIBC version, use -static build", NULL);
		exit(1);
	}

	if (!glConfigID) {
		LOG_ERROR("\n\n!!!!!!!!!!!!!!!!!! ERROR LOADING CONFIG FILE !!!!!!!!!!!!!!!!!!!!!\n", NULL);
	}

#if LINUX || FREEBSD
	if (glDaemonize && !glSaveConfigFile) {
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
		}
		else {
			LOG_ERROR("Cannot open PID file %s", glPidFile);
		}
	}

	sq_init();

	if (!Start()) {
		LOG_ERROR("Cannot start UPnP", NULL);
		strcpy(resp, "exit");
	}

	if (glSaveConfigFile) {
		while (!glDiscovery) sleep(1);
		SaveConfig(glSaveConfigFile, glConfigID, true);
	}

	while (strcmp(resp, "exit") && !glSaveConfigFile) {

#if LINUX || FREEBSD
		if (!glDaemonize && glInteractive)
			i = scanf("%s", resp);
		else
			pause();
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
		if (!strcmp(resp, "streamdbg"))	{
			char level[20];
			i = scanf("%s", level);
			stream_loglevel = debug2level(level);
		}

		if (!strcmp(resp, "outputdbg"))	{
			char level[20];
			i = scanf("%s", level);
			output_loglevel = debug2level(level);
		}

		if (!strcmp(resp, "slimprotodbg"))	{
			char level[20];
			i = scanf("%s", level);
			slimproto_loglevel = debug2level(level);
		}

		if (!strcmp(resp, "webdbg"))	{
			char level[20];
			i = scanf("%s", level);
			web_loglevel = debug2level(level);
		}

		if (!strcmp(resp, "maindbg"))	{
			char level[20];
			i = scanf("%s", level);
			main_loglevel = debug2level(level);
		}

		if (!strcmp(resp, "slimmainqdbg"))	{
			char level[20];
			i = scanf("%s", level);
			slimmain_loglevel = debug2level(level);
		}

		if (!strcmp(resp, "utildbg"))	{
			char level[20];
			i = scanf("%s", level);
			util_loglevel = debug2level(level);
		}

		if (!strcmp(resp, "upnpdbg"))	{
			char level[20];
			i = scanf("%s", level);
			upnp_loglevel = debug2level(level);
		}


		 if (!strcmp(resp, "save"))	{
			char name[128];
			i = scanf("%s", name);
			SaveConfig(name, glConfigID, true);
		}
	}

	if (glConfigID) ixmlDocument_free(glConfigID);
	glMainRunning = false;
	LOG_INFO("stopping squeelite devices ...", NULL);
	sq_stop();
	LOG_INFO("stopping UPnP devices ...", NULL);
	Stop();
	LOG_INFO("all done", NULL);

	return true;
}




