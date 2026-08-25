/*******************************************************************
                   MPEG Audio stream read interface
 *******************************************************************/

#ifndef AUDIO_STREAM_H
#define AUDIO_STREAM_H

#include <stdint.h>

typedef struct {
	__int64 sample;
	int     frequency;
	int     channel;
} AUDIO_INFO;

typedef struct {
	intptr_t      stream;
	
	void         (* close)(void *audio_stream);
	__int64      (* tell)(intptr_t stream);
	__int64      (* seek)(intptr_t stream, __int64 sample);
	int          (* read)(intptr_t stream, void *buffer, int size);
	unsigned int (* next_sync)(intptr_t stream);
	void         (* get_info)(intptr_t stream, AUDIO_INFO *info);
} AUDIO_STREAM;

#ifdef __cplusplus
extern "C" {
#endif

extern AUDIO_STREAM *audio_stream_open(char *path);

#ifdef __cplusplus
}
#endif

#endif /* AUDIO_STREAM_H */
