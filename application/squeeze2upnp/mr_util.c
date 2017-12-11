/*
 *  Squeeze2upnp - LMS to uPNP gateway
 *
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

#include <stdarg.h>

#include "squeezedefs.h"
#include "squeeze2upnp.h"
#include "avt_util.h"
#include "mr_util.h"
#include "util.h"
#include "util_common.h"
#include "upnptools.h"
#include "log_util.h"

extern log_level util_loglevel;
static log_level *loglevel = &util_loglevel;

 /* DLNA.ORG_CI: conversion indicator parameter (integer)
 *     0 not transcoded
 *     1 transcoded
 */
typedef enum {
  DLNA_ORG_CONVERSION_NONE = 0,
  DLNA_ORG_CONVERSION_TRANSCODED = 1,
} dlna_org_conversion_t;

/* DLNA.ORG_OP: operations parameter (string)
 *     "00" (or "0") neither time seek range nor range supported
 *     "01" range supported
 *     "10" time seek range supported
 *     "11" both time seek range and range supported
 */
typedef enum {
  DLNA_ORG_OPERATION_NONE                  = 0x00,
  DLNA_ORG_OPERATION_RANGE                 = 0x01,
  DLNA_ORG_OPERATION_TIMESEEK              = 0x10,
} dlna_org_operation_t;

/* DLNA.ORG_FLAGS, padded with 24 trailing 0s
 *     8000 0000  31  senderPaced
 *     4000 0000  30  lsopTimeBasedSeekSupported
 *     2000 0000  29  lsopByteBasedSeekSupported
 *     1000 0000  28  playcontainerSupported
 *      800 0000  27  s0IncreasingSupported
 *      400 0000  26  sNIncreasingSupported
 *      200 0000  25  rtspPauseSupported
 *      100 0000  24  streamingTransferModeSupported
 *       80 0000  23  interactiveTransferModeSupported
 *       40 0000  22  backgroundTransferModeSupported
 *       20 0000  21  connectionStallingSupported
 *       10 0000  20  dlnaVersion15Supported
 *
 *     Example: (1 << 24) | (1 << 22) | (1 << 21) | (1 << 20)
 *       DLNA.ORG_FLAGS=0170 0000[0000 0000 0000 0000 0000 0000] // [] show padding
 */
typedef enum {
  DLNA_ORG_FLAG_SENDER_PACED               = (1 << 31),
  DLNA_ORG_FLAG_TIME_BASED_SEEK            = (1 << 30),
  DLNA_ORG_FLAG_BYTE_BASED_SEEK            = (1 << 29),
  DLNA_ORG_FLAG_PLAY_CONTAINER             = (1 << 28),

  DLNA_ORG_FLAG_S0_INCREASE                = (1 << 27),
  DLNA_ORG_FLAG_SN_INCREASE                = (1 << 26),
  DLNA_ORG_FLAG_RTSP_PAUSE                 = (1 << 25),
  DLNA_ORG_FLAG_STREAMING_TRANSFERT_MODE    = (1 << 24),

  DLNA_ORG_FLAG_INTERACTIVE_TRANSFERT_MODE = (1 << 23),
  DLNA_ORG_FLAG_BACKGROUND_TRANSFERT_MODE  = (1 << 22),
  DLNA_ORG_FLAG_CONNECTION_STALL           = (1 << 21),
  DLNA_ORG_FLAG_DLNA_V15                   = (1 << 20),
} dlna_org_flags_t;


#define DLNA_ORG_OP (DLNA_ORG_OPERATION_RANGE)
// GNU pre-processor seems to be confused if this is multiline ...
#define DLNA_ORG_FLAG ( DLNA_ORG_FLAG_STREAMING_TRANSFERT_MODE | DLNA_ORG_FLAG_BACKGROUND_TRANSFERT_MODE | DLNA_ORG_FLAG_CONNECTION_STALL |	DLNA_ORG_FLAG_DLNA_V15 )

/*
static char DLNA_OPT[] = ";DLNA.ORG_OP=01;DLNA.ORG_FLAGS=01700000000000000000000000000000";
*/


