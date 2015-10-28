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
TODO
 - build the DIDLE string with a proper XML constructor rather than string tinkering
*/

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
DLNA options, that *might* be the only ones or might be added to other options
so the ; might have to be removed
*/

/*
static char DLNA_OPT[] = ";DLNA.ORG_OP=01;DLNA.ORG_FLAGS=01700000000000000000000000000000";
*/

static log_level loglevel;
static char *CreateDIDL(char *URI, char *ProtInfo, struct sq_metadata_s *MetaData, struct sMRConfig *Config);

/*----------------------------------------------------------------------------*/
void AVTInit(log_level level)
{
	loglevel = level;
}

/*----------------------------------------------------------------------------*/
int AVTSetURI(char *ControlURL, char *URI, char *ProtInfo, struct sq_metadata_s *MetaData, struct sMRConfig *Config, void *Cookie)
{
	IXML_Document *ActionNode = NULL;
	int rc;
	char *DIDLData;

	DIDLData = CreateDIDL(URI, ProtInfo, MetaData, Config);
	LOG_DEBUG("DIDL header: %s", DIDLData);

	LOG_INFO("uPNP setURI %s for %s (cookie %p)", URI, ControlURL, Cookie);
	ActionNode =  UpnpMakeAction("SetAVTransportURI", AV_TRANSPORT, 0, NULL);
	UpnpAddToAction(&ActionNode, "SetAVTransportURI", AV_TRANSPORT, "InstanceID", "0");
	UpnpAddToAction(&ActionNode, "SetAVTransportURI", AV_TRANSPORT, "CurrentURI", URI);
	UpnpAddToAction(&ActionNode, "SetAVTransportURI", AV_TRANSPORT, "CurrentURIMetaData", DIDLData);

	rc = UpnpSendActionAsync(glControlPointHandle, ControlURL, AV_TRANSPORT, NULL,
							 ActionNode, CallbackActionHandler, Cookie);

	if (rc != UPNP_E_SUCCESS) {
		LOG_ERROR("Error in UpnpSendActionAsync -- %d", rc);
	}

	free(DIDLData);
	if (ActionNode) ixmlDocument_free(ActionNode);

	return rc;
}

/*----------------------------------------------------------------------------*/
int AVTSetNextURI(char *ControlURL, char *URI, char *ProtInfo, struct sq_metadata_s *MetaData, struct sMRConfig *Config, void *Cookie)
{
	IXML_Document *ActionNode = NULL;
	int rc;
	char *DIDLData;

	DIDLData = CreateDIDL(URI, ProtInfo, MetaData, Config);
	LOG_DEBUG("DIDL header: %s", DIDLData);

	LOG_INFO("uPNP setNextURI %s for %s (cookie %p)", URI, ControlURL, Cookie);
	ActionNode =  UpnpMakeAction("SetNextAVTransportURI", AV_TRANSPORT, 0, NULL);
	UpnpAddToAction(&ActionNode, "SetNextAVTransportURI", AV_TRANSPORT, "InstanceID", "0");
	UpnpAddToAction(&ActionNode, "SetNextAVTransportURI", AV_TRANSPORT, "NextURI", URI);
	UpnpAddToAction(&ActionNode, "SetNextAVTransportURI", AV_TRANSPORT, "NextURIMetaData", DIDLData);

	rc = UpnpSendActionAsync(glControlPointHandle, ControlURL, AV_TRANSPORT, NULL,
							 ActionNode, CallbackActionHandler, Cookie);

	if (rc != UPNP_E_SUCCESS) {
		LOG_ERROR("Error in UpnpSendActionAsync -- %d", rc);
	}

	free(DIDLData);
	if (ActionNode) ixmlDocument_free(ActionNode);

	return rc;
}

