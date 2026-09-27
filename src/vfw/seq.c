/*
 * ICSeqCompressFrameStart / ICSeqCompressFrame / ICSeqCompressFrameEnd and
 * ICCompressorFree, after Wine's dlls/msvfw32/msvideo_main.c: compressing
 * a sequence of frames through a COMPVARS, with keyframes every lKey
 * frames, a per-frame size budget from lDataRate for codecs that can
 * meet one (VIDCF_CRUNCH), and the previous frame for temporal codecs
 * that don't keep their own (VIDCF_TEMPORAL without VIDCF_FASTTEMPORALC).
 *
 * The harness's encode command goes through the same code.
 */
#include <pthread.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "common/error.h"
#include "vfw/icm.h"

#define MAX_SEQS 64

/* What a started COMPVARS needs beyond its own fields. */
struct seq {
	COMPVARS *pc;		/* NULL when free */
	size_t out_size;	/* bytes of lpbiOut */
	DWORD max_frame;	/* capacity of lpBitsOut */
	DWORD in_frame;		/* bytes of an input frame */
	DWORD budget;		/* dwFrameSize for each frame, or 0 */
	int temporal;		/* give the codec the previous frame */
};

static pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;
static struct seq seqs[MAX_SEQS];

static struct seq *find(const COMPVARS *pc)
{
	struct seq *s = NULL;

	pthread_mutex_lock(&lock);
	for (int i = 0; i < MAX_SEQS && !s; i++)
		if (pc && seqs[i].pc == pc)
			s = &seqs[i];
	pthread_mutex_unlock(&lock);
	return s;
}

static struct seq *claim(COMPVARS *pc)
{
	struct seq *s = NULL;

	pthread_mutex_lock(&lock);
	for (int i = 0; i < MAX_SEQS && !s; i++)
		if (!seqs[i].pc) {
			s = &seqs[i];
			memset(s, 0, sizeof(*s));
			s->pc = pc;
		}
	pthread_mutex_unlock(&lock);
	return s;
}

static void release(struct seq *s)
{
	pthread_mutex_lock(&lock);
	s->pc = NULL;
	pthread_mutex_unlock(&lock);
}

/* An uncompressed frame's size from its header. */
static DWORD image_bytes(const BITMAPINFOHEADER *b)
{
	LONG h = b->biHeight < 0 ? -b->biHeight : b->biHeight;

	if (b->biCompression == BI_RGB || b->biCompression == BI_BITFIELDS)
		return (DWORD)(((uint64_t)b->biWidth * b->biBitCount + 31) / 32 *
			       4 * h);
	return b->biSizeImage;
}

/* Frees what ic_seq_start_ex allocated in pc. */
static void free_buffers(COMPVARS *pc)
{
	free(pc->lpbiIn);
	free(pc->lpbiOut);
	free(pc->lpBitsOut);
	free(pc->lpBitsPrev);
	pc->lpbiIn = pc->lpbiOut = NULL;
	pc->lpBitsOut = pc->lpBitsPrev = NULL;
}

