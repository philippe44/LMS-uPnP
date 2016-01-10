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


#include <stdlib.h>
#include <math.h>

#include "upnptools.h"
#include "squeezedefs.h"
#include "util_common.h"
#include "util.h"
#include "avt_util.h"
#include "squeeze2upnp.h"

/*
WARNING
 - ALL THESE FUNCTION MUST BE CALLED WITH MUTEX LOCKED
*/

extern log_level	upnp_loglevel;
static log_level 	*loglevel = &upnp_loglevel;

static char *CreateDIDL(char *URI, char *ProtInfo, struct sq_metadata_s *MetaData, struct sMRConfig *Config);


/*----------------------------------------------------------------------------*/
bool SubmitTransportAction(struct sMR *Device, IXML_Document *ActionNode)
{
	int rc = 0;

	if (!Device->WaitCookie) {
		Device->WaitCookie = Device->seqN++;
		rc = UpnpSendActionAsync(glControlPointHandle, Device->Service[AVT_SRV_IDX].ControlURL,
									 AV_TRANSPORT, NULL, ActionNode, CallbackActionHandler,
									 Device->WaitCookie);

		if (rc != UPNP_E_SUCCESS) {
			LOG_ERROR("Error in UpnpSendActionAsync -- %d", rc);
		}

		ixmlDocument_free(ActionNode);
	}
	else {
		tAction *Action = malloc(sizeof(tAction));
		Action->Device = Device;
		Action->ActionNode = ActionNode;
		QueueInsert(&Device->ActionQueue, Action);
	}

	return (rc == 0);
}


/*----------------------------------------------------------------------------*/
void AVTActionFlush(tQueue *Queue)
{
	tAction *Action;

	while ((Action = QueueExtract(Queue)) != NULL) {
		free(Action);
	}
}

/*----------------------------------------------------------------------------*/
bool AVTSetURI(struct sMR *Device)
{
	IXML_Document *ActionNode = NULL;
	char *DIDLData;

	DIDLData = CreateDIDL(Device->CurrentURI, Device->ProtoInfo, &Device->MetaData, &Device->Config);
	LOG_DEBUG("DIDL header: %s", DIDLData);

	LOG_INFO("uPNP setURI %s for %s (cookie %p)", Device->CurrentURI, Device->Service[AVT_SRV_IDX].ControlURL, Device->seqN);

	if ((ActionNode = UpnpMakeAction("SetAVTransportURI", AV_TRANSPORT, 0, NULL)) == NULL) return false;
	UpnpAddToAction(&ActionNode, "SetAVTransportURI", AV_TRANSPORT, "InstanceID", "0");
	UpnpAddToAction(&ActionNode, "SetAVTransportURI", AV_TRANSPORT, "CurrentURI", Device->CurrentURI);
	UpnpAddToAction(&ActionNode, "SetAVTransportURI", AV_TRANSPORT, "CurrentURIMetaData", DIDLData);
	free(DIDLData);

	return SubmitTransportAction(Device, ActionNode);
}

/*----------------------------------------------------------------------------*/
bool AVTSetNextURI(struct sMR *Device)
{
	IXML_Document *ActionNode = NULL;
	char *DIDLData;

	DIDLData = CreateDIDL(Device->NextURI, Device->ProtoInfo, &Device->MetaData, &Device->Config);
	LOG_DEBUG("DIDL header: %s", DIDLData);

	LOG_INFO("uPNP setNextURI %s for %s (cookie %p)", Device->NextURI, Device->Service[AVT_SRV_IDX].ControlURL, Device->seqN);

	if ((ActionNode = UpnpMakeAction("SetNextAVTransportURI", AV_TRANSPORT, 0, NULL)) == NULL) return false;
	UpnpAddToAction(&ActionNode, "SetNextAVTransportURI", AV_TRANSPORT, "InstanceID", "0");
	UpnpAddToAction(&ActionNode, "SetNextAVTransportURI", AV_TRANSPORT, "NextURI", Device->NextURI);
	UpnpAddToAction(&ActionNode, "SetNextAVTransportURI", AV_TRANSPORT, "NextURIMetaData", DIDLData);
	free(DIDLData);

	return SubmitTransportAction(Device, ActionNode);
}

