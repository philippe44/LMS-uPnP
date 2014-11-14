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

#ifndef __UTIL_H
#define __UTIL_H

#include <stdlib.h>
#include <string.h>
#include <stdio.h>

#include "ithread.h"
#include "ixml.h" /* for IXML_Document, IXML_Element */
#include "upnp.h" /* for Upnp_EventType */
#include "upnptools.h"
#include "util_common.h"

char 		*XMLGetFirstDocumentItem(IXML_Document *doc, const char *item);
int 		XMLFindAndParseService(IXML_Document *DescDoc, const char *location,
							const char *serviceType, char **serviceId,
							char **eventURL, char **controlURL);
char 		*uPNPEvent2String(Upnp_EventType S);
void 		uPNPUtilInit(log_level level);
void 		ExtractIP(const char *URL, in_addr_t *IP);
unsigned 	Time2Int(char *Time);
char 		*XMLGetChangeItem(IXML_Document *doc, char *Item);
IXML_Node 	*XMLAddNode(IXML_Document *doc, IXML_Node *parent, char *name, char *fmt, ...);

void uPNPLogLevel(log_level level);

#endif
