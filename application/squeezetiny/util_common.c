/*
 *  Squeezelite - lightweight headless squeezebox emulator
 *
 *  (c) Adrian Smith 2012-2015, triode1@btinternet.com
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

#include <stdio.h>
#include <stdlib.h>
#include <stdarg.h>
#include <string.h>
#include <ctype.h>

#if defined(WIN32) || defined(WIN)
#include <winsock2.h>
#endif

#include "squeezedefs.h"

#if LINUX || OSX || FREEBSD
#include <sys/ioctl.h>
#include <net/if.h>
#include <net/if_arp.h>
#include <netdb.h>
#if FREEBSD
#include <ifaddrs.h>
#include <net/if_dl.h>
#include <net/if_types.h>
#endif
#endif
#if WIN
#include <iphlpapi.h>
#endif
#if OSX
#include <sys/socket.h>
#include <sys/sysctl.h>
#include <net/if_dl.h>
#include <net/if_types.h>
#include <net/route.h>
#include <ifaddrs.h>
#include <netdb.h>
#include <netinet/in.h>
#include <netinet/if_ether.h>
#include <sys/time.h>
#endif

#include "util_common.h"
#include "log_util.h"


extern log_level 	util_loglevel;
//static log_level 	*loglevel = &util_loglevel;

/* Converts a hex character to its integer value */
static char from_hex(char ch) {
  return isdigit(ch) ? ch - '0' : tolower(ch) - 'a' + 10;
}

/* Converts an integer value to its hex character*/
static char to_hex(char code) {
  static char hex[] = "0123456789abcdef";
  return hex[code & 15];
}

/* Returns a url-encoded version of str */
/* IMPORTANT: be sure to free() the returned string after use */
char *url_encode(char *str) {
  char *pstr = str, *buf = malloc(strlen(str) * 3 + 1), *pbuf = buf;
  while (*pstr) {
    if (isalnum(*pstr) || *pstr == '-' || *pstr == '_' || *pstr == '.' || *pstr == '~')
	  *pbuf++ = *pstr;
	else if (*pstr == ' ') {
	  *pbuf++ = '+';
	}
	else
	  *pbuf++ = '%', *pbuf++ = to_hex(*pstr >> 4), *pbuf++ = to_hex(*pstr & 15);
    pstr++;
  }
  *pbuf = '\0';
  return buf;
}

/* Returns a url-decoded version of str */
/* IMPORTANT: be sure to free() the returned string after use */
char *url_decode(char *str) {
  char *pstr = str, *buf = malloc(strlen(str) + 1), *pbuf = buf;
  while (*pstr) {
	if (*pstr == '%') {
	  if (pstr[1] && pstr[2]) {
		*pbuf++ = from_hex(pstr[1]) << 4 | from_hex(pstr[2]);
		pstr += 2;
	  }
	} else if (*pstr == '+') {
	  *pbuf++ = ' ';
	} else {
	  *pbuf++ = *pstr;
	}
	pstr++;
  }
  *pbuf = '\0';
  return buf;
}

#if LINUX || OSX || FREEBSD
/*---------------------------------------------------------------------------*/
char *strlwr(char *str)
{
 char *p = str;
 while (*p) {
	*p = tolower(*p);
	p++;
 }
 return str;
}
#endif

/*---------------------------------------------------------------------------*/
u32_t hash32(char *str)
{
	u32_t hash = 5381;
	s32_t c;

	if (!str) return 0;

	while ((c = *str++) != 0)
		hash = ((hash << 5) + hash) + c; /* hash * 33 + c */

	return hash;
}

/*---------------------------------------------------------------------------*/
char *stristr(char *s1, char *s2)
{
 char *s1_lwr = strlwr(strdup(s1));
 char *s2_lwr = strlwr(strdup(s2));
 char *p = strstr(s1_lwr, s2_lwr);

 if (p) p = s1 + (p - s1_lwr);
 free(s1_lwr);
 free(s2_lwr);
 return p;
}