/*----------------------------------------------------------------------------*/
int AVTCallAction(char *ControlURL, char *Action, void *Cookie)
{
	IXML_Document *ActionNode = NULL;
	int rc;

	LOG_SDEBUG("uPNP %s for %s (cookie %p)", Action, ControlURL, Cookie);

	if ((ActionNode = UpnpMakeAction(Action, AV_TRANSPORT, 0, NULL)) == NULL) return false;
	UpnpAddToAction(&ActionNode, Action, AV_TRANSPORT, "InstanceID", "0");

	rc = UpnpSendActionAsync(glControlPointHandle, ControlURL, AV_TRANSPORT, NULL,
							 ActionNode, CallbackActionHandler, Cookie);

	if (rc != UPNP_E_SUCCESS) LOG_ERROR("Error in UpnpSendActionAsync -- %d", rc);
	ixmlDocument_free(ActionNode);

	return rc;
}


/*----------------------------------------------------------------------------*/
bool AVTPlay(struct sMR *Device)
{
	IXML_Document *ActionNode = NULL;

	LOG_INFO("uPNP play for %s (cookie %p)", Device->Service[AVT_SRV_IDX].ControlURL, Device->seqN);

	if ((ActionNode =  UpnpMakeAction("Play", AV_TRANSPORT, 0, NULL)) == NULL) return false;
	UpnpAddToAction(&ActionNode, "Play", AV_TRANSPORT, "InstanceID", "0");
	UpnpAddToAction(&ActionNode, "Play", AV_TRANSPORT, "Speed", "1");

	return SubmitTransportAction(Device, ActionNode);
}


/*----------------------------------------------------------------------------*/
bool AVTSetPlayMode(struct sMR *Device)
{
	IXML_Document *ActionNode = NULL;

	LOG_INFO("uPNP set play mode for %s (cookie %p)", Device->Service[AVT_SRV_IDX].ControlURL, Device->seqN);
	if ((ActionNode =  UpnpMakeAction("SetPlayMode", AV_TRANSPORT, 0, NULL)) == NULL) return false;;
	UpnpAddToAction(&ActionNode, "SetPlayMode", AV_TRANSPORT, "InstanceID", "0");
	UpnpAddToAction(&ActionNode, "SetPlayMode", AV_TRANSPORT, "NewPlayMode", "NORMAL");

	return SubmitTransportAction(Device, ActionNode);
}


/*----------------------------------------------------------------------------*/
bool AVTSeek(struct sMR *Device, unsigned Interval)
{
	IXML_Document *ActionNode = NULL;
	char	params[128];

	LOG_INFO("uPNP seek for %s (%ds) (cookie %p)", Device->Service[AVT_SRV_IDX].ControlURL, Device->seqN);

	if ((ActionNode =  UpnpMakeAction("Seek", AV_TRANSPORT, 0, NULL)) == NULL) return false;
	UpnpAddToAction(&ActionNode, "Seek", AV_TRANSPORT, "InstanceID", "0");
	sprintf(params, "%d", (int) (Interval / 1000 + 0.5));
	UpnpAddToAction(&ActionNode, "Seek", AV_TRANSPORT, "Unit", params);
	UpnpAddToAction(&ActionNode, "Seek", AV_TRANSPORT, "Target", "REL_TIME");

	return SubmitTransportAction(Device, ActionNode);
}


/*----------------------------------------------------------------------------*/
bool AVTBasic(struct sMR *Device, char *Action)
{
	IXML_Document *ActionNode = NULL;

	LOG_INFO("uPNP %s for %s (cookie %p)", Action, Device->Service[AVT_SRV_IDX].ControlURL, Device->seqN);

	if ((ActionNode = UpnpMakeAction(Action, AV_TRANSPORT, 0, NULL)) == NULL) return false;
	UpnpAddToAction(&ActionNode, Action, AV_TRANSPORT, "InstanceID", "0");

	return SubmitTransportAction(Device, ActionNode);
}


/*----------------------------------------------------------------------------*/
bool AVTStop(struct sMR *Device)
{
	int rc;
	IXML_Document *ActionNode = NULL;

	LOG_INFO("uPNP stop for %s (cookie %p)", Device->Service[AVT_SRV_IDX].ControlURL, Device->seqN);

	if ((ActionNode = UpnpMakeAction("Stop", AV_TRANSPORT, 0, NULL)) == NULL) return false;
	UpnpAddToAction(&ActionNode, "Stop", AV_TRANSPORT, "InstanceID", "0");
	AVTActionFlush(&Device->ActionQueue);

	Device->WaitCookie = Device->seqN++;
	rc = UpnpSendActionAsync(glControlPointHandle, Device->Service[AVT_SRV_IDX].ControlURL,
									 AV_TRANSPORT, NULL, ActionNode, CallbackActionHandler,
									 Device->WaitCookie);

	if (rc != UPNP_E_SUCCESS) {
		LOG_ERROR("Error in UpnpSendActionAsync -- %d", rc);
	}

	ixmlDocument_free(ActionNode);

	return (rc == 0);
}


/*----------------------------------------------------------------------------*/
int CtrlSetVolume(char *ControlURL, u8_t Volume, void *Cookie)
{
	IXML_Document *ActionNode = NULL;
	int rc;
	char params[8];

	LOG_INFO("uPNP volume %d for %s (cookie %p)", Volume, ControlURL, Cookie);
	ActionNode =  UpnpMakeAction("SetVolume", RENDERING_CTRL, 0, NULL);
	UpnpAddToAction(&ActionNode, "SetVolume", RENDERING_CTRL, "InstanceID", "0");
	UpnpAddToAction(&ActionNode, "SetVolume", RENDERING_CTRL, "Channel", "Master");
	sprintf(params, "%d", (int) Volume);
	UpnpAddToAction(&ActionNode, "SetVolume", RENDERING_CTRL, "DesiredVolume", params);

	rc = UpnpSendActionAsync(glControlPointHandle, ControlURL, RENDERING_CTRL, NULL,
							 ActionNode, CallbackActionHandler, Cookie);

	if (rc != UPNP_E_SUCCESS) {
		LOG_ERROR("Error in UpnpSendActionAsync -- %d", rc);
	}

	if (ActionNode) ixmlDocument_free(ActionNode);

	return rc;
}


/*----------------------------------------------------------------------------*/
int CtrlSetMute(char *ControlURL, bool Mute, void *Cookie)
{
	IXML_Document *ActionNode = NULL;
	int rc;

	LOG_INFO("uPNP mute %d for %s (cookie %p)", Mute, ControlURL, Cookie);
	ActionNode =  UpnpMakeAction("SetMute", RENDERING_CTRL, 0, NULL);
	UpnpAddToAction(&ActionNode, "SetMute", RENDERING_CTRL, "InstanceID", "0");
	UpnpAddToAction(&ActionNode, "SetMute", RENDERING_CTRL, "Channel", "Master");
	UpnpAddToAction(&ActionNode, "SetMute", RENDERING_CTRL, "DesiredMute", Mute ? "1" : "0");

	rc = UpnpSendActionAsync(glControlPointHandle, ControlURL, RENDERING_CTRL, NULL,
							 ActionNode, CallbackActionHandler, Cookie);

	if (rc != UPNP_E_SUCCESS) {
		LOG_ERROR("Error in UpnpSendActionAsync -- %d", rc);
	}

	if (ActionNode) ixmlDocument_free(ActionNode);

	return rc;
}


