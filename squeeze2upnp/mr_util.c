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
				// special case of wave, need to alter the file extension here
				if (strstr(*p, "wav")) strcpy(uri->format, "wav");
				break;
			}
			// if the proposed format accepts any rate & channel, give it a try
			if (!strstr(*p, "channels") && !strstr(*p, "rate")) {
				sprintf(uri->content_type, "%s;channels=%d;rate=%d", fmt, uri->channels, uri->sample_rate);
				strcpy(uri->proto_info, *p);
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
bool SetContentType(char *Cap[], sq_seturi_t *uri)
{
	strcpy(uri->format, format2ext(uri->content_type[0]));

	switch (uri->content_type[0]) {
	case 'm': return _SetContentType(Cap, uri, 3, "audio/mp3", "audio/mpeg", "audio/mpeg3");
	case 'f': return _SetContentType(Cap, uri, 2, "audio/x-flac", "audio/flac");
	case 'w': return _SetContentType(Cap, uri, 2, "audio/x-wma", "audio/wma");
	case 'o': return _SetContentType(Cap, uri, 1, "audio/ogg");
	case 'a': return _SetContentType(Cap, uri, 4, "audio/x-aac", "audio/aac", "audio/m4a", "audio/mp4");
	case 'l': return _SetContentType(Cap, uri, 1, "audio/m4a");
	case 'p': {
		char p[SQ_STR_LENGTH];
		sprintf(p, "audio/L%d", uri->sample_size);
		return _SetContentType(Cap, uri, 4, p, "audio/wav", "audio/x-wav", "audio/wave");
	}
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
		Action->Param.Volume = *((double*) param);
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
void FlushMRList(void)
{
	struct sMR *p;

	ithread_mutex_lock(&glDeviceListMutex);

	p = glDeviceList;
	while (p) {
		int i = 0;
		struct sMR *n = p->Next;
		FlushActionList(p);
		NFREE(p->CurrentURI);
		NFREE(p->NextURI);
		while (p->ProtocolCap[i] && i < MAX_PROTO) {
			NFREE(p->ProtocolCap[i]);
			i++;
		}
        free(p);
		p = n;
	}

	glDeviceList = glSQ2MRList = NULL;

	ithread_mutex_unlock(&glDeviceListMutex);
}


/*----------------------------------------------------------------------------*/
struct sMR* CURL2Device(char *CtrlURL)
{
	struct sMR *p;

	ithread_mutex_lock(&glDeviceListMutex);
	p = glSQ2MRList;
	while (p) {
		int i;
		for (i = 0; i < NB_SRV; i++) {
			if (!strcmp(p->Service[i].ControlURL, CtrlURL)) {
				ithread_mutex_unlock(&glDeviceListMutex);
				return p;
			}
		}
		p = p->NextSQ;
	}
	ithread_mutex_unlock(&glDeviceListMutex);
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
	for (i = 0; (p = Device->ProtocolCap[i]); i++)
		if (p[strlen(p) - 1] == '*') p[strlen(p) - 1] = '\0';
}



/*----------------------------------------------------------------------------*/
void SaveConfig(char *name)
{
	struct sMR *p;
	IXML_Document *doc = ixmlDocument_createDocument();
	IXML_Node	 *root, *common;
	char *s;
	FILE *file;

	ithread_mutex_lock(&glDeviceListMutex);
	root = XMLAddNode(doc, NULL, "squeeze2upnp", NULL);

	XMLAddNode(doc, root, "server", glSQServer);
	XMLAddNode(doc, root, "slimproto_stream_port", "%d", gl_slimproto_stream_port);
	XMLAddNode(doc, root, "base_mac", "%02x:%02x:%02x:%02x:%02x:%02x", glMac[0], glMac[1], glMac[2], glMac[3], glMac[4], glMac[5]);
	XMLAddNode(doc, root, "slimproto_log", level2debug(glLog.slimproto));
	XMLAddNode(doc, root, "stream_log", level2debug(glLog.stream));
	XMLAddNode(doc, root, "output_log", level2debug(glLog.output));
	XMLAddNode(doc, root, "decode_log", level2debug(glLog.decode));
	XMLAddNode(doc, root, "web_log", level2debug(glLog.web));
	XMLAddNode(doc, root, "upnp_log", level2debug(glLog.upnp));
	XMLAddNode(doc, root, "main_log",level2debug(glLog.main));
	XMLAddNode(doc, root, "sq2mr_log", level2debug(glLog.sq2mr));

	common = XMLAddNode(doc, root, "common", NULL);
	XMLAddNode(doc, common, "streambuf_size", "%d", (u32_t) glDeviceParam.stream_buf_size);
	XMLAddNode(doc, common, "output_size", "%d", (u32_t) glDeviceParam.output_buf_size);
	XMLAddNode(doc, common, "buffer_dir", glDeviceParam.buffer_dir);
	XMLAddNode(doc, common, "buffer_limit", "%d", (u32_t) glDeviceParam.buffer_limit);
	XMLAddNode(doc, common, "stream_length", "%d", (s32_t) glMRConfig.StreamLength);
	XMLAddNode(doc, common, "max_read_wait", "%d", (int) glDeviceParam.max_read_wait);
	XMLAddNode(doc, common, "max_GET_bytes", "%d", (s32_t) glDeviceParam.max_get_bytes);
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
	XMLAddNode(doc, common, "accept_nexturi", "%d", (int) glMRConfig.AcceptNextURI);

	s =  ixmlDocumenttoString(doc);

	p = glDeviceList;
	while (p)
	{
		IXML_Node *dev_node;
		dev_node = XMLAddNode(doc, root, "device", NULL);
		XMLAddNode(doc, dev_node, "udn", p->UDN);
		XMLAddNode(doc, dev_node, "name", p->FriendlyName);
		XMLAddNode(doc, dev_node, "enabled", "%d", (int) p->Config.Enabled);

		if (p->sq_config.stream_buf_size != glDeviceParam.stream_buf_size)
			XMLAddNode(doc, dev_node, "streambuf_size", "%d", (u32_t) p->sq_config.stream_buf_size);
		if (p->sq_config.output_buf_size != glDeviceParam.output_buf_size)
			XMLAddNode(doc, dev_node, "output_size", "%d", (u32_t) p->sq_config.output_buf_size);
		if (strcmp(p->sq_config.buffer_dir, glDeviceParam.buffer_dir))
			XMLAddNode(doc, dev_node, "buffer_dir", p->sq_config.buffer_dir);
		if (p->sq_config.buffer_limit != glDeviceParam.buffer_limit)
			XMLAddNode(doc, dev_node, "buffer_limit", "%d", (u32_t) p->sq_config.buffer_limit);
		if (p->Config.StreamLength != glMRConfig.StreamLength)
			XMLAddNode(doc, dev_node, "stream_length", "%d", (s32_t) p->Config.StreamLength);
		if (p->sq_config.max_read_wait != glDeviceParam.max_read_wait)
			XMLAddNode(doc, dev_node, "max_read_wait", "%d", (int) p->sq_config.max_read_wait);
		if (p->sq_config.max_get_bytes != glDeviceParam.max_get_bytes)
			XMLAddNode(doc, dev_node, "max_GET_size", "%d", (s32_t) p->sq_config.max_get_bytes);
		if (p->Config.ProcessMode != glMRConfig.ProcessMode)
			XMLAddNode(doc, dev_node, "process_mode", "%d", (int) p->Config.ProcessMode);
		if (p->Config.SeekAfterPause != glMRConfig.SeekAfterPause)
			XMLAddNode(doc, dev_node, "seek_after_pause", "%d", (int) p->Config.SeekAfterPause);
		if (p->Config.ForceVolume != glMRConfig.ForceVolume)
			XMLAddNode(doc, dev_node, "force_volume", "%d", (int) p->Config.ForceVolume);
		if (p->Config.VolumeOnPlay != glMRConfig.VolumeOnPlay)
			XMLAddNode(doc, dev_node, "volume_on_play", "%d", (int) p->Config.VolumeOnPlay);
		if (p->Config.SendMetaData != glMRConfig.SendMetaData)
			XMLAddNode(doc, dev_node, "send_metadata", "%d", (int) p->Config.SendMetaData);
		if (strcmp(p->Config.VolumeCurve, glMRConfig.VolumeCurve))
			XMLAddNode(doc, dev_node, "volume_curve", p->Config.VolumeCurve);
		if (p->Config.AcceptNextURI != glMRConfig.AcceptNextURI)
			XMLAddNode(doc, dev_node, "accept_nexturi", "%d", (int) p->Config.AcceptNextURI);
		if (strcmp(p->sq_config.codecs, glDeviceParam.codecs))
			XMLAddNode(doc, dev_node, "codecs", p->sq_config.codecs);
		if (p->sq_config.sample_rate != glDeviceParam.sample_rate)
			XMLAddNode(doc, dev_node, "sample_rate", "%d", (int) p->sq_config.sample_rate);
		if (p->sq_config.L24_format != glDeviceParam.L24_format)
			XMLAddNode(doc, dev_node, "L24_format", "%d", (int) p->sq_config.L24_format);
		if (p->sq_config.flac_header != glDeviceParam.flac_header)
			XMLAddNode(doc, dev_node, "flac_header", "%d", (int) p->sq_config.flac_header);

		p = p->Next;
	}
	ithread_mutex_unlock(&glDeviceListMutex);

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
	if (!strcmp(name, "seek_after_pause")) Conf->SeekAfterPause = atol(val);
	if (!strcmp(name, "force_volume")) Conf->ForceVolume = atol(val);
	if (!strcmp(name, "volume_on_play")) Conf->VolumeOnPlay = atol(val);
	if (!strcmp(name, "volume_curve")) strcpy(Conf->VolumeCurve, val);
	if (!strcmp(name, "accept_nexturi")) Conf->AcceptNextURI = atol(val);
	if (!strcmp(name, "send_metadata")) Conf->SendMetaData = atol(val);
	if (!strcmp(name, "name")) strcpy(Conf->Name, val);
}

/*----------------------------------------------------------------------------*/
static void LoadGlobalItem(char *name, char *val)
{
	int i;
	if (!val) return;

	if (!strcmp(name, "server")) strcpy(glSQServer, val);
	if (!strcmp(name, "slimproto_stream_port")) gl_slimproto_stream_port = atol(val);
	if (!strcmp(name, "slimproto_log")) glLog.slimproto = debug2level(val);
	if (!strcmp(name, "stream_log")) glLog.stream = debug2level(val);
	if (!strcmp(name, "output_log")) glLog.output = debug2level(val);
	if (!strcmp(name, "decode_log")) glLog.decode = debug2level(val);
	if (!strcmp(name, "web_log")) glLog.web = debug2level(val);
	if (!strcmp(name, "upnp_log")) glLog.upnp = debug2level(val);
	if (!strcmp(name, "main_log")) glLog.main = debug2level(val);
	if (!strcmp(name, "sq2mr_log")) glLog.sq2mr = debug2level(val);
	if (!strcmp(name, "base_mac")) {
		char *p = strtok(val, ":");
		for (i = 0; i < 5; i++) {
			glMac[i] = atol(p) & 0xff;
			p = strtok(NULL, ":");
		}
	}
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


