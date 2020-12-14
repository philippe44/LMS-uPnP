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

#include <stdarg.h>

#include "squeezedefs.h"
#include "util.h"
#include "util_common.h"
#include "upnptools.h"

/*----------------------------------------------------------------------------*/
/* globals */
/*----------------------------------------------------------------------------*/

/*----------------------------------------------------------------------------*/
/* locals */
/*----------------------------------------------------------------------------*/
extern log_level util_loglevel;
static log_level *loglevel = &util_loglevel;
static pthread_mutex_t	wakeMutex;
static pthread_cond_t	wakeCond;

static IXML_Node *_getAttributeNode(IXML_Node *node, char *SearchAttr);


/*----------------------------------------------------------------------------*/
void InitUtils(void)
{
	pthread_mutex_init(&wakeMutex, 0);
	pthread_cond_init(&wakeCond, 0);
}


/*----------------------------------------------------------------------------*/
void EndUtils(void)
{
	pthread_mutex_destroy(&wakeMutex);
	pthread_cond_destroy(&wakeCond);
}


/*----------------------------------------------------------------------------*/
/* 																			  */
/* system-wide sleep & wakeup												  */
/* 																			  */
/*----------------------------------------------------------------------------*/

/*----------------------------------------------------------------------------*/
void WakeableSleep(u32_t ms) {
	pthread_mutex_lock(&wakeMutex);
	if (ms) pthread_cond_reltimedwait(&wakeCond, &wakeMutex, ms);
	else pthread_cond_wait(&wakeCond, &wakeMutex);
	pthread_mutex_unlock(&wakeMutex);
}

/*----------------------------------------------------------------------------*/
void WakeAll(void) {
	pthread_mutex_lock(&wakeMutex);
	pthread_cond_broadcast(&wakeCond);
	pthread_mutex_unlock(&wakeMutex);
}



#if WIN
/*----------------------------------------------------------------------------*/
void winsock_init(void) {
	WSADATA wsaData;
	WORD wVersionRequested = MAKEWORD(2, 2);
	int WSerr = WSAStartup(wVersionRequested, &wsaData);
	if (WSerr != 0) {
		LOG_ERROR("Bad winsock version", NULL);
		exit(1);
	}
}


/*----------------------------------------------------------------------------*/
void winsock_close(void) {
	WSACleanup();
}
#endif

/*----------------------------------------------------------------------------*/
/* 																			  */
/* pthread utils															  */
/* 																			  */
/*----------------------------------------------------------------------------*/

/*----------------------------------------------------------------------------*/
int pthread_cond_reltimedwait(pthread_cond_t *cond, pthread_mutex_t *mutex, u32_t msWait)
{
	struct timespec ts;
	u32_t	nsec;
#if OSX || SUNOS
	struct timeval tv;
#endif

#if WIN
	struct _timeb SysTime;

	_ftime(&SysTime);
	ts.tv_sec = (long) SysTime.time;
	ts.tv_nsec = 1000000 * SysTime.millitm;
#elif LINUX || FREEBSD
	clock_gettime(CLOCK_REALTIME, &ts);
#elif OSX || SUNOS
	gettimeofday(&tv, NULL);
	ts.tv_sec = (long) tv.tv_sec;
	ts.tv_nsec = 1000L * tv.tv_usec;
#endif

	if (!msWait) return pthread_cond_wait(cond, mutex);

	nsec = ts.tv_nsec + (msWait % 1000) * 1000000;
	ts.tv_sec += msWait / 1000 + (nsec / 1000000000);
	ts.tv_nsec = nsec % 1000000000;

	return pthread_cond_timedwait(cond, mutex, &ts);
}


/*----------------------------------------------------------------------------*/
in_addr_t ExtractIP(const char *URL)
{
	char *p1, ip[32];

	sscanf(URL, "http://%31s", ip);

	ip[31] = '\0';
	p1 = strchr(ip, ':');
	if (p1) *p1 = '\0';

	return inet_addr(ip);;
}


