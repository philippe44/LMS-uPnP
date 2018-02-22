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

#ifndef __UTIL_H
#define __UTIL_H

#include <stdlib.h>
#include <string.h>
#include <stdio.h>

#include "ixml.h" /* for IXML_Document, IXML_Element */
#include "upnp.h" /* for Upnp_EventType */
#include "util_common.h"
#include "log_util.h"

typedef struct {
	pthread_mutex_t	*mutex;
	void (*cleanup)(void*);
	struct sQueue_e {
		struct sQueue_e *next;
		void 			*item;
	} list;
} tQueue;

void 		QueueInit(tQueue *queue, bool mutex, void (*f)(void*));
void 		QueueInsert(tQueue *queue, void *item);
void 		*QueueExtract(tQueue *queue);
void 		QueueFlush(tQueue *queue);

int 		pthread_cond_reltimedwait(pthread_cond_t *cond, pthread_mutex_t *mutex,	u32_t msWait);

char 		*uPNPEvent2String(Upnp_EventType S);
void 		uPNPUtilInit(log_level level);
in_addr_t	ExtractIP(const char *URL);
s64_t	 	Time2Int(char *Time);

char 	   	*XMLGetChangeItem(IXML_Document *doc, char *Tag, char *SearchAttr,
							  char *SearchVal, char *RetAttr);
const char 	*XMLGetLocalName(IXML_Document *doc, int Depth);
IXML_Node  	*XMLAddNode(IXML_Document *doc, IXML_Node *parent, char *name,
						char *fmt, ...);
IXML_Node  	*XMLUpdateNode(IXML_Document *doc, IXML_Node *parent, bool refresh,
						   char *name, char *fmt, ...);
int 	   	XMLAddAttribute(IXML_Document *doc, IXML_Node *parent, char *name,
							char *fmt, ...);
char 	   	*XMLGetFirstDocumentItem(IXML_Document *doc, const char *item);
int 	   	XMLFindAndParseService(IXML_Document *DescDoc, const char *location,
								   const char *serviceTypeBase, char **serviceId,
								   char **serviceType, char **eventURL,
								   char **controlURL, char **serviceURL);
bool 		XMLMatchDocumentItem(IXML_Document *doc, const char *item, const char *s);
bool 		XMLFindAction(const char *base, char *service, char *action);

void 	   	uPNPLogLevel(log_level level);

#if WIN
void  		winsock_init(void);
void		winsock_close(void);
#endif

#endif
