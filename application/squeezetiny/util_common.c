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

 /* DLNA.ORG_CI: conversion indicator parameter (integer)
 *     0 not transcoded
 *     1 transcoded
 */
typedef enum {
  DLNA_ORG_CONVERSION_NONE = 0,
  DLNA_ORG_CONVERSION_TRANSCODED = 1,
} dlna_org_conversion_t;

/* DLNA.ORG_OP: operations parameter (string)
 *     "00" (or "0") neither time seek range nor range supported
 *     "01" range supported
 *     "10" time seek range supported
 *     "11" both time seek range and range supported
 */
typedef enum {
  DLNA_ORG_OPERATION_NONE                  = 0x00,
  DLNA_ORG_OPERATION_RANGE                 = 0x01,
  DLNA_ORG_OPERATION_TIMESEEK              = 0x10,
} dlna_org_operation_t;

/* DLNA.ORG_FLAGS, padded with 24 trailing 0s
 *     8000 0000  31  senderPaced
 *     4000 0000  30  lsopTimeBasedSeekSupported
 *     2000 0000  29  lsopByteBasedSeekSupported
 *     1000 0000  28  playcontainerSupported
 *      800 0000  27  s0IncreasingSupported
 *      400 0000  26  sNIncreasingSupported
 *      200 0000  25  rtspPauseSupported
 *      100 0000  24  streamingTransferModeSupported
 *       80 0000  23  interactiveTransferModeSupported
 *       40 0000  22  backgroundTransferModeSupported
 *       20 0000  21  connectionStallingSupported
 *       10 0000  20  dlnaVersion15Supported
 *
 *     Example: (1 << 24) | (1 << 22) | (1 << 21) | (1 << 20)
 *       DLNA.ORG_FLAGS=0170 0000[0000 0000 0000 0000 0000 0000] // [] show padding
 */
typedef enum {
  DLNA_ORG_FLAG_SENDER_PACED               = (1 << 31),
  DLNA_ORG_FLAG_TIME_BASED_SEEK            = (1 << 30),
  DLNA_ORG_FLAG_BYTE_BASED_SEEK            = (1 << 29),
  DLNA_ORG_FLAG_PLAY_CONTAINER             = (1 << 28),

  DLNA_ORG_FLAG_S0_INCREASE                = (1 << 27),
  DLNA_ORG_FLAG_SN_INCREASE                = (1 << 26),
  DLNA_ORG_FLAG_RTSP_PAUSE                 = (1 << 25),
  DLNA_ORG_FLAG_STREAMING_TRANSFERT_MODE    = (1 << 24),

  DLNA_ORG_FLAG_INTERACTIVE_TRANSFERT_MODE = (1 << 23),
  DLNA_ORG_FLAG_BACKGROUND_TRANSFERT_MODE  = (1 << 22),
  DLNA_ORG_FLAG_CONNECTION_STALL           = (1 << 21),
  DLNA_ORG_FLAG_DLNA_V15                   = (1 << 20),
} dlna_org_flags_t;



#define DLNA_ORG_OP (DLNA_ORG_OPERATION_RANGE)

// GNU pre-processor seems to be confused if this is multiline ...

#define DLNA_ORG_FLAG ( DLNA_ORG_FLAG_S0_INCREASE | DLNA_ORG_FLAG_STREAMING_TRANSFERT_MODE | DLNA_ORG_FLAG_BACKGROUND_TRANSFERT_MODE | DLNA_ORG_FLAG_CONNECTION_STALL | DLNA_ORG_FLAG_DLNA_V15 )


/*----------------------------------------------------------------------------*/
/* 																			  */
/* CODEC & MIME management													  */
/* 																			  */
/*----------------------------------------------------------------------------*/

/*---------------------------------------------------------------------------*/
char *make_dlna_content(char *mimetype, u32_t duration) {
	char *buf;
	char *DLNAOrgPN;

	switch (mimetype2format(mimetype)) {
	case 'm':
		DLNAOrgPN = "DLNA.ORG_PN=MP3;";
		break;
	case 'p':
		DLNAOrgPN = "DLNA.ORG_PN=LPCM;";
		break;
	default:
		DLNAOrgPN = "";
	}

	asprintf(&buf, "%sDLNA.ORG_OP=00;DLNA.ORG_CI=0;DLNA.ORG_FLAGS=%08x000000000000000000000000",
						   DLNAOrgPN, DLNA_ORG_FLAG | (duration ? 0 : DLNA_ORG_FLAG_SN_INCREASE));

	return buf;
}


