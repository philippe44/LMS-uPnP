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

#include <stdarg.h>

#include "squeezedefs.h"
#include "squeeze2upnp.h"
#include "avt_util.h"
#include "mr_util.h"
#include "util.h"
#include "util_common.h"
#include "log_util.h"

extern log_level	slimproto_loglevel;
extern log_level	stream_loglevel;
extern log_level	decode_loglevel;
extern log_level	output_loglevel;
extern log_level	web_loglevel;
extern log_level	main_loglevel;
extern log_level	slimmain_loglevel;
extern log_level	util_loglevel;
extern log_level	upnp_loglevel;

extern log_level util_loglevel;
//static log_level *loglevel = &util_loglevel;

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
void SaveConfig(char *name, void *ref, bool full)
{
	struct sMR *p;
	IXML_Document *doc = ixmlDocument_createDocument();
	IXML_Document *old_doc = ref;
	IXML_Node	 *root, *common;
	IXML_NodeList *list;
	IXML_Element *old_root;
	char *s;
	FILE *file;
	int i;

	old_root = ixmlDocument_getElementById(old_doc, "squeeze2upnp");

	if (!full && old_doc) {
		ixmlDocument_importNode(doc, (IXML_Node*) old_root, true, &root);
		ixmlNode_appendChild((IXML_Node*) doc, root);

		list = ixmlDocument_getElementsByTagName((IXML_Document*) root, "device");
		for (i = 0; i < (int) ixmlNodeList_length(list); i++) {
			IXML_Node *device;

			device = ixmlNodeList_item(list, i);
			ixmlNode_removeChild(root, device, &device);
			ixmlNode_free(device);
		}
		if (list) ixmlNodeList_free(list);
		common = (IXML_Node*) ixmlDocument_getElementById((IXML_Document*) root, "common");
	}
	else {
		root = XMLAddNode(doc, NULL, "squeeze2upnp", NULL);
		common = XMLAddNode(doc, root, "common", NULL);
	}

	XMLUpdateNode(doc, root, "upnp_socket", gluPNPSocket);
	XMLUpdateNode(doc, root, "slimproto_log", level2debug(slimproto_loglevel));
	XMLUpdateNode(doc, root, "stream_log", level2debug(stream_loglevel));
	XMLUpdateNode(doc, root, "output_log", level2debug(output_loglevel));
	XMLUpdateNode(doc, root, "decode_log", level2debug(decode_loglevel));
	XMLUpdateNode(doc, root, "web_log", level2debug(web_loglevel));
	XMLUpdateNode(doc, root, "main_log",level2debug(main_loglevel));
	XMLUpdateNode(doc, root, "slimmain_log", level2debug(slimmain_loglevel));
	XMLUpdateNode(doc, root, "upnp_log",level2debug(upnp_loglevel));
	XMLUpdateNode(doc, root, "util_log",level2debug(util_loglevel));
	XMLUpdateNode(doc, root, "upnp_scan_interval", "%d", (u32_t) gluPNPScanInterval);
	XMLUpdateNode(doc, root, "upnp_scan_timeout", "%d", (u32_t) gluPNPScanTimeout);
	XMLUpdateNode(doc, root, "log_limit", "%d", (s32_t) glLogLimit);

	XMLUpdateNode(doc, common, "streambuf_size", "%d", (u32_t) glDeviceParam.stream_buf_size);
	XMLUpdateNode(doc, common, "output_size", "%d", (u32_t) glDeviceParam.output_buf_size);
	XMLUpdateNode(doc, common, "buffer_dir", glDeviceParam.buffer_dir);
	XMLUpdateNode(doc, common, "buffer_limit", "%d", (u32_t) glDeviceParam.buffer_limit);
	XMLUpdateNode(doc, common, "stream_length", "%d", (s32_t) glMRConfig.StreamLength);
	XMLUpdateNode(doc, common, "stream_pacing_size", "%d", (int) glDeviceParam.stream_pacing_size);
	XMLUpdateNode(doc, common, "max_GET_bytes", "%d", (s32_t) glDeviceParam.max_get_bytes);
	XMLUpdateNode(doc, common, "keep_buffer_file", "%d", (int) glDeviceParam.keep_buffer_file);
	XMLUpdateNode(doc, common, "enabled", "%d", (int) glMRConfig.Enabled);
	XMLUpdateNode(doc, common, "default_on", "%d", (int) glMRConfig.DefaultOn);
	XMLUpdateNode(doc, common, "process_mode", "%d", (int) glMRConfig.ProcessMode);
	XMLUpdateNode(doc, common, "codecs", glDeviceParam.codecs);
	XMLUpdateNode(doc, common, "sample_rate", "%d", (int) glDeviceParam.sample_rate);
	XMLUpdateNode(doc, common, "L24_format", "%d", (int) glDeviceParam.L24_format);
	XMLUpdateNode(doc, common, "flac_header", "%d", (int) glDeviceParam.flac_header);
	XMLUpdateNode(doc, common, "allow_flac", "%d", (int) glMRConfig.AllowFlac);
	XMLUpdateNode(doc, common, "seek_after_pause", "%d", (int) glMRConfig.SeekAfterPause);
	XMLUpdateNode(doc, common, "byte_seek", "%d", (int) glMRConfig.ByteSeek);
	XMLUpdateNode(doc, common, "send_icy", "%d", (int) glDeviceParam.send_icy);
	XMLUpdateNode(doc, common, "volume_on_play", "%d", (int) glMRConfig.VolumeOnPlay);
	XMLUpdateNode(doc, common, "volume_feedback", "%d", (int) glMRConfig.VolumeFeedback);
	XMLUpdateNode(doc, common, "send_metadata", "%d", (int) glMRConfig.SendMetaData);
	XMLUpdateNode(doc, common, "send_coverart", "%d", (int) glMRConfig.SendCoverArt);
	XMLUpdateNode(doc, common, "max_volume", "%d", glMRConfig.MaxVolume);
	XMLUpdateNode(doc, common, "accept_nexturi", "%d", (int) glMRConfig.AcceptNextURI);
	XMLUpdateNode(doc, common, "min_gapless", "%d", (int) glMRConfig.MinGapless);
	XMLUpdateNode(doc, common, "upnp_remove_count", "%d", (u32_t) glMRConfig.UPnPRemoveCount);
	XMLUpdateNode(doc, common, "raw_audio_format", glMRConfig.RawAudioFormat);
	XMLUpdateNode(doc, common, "match_endianness", "%d", (int) glMRConfig.MatchEndianness);
	XMLUpdateNode(doc, common, "auto_play", "%d", (int) glMRConfig.AutoPlay);
	XMLUpdateNode(doc, common, "server", glDeviceParam.server);

	for (i = 0; i < MAX_RENDERERS; i++) {
		IXML_Node *dev_node;

		if (!glMRDevices[i].InUse) continue;
		else p = &glMRDevices[i];

		// existing device, keep param and update "name" if LMS has requested it
		if (old_doc && ((dev_node = (IXML_Node*) FindMRConfig(old_doc, p->UDN)) != NULL)) {
			IXML_Node *node;

			ixmlDocument_importNode(doc, dev_node, true, &dev_node);

			// TODO: remove after migration
			XMLUpdateNode(doc, dev_node, "friendly_name", p->FriendlyName);
			if (!strstr(p->sq_config.server, "?")) XMLUpdateNode(doc, dev_node, "server", p->sq_config.server);

			ixmlNode_appendChild((IXML_Node*) root, dev_node);
			node = (IXML_Node*) ixmlDocument_getElementById((IXML_Document*) dev_node, "name");
			node = ixmlNode_getFirstChild(node);
			ixmlNode_setNodeValue(node, p->sq_config.name);

			if (!strstr(p->sq_config.server, "?")) {
				ixmlNode_appendChild((IXML_Node*) root, dev_node);
				node = (IXML_Node*) ixmlDocument_getElementById((IXML_Document*) dev_node, "server");
				node = ixmlNode_getFirstChild(node);
				ixmlNode_setNodeValue(node, p->sq_config.server);
			}
		}
		// new device, add nodes
		else {
			dev_node = XMLAddNode(doc, root, "device", NULL);
			XMLAddNode(doc, dev_node, "udn", p->UDN);
			XMLAddNode(doc, dev_node, "name", p->FriendlyName);
			XMLAddNode(doc, dev_node, "friendly_name", p->FriendlyName);
			if (!strstr(p->sq_config.server, "?")) XMLAddNode(doc, dev_node, "server", p->sq_config.server);
			XMLAddNode(doc, dev_node, "mac", "%02x:%02x:%02x:%02x:%02x:%02x", p->sq_config.mac[0],
						p->sq_config.mac[1], p->sq_config.mac[2], p->sq_config.mac[3], p->sq_config.mac[4], p->sq_config.mac[5]);
			XMLAddNode(doc, dev_node, "enabled", "%d", (int) p->Config.Enabled);
		}
	}

	// add devices in old XML file that has not been discovered
	list = ixmlDocument_getElementsByTagName((IXML_Document*) old_root, "device");
	for (i = 0; i < (int) ixmlNodeList_length(list); i++) {
		char *udn;
		IXML_Node *device, *node;

		device = ixmlNodeList_item(list, i);
		node = (IXML_Node*) ixmlDocument_getElementById((IXML_Document*) device, "udn");
		node = ixmlNode_getFirstChild(node);
		udn = (char*) ixmlNode_getNodeValue(node);
		if (!FindMRConfig(doc, udn)) {
			ixmlDocument_importNode(doc, device, true, &device);
			ixmlNode_appendChild((IXML_Node*) root, device);
		}
	}
	if (list) ixmlNodeList_free(list);

	file = fopen(name, "wb");
	s = ixmlDocumenttoString(doc);
	fwrite(s, 1, strlen(s), file);
	fclose(file);
	free(s);

	ixmlDocument_free(doc);
}