/*---------------------------------------------------------------------------*/
/* IMPORTANT: be sure to free() the returned string after use */
#define QUOT 	"&quot;"
#define AMP	 	"&amp;"
#define LT		"&lt;"
#define GT		"&gt;"
char *toxml(char *src)
{
	char *p, *q, *res;
	int i;

	for (i = 0, p = src; *p; p++) {
		switch (*p) {
			case '\"': i += strlen(QUOT); break;
			case '&':  i += strlen(AMP); break;
			case '<':  i += strlen(LT); break;
			case '>':  i += strlen(GT); break;
		}
	}

	res = malloc(strlen(src) + i + 1);
	if (!res) return NULL;

	for (q = res, p = src; *p; p++) {
		char *rep = NULL;
		switch (*p) {
			case '\"': rep = QUOT; break;
			case '&':  rep = AMP; break;
			case '<':  rep = LT; break;
			case '>':  rep = GT; break;
		}
		if (rep) {
			memcpy(q, rep, strlen(rep));
			q += strlen(rep);
		}
		else {
			*q = *p;
			q++;
		}
	}

	return res;
}

#if LINUX || OSX || BSD
bool get_interface(struct in_addr *addr)
{
	struct ifreq *ifreq;
	struct ifconf ifconf;
	char buf[512];
	unsigned i, nb;
	int fd;
	bool valid = false;

	fd = socket(AF_INET, SOCK_DGRAM, 0);

	ifconf.ifc_len = sizeof(buf);
	ifconf.ifc_buf = buf;

	if (ioctl(fd, SIOCGIFCONF, &ifconf)!=0) return false;

	ifreq = ifconf.ifc_req;
	nb = ifconf.ifc_len / sizeof(struct ifreq);

	for (i = 0; i < nb; i++) {
		ioctl(fd, SIOCGIFFLAGS, &ifreq[i]);
		//!(ifreq[i].ifr_flags & IFF_POINTTOPOINT);
		if ((ifreq[i].ifr_flags & IFF_UP) &&
			!(ifreq[i].ifr_flags & IFF_LOOPBACK) &&
			ifreq[i].ifr_flags & IFF_MULTICAST) {
				*addr = ((struct sockaddr_in *) &(ifreq[i].ifr_addr))->sin_addr;
				valid = true;
				break;
		 }
	}

	close(fd);
	return valid;
}
#endif


#if WIN
bool get_interface(struct in_addr *addr)
{
	INTERFACE_INFO ifList[20];
	unsigned bytes;
	int i, nb;
	bool valid = false;
	int fd;

	memset(addr, 0, sizeof(struct in_addr));
	fd = socket(AF_INET, SOCK_DGRAM, 0);

	if (WSAIoctl(fd, SIO_GET_INTERFACE_LIST, 0, 0, (void*) &ifList, sizeof(ifList), (void*) &bytes, 0, 0) == SOCKET_ERROR) return false;

	nb = bytes / sizeof(INTERFACE_INFO);
	for (i = 0; i < nb; i++) {
		if ((ifList[i].iiFlags & IFF_UP) &&
			!(ifList[i].iiFlags & IFF_POINTTOPOINT) &&
			!(ifList[i].iiFlags & IFF_LOOPBACK) &&
			(ifList[i].iiFlags & IFF_MULTICAST)) {
				*addr = ((struct sockaddr_in *) &(ifList[i].iiAddress))->sin_addr;
				valid = true;
			break;
		}
	}

	close(fd);
	return valid;
}
#endif



