/*******************************************************************
                     Transport Stream interface
 *******************************************************************/
#ifndef TRANSPORT_STREAM_H
#define TRANSPORT_STREAM_H

#include <stdint.h>
#include "stream_type.h"

#ifdef __cplusplus
extern "C" {
#endif

#ifndef TRANSPROT_STREAM_C
extern intptr_t ts_open(const char *filename, int stream_type);
extern int ts_close(intptr_t in);
extern int ts_read(intptr_t in, void *data, unsigned int count);
extern __int64 ts_seek(intptr_t in, __int64 offset, int origin);
extern __int64 ts_tell(intptr_t in);
#endif

#ifdef __cplusplus
}
#endif

#endif