/*----------------------------------------------------------------------------*/
static void LoadConfigItem(tMRConfig *Conf, sq_dev_param_t *sq_conf, char *name, char *val)
{
	if (!strcmp(name, "streambuf_size")) sq_conf->stream_buf_size = atol(val);
	if (!strcmp(name, "output_size")) sq_conf->output_buf_size = atol(val);
	if (!strcmp(name, "buffer_dir")) strcpy(sq_conf->buffer_dir, val);
	if (!strcmp(name, "buffer_limit")) sq_conf->buffer_limit = atol(val);
	if (!strcmp(name, "stream_length")) Conf->StreamLength = atol(val);
	if (!strcmp(name, "stream_pacing_size")) sq_conf->stream_pacing_size = atol(val);
	if (!strcmp(name, "max_GET_bytes")) sq_conf->max_get_bytes = atol(val);
	if (!strcmp(name, "send_icy")) sq_conf->send_icy = atol(val);
	if (!strcmp(name, "enabled")) Conf->Enabled = atol(val);
	if (!strcmp(name, "default_on")) Conf->DefaultOn = atol(val);
	if (!strcmp(name, "process_mode")) {
		Conf->ProcessMode = atol(val);
		sq_conf->mode = Conf->ProcessMode;
	}
	if (!strcmp(name, "codecs")) strcpy(sq_conf->codecs, val);
	if (!strcmp(name, "sample_rate"))sq_conf->sample_rate = atol(val);
	if (!strcmp(name, "L24_format"))sq_conf->L24_format = atol(val);
	if (!strcmp(name, "flac_header"))sq_conf->flac_header = atol(val);
	if (!strcmp(name, "allow_flac")) Conf->AllowFlac = atol(val);
	if (!strcmp(name, "keep_buffer_file"))sq_conf->keep_buffer_file = atol(val);
	if (!strcmp(name, "upnp_remove_count"))Conf->UPnPRemoveCount = atol(val);
	if (!strcmp(name, "raw_audio_format")) strcpy(Conf->RawAudioFormat, val);
	if (!strcmp(name, "match_endianness")) Conf->MatchEndianness = atol(val);
	if (!strcmp(name, "seek_after_pause")) Conf->SeekAfterPause = atol(val);
	if (!strcmp(name, "byte_seek")) Conf->ByteSeek = atol(val);
	if (!strcmp(name, "volume_on_play")) Conf->VolumeOnPlay = atol(val);
	if (!strcmp(name, "volume_feedback")) Conf->VolumeFeedback = atol(val);
	if (!strcmp(name, "max_volume")) Conf->MaxVolume = atol(val);
	if (!strcmp(name, "auto_play")) Conf->AutoPlay = atol(val);
	if (!strcmp(name, "accept_nexturi")) Conf->AcceptNextURI = atol(val);
	if (!strcmp(name, "min_gapless")) Conf->MinGapless = atol(val);
	if (!strcmp(name, "send_metadata")) Conf->SendMetaData = atol(val);
	if (!strcmp(name, "send_coverart")) Conf->SendCoverArt = atol(val);
	if (!strcmp(name, "name")) strcpy(sq_conf->name, val);
	if (!strcmp(name, "friendly_name")) strcpy(Conf->Name, val);
	if (!strcmp(name, "server")) strcpy(sq_conf->server, val);
	if (!strcmp(name, "mac"))  {
		unsigned mac[6];
		int i;
		// seems to be a Windows scanf buf, cannot support %hhx
		sscanf(val,"%2x:%2x:%2x:%2x:%2x:%2x", &mac[0], &mac[1], &mac[2], &mac[3], &mac[4], &mac[5]);
		for (i = 0; i < 6; i++) sq_conf->mac[i] = mac[i];
	}
}

