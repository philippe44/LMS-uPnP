/*
 *  Squeeze2upnp - LMS to uPNP gateway
 *
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

#include <sys/stat.h>

#include "squeezedefs.h"
#include "squeeze2upnp.h"
#include "webserver.h"

extern log_level	web_loglevel;
static log_level	*loglevel = &web_loglevel;


/*---------------------------------------------------------------------------*/
int WebGetInfo(const char *FileName, struct File_Info *Info)
{
	struct stat Status;
	s32_t 		FileSize;
	u16_t		IcyInterval = 0;
	char 		*dlna_options, **ContentFeatures = NULL;
	struct Extra_Headers *Headers = Info->extra_headers;

	while (Headers->name) {
		LOG_INFO("Headers: %s: %s", Headers->name, Headers->value);

		if (stristr(Headers->name, "Icy-MetaData") && sq_is_remote(FileName)) {
			char *p = malloc(128);

			IcyInterval = 32000;
			sprintf(p, "icy-metaint:%d", IcyInterval);
			Headers->resp = p;
		}

		if (stristr(Headers->name, "transferMode.dlna.org")) {
			char *p = malloc(strlen(Headers->name) + strlen(Headers->value) + 3);

			sprintf(p, "%s: %s", Headers->name, Headers->value);
			Headers->resp = p;
		}

		if (stristr(Headers->name, "getcontentFeatures.dlna.org") && stristr(Headers->value ,"1")) {
			ContentFeatures = &Headers->resp;
		}

		/*
		if (stristr(Headers->name, "TimeSeekRange.dlna.org")) {
			u32_t h = 0, m = 0, s = 0, ms = 0;
			sq_dev_handle_t Handle = sq_urn2handle(FileName);

			if (strchr(Headers->value, ':'))
				sscanf(Headers->value,"npt=%d:%d:%d.%d", &h, &m, &s, &ms);
			else
				sscanf(Headers->value,"npt=%d.%d", &s, &ms);

			 sq_set_time(Handle, (h*3600 + m*60 + s)*1000 + ms);
			 return -1;
		}
		*/

		Headers++;
	}

	if(!sq_get_info(FileName, &FileSize, &Info->content_type, &dlna_options, IcyInterval)) {
		return UPNP_E_FILE_NOT_FOUND;
	}

	if (ContentFeatures) {
		char *dlna_rsp = "contentFeatures.dlna.org";

		*ContentFeatures = malloc(strlen(dlna_rsp) + strlen(dlna_options) + 1);
		sprintf(*ContentFeatures, "%s: %s", dlna_rsp, dlna_options);
	}
	free(dlna_options);

	Status.st_ctime 	= 0;
	Info->is_directory 	= false;
	Info->is_readable 	= true;
	Info->last_modified = Status.st_ctime;
	Info->file_length 	= FileSize;

	return UPNP_E_SUCCESS;
}


/*---------------------------------------------------------------------------*/
UpnpWebFileHandle WebOpen(const char *FileName, enum UpnpOpenFileMode Mode)
{
	void *p;

	p = sq_open(FileName);
	if (!p) {
		LOG_ERROR("No context for %s", FileName);
	}

	LOG_DEBUG("Webserver Open %s", FileName);
	return (UpnpWebFileHandle) p;
}


/*---------------------------------------------------------------------------*/
int WebWrite(UpnpWebFileHandle FileHandle, char *buf, size_t buflen)
{
	return 0;
}


/*---------------------------------------------------------------------------*/
int WebRead(UpnpWebFileHandle FileHandle, char *buf, size_t buflen)
{
	int read;

	if (!FileHandle) return 0;

	read = sq_read(FileHandle, buf, buflen);

	LOG_DEBUG("read %d on %d", read, buflen);
	return read;
}

/*---------------------------------------------------------------------------*/
int WebSeek(UpnpWebFileHandle FileHandle, off_t offset, int origin)
{
	int rc;

	if (!FileHandle) return -1;

	rc = sq_seek(FileHandle, offset, origin);

	LOG_DEBUG("[%p]: seek %d, %d (rc:%d)", FileHandle, offset, origin, rc);
	return rc;
}

/*---------------------------------------------------------------------------*/
int WebClose(UpnpWebFileHandle FileHandle)
{
	if (!FileHandle) return -1;

	sq_close(FileHandle);

	LOG_DEBUG("webserver close", NULL);
	return UPNP_E_SUCCESS;
}


