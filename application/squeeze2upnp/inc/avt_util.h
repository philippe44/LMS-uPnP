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

#ifndef __AVT_UTIL_H
#define __AVT_UTIL_H

#include "util_common.h"

struct sq_metadata_s;

void AVTInit(log_level level);
int AVTSetURI(char *ControlURL, char *URI, char *ProtocolInfo, struct sq_metadata_s *MetaData, void *Cookie);
int AVTSetNextURI(char *ControlURL, char *URI, char *ProtocolInfo, struct sq_metadata_s *MetaData, void *Cookie);
int AVTCallAction(char *ControlURL, char *Var, void *Cookie);
int AVTPlay(char *ControlURL, void *Cookie);
int SetVolume(char *ControlURL, u8_t Volume, void *Cookie);
int AVTSeek(char *ControlURL, unsigned Interval, void *Cookie);
int AVTBasic(char *ControlURL, char *Action, void *Cookie);
int GetProtocolInfo(char *ControlURL, void *Cookie);

#endif