/*---------------------------------------------------------------------------*/
static char *format2ext(u8_t format)
{
	switch(format) {
		case 'p': return "pcm";
		case 'm': return "mp3";
		case 'f': return "flac";
		case 'w': return "wma";
		case 'o': return "ogg";
		case 'a':
		case 'l': return "m4a";
		default: return "xxx";
	}
}

/*---------------------------------------------------------------------------*/
u8_t ext2format(char *ext)
{
	if (!ext) return ' ';

	if (strstr(ext, "wav")) return 'w';
	if (strstr(ext, "flac") || strstr(ext, "flc")) return 'f';
	if (strstr(ext, "mp3")) return 'm';
	if (strstr(ext, "wma")) return 'a';
	if (strstr(ext, "ogg")) return 'o';
	if (strstr(ext, "m4a")) return '4';
	if (strstr(ext, "mp4")) return '4';
	if (strstr(ext, "aif")) return 'i';

	return ' ';
}


/*----------------------------------------------------------------------------*/
bool _SetContentType(char *Cap[], sq_seturi_t *uri, int n, ...)
{
	int i;
	char *fmt, **p = NULL;
	char channels[SQ_STR_LENGTH], rate[SQ_STR_LENGTH];
	va_list args;

	va_start(args, n);

	for (i = 0; i < n; i++) {
		fmt = va_arg(args, char*);

		// see if we have the complicated case of PCM
		if (strstr(fmt, "audio/L")) {
			sprintf(channels, "channels=%d", uri->channels);
			sprintf(rate, "rate=%d", uri->sample_rate);
		}

		// find the corresponding line in the device's protocol info
		for (p = Cap; *p; p++) {
			if (!strstr(*p, fmt)) continue;
			// if not PCM, just copy the found format
			if (!strstr(*p, "audio/L")) {
				strcpy(uri->proto_info, *p);
				// force audio/aac for m4a - alac is still unknown ...
				if (uri->codec == 'a') strcpy(uri->content_type, "audio/aac");
				else strcpy(uri->content_type, fmt);
				// special case of wave and aiff, need to change file extension
				if (strstr(*p, "wav")) strcpy(uri->ext, "wav");
				if (strstr(*p, "aiff")) strcpy(uri->ext, "aif");
				break;
			}
			// re-set file extension
			strcpy(uri->ext, "pcm");
			// if the proposed format accepts any rate & channel, give it a try
			if (!strstr(*p, "channels") && !strstr(*p, "rate")) {
				int size = strstr(*p, fmt) - *p;

				sprintf(uri->content_type, "%s;channels=%d;rate=%d", fmt, uri->channels, uri->sample_rate);
				strncpy(uri->proto_info, *p, size);
				*(uri->proto_info + size) = '\0';
				strcat(uri->proto_info, uri->content_type);
				if (*(*p + size + strlen(fmt))) strcat(uri->proto_info, *p + size + strlen(fmt));
				break;
			}
			// if PCM, try to find an exact match
			if (strstr(*p, channels) && strstr(*p, rate)) {
				sprintf(uri->content_type, "%s;channels=%d;rate=%d", fmt, uri->channels, uri->sample_rate);
				strcpy(uri->proto_info, *p);
				break;
			}
		}
		if (*p) break;
	}

	va_end(args);

	if (!*p) {
		strcpy(uri->proto_info, "audio/unknown");
		strcpy(uri->content_type, "audio/unknown");
		return false;
	}

	return true;
 }


 /*----------------------------------------------------------------------------*/
static bool SetContentTypeRawAudio(struct sMR *Device, sq_seturi_t *uri, bool MatchEndianness)
{
	bool ret = false;
	char *p, *buf;

	p = buf = strdup(Device->Config.RawAudioFormat);

	while (!ret && p && *p) {
		u8_t order = 0xff;
		char *q = strchr(p, ',');

		if (q) *q = '\0';

		if (strstr(p, "pcm") || strstr(p,"raw")) {
			char fmt[SQ_STR_LENGTH];

			sprintf(fmt, "audio/L%d", (Device->sq_config.L24_format == L24_TRUNC_16 && uri->sample_size == 24) ? 16 : uri->sample_size);
			ret = _SetContentType(Device->ProtocolCap, uri, 1, fmt);
			if (!ret && Device->sq_config.L24_format == L24_TRUNC_16_PCM && uri->sample_size == 24) {
				ret = _SetContentType(Device->ProtocolCap, uri, 1, "audio/L16");
			}
			order = 0;
		}
		if (strstr(p, "wav")) { ret = _SetContentType(Device->ProtocolCap, uri, 3, "audio/wav", "audio/x-wav", "audio/wave"); order = 1;}
		if (strstr(p, "aif")) { ret = _SetContentType(Device->ProtocolCap, uri, 2, "audio/aiff", "audio/x-aiff"); order = 0;}

		if (MatchEndianness && (order != uri->endianness)) ret = false;

		p = (q) ? q + 1 : NULL;
	}

	if (!ret && MatchEndianness) ret = SetContentTypeRawAudio(Device, uri, false);

	NFREE(buf);
	return ret;
}


