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

#include "upnp.h"
#include "ithread.h"
#include "squeezedefs.h"
#include "squeezeitf.h"
#include "util.h"

/*----------------------------------------------------------------------------*/
/* typedefs */
/*----------------------------------------------------------------------------*/

#define MAX_PROTO		128
#define MAX_RENDERERS	32
#define MAGIC			0xAABBCCDD
#define RESOURCE_LENGTH	250
#define	SCAN_TIMEOUT 	15
#define SCAN_INTERVAL	30

#define	HTTP_DEFAULT_MODE	-3


enum 	eMRstate { UNKNOWN, STOPPED, PLAYING, PAUSED, TRANSITIONING };
enum 	{ AVT_SRV_IDX = 0, REND_SRV_IDX, CNX_MGR_IDX, TOPOLOGY_IDX, GRP_REND_SRV_IDX, NB_SRV };

struct sService {
	char Id			[RESOURCE_LENGTH];
	char Type		[RESOURCE_LENGTH];
	char EventURL	[RESOURCE_LENGTH];
	char ControlURL	[RESOURCE_LENGTH];
	Upnp_SID		SID;
	s32_t			TimeOut;
	u32_t			Failed;
};

typedef struct sMRConfig
{
	char		StreamLength[SQ_STR_LENGTH];
	bool		SeekAfterPause;
	bool		ByteSeek;
	bool		Enabled;
	char		Name[SQ_STR_LENGTH];
	int 		VolumeOnPlay;		// change only volume when playing has started or disable volume commands
	bool		VolumeFeedback;
	bool		AcceptNextURI;
	int			MinGapless;
	bool		SendMetaData;
	bool		SendCoverArt;
	int			MaxVolume;
	char		RawAudioFormat[SQ_STR_LENGTH];
	bool		MatchEndianness;
	bool		AutoPlay;
	bool 		AllowFlac;
	bool		RoonMode;
} tMRConfig;

struct sMR {
	u32_t Magic;
	bool  Running;
	tMRConfig Config;
	sq_dev_param_t	sq_config;
	bool on;
	char friendlyName	[RESOURCE_LENGTH];
	char UDN			[RESOURCE_LENGTH];
	char DescDocURL		[RESOURCE_LENGTH];
	enum eMRstate 	State;
	char			*CurrentURI;
	char			*NextURI;
	bool			GapExpected;
	int				NextDuration;
	char			ProtoInfo[SQ_STR_LENGTH];		// a bit patchy ... used for faulty NEXTURI players
	metadata_t		MetaData;
	sq_action_t		sqState;
	s64_t			Elapsed, Duration;
	bool			ExpectStop;
	u8_t			*seqN;
	void			*WaitCookie, *StartCookie;
	tQueue			ActionQueue;
	unsigned		TrackPoll, StatePoll;
	bool			TimeOut;
	int	 			SqueezeHandle;
	struct sService Service[NB_SRV];
	struct sAction	*Actions;
	pthread_mutex_t Mutex;
	pthread_cond_t	Cond;
	bool			Delete;
	u32_t			Busy;
	pthread_t 		Thread;
	u8_t			Volume;
	bool			Muted;
	char			*ProtocolCap[MAX_PROTO + 1];
	u16_t			ErrorCount;
	u32_t			LastSeen;
};

extern UpnpClient_Handle   	glControlPointHandle;
extern char 				glUPnPSocket[];
extern s32_t				glLogLimit;
extern tMRConfig			glMRConfig;
extern sq_dev_param_t		glDeviceParam;
extern struct sMR			glMRDevices[MAX_RENDERERS];
extern pthread_mutex_t 		glMRMutex;

struct sMR 		*mr_File2Device(const char *FileName);
sq_dev_handle_t	mr_GetSqHandle(struct sMR *Device);
int 			MasterHandler(Upnp_EventType EventType, void *Event, void *Cookie);
int 			ActionHandler(Upnp_EventType EventType, void *Event, void *Cookie);


#endif
