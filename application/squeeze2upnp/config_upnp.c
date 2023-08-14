/*
 * AirUPnP - Config utils
 *
 * (c) Philippe, philippe_44@outlook.com
 *
 * see LICENSE
 *
 */

#include <stdarg.h>

#include "squeeze2upnp.h"
#include "ixmlextra.h"
#include "config_upnp.h"
#include "cross_log.h"

extern log_level	slimproto_loglevel;
extern log_level	slimmain_loglevel;
extern log_level	stream_loglevel;
extern log_level	decode_loglevel;
extern log_level	output_loglevel;
extern log_level	main_loglevel;
extern log_level	util_loglevel;
extern log_level	upnp_loglevel;

static void *MigrateConfig(IXML_Document *doc);
static void *MigrateMRConfig(IXML_Node *device);

/*----------------------------------------------------------------------------*/
void SaveConfig(char *name, void *ref, bool full) {
	struct sMR *p;
	IXML_Document *doc = ixmlDocument_createDocument();
	IXML_Document *old_doc = ref;
	IXML_Node *root, *common;
	IXML_Element* old_root = ixmlDocument_getElementById(old_doc, "squeeze2upnp");

	if (!full && old_doc) {
		ixmlDocument_importNode(doc, (IXML_Node*) old_root, true, &root);
		ixmlNode_appendChild((IXML_Node*) doc, root);

		IXML_NodeList* list = ixmlDocument_getElementsByTagName((IXML_Document*) root, "device");
		for (int i = 0; i < (int) ixmlNodeList_length(list); i++) {
			IXML_Node *device = ixmlNodeList_item(list, i);
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

	XMLUpdateNode(doc, root, false, "binding", glBinding);
	XMLUpdateNode(doc, root, false, "custom_discovery", "%s", glCustomDiscovery);
	XMLUpdateNode(doc, root, false, "slimproto_log", level2debug(slimproto_loglevel));
	XMLUpdateNode(doc, root, false, "slimmain_log", level2debug(slimmain_loglevel));
	XMLUpdateNode(doc, root, false, "stream_log", level2debug(stream_loglevel));
	XMLUpdateNode(doc, root, false, "output_log", level2debug(output_loglevel));
	XMLUpdateNode(doc, root, false, "decode_log", level2debug(decode_loglevel));
	XMLUpdateNode(doc, root, false, "main_log",level2debug(main_loglevel));
	XMLUpdateNode(doc, root, false, "upnp_log",level2debug(upnp_loglevel));
	XMLUpdateNode(doc, root, false, "util_log",level2debug(util_loglevel));
	XMLUpdateNode(doc, root, false, "log_limit", "%d", (int32_t) glLogLimit);

	XMLUpdateNode(doc, common, false, "streambuf_size", "%d", (uint32_t) glDeviceParam.streambuf_size);
	XMLUpdateNode(doc, common, false, "output_size", "%d", (uint32_t) glDeviceParam.outputbuf_size);
	XMLUpdateNode(doc, common, false, "stream_length", "%d", (uint32_t) glDeviceParam.stream_length);
	XMLUpdateNode(doc, common, false, "enabled", "%d", (int) glMRConfig.Enabled);
	XMLUpdateNode(doc, common, false, "remove_timeout", "%d", (int) glMRConfig.RemoveTimeout);
	XMLUpdateNode(doc, common, false, "codecs", glDeviceParam.codecs);
	XMLUpdateNode(doc, common, false, "mode", glDeviceParam.mode);
	XMLUpdateNode(doc, common, false, "raw_audio_format", glDeviceParam.raw_audio_format);
	XMLUpdateNode(doc, common, false, "sample_rate", "%d", (int) glDeviceParam.sample_rate);
	XMLUpdateNode(doc, common, false, "L24_format", "%d", (int) glDeviceParam.L24_format);
	XMLUpdateNode(doc, common, false, "flac_header", "%d", (int) glDeviceParam.flac_header);
	XMLUpdateNode(doc, common, false, "roon_mode", "%d", (int) glDeviceParam.roon_mode);
	XMLUpdateNode(doc, common, false, "forced_mimetypes", "%s", glMRConfig.ForcedMimeTypes);
	XMLUpdateNode(doc, common, false, "seek_after_pause", "%d", (int) glMRConfig.SeekAfterPause);
	XMLUpdateNode(doc, common, false, "live_pause", "%d", (int)glMRConfig.LivePause);
	XMLUpdateNode(doc, common, false, "send_icy", "%d", (int) glMRConfig.SendIcy);
	XMLUpdateNode(doc, common, false, "volume_on_play", "%d", (int) glMRConfig.VolumeOnPlay);
	XMLUpdateNode(doc, common, false, "volume_feedback", "%d", (int) glMRConfig.VolumeFeedback);
	XMLUpdateNode(doc, common, false, "send_metadata", "%d", (int) glMRConfig.SendMetaData);
	XMLUpdateNode(doc, common, false, "send_coverart", "%d", (int) glMRConfig.SendCoverArt);
	XMLUpdateNode(doc, common, false, "max_volume", "%d", glMRConfig.MaxVolume);
	XMLUpdateNode(doc, common, false, "accept_nexturi", "%d", (int) glMRConfig.AcceptNextURI);
	XMLUpdateNode(doc, common, false, "next_delay", "%d", (int)glDeviceParam.next_delay);
	XMLUpdateNode(doc, common, false, "auto_play", "%d", (int) glMRConfig.AutoPlay);
	XMLUpdateNode(doc, common, false, "server", glDeviceParam.server);
	XMLUpdateNode(doc, common, false, "coverart", glDeviceParam.coverart);
#ifdef RESAMPLE
	XMLUpdateNode(doc, common, false, "resample_options", glDeviceParam.resample_options);
#endif

	for (int i = 0; i < MAX_RENDERERS; i++) {
		IXML_Node *dev_node;

		if (!glMRDevices[i].Running) continue;
		else p = &glMRDevices[i];

		// existing device, keep param and update "name" if LMS has requested it
		if (old_doc && ((dev_node = (IXML_Node*) FindMRConfig(old_doc, p->UDN)) != NULL)) {
			ixmlDocument_importNode(doc, dev_node, true, &dev_node);
			ixmlNode_appendChild((IXML_Node*) root, dev_node);

			XMLUpdateNode(doc, dev_node, true, "friendly_name", p->friendlyName);
			XMLUpdateNode(doc, dev_node, true, "name", p->sq_config.name);
			if (*p->sq_config.set_server) XMLUpdateNode(doc, dev_node, true, "server", p->sq_config.set_server);
		}
		// new device, add nodes
		else {
			dev_node = XMLAddNode(doc, root, "device", NULL);
			XMLAddNode(doc, dev_node, "udn", p->UDN);
			XMLAddNode(doc, dev_node, "name", p->friendlyName);
			XMLAddNode(doc, dev_node, "friendly_name", p->friendlyName);
			if (*p->sq_config.set_server) XMLAddNode(doc, dev_node, "server", p->sq_config.set_server);
			XMLAddNode(doc, dev_node, "mac", "%02x:%02x:%02x:%02x:%02x:%02x", p->sq_config.mac[0],
						p->sq_config.mac[1], p->sq_config.mac[2], p->sq_config.mac[3], p->sq_config.mac[4], p->sq_config.mac[5]);
			XMLAddNode(doc, dev_node, "enabled", "%d", (int) p->Config.Enabled);
		}
	}

	// add devices in old XML file that has not been discovered
	IXML_NodeList* list = ixmlDocument_getElementsByTagName((IXML_Document*) old_root, "device");
	for (int i = 0; i < (int) ixmlNodeList_length(list); i++) {
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

	FILE* file = fopen(name, "wb");
	char *s = ixmlDocumenttoString(doc);
	fwrite(s, 1, strlen(s), file);
	fclose(file);
	free(s);

	ixmlDocument_free(doc);
}

/*----------------------------------------------------------------------------*/
static void LoadConfigItem(tMRConfig *Conf, sq_dev_param_t *sq_conf, char *name, char *val) {
	if (!val) return;

	if (!strcmp(name, "streambuf_size")) sq_conf->streambuf_size = atol(val);
	if (!strcmp(name, "output_size")) sq_conf->outputbuf_size = atol(val);
	if (!strcmp(name, "stream_length")) sq_conf->stream_length = atol(val);
	if (!strcmp(name, "send_icy")) Conf->SendIcy = atol(val);
	if (!strcmp(name, "enabled")) Conf->Enabled = atol(val);
	if (!strcmp(name, "remove_timeout")) Conf->RemoveTimeout = atol(val);
	if (!strcmp(name, "codecs")) strcpy(sq_conf->codecs, val);
	if (!strcmp(name, "mode")) strcpy(sq_conf->mode, val);
	if (!strcmp(name, "roon_mode")) sq_conf->roon_mode = atol(val);
	if (!strcmp(name, "raw_audio_format")) strcpy(sq_conf->raw_audio_format, val);
	if (!strcmp(name, "store_prefix")) strcpy(sq_conf->store_prefix, val);			//RO
	if (!strcmp(name, "sample_rate")) sq_conf->sample_rate = atol(val);
	if (!strcmp(name, "L24_format")) sq_conf->L24_format = atol(val);
	if (!strcmp(name, "flac_header")) sq_conf->flac_header = atol(val);
	if (!strcmp(name, "forced_mimetypes")) strcpy(Conf->ForcedMimeTypes, val);
	if (!strcmp(name, "seek_after_pause")) Conf->SeekAfterPause = atol(val);
	if (!strcmp(name, "live_pause")) Conf->LivePause = atol(val);
	if (!strcmp(name, "volume_on_play")) Conf->VolumeOnPlay = atol(val);
	if (!strcmp(name, "volume_feedback")) Conf->VolumeFeedback = atol(val);
	if (!strcmp(name, "max_volume")) Conf->MaxVolume = atol(val);
	if (!strcmp(name, "auto_play")) Conf->AutoPlay = atol(val);
	if (!strcmp(name, "accept_nexturi")) Conf->AcceptNextURI = atol(val);
	if (!strcmp(name, "next_delay")) sq_conf->next_delay = atol(val);
	if (!strcmp(name, "send_metadata")) Conf->SendMetaData = atol(val);
	if (!strcmp(name, "send_coverart")) Conf->SendCoverArt = atol(val);
	if (!strcmp(name, "name")) strcpy(sq_conf->name, val);
	if (!strcmp(name, "server")) strcpy(sq_conf->server, val);
	if (!strcmp(name, "coverart")) strcpy(sq_conf->coverart, val);
	if (!strcmp(name, "mac"))  {
		unsigned mac[6];
		int i;
		// seems to be a Windows scanf buf, cannot support %hhx
		sscanf(val,"%2x:%2x:%2x:%2x:%2x:%2x", &mac[0], &mac[1], &mac[2], &mac[3], &mac[4], &mac[5]);
		for (i = 0; i < 6; i++) sq_conf->mac[i] = mac[i];
	}
#ifdef RESAMPLE
	if (!strcmp(name, "resample_options")) strcpy(sq_conf->resample_options, val);
#endif
}

/*----------------------------------------------------------------------------*/
static void LoadGlobalItem(char *name, char *val)
{
	if (!val) return;

	if (!strcmp(name, "binding")) strcpy(glBinding, val);
	if (!strcmp(name, "custom_discovery")) strcpy(glCustomDiscovery, val);
	if (!strcmp(name, "slimproto_log")) slimproto_loglevel = debug2level(val);
	if (!strcmp(name, "slimmain_log")) slimmain_loglevel = debug2level(val);
	if (!strcmp(name, "stream_log")) stream_loglevel = debug2level(val);
	if (!strcmp(name, "output_log")) output_loglevel = debug2level(val);
	if (!strcmp(name, "decode_log")) decode_loglevel = debug2level(val);
	if (!strcmp(name, "main_log")) main_loglevel = debug2level(val);
	if (!strcmp(name, "upnp_log")) upnp_loglevel = debug2level(val);
	if (!strcmp(name, "util_log")) util_loglevel = debug2level(val);
	if (!strcmp(name, "log_limit")) glLogLimit = atol(val);
}

/*----------------------------------------------------------------------------*/
void *FindMRConfig(void *ref, char *UDN) {
	IXML_Node	*device = NULL;
	IXML_Document *doc = (IXML_Document*) ref;
	IXML_Element* elm = ixmlDocument_getElementById(doc, "squeeze2upnp");
	IXML_NodeList* l1_node_list = ixmlDocument_getElementsByTagName((IXML_Document*) elm, "udn");

	for (unsigned i = 0; i < ixmlNodeList_length(l1_node_list); i++) {
		IXML_Node* l1_node = ixmlNodeList_item(l1_node_list, i);
		IXML_Node* l1_1_node = ixmlNode_getFirstChild(l1_node);
		char* v = (char*) ixmlNode_getNodeValue(l1_1_node);
		if (v && !strcmp(v, UDN)) {
			device = ixmlNode_getParentNode(l1_node);
			break;
		}
	}
	if (l1_node_list) ixmlNodeList_free(l1_node_list);
	return device;
}

/*----------------------------------------------------------------------------*/
void* LoadMRConfig(void* ref, char* UDN, tMRConfig * Conf, sq_dev_param_t * sq_conf) {
	IXML_Document *doc = (IXML_Document*) ref;
	IXML_Node* node = (IXML_Node*) FindMRConfig(doc, UDN);

	if (node) {
		IXML_NodeList* node_list = ixmlNode_getChildNodes(node);
		for (unsigned i = 0; i < ixmlNodeList_length(node_list); i++) {
			IXML_Node* l1_node = ixmlNodeList_item(node_list, i);
			char* n = (char*) ixmlNode_getNodeName(l1_node);
			IXML_Node* l1_1_node = ixmlNode_getFirstChild(l1_node);
			char *v = (char*) ixmlNode_getNodeValue(l1_1_node);
			LoadConfigItem(Conf, sq_conf, n, v);
		}
		if (node_list) ixmlNodeList_free(node_list);
	}

	return MigrateMRConfig(node);
}

/*----------------------------------------------------------------------------*/
void *LoadConfig(char *name, tMRConfig *Conf, sq_dev_param_t *sq_conf) {
	IXML_Document* doc = ixmlLoadDocument(name);
	if (!doc) return NULL;

	IXML_Element* elm = ixmlDocument_getElementById(doc, "squeeze2upnp");
	if (elm) {
		IXML_NodeList* l1_node_list = ixmlNode_getChildNodes((IXML_Node*) elm);
		for (unsigned i = 0; i < ixmlNodeList_length(l1_node_list); i++) {
			IXML_Node* l1_node = ixmlNodeList_item(l1_node_list, i);
			char* n = (char*) ixmlNode_getNodeName(l1_node);
			IXML_Node* l1_1_node = ixmlNode_getFirstChild(l1_node);
			char *v = (char*) ixmlNode_getNodeValue(l1_1_node);
			LoadGlobalItem(n, v);
		}
		if (l1_node_list) ixmlNodeList_free(l1_node_list);
	}

	elm = ixmlDocument_getElementById((IXML_Document*) elm, "common");
	if (elm) {
		IXML_NodeList* l1_node_list = ixmlNode_getChildNodes((IXML_Node*) elm);
		for (unsigned i = 0; i < ixmlNodeList_length(l1_node_list); i++) {
			IXML_Node* l1_node = ixmlNodeList_item(l1_node_list, i);
			char* n = (char*) ixmlNode_getNodeName(l1_node);
			IXML_Node* l1_1_node = ixmlNode_getFirstChild(l1_node);
			char *v = (char*) ixmlNode_getNodeValue(l1_1_node);
			LoadConfigItem(&glMRConfig, sq_conf, n, v);
		}
		if (l1_node_list) ixmlNodeList_free(l1_node_list);
	}

	return MigrateConfig(doc);
}

/*---------------------------------------------------------------------------*/
static void *MigrateConfig(IXML_Document *doc) {
	if (!doc) return NULL;

	// change "upnp_socket" into "binding"
	char* value = XMLDelNode((IXML_Node*) doc, "upnp_socket");
	if (value) {
		IXML_Node* node = XMLUpdateNode(doc, (IXML_Node*) doc, false, "binding", "%s", value);
		if (!node) strcpy(glBinding, value);
		free(value);
	}

	return doc;
}


/*---------------------------------------------------------------------------*/
static void *MigrateMRConfig(IXML_Node *device) {
	return device;
}

