#ifndef __SQUEEZE2UPNP_H
#define __SQUEEZE2UPNP_H

#include <signal.h>
#include <stdarg.h>
#include <stdio.h>

#include "upnp.h"
#include "UpnpString.h"
#include "upnptools.h"
#include "ithread.h"
#include "squeezedefs.h"
#include "squeezeitf.h"

/*----------------------------------------------------------------------------*/
/* typedefs */
/*----------------------------------------------------------------------------*/

#define MAX_SRV			10
#define MAX_PROTO		32
#define	AV_TRANSPORT 	"urn:schemas-upnp-org:service:AVTransport:1"
#define	RENDERING_CTRL 	"urn:schemas-upnp-org:service:RenderingControl:1"
#define	CONNECTION_MGR 	"urn:schemas-upnp-org:service:ConnectionManager:1"
#define MAGIC			0xAABBCCDD
#define RESOURCE_LENGTH	250

enum eMRstate {STOPPED, PLAYING, PAUSED, TRANSITIONING};
enum {AVT_SRV_IDX = 0, REND_SRV_IDX, CNX_MGR_IDX};

struct sService {
	char Id			[RESOURCE_LENGTH];
	char Type		[RESOURCE_LENGTH];
	char EventURL	[RESOURCE_LENGTH];
	char ControlURL	[RESOURCE_LENGTH];
	Upnp_SID		SID;
	int				TO;
};

typedef struct sMRConfig
{
	int			StreamLength;		// length of the "fake" file
	char 		DidleDuration[32];	// Duration of track in DIDL header
	sq_mode_t	ProcessMode;   		// DIRECT, STREAM, FULL
	bool		NoZeroVolume;		// prevent volume to be set at 0 between tracks
	bool		SeekAfterPause;		// ask for a SEEK after unpause ?
	bool		CanPause;			// pause does not work becase seek does not
	u16_t		VolumeCorrector;	// not yet
	bool		Enabled;			//
	char		Name[SQ_STR_LENGTH];
	bool		ForceVolume;		// force volume after each state detection
	bool		AcceptNextURI;
	char 		VolumeCurve[SQ_STR_LENGTH];
} tMRConfig;

struct sMR {
	u32_t Magic;
	tMRConfig Config;
	sq_dev_param_t	sq_config;
	bool on;
	char UDN			[RESOURCE_LENGTH];
	char DescDocURL		[RESOURCE_LENGTH];
	char FriendlyName	[RESOURCE_LENGTH];
	char PresURL		[RESOURCE_LENGTH];
	char Manufacturer	[RESOURCE_LENGTH];
	in_addr_t ip;
	u8_t mac			[6];
	u8_t macSize;
	enum eMRstate 	State;
	char			*CurrentURI;
	char			*NextURI;
	char			NextProtInfo[SQ_STR_LENGTH];		// a bit patchy ... used for faulty NEXTURI players
	sq_action_t		sqState;
	u32_t			Elapsed;
	u32_t			seqN;
	u32_t			PausedTime;
	unsigned	Stalled;
	unsigned	TrackPoll, StatePoll;
	bool		uPNPTimeOut;
	int	 SqueezeHandle;
	struct sService Service[MAX_SRV];
	struct sAction	*Actions;
	ithread_mutex_t  ActionsMutex;
	ithread_mutex_t  Mutex;
	u8_t		Volume;
	struct {
		u32_t	a;
		u8_t	b;
	} VolumeCurve[32];
	char	*ProtocolCap[MAX_PROTO + 1];
	struct sMR	*NextSQ;
	struct sMR	*Next;
};

/*
struct sMRList {
	struct sMR		*device;
	struct sMRList 	*next;
};
*/

struct sAction	{
	sq_dev_handle_t Handle;
	struct sMR		*Caller;
	sq_action_t 	Action;
	int 			Cookie;
	union {
		double	Volume;
		u32_t	Time;
	} 				Param;
	struct sAction	*Next;
	int				Count;
	bool			Sticky;
};

extern UpnpClient_Handle   	glControlPointHandle;
extern unsigned int 		glPort;
extern char 				glIPaddress[];
extern ithread_mutex_t 	  	glDeviceListMutex;
extern struct sMR 		  	*glDeviceList;
extern struct sMR		 	*glSQ2MRList;
extern u8_t		   			glMac[6];
extern sq_log_level_t		glLog;
extern tMRConfig			glMRConfig;
extern sq_dev_param_t		glDeviceParam;
extern char					glSQServer[SQ_STR_LENGTH];
extern const int			NB_SRV;

struct sMR 		*mr_File2Device(const char *FileName);
sq_dev_handle_t	mr_GetSqHandle(struct sMR *Device);
int 			CallbackEventHandler(Upnp_EventType EventType, void *Event, void *Cookie);
int 			CallbackActionHandler(Upnp_EventType EventType, void *Event, void *Cookie);


#endif
