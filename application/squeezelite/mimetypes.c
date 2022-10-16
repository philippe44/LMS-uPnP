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

#define DLNA_ORG_OP (DLNA_ORG_OPERATION_RANGE)

#define DLNA_ORG_FLAG ( DLNA_ORG_FLAG_S0_INCREASE | DLNA_ORG_FLAG_STREAMING_TRANSFERT_MODE | \
                        DLNA_ORG_FLAG_BACKGROUND_TRANSFERT_MODE | DLNA_ORG_FLAG_CONNECTION_STALL | \
                        DLNA_ORG_FLAG_DLNA_V15 )

/*----------------------------------------------------------------------------*/
static char* _lookup(char* mimetypes[], char* details, int n, ...) {
	char* mimetype, ** p;
	va_list args;

	va_start(args, n);

	while (n--) {
		mimetype = va_arg(args, char*);
		p = mimetypes;
		while (*p) {
			if (**p == '*' || (strstr(*p, mimetype) && (!details || strstr(*p, details)))) {
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
char* mimetype_from_codec(char codec, char* mimetypes[], char* options) {
	switch (codec) {
	case 'm': return _lookup(mimetypes, NULL, 3, "audio/mp3", "audio/mpeg", "audio/mpeg3");
	case 'w': return _lookup(mimetypes, NULL, 2, "audio/wma", "audio/x-wma");
	case 'o': return _lookup(mimetypes, NULL, 2, "audio/ogg", "audio/x-ogg");
	case 'u': return _lookup(mimetypes, "codecs=opus", 2, "audio/ogg", "audio/x-ogg");
	case 'a': return _lookup(mimetypes, NULL, 4, "audio/aac", "audio/x-aac", "audio/m4a", "audio/mp4");
	case 'l': return _lookup(mimetypes, NULL, 2, "audio/m4a", "audio/mp4");
	case 'c':
	case 'f':
		if (options && !strcmp(options, "ogg")) return _lookup(mimetypes, "codecs=flac", 2, "audio/ogg", "audio/x-ogg");
		else return _lookup(mimetypes, NULL, 2, "audio/flac", "audio/x-flac");
	case 'd': {
		char* mimetype;

		if (strstr(options, "dsf")) {
			mimetype = _lookup(mimetypes, NULL, 2, "audio/dsf", "audio/x-dsf");
			if (mimetype) return mimetype;
		}

		if (strstr(options, "dff")) {
			mimetype = _lookup(mimetypes, NULL, 2, "audio/dff", "audio/x-dff");
			if (mimetype) return mimetype;
		}

		mimetype = _lookup(mimetypes, NULL, 2, "audio/dsd", "audio/x-dsd");
		if (mimetype) return mimetype;
		else return strdup("audio/dsd");
	}
	case 'p': {
		char fmt[8];
		char* mimetype;

		while (1) {
			if (sscanf(options, "%[^,]", fmt) <= 0) return NULL;

			if (strstr(fmt, "wav")) {
				mimetype = _lookup(mimetypes, NULL, 3, "audio/wav", "audio/x-wav", "audio/wave");
				if (mimetype) return mimetype;
			}

			if (strstr(fmt, "aif")) {
				mimetype = _lookup(mimetypes, NULL, 4, "audio/aiff", "audio/x-aiff", "audio/aif", "audio/x-aif");
				if (mimetype) return mimetype;
			}

			options += strlen(fmt);
			if (*options) options++;
		}
	}
	}

	return NULL;
}

/*---------------------------------------------------------------------------*/
char* mimetype_from_pcm(uint8_t* sample_size, bool truncable, uint32_t sample_rate,
	uint8_t channels, char* mimetypes[], char* options) {
	char* mimetype, fmt[8];
	uint8_t size = *sample_size;

	while (1) {

		if (sscanf(options, "%[^,]", fmt) <= 0) return NULL;

		while (strstr(fmt, "raw")) {
			char** p, a[16], r[16], c[16];

			// find audio/Lxx
			p = mimetypes;
			sprintf(a, "audio/L%hhu", *sample_size);
			sprintf(r, "rate=%u", sample_rate);
			sprintf(c, "channels=%hhu", channels);
			while (*p) {
				if (**p == '*' ||
					(strstr(*p, a) &&
						(!strstr(*p, "rate=") || strstr(*p, r)) &&
						(!strstr(*p, "channels=") || strstr(*p, c)))) {
					char* rsp;

					(void)! asprintf(&rsp, "%s;%s;%s", a, r, c);
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
			mimetype = _lookup(mimetypes, NULL, 3, "audio/wav", "audio/x-wav", "audio/wave");
			if (mimetype) return mimetype;
		}

		if (strstr(fmt, "aif")) {
			mimetype = _lookup(mimetypes, NULL, 4, "audio/aiff", "audio/x-aiff", "audio/aif", "audio/x-aif");
			if (mimetype) return mimetype;
		}

		// try next one
		options += strlen(fmt);
		if (*options) options++;
	}
}

/*---------------------------------------------------------------------------*/
char* mimetype_to_dlna(char* mimetype, uint32_t duration) {
	char* buf;
	char* DLNAOrgPN;

	switch (mimetype_to_format(mimetype)) {
	case 'm':
		DLNAOrgPN = "DLNA.ORG_PN=MP3;";
		break;
	case 'p':
		DLNAOrgPN = "DLNA.ORG_PN=LPCM;";
		break;
	default:
		DLNAOrgPN = "";
	}

	(void)!asprintf(&buf, "%sDLNA.ORG_OP=00;DLNA.ORG_CI=0;DLNA.ORG_FLAGS=%08x000000000000000000000000",
		DLNAOrgPN, DLNA_ORG_FLAG | (duration ? 0 : DLNA_ORG_FLAG_SN_INCREASE));

	return buf;
}


/*---------------------------------------------------------------------------*/
char* mimetype_to_ext(char* mimetype)
{
	char* p;

	if (!mimetype) return "";
	if ((p = strstr(mimetype, "audio/x-")) != NULL) p += strlen("audio/x-");
	else if ((p = strstr(mimetype, "audio/")) != NULL) p += strlen("audio/");
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

/*---------------------------------------------------------------------------*/
char mimetype_to_format(char* mimetype)
{
	if (!mimetype) return '\0';

	if (strstr(mimetype, "wav")) return 'w';
	if (strstr(mimetype, "audio/L") || mimetype[0] == '*') return 'p';
	if (strstr(mimetype, "flac") || strstr(mimetype, "flc")) return 'f';
	if (strstr(mimetype, "mp3") || strstr(mimetype, "mpeg")) return 'm';
	if (strstr(mimetype, "ogg") && strstr(mimetype, "codecs=opus")) return 'u';
	if (strstr(mimetype, "ogg") && strstr(mimetype, "codecs=flac")) return 'f';
	if (strstr(mimetype, "ogg")) return 'o';
	if (strstr(mimetype, "aif")) return 'i';
	if (strstr(mimetype, "aac")) return 'a';
	if (strstr(mimetype, "mp4") || strstr(mimetype, "m4a")) return '4';
	if (strstr(mimetype, "dsd") || strstr(mimetype, "dsf") || strstr(mimetype, "dff")) return 'd';

	return '*';
}