/*----------------------------------------------------------------------------*/
int GetProtocolInfo(char *ControlURL, void *Cookie)
{
	IXML_Document *ActionNode = NULL;
	int rc;

	LOG_DEBUG("uPNP %s GetProtocolInfo (cookie %p)", ControlURL, Cookie);
	ActionNode =  UpnpMakeAction("GetProtocolInfo", CONNECTION_MGR, 0, NULL);

	rc = UpnpSendActionAsync(glControlPointHandle, ControlURL, CONNECTION_MGR, NULL,
							 ActionNode, CallbackEventHandler, Cookie);

	if (rc != UPNP_E_SUCCESS) {
		LOG_ERROR("Error in UpnpSendActionAsync -- %d", rc);
	}

	if (ActionNode) ixmlDocument_free(ActionNode);

	return rc;
}


/*----------------------------------------------------------------------------*/
char *CreateDIDL(char *URI, char *DLNAOptions, struct sq_metadata_s *MetaData, struct sMRConfig *Config)
{
	char *s;

	IXML_Document *doc = ixmlDocument_createDocument();
	IXML_Node	 *node, *root;

	root = XMLAddNode(doc, NULL, "DIDL-Lite", NULL);
	XMLAddAttribute(doc, root, "xmlns:dc", "http://purl.org/dc/elements/1.1/");
	XMLAddAttribute(doc, root, "xmlns:upnp", "urn:schemas-upnp-org:metadata-1-0/upnp/");
	XMLAddAttribute(doc, root, "xmlns", "urn:schemas-upnp-org:metadata-1-0/DIDL-Lite/");
	XMLAddAttribute(doc, root, "xmlns:dlna", "urn:schemas-dlna-org:metadata-1-0/");

	node = XMLAddNode(doc, root, "item", NULL);
	XMLAddAttribute(doc, node, "id", "1");
	XMLAddAttribute(doc, node, "parentID", "0");
	XMLAddAttribute(doc, node, "restricted", "1");

	if (Config->SendMetaData) {
		XMLAddNode(doc, node, "dc:title", MetaData->title);
		XMLAddNode(doc, node, "dc:creator", MetaData->artist);
		XMLAddNode(doc, node, "upnp:genre", MetaData->genre);
	}

	if (MetaData->artwork && Config->SendMetaData)
		XMLAddNode(doc, node, "upnp:albumArtURI", "%s", MetaData->artwork);

	if (MetaData->duration) {
		div_t duration 	= div(MetaData->duration, 1000);

		if (Config->SendMetaData) {
			XMLAddNode(doc, node, "upnp:artist", MetaData->artist);
			XMLAddNode(doc, node, "upnp:album", MetaData->album);
			XMLAddNode(doc, node, "upnp:originalTrackNumber", "%d", MetaData->track);
		}

		XMLAddNode(doc, node, "upnp:class", "object.item.audioItem.musicTrack");
		node = XMLAddNode(doc, node, "res", URI);
		XMLAddAttribute(doc, node, "duration", "%1d:%02d:%02d.%03d",
						duration.quot/3600, (duration.quot % 3600) / 60,
						duration.quot % 60, duration.rem);
	}
	else {
		XMLAddNode(doc, node, "upnp:channelName", MetaData->artist);
		XMLAddNode(doc, node, "upnp:channelNr", "%d", MetaData->track);
		XMLAddNode(doc, node, "upnp:class", "object.item.audioItem.audioBroadcast");
		node = XMLAddNode(doc, node, "res", URI);
	}

	XMLAddAttribute(doc, node, "protocolInfo", DLNAOptions);

	s = ixmlNodetoString((IXML_Node*) doc);

	ixmlDocument_free(doc);

	return s;
}


#if 0
// typical DIDL header

