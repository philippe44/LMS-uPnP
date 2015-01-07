/*
 *  Squeeze2upnp - LMS to uPNP gateway
 *
 *  Squeezelite : (c) Adrian Smith 2012-2014, triode1@btinternet.com
 *  Additions & gateway : (c) Philippe 2014, philippe_44@outlook.com
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

#include <sys/stat.h>

#include "squeezedefs.h"
#include "squeeze2upnp.h"
#include "webserver.h"

static log_level	loglevel = lWARN;

int WebGetInfo(const char *FileName, struct File_Info *Info)
{
#ifdef TEST_IDX_BUF
	FILE *Handle;
#endif
	struct stat Status;
	void *Ref;

#ifdef TEST_IDX_BUF
	if (strstr(FileName, "__song__.mp3")) {
		Handle = fopen("__song__.mp3", "rb");
		fstat(fileno(Handle), &Status);
		Status.st_size = -3;
		fclose(Handle);
	}
	else
#endif
	{
		Status.st_ctime = 0;
#if 0
		Device = (struct sMR*) sq_urn2MR(FileName);
		// some uPnP device have a long memory of this ...
		if (!Device) {
			LOG_ERROR("No context for %s", FileName);
			return UPNP_E_INVALID_HANDLE;
		}
		Status.st_size = Device->Config.StreamLength;
#endif
	}

	Info->is_directory = false;
	Info->is_readable = true;
	Info->last_modified = Status.st_ctime;
	// Info->file_length = Status.st_size;
	Ref = sq_get_info(FileName, &Info->file_length, &Info->content_type);
#ifdef TEST_IDX_BUF
	if (!strcmp(Info->content_type, "audio/unknown"))
		Info->content_type = strdup("audio/mpeg");
#endif
	LOG_INFO("[%p]: GetInfo %s %s", Ref, FileName, Info->content_type);

#if 0
	{
	struct Extra_Headers *Headers = Info->extra_headers;
	while (Headers->name) {
		if (strstr(Headers->name, "Icy")) {
			Headers->resp = strdup("icy:blabla");
		}
		Headers++;
	}
	}
#endif

	// Some clients try to open 2 sessions : do not allow that
	if (sq_isopen(FileName)) return -1;
	else return UPNP_E_SUCCESS;
}

UpnpWebFileHandle WebOpen(const char *FileName, enum UpnpOpenFileMode Mode)
{
	void *p;

#ifdef TEST_IDX_BUF
	if (strstr(FileName, "__song__.mp3"))
	   p = (void*) fopen("__song__.mp3", "rb");

	else
#endif
	{
		p = sq_open(FileName);
		if (!p) {
			LOG_ERROR("No context for %s", FileName);
		}
	 }

	LOG_DEBUG("Webserver Open %s", FileName);
	return (UpnpWebFileHandle) p;
}

int WebWrite(UpnpWebFileHandle FileHandle, char *buf, size_t buflen)
{
	return 0;
}

int WebRead(UpnpWebFileHandle FileHandle, char *buf, size_t buflen)
{
	int read;

	if (!FileHandle) return 0;

	read = sq_read(FileHandle, buf, buflen);
#ifdef TEST_IDX_BUF
	if (read == -1) read = fread(buf, 1, buflen, FileHandle);
#endif
	LOG_DEBUG("read %d on %d", read, buflen);

	return read;
}

/*---------------------------------------------------------------------------*/
int WebSeek(UpnpWebFileHandle FileHandle, off_t offset, int origin)
{
	int rc;

	if (!FileHandle) return -1;

	rc = sq_seek(FileHandle, offset, origin);
	if (rc == -1) rc = fseek(FileHandle, offset, origin);

	LOG_DEBUG("[%p]: seek %d, %d (rc:%d)", FileHandle, offset, origin, rc);
	return rc;
}

/*---------------------------------------------------------------------------*/
int WebClose(UpnpWebFileHandle FileHandle)
{
	if (!FileHandle) return -1;

	LOG_DEBUG("webserver close", NULL);
#ifdef TEST_IDX_BUF
	if (!sq_close(FileHandle)) fclose(FileHandle);
#else
	sq_close(FileHandle);
#endif
	return UPNP_E_SUCCESS;
}

/*---------------------------------------------------------------------------*/
void WebServerLogLevel(log_level level)
{
	LOG_WARN("webserver change loglevel %d", level);
	loglevel = level;
}


