#ifndef __AVT_UTIL_H
#define __AVT_UTIL_H

#include "util_common.h"

void AVTInit(log_level level);
int AVTSetURI(char *ControlURL, char *URI, char *ProtocolInfo, void *Cookie);
int AVTSetNextURI(char *ControlURL, char *URI, char *ProtocolInfo, void *Cookie);
int AVTCallAction(char *ControlURL, char *Var, void *Cookie);
int AVTPlay(char *ControlURL, void *Cookie);
int SetVolume(char *ControlURL, u8_t Volume, void *Cookie);
int AVTSeek(char *ControlURL, unsigned Interval, void *Cookie);
int AVTBasic(char *ControlURL, char *Action, void *Cookie);
int GetProtocolInfo(char *ControlURL, void *Cookie);

#endif

