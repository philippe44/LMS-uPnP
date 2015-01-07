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

#include <signal.h>
#include <stdarg.h>
#include <stdio.h>

#include "util_common.h"
#include "util.h"

#ifndef __WEBSERVER_H
#define __WEBSERVER_H


int WebGetInfo(const char *FileName, struct File_Info *Info);
int WebSetExtraHeaders(const char *FileName, struct Extra_Headers *Headers);
UpnpWebFileHandle WebOpen(const char *FileName, enum UpnpOpenFileMode Mode);
int WebRead(UpnpWebFileHandle FileHandle, char *buf, size_t buflen);
int WebWrite(UpnpWebFileHandle FileHandle, char *buf, size_t buflen);
int WebSeek(UpnpWebFileHandle FileHandle, off_t offset, int origin);
int WebClose(UpnpWebFileHandle FileHandle);
void WebServerLogLevel(log_level level);

extern char glBaseVDIR[];

#endif
