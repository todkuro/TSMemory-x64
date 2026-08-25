// by HDUSTest‚Ì’†‚Ìl
#ifndef SHARED_MEMORY_H
#define SHARED_MEMORY_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

intptr_t open_shared_memory(const char *name);
int shm_close(intptr_t id);
int shm_read(intptr_t id,void *buf,int length);
__int64 shm_tell(intptr_t id);
__int64 shm_seek(intptr_t id,__int64 offset,int origin);

#ifdef __cplusplus
}
#endif

#endif
