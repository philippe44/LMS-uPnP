/*
 *  Squeezelite bridges - core defines
 *
 *  (c) Adrian Smith 2012-2014, triode1@btinternet.com
 *	(c) Philippe, philippe_44@outlook.com
 *
 *  See LICENSE
 * 
 */

#pragma once

#define VERSION "v2.3.1"" ("__DATE__" @ "__TIME__")"

// hopefully we'll get rid of this one at some point
#define STR_LEN 256

#if defined(RESAMPLE) || defined(RESAMPLE_MP)
#undef  RESAMPLE
#define RESAMPLE  1 // resampling
#define PROCESS   1 // any sample processing (only resampling at present)
#else
#define RESAMPLE  0
#define PROCESS   0
#endif
#if defined(RESAMPLE_MP)
#undef RESAMPLE_MP
#define RESAMPLE_MP 1
#else
#define RESAMPLE_MP 0
#endif

#if defined(CODECS)
#undef CODECS
#define CODECS    1
#else
#define CODECS	  0
#endif

#if defined(FFMPEG)
#undef FFMPEG
#define FFMPEG    1
#else
#define FFMPEG    0
#endif

#if defined(USE_SSL)
#undef USE_SSL
#define USE_SSL 1
#else
#define USE_SSL 0
#endif

#if defined(LOOPBACK)
#undef LOOPBACK
#define LOOPBACK 1
#endif

#if defined(LINKALL)
#undef LINKALL
#define LINKALL   1 // link all libraries at build time - requires all to be available at run time
#else
#define LINKALL   0
#endif