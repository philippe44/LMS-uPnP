/*
 *  Metadata instance
 *
 *  (c) Philippe, philippe_44@outlook.com
 *
 *  See LICENSE
 *
 *
 */

#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <stdarg.h>

#include "platform.h"
#include "mimetypes.h"

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
	DLNA_ORG_OPERATION_NONE = 0x00,
	DLNA_ORG_OPERATION_RANGE = 0x01,
	DLNA_ORG_OPERATION_TIMESEEK = 0x10,
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
	DLNA_ORG_FLAG_SENDER_PACED = (1 << 31),
	DLNA_ORG_FLAG_TIME_BASED_SEEK = (1 << 30),
	DLNA_ORG_FLAG_BYTE_BASED_SEEK = (1 << 29),
	DLNA_ORG_FLAG_PLAY_CONTAINER = (1 << 28),

	DLNA_ORG_FLAG_S0_INCREASE = (1 << 27),
	DLNA_ORG_FLAG_SN_INCREASE = (1 << 26),
	DLNA_ORG_FLAG_RTSP_PAUSE = (1 << 25),
	DLNA_ORG_FLAG_STREAMING_TRANSFERT_MODE = (1 << 24),

	DLNA_ORG_FLAG_INTERACTIVE_TRANSFERT_MODE = (1 << 23),
	DLNA_ORG_FLAG_BACKGROUND_TRANSFERT_MODE = (1 << 22),
	DLNA_ORG_FLAG_CONNECTION_STALL = (1 << 21),
	DLNA_ORG_FLAG_DLNA_V15 = (1 << 20),
} dlna_org_flags_t;

/*----------------------------------------------------------------------------*/
bool mimetype_match_codec(char* mimetypes[], int n, ...) {
	bool match = false;
	va_list args;
	va_start(args, n);

	while (!match && n--) {
		char* needle = va_arg(args, char*);
		for (char** p = mimetypes; *p && !match; p++) if (strstr(*p, needle)) return match = true;
	}

	va_end(args);
	return match;
}