/*----------------------------------------------------------------------------*/
int AVTCallAction(char *ControlURL, char *Action, void *Cookie)
{
	IXML_Document *ActionNode = NULL;
	int rc;

	LOG_SDEBUG("uPNP %s for %s (cookie %p)", Action, ControlURL, Cookie);
	ActionNode =  UpnpMakeAction(Action, AV_TRANSPORT, 0, NULL);
	UpnpAddToAction(&ActionNode, Action, AV_TRANSPORT, "InstanceID", "0");

	rc = UpnpSendActionAsync(glControlPointHandle, ControlURL, AV_TRANSPORT, NULL,
							 ActionNode, CallbackActionHandler, Cookie);

	if (rc != UPNP_E_SUCCESS) LOG_ERROR("Error in UpnpSendActionAsync -- %d", rc);

	if (ActionNode) ixmlDocument_free(ActionNode);

	return rc;
}


/*----------------------------------------------------------------------------*/
int AVTPlay(char *ControlURL, void *Cookie)
{
	IXML_Document *ActionNode = NULL;
	int rc;

	LOG_INFO("uPNP play for %s (cookie %p)", ControlURL, Cookie);
	ActionNode =  UpnpMakeAction("Play", AV_TRANSPORT, 0, NULL);
	UpnpAddToAction(&ActionNode, "Play", AV_TRANSPORT, "InstanceID", "0");
	UpnpAddToAction(&ActionNode, "Play", AV_TRANSPORT, "Speed", "1");

	rc = UpnpSendActionAsync(glControlPointHandle, ControlURL, AV_TRANSPORT, NULL,
							 ActionNode, CallbackActionHandler, Cookie);

	if (rc != UPNP_E_SUCCESS) {
		LOG_ERROR("Error in UpnpSendActionAsync -- %d", rc);
	}

	if (ActionNode) ixmlDocument_free(ActionNode);

	return rc;
}

/*----------------------------------------------------------------------------*/
int AVTSetPlayMode(char *ControlURL, void *Cookie)
{
	IXML_Document *ActionNode = NULL;
	int rc;

	LOG_INFO("uPNP set play mode for %s (cookie %p)", ControlURL, Cookie);
	ActionNode =  UpnpMakeAction("SetPlayMode", AV_TRANSPORT, 0, NULL);
	UpnpAddToAction(&ActionNode, "SetPlayMode", AV_TRANSPORT, "InstanceID", "0");
	UpnpAddToAction(&ActionNode, "SetPlayMode", AV_TRANSPORT, "NewPlayMode", "NORMAL");

	rc = UpnpSendActionAsync(glControlPointHandle, ControlURL, AV_TRANSPORT, NULL,
							 ActionNode, CallbackActionHandler, Cookie);

	if (rc != UPNP_E_SUCCESS) {
		LOG_ERROR("Error in UpnpSendActionAsync -- %d", rc);
	}

	if (ActionNode) ixmlDocument_free(ActionNode);

	return rc;
}

/*----------------------------------------------------------------------------*/
int SetVolume(char *ControlURL, u8_t Volume, void *Cookie)
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
int GetVolume(char *ControlURL, void *Cookie)
{
	IXML_Document *ActionNode = NULL;
	int rc;

	LOG_SDEBUG("uPNP get volume for %s (cookie %p)", ControlURL, Cookie);
	ActionNode =  UpnpMakeAction("GetVolume", RENDERING_CTRL, 0, NULL);
	UpnpAddToAction(&ActionNode, "GetVolume", RENDERING_CTRL, "InstanceID", "0");
	UpnpAddToAction(&ActionNode, "GetVolume", RENDERING_CTRL, "Channel", "Master");

	rc = UpnpSendActionAsync(glControlPointHandle, ControlURL, RENDERING_CTRL, NULL,
							 ActionNode, CallbackActionHandler, Cookie);

	if (rc != UPNP_E_SUCCESS) {
		LOG_ERROR("Error in UpnpSendActionAsync -- %d", rc);
	}

	if (ActionNode) ixmlDocument_free(ActionNode);

	return rc;
}



