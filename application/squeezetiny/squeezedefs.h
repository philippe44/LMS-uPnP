/*
 *  Squeeze2xxx - squeezebox emulator gateway
 *
 *  (c) Adrian Smith 2012-2014, triode1@btinternet.com
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

#ifndef __SQUEEZEDEFS_H
#define __SQUEEZEDEFS_H

#define VERSION "v1.64.0"" ("__DATE__" @ "__TIME__")"

#include "platform.h"

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

#if LINUX || OSX || FREEBSD
#include <sys/types.h>
#include <unistd.h>
#include <stdbool.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <sys/time.h>
#include <sys/socket.h>
#include <poll.h>
#include <dlfcn.h>
#include <pthread.h>
#include <signal.h>

#define STREAM_THREAD_STACK_SIZE  64 * 1024
#define OUTPUT_THREAD_STACK_SIZE  64 * 1024
#define SLIMPROTO_THREAD_STACK_SIZE  64 * 1024
#define DECODE_THREAD_STACK_SIZE (1024 * 128)
#define thread_t pthread_t;
#define closesocket(s) close(s)
#define mutex_create_p(m) pthread_mutexattr_t attr; pthread_mutexattr_init(&attr); pthread_mutexattr_setprotocol(&attr, PTHREAD_PRIO_INHERIT); pthread_mutex_init(&m, &attr); pthread_mutexattr_destroy(&attr)

#endif

#if WIN

#include <winsock2.h>
#include <ws2tcpip.h>
#include <io.h>
#include <stdbool.h>
#include <sys/timeb.h>
#include "pthread.h"

#define STREAM_THREAD_STACK_SIZE (1024 * 64)
#define DECODE_THREAD_STACK_SIZE (1024 * 128)
#define OUTPUT_THREAD_STACK_SIZE (1024 * 64)
#define SLIMPROTO_THREAD_STACK_SIZE  (1024 * 64)
#define mutex_create_p(m) pthread_mutex_init(&m, NULL)

#define inline __inline

#endif

#define mutex_type pthread_mutex_t
#define mutex_create(m) pthread_mutex_init(&m, NULL)
#define mutex_lock(m) pthread_mutex_lock(&m)
#define mutex_unlock(m) pthread_mutex_unlock(&m)
#define mutex_trylock(m) pthread_mutex_trylock(&m)
#define mutex_destroy(m) pthread_mutex_destroy(&m)
#define thread_type pthread_t
#define mutex_timedlock(m, t) _mutex_timedlock(&m, t)
int _mutex_timedlock(mutex_type *m, u32_t wait);

#endif     // __SQUEEZEDEFS_H