/*----------------------------------------------------------------------------*/
char *XMLGetFirstDocumentItem(IXML_Document *doc, const char *item, bool strict)
{
	IXML_NodeList *nodeList = NULL;
	IXML_Node *textNode = NULL;
	IXML_Node *tmpNode = NULL;
	char *ret = NULL;
	int i;

	nodeList = ixmlDocument_getElementsByTagName(doc, (char *)item);

	for (i = 0; nodeList && i < (int) ixmlNodeList_length(nodeList); i++) {
		tmpNode = ixmlNodeList_item(nodeList, i);

		if (tmpNode) {
			textNode = ixmlNode_getFirstChild(tmpNode);
			if (textNode && (ret = (char*) ixmlNode_getNodeValue(textNode)) != NULL) {
				ret = strdup(ret);
				break;
			} else {
				LOG_WARN("ixmlNode_getFirstChild(tmpNode) returned %p %p", textNode, ret);
			}
		} else {
			LOG_WARN("ixmlNodeList_item(nodeList, %d) returned NULL", i);
		}

		if (strict) break;
	}

	if (nodeList) {
		ixmlNodeList_free(nodeList);
    } else {
		LOG_SDEBUG("Error finding %s in XML Node", item);
    }

	return ret;
}

/*----------------------------------------------------------------------------*/
char *XMLGetFirstElementItem(IXML_Element *element, const char *item)
{
	IXML_NodeList *nodeList = NULL;
	IXML_Node *textNode = NULL;
	IXML_Node *tmpNode = NULL;
	char *ret = NULL;

	nodeList = ixmlElement_getElementsByTagName(element, (char *)item);
	if (nodeList == NULL) {
		LOG_WARN("Error finding %s in XML Node", item);
		return NULL;
	}
	tmpNode = ixmlNodeList_item(nodeList, 0);
	if (!tmpNode) {
		LOG_WARN("Error finding %s value in XML Node", item);
		ixmlNodeList_free(nodeList);
		return NULL;
	}
	textNode = ixmlNode_getFirstChild(tmpNode);
	ret = strdup(ixmlNode_getNodeValue(textNode));
	if (!ret) {
		LOG_ERROR("Error allocating memory for %s in XML Node",item);
		ixmlNodeList_free(nodeList);
		return NULL;
	}
	ixmlNodeList_free(nodeList);

	return ret;
}

/*----------------------------------------------------------------------------*/
bool XMLFindAction(const char *base, char *service, char *action) {
	char *url = malloc(strlen(base) + strlen(service) + 1);
	IXML_Document *AVTDoc = NULL;
	bool res = false;

	UpnpResolveURL(base, service, url);

	if (UpnpDownloadXmlDoc(url, &AVTDoc) == UPNP_E_SUCCESS) {
		IXML_Element *actions = ixmlDocument_getElementById(AVTDoc, "actionList");
		IXML_NodeList *actionList = ixmlDocument_getElementsByTagName((IXML_Document*) actions, "action");
		int i;

		for (i = 0; actionList && i < (int) ixmlNodeList_length(actionList); i++) {
			IXML_Node *node = ixmlNodeList_item(actionList, i);
			const char *name;
			node = (IXML_Node*) ixmlDocument_getElementById((IXML_Document*) node, "name");
			node = ixmlNode_getFirstChild(node);
			name = ixmlNode_getNodeValue(node);
			if (name && !strcasecmp(name, action)) {
				res = true;
				break;
			}
		}
		ixmlNodeList_free(actionList);
	}

	free(url);
	ixmlDocument_free(AVTDoc);

	return res;
}