/*---------------------------------------------------------------------------*/
char *mimetype2ext(char *mimetype)
{
	if (!mimetype) return '\0';

	if (strstr(mimetype, "wav")) return "wav";
	if (strstr(mimetype, "audio/L")) return "pcm";
	if (strstr(mimetype, "flac")) return "flac";
	if (strstr(mimetype, "flc")) return "flc";
	if (strstr(mimetype, "mp3") || strstr(mimetype, "mpeg")) return "mp3";
	if (strstr(mimetype, "ogg")) return "ogg";
	if (strstr(mimetype, "aif")) return "aif";
	if (strstr(mimetype, "aac")) return "aac";
	if (strstr(mimetype, "mp4")) return "mp4";

	return "nil";

}

/*---------------------------------------------------------------------------*/
u8_t mimetype2format(char *mimetype)
{
	if (!mimetype) return '\0';

	if (strstr(mimetype, "wav")) return 'w';
	if (strstr(mimetype, "audio/L")) return 'p';
	if (strstr(mimetype, "flac") || strstr(mimetype, "flc")) return 'f';
	if (strstr(mimetype, "mp3") || strstr(mimetype, "mpeg")) return 'm';
	if (strstr(mimetype, "ogg")) return 'o';
	if (strstr(mimetype, "aif")) return 'i';
	if (strstr(mimetype, "aac")) return 'a';
	if (strstr(mimetype, "mp4")) return 'a';

	return '*';
}

/*---------------------------------------------------------------------------*/
static char *_lookup(char *mimetypes[], int n, ...) {
	char *mimetype, **p;
	va_list args;

	va_start(args, n);

	while (n--) {
		mimetype = va_arg(args, char*);
		p = mimetypes;
		while (*p) {
			if (!strcmp(mimetype, *p)) {
				va_end(args);
				return strdup(*p);
			}
			p++;
		}
   }

   va_end(args);

   return NULL;
}

/*---------------------------------------------------------------------------*/
char *find_mimetype(char codec, char *mimetypes[], char *out) {
	switch (codec) {
	case 'm': return _lookup(mimetypes, 3, "audio/mp3", "audio/mpeg", "audio/mpeg3");
	case 'f': return _lookup(mimetypes, 2, "audio/x-flac", "audio/flac");
	case 'w': return _lookup(mimetypes, 2, "audio/x-wma", "audio/wma");
	case 'o': return _lookup(mimetypes, 2, "audio/ogg", "audio/x-ogg");
	case 'a': return _lookup(mimetypes, 4, "audio/x-aac", "audio/aac", "audio/m4a", "audio/mp4");
	case 'l': return _lookup(mimetypes, 1, "audio/m4a");
	}

   return NULL;
}

/*---------------------------------------------------------------------------*/
char* find_pcm_mimetype(u8_t endian, u8_t *sample_size, bool truncable, u32_t sample_rate,
						u8_t channels, char *mimetypes[], char *options) {
	char *mimetype, fmt[8];
	u8_t size = *sample_size;

	while (1) {

		if (sscanf(options, "%[^,]", fmt) <= 0) return NULL;

		while (strstr(fmt, "raw")) {
			char **p, a[16], r[16], c[16];

			// find audio/Lxx
			p = mimetypes;
			sprintf(a, "audio/L%hhu", *sample_size);
			sprintf(r, "rate=%u", sample_rate);
			sprintf(c, "channels=%hhu", channels);
			while (*p) {
				if (strstr(*p, a) &&
				   (!strstr(*p, "rate=") || strstr(*p, r)) &&
				   (!strstr(*p, "channels=") || strstr(*p, c))) {
					char *rsp;

					asprintf(&rsp, "%s;%s;%s", a, r, c);
					return rsp;
				}
				p++;
			}

			if (*sample_size == 24 && truncable) *sample_size = 16;
			else {
				*sample_size = size;
				break;
			}
		}

		if (strstr(fmt, "wav")) {
			mimetype = _lookup(mimetypes, 3, "audio/wav", "audio/x-wav", "audio/wave");
			if (mimetype) return mimetype;
		}

		if (strstr(fmt, "aif")) {
			mimetype = _lookup(mimetypes, 4, "audio/aiff", "audio/x-aiff", "audio/aif", "audio/x-aif");
			if (mimetype) return mimetype;
		}

		// try next one
		options += strlen(fmt);
		if (*options) options++;
	}
}


