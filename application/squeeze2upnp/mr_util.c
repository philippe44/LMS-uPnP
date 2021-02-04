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

int 	_voidHandler(Upnp_EventType EventType, void *_Event, void *Cookie) { return 0; }


/*----------------------------------------------------------------------------*/
int CalcGroupVolume(struct sMR *Device) {
	int i, n = 0;
	double GroupVolume = 0;

	if (!*Device->Service[GRP_REND_SRV_IDX].ControlURL) return -1;

	for (i = 0; i < MAX_RENDERERS; i++) {
		struct sMR *p = glMRDevices + i;
		if (p->Running && (p == Device || p->Master == Device)) {
			if (p->Volume == -1) p->Volume = CtrlGetVolume(p);
			GroupVolume += p->Volume;
			n++;
		}
	}

	return GroupVolume / n;
}


/*----------------------------------------------------------------------------*/
char *MakeProtoInfo(char *MimeType, u32_t duration)
{
	char *buf, *DLNAfeatures;

	DLNAfeatures = make_dlna_content(MimeType, duration);
	asprintf(&buf, "http-get:*:%s:%s", MimeType, DLNAfeatures);
	free(DLNAfeatures);

	return buf;
}


/*----------------------------------------------------------------------------*/
struct sMR *GetMaster(struct sMR *Device, char **Name)
{
	IXML_Document *ActionNode = NULL, *Response;
	char *Body;
	struct sMR *Master = NULL;
	struct sService *Service = &Device->Service[TOPOLOGY_IDX];
	bool done = false;

	if (!*Service->ControlURL) return NULL;

	ActionNode = UpnpMakeAction("GetZoneGroupState", Service->Type, 0, NULL);
	UpnpSendAction(glControlPointHandle, Service->ControlURL, Service->Type,
								 NULL, ActionNode, &Response);

	if (ActionNode) ixmlDocument_free(ActionNode);

	Body = XMLGetFirstDocumentItem(Response, "ZoneGroupState", true);
	if (Response) ixmlDocument_free(Response);

	Response = ixmlParseBuffer(Body);
	NFREE(Body);