BOOL ic_seq_start_ex(COMPVARS *pc, const BITMAPINFO *in, DWORD rate,
		     DWORD scale, LONG frames)
{
	char err[24];

	if (!pc || !in || pc->cbSize != sizeof(*pc) || !ic_valid(pc->hic)) {
		vfw_set_error("ICSeqCompressFrameStart: bad COMPVARS or codec");
		return FALSE;
	}
	if (find(pc))
		ic_seq_end(pc);

	const BITMAPINFOHEADER *ib = &in->bmiHeader;
	if (ib->biSize < sizeof(*ib) || ib->biWidth <= 0 || !ib->biHeight) {
		vfw_set_error("ICSeqCompressFrameStart: bad input format");
		return FALSE;
	}
	struct seq *s = claim(pc);
	if (!s) {
		vfw_set_error("ICSeqCompressFrameStart: too many sequences");
		return FALSE;
	}

	/* The input format, then what the codec makes of it. */
	size_t in_size = vfw_format_size(ib);
	LRESULT r;
	pc->lpbiIn = malloc(in_size);
	if (!pc->lpbiIn)
		goto nomem;
	memcpy(pc->lpbiIn, in, in_size);
	BITMAPINFOHEADER *out = ic_get_format(pc->hic, ICM_COMPRESS_GET_FORMAT,
					      &pc->lpbiIn->bmiHeader,
					      &s->out_size, &r);
	if (!out) {
		vfw_set_error("ICM_COMPRESS_GET_FORMAT: %s", icerr_name(r, err));
		goto fail;
	}
	pc->lpbiOut = (BITMAPINFO *)out;
	r = ic_send(pc->hic, ICM_COMPRESS_QUERY, (DWORD_PTR)pc->lpbiIn,
		    (DWORD_PTR)out);
	if (r != ICERR_OK) {
		vfw_set_error("ICM_COMPRESS_QUERY: %s", icerr_name(r, err));
		goto fail;
	}

	s->in_frame = image_bytes(ib);
	r = ic_send(pc->hic, ICM_COMPRESS_GET_SIZE, (DWORD_PTR)pc->lpbiIn,
		    (DWORD_PTR)out);
	if (r > 0)
		s->max_frame = (DWORD)r;
	else
		/* No answer: allow twice the input, as some drivers expect. */
		s->max_frame = s->in_frame * 2 + 65536;
	pc->lpBitsOut = calloc(1, s->max_frame);
	if (!pc->lpBitsOut)
		goto nomem;

	ICINFO info;
	memset(&info, 0, sizeof(info));
	ic_get_info(pc->hic, &info, sizeof(info));
	s->temporal = (info.dwFlags & VIDCF_TEMPORAL) &&
		      !(info.dwFlags & VIDCF_FASTTEMPORALC);
	if (s->temporal && !(pc->lpBitsPrev = calloc(1, s->in_frame ? s->in_frame : 1)))
		goto nomem;
	if (!rate || !scale) {
		rate = 15;
		scale = 1;
	}
	if ((info.dwFlags & VIDCF_CRUNCH) && pc->lDataRate > 0)
		s->budget = (DWORD)((uint64_t)pc->lDataRate * 1024 * scale / rate);

	if ((pc->dwFlags & ICMF_COMPVARS_VALID) && pc->lpState && pc->cbState > 0) {
		/* The bytes of state used; 0 or an ICERR_* if refused. */
		r = ic_send(pc->hic, ICM_SETSTATE, (DWORD_PTR)pc->lpState,
			    (DWORD_PTR)pc->cbState);
		if (r <= 0) {
			vfw_set_error("the codec refused the state (ICM_SETSTATE: %s)",
				      icerr_name(r, err));
			goto fail;
		}
	}

	/*
	 * ICQUALITY_DEFAULT becomes a real quality, since codecs may expect
	 * 0..10000 in every frame: the current one (which a state may have
	 * set), else the default, as ICCompressorChoose would fill in.
	 */
	if (pc->lQ == ICQUALITY_DEFAULT) {
		DWORD q = 0;
		if ((ic_send(pc->hic, ICM_GETQUALITY, (DWORD_PTR)&q, 0) == ICERR_OK &&
		     q <= ICQUALITY_HIGH) ||
		    (q = 0, ic_send(pc->hic, ICM_GETDEFAULTQUALITY, (DWORD_PTR)&q, 0) ==
		     ICERR_OK && q <= ICQUALITY_HIGH))
			pc->lQ = (LONG)q;
	}

	/* Informational; codecs that don't care say so, which is fine. */
	ICCOMPRESSFRAMES f = {
		.lpbiOutput = out, .lpbiInput = &pc->lpbiIn->bmiHeader,
		.lStartFrame = 0, .lFrameCount = frames, .lQuality = pc->lQ,
		.lDataRate = pc->lDataRate > 0 ? pc->lDataRate * 1024 : 0,
		.lKeyRate = pc->lKey, .dwRate = rate, .dwScale = scale,
	};
	ic_send(pc->hic, ICM_COMPRESS_FRAMES_INFO, (DWORD_PTR)&f, sizeof(f));

	r = ic_send(pc->hic, ICM_COMPRESS_BEGIN, (DWORD_PTR)pc->lpbiIn,
		    (DWORD_PTR)out);
	if (r != ICERR_OK) {
		vfw_set_error("ICM_COMPRESS_BEGIN: %s", icerr_name(r, err));
		goto fail;
	}
	pc->lFrame = 0;
	pc->lKeyCount = 0;
	return TRUE;

nomem:
	vfw_set_error("ICSeqCompressFrameStart: out of memory");
fail:
	free_buffers(pc);
	release(s);
	return FALSE;
}

