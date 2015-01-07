#ifndef __MR_UTIL_H
#define __MR_UTIL_H

#include "squeeze2upnp.h"
#include "util_common.h"

void 			MRutilInit(log_level level);
void 			FlushActionList(struct sMR *Device);
void 			InitActionList(struct sMR *Device);
void			QueueAction(sq_dev_handle_t handle, struct sMR *Device, sq_action_t action, u8_t *cookie, void *param, bool sticky);
struct sAction*	UnQueueAction(struct sMR *Device, bool Keep);

void 			FlushMRList(void);
void 			DeleteMR(struct sMR *p);
sq_dev_handle_t mr_GetSqHandle(struct sMR *Device);
struct sMR* 	mr_File2Device(const char *FileName);
struct sMR* 	SID2Device(Upnp_SID Sid);
struct sMR* 	CURL2Device(char *CtrlURL);
struct sMR 		*IsDevice(void *Cookie);
bool 			SetContentType(char *Cap[], sq_seturi_t *uri);

void			SaveConfig(char *name);
void		 	*LoadConfig(char *name, tMRConfig *Conf, sq_dev_param_t *sq_conf);
void	 		*FindMRConfig(void *ref, char *UDN);
void 			*LoadMRConfig(void *ref, char *UDN, tMRConfig *Conf, sq_dev_param_t *sq_conf);
void 			ParseProtocolInfo(struct sMR *Device, char *Info);

#endif