/*----------------------------------------------------------------------------*/
bool SetContentType(struct sMR *Device, sq_seturi_t *uri)
{
	char buf[SQ_STR_LENGTH];
	char *DLNAOrg;
	bool rc;

	strcpy(uri->ext, format2ext(uri->codec));

	switch (uri->codec) {
	case 'm': rc = _SetContentType(Device->ProtocolCap, uri, 3, "audio/mp3", "audio/mpeg", "audio/mpeg3"); break;
	case 'f': rc = _SetContentType(Device->ProtocolCap, uri, 2, "audio/x-flac", "audio/flac");break;
	case 'w': rc = _SetContentType(Device->ProtocolCap, uri, 2, "audio/x-wma", "audio/wma");break;
	case 'o': rc = _SetContentType(Device->ProtocolCap, uri, 1, "audio/ogg");break;
	case 'a': rc = _SetContentType(Device->ProtocolCap, uri, 4, "audio/x-aac", "audio/aac", "audio/m4a", "audio/mp4");break;
	case 'l': rc = _SetContentType(Device->ProtocolCap, uri, 1, "audio/m4a");break;
	case 'p': rc = SetContentTypeRawAudio(Device, uri, Device->Config.MatchEndianness);break;
	default:
		strcpy(uri->content_type, "unknown");
		strcpy(uri->proto_info, "");
		return false;
	}

	if (Device->Config.ByteSeek) DLNAOrg = ";DLNA.ORG_OP=01;DLNA.ORG_CI=0";
	else DLNAOrg = ";DLNA.ORG_CI=0";

	sprintf(buf, "%s;DLNA.ORG_FLAGS=%08x000000000000000000000000",
				  DLNAOrg, DLNA_ORG_FLAG | ((uri->duration) ? 0 : DLNA_ORG_FLAG_SN_INCREASE));


	if (uri->proto_info[strlen(uri->proto_info) - 1] == ':') strcat(uri->proto_info, buf + 1);
	else strcat(uri->proto_info, buf);

	return rc;
}


/*----------------------------------------------------------------------------*/
bool isMaster(char *UDN, struct sService *Service)
{
	IXML_Document *ActionNode = NULL, *Response;
	char *Body;
	bool Master = false;

	if (!*Service->ControlURL) return true;

	ActionNode = UpnpMakeAction("GetZoneGroupState", Service->Type, 0, NULL);
	UpnpSendAction(glControlPointHandle, Service->ControlURL, Service->Type,
								 NULL, ActionNode, &Response);

	Body = XMLGetFirstDocumentItem(Response, "ZoneGroupState");
	if (Response) ixmlDocument_free(Response);

	Response = ixmlParseBuffer(Body);
	NFREE(Body);

	/* if member but not coordinator, eliminate */
	if (Response) {
		char myUUID[RESOURCE_LENGTH] = "";
		IXML_NodeList *GroupList = ixmlDocument_getElementsByTagName(Response, "ZoneGroup");
		int i;

		sscanf(UDN, "uuid:%s", myUUID);

		// get the UUID and see if it's member of a zone and not the coordinator
		for (i = 0; GroupList && i < (int) ixmlNodeList_length(GroupList); i++) {
			IXML_Node *Group = ixmlNodeList_item(GroupList, i);
			const char *Coordinator = ixmlElement_getAttribute((IXML_Element*) Group, "Coordinator");

			if (!strcasecmp(myUUID, Coordinator)) Master = true;
		}

		ixmlNodeList_free(GroupList);
		ixmlDocument_free(Response);
	}

	return Master;
}


