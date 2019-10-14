/*
 * G729 codec for Asterisk
 *
 * For G.729(,A,B) royalty payments, see http://www.sipro.com 
 *
 *   WARNING: please make sure you are sitting down before looking
 *            at their price list.
 *
 * This source file is Copyright (C) 2004 Ready Technology Limited
 * This code is provided for educational purposes and is not warranted
 * to be fit for commercial use.  There is no warranty of any kind.
 * 
 * Author: daniel@readytechnology.co.uk
 */
#include "asterisk/lock.h"
#include "asterisk/translate.h"
#include "asterisk/module.h"
#include "asterisk/logger.h"
#include "asterisk/channel.h"

#include <pthread.h>
#include <fcntl.h>
#include <stdlib.h>
#include <unistd.h>
#include <netinet/in.h>
#include <string.h>
#include <stdio.h>

#include "g729a_v11/typedef.h"
#include "g729a_v11/basic_op.h"
#include "g729a_v11/ld8a.h"
#include "g729a_v11/tab_ld8a.h"
#include "g729a_v11/util.h"
#include "g729a_v11/pre_proc.h"

/* Sample frame data */
#include "slin_g729_ex.h"
#include "g729_slin_ex.h"

AST_MUTEX_DEFINE_STATIC(localuser_lock);

static int localusecnt=0;

static char *tdesc = "G729/PCM16 (signed linear) Codec Translator, based on ITU Reference code";

struct ast_translator_pvt {

	struct ast_frame f;
	
	CodState *coder;
	DecState *decoder;

	short pcm_buf[8000];
	unsigned char bitstream_buf[1000];

	int tail;
};

#define g729_coder_pvt ast_translator_pvt

static struct ast_translator_pvt *lintog729_new(void) {
	struct g729_coder_pvt *tmp;
	tmp = malloc(sizeof(struct g729_coder_pvt));
	if(tmp) {

		tmp->coder = Init_Coder_ld8a();
		Init_Pre_Process(tmp->coder);
		tmp->decoder = NULL;

		tmp->tail = 0;
		localusecnt++;
	}
	return tmp;
}

static struct ast_translator_pvt *g729tolin_new(void) {
	struct g729_coder_pvt *tmp;
	tmp = malloc(sizeof(struct g729_coder_pvt));
	if(tmp) {
		tmp->decoder = Init_Decod_ld8a();
		Init_Post_Filter(tmp->decoder);
		Init_Post_Process(tmp->decoder);
		tmp->coder = NULL;

		tmp->tail = 0;
		localusecnt++;
	}
	return tmp;
}

static struct ast_frame *lintog729_sample(void) {
	static struct ast_frame f;
	f.frametype = AST_FRAME_VOICE;
	f.subclass = AST_FORMAT_SLINEAR;
	f.datalen = sizeof(slin_g729_ex);
	f.samples = sizeof(slin_g729_ex) / 2;
	f.mallocd = 0;
	f.offset = 0;
	f.src = __PRETTY_FUNCTION__;
	f.data = slin_g729_ex;
	return &f;
}

static struct ast_frame *g729tolin_sample(void) {
	static struct ast_frame f;
	f.frametype = AST_FRAME_VOICE;
	f.subclass = AST_FORMAT_G729A;
	f.datalen = sizeof(g729_slin_ex);
	f.samples = 240;
	f.mallocd = 0;
	f.offset = 0;
	f.src = __PRETTY_FUNCTION__;
	f.data = g729_slin_ex;
	return &f;
}

/**
 * Retrieve a frame that has already been decompressed
 */
static struct ast_frame *g729tolin_frameout(struct ast_translator_pvt *tmp) {
	if(!tmp->tail)
		return NULL;
	tmp->f.frametype = AST_FRAME_VOICE;
	tmp->f.subclass = AST_FORMAT_SLINEAR;
	tmp->f.datalen = tmp->tail * 2;
	tmp->f.samples = tmp->tail;
	tmp->f.mallocd = 0;
	tmp->f.offset = AST_FRIENDLY_OFFSET;
	tmp->f.src = __PRETTY_FUNCTION__;
	tmp->f.data = tmp->pcm_buf;
	tmp->tail = 0;
	return &tmp->f;
}

/**
 * Accept a frame and decode it at the end of the current buffer
 */