BOOL STUB_WINAPI ic_seq_start(COMPVARS *pc, BITMAPINFO *in)
{
	return ic_seq_start_ex(pc, in, 0, 0, 0);
}

LPVOID STUB_WINAPI ic_seq_frame(COMPVARS *pc, UINT flags, LPVOID bits,
				BOOL *key, LONG *size)
{
	struct seq *s = find(pc);
	char err[24];

	if (!s || !bits) {
		vfw_set_error("ICSeqCompressFrame: the sequence was not started");
		return NULL;
	}
	BITMAPINFOHEADER *out = &pc->lpbiOut->bmiHeader;
	DWORD icflags = 0, ckid = 0, oflags = 0;
	if (pc->lFrame == 0 || (flags & ICCOMPRESS_KEYFRAME) ||
	    (pc->lKey > 0 && pc->lKeyCount >= pc->lKey))
		icflags = ICCOMPRESS_KEYFRAME;

	out->biSizeImage = s->max_frame;
	DWORD r = ic_compress(pc->hic, icflags, out, pc->lpBitsOut,
			      &pc->lpbiIn->bmiHeader, bits, &ckid, &oflags,
			      pc->lFrame, s->budget, (DWORD)pc->lQ,
			      s->temporal && pc->lFrame ?
			      &pc->lpbiIn->bmiHeader : NULL,
			      s->temporal && pc->lFrame ? pc->lpBitsPrev : NULL);
	if (r != ICERR_OK) {
		vfw_set_error("ICM_COMPRESS, frame %ld: %s", (long)pc->lFrame,
			      icerr_name((LRESULT)(LONG)r, err));
		return NULL;
	}
	if (out->biSizeImage > s->max_frame) {
		vfw_set_error("ICM_COMPRESS, frame %ld: the codec reports %lu "
			      "bytes, more than the %lu it said it needs at most",
			      (long)pc->lFrame, (unsigned long)out->biSizeImage,
			      (unsigned long)s->max_frame);
		return NULL;
	}
	if (s->temporal)
		memcpy(pc->lpBitsPrev, bits, s->in_frame);

	int is_key = (oflags & AVIIF_KEYFRAME) != 0;
	pc->lKeyCount = is_key ? 1 : pc->lKeyCount + 1;
	pc->lFrame++;
	if (key)
		*key = is_key;
	if (size)
		*size = (LONG)out->biSizeImage;
	return pc->lpBitsOut;
}

void STUB_WINAPI ic_seq_end(COMPVARS *pc)
{
	struct seq *s = find(pc);

	if (!s)
		return;
	if (ic_valid(pc->hic))
		ic_send(pc->hic, ICM_COMPRESS_END, 0, 0);
	free_buffers(pc);
	release(s);
}

void STUB_WINAPI ic_compressor_free(COMPVARS *pc)
{
	if (!pc || pc->cbSize != sizeof(*pc))
		return;
	ic_seq_end(pc);
	if (pc->hic) {
		ic_close(pc->hic);
		pc->hic = NULL;
	}
	pc->dwFlags = 0;
}

size_t ic_seq_format_size(const COMPVARS *pc)
{
	struct seq *s = find(pc);
	return s ? s->out_size : 0;
}
