/*******************************************************************
                    Program Stream interfaces
 *******************************************************************/
#ifndef PROGRAM_STREAM_H
#define PROGRAM_STREAM_H

#include <stdint.h>
#include "stream_type.h"

#ifdef __cplusplus
extern "C" {
#endif

#ifndef PROGRAM_STREAM_C
extern intptr_t ps_open(const char *filename, int stream_type);
extern int ps_close(intptr_t in);
extern int ps_read(intptr_t in, void *data, unsigned int count);
extern __int64 ps_seek(intptr_t in, __int64 offset, int origin);
extern __int64 ps_tell(intptr_t in);
#endif
	
#ifdef __cplusplus
}
#endif

#endif	