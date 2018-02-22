                   /*
 *  Squeeze2upnp - LMS to uPNP gateway
 *
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

#ifndef __MR_UTIL_H
#define __MR_UTIL_H

#include "squeeze2upnp.h"
#include "util_common.h"

void 			FlushMRDevices(void);
void 			DelMRDevice(struct sMR *p);
bool 			isMaster(char *UDN, struct sService *Service, char **Name);
void 			BusyRaise(struct sMR *Device);
void 			BusyDrop(struct sMR *Device);
bool 			CheckAndLock(struct sMR *Device);

struct sMR* 	SID2Device(Upnp_SID Sid);
struct sMR* 	CURL2Device(char *CtrlURL);
struct sMR* 	UDN2Device(char *SID);
struct sService *EventURL2Service(char *URL, struct sService *s);

void 			MakeMacUnique(struct sMR *Device);

char*			MakeProtocolInfo(char *MimeType, u32_t duration);
char** 			ParseProtocolInfo(char *Info, char* Forced);
void 			CheckCodecs(char *Codecs, char *Sink);


#endif
