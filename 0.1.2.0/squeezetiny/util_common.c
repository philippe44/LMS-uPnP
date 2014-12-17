#include <stdio.h>
#include <stdlib.h>
#include <stdarg.h>
#include <string.h>

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