/*----------------------------------------------------------------------------*/
static void LoadGlobalItem(char *name, char *val)
{
	if (!val) return;

	// temporary to ensure parameter transfer from global to common
	if (!strcmp(name, "server")) strcpy(glDeviceParam.server, val);

	if (!strcmp(name, "upnp_socket")) strcpy(gluPNPSocket, val);
	if (!strcmp(name, "slimproto_log")) slimproto_loglevel = debug2level(val);
	if (!strcmp(name, "stream_log")) stream_loglevel = debug2level(val);
	if (!strcmp(name, "output_log")) output_loglevel = debug2level(val);
	if (!strcmp(name, "decode_log")) decode_loglevel = debug2level(val);
	if (!strcmp(name, "web_log")) web_loglevel = debug2level(val);
	if (!strcmp(name, "main_log")) main_loglevel = debug2level(val);
	if (!strcmp(name, "slimmain_log")) slimmain_loglevel = debug2level(val);
	if (!strcmp(name, "upnp_log")) upnp_loglevel = debug2level(val);
	if (!strcmp(name, "util_log")) util_loglevel = debug2level(val);
	if (!strcmp(name, "upnp_scan_interval")) gluPNPScanInterval = atol(val);
	if (!strcmp(name, "upnp_scan_timeout")) gluPNPScanTimeout = atol(val);
	if (!strcmp(name, "log_limit")) glLogLimit = atol(val);
 }


