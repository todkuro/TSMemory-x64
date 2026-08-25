/*******************************************************************
                          resize module
 *******************************************************************/
#define RESIZE_C
#include <emmintrin.h>
#include "resize.h"

#include <math.h>
#include <stdlib.h>
#include <float.h>

#ifndef PI
#define PI (atan(1)*4)
#endif

/* grobal */
void resize(FRAME *in, FRAME *out, RESIZE_PARAMETER *prm);
RESIZE_PARAMETER *create_resize_parameter(SEQUENCE_HEADER *seq, M2V_CONFIG *cfg);
RESIZE_PARAMETER *create_force_resize_parameter(SEQUENCE_HEADER *seq, int width, int height);
void release_resize_parameter(RESIZE_PARAMETER *prm);

/* local */
static void setup_interpolation_parameter(int source_length, int result_length, COMPONENT_RESIZE_PARAMETER *out);
static void setup_decimation_parameter(int source_length, int result_length, COMPONENT_RESIZE_PARAMETER *out);
static double lanczos3_weight(double phase);

static void setup_crop_parameter(int source_length, COMPONENT_RESIZE_PARAMETER *r);

static void component_resize(unsigned char *in, unsigned char *out, COMPONENT_RESIZE_PARAMETER *prm);

/*-----------------------------------------------------------------*/
void resize(FRAME *in, FRAME *out, RESIZE_PARAMETER *prm)
{
	/* A damaged stream can present a different size after the parameter
	   was built. Then the row count, the row stride, the index table and
	   the chroma subsampling all disagree with the frame, and the reads
	   run off the plane. Nothing sane can be produced, so skip it. */
	if(prm->src_width != 0 && prm->src_height != 0
	   && (in->width != prm->src_width || in->height != prm->src_height)){
		return;
	}

	component_resize(in->y, out->y, &(prm->l));
	component_resize(in->u, out->u, &(prm->c));
	component_resize(in->v, out->v, &(prm->c));
}

