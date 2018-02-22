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
#define DLNA_ORG_FLAG ( DLNA_ORG_FLAG_S0_INCREASE | DLNA_ORG_FLAG_STREAMING_TRANSFERT_MODE | DLNA_ORG_FLAG_BACKGROUND_TRANSFERT_MODE | DLNA_ORG_FLAG_CONNECTION_STALL | DLNA_ORG_FLAG_DLNA_V15 )
/*
static char DLNA_OPT[] = ";DLNA.ORG_OP=01;DLNA.ORG_FLAGS=01700000000000000000000000000000";
*/

int 	_voidHandler(Upnp_EventType EventType, void *_Event, void *Cookie) { return 0; }


/*----------------------------------------------------------------------------*/
char *MakeProtocolInfo(char *MimeType, u32_t duration)
{
	char *buf;
	char *DLNAOrgPN;

	switch (mimetype2format(MimeType)) {
	case 'm':
		DLNAOrgPN = "DLNA.ORG_PN=MP3;";
		break;
	case 'p':
		DLNAOrgPN = "DLNA.ORG_PN=LPCM;";
		break;
	default:
		DLNAOrgPN = "";
	}

	asprintf(&buf, "http-get:*:%s:%sDLNA.ORG_CI=0;DLNA.ORG_FLAGS=%08x000000000000000000000000",
				  MimeType, DLNAOrgPN, DLNA_ORG_FLAG | (duration ? 0 : DLNA_ORG_FLAG_SN_INCREASE));
	return buf;
}


/*----------------------------------------------------------------------------*/
bool isMaster(char *UDN, struct sService *Service, char **Name)
{
	IXML_Document *ActionNode = NULL, *Response;
	char *Body;
	bool Master = false;

	if (!*Service->ControlURL) return true;

	ActionNode = UpnpMakeAction("GetZoneGroupState", Service->Type, 0, NULL);
	UpnpSendAction(glControlPointHandle, Service->ControlURL, Service->Type,
								 NULL, ActionNode, &Response);

	if (ActionNode) ixmlDocument_free(ActionNode);

	Body = XMLGetFirstDocumentItem(Response, "ZoneGroupState");
	if (Response) ixmlDocument_free(Response);

	Response = ixmlParseBuffer(Body);
	NFREE(Body);

	if (Response) {
		char myUUID[RESOURCE_LENGTH] = "";
		IXML_NodeList *GroupList = ixmlDocument_getElementsByTagName(Response, "ZoneGroup");
		int i;

		sscanf(UDN, "uuid:%s", myUUID);

		// list all ZoneGroups
		for (i = 0; GroupList && i < (int) ixmlNodeList_length(GroupList); i++) {
			IXML_Node *Group = ixmlNodeList_item(GroupList, i);
			const char *Coordinator = ixmlElement_getAttribute((IXML_Element*) Group, "Coordinator");

			// are we the coordinator of that Zone
			if (!strcasecmp(myUUID, Coordinator)) {
				IXML_NodeList *MemberList = ixmlDocument_getElementsByTagName((IXML_Document*) Group, "ZoneGroupMember");
				int j;

				// list all ZoneMembers to find ZoneName
				for (j = 0; Name && j < (int) ixmlNodeList_length(MemberList); j++) {
					IXML_Node *Member = ixmlNodeList_item(MemberList, j);
					const char *UUID = ixmlElement_getAttribute((IXML_Element*) Member, "UUID");

					if (!strcasecmp(myUUID, UUID)) {
						NFREE(*Name);
						*Name = strdup(ixmlElement_getAttribute((IXML_Element*) Member, "ZoneName"));
					}
				}

				Master = true;
				ixmlNodeList_free(MemberList);
			}
		}

		ixmlNodeList_free(GroupList);
		ixmlDocument_free(Response);
	} else Master = true;

	return Master;
}


/*----------------------------------------------------------------------------*/
void FlushMRDevices(void)
{
	int i;

	for (i = 0; i < MAX_RENDERERS; i++) {
		struct sMR *p = &glMRDevices[i];
		pthread_mutex_lock(&p->Mutex);
		if (p->Running) {
			// critical to stop the device otherwise libupnp mean wait forever
			if (p->sqState == SQ_PLAY || p->sqState == SQ_PAUSE) AVTStop(p);
			DelMRDevice(p);
		} else pthread_mutex_unlock(&p->Mutex);
	}
}


/*----------------------------------------------------------------------------*/
void DelMRDevice(struct sMR *p)
{
	int i;

	// already locked expect for failed creation which means a trylock is fine
	pthread_mutex_trylock(&p->Mutex);

	for (i = 0; i < NB_SRV; i++) {
		if (p->Service[i].TimeOut) {
			UpnpUnSubscribeAsync(glControlPointHandle, p->Service[i].SID, _voidHandler, NULL);
		}
	}

	p->Running = false;
	sq_free_metadata(&p->NextMetaData);

	pthread_mutex_unlock(&p->Mutex);
	pthread_join(p->Thread, NULL);

	AVTActionFlush(&p->ActionQueue);

	NFREE(p->NextProtoInfo);
	NFREE(p->NextURI);
}


/*----------------------------------------------------------------------------*/
struct sMR* CURL2Device(char *CtrlURL)
{
	int i, j;