/*----------------------------------------------------------------------------*/
static IXML_NodeList *XMLGetNthServiceList(IXML_Document *doc, unsigned int n, bool *contd)
{
	IXML_NodeList *ServiceList = NULL;
	IXML_NodeList *servlistnodelist = NULL;
	IXML_Node *servlistnode = NULL;
	*contd = false;

	/*  ixmlDocument_getElementsByTagName()
	 *  Returns a NodeList of all Elements that match the given
	 *  tag name in the order in which they were encountered in a preorder
	 *  traversal of the Document tree.
	 *
	 *  return (NodeList*) A pointer to a NodeList containing the
	 *                      matching items or NULL on an error. 	 */
	LOG_SDEBUG("GetNthServiceList called : n = %d", n);
	servlistnodelist = ixmlDocument_getElementsByTagName(doc, "serviceList");
	if (servlistnodelist &&
		ixmlNodeList_length(servlistnodelist) &&
		n < ixmlNodeList_length(servlistnodelist)) {
		/* Retrieves a Node from a NodeList} specified by a
		 *  numerical index.
		 *
		 *  return (Node*) A pointer to a Node or NULL if there was an
		 *                  error. */
		servlistnode = ixmlNodeList_item(servlistnodelist, n);
		if (servlistnode) {
			/* create as list of DOM nodes */
			ServiceList = ixmlElement_getElementsByTagName(
				(IXML_Element *)servlistnode, "service");
			*contd = true;
		} else
			LOG_WARN("ixmlNodeList_item(nodeList, n) returned NULL", NULL);
	}
	if (servlistnodelist)
		ixmlNodeList_free(servlistnodelist);

	return ServiceList;
}

/*----------------------------------------------------------------------------*/
int XMLFindAndParseService(IXML_Document *DescDoc, const char *location,
	const char *serviceTypeBase, char **serviceType, char **serviceId, char **eventURL, char **controlURL, char **serviceURL)
{
	unsigned int i;
	unsigned long length;
	int found = 0;
	int ret;
	unsigned int sindex = 0;
	char *tempServiceType = NULL;
	char *baseURL = NULL;
	const char *base = NULL;
	char *relcontrolURL = NULL;
	char *releventURL = NULL;
	IXML_NodeList *serviceList = NULL;
	IXML_Element *service = NULL;
	bool contd = true;

	baseURL = XMLGetFirstDocumentItem(DescDoc, "URLBase", true);
	if (baseURL) base = baseURL;
	else base = location;

	for (sindex = 0; contd; sindex++) {
		tempServiceType = NULL;
		relcontrolURL = NULL;
		releventURL = NULL;
		service = NULL;

		if ((serviceList = XMLGetNthServiceList(DescDoc , sindex, &contd)) == NULL) continue;
		length = ixmlNodeList_length(serviceList);
		for (i = 0; i < length; i++) {
			service = (IXML_Element *)ixmlNodeList_item(serviceList, i);
			tempServiceType = XMLGetFirstElementItem((IXML_Element *)service, "serviceType");
			LOG_SDEBUG("serviceType %s", tempServiceType);

			// remove version from service type
			*strrchr(tempServiceType, ':') = '\0';
			if (tempServiceType && strcmp(tempServiceType, serviceTypeBase) == 0) {
				NFREE(*serviceURL);
				*serviceURL = XMLGetFirstElementItem((IXML_Element *)service, "SCPDURL");
				NFREE(*serviceType);
				*serviceType = XMLGetFirstElementItem((IXML_Element *)service, "serviceType");
				NFREE(*serviceId);
				*serviceId = XMLGetFirstElementItem(service, "serviceId");
				LOG_SDEBUG("Service %s, serviceId: %s", serviceType, *serviceId);
				relcontrolURL = XMLGetFirstElementItem(service, "controlURL");
				releventURL = XMLGetFirstElementItem(service, "eventSubURL");
				NFREE(*controlURL);
				*controlURL = (char*) malloc(strlen(base) + strlen(relcontrolURL) + 1);
				if (*controlURL) {
					ret = UpnpResolveURL(base, relcontrolURL, *controlURL);
					if (ret != UPNP_E_SUCCESS) LOG_ERROR("Error generating controlURL from %s + %s", base, relcontrolURL);
				}
				NFREE(*eventURL);
				*eventURL = (char*) malloc(strlen(base) + strlen(releventURL) + 1);
				if (*eventURL) {
					ret = UpnpResolveURL(base, releventURL, *eventURL);
					if (ret != UPNP_E_SUCCESS) LOG_ERROR("Error generating eventURL from %s + %s", base, releventURL);
				}
				free(relcontrolURL);
				free(releventURL);
				relcontrolURL = NULL;
				releventURL = NULL;
				found = 1;
				break;
			}
			free(tempServiceType);
			tempServiceType = NULL;
		}
		free(tempServiceType);
		tempServiceType = NULL;
		if (serviceList) ixmlNodeList_free(serviceList);
		serviceList = NULL;
	}

	free(baseURL);

	return found;
}