	if (Response) {
		char myUUID[RESOURCE_LENGTH] = "";
		IXML_NodeList *GroupList = ixmlDocument_getElementsByTagName(Response, "ZoneGroup");
		int i;

		sscanf(Device->UDN, "uuid:%s", myUUID);

		// list all ZoneGroups
		for (i = 0; !done && GroupList && i < (int) ixmlNodeList_length(GroupList); i++) {
			IXML_Node *Group = ixmlNodeList_item(GroupList, i);
			const char *Coordinator = ixmlElement_getAttribute((IXML_Element*) Group, "Coordinator");
			IXML_NodeList *MemberList = ixmlDocument_getElementsByTagName((IXML_Document*) Group, "ZoneGroupMember");
			int j;

			// list all ZoneMembers
			for (j = 0; !done && j < (int) ixmlNodeList_length(MemberList); j++) {
				IXML_Node *Member = ixmlNodeList_item(MemberList, j);
				const char *UUID = ixmlElement_getAttribute((IXML_Element*) Member, "UUID");
				int k;

				// get ZoneName
				if (!strcasecmp(myUUID, UUID)) {
					NFREE(*Name);
					*Name = strdup(ixmlElement_getAttribute((IXML_Element*) Member, "ZoneName"));
					if (!strcasecmp(myUUID, Coordinator)) done = true;
				}

				// look for our master (if we are not)
				for (k = 0; !done && k < MAX_RENDERERS; k++) {
					if (glMRDevices[k].Running && strcasestr(glMRDevices[k].UDN, (char*) Coordinator)) {
						Master = glMRDevices + k;
						LOG_DEBUG("Found Master %s %s", myUUID, Master->UDN);
						done = true;
					}
				}
			}

			ixmlNodeList_free(MemberList);
		}

		// our master is not yet discovered, refer to self then
		if (!done) {
			Master = Device;
			LOG_INFO("[%p]: Master not discovered yet, assigning to self", Device);
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

	// kick-up all sleepers
	WakeAll();

	pthread_mutex_unlock(&p->Mutex);
	pthread_join(p->Thread, NULL);

	AVTActionFlush(&p->ActionQueue);

	NFREE(p->NextProtoInfo);
	NFREE(p->NextURI);
	NFREE(p->ExpectedURI);
	NFREE(p->Sink);
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
bool CheckAndLock(struct sMR *Device)
{
	if (!Device) {
		LOG_DEBUG("device is NULL", NULL);
		return false;
	}

	pthread_mutex_lock(&Device->Mutex);

	if (Device->Running) return true;

	LOG_INFO("[%p]: device has been removed", Device);
	pthread_mutex_unlock(&Device->Mutex);

	return false;
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
		// in case Info finishes by a serie of ','
		if (!p) break;
		n += strlen(p) + 1;
		if (sscanf(p, "http-get:*:%[^:]", MimeType) && strstr(MimeType, "audio")) {
			MimeTypes[i] = strdup(MimeType);
			i++;
		}
		p += strlen(p) + 1;
	} while (i < MAX_MIMETYPES && n < size);

	p = Forced;
	size = p ? strlen(p) : 0;
	while (size > 0 && i < MAX_MIMETYPES) {
		strtok(p, ",");
		MimeTypes[i] = strdup(p);
		size -= strlen(p) + 1;
		p += strlen(p) + 1;
		i++;
	}

	return MimeTypes;
}


/*----------------------------------------------------------------------------*/
static void _CheckCodecs(char *Codecs, char *Sink, char *Forced, char *Details, char *Codec, int n, ...)
{
	int i;
	va_list args;

	va_start(args, n);

	for (i = 0; i < n; i++) {
		char *lookup = va_arg(args, char*);

		if ((strstr(Sink, lookup) && (!Details || strstr(Sink, Details))) ||
			(strstr(Forced, lookup) && (!Details || strstr(Forced, Details)))) {
			if (strlen(Codecs)) {
				strcat(Codecs, ",");
				strcat(Codecs, Codec);
			} else strcpy(Codecs, Codec);
			return;
		}
	}

	va_end(args);
}

/*----------------------------------------------------------------------------*/
void CheckCodecs(char *Codecs, char *Sink, char *Forced)
{
	char *p, *buf;

	p = buf = strdup(Codecs);
	*Codecs = '\0';

	while (p && *p) {
		char *q = strchr(p, ',');
		if (q) *q = '\0';

		if (strstr(p,"mp3")) _CheckCodecs(Codecs, Sink, Forced, NULL, "mp3", 2, "mp3", "mpeg");
		if (strstr(p,"flc")) _CheckCodecs(Codecs, Sink, Forced, NULL, "flc", 1, "flac");
		if (strstr(p,"wma")) _CheckCodecs(Codecs, Sink, Forced, NULL, "wma", 1, "wma");
		if (strstr(p,"ogg")) _CheckCodecs(Codecs, Sink, Forced, NULL, "ogg", 1, "ogg");
		if (strstr(p,"ops")) _CheckCodecs(Codecs, Sink, Forced, "codecs=opus", "ops", 1, "ogg");
		if (strstr(p,"ogf")) _CheckCodecs(Codecs, Sink, Forced, "codecs=flac", "ogf", 1, "ogg");
		if (strstr(p,"aac")) _CheckCodecs(Codecs, Sink, Forced, NULL, "aac", 3, "aac", "m4a", "mp4");
		if (strstr(p,"alc")) _CheckCodecs(Codecs, Sink, Forced, NULL, "alc", 2, "m4a", "mp4");
		if (strstr(p,"pcm")) _CheckCodecs(Codecs, Sink, Forced, NULL, "pcm", 2, "wav", "audio/L");
		if (strstr(p,"wav")) _CheckCodecs(Codecs, Sink, Forced, NULL, "wav", 2, "wav", "audio/L");
		if (strstr(p,"aif")) _CheckCodecs(Codecs, Sink, Forced, NULL, "aif", 3, "aif", "wav", "audio/L");
		if (strstr(p,"dsf")) _CheckCodecs(Codecs, Sink, Forced, NULL, "dsf", 2, "dsf", "dsd");
		if (strstr(p,"dff")) _CheckCodecs(Codecs, Sink, Forced, NULL, "dff", 2, "dff", "dsd");

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
			memset(&Device->sq_config.mac[0], 0xbb, 2);
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

