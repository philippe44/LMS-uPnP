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

static log_level loglevel = lWARN;

/*----------------------------------------------------------------------------*/
void MRutilInit(log_level level)
{
	loglevel = level;
}

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

	if (strstr(ext, "wav")) return 'p';
	if (strstr(ext, "flac") || strstr(ext, "flc")) return 'f';
	if (strstr(ext, "mp3")) return 'm';
	if (strstr(ext, "wma")) return 'w';
	if (strstr(ext, "ogg")) return 'o';
	if (strstr(ext, "m4a")) return 'a';
	if (strstr(ext, "mp4")) return 'l';
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
				strcpy(uri->content_type, fmt);
				strcpy(uri->proto_info, *p);
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
	char fmt[SQ_STR_LENGTH];

	sprintf(fmt, "audio/L%d", (Device->sq_config.L24_format != L24_TRUNC_16) ? uri->sample_size : 16);
	p = buf = strdup(Device->Config.RawAudioFormat);

	while (!ret && p && *p) {
		u8_t order = 0xff;
		char *q = strchr(p, ',');

		if (q) *q = '\0';

		if (strstr(p, "pcm") || strstr(p,"raw")) { ret = _SetContentType(Device->ProtocolCap, uri, 1, fmt); order = 0;}
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
	strcpy(uri->ext, format2ext(uri->content_type[0]));

	switch (uri->content_type[0]) {
	case 'm': return _SetContentType(Device->ProtocolCap, uri, 3, "audio/mp3", "audio/mpeg", "audio/mpeg3");
	case 'f': return _SetContentType(Device->ProtocolCap, uri, 2, "audio/x-flac", "audio/flac");
	case 'w': return _SetContentType(Device->ProtocolCap, uri, 2, "audio/x-wma", "audio/wma");
	case 'o': return _SetContentType(Device->ProtocolCap, uri, 1, "audio/ogg");
	case 'a': return _SetContentType(Device->ProtocolCap, uri, 4, "audio/x-aac", "audio/aac", "audio/m4a", "audio/mp4");
	case 'l': return _SetContentType(Device->ProtocolCap, uri, 1, "audio/m4a");
	case 'p': return SetContentTypeRawAudio(Device, uri, Device->Config.MatchEndianness);
	default:
		strcpy(uri->content_type, "unknown");
		strcpy(uri->proto_info, "");
		return false;
	}
}

/*----------------------------------------------------------------------------*/
void FlushActionList(struct sMR *Device)
{
	struct sAction *p;

	ithread_mutex_lock(&Device->ActionsMutex);
	p = Device->Actions;
	while (Device->Actions) {
		p = Device->Actions;
		Device->Actions = Device->Actions->Next;
		free(p);
	}
	ithread_mutex_unlock(&Device->ActionsMutex);
}

/*----------------------------------------------------------------------------*/
void InitActionList(struct sMR *Device)
{
	Device->Actions = NULL;
	ithread_mutex_init(&Device->ActionsMutex, 0);
}

/*----------------------------------------------------------------------------*/
void QueueAction(sq_dev_handle_t handle, struct sMR *Device, sq_action_t action, u8_t *cookie, void *param, bool ordered)
{
	struct sAction *Action = malloc(sizeof(struct sAction));
	struct sAction *p;

	LOG_INFO("[%p]: queuing action %d", Device, action);

	Action->Handle  = handle;
	Action->Caller  = Device;
	Action->Action  = action;
	Action->Cookie  = cookie;
	Action->Next	= NULL;
	Action->Ordered	= ordered;

	switch(action) {
	case SQ_VOLUME:
		Action->Param.Volume = *((u32_t*) param);
		break;
	case SQ_SEEK:
		Action->Param.Time = *((u32_t*) param);
		break;
	default:
		break;
	}

	ithread_mutex_lock(&Device->ActionsMutex);
	if (!Device->Actions) Device->Actions = Action;
	else {
		p = Device->Actions;
		while (p->Next) p = p->Next;
		p->Next = Action;
	}
	ithread_mutex_unlock(&Device->ActionsMutex);
}