/*----------------------------------------------------------------------------*/
char *XMLGetChangeItem(IXML_Document *doc, char *Tag, char *SearchAttr, char *SearchVal, char *RetAttr)
{
	IXML_Node *node;
	IXML_Document *ItemDoc;
	IXML_Element *LastChange;
	IXML_NodeList *List;
	char *buf, *ret = NULL;
	u32_t i;

	LastChange = ixmlDocument_getElementById(doc, "LastChange");
	if (!LastChange) return NULL;

	node = ixmlNode_getFirstChild((IXML_Node*) LastChange);
	if (!node) return NULL;

	buf = (char*) ixmlNode_getNodeValue(node);
	if (!buf) return NULL;

	ItemDoc = ixmlParseBuffer(buf);
	if (!ItemDoc) return NULL;

	List = ixmlDocument_getElementsByTagName(ItemDoc, Tag);
	if (!List) {
		ixmlDocument_free(ItemDoc);
		return NULL;
	}

	for (i = 0; i < ixmlNodeList_length(List); i++) {
		IXML_Node *node = ixmlNodeList_item(List, i);
		IXML_Node *attr = _getAttributeNode(node, SearchAttr);

		if (!attr) continue;

		if (!strcasecmp(ixmlNode_getNodeValue(attr), SearchVal)) {
			if ((node = ixmlNode_getNextSibling(attr)) == NULL)
				if ((node = ixmlNode_getPreviousSibling(attr)) == NULL) continue;
			if (!strcasecmp(ixmlNode_getNodeName(node), "val")) {
				ret = strdup(ixmlNode_getNodeValue(node));
				break;
			}
		}
	}

	ixmlNodeList_free(List);
	ixmlDocument_free(ItemDoc);

	return ret;
}


/*----------------------------------------------------------------------------*/
const char *XMLGetLocalName(IXML_Document *doc, int Depth)
{
	IXML_Node *node = (IXML_Node*) doc;

	while (Depth--) {
		node = ixmlNode_getFirstChild(node);
		if (!node) return NULL;
	}

	return ixmlNode_getLocalName(node);
}


/*----------------------------------------------------------------------------*/
IXML_Node *_getAttributeNode(IXML_Node *node, char *SearchAttr)
{
	IXML_Node *ret;
	IXML_NamedNodeMap *map = ixmlNode_getAttributes(node);
	int i;

	/*
	supposed to act like but case insensitive
	ixmlElement_getAttributeNode((IXML_Element*) node, SearchAttr);
	*/

	for (i = 0; i < ixmlNamedNodeMap_getLength(map); i++) {
		ret = ixmlNamedNodeMap_item(map, i);
		if (strcasecmp(ixmlNode_getNodeName(ret), SearchAttr)) ret = NULL;
		else break;
	}

	ixmlNamedNodeMap_free(map);

	return ret;
}


/*----------------------------------------------------------------------------*/
IXML_Node *XMLAddNode(IXML_Document *doc, IXML_Node *parent, char *name, char *fmt, ...)
{
	IXML_Node *node, *elm;

	char buf[256];
	va_list args;

	elm = (IXML_Node*) ixmlDocument_createElement(doc, name);
	if (parent) ixmlNode_appendChild(parent, elm);
	else ixmlNode_appendChild((IXML_Node*) doc, elm);

	if (fmt) {
		va_start(args, fmt);

		vsprintf(buf, fmt, args);
		node = ixmlDocument_createTextNode(doc, buf);
		ixmlNode_appendChild(elm, node);

		va_end(args);
	}

	return elm;
}