/*----------------------------------------------------------------------------*/
void FlushMRDevices(void)
{
	int i;

	for (i = 0; i < MAX_RENDERERS; i++) {
		struct sMR *p = &glMRDevices[i];
		if (p->InUse) {
			// critical to stop the device otherwise libupnp mean wait forever
			if (p->sqState == SQ_PLAY || p->sqState == SQ_PAUSE) AVTStop(p);
			DelMRDevice(p);
		}
	}
}


/*----------------------------------------------------------------------------*/
void DelMRDevice(struct sMR *p)
{
	int i = 0;

	ithread_mutex_lock(&p->Mutex);
	p->Running = false;
	ithread_mutex_unlock(&p->Mutex);
	ithread_join(p->Thread, NULL);

	ithread_mutex_lock(&p->Mutex);
	p->InUse = false;

	AVTActionFlush(&p->ActionQueue);
	sq_free_metadata(&p->MetaData);
	NFREE(p->CurrentURI);
	NFREE(p->NextURI);
	while (p->ProtocolCap[i] && i < MAX_PROTO) {
		NFREE(p->ProtocolCap[i]);
		i++;
	}
	ithread_mutex_unlock(&p->Mutex);
	ithread_mutex_destroy(&p->Mutex);
	memset(p, 0, sizeof(struct sMR));
}


/*----------------------------------------------------------------------------*/
struct sMR* CURL2Device(char *CtrlURL)
{
	int i, j;

	for (i = 0; i < MAX_RENDERERS; i++) {
		if (!glMRDevices[i].InUse) continue;
		for (j = 0; j < NB_SRV; j++) {
			if (!strcmp(glMRDevices[i].Service[j].ControlURL, CtrlURL)) {
				return &glMRDevices[i];
			}
		}
	}

	return NULL;
}


/*----------------------------------------------------------------------------*/
struct sMR* SID2Device(char *SID)
{
	int i, j;

	for (i = 0; i < MAX_RENDERERS; i++) {
		if (!glMRDevices[i].InUse) continue;
		for (j = 0; j < NB_SRV; j++) {
			if (!strcmp(glMRDevices[i].Service[j].SID, SID)) {
				return &glMRDevices[i];
			}
		}
	}

	return NULL;
}

/*----------------------------------------------------------------------------*/
struct sMR* UDN2Device(char *UDN)
{
	int i;

	for (i = 0; i < MAX_RENDERERS; i++) {
		if (!glMRDevices[i].InUse) continue;
		if (!strcmp(glMRDevices[i].UDN, UDN)) {
			return &glMRDevices[i];
		}
	}

	return NULL;
}


/*----------------------------------------------------------------------------*/
int Codec2Length(char CodecShort, char *Rule)
{
	char *buf = strdup(Rule);
	char *p = buf;
	char *end = p + strlen(p);
	char *Codec = format2ext(CodecShort);
	int mode = HTTP_DEFAULT_MODE;

	mode = atol(p);

	// strtok is not re-entrant
	while (p < end && (p = strtok(p, ",")) != NULL) {
		if (stristr(p, Codec)) {
			p = strchr(p, ':') + 1;
			mode = atol(p);
			break;
		}
		p += strlen(p) + 1;
	}

	free(buf);

	if (!mode || (mode > 0 && mode < 100000000L)) mode = HTTP_DEFAULT_MODE;

	return mode;
}

/*----------------------------------------------------------------------------*/
void ParseProtocolInfo(struct sMR *Device, char *Info)
{
	char *p = Info;
	int n = 0, i = 0;
	int size = strlen(Info);
	bool flac = false;

	// strtok is no re-entrant
	memset(Device->ProtocolCap, 0, sizeof(char*) * (MAX_PROTO + 1));
	do {
		p = strtok(p, ",");
		n += strlen(p) + 1;
		if (strstr(p, "http-get") && strstr(p, "audio")) {
			Device->ProtocolCap[i] = malloc(strlen(p) + 1);
			strcpy(Device->ProtocolCap[i], p);
			if (strstr(p, "flac")) flac = true;
			i++;
		}
		p += strlen(p) + 1;
	} while (i < MAX_PROTO && n < size);

	// remove trailing "*" as we WILL add DLNA-related info, so some options to come
	for (i = 0; (p = Device->ProtocolCap[i]) != NULL; i++)
		if (p[strlen(p) - 1] == '*') p[strlen(p) - 1] = '\0';

	if (Device->Config.AllowFlac && flac == false && i < MAX_PROTO)
		Device->ProtocolCap[i] = strdup("http-get:*:audio/flac:");

	Device->ProtocolCapReady = true;
}

