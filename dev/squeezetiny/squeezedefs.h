#ifndef __SQUEEZEDEFS_H
#define __SQUEEZEDEFS_H

//#define __EARLY_STMd__

#define VERSION "v0.2.2.2-a1"

#if defined(linux)
#define LINUX     1
#define OSX       0
#define WIN       0
#define FREEBSD   0
#elif defined (__APPLE__)
#define LINUX     0
#define OSX       1
#define WIN       0
#define FREEBSD   0
#elif defined (_MSC_VER) || defined(__BORLANDC__)
#define LINUX     0
#define OSX       0
#define WIN       1
#define FREEBSD   0
#elif defined(__FreeBSD__)
#define LINUX     0
#define OSX       0
#define WIN       0
#define FREEBSD   1
#else
#error unknown target
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
#define DECODE_THREAD_STACK_SIZE 32 * 1024
#define OUTPUT_THREAD_STACK_SIZE  64 * 1024
#define SLIMPROTO_THREAD_STACK_SIZE  64 * 1024
#define thread_t pthread_t;
#define closesocket(s) close(s)
#define last_error() errno
#define ERROR_WOULDBLOCK EWOULDBLOCK

typedef u_int8_t  u8_t;
typedef u_int16_t u16_t;
typedef u_int32_t u32_t;
typedef u_int64_t u64_t;
typedef int16_t   s16_t;
typedef int32_t   s32_t;
typedef int64_t   s64_t;

#define min(a,b) (((a) < (b)) ? (a) : (b))
#define max(a,b) (((a) > (b)) ? (a) : (b))

#define mutex_type pthread_mutex_t
#define mutex_create(m) pthread_mutex_init(&m, NULL)
#define mutex_create_p(m) pthread_mutexattr_t attr; pthread_mutexattr_init(&attr); pthread_mutexattr_setprotocol(&attr, PTHREAD_PRIO_INHERIT); pthread_mutex_init(&m, &attr); pthread_mutexattr_destroy(&attr)
#define mutex_lock(m) pthread_mutex_lock(&m)
#define mutex_unlock(m) pthread_mutex_unlock(&m)
#define mutex_destroy(m) pthread_mutex_destroy(&m)
#define thread_type pthread_t
int SendARP(in_addr_t src, in_addr_t dst, u8_t mac[], u32_t *size);
#define fresize(f,s) ftruncate(fileno(f), s)
char *strlwr(char *str);
char *GetTempPath(u16_t size, char *path);

#endif

#if WIN

#include <winsock2.h>
#include <ws2tcpip.h>
#include <io.h>
#include "pthread.h"

#define STREAM_THREAD_STACK_SIZE (1024 * 64)
#define DECODE_THREAD_STACK_SIZE (1024 * 128)
#define OUTPUT_THREAD_STACK_SIZE (1024 * 64)
#define SLIMPROTO_THREAD_STACK_SIZE  (1024 * 64)

typedef unsigned __int8  u8_t;
typedef unsigned __int16 u16_t;
typedef unsigned __int32 u32_t;
typedef unsigned __int64 u64_t;
typedef __int16 s16_t;
typedef __int32 s32_t;
typedef __int64 s64_t;

typedef BOOL bool;
#define true TRUE
#define false FALSE

#define inline __inline

#define mutex_type HANDLE
#define mutex_create(m) m = CreateMutex(NULL, FALSE, NULL)
#define mutex_create_p mutex_create
// in Windows, a mutex never locks if the owner tries to lock it
#define mutex_lock(m) WaitForSingleObject(m, INFINITE)
#define mutex_unlock(m) ReleaseMutex(m)
#define mutex_destroy(m) CloseHandle(m)
#define thread_type HANDLE

#define usleep(x) Sleep(x/1000)
#define sleep(x) Sleep(x*1000)
#define last_error() WSAGetLastError()
#define ERROR_WOULDBLOCK WSAEWOULDBLOCK
#define open _open
#define read _read
#define snprintf _snprintf
#define fresize(f, s) chsize(fileno(f), s)

#define in_addr_t u32_t
#define socklen_t int
#define ssize_t int

#define RTLD_NOW 0

#endif

#endif     // __SQUEEZEDEFS_H