/*-----------------------------------------------------------------*/
RESIZE_PARAMETER *create_resize_parameter(SEQUENCE_HEADER *seq, M2V_CONFIG *cfg)
{
	RESIZE_PARAMETER *r;

	int n;
	int src_width[2];
	
	int chroma_format;
	
	if(cfg->aspect_ratio == M2V_CONFIG_IGNORE_ASPECT_RATIO){
		return NULL;
	}

	r = (RESIZE_PARAMETER *)malloc(sizeof(RESIZE_PARAMETER));
	if(r == NULL){
		return NULL;
	}
	memset(r, 0, sizeof(RESIZE_PARAMETER));

	if(seq->has_sequence_display_extension){
		if(    (seq->sd.display_h_size == 0)
		    || (seq->sd.display_v_size == 0)
		    || (seq->sd.display_h_size > seq->h_size)
		    || (seq->sd.display_v_size > seq->v_size) ) {
			seq->sd.display_h_size = seq->h_size;
			seq->sd.display_v_size = seq->v_size;
		}
	}
	
	if(seq->has_sequence_display_extension){
		r->l.height = seq->sd.display_v_size;
	}else{
		r->l.height = seq->orig_v_size;
	}
	r->c.height = r->l.height;

	if(seq->has_sequence_display_extension){
		src_width[0] = seq->sd.display_h_size;
	}else{
		src_width[0] = seq->orig_h_size;
	}

	if(seq->has_sequence_extension){
		switch(seq->aspect_ratio){
		case 2: /* 4:3 */
			r->l.width = r->l.height * 4 / 3;
			break;
		case 3: /* 16:9 */
			r->l.width = r->l.height * 16 / 9;
			break;
		case 4:
			r->l.width = r->l.height * 221 / 100;
			break;
		default:
			r->l.width = src_width[0];
		}
		chroma_format = seq->se.chroma_format;
	}else{
		switch(seq->aspect_ratio){
		case 2: /* 0.6735 */
			r->l.width = src_width[0] * 10000 / 6735;
			break;
		case 3: /* 0.7031 */
			r->l.width = src_width[0] * 10000 / 7031;
			break;
		case 4: /* 0.7615 */
			r->l.width = src_width[0] * 10000 / 7615;
			break;
		case 5: /* 0.8055 */
			r->l.width = src_width[0] * 10000 / 8055;
			break;
		case 6: /* 0.8437 */
			r->l.width = src_width[0] * 10000 / 8437;
			break;
		case 7: /* 0.8935 */
			r->l.width = src_width[0] * 10000 / 8935;
			break;
		case 8: /* 0.9815 */
			r->l.width = src_width[0] * 10000 / 9815;
			break;
		case 9: /* 54:59 PAL */
			r->l.width = src_width[0] * 59 / 54;
			break; 
		case 10: /* 1.0255 */
			r->l.width = src_width[0] * 10000 / 10255;
			break;
		case 11: /* 1.0695 */
			r->l.width = src_width[0] * 10000 / 10695;
			break;
		case 12: /* 11:10 NTSC */
			r->l.width = src_width[0] * 10 / 11;
			break;
		case 13: /* 1.1575 */
			r->l.width = src_width[0] * 10000 / 11575;
			break;
		case 14: /* 1.2015 */
			r->l.width = src_width[0] * 10000 / 12015;
			break;
		default:
			r->l.width = src_width[0];
		}
		chroma_format = 1;
	}				
	
	if(r->l.width == seq->orig_h_size){
		free(r);
		return NULL;
	}

	/* width is restricted to the multiple of 2. */
	r->l.width += 1;
	r->l.width &= 0xfffffffe;

	if(chroma_format == 3){ /* YUV 444 */
		r->c.width = r->l.width;
		src_width[1] = src_width[0];
	}else{
		r->c.width = r->l.width/2;
		src_width[1] = src_width[0]/2;
	}

	r->l.in_step = seq->h_size;
	r->c.in_step = r->l.in_step;

	r->l.out_step = (r->l.width + 15) & 0xfffffff0;
	r->c.out_step = r->l.out_step;

	if(seq->has_sequence_display_extension){
		n = (seq->orig_v_size - seq->sd.display_v_size) / 2;
		r->l.in_offset = r->l.in_step * n;
		r->c.in_offset = r->l.in_offset;

		n = (seq->orig_h_size - seq->sd.display_h_size) / 2;
		r->l.in_offset += n;
		if(chroma_format == 3){
			r->c.in_offset += n;
		}else{
			r->c.in_offset += n/2;
		}
	}else{
		r->l.in_offset = 0;
		r->c.in_offset = 0;
	}

	r->l.out_offset = 0;
	
	if(chroma_format == 1){ /* YUV 420 */
		r->c.in_offset += r->l.in_step/2;
		r->c.out_offset = r->l.out_step / 2;
	}else{
		r->c.out_offset = 0;
	}
	
	if(r->l.width < src_width[0]){
		setup_decimation_parameter(src_width[0], r->l.width, &(r->l));
		setup_decimation_parameter(src_width[1], r->c.width, &(r->c));
	}else if(r->l.width == src_width[0]){
		setup_crop_parameter(src_width[0], &(r->l));
		setup_crop_parameter(src_width[1], &(r->c));
	}else{
		setup_interpolation_parameter(src_width[0], r->l.width, &(r->l));
		setup_interpolation_parameter(src_width[1], r->c.width, &(r->c));
	}

	/* remember what this parameter was built for (see resize()) */
	r->src_width = seq->h_size;
	r->src_height = seq->v_size;

	return r;
}

/*-----------------------------------------------------------------*/
RESIZE_PARAMETER *create_force_resize_parameter(SEQUENCE_HEADER *seq, int width, int height)
{
	RESIZE_PARAMETER *r;

	int n;
	int src_width[2];

	int chroma_format;

	r = (RESIZE_PARAMETER *)malloc(sizeof(RESIZE_PARAMETER));
	if(r == NULL){
		return NULL;
	}
	memset(r, 0, sizeof(RESIZE_PARAMETER));

	if(seq->has_sequence_display_extension){
		if(    (seq->sd.display_h_size == 0)
		    || (seq->sd.display_v_size == 0)
		    || (seq->sd.display_h_size > seq->h_size)
		    || (seq->sd.display_v_size > seq->v_size) ) {
			seq->sd.display_h_size = seq->h_size;
			seq->sd.display_v_size = seq->v_size;
		}
	}
	
	if(seq->has_sequence_display_extension){
		r->l.height = seq->sd.display_v_size;
	}else{
		r->l.height = seq->orig_v_size;
	}
	if(height < r->l.height){
		r->l.height = height;
	}
	r->c.height = r->l.height;
	
	if(seq->has_sequence_display_extension){
		src_width[0] = seq->sd.display_h_size;
	}else{
		src_width[0] = seq->orig_h_size;
	}
	r->l.width = width;
	if(r->l.width == seq->orig_h_size){
		free(r);
		return NULL;
	}

	/* width is restricted to the multiple of 2. */
	r->l.width += 1;
	r->l.width &= 0xfffffffe;

	if(seq->has_sequence_extension){
		chroma_format = seq->se.chroma_format;
	}else{
		chroma_format = 1;
	}

	if(chroma_format == 3){ /* YUV 444 */
		r->c.width = r->l.width;
		src_width[1] = src_width[0];
	}else{
		r->c.width = r->l.width/2;
		src_width[1] = src_width[0]/2;
	}
	
	r->l.in_step = seq->h_size;
	r->c.in_step = r->l.in_step;
	
	r->l.out_step = (r->l.width + 15) & 0xfffffff0;
	r->c.out_step = r->l.out_step;

	if(seq->has_sequence_display_extension){
		n = (seq->orig_v_size - seq->sd.display_v_size) / 2;
		r->l.in_offset = r->l.in_step * n;
		r->c.in_offset = r->l.in_offset;

		n = (seq->orig_h_size - seq->sd.display_h_size) / 2;
		r->l.in_offset += n;
		if(chroma_format == 3){
			r->c.in_offset += n;
		}else{
			r->c.in_offset += n/2;
		}
	}else{
		r->l.in_offset = 0;
		r->c.in_offset = 0;
	}

	r->l.out_offset = 0;
	
	if(chroma_format == 1){ /* YUV 420 */
		r->c.in_offset += r->l.in_step/2;
		r->c.out_offset = r->l.out_step / 2;
	}else{
		r->c.out_offset = 0;
	}
	
	if(r->l.width < src_width[0]){
		setup_decimation_parameter(src_width[0], r->l.width, &(r->l));
		setup_decimation_parameter(src_width[1], r->c.width, &(r->c));
	}else if(r->l.width == src_width[0]){
		setup_crop_parameter(src_width[0], &(r->l));
		setup_crop_parameter(src_width[1], &(r->c));
	}else{
		setup_interpolation_parameter(src_width[0], r->l.width, &(r->l));
		setup_interpolation_parameter(src_width[1], r->c.width, &(r->c));
	}

	/* remember what this parameter was built for (see resize()) */
	r->src_width = seq->h_size;
	r->src_height = seq->v_size;

	return r;
}

/*-----------------------------------------------------------------*/
void release_resize_parameter(RESIZE_PARAMETER *prm)
{
	int i;
	
	if(prm == NULL){
		return;
	}

	for(i=0;i<prm->l.length;i++){
		free(prm->l.weight[i]);
		free(prm->l.index[i]);
	}

	free(prm->l.index);
	free(prm->l.weight);
	free(prm->l.flat_weight);
	free(prm->l.flat_index);
	free(prm->l.start);
	free(prm->l.w16hi);
	free(prm->l.w16lo);

	for(i=0;i<prm->c.length;i++){
		free(prm->c.weight[i]);
		free(prm->c.index[i]);
	}

	free(prm->c.index);
	free(prm->c.weight);
	free(prm->c.flat_weight);
	free(prm->c.flat_index);
	free(prm->c.start);
	free(prm->c.w16hi);
	free(prm->c.w16lo);

	free(prm);
}

/*-----------------------------------------------------------------*/
static void setup_interpolation_parameter(int source_length, int result_length, COMPONENT_RESIZE_PARAMETER *out)
{
	int i,j,n;
	double *work;
	double  sum;
	double  pos;

	out->length = result_length;
	out->index = (int **)malloc(sizeof(int *)*out->length);
	out->weight = (int **)malloc(sizeof(int *)*out->length);
	out->tap = 6;

	for(i=0;i<result_length;i++){
		out->weight[i] = (int *)malloc(sizeof(int)*out->tap);
		out->index[i] = (int *)malloc(sizeof(int)*out->tap);
	}
	work = (double *)malloc(sizeof(double)*out->tap);

	__asm {emms};

	for(i=0;i<result_length;i++){
		pos = (i+0.5)*source_length;
		pos /= result_length;
		n = floor(pos-2.5);
		pos = (n+0.5-pos);
		sum = 0;
		for(j=0;j<out->tap;j++){
			if(n < 0){
				out->index[i][j] = 0;
			}else if(n >= source_length){
				out->index[i][j] = source_length-1;
			}else{
				out->index[i][j] = n;
			}
			work[j] = lanczos3_weight(pos);
			sum += work[j];
			pos += 1;
			n += 1;
		}

		for(j=0;j<out->tap;j++){
			out->weight[i][j] = (int)((work[j] / sum) * (1<<16));
		}
	}

	free(work);
}

/*-----------------------------------------------------------------*/
static void setup_decimation_parameter(int source_length, int result_length, COMPONENT_RESIZE_PARAMETER *out)
{
	int i,j,n;
	double *work;
	double  sum;
	double  pos, phase;

	out->length = result_length;
	out->weight = (int **)malloc(sizeof(int *)*out->length);
	out->index = (int **)malloc(sizeof(int *)*out->length);
	
	__asm {emms};

	out->tap = (6*(source_length)+(result_length-1)) / result_length;

	if((source_length % result_length) == 0){
		out->tap -= 1;
	}

	for(i=0;i<result_length;i++){
		out->weight[i] = (int *)malloc(sizeof(int)*out->tap);
		out->index[i] = (int *)malloc(sizeof(int)*out->tap);
	}
	work = (double *)malloc(sizeof(double)*out->tap);

	for(i=0;i<result_length;i++){
		pos = (i-3+0.5)*source_length/result_length + 0.5;
		n = floor(pos);
		sum = 0;
		for(j=0;j<out->tap;j++){
			phase = (n+0.5)*result_length;
			phase /= source_length;
			phase -= (i+0.5);
			if(n < 0){
				out->index[i][j] = 0;
			}else if(n >= source_length){
				out->index[i][j] = source_length-1;
			}else{
				out->index[i][j] = n;
			}
			work[j] = lanczos3_weight(phase);
			sum += work[j];
			n += 1;
		}

		for(j=0;j<out->tap;j++){
			out->weight[i][j] = (int)((work[j] / sum) * (1<<16));
		}
	}

	free(work);
}

/*-----------------------------------------------------------------*/
static double lanczos3_weight(double phase)
{
	double ret;
	
	if(fabs(phase) < DBL_EPSILON){
		return 1.0;
	}

	if(fabs(phase) >= 3.0){
		return 0.0;
	}

	ret = sin(PI*phase)*sin(PI*phase/3)/(PI*PI*phase*phase/3);

	return ret;
}

/*-----------------------------------------------------------------*/
static void setup_crop_parameter(int result_length, COMPONENT_RESIZE_PARAMETER *out)
{
	int i;

	out->length = result_length;
	out->index = (int **)malloc(sizeof(int *)*out->length);
	out->weight = (int **)malloc(sizeof(int *)*out->length);
	out->tap = 1;

	for(i=0;i<result_length;i++){
		out->weight[i] = (int *)malloc(sizeof(int));
		out->index[i] = (int *)malloc(sizeof(int));
	}

	for(i=0;i<result_length;i++){
		out->index[i][0] = i;
		out->weight[i][0] = 1<<16;
	}

	return;
}

/*-----------------------------------------------------------------*/
/* Flatten index/weight so the inner loop reads them sequentially.
   The original code did two pointer indirections per tap, which
   dominated the decoding time (measured: over 70%). */
static void build_fast_resize_table(COMPONENT_RESIZE_PARAMETER *prm)
{
	int x, i;
	const int tap = prm->tap;
	const int len = prm->length;
	/* reads have to stay inside one input row */
	const int limit = prm->in_step;

	prm->flat_weight = (int *)malloc(sizeof(int) * len * tap);
	prm->flat_index  = (int *)malloc(sizeof(int) * len * tap);
	prm->start       = (int *)malloc(sizeof(int) * len);
	/* 8 lanes per pixel, zero padded, for _mm_madd_epi16 */
	prm->w16hi       = (short *)malloc(sizeof(short) * len * 8);
	prm->w16lo       = (short *)malloc(sizeof(short) * len * 8);

	if(prm->w16hi != NULL && prm->w16lo != NULL){
		memset(prm->w16hi, 0, sizeof(short) * len * 8);
		memset(prm->w16lo, 0, sizeof(short) * len * 8);
	}

	if(prm->flat_weight == NULL || prm->flat_index == NULL || prm->start == NULL){
		free(prm->flat_weight); prm->flat_weight = NULL;
		free(prm->flat_index);  prm->flat_index  = NULL;
		free(prm->start);       prm->start       = NULL;
		free(prm->w16hi);       prm->w16hi       = NULL;
		free(prm->w16lo);       prm->w16lo       = NULL;
		return;
	}

	for(x=0;x<len;x++){
		int contiguous = 1;
		for(i=0;i<tap;i++){
			int idx = prm->index[x][i];
			if(idx < 0){
				idx = 0;
			}else if(limit > 0 && idx >= limit){
				idx = limit - 1;
			}
			prm->flat_weight[x*tap+i] = prm->weight[x][i];
			prm->flat_index [x*tap+i] = idx;
			if(idx != prm->flat_index[x*tap] + i){
				contiguous = 0;
			}
		}
		prm->start[x] = contiguous ? prm->flat_index[x*tap] : -1;
		if(prm->start[x] >= 0 && limit > 0 && prm->start[x] + tap > limit){
			prm->start[x] = -1;
		}

		if(prm->w16hi != NULL && prm->w16lo != NULL && tap <= 8){
			for(i=0;i<tap;i++){
				const int v = prm->weight[x][i];
				prm->w16hi[x*8+i] = (short)(v >> 8);
				prm->w16lo[x*8+i] = (short)(v & 255);
			}
		}
	}
}

/* 6 tap dot product with SSE2.
   weight = (w>>8)*256 + (w&255) ; both halves fit in int16 so the
   result is bit exact with the scalar version. */
static __inline int resize_dot_sse2(const unsigned char *p,
                                    const short *whi, const short *wlo)
{
	const __m128i zero = _mm_setzero_si128();
	__m128i pix = _mm_unpacklo_epi8(_mm_loadl_epi64((const __m128i *)p), zero);
	__m128i hi  = _mm_madd_epi16(pix, _mm_loadu_si128((const __m128i *)whi));
	__m128i lo  = _mm_madd_epi16(pix, _mm_loadu_si128((const __m128i *)wlo));
	__m128i acc = _mm_add_epi32(_mm_slli_epi32(hi, 8), lo);
	acc = _mm_add_epi32(acc, _mm_shuffle_epi32(acc, _MM_SHUFFLE(1,0,3,2)));
	acc = _mm_add_epi32(acc, _mm_shuffle_epi32(acc, _MM_SHUFFLE(2,3,0,1)));
	return _mm_cvtsi128_si32(acc);
}

static void component_resize(unsigned char *in, unsigned char *out, COMPONENT_RESIZE_PARAMETER *prm)
{
	int x,y;
	int i;
	int w;
	const int tap = prm->tap;
	const int width = prm->width;
	const int *fw;
	const int *fi;

	in += prm->in_offset;
	out += prm->out_offset;

	if(prm->flat_weight == NULL && width <= prm->length){
		build_fast_resize_table(prm);
	}

	{
	const int use_sse2 = (prm->w16hi != NULL && prm->w16lo != NULL);


	/* fast path : same arithmetic, sequential tables, direct reads */
	if(prm->flat_weight != NULL && width <= prm->length){
		for(y=0;y<prm->height;y++){
			fw = prm->flat_weight;
			fi = prm->flat_index;
			if(tap == 6){
				/* Lanczos3 upscale is always 6 taps */
				for(x=0;x<width;x++){
					const int s = prm->start[x];
					if(s >= 0 && use_sse2 && s + 8 <= prm->in_step){
						w = resize_dot_sse2(in + s,
						                    prm->w16hi + x*8, prm->w16lo + x*8);
					}else if(s >= 0){
						const unsigned char *p = in + s;
						w = p[0]*fw[0] + p[1]*fw[1] + p[2]*fw[2]
						  + p[3]*fw[3] + p[4]*fw[4] + p[5]*fw[5];
					}else{
						w = in[fi[0]]*fw[0] + in[fi[1]]*fw[1] + in[fi[2]]*fw[2]
						  + in[fi[3]]*fw[3] + in[fi[4]]*fw[4] + in[fi[5]]*fw[5];
					}
					w += 32768;
					out[x] = uchar_clip_table[UCHAR_CLIP_TABLE_OFFSET+(w>>16)];
					fw += 6;
					fi += 6;
				}
			}else{
				for(x=0;x<width;x++){
					const int s = prm->start[x];
					w = 0;
					if(s >= 0){
						const unsigned char *p = in + s;
						for(i=0;i<tap;i++){
							w += p[i] * fw[i];
						}
					}else{
						for(i=0;i<tap;i++){
							w += in[fi[i]] * fw[i];
						}
					}
					w += 32768;
					out[x] = uchar_clip_table[UCHAR_CLIP_TABLE_OFFSET+(w>>16)];
					fw += tap;
					fi += tap;
				}
			}
			out += prm->out_step;
			in += prm->in_step;
		}
		return;
	}
	}

	for(y=0;y<prm->height;y++){
		for(x=0;x<prm->width;x++){
			w = 0;
			for(i=0;i<prm->tap;i++){
				w += in[prm->index[x][i]] * prm->weight[x][i];
			}
			w += 32768;
			out[x] = uchar_clip_table[UCHAR_CLIP_TABLE_OFFSET+(w>>16)];
		}
		out += prm->out_step;
		in += prm->in_step;
	}
}