/*----------------------------------------------------------------------------*/
static void _CheckCodecs(struct sMR *Device, char *codec, int n, ...)
{
	int i;
	va_list args;

	va_start(args, n);

	for (i = 0; i < n; i++) {
		char **p, *lookup = va_arg(args, char*);

		p = Device->ProtocolCap;
		// there is always a last "ProtocolCap" that is NULL
		while (*p) {
			if (strstr(*p, lookup)) {
				if (strlen(Device->sq_config.codecs)) {
					strcat(Device->sq_config.codecs, ",");
					strcat(Device->sq_config.codecs, codec);
				}
				else strcpy(Device->sq_config.codecs, codec);
				return;
			}
			p++;
		}
	}

	va_end(args);
}

/*----------------------------------------------------------------------------*/
void CheckCodecs(struct sMR *Device)
{
	char *p, *buf;

	p = buf = strdup(Device->sq_config.codecs);
	*Device->sq_config.codecs = '\0';

	while (p && *p) {
		char *q = strchr(p, ',');
		if (q) *q = '\0';

		if (strstr(p,"mp3")) _CheckCodecs(Device, "mp3", 2, "mp3", "mpeg");
		if (strstr(p,"flc")) _CheckCodecs(Device, "flc", 1, "flac");
		if (strstr(p,"wma")) _CheckCodecs(Device, "wma", 1, "wma");
		if (strstr(p,"ogg")) _CheckCodecs(Device, "ogg", 1, "ogg");
		if (strstr(p,"aac")) _CheckCodecs(Device, "aac", 3, "aac", "m4a", "mp4");
		if (strstr(p,"alc")) _CheckCodecs(Device, "alc", 1, "m4a");
		if (strstr(p,"pcm")) _CheckCodecs(Device, "pcm", 2, "wav", "audio/L");
		if (strstr(p,"aif")) _CheckCodecs(Device, "aif", 3, "aif", "wav", "audio/L");

		p = (q) ? q + 1 : NULL;
	}

	NFREE(buf);
}


 /*----------------------------------------------------------------------------*/
