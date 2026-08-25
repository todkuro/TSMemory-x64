/*******************************************************************
                         resize interfaces
 *******************************************************************/
#ifndef RESIZE_H
#define RESIZE_H

#include "frame.h"
#include "config.h"
#include "sequence_header.h"

typedef struct {

	int height;
	int width;

	int in_step;
	int in_offset;
	
	int out_step;
	int out_offset;

	int length;
	int tap;

	int **index;
	int **weight;

	/* added by tools/patch64.py : flattened tables for speed */
	int  *flat_weight;  /* length*tap entries, contiguous */
	int  *flat_index;   /* ditto */
	int  *start;        /* first tap index, or -1 if not contiguous */
	short *w16hi;       /* weight >> 8   , 8 lanes per pixel */
	short *w16lo;       /* weight & 255  , 8 lanes per pixel */
	
} COMPONENT_RESIZE_PARAMETER;

typedef struct {
	COMPONENT_RESIZE_PARAMETER l; /* luminance   */
	COMPONENT_RESIZE_PARAMETER c; /* chrominance */
	int src_width;                /* what the parameter was built for */
	int src_height;
} RESIZE_PARAMETER;

#ifdef __cplusplus
extern "C" {
#endif

#ifndef RESIZE_C
extern void resize(FRAME *in, FRAME *out, RESIZE_PARAMETER *prm);
extern RESIZE_PARAMETER *create_resize_parameter(SEQUENCE_HEADER *seq, M2V_CONFIG *cfg);
extern RESIZE_PARAMETER *create_force_resize_parameter(SEQUENCE_HEADER *seq, int width, int height);
extern void release_resize_parameter(RESIZE_PARAMETER *prm);
#endif

#ifdef __cplusplus
}
#endif

#endif