	for (i = 0; i < MAX_RENDERERS; i++) {
		if (!glMRDevices[i].Running) continue;
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
		if (!glMRDevices[i].Running) continue;
		for (j = 0; j < NB_SRV; j++) {
			if (!strcmp(glMRDevices[i].Service[j].SID, SID)) {
				return &glMRDevices[i];
			}
		}
	}

	return NULL;
}


/*----------------------------------------------------------------------------*/
struct sService *EventURL2Service(char *URL, struct sService *s)
{
	int i;

	for (i = 0; i < NB_SRV; s++, i++) {
		if (strcmp(s->EventURL, URL)) continue;
		return s;
	}

	return NULL;
}


/*----------------------------------------------------------------------------*/
struct sMR* UDN2Device(char *UDN)
{
	int i;

	for (i = 0; i < MAX_RENDERERS; i++) {
		if (!glMRDevices[i].Running) continue;
		if (!strcmp(glMRDevices[i].UDN, UDN)) {
			return &glMRDevices[i];
		}
	}

	return NULL;
}


/*----------------------------------------------------------------------------*/
void BusyRaise(struct sMR *Device)
{
	LOG_DEBUG("[%p]: busy raise %u", Device, Device->Busy);
	Device->Busy++;
	pthread_mutex_unlock(&Device->Mutex);
}


/*----------------------------------------------------------------------------*/
bool CheckAndLock(struct sMR *Device)
{
	bool Checked = false;

	if (!Device) {
		LOG_INFO("device is NULL", NULL);
		return false;
	}

	pthread_mutex_lock(&Device->Mutex);
	if (Device->Running) Checked = true;
	else { LOG_INFO("[%p]: device has been removed", Device); }

	pthread_mutex_unlock(&Device->Mutex);

	return Checked;
}


/*----------------------------------------------------------------------------*/
void BusyDrop(struct sMR *Device)
{
	pthread_mutex_lock(&Device->Mutex);
	Device->Busy--;
	if (!Device->Busy && Device->Delete) pthread_cond_signal(&Device->Cond);
	LOG_DEBUG("[%p]: busy drop %u", Device, Device->Busy);
	pthread_mutex_unlock(&Device->Mutex);
}


/*----------------------------------------------------------------------------*/
char** ParseProtocolInfo(char *Info, char *Forced)
{
	char *p = Info, **MimeTypes = calloc(MAX_MIMETYPES + 1, sizeof(char*));
	int n = 0, i = 0;
	int size = strlen(Info);
	char MimeType[_STR_LEN_];

	// strtok is not re-entrant
	do {
		p = strtok(p, ",");
		n += strlen(p) + 1;
		if (sscanf(p, "http-get:*:%[^:]", MimeType) && strstr(MimeType, "audio")) {
			MimeTypes[i] = strdup(MimeType);
			i++;
		}
		p += strlen(p) + 1;
	} while (i < MAX_MIMETYPES && n < size);

	p = Forced;
	size = strlen(Forced);
	while (i < MAX_MIMETYPES && *p) {
		strtok(p, ",");
		MimeTypes[i] = strdup(p);
		p += strlen(p);
		if (*p) p++;
		i++;
	}

	return MimeTypes;
}


/*----------------------------------------------------------------------------*/
static void _CheckCodecs(char *Codecs, char *Sink, char *Codec, int n, ...)
{
	int i;
	va_list args;

	va_start(args, n);

	for (i = 0; i < n; i++) {
		char *lookup = va_arg(args, char*);

		if (strstr(Sink, lookup)) {
			if (strlen(Codecs)) {
				strcat(Codecs, ",");
				strcat(Codecs, Codec);
			}
			else strcpy(Codecs, Codec);
			return;
		}
	}

	va_end(args);
}

/*----------------------------------------------------------------------------*/
void CheckCodecs(char *Codecs, char *Sink)
{
	char *p, *buf;

	p = buf = strdup(Codecs);
	*Codecs = '\0';

	while (p && *p) {
		char *q = strchr(p, ',');
		if (q) *q = '\0';

		if (strstr(p,"mp3")) _CheckCodecs(Codecs, Sink, "mp3", 2, "mp3", "mpeg");
		if (strstr(p,"flc")) _CheckCodecs(Codecs, Sink, "flc", 1, "flac");
		if (strstr(p,"wma")) _CheckCodecs(Codecs, Sink, "wma", 1, "wma");
		if (strstr(p,"ogg")) _CheckCodecs(Codecs, Sink, "ogg", 1, "ogg");
		if (strstr(p,"aac")) _CheckCodecs(Codecs, Sink, "aac", 3, "aac", "m4a", "mp4");
		if (strstr(p,"alc")) _CheckCodecs(Codecs, Sink, "alc", 1, "m4a");
		if (strstr(p,"pcm")) _CheckCodecs(Codecs, Sink, "pcm", 2, "wav", "audio/L");
		if (strstr(p,"aif")) _CheckCodecs(Codecs, Sink, "aif", 3, "aif", "wav", "audio/L");

		p = (q) ? q + 1 : NULL;
	}

	NFREE(buf);
}


 /*----------------------------------------------------------------------------*/
void MakeMacUnique(struct sMR *Device)
{
	int i;

	for (i = 0; i < MAX_RENDERERS; i++) {
		if (!glMRDevices[i].Running || Device == &glMRDevices[i]) continue;
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