/*----------------------------------------------------------------------------*/
void *FindMRConfig(void *ref, char *UDN)
{
	IXML_Element *elm;
	IXML_Node	*device = NULL;
	IXML_NodeList *l1_node_list;
	IXML_Document *doc = (IXML_Document*) ref;
	char *v;
	unsigned i;

	elm = ixmlDocument_getElementById(doc, "squeeze2upnp");
	l1_node_list = ixmlDocument_getElementsByTagName((IXML_Document*) elm, "udn");
	for (i = 0; i < ixmlNodeList_length(l1_node_list); i++) {
		IXML_Node *l1_node, *l1_1_node;
		l1_node = ixmlNodeList_item(l1_node_list, i);
		l1_1_node = ixmlNode_getFirstChild(l1_node);
		v = (char*) ixmlNode_getNodeValue(l1_1_node);
		if (v && !strcmp(v, UDN)) {
			device = ixmlNode_getParentNode(l1_node);
			break;
		}
	}
	if (l1_node_list) ixmlNodeList_free(l1_node_list);
	return device;
}

/*----------------------------------------------------------------------------*/
void *LoadMRConfig(void *ref, char *UDN, tMRConfig *Conf, sq_dev_param_t *sq_conf)
{
	IXML_NodeList *node_list;
	IXML_Document *doc = (IXML_Document*) ref;
	IXML_Node *node;
	char *n, *v;
	unsigned i;

	node = (IXML_Node*) FindMRConfig(doc, UDN);
	if (node) {
		node_list = ixmlNode_getChildNodes(node);
		for (i = 0; i < ixmlNodeList_length(node_list); i++) {
			IXML_Node *l1_node, *l1_1_node;
			l1_node = ixmlNodeList_item(node_list, i);
			n = (char*) ixmlNode_getNodeName(l1_node);
			l1_1_node = ixmlNode_getFirstChild(l1_node);
			v = (char*) ixmlNode_getNodeValue(l1_1_node);
			LoadConfigItem(Conf, sq_conf, n, v);
		}
		if (node_list) ixmlNodeList_free(node_list);
	}

	return node;
}

/*----------------------------------------------------------------------------*/
void *LoadConfig(char *name, tMRConfig *Conf, sq_dev_param_t *sq_conf)
{
	IXML_Element *elm;
	IXML_Document	*doc;

	doc = ixmlLoadDocument(name);
	if (!doc) return NULL;

	elm = ixmlDocument_getElementById(doc, "squeeze2upnp");
	if (elm) {
		unsigned i;
		char *n, *v;
		IXML_NodeList *l1_node_list;
		l1_node_list = ixmlNode_getChildNodes((IXML_Node*) elm);
		for (i = 0; i < ixmlNodeList_length(l1_node_list); i++) {
			IXML_Node *l1_node, *l1_1_node;
			l1_node = ixmlNodeList_item(l1_node_list, i);
			n = (char*) ixmlNode_getNodeName(l1_node);
			l1_1_node = ixmlNode_getFirstChild(l1_node);
			v = (char*) ixmlNode_getNodeValue(l1_1_node);
			LoadGlobalItem(n, v);
		}
		if (l1_node_list) ixmlNodeList_free(l1_node_list);
	}

	elm = ixmlDocument_getElementById((IXML_Document	*)elm, "common");
	if (elm) {
		char *n, *v;
		IXML_NodeList *l1_node_list;
		unsigned i;
		l1_node_list = ixmlNode_getChildNodes((IXML_Node*) elm);
		for (i = 0; i < ixmlNodeList_length(l1_node_list); i++) {
			IXML_Node *l1_node, *l1_1_node;
			l1_node = ixmlNodeList_item(l1_node_list, i);
			n = (char*) ixmlNode_getNodeName(l1_node);
			l1_1_node = ixmlNode_getFirstChild(l1_node);
			v = (char*) ixmlNode_getNodeValue(l1_1_node);
			LoadConfigItem(&glMRConfig, &glDeviceParam, n, v);
		}
		if (l1_node_list) ixmlNodeList_free(l1_node_list);
	}

	return doc;
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