void MakeMacUnique(struct sMR *Device)
{
	int i;

	for (i = 0; i < MAX_RENDERERS; i++) {
		if (!glMRDevices[i].InUse || Device == &glMRDevices[i]) continue;
		if (!memcmp(&glMRDevices[i].sq_config.mac, &Device->sq_config.mac, 6)) {
			u32_t hash = hash32(Device->UDN);

			LOG_INFO("[%p]: duplicated mac ... updating", Device);
			memset(&Device->sq_config.mac[0], 0xcc, 2);
			memcpy(&Device->sq_config.mac[0] + 2, &hash, 4);
		}
	}
}


 /*
"http-get:*:audio/mpeg:DLNA.ORG_PN=MP3;DLNA.ORG_OP=01;DLNA.ORG_FLAGS=$flags",
"http-get:*:audio/L16;rate=8000;channels=1:DLNA.ORG_PN=LPCM;DLNA.ORG_OP=01,DLNA.ORG_FLAGS=$flags",
"http-get:*:audio/L16;rate=8000;channels=2:DLNA.ORG_PN=LPCM;DLNA.ORG_OP=01,DLNA.ORG_FLAGS=$flags",
"http-get:*:audio/L16;rate=11025;channels=1:DLNA.ORG_PN=LPCM;DLNA.ORG_OP=01,DLNA.ORG_FLAGS=$flags",
"http-get:*:audio/L16;rate=11025;channels=2:DLNA.ORG_PN=LPCM;DLNA.ORG_OP=01,DLNA.ORG_FLAGS=$flags",
"http-get:*:audio/L16;rate=12000;channels=1:DLNA.ORG_PN=LPCM;DLNA.ORG_OP=01,DLNA.ORG_FLAGS=$flags",
"http-get:*:audio/L16;rate=12000;channels=2:DLNA.ORG_PN=LPCM;DLNA.ORG_OP=01,DLNA.ORG_FLAGS=$flags",
"http-get:*:audio/L16;rate=16000;channels=1:DLNA.ORG_PN=LPCM;DLNA.ORG_OP=01,DLNA.ORG_FLAGS=$flags",
"http-get:*:audio/L16;rate=16000;channels=2:DLNA.ORG_PN=LPCM;DLNA.ORG_OP=01,DLNA.ORG_FLAGS=$flags",
"http-get:*:audio/L16;rate=22050;channels=1:DLNA.ORG_PN=LPCM;DLNA.ORG_OP=01,DLNA.ORG_FLAGS=$flags",
"http-get:*:audio/L16;rate=22050;channels=2:DLNA.ORG_PN=LPCM;DLNA.ORG_OP=01,DLNA.ORG_FLAGS=$flags",
"http-get:*:audio/L16;rate=24000;channels=1:DLNA.ORG_PN=LPCM;DLNA.ORG_OP=01,DLNA.ORG_FLAGS=$flags",
"http-get:*:audio/L16;rate=24000;channels=2:DLNA.ORG_PN=LPCM;DLNA.ORG_OP=01,DLNA.ORG_FLAGS=$flags",
"http-get:*:audio/L16;rate=32000;channels=1:DLNA.ORG_PN=LPCM;DLNA.ORG_OP=01,DLNA.ORG_FLAGS=$flags",
"http-get:*:audio/L16;rate=32000;channels=2:DLNA.ORG_PN=LPCM;DLNA.ORG_OP=01,DLNA.ORG_FLAGS=$flags",
"http-get:*:audio/L16;rate=44100;channels=1:DLNA.ORG_PN=LPCM;DLNA.ORG_OP=01,DLNA.ORG_FLAGS=$flags",
"http-get:*:audio/L16;rate=44100;channels=2:DLNA.ORG_PN=LPCM;DLNA.ORG_OP=01,DLNA.ORG_FLAGS=$flags",
"http-get:*:audio/L16;rate=48000;channels=1:DLNA.ORG_PN=LPCM;DLNA.ORG_OP=01,DLNA.ORG_FLAGS=$flags",
"http-get:*:audio/L16;rate=48000;channels=2:DLNA.ORG_PN=LPCM;DLNA.ORG_OP=01,DLNA.ORG_FLAGS=$flags",
"http-get:*:audio/vnd.dlna.adts:DLNA.ORG_PN=AAC_ADTS;DLNA.ORG_OP=01;DLNA.ORG_FLAGS=$flags",
"http-get:*:audio/vnd.dlna.adts:DLNA.ORG_PN=HEAAC_L2_ADTS;DLNA.ORG_OP=01;DLNA.ORG_FLAGS=$flags",
"http-get:*:audio/mp4:DLNA.ORG_PN=AAC_ISO;DLNA.ORG_OP=00;DLNA.ORG_FLAGS=$flags",
"http-get:*:audio/mp4:DLNA.ORG_PN=AAC_ISO_320;DLNA.ORG_OP=00;DLNA.ORG_FLAGS=$flags",
"http-get:*:audio/mp4:DLNA.ORG_PN=HEAAC_L2_ISO;DLNA.ORG_OP=00;DLNA.ORG_FLAGS=$flags",
"http-get:*:audio/x-ms-wma:DLNA.ORG_PN=WMABASE;DLNA.ORG_OP=01;DLNA.ORG_FLAGS=$flags",
"http-get:*:audio/x-ms-wma:DLNA.ORG_PN=WMAFULL;DLNA.ORG_OP=01;DLNA.ORG_FLAGS=$flags",
"http-get:*:audio/x-ms-wma:DLNA.ORG_PN=WMAPRO;DLNA.ORG_OP=01;DLNA.ORG_FLAGS=$flags",
"http-get:*:application/ogg:DLNA.ORG_OP=00;DLNA.ORG_FLAGS=$flags",
"http-get:*:audio/x-flac:DLNA.ORG_OP=00;DLNA.ORG_FLAGS=$flags",
*/