/*----------------------------------------------------------------------------*/
static char* _lookup(char* mimetypes[], char* details, int n, ...) {
	char* mimetype = NULL;
	va_list args;
	va_start(args, n);

	while (!mimetype && n--) {
		char *needle = va_arg(args, char*);
		for (char** p = mimetypes; *p && !mimetype; p++) {
#ifdef CODECS_STRICT
			if (**p == '*' || (strstr(*p, needle) && (!details || strstr(*p, details)))) {
#else
			if (**p == '*' || strcasestr(*p, needle)) {
#endif
				if (!details) mimetype = strdup(needle);
				else asprintf(&mimetype, "%s;%s", needle, details);
			}

		}
	}

	va_end(args);
	return mimetype;
}

/*---------------------------------------------------------------------------*/
char* mimetype_from_codec(char codec, char* mimetypes[], ...) {
	va_list args;

	// in variadic, char are promoted to int
	switch (codec) {
	case 'm': return _lookup(mimetypes, NULL, 3, "audio/mp3", "audio/mpeg", "audio/mpeg3");
	case 'w': return _lookup(mimetypes, NULL, 2, "audio/wma", "audio/x-wma");
	case 'o': return _lookup(mimetypes, "codecs=vorbis", 3, "audio/ogg", "audio/x-ogg", "application/ogg");
	case 'u': return _lookup(mimetypes, "codecs=opus", 3, "audio/ogg", "audio/x-ogg", "application/ogg");
	case 'l': return _lookup(mimetypes, NULL, 2, "audio/mp4", "audio/m4a");
	case 'a': {
		va_start(args, mimetypes);
		bool mp4 = (va_arg(args, int) == '5');
		va_end(args);

		if (mp4) return _lookup(mimetypes, NULL, 4, "audio/mp4", "audio/m4a", "audio/aac", "audio/x-aac");
		else return _lookup(mimetypes, NULL, 4, "audio/aac", "audio/x-aac", "audio/mp4", "audio/m4a");
	}
	case 'F':
	case 'f': {
		va_start(args, mimetypes);
		bool ogg = (va_arg(args, int) == 'o');
		va_end(args);

		if (ogg) return _lookup(mimetypes, "codecs=flac", 3, "audio/ogg", "audio/x-ogg", "application/ogg");
		else return _lookup(mimetypes, NULL, 2, "audio/flac", "audio/x-flac");
	}
	case 'd': {
		va_start(args, mimetypes);
		char *mimetype, type = va_arg(args, int);
		va_end(args);

		if (type == '0') mimetype = _lookup(mimetypes, NULL, 2, "audio/dsf", "audio/x-dsf");
		else if (type == '1') mimetype = _lookup(mimetypes, NULL, 2, "audio/dff", "audio/x-dff");
		else mimetype = _lookup(mimetypes, NULL, 2, "audio/dsd", "audio/x-dsd");
		
		return mimetype;
	}
	case 'p': {
		va_start(args, mimetypes);
		char fmt[8], * mimetype = NULL, * codecs = va_arg(args, char*);
		va_end(args);

		while (codecs && !mimetype && sscanf(codecs, "%[^,]", fmt) > 0) {
			codecs = strchr(codecs, ',');
			if (codecs) codecs++;

			if (strstr(fmt, "wav")) mimetype = _lookup(mimetypes, NULL, 3, "audio/wav", "audio/x-wav", "audio/wave");
			else if (strstr(fmt, "aif")) mimetype = _lookup(mimetypes, NULL, 4, "audio/aiff", "audio/x-aiff", "audio/aif", "audio/x-aif");
		}

		return mimetype;
	}
	}

	return NULL;
}

/*---------------------------------------------------------------------------*/
char* mimetype_from_pcm(uint8_t* sample_size, bool truncable, uint32_t sample_rate,
						uint8_t channels, char* mimetypes[], char* codecs) {
						char* mimetype = NULL, fmt[8];
	uint8_t size = *sample_size;

	while (codecs && !mimetype && sscanf(codecs, "%[^,]", fmt) > 0) {
		codecs = strchr(codecs, ',');
		if (codecs) codecs++;

		while (strstr(fmt, "raw")) {
			char a[16], r[16], c[16];

			// find audio/Lxx
			sprintf(a, "audio/L%hhu", *sample_size);
			sprintf(r, "rate=%u", sample_rate);
			sprintf(c, "channels=%hhu", channels);
			for (char** p = mimetypes; *p; p++) {
				if (**p == '*' ||
					(strstr(*p, a) &&
						(!strstr(*p, "rate=") || strstr(*p, r)) &&
						(!strstr(*p, "channels=") || strstr(*p, c)))) {

					(void)! asprintf(&mimetype, "%s;%s;%s", a, r, c);
					return mimetype;
				}
			}

			// if we have no found anything with 24 bist, try 16 bits if authorized
			if (*sample_size == 24 && truncable) {
				*sample_size = 16;
			} else {
				*sample_size = size;
				break;
			}
		}

		if (strstr(fmt, "wav")) mimetype = _lookup(mimetypes, NULL, 3, "audio/wav", "audio/x-wav", "audio/wave");
		else if (strstr(fmt, "aif")) mimetype = _lookup(mimetypes, NULL, 4, "audio/aiff", "audio/x-aiff", "audio/aif", "audio/x-aif");
	}

	return mimetype;
}

/*---------------------------------------------------------------------------*/
char mimetype_to_format(char *mimetype) {
	char* p;

	if ((p = strstr(mimetype, "audio/x-")) != NULL) p += strlen("audio/x-");
	else if ((p = strstr(mimetype, "audio/")) != NULL) p += strlen("audio/");
	else if ((p = strstr(mimetype, "application/")) != NULL) p += strlen("application/");

	if (strstr(p, "wav")) return 'w';
	if (strstr(p, "aif")) return 'i';
	if (p[0] == 'L' || p[0] == '*') return 'p';
	if (strstr(p, "flac") || strstr(p, "flc")) return 'f';
	if (strstr(p, "mp3") || strstr(p, "mpeg")) return 'm';
	if (strstr(p, "ogg")) return 'o';
	if (strstr(p, "aac")) return 'a';
	if (strstr(p, "mp4") || strstr(p, "m4a")) return '4';
	if (strstr(p, "dsd") || strstr(p, "dsf") || strstr(p, "dff")) return 'd';

	return '\0';
}

/*---------------------------------------------------------------------------*/
char* format_to_dlna(char format, bool full_cache, bool live) {
	char* buf, * DLNAOrgPN;

	switch (format) {
	case 'm':
		DLNAOrgPN = "DLNA.ORG_PN=MP3;";
		break;
	case 'A':
	case 'a':
		DLNAOrgPN = "DLNA.ORG_PN=AAC_ADTS;";
		break;
	case 'p':
		DLNAOrgPN = "DLNA.ORG_PN=LPCM;";
		break;
	default:
		DLNAOrgPN = "";
	}

	/* OP set means that the full resource must be accessible, but Sn can still increase. It
	 * is exclusive with b29 (DLNA_ORG_FLAG_BYTE_BASED_SEEK) of FLAGS which is limited random 
	 * access and when that is set, player shall not expect full access to already received 
	 * bytes and for example, S0 can increase (it does not have to). When live is set, either 
	 * because we have no duration (it's a webradio) or we are in flow, we have to set S0 
	 * because we lose track of the head and we can't have full cache. 
	 * The value for Sn is questionable as it actually changes only for live stream but we 
	 * don't have access to it until we have received full content. As it is supposed to 
	 * represent what is accessible, not the media, we'll always set it. We can still use
	 * in-memory cache, so b29 shall be set (then OP shall not be). If user has opted-out 
	 * file-cache, we can only do b29. In any case, we don't support time-based seek */

	uint32_t org_op = full_cache ? DLNA_ORG_OPERATION_RANGE : 0;
	uint32_t org_flags = DLNA_ORG_FLAG_STREAMING_TRANSFERT_MODE | DLNA_ORG_FLAG_BACKGROUND_TRANSFERT_MODE |
					 	 DLNA_ORG_FLAG_CONNECTION_STALL | DLNA_ORG_FLAG_DLNA_V15 |
						 DLNA_ORG_FLAG_SN_INCREASE;

	if (live) org_flags |= DLNA_ORG_FLAG_S0_INCREASE;
	if (!full_cache) org_flags |= DLNA_ORG_FLAG_BYTE_BASED_SEEK;

	(void)!asprintf(&buf, "%sDLNA.ORG_OP=%02u;DLNA.ORG_CI=0;DLNA.ORG_FLAGS=%08x000000000000000000000000",
						   DLNAOrgPN, org_op, org_flags);

	return buf;
}

/*---------------------------------------------------------------------------*/
char* mimetype_to_ext(char* mimetype) {
	char* p;

	if (!mimetype) return "";
	if ((p = strstr(mimetype, "audio/x-")) != NULL) p += strlen("audio/x-");
	else if ((p = strstr(mimetype, "audio/")) != NULL) p += strlen("audio/");
	else if ((p = strstr(mimetype, "application/")) != NULL) p += strlen("application/");
	else return "";

	if (strstr(mimetype, "wav") == p) return "wav";
	if (strstr(mimetype, "L") == p || mimetype[0] == '*') return "pcm";
	if (strstr(mimetype, "flac") == p) return "flac";
	if (strstr(mimetype, "flc") == p) return "flc";
	if (strstr(mimetype, "mp3") == p || strstr(mimetype, "mpeg") == p) return "mp3";
	if (strstr(mimetype, "ogg") == p && strstr(mimetype, "codecs=opus")) return "ops";
	if (strstr(mimetype, "ogg") == p && strstr(mimetype, "codecs=flac")) return "ogg";
	if (strstr(mimetype, "ogg") == p) return "ogg";
	if (strstr(mimetype, "aif") == p) return "aif";
	if (strstr(mimetype, "aac") == p) return "aac";
	if (strstr(mimetype, "mp4") == p) return "mp4";
	if (strstr(mimetype, "m4a") == p) return "m4a";
	if (strstr(mimetype, "dsd") == p) return "dsd";
	if (strstr(mimetype, "dsf") == p) return "dsf";
	if (strstr(mimetype, "dff") == p) return "dff";

	return "nil";
}
