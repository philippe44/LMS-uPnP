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
static IXML_Node *_getAttributeNode(IXML_Node *node, char *SearchAttr);

/*----------------------------------------------------------------------------*/
void ExtractIP(const char *URL, in_addr_t *IP)
{
	int i;
	char*p1 = malloc(strlen(URL) + 1);
	char *p2;

	strcpy(p1, URL);
	p2 = strtok(p1,"/");
	p2 = strtok(NULL,"/");
	strtok(p2, ".");
	for (i = 0; i < 3; i++) {
		*((u8_t*) IP + i) = p2 ? atoi(p2) : 0;
		p2 = strtok(NULL, ".");
	}
	strtok(p2, ":");
	*((u8_t*) IP + 3) = p2 ? atoi(p2) : 0;
	free(p1);
}


/*----------------------------------------------------------------------------*/
char *XMLGetFirstDocumentItem(IXML_Document *doc, const char *item)
{
	IXML_NodeList *nodeList = NULL;
	IXML_Node *textNode = NULL;
	IXML_Node *tmpNode = NULL;
	char *ret = NULL;

	nodeList = ixmlDocument_getElementsByTagName(doc, (char *)item);
	if (nodeList) {
		tmpNode = ixmlNodeList_item(nodeList, 0);
		if (tmpNode) {
			textNode = ixmlNode_getFirstChild(tmpNode);
			if (!textNode) {
				LOG_WARN("(BUG) ixmlNode_getFirstChild(tmpNode) returned NULL", NULL);
				ret = strdup("");
			}
			else {
				ret = strdup(ixmlNode_getNodeValue(textNode));
				if (!ret) {
					LOG_WARN("ixmlNode_getNodeValue returned NULL", NULL);
					ret = strdup("");
				}
			}
		} else
			LOG_WARN("ixmlNodeList_item(nodeList, 0) returned NULL", NULL);
	} else
		LOG_SDEBUG("Error finding %s in XML Node", item);

	if (nodeList) ixmlNodeList_free(nodeList);

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
	const char *serviceTypeBase, char **serviceType, char **serviceId, char **eventURL, char **controlURL)
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

	baseURL = XMLGetFirstDocumentItem(DescDoc, "URLBase");
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
			LOG_SDEBUG("serviceType %s", serviceType);

			// remove version from service type
			*strrchr(tempServiceType, ':') = '\0';
			if (tempServiceType && strcmp(tempServiceType, serviceTypeBase) == 0) {
				NFREE(*serviceType);
				*serviceType = XMLGetFirstElementItem((IXML_Element *)service, "serviceType");
				NFREE(*serviceId);
				*serviceId = XMLGetFirstElementItem(service, "serviceId");
				LOG_SDEBUG("Service %s, serviceId: %s", serviceType, serviceId);
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
s64_t Time2Int(char *Time)
{
	char *p;
	s64_t ret;

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
void QueueInit(tQueue *queue)
{
	queue->item = NULL;
}

/*----------------------------------------------------------------------------*/
void QueueInsert(tQueue *queue, void *item)
{
	while (queue->item)	queue = queue->next;
	queue->item = item;
	queue->next = malloc(sizeof(tQueue));
	queue->next->item = NULL;
}


/*----------------------------------------------------------------------------*/
void *QueueExtract(tQueue *queue)
{
	void *item = queue->item;
	tQueue *next = queue->next;

	if (item) {
		queue->item = next->item;
		if (next->item) queue->next = next->next;
		NFREE(next);
	}

	return item;
}


/*----------------------------------------------------------------------------*/
void QueueFlush(tQueue *queue)
{
	void *item = queue->item;
	tQueue *next = queue->next;

	queue->item = NULL;

	while (item) {
		next = queue->next;
		item = next->item;
		if (next->item) queue->next = next->next;
		NFREE(next);
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


