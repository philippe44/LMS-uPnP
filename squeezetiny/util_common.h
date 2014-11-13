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

#define LOG_ERROR(fmt, ...) logprint("%s %s:%d " fmt "\n", logtime(), __FUNCTION__, __LINE__, ##__VA_ARGS__)
#define LOG_WARN(fmt, ...)  if (loglevel >= lWARN)  logprint("%s %s:%d " fmt "\n", logtime(), __FUNCTION__, __LINE__, ##__VA_ARGS__)
#define LOG_INFO(fmt, ...)  if (loglevel >= lINFO)  logprint("%s %s:%d " fmt "\n", logtime(), __FUNCTION__, __LINE__, ##__VA_ARGS__)
#define LOG_DEBUG(fmt, ...) if (loglevel >= lDEBUG) logprint("%s %s:%d " fmt "\n", logtime(), __FUNCTION__, __LINE__, ##__VA_ARGS__)
#define LOG_SDEBUG(fmt, ...) if (loglevel >= lSDEBUG) logprint("%s %s:%d " fmt "\n", logtime(), __FUNCTION__, __LINE__, ##__VA_ARGS__)

#endif