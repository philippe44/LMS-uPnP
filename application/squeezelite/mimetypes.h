/*
 *  mimetype tools
 *
 *	(c) Philippe, philippe_44@outlook.com
 *
 *  See LICENSE
 */

#include <stdint.h>

#define MAX_MIMETYPES	256
 
bool  mimetype_match_codec(char* mimetypes[], int n, ...);
char* mimetype_from_codec(char codec, char* mimetypes[], ...);
char* mimetype_from_pcm(uint8_t* sample_size, bool truncable, uint32_t sample_rate, uint8_t channels, char* mimetypes[], char* options);
char* mimetype_to_ext(char* mimetype);
char  mimetype_to_format(char* mimetype);
char* format_to_dlna(char format, bool live, bool full_cache);

