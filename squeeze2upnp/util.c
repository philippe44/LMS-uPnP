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

/*----------------------------------------------------------------------------*/
/* globals */
/*----------------------------------------------------------------------------*/

/*----------------------------------------------------------------------------*/
/* locals */
/*----------------------------------------------------------------------------*/
static log_level loglevel = lWARN;

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
static IXML_NodeList *XMLGetNthServiceList(IXML_Document *doc, unsigned int n)
{
	IXML_NodeList *ServiceList = NULL;
	IXML_NodeList *servlistnodelist = NULL;
	IXML_Node *servlistnode = NULL;

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
		} else
			LOG_WARN("ixmlNodeList_item(nodeList, n) returned NULL", NULL);
	}
	if (servlistnodelist)
		ixmlNodeList_free(servlistnodelist);

	return ServiceList;
}

/*----------------------------------------------------------------------------*/
int XMLFindAndParseService(IXML_Document *DescDoc, const char *location,
	const char *serviceType, char **serviceId, char **eventURL, char **controlURL)
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

	baseURL = XMLGetFirstDocumentItem(DescDoc, "URLBase");
	if (baseURL) base = baseURL;
	else base = location;

	for (sindex = 0; (serviceList = XMLGetNthServiceList(DescDoc , sindex)) != NULL; sindex++) {
		tempServiceType = NULL;
		relcontrolURL = NULL;
		releventURL = NULL;
		service = NULL;

		length = ixmlNodeList_length(serviceList);
		for (i = 0; i < length; i++) {
			service = (IXML_Element *)ixmlNodeList_item(serviceList, i);
			tempServiceType = XMLGetFirstElementItem((IXML_Element *)service, "serviceType");
			LOG_SDEBUG("serviceType %s", tempServiceType);

			if (tempServiceType && strcmp(tempServiceType, serviceType) == 0) {
				*serviceId = XMLGetFirstElementItem(service, "serviceId");
				LOG_SDEBUG("Service %s, serviceId: %s", serviceType, serviceId);
				relcontrolURL = XMLGetFirstElementItem(service, "controlURL");
				releventURL = XMLGetFirstElementItem(service, "eventSubURL");
				*controlURL = (char*) malloc(strlen(base) + strlen(relcontrolURL) + 1);
				if (*controlURL) {
					ret = UpnpResolveURL(base, relcontrolURL, *controlURL);
					if (ret != UPNP_E_SUCCESS) LOG_ERROR("Error generating controlURL from %s + %s", base, relcontrolURL);
				}
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
char *XMLGetChangeItem(IXML_Document *doc, char *Item)
{
	IXML_NodeList *changenodelist;
	IXML_Node *changenode, *textnode;
	char *buf, *p;

	changenodelist = ixmlDocument_getElementsByTagName(doc, "LastChange");
	if (!changenodelist) {
		LOG_ERROR("LastChange missing", NULL);
		return NULL;
	}

	changenode = ixmlNodeList_item(changenodelist, 0);
	if (!changenode) {
		ixmlNodeList_free(changenodelist);
		LOG_ERROR("LastChange missing", NULL);
		return NULL;
	}

	textnode = ixmlNode_getFirstChild(changenode);
	if (!textnode) {
		ixmlNodeList_free(changenodelist);
		LOG_ERROR("Child missing in LastChange", NULL);
		return NULL;
	}

	buf = strdup(ixmlNode_getNodeValue(textnode));
	p = strstr(buf, Item);
	if (p) {
		p = strtok(p, "\"");
		p = strtok(NULL, "\"");
		strcpy(buf, p);
	}
	else {
		free(buf);
		buf = NULL;
	}

	ixmlNodeList_free(changenodelist);

	return buf;
}

/*----------------------------------------------------------------------------*/
IXML_Node *XMLAddNode(IXML_Document *doc, IXML_Node *parent, char *name, char *fmt, ...)
{
	IXML_Node *node;
	IXML_Element *elm;

	char buf[256];
	va_list args;

	elm =  ixmlDocument_createElement(doc, name);
	if (parent) ixmlNode_appendChild(parent, elm);
	else ixmlNode_appendChild(doc, elm);

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
unsigned Time2Int(char *Time)
{
	char *p;
	unsigned ret;

	p = strrchr(Time, ':');

	if (!p) return 0;

	*p = '\0';
	ret = atol(p + 1);

	p = strrchr(Time, ':');
	if (p) {
		*p = '\0';
		ret += atol(p + 1)*60;
	}

	p = strrchr(Time, ':');
	if (p) {
		*p = '\0';
		ret += atol(p + 1)*3600;
	}

	return ret;
}

#if 0
/*----------------------------------------------------------------------------*/
char *XMLGetChangeItem(IXML_Document *doc, char *Item)
{
	unsigned i;
	IXML_NodeList *changenodelist, *p_list;
	IXML_Node *changenode, *p_node;
	char *Change;

	changenodelist = ixmlDocument_getElementsByTagName(doc, "LastChange");
	if (!changenodelist) return NULL;

	for (i = 0; i < ixmlNodeList_length(changenodelist); i++) {
		changenode = ixmlNodeList_item(changenodelist, i);
		if (changenode) {
			p_list = ixmlElement_getElementsByTagName((IXML_Element *)changenode, Item);
			if (p_list) {
					p_node = ixmlNodeList_item(p_list, 0);
					p_node = ixmlNode_getFirstChild(p_node);
					Change = strdup(ixmlNode_getNodeValue(p_node));
					break;
			}
			free(p_list);
		}
	}

	if (changenodelist)
		ixmlNodeList_free(changenodelist);

	return Change;
}
#endif


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

void uPNPLogLevel(log_level level)
{
	LOG_WARN("uPNP change log", NULL);
	loglevel = level;
}