static int g729tolin_framein(struct ast_translator_pvt *tmp, struct ast_frame *f) {
	int x;
	int frameSize = 0;

	for(x = 0; x < f->datalen; x += frameSize) {
		if((f->datalen - x) == 2)
			frameSize = 2;   /* VAD frame */
		else
			frameSize = 10;  /* Regular frame */

		if(tmp->tail + 80 < sizeof(tmp->pcm_buf) / 2) {

			{
				Word16 i;
				Word16 *synth;
				Word16 parm[PRM_SIZE + 1];
	    
				Restore_Params(f->data + x, &parm[1]);
				synth = tmp->decoder->synth_buf + M;

				parm[0] = 1;
				for (i = 0; i < PRM_SIZE; i++) {
					if (parm[i + 1] != 0) {
						parm[0] = 0;
						break;
					}
				}

				parm[4] = Check_Parity_Pitch(parm[3], parm[4]);	

				Decod_ld8a(tmp->decoder, parm, synth, tmp->decoder->Az_dec, tmp->decoder->T2, &tmp->decoder->bad_lsf);
				Post_Filter(tmp->decoder, synth, tmp->decoder->Az_dec, tmp->decoder->T2);
				Post_Process(tmp->decoder, synth, L_FRAME);

				memmove(tmp->pcm_buf + tmp->tail, synth, 2 * L_FRAME);
			}

			tmp->tail += 80;
		} else {
			ast_log(LOG_WARNING, "Out of G.729 buffer space\n");
			return -1;
		}
	}
	return 0;
}

static int lintog729_framein(struct ast_translator_pvt *tmp, struct ast_frame *f) {
	if(tmp->tail + f->datalen/2 < sizeof(tmp->pcm_buf) / 2) {
		memcpy((tmp->pcm_buf + tmp->tail), f->data, f->datalen);
		tmp->tail += f->datalen/2;
	} else {
		ast_log(LOG_WARNING, "Out of buffer space\n");
		return -1;
	}
	return 0;
}

static struct ast_frame *lintog729_frameout(struct ast_translator_pvt *tmp) {

	int x = 0;

	if(tmp->tail < 80)
		return NULL;
	tmp->f.frametype = AST_FRAME_VOICE;
	tmp->f.subclass = AST_FORMAT_G729A;
	tmp->f.mallocd = 0;
	tmp->f.offset = AST_FRIENDLY_OFFSET;
	tmp->f.src = __PRETTY_FUNCTION__;
	tmp->f.data = tmp->bitstream_buf;
	while(tmp->tail >= 80) {
		if((x+1) * 10 >= sizeof(tmp->bitstream_buf)) {
			ast_log(LOG_WARNING, "Out of buffer space\n");
			break;
		}

		{
			Word16 parm[PRM_SIZE];
		
			Copy ((Word16 *) tmp->pcm_buf, tmp->coder->new_speech, 80);
			Pre_Process(tmp->coder, tmp->coder->new_speech, 80);
			Coder_ld8a(tmp->coder, parm);
			Store_Params(parm, tmp->bitstream_buf + (x * 10));
		}

		tmp->tail -= 80;
		if(tmp->tail)
			memmove(tmp->pcm_buf, tmp->pcm_buf + 80, tmp->tail * 2);
		x++;
	}
	tmp->f.datalen = x * 10;
	tmp->f.samples = x * 80;

	return &(tmp->f);
}

static void g729_release(struct ast_translator_pvt *pvt) {
	if (pvt->coder)
		free (pvt->coder);
	if (pvt->decoder)
		free (pvt->decoder);
	localusecnt--;
}

static struct ast_translator g729tolin = {
	"g729tolin",
	AST_FORMAT_G729A, AST_FORMAT_SLINEAR,
	g729tolin_new,
	g729tolin_framein,
	g729tolin_frameout,
	g729_release,
	g729tolin_sample };

static struct ast_translator lintog729 = {
	"lintog729",
	AST_FORMAT_SLINEAR, AST_FORMAT_G729A,
	lintog729_new,
	lintog729_framein,
	lintog729_frameout,
	g729_release,
	lintog729_sample };

int load_module(void) {
	int res;
	res = ast_register_translator(&g729tolin);
	if(!res)
		res = ast_register_translator(&lintog729);
	else
		ast_unregister_translator(&g729tolin);
	return res;
}

int unload_module(void) {
	int res;
	ast_mutex_lock(&localuser_lock);
	res = ast_unregister_translator(&lintog729);
	if(!res)
		res = ast_unregister_translator(&g729tolin);
	if(localusecnt)
		res = -1;
	ast_mutex_unlock(&localuser_lock);
	return res;
}

char *description(void) {
	return tdesc;
}

int usecount(void) {
	int res;
	STANDARD_USECOUNT(res);
	return res;
}

char *key() {
	return ASTERISK_GPL_KEY;
}

