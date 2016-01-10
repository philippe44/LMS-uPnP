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

#ifndef __UTIL_COMMON_H
#define __UTIL_COMMON_H

#include "squeezedefs.h"

#define NFREE(p) if (p) { free(p); p = NULL; }
typedef enum { lERROR = 0, lWARN, lINFO, lDEBUG, lSDEBUG } log_level;

u32_t gettime_ms(void);
const char *logtime(void);
void logprint(const char *fmt, ...);
log_level debug2level(char *level);
char *level2debug(log_level level);
char *url_encode(char *str);
char *url_decode(char *str);
char *stristr(char *s1, char *s2);
char *toxml(char *src);
u32_t hash32(char *str);

#define LOG_ERROR(fmt, ...) logprint("%s %s:%d " fmt "\n", logtime(), __FUNCTION__, __LINE__, ##__VA_ARGS__)
#define LOG_WARN(fmt, ...)  if (*loglevel >= lWARN)  logprint("%s %s:%d " fmt "\n", logtime(), __FUNCTION__, __LINE__, ##__VA_ARGS__)
#define LOG_INFO(fmt, ...)  if (*loglevel >= lINFO)  logprint("%s %s:%d " fmt "\n", logtime(), __FUNCTION__, __LINE__, ##__VA_ARGS__)
#define LOG_DEBUG(fmt, ...) if (*loglevel >= lDEBUG) logprint("%s %s:%d " fmt "\n", logtime(), __FUNCTION__, __LINE__, ##__VA_ARGS__)
#define LOG_SDEBUG(fmt, ...) if (*loglevel >= lSDEBUG) logprint("%s %s:%d " fmt "\n", logtime(), __FUNCTION__, __LINE__, ##__VA_ARGS__)

#endif