"<DIDL-Lite xmlns:dc=\"http://purl.org/dc/elements/1.1/\" xmlns:upnp=\"urn:schemas-upnp-org:metadata-1-0/upnp/\" xmlns=\"urn:schemas-upnp-org:metadata-1-0/DIDL-Lite/\">"
	"<item id=\"{2148F1D5-1BE6-47C3-81AF-615A960E3704}.0.4\" restricted=\"0\" parentID=\"4\">"
		"<dc:title>Make You Feel My Love</dc:title>"
		"<dc:creator>Adele</dc:creator>"
		"<res size=\"2990984\" duration=\"0:03:32.000\" bitrate=\"14101\" protocolInfo=\"http-get:*:audio/mpeg:DLNA.ORG_PN=MP3;DLNA.ORG_OP=01;DLNA.ORG_FLAGS=01500000000000000000000000000000\" sampleFrequency=\"44100\" bitsPerSample=\"16\" nrAudioChannels=\"2\" microsoft:codec=\"{00000055-0000-0010-8000-00AA00389B71}\" xmlns:microsoft=\"urn:schemas-microsoft-com:WMPNSS-1-0/\">http://192.168.2.10:10243/WMPNSSv4/4053149364/0_ezIxNDhGMUQ1LTFCRTYtNDdDMy04MUFGLTYxNUE5NjBFMzcwNH0uMC40.mp3</res>"
		"<res duration=\"0:03:32.000\" bitrate=\"176400\" protocolInfo=\"http-get:*:audio/L16;rate=44100;channels=2:DLNA.ORG_PN=LPCM;DLNA.ORG_OP=10;DLNA.ORG_CI=1;DLNA.ORG_FLAGS=01500000000000000000000000000000\" sampleFrequency=\"44100\" bitsPerSample=\"16\" nrAudioChannels=\"2\" microsoft:codec=\"{00000001-0000-0010-8000-00AA00389B71}\" xmlns:microsoft=\"urn:schemas-microsoft-com:WMPNSS-1-0/\">http://192.168.2.10:10243/WMPNSSv4/4053149364/ezIxNDhGMUQ1LTFCRTYtNDdDMy04MUFGLTYxNUE5NjBFMzcwNH0uMC40?formatID=20</res>"
		"<res duration=\"0:03:32.000\" bitrate=\"88200\" protocolInfo=\"http-get:*:audio/L16;rate=44100;channels=1:DLNA.ORG_PN=LPCM;DLNA.ORG_OP=10;DLNA.ORG_CI=1;DLNA.ORG_FLAGS=01500000000000000000000000000000\" sampleFrequency=\"44100\" bitsPerSample=\"16\" nrAudioChannels=\"1\" microsoft:codec=\"{00000001-0000-0010-8000-00AA00389B71}\" xmlns:microsoft=\"urn:schemas-microsoft-com:WMPNSS-1-0/\">http://192.168.2.10:10243/WMPNSSv4/4053149364/ezIxNDhGMUQ1LTFCRTYtNDdDMy04MUFGLTYxNUE5NjBFMzcwNH0uMC40?formatID=18</res>"
		"<res duration=\"0:03:32.000\" bitrate=\"16000\" protocolInfo=\"http-get:*:audio/mpeg:DLNA.ORG_PN=MP3;DLNA.ORG_OP=10;DLNA.ORG_CI=1;DLNA.ORG_FLAGS=01500000000000000000000000000000\" sampleFrequency=\"44100\" nrAudioChannels=\"1\" microsoft:codec=\"{00000055-0000-0010-8000-00AA00389B71}\" xmlns:microsoft=\"urn:schemas-microsoft-com:WMPNSS-1-0/\">http://192.168.2.10:10243/WMPNSSv4/4053149364/ezIxNDhGMUQ1LTFCRTYtNDdDMy04MUFGLTYxNUE5NjBFMzcwNH0uMC40.mp3?formatID=24</res>"
		"<res duration=\"0:03:32.000\" bitrate=\"16000\" protocolInfo=\"http-get:*:audio/x-ms-wma:DLNA.ORG_PN=WMABASE;DLNA.ORG_OP=10;DLNA.ORG_CI=1;DLNA.ORG_FLAGS=01500000000000000000000000000000\" sampleFrequency=\"44100\" nrAudioChannels=\"2\" microsoft:codec=\"{00000161-0000-0010-8000-00AA00389B71}\" xmlns:microsoft=\"urn:schemas-microsoft-com:WMPNSS-1-0/\">http://192.168.2.10:10243/WMPNSSv4/4053149364/ezIxNDhGMUQ1LTFCRTYtNDdDMy04MUFGLTYxNUE5NjBFMzcwNH0uMC40.wma?formatID=42</res>"
		"<res duration=\"0:03:32.000\" bitrate=\"6000\" protocolInfo=\"http-get:*:audio/x-ms-wma:DLNA.ORG_PN=WMABASE;DLNA.ORG_OP=10;DLNA.ORG_CI=1;DLNA.ORG_FLAGS=01500000000000000000000000000000\" sampleFrequency=\"44100\" nrAudioChannels=\"1\" microsoft:codec=\"{00000161-0000-0010-8000-00AA00389B71}\" xmlns:microsoft=\"urn:schemas-microsoft-com:WMPNSS-1-0/\">http://192.168.2.10:10243/WMPNSSv4/4053149364/ezIxNDhGMUQ1LTFCRTYtNDdDMy04MUFGLTYxNUE5NjBFMzcwNH0uMC40.wma?formatID=50</res>"
		"<res duration=\"0:03:32.000\" bitrate=\"8000\" protocolInfo=\"http-get:*:audio/x-ms-wma:DLNA.ORG_PN=WMABASE;DLNA.ORG_OP=10;DLNA.ORG_CI=1;DLNA.ORG_FLAGS=01500000000000000000000000000000\" sampleFrequency=\"44100\" nrAudioChannels=\"2\" microsoft:codec=\"{00000161-0000-0010-8000-00AA00389B71}\" xmlns:microsoft=\"urn:schemas-microsoft-com:WMPNSS-1-0/\">http://192.168.2.10:10243/WMPNSSv4/4053149364/ezIxNDhGMUQ1LTFCRTYtNDdDMy04MUFGLTYxNUE5NjBFMzcwNH0uMC40.wma?formatID=54</res>"
		"<upnp:class>object.item.audioItem.musicTrack</upnp:class>"
		"<upnp:genre>[Unknown Genre]</upnp:genre>"
		"<upnp:artist role=\"AlbumArtist\">Adele</upnp:artist>"
		"<upnp:artist role=\"Performer\">Adele</upnp:artist>"
		"<upnp:author role=\"Composer\">[Unknown Composer]</upnp:author>"
		"<upnp:album>19</upnp:album>"
		"<upnp:originalTrackNumber>9</upnp:originalTrackNumber>"
		"<dc:date>2008-01-02</dc:date>"
		"<upnp:actor>Adele</upnp:actor>"
		"<desc id=\"artist\" nameSpace=\"urn:schemas-microsoft-com:WMPNSS-1-0/\" xmlns:microsoft=\"urn:schemas-microsoft-com:WMPNSS-1-0/\">"
			"<microsoft:artistAlbumArtist>Adele</microsoft:artistAlbumArtist>"
			"<microsoft:artistPerformer>Adele</microsoft:artistPerformer>"
		"</desc>"
		"<desc id=\"author\" nameSpace=\"urn:schemas-microsoft-com:WMPNSS-1-0/\" xmlns:microsoft=\"urn:schemas-microsoft-com:WMPNSS-1-0/\">"
			"<microsoft:authorComposer>[Unknown Composer]</microsoft:authorComposer>"
		"</desc>"
		"<desc id=\"Year\" nameSpace=\"urn:schemas-microsoft-com:WMPNSS-1-0/\" xmlns:microsoft=\"urn:schemas-microsoft-com:WMPNSS-1-0/\">"
			"<microsoft:year>2008</microsoft:year>"
		"</desc>"
		"<desc id=\"UserRating\" nameSpace=\"urn:schemas-microsoft-com:WMPNSS-1-0/\" xmlns:microsoft=\"urn:schemas-microsoft-com:WMPNSS-1-0/\">"
			"<microsoft:userEffectiveRatingInStars>3</microsoft:userEffectiveRatingInStars>"
			"<microsoft:userEffectiveRating>50</microsoft:userEffectiveRating>"
		"</desc>"
   "</item>"
"</DIDL-Lite>"
#endif


