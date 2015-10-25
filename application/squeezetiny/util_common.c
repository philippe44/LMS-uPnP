#include <stdio.h>
#include <stdlib.h>
#include <stdarg.h>
#include <string.h>
#include <ctype.h>

#if defined(WIN32) || defined(WIN)
#include <winsock2.h>
#endif

#include "squeezedefs.h"
#include "util_common.h"

// logging functions
const char *logtime(void) {
	static char buf[100];
#if WIN
	SYSTEMTIME lt;
	GetLocalTime(&lt);
	sprintf(buf, "[%02d:%02d:%02d.%03d]", lt.wHour, lt.wMinute, lt.wSecond, lt.wMilliseconds);
#else
	struct timeval tv;
	gettimeofday(&tv, NULL);
	strftime(buf, sizeof(buf), "[%T.", localtime(&tv.tv_sec));
	sprintf(buf+strlen(buf), "%06ld]", (long)tv.tv_usec);
#endif
	return buf;
}

/*---------------------------------------------------------------------------*/
void logprint(const char *fmt, ...) {
	va_list args;

	va_start(args, fmt);
	vfprintf(stderr, fmt, args);
	fflush(stderr);
}

/*---------------------------------------------------------------------------*/
log_level debug2level(char *level)
{
	if (!strcmp(level, "error")) return lERROR;
	if (!strcmp(level, "warn")) return lWARN;
	if (!strcmp(level, "info")) return lINFO;
	if (!strcmp(level, "debug")) return lDEBUG;
	if (!strcmp(level, "sdebug")) return lSDEBUG;
	return lWARN;
}

/*---------------------------------------------------------------------------*/
char *level2debug(log_level level)
{
	switch (level) {
	case lERROR: return "error";
	case lWARN: return "warn";
	case lINFO: return "info";
	case lDEBUG: return "debug";
	case lSDEBUG: return "debug";
	default: return "warn";
	}
}

/*---------------------------------------------------------------------------*/
#if 0
 * C++ version 0.4 char* style "itoa":
	 * Written by Lukás Chmela
	 * Released under GPLv3.
	 */

char* itoa(int value, char* result, int base) {
		// check that the base if valid
		if (base < 2 || base > 36) { *result = '\0'; return result; }

		char* ptr = result, *ptr1 = result, tmp_char;
		int tmp_value;

		do {
			tmp_value = value;
			value /= base;
			*ptr++ = "zyxwvutsrqponmlkjihgfedcba9876543210123456789abcdefghijklmnopqrstuvwxyz" [35 + (tmp_value - value * base)];
		} while ( value );

		// Apply negative sign
		if (tmp_value < 0) *ptr++ = '-';
		*ptr-- = '\0';
		while(ptr1 < ptr) {
			tmp_char = *ptr;
			*ptr--= *ptr1;
			*ptr1++ = tmp_char;
		}
		return result;
	}
}
#endif

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


