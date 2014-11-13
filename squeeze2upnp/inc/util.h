
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
