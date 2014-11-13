#include <signal.h>
#include <stdarg.h>
#include <stdio.h>

#include "upnp.h"
#include "UpnpString.h"
#include "upnptools.h"
#include "util_common.h"
#include "util.h"

#ifndef __WEBSERVER_H
#define __WEBSERVER_H


int WebGetInfo(const char *FileName, struct File_Info *Info);
UpnpWebFileHandle WebOpen(const char *FileName, enum UpnpOpenFileMode Mode);
int WebRead(UpnpWebFileHandle FileHandle, char *buf, size_t buflen);
int WebWrite(UpnpWebFileHandle FileHandle, char *buf, size_t buflen);
int WebSeek(UpnpWebFileHandle FileHandle, off_t offset, int origin);
int WebClose(UpnpWebFileHandle FileHandle);
void WebServerLogLevel(log_level level);

extern char glBaseVDIR[];

#endif