#if 0
/*---------------------------------------------------------------------------*/
char* find_pcm_mimetype(u8_t endian, u8_t *sample_size, bool truncable, u32_t sample_rate,
							  u8_t channels, char *mimetypes[], char *out) {
	char *mimetype;
	u8_t size = *sample_size;

	// first try pcm search (preferred format)
	while (1) {
		char *mimetype, **p, *s;

		// try the full mime-type
		p = mimetypes;
		asprintf(&s, "audio/L%hhu;rate=%u;channels=%hhu", *sample_size, sample_rate, channels);
		while (*p) {
			if (!strcmp(s, *p)) return strdup(*p);
			p++;
		}

		// no luck, try a simple audio/Lxx w/o channel or rate indication
		p = mimetypes;
		sprintf(s, "audio/L%hhu", *sample_size);
		while (*p) {
			if (!strcmp(s, *p)) {
				free(s);
				asprintf(&s, "%s;rate=%u;channels=%hhu", *p, sample_rate, channels);
				return s;
			}
			p++;
		}
		free(s);

		// still no luck, try again with 16 bits if authorize to trunc 24 bits
		if (*sample_size == 24 && truncable) *sample_size = 16;
		else break;
	}
	*/

	// need to restore sample_size;
	*sample_size = size;

	// no pcm, try based on endianness
	if (endian) {
		mimetype = _lookup(mimetypes, 3, "audio/wav", "audio/x-wav", "audio/wave");
		if (strcmp(mimetype, "audio/null")) return mimetype;
		free(mimetype);
		return _lookup(mimetypes, 2, "audio/aiff", "audio/x-aiff");
	}

	// this is aiff, try that first and then wav
	mimetype = _lookup(mimetypes, 2, "audio/aiff", "audio/x-aiff");
	if (strcmp(mimetype, "audio/null")) return mimetype;
	free(mimetype);
	return _lookup(mimetypes, 3, "audio/wav", "audio/x-wav", "audio/wave");
}
#endif


/*----------------------------------------------------------------------------*/
/* 																			  */
/* URL management													  	      */
/* 																			  */
/*----------------------------------------------------------------------------*/

/*---------------------------------------------------------------------------*/
/* Converts a hex character to its integer value */
static char from_hex(char ch) {
  return isdigit(ch) ? ch - '0' : tolower(ch) - 'a' + 10;
}

/*---------------------------------------------------------------------------*/
/* Converts an integer value to its hex character*/
static char to_hex(char code) {
  static char hex[] = "0123456789abcdef";
  return hex[code & 15];
}

/*---------------------------------------------------------------------------*/
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

/*---------------------------------------------------------------------------*/
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

/*----------------------------------------------------------------------------*/
/* 																			  */
/* SYSTEM															  	      */
/* 																			  */
/*----------------------------------------------------------------------------*/

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


#if WIN
/*----------------------------------------------------------------------------*/
int asprintf(char **strp, const char *fmt, ...)
{
	va_list args, cp;
	int len, ret = 0;

	va_start(args, fmt);
	len = vsnprintf(NULL, 0, fmt, args);
	*strp = malloc(len + 1);

	if (*strp) ret = vsprintf(*strp, fmt, args);

	va_end(args);

	return ret;
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
char *strdupn(char *p)
{
	if (p) return strdup(p);
	return NULL;
}

/*----------------------------------------------------------------------------*/
/* 																			  */
/* NETWORK															  	      */
/* 																			  */
/*----------------------------------------------------------------------------*/

/*---------------------------------------------------------------------------*/
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