/*----------------------------------------------------------------------------*/
IXML_Node *XMLUpdateNode(IXML_Document *doc, IXML_Node *parent, bool refresh, char *name, char *fmt, ...)
{
	char *buf;
	va_list args;
	IXML_Node *node = (IXML_Node*) ixmlDocument_getElementById((IXML_Document*) parent, name);

	va_start(args, fmt);
	vasprintf(&buf, fmt, args);

	if (!node) {
		XMLAddNode(doc, parent, name, buf);
	} else if (refresh) {
		IXML_Node *child = ixmlNode_getFirstChild(node);
		ixmlNode_setNodeValue(child, buf);
	}

	va_end(args);
	free(buf);

	return node;
}


/*----------------------------------------------------------------------------*/
char *XMLDelNode(IXML_Node *from, char *name)
{
	IXML_Node *self, *node;
	char *value = NULL;

	self = (IXML_Node*) ixmlDocument_getElementById((IXML_Document*) from, name);
	if (!self) return NULL;

	node = (IXML_Node*) ixmlNode_getParentNode(self);
	if (node) ixmlNode_removeChild(node, self, &self);

	node = ixmlNode_getFirstChild(self);
	value = (char*) ixmlNode_getNodeValue(node);
	if (value) value = strdup(value);

	ixmlNode_free(self);
	return value;
}


/*----------------------------------------------------------------------------*/
int XMLAddAttribute(IXML_Document *doc, IXML_Node *parent, char *name, char *fmt, ...)
{
	char buf[256];
	int ret;
	va_list args;

	va_start(args, fmt);
	vsnprintf(buf, 256, fmt, args);
	ret = ixmlElement_setAttribute((IXML_Element*) parent, name, buf);
	va_end(args);

	return ret;
}


/*----------------------------------------------------------------------------*/
bool XMLMatchDocumentItem(IXML_Document *doc, const char *item, const char *s, bool match)
{
	IXML_NodeList *nodeList = NULL;
	IXML_Node *textNode = NULL;
	IXML_Node *tmpNode = NULL;
	int i;
	bool ret = false;
	const char *value;

	nodeList = ixmlDocument_getElementsByTagName(doc, (char *)item);

	for (i = 0; nodeList && i < (int) ixmlNodeList_length(nodeList); i++) {
		tmpNode = ixmlNodeList_item(nodeList, i);
		if (!tmpNode) continue;
		textNode = ixmlNode_getFirstChild(tmpNode);
		if (!textNode) continue;
		value = ixmlNode_getNodeValue(textNode);
		if ((match && !strcmp(value, s)) || (!match && strcasestr(value, s))) {
			ret = true;
			break;
		}
	}

	if (nodeList) ixmlNodeList_free(nodeList);

	return ret;
}


/*----------------------------------------------------------------------------*/
s64_t Time2Int(char *Time)
{
	char *p;
	s64_t ret;

	if (!Time) return 0;

	p = strrchr(Time, ':');

	if (!p) return 0;

	*p = '\0';
	ret = atol(p + 1);

	p = strrchr(Time, ':');
	if (p) {
		*p = '\0';
		ret += atol(p + 1)*60;
	}

	p = Time;
	if (p && *p) {
		ret += atol(p)*3600;
	}

	return ret;
}

/*----------------------------------------------------------------------------*/
/* 																			  */
/* QUEUE management															  */
/* 																			  */
/*----------------------------------------------------------------------------*/

/*----------------------------------------------------------------------------*/
void QueueInit(tQueue *queue, bool mutex, void (*cleanup)(void*))
{
	queue->cleanup = cleanup;
	queue->list.item = NULL;
	if (mutex) {
		queue->mutex = malloc(sizeof(pthread_mutex_t));
		pthread_mutex_init(queue->mutex, NULL);
	}
	else queue->mutex = NULL;
}


/*----------------------------------------------------------------------------*/
void QueueInsert(tQueue *queue, void *item)
{
	struct sQueue_e *list;

	if (queue->mutex) pthread_mutex_lock(queue->mutex);
	list = &queue->list;

	while (list->item) list = list->next;
	list->item = item;
	list->next = malloc(sizeof(struct sQueue_e));
	list->next->item = NULL;

	if (queue->mutex) pthread_mutex_unlock(queue->mutex);
}