/*---------------------------------------------------------------------------*/
#define MAX_INTERFACES 256
#define DEFAULT_INTERFACE 1
#if !defined(WIN32)
#define INVALID_SOCKET (-1)
#endif
in_addr_t get_localhost(char **name)
{
#ifdef WIN32
	char buf[256];
	struct hostent *h = NULL;
	struct sockaddr_in LocalAddr;

	memset(&LocalAddr, 0, sizeof(LocalAddr));

	gethostname(buf, 256);
	h = gethostbyname(buf);

	if (name) *name = strdup(buf);

	if (h != NULL) {
		memcpy(&LocalAddr.sin_addr, h->h_addr_list[0], 4);
		return LocalAddr.sin_addr.s_addr;
	}
	else return INADDR_ANY;
#elif defined (__APPLE__) || defined(__FreeBSD__)
	struct ifaddrs *ifap, *ifa;

	if (name) {
		*name = malloc(256);
		gethostname(*name, 256);
	}

	if (getifaddrs(&ifap) != 0) return INADDR_ANY;

	/* cycle through available interfaces */
	for (ifa = ifap; ifa != NULL; ifa = ifa->ifa_next) {
		/* Skip loopback, point-to-point and down interfaces,
		 * except don't skip down interfaces
		 * if we're trying to get a list of configurable interfaces. */
		if ((ifa->ifa_flags & IFF_LOOPBACK) ||
			(!( ifa->ifa_flags & IFF_UP))) {
			continue;
		}
		if (ifa->ifa_addr->sa_family == AF_INET) {
			/* We don't want the loopback interface. */
			if (((struct sockaddr_in *)(ifa->ifa_addr))->sin_addr.s_addr ==
				htonl(INADDR_LOOPBACK)) {
				continue;
			}
			return ((struct sockaddr_in *)(ifa->ifa_addr))->sin_addr.s_addr;
			break;
		}
	}
	freeifaddrs(ifap);

	return INADDR_ANY;
#else
	char szBuffer[MAX_INTERFACES * sizeof (struct ifreq)];
	struct ifconf ifConf;
	struct ifreq ifReq;
	int nResult;
	long unsigned int i;
	int LocalSock;
	struct sockaddr_in LocalAddr;
	int j = 0;

	if (name) {
		*name = malloc(256);
		gethostname(*name, 256);
	}

	/* purify */
	memset(&ifConf,  0, sizeof(ifConf));
	memset(&ifReq,   0, sizeof(ifReq));
	memset(szBuffer, 0, sizeof(szBuffer));
	memset(&LocalAddr, 0, sizeof(LocalAddr));

	/* Create an unbound datagram socket to do the SIOCGIFADDR ioctl on.  */
	LocalSock = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
	if (LocalSock == INVALID_SOCKET) return false;
	/* Get the interface configuration information... */
	ifConf.ifc_len = (int)sizeof szBuffer;
	ifConf.ifc_ifcu.ifcu_buf = (caddr_t) szBuffer;
	nResult = ioctl(LocalSock, SIOCGIFCONF, &ifConf);
	if (nResult < 0) {
		close(LocalSock);
		return INADDR_ANY;
	}

	/* Cycle through the list of interfaces looking for IP addresses. */
	for (i = 0lu; i < (long unsigned int)ifConf.ifc_len && j < DEFAULT_INTERFACE; ) {
		struct ifreq *pifReq =
			(struct ifreq *)((caddr_t)ifConf.ifc_req + i);
		i += sizeof *pifReq;
		/* See if this is the sort of interface we want to deal with. */
		memset(ifReq.ifr_name, 0, sizeof(ifReq.ifr_name));
		strncpy(ifReq.ifr_name, pifReq->ifr_name,
			sizeof(ifReq.ifr_name) - 1);
		/* Skip loopback, point-to-point and down interfaces,
		 * except don't skip down interfaces
		 * if we're trying to get a list of configurable interfaces. */
		ioctl(LocalSock, SIOCGIFFLAGS, &ifReq);
		if ((ifReq.ifr_flags & IFF_LOOPBACK) ||
			(!(ifReq.ifr_flags & IFF_UP))) {
			continue;
		}
		if (pifReq->ifr_addr.sa_family == AF_INET) {
			/* Get a pointer to the address...*/
			memcpy(&LocalAddr, &pifReq->ifr_addr,
				sizeof pifReq->ifr_addr);
			/* We don't want the loopback interface. */
			if (LocalAddr.sin_addr.s_addr ==
				htonl(INADDR_LOOPBACK)) {
				continue;
			}
		}
		/* increment j if we found an address which is not loopback
		 * and is up */
		j++;
	}
	close(LocalSock);

	return LocalAddr.sin_addr.s_addr;
#endif
}


