/*
 *  mimetype tools
 *
 *	(c) Philippe, philippe_44@outlook.com
 *
 *  See LICENSE
 */

#include <stdint.h>

#define MAX_MIMETYPES	128
 
char* mimetype_from_codec(char codec, char* mimetypes[], char* options);
char* mimetype_from_pcm(uint8_t* sample_size, bool truncable, uint32_t sample_rate, uint8_t channels, char* mimetypes[], char* options);
char* mimetype_to_dlna(char* mimetype, uint32_t duration);
char* mimetype_to_ext(char* mimetype);
char  mimetype_to_format(char* mimetype);