/*----------------------------------------------------------------------------*/
void *QueueExtract(tQueue *queue)
{
	void *item;
	struct sQueue_e *list;

	if (queue->mutex) pthread_mutex_lock(queue->mutex);

	list = &queue->list;
	item = list->item;

	if (item) {
		struct sQueue_e *next = list->next;
		if (next->item) {
			list->item = next->item;
			list->next = next->next;
		} else list->item = NULL;
		NFREE(next);
	}

	if (queue->mutex) pthread_mutex_unlock(queue->mutex);

	return item;
}


/*----------------------------------------------------------------------------*/
void *QueueHead(tQueue *queue)
{
	void *item;

	if (queue->mutex) pthread_mutex_lock(queue->mutex);
	item = queue->list.item;
	if (queue->mutex) pthread_mutex_unlock(queue->mutex);

	return item;
}


/*----------------------------------------------------------------------------*/
void QueueFlush(tQueue *queue)
{
	struct sQueue_e *list;

	if (queue->mutex) pthread_mutex_lock(queue->mutex);

	list = &queue->list;

	while (list->item) {
		struct sQueue_e *next = list->next;
		if (queue->cleanup)	(*(queue->cleanup))(list->item);
		if (list != &queue->list) { NFREE(list); }
		list = next;
	}

	if (list != &queue->list) { NFREE(list); }
	queue->list.item = NULL;

	if (queue->mutex) {
		pthread_mutex_unlock(queue->mutex);
		pthread_mutex_destroy(queue->mutex);
		free(queue->mutex);
	}
}


/*----------------------------------------------------------------------------*/
char *uPNPEvent2String(Upnp_EventType S)
{
	switch (S) {
	/* Discovery */
	case UPNP_DISCOVERY_ADVERTISEMENT_ALIVE:
		return "UPNP_DISCOVERY_ADVERTISEMENT_ALIVE";
	case UPNP_DISCOVERY_ADVERTISEMENT_BYEBYE:
		return "UPNP_DISCOVERY_ADVERTISEMENT_BYEBYE";
	case UPNP_DISCOVERY_SEARCH_RESULT:
		return "UPNP_DISCOVERY_SEARCH_RESULT";
	case UPNP_DISCOVERY_SEARCH_TIMEOUT:
		return "UPNP_DISCOVERY_SEARCH_TIMEOUT";
	/* SOAP */
	case UPNP_CONTROL_ACTION_REQUEST:
		return "UPNP_CONTROL_ACTION_REQUEST";
	case UPNP_CONTROL_ACTION_COMPLETE:
		return "UPNP_CONTROL_ACTION_COMPLETE";
	case UPNP_CONTROL_GET_VAR_REQUEST:
		return "UPNP_CONTROL_GET_VAR_REQUEST";
	case UPNP_CONTROL_GET_VAR_COMPLETE:
		return "UPNP_CONTROL_GET_VAR_COMPLETE";
	case UPNP_EVENT_SUBSCRIPTION_REQUEST:
		return "UPNP_EVENT_SUBSCRIPTION_REQUEST";
	case UPNP_EVENT_RECEIVED:
		return "UPNP_EVENT_RECEIVED";
	case UPNP_EVENT_RENEWAL_COMPLETE:
		return "UPNP_EVENT_RENEWAL_COMPLETE";
	case UPNP_EVENT_SUBSCRIBE_COMPLETE:
		return "UPNP_EVENT_SUBSCRIBE_COMPLETE";
	case UPNP_EVENT_UNSUBSCRIBE_COMPLETE:
		return "UPNP_EVENT_UNSUBSCRIBE_COMPLETE";
	case UPNP_EVENT_AUTORENEWAL_FAILED:
		return "UPNP_EVENT_AUTORENEWAL_FAILED";
	case UPNP_EVENT_SUBSCRIPTION_EXPIRED:
		return "UPNP_EVENT_SUBSCRIPTION_EXPIRED";
	}

	return "";
}


