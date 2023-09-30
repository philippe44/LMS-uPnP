/*
 *  Squeeze2upnp - LMS to uPNP gateway
 *
 *  (c) Adrian Smith 2012-2014, triode1@btinternet.com
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

#ifndef __SQUEEZE2UPNP_H
#define __SQUEEZE2UPNP_H

#include <signal.h>
#include <stdarg.h>
#include <stdio.h>
#include "pthread.h"

#include "upnp.h"
#include "squeezedefs.h"
#include "squeezeitf.h"
#include "cross_util.h"

/*----------------------------------------------------------------------------*/
/* typedefs */
/*----------------------------------------------------------------------------*/

#define MAX_RENDERERS	32
#define MAGIC			0xAABBCCDD
#define RESOURCE_LENGTH	250

enum 	eMRstate { UNKNOWN, STOPPED, PLAYING, PAUSED, TRANSITIONING };
enum 	{ AVT_SRV_IDX = 0, REND_SRV_IDX, CNX_MGR_IDX, TOPOLOGY_IDX, GRP_REND_SRV_IDX, NB_SRV };

struct sService {
	char Id			[RESOURCE_LENGTH];
	char Type		[RESOURCE_LENGTH];
	char EventURL	[RESOURCE_LENGTH];
	char ControlURL	[RESOURCE_LENGTH];
	Upnp_SID		SID;
	int32_t			TimeOut;
	uint32_t		Failed;
};


typedef struct sMRConfig
{
	bool		SeekAfterPause;
	bool		LivePause;
	bool		ByteSeek;
	bool		Enabled;
	int			RemoveTimeout;
	int 		VolumeOnPlay;		// change only volume when playing has started or disable volume commands
	bool		VolumeFeedback;
	int			AcceptNextURI;
	bool		SendMetaData;
	bool		SendCoverArt;
	sq_icy_e	SendIcy;
	int			MaxVolume;
	bool		AutoPlay;
	char		ForcedMimeTypes[STR_LEN];
	char		CustomPattern[STR_LEN * 8];

} tMRConfig;

struct sMR {
	uint32_t Magic;									// just a marker to trace context in memory
	bool  Running;
	tMRConfig Config;
	sq_dev_param_t	sq_config;
	bool on;
	char friendlyName	[RESOURCE_LENGTH];
	char UDN			[RESOURCE_LENGTH];
	char DescDocURL		[RESOURCE_LENGTH];
	enum eMRstate 	State;
	char			*NextURI;                  		// gapped next URI
	char			*NextProtoInfo;					// gapped next protocolInfo
	metadata_t		NextMetaData;					// gapped next metadata
	char			*ExpectedURI;					// to detect track change
	int32_t			Duration;       			 	// for players that don't report end of track (Bose)
	uint32_t 		ElapsedLast, ElapsedOffset;     // for players that reset counter on icy changes
	bool			ShortTrack;    					// current or next track is short
	int16_t			ShortTrackWait;					// stop timeout when short track is last track
	sq_action_t		sqState;
	uint8_t			*seqN;
	void			*WaitCookie, *StartCookie;
	cross_queue_t	ActionQueue;
	unsigned		TrackPoll, StatePoll;
	int				InfoExPoll;
	int	 			SqueezeHandle;
	struct sService Service[NB_SRV];
	struct sAction	*Actions;
	struct sMR		*Master;
	pthread_mutex_t Mutex;
	pthread_t 		Thread;
	double			Volume;
	bool			Muted;
	uint32_t		VolumeStampRx, VolumeStampTx;	// timestamps to filter volume loopbacks
	int				ErrorCount;                     // UPnP protocol error count, negative means fatal error
	uint32_t		LastSeen;						// presence timeout for player which went dark
	char			*Sink;
};

extern UpnpClient_Handle   	glControlPointHandle;
extern char					glCustomDiscovery[];
extern char 				glBinding[];
extern int32_t				glLogLimit;
extern tMRConfig			glMRConfig;
extern sq_dev_param_t		glDeviceParam;
extern struct sMR			glMRDevices[MAX_RENDERERS];
extern pthread_mutex_t 		glMRMutex;

int MasterHandler(Upnp_EventType EventType, const void* Event, void* Cookie);
int ActionHandler(Upnp_EventType EventType, const void* Event, void* Cookie);

#endif