/*----------------------------------------------------------------------------*/
int AVTSeek(char *ControlURL, unsigned Interval, void *Cookie)
{
	IXML_Document *ActionNode = NULL;
	int rc;
	char	params[128];

	LOG_INFO("uPNP seek for %s (%ds) (cookie %p)", ControlURL, Interval, Cookie);
	ActionNode =  UpnpMakeAction("Seek", AV_TRANSPORT, 0, NULL);
	UpnpAddToAction(&ActionNode, "Seek", AV_TRANSPORT, "InstanceID", "0");
	sprintf(params, "%d", (int) (Interval / 1000 + 0.5));
	UpnpAddToAction(&ActionNode, "Seek", AV_TRANSPORT, "Unit", params);
	UpnpAddToAction(&ActionNode, "Seek", AV_TRANSPORT, "Target", "REL_TIME");

	rc = UpnpSendActionAsync(glControlPointHandle, ControlURL, AV_TRANSPORT, NULL,
							 ActionNode, CallbackActionHandler, Cookie);

	if (rc != UPNP_E_SUCCESS) {
		LOG_ERROR("Error in UpnpSendActionAsync -- %d", rc);
	}

	if (ActionNode) ixmlDocument_free(ActionNode);

	return rc;
}

/*----------------------------------------------------------------------------*/
int AVTBasic(char *ControlURL, char *Action, void *Cookie)
{
	IXML_Document *ActionNode = NULL;
	int rc;

	LOG_INFO("uPNP %s for %s (cookie %p)", Action, ControlURL, Cookie);
	ActionNode =  UpnpMakeAction(Action, AV_TRANSPORT, 0, NULL);
	UpnpAddToAction(&ActionNode, Action, AV_TRANSPORT, "InstanceID", "0");

	rc = UpnpSendActionAsync(glControlPointHandle, ControlURL, AV_TRANSPORT, NULL,
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
char *CreateDIDL(char *URI, char *ProtInfo, struct sq_metadata_s *MetaData, struct sMRConfig *Config)
{
	char *s;
	u32_t Sinc = 0;
	char DLNAOpt[128];

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
	XMLAddNode(doc, node, "dc:title", MetaData->title);
	XMLAddNode(doc, node, "dc:creator", MetaData->artist);
	XMLAddNode(doc, node, "upnp:genre", MetaData->genre);

	if (MetaData->artwork)
		XMLAddNode(doc, node, "upnp:albumArtURI", "%s", MetaData->artwork);

	if (MetaData->duration) {
		div_t duration 	= div(MetaData->duration, 1000);

		XMLAddNode(doc, node, "upnp:artist", MetaData->artist);
		XMLAddNode(doc, node, "upnp:album", MetaData->album);
		XMLAddNode(doc, node, "upnp:originalTrackNumber", "%d", MetaData->track);
		XMLAddNode(doc, node, "upnp:class", "object.item.audioItem.musicTrack");
		node = XMLAddNode(doc, node, "res", URI);
		XMLAddAttribute(doc, node, "duration", "%1d:%02d:%02d.%03d",
						duration.quot/3600, (duration.quot % 3600) / 60,
						duration.quot % 60, duration.rem);
	}
	else {
		Sinc = DLNA_ORG_FLAG_SN_INCREASE;
		XMLAddNode(doc, node, "upnp:channelName", MetaData->artist);
		XMLAddNode(doc, node, "upnp:channelNr", "%d", MetaData->track);
		XMLAddNode(doc, node, "upnp:class", "object.item.audioItem.audioBroadcast");
		node = XMLAddNode(doc, node, "res", URI);
	}

	if (Config->ByteSeek)
		sprintf(DLNAOpt, ";DLNA.ORG_OP=01;DLNA.ORG_CI=0;DLNA.ORG_FLAGS=%08x000000000000000000000000",
						  DLNA_ORG_FLAG | Sinc);
	else
		sprintf(DLNAOpt, ";DLNA.ORG_CI=0;DLNA.ORG_FLAGS=%08x000000000000000000000000",
						  DLNA_ORG_FLAG | Sinc);

	if (ProtInfo[strlen(ProtInfo) - 1] == ':')
		XMLAddAttribute(doc, node, "protocolInfo", "%s%s", ProtInfo, DLNAOpt + 1);
	else
		XMLAddAttribute(doc, node, "protocolInfo", "%s%s", ProtInfo, DLNAOpt);

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

// typical ProtocolInfo

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
#endif

