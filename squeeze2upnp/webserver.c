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
	FILE *Handle;
	struct stat Status;
	struct sMR *Device;

#ifdef __FLAC
	if (strstr(FileName, "__song__.flac")) {
		Handle = (void*) fopen("__song__.flac", "rb");
#else
	if (strstr(FileName, "__song__.mp3")) {
		Handle = fopen("__song__.mp3", "rb");
#endif
		fstat(fileno(Handle), &Status);
		Status.st_size = -3;
		fclose(Handle);
	}
	else
	{
		Status.st_ctime = 0;
		Device = (struct sMR*) sq_urn2MR(FileName);
		// some uPnP device have a long memory of this ...
		if (!Device) {
			LOG_ERROR("No context for %s", FileName);
			return UPNP_E_INVALID_HANDLE;
		}
		Status.st_size = Device->Config.StreamLength;
	}

	Info->is_directory = false;
	Info->is_readable = true;
	Info->last_modified = Status.st_ctime;
	Info->file_length = Status.st_size;
	Info->content_type = sq_content_type(FileName);
	if (!strcmp(Info->content_type, "audio/unknown"))
#ifdef __FLAC
		Info->content_type = strdup("audio/flac");
#else
		Info->content_type = strdup("audio/mpeg");
#endif
	LOG_DEBUG("Webserver GetInfo %s %s", FileName, Info->content_type);

	return UPNP_E_SUCCESS;
}

UpnpWebFileHandle WebOpen(const char *FileName, enum UpnpOpenFileMode Mode)
{
	void *p;

#ifdef __FLAC
	if (strstr(FileName, "__song__.flac"))
		p = (void*) fopen("__song__.fac", "rb");
#else
	if (strstr(FileName, "__song__.mp3"))
	   p = (void*) fopen("__song__.mp3", "rb");
#endif
	else {
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
	if (read == -1) read = fread(buf, 1, buflen, FileHandle);

	LOG_DEBUG("read %d on %d", read, buflen);

	return read;
}

/*---------------------------------------------------------------------------*/
int WebSeek(UpnpWebFileHandle FileHandle, off_t offset, int origin)
{
	int seek;

	if (!FileHandle) return -1;

	if (offset < 0) {
		LOG_WARN("[%p]: no negative seek %d, %d", FileHandle, offset, origin);
		return -1;
	}

	seek = sq_seek(FileHandle, (unsigned) offset, origin);
	if (seek == -1) seek = fseek(FileHandle, offset, origin);

	LOG_INFO("[%p]: seek %d, %d", FileHandle, offset, origin);
	return seek;
}

/*---------------------------------------------------------------------------*/
int WebClose(UpnpWebFileHandle FileHandle)
{
	if (!FileHandle) return -1;

	LOG_DEBUG("webserver close", NULL);
	if (!sq_close(FileHandle)) fclose(FileHandle);
	return UPNP_E_SUCCESS;
}

/*---------------------------------------------------------------------------*/
void WebServerLogLevel(log_level level)
{
	LOG_WARN("webserver change loglevel %d", level);
	loglevel = level;
}