/*----------------------------------------------------------------------------*/
struct sAction *UnQueueAction(struct sMR *Device, bool Keep)
{
	struct sAction  *p = NULL;

	if (Keep) return Device->Actions;

	ithread_mutex_lock(&Device->ActionsMutex);
	if (Device->Actions) {
		p = Device->Actions;
		Device->Actions = Device->Actions->Next;
	}
	ithread_mutex_unlock(&Device->ActionsMutex);

	if (!p) return NULL;

	LOG_INFO("[%p]: un-queuing action %d", p->Caller, p->Action);
	if (p->Caller != Device) {
		LOG_ERROR("[%p]: action in wrong queue %p", Device, p->Caller);
	}

	return p;
}


/*----------------------------------------------------------------------------*/
void FlushMRDevices(void)
{
	int i;

	for (i = 0; i < MAX_RENDERERS; i++) {
		struct sMR *p = &glMRDevices[i];
		if (p->InUse) {
			// critical to stop the device otherwise libupnp mean wait forever
			if (p->sqState == SQ_PLAY || p->sqState == SQ_PAUSE)
				AVTBasic(p->Service[AVT_SRV_IDX].ControlURL, "Stop", p->seqN++);
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
	ithread_join(p->Thread, NULL);
	p->InUse = false;

	FlushActionList(p);
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
void ParseProtocolInfo(struct sMR *Device, char *Info)
{
	char *p = Info;
	int n = 0, i = 0;
	int size = strlen(Info);

	// strtok is no re-entrant
	memset(Device->ProtocolCap, 0, sizeof(char*) * (MAX_PROTO + 1));
	do {
		p = strtok(p, ",");
		n += strlen(p) + 1;
		if (strstr(p, "http-get") && strstr(p, "audio")) {
			Device->ProtocolCap[i] = malloc(strlen(p) + 1);
			strcpy(Device->ProtocolCap[i], p);
			i++;
		}
		p += strlen(p) + 1;
	} while (i < MAX_PROTO && n < size);

	// remove trailing "*" as we WILL add DLNA-related info, so some options to come
	for (i = 0; (p = Device->ProtocolCap[i]) != NULL; i++)
		if (p[strlen(p) - 1] == '*') p[strlen(p) - 1] = '\0';

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
		root = ixmlNode_cloneNode((IXML_Node*) old_root, true);
		ixmlNode_appendChild((IXML_Node*) doc, root);

		list = ixmlDocument_getElementsByTagName((IXML_Document*) root, "device");
		for (i = 0; i < (int) ixmlNodeList_length(list); i++) {
			IXML_Node *device;

			device = ixmlNodeList_item(list, i);
			ixmlNode_removeChild(root, device, &device);
			ixmlNode_free(device);
		}
		if (list) ixmlNodeList_free(list);
	}
	else {
		root = XMLAddNode(doc, NULL, "squeeze2upnp", NULL);

		XMLAddNode(doc, root, "server", glSQServer);
		XMLAddNode(doc, root, "upnp_socket", gluPNPSocket);
		XMLAddNode(doc, root, "slimproto_stream_port", "%d", gl_slimproto_stream_port);
		XMLAddNode(doc, root, "base_mac", "%02x:%02x:%02x:%02x:%02x:%02x", glMac[0],
					glMac[1], glMac[2], glMac[3], glMac[4], glMac[5]);
		XMLAddNode(doc, root, "slimproto_log", level2debug(glLog.slimproto));
		XMLAddNode(doc, root, "stream_log", level2debug(glLog.stream));
		XMLAddNode(doc, root, "output_log", level2debug(glLog.output));
		XMLAddNode(doc, root, "decode_log", level2debug(glLog.decode));
		XMLAddNode(doc, root, "web_log", level2debug(glLog.web));
		XMLAddNode(doc, root, "upnp_log", level2debug(glLog.upnp));
		XMLAddNode(doc, root, "main_log",level2debug(glLog.main));
		XMLAddNode(doc, root, "sq2mr_log", level2debug(glLog.sq2mr));
		XMLAddNode(doc, root, "upnp_scan_interval", "%d", (u32_t) gluPNPScanInterval);
		XMLAddNode(doc, root, "upnp_scan_timeout", "%d", (u32_t) gluPNPScanTimeout);
		XMLAddNode(doc, root, "log_limit", "%d", (s32_t) glLogLimit);

		common = XMLAddNode(doc, root, "common", NULL);
		XMLAddNode(doc, common, "streambuf_size", "%d", (u32_t) glDeviceParam.stream_buf_size);
		XMLAddNode(doc, common, "output_size", "%d", (u32_t) glDeviceParam.output_buf_size);
		XMLAddNode(doc, common, "buffer_dir", glDeviceParam.buffer_dir);
		XMLAddNode(doc, common, "buffer_limit", "%d", (u32_t) glDeviceParam.buffer_limit);
		XMLAddNode(doc, common, "stream_length", "%d", (s32_t) glMRConfig.StreamLength);
		XMLAddNode(doc, common, "max_read_wait", "%d", (int) glDeviceParam.max_read_wait);
		XMLAddNode(doc, common, "max_GET_bytes", "%d", (s32_t) glDeviceParam.max_get_bytes);
		XMLAddNode(doc, common, "keep_buffer_file", "%d", (int) glDeviceParam.keep_buffer_file);
		XMLAddNode(doc, common, "enabled", "%d", (int) glMRConfig.Enabled);
		XMLAddNode(doc, common, "process_mode", "%d", (int) glMRConfig.ProcessMode);
		XMLAddNode(doc, common, "codecs", glDeviceParam.codecs);
		XMLAddNode(doc, common, "sample_rate", "%d", (int) glDeviceParam.sample_rate);
		XMLAddNode(doc, common, "L24_format", "%d", (int) glDeviceParam.L24_format);
		XMLAddNode(doc, common, "flac_header", "%d", (int) glDeviceParam.flac_header);
		XMLAddNode(doc, common, "seek_after_pause", "%d", (int) glMRConfig.SeekAfterPause);
		XMLAddNode(doc, common, "force_volume", "%d", (int) glMRConfig.ForceVolume);
		XMLAddNode(doc, common, "volume_on_play", "%d", (int) glMRConfig.VolumeOnPlay);
		XMLAddNode(doc, common, "send_metadata", "%d", (int) glMRConfig.SendMetaData);
		XMLAddNode(doc, common, "volume_curve", glMRConfig.VolumeCurve);
		XMLAddNode(doc, common, "max_volume", "%d", glMRConfig.MaxVolume);
		XMLAddNode(doc, common, "accept_nexturi", "%d", (int) glMRConfig.AcceptNextURI);
		XMLAddNode(doc, common, "upnp_remove_count", "%d", (u32_t) glMRConfig.uPNPRemoveCount);
		XMLAddNode(doc, common, "raw_audio_format", glMRConfig.RawAudioFormat);
		XMLAddNode(doc, common, "match_endianness", "%d", (int) glMRConfig.MatchEndianness);
		XMLAddNode(doc, common, "pause_volume", "%d", (int) glMRConfig.PauseVolume);
	}

	for (i = 0; i < MAX_RENDERERS; i++) {
		IXML_Node *dev_node;

		if (!glMRDevices[i].InUse) continue;
		else p = &glMRDevices[i];

		if (old_doc && ((dev_node = (IXML_Node*) FindMRConfig(old_doc, p->UDN)) != NULL)) {
			dev_node = ixmlNode_cloneNode(dev_node, true);
			ixmlNode_appendChild((IXML_Node*) doc, root);
		}
		else {
			dev_node = XMLAddNode(doc, root, "device", NULL);
			XMLAddNode(doc, dev_node, "udn", p->UDN);
			XMLAddNode(doc, dev_node, "name", *(p->Config.Name) ? p->Config.Name : p->FriendlyName);
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
			device = ixmlNode_cloneNode(device, true);
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
	if (!strcmp(name, "max_read_wait")) sq_conf->max_read_wait = atol(val);
	if (!strcmp(name, "max_GET_bytes")) sq_conf->max_get_bytes = atol(val);
	if (!strcmp(name, "enabled")) Conf->Enabled = atol(val);
	if (!strcmp(name, "process_mode")) {
		Conf->ProcessMode = atol(val);
		sq_conf->mode = Conf->ProcessMode;
	}
	if (!strcmp(name, "codecs")) strcpy(sq_conf->codecs, val);
	if (!strcmp(name, "sample_rate"))sq_conf->sample_rate = atol(val);
	if (!strcmp(name, "L24_format"))sq_conf->L24_format = atol(val);
	if (!strcmp(name, "flac_header"))sq_conf->flac_header = atol(val);
	if (!strcmp(name, "keep_buffer_file"))sq_conf->keep_buffer_file = atol(val);
	if (!strcmp(name, "upnp_remove_count"))Conf->uPNPRemoveCount = atol(val);
	if (!strcmp(name, "raw_audio_format")) strcpy(Conf->RawAudioFormat, val);
	if (!strcmp(name, "match_endianness")) Conf->MatchEndianness = atol(val);
	if (!strcmp(name, "seek_after_pause")) {
		Conf->SeekAfterPause = atol(val);
		sq_conf->seek_after_pause = Conf->SeekAfterPause;
	}
	if (!strcmp(name, "force_volume")) Conf->ForceVolume = atol(val);
	if (!strcmp(name, "volume_on_play")) Conf->VolumeOnPlay = atol(val);
	if (!strcmp(name, "volume_curve")) strcpy(Conf->VolumeCurve, val);
	if (!strcmp(name, "max_volume")) Conf->MaxVolume = atol(val);
	if (!strcmp(name, "pause_volume")) Conf->PauseVolume = atol(val);
	if (!strcmp(name, "accept_nexturi")) Conf->AcceptNextURI = atol(val);
	if (!strcmp(name, "send_metadata")) Conf->SendMetaData = atol(val);
	if (!strcmp(name, "name")) strcpy(Conf->Name, val);
	if (!strcmp(name, "mac"))  sscanf(val,"%2hhx:%2hhx:%2hhx:%2hhx:%2hhx:%2hhx",
								   &sq_conf->mac[0],&sq_conf->mac[1],&sq_conf->mac[2],
								   &sq_conf->mac[3],&sq_conf->mac[4],&sq_conf->mac[5]);
}

/*----------------------------------------------------------------------------*/
static void LoadGlobalItem(char *name, char *val)
{
	if (!val) return;

	if (!strcmp(name, "server")) strcpy(glSQServer, val);
	if (!strcmp(name, "upnp_socket")) strcpy(gluPNPSocket, val);
	if (!strcmp(name, "slimproto_stream_port")) gl_slimproto_stream_port = atol(val);
	if (!strcmp(name, "slimproto_log")) glLog.slimproto = debug2level(val);
	if (!strcmp(name, "stream_log")) glLog.stream = debug2level(val);
	if (!strcmp(name, "output_log")) glLog.output = debug2level(val);
	if (!strcmp(name, "decode_log")) glLog.decode = debug2level(val);
	if (!strcmp(name, "web_log")) glLog.web = debug2level(val);
	if (!strcmp(name, "upnp_log")) glLog.upnp = debug2level(val);
	if (!strcmp(name, "main_log")) glLog.main = debug2level(val);
	if (!strcmp(name, "sq2mr_log")) glLog.sq2mr = debug2level(val);
	if (!strcmp(name, "base_mac"))  sscanf(val,"%2hhx:%2hhx:%2hhx:%2hhx:%2hhx:%2hhx",
								   &glMac[0],&glMac[1],&glMac[2],&glMac[3],&glMac[4],&glMac[5]);
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


