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

u32_t gettime_ms(void);
char *url_encode(char *str);
char *url_decode(char *str);
char *stristr(char *s1, char *s2);
char *toxml(char *src);
u32_t hash32(char *str);
bool get_interface(struct in_addr *addr);
in_addr_t get_localhost(char **name);

#endif
