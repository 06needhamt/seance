/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * AVI writer: one video stream, AVI 1.0 idx1 plus OpenDML indexes.
 *
 * Layout:
 *   RIFF 'AVI '
 *     LIST 'hdrl'  avih, LIST 'strl' (strh, strf, indx), LIST 'odml' (dmlh)
 *     LIST 'movi'  00dc/00db chunks, ix00
 *     idx1
 *   RIFF 'AVIX'    LIST 'movi' (chunks, ix00)   ... when over riff_limit
 *
 * The indx and odml chunks are reserved as JUNK and only filled in when
 * the file ends up as OpenDML, so small files are plain AVI 1.0 (the ix00
 * chunks inside movi are ignored by readers that don't know them).
 */
#include <errno.h>
#include <fcntl.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "avi/avi.h"
#include "common/error.h"

#define FCC(a, b, c, d) mmioFOURCC(a, b, c, d)
#define SUPER_ENTRIES 256		/* RIFF segments the super index can hold */
#define SUPER_BYTES   (24 + 16 * SUPER_ENTRIES)
#define ODML_BYTES    (4 + 8 + 248)	/* 'odml' + the dmlh chunk */
#define DEFAULT_RIFF_LIMIT (1u << 30)
#define MAX_RIFF_LIMIT     (2u << 30)

struct frame_rec {
	uint64_t pos;		/* of the chunk header */
	DWORD size;
	int key;
};

struct segment {
	uint64_t ix_pos;
	DWORD ix_size;		/* whole chunk, header included */
	DWORD frames;
};

struct avi_writer {
	int fd;
	struct avi_writer_options opts;
	BYTE *fmt;
	DWORD fmt_size;
	FOURCC handler, chunk_id;
	DWORD rate, scale;
	LONG width, height;

	uint64_t pos;		/* end of the file so far */
	uint64_t riff_pos;	/* current RIFF chunk */
	uint64_t movi_pos;	/* current LIST 'movi' */
	uint64_t avih_pos, strh_pos, super_pos, odml_pos;

	struct frame_rec *frames;
	size_t nframes, cap;
	size_t seg_first;	/* first frame of the current RIFF */
	size_t first_riff_frames;
	struct segment segs[SUPER_ENTRIES];
	size_t nsegs;
	DWORD max_chunk;
	int failed;
};

static void wr32(BYTE *p, DWORD v) { memcpy(p, &v, 4); }
static void wr16(BYTE *p, WORD v) { memcpy(p, &v, 2); }
static void wr64(BYTE *p, uint64_t v) { memcpy(p, &v, 8); }

static int put_at(struct avi_writer *w, uint64_t pos, const void *buf,
		  size_t len)
{
	size_t done = 0;

	while (done < len) {
		ssize_t r = pwrite(w->fd, (const BYTE *)buf + done, len - done,
				   (off_t)(pos + done));
		if (r < 0 && errno == EINTR)
			continue;
		if (r <= 0) {
			vfw_set_error("write error: %s", strerror(errno));
			w->failed = 1;
			return -1;
		}
		done += r;
	}
	return 0;
}

static int append(struct avi_writer *w, const void *buf, size_t len)
{
	if (put_at(w, w->pos, buf, len))
		return -1;
	w->pos += len;
	return 0;
}

static int put32_at(struct avi_writer *w, uint64_t pos, DWORD v)
{
	BYTE b[4];
	wr32(b, v);
	return put_at(w, pos, b, 4);
}

/* Starts a list: 'LIST' <size to patch> type. Returns its position. */
static uint64_t begin_list(struct avi_writer *w, FOURCC fcc, FOURCC type)
{
	BYTE b[12];
	uint64_t at = w->pos;

	wr32(b, fcc);
	wr32(b + 4, 0);
	wr32(b + 8, type);
	append(w, b, 12);
	return at;
}

static int end_list(struct avi_writer *w, uint64_t at)
{
	return put32_at(w, at + 4, (DWORD)(w->pos - at - 8));
}

static int put_chunk(struct avi_writer *w, FOURCC id, const void *data,
		     DWORD size)
{
	BYTE hdr[8], pad = 0;

	wr32(hdr, id);
	wr32(hdr + 4, size);
	if (append(w, hdr, 8) || (size && append(w, data, size)) ||
	    ((size & 1) && append(w, &pad, 1)))
		return -1;
	return 0;
}

/* A JUNK chunk whose total size is `total` bytes (a placeholder). */
static int put_junk(struct avi_writer *w, DWORD total)
{
	BYTE *zero = calloc(1, total - 8);
	int r;

	if (!zero) {
		vfw_set_error("out of memory");
		return -1;
	}
	r = put_chunk(w, FCC('J', 'U', 'N', 'K'), zero, total - 8);
	free(zero);
	return r;
}

struct avi_writer *avi_writer_open(const char *path, const void *fmt,
				   DWORD fmt_size, FOURCC handler, DWORD rate,
				   DWORD scale,
				   const struct avi_writer_options *opts)
{
	const BITMAPINFOHEADER *bih = fmt;
	struct avi_writer *w;

	if (fmt_size < sizeof(*bih) || !rate || !scale) {
		vfw_set_error("bad video format or frame rate");
		return NULL;
	}
	if (!(w = calloc(1, sizeof(*w))) || !(w->fmt = malloc(fmt_size))) {
		free(w);
		vfw_set_error("out of memory");
		return NULL;
	}
	if (opts)
		w->opts = *opts;
	if (!w->opts.riff_limit)
		w->opts.riff_limit = DEFAULT_RIFF_LIMIT;
	/* RIFF sizes are 32-bit; leave room for one more large chunk. */
	if (w->opts.riff_limit > MAX_RIFF_LIMIT)
		w->opts.riff_limit = MAX_RIFF_LIMIT;
	memcpy(w->fmt, fmt, fmt_size);
	w->fmt_size = fmt_size;
	w->handler = handler;
	w->rate = rate;
	w->scale = scale;
	w->width = bih->biWidth;
	w->height = bih->biHeight < 0 ? -bih->biHeight : bih->biHeight;
	w->chunk_id = (bih->biCompression == BI_RGB ||
		       bih->biCompression == BI_BITFIELDS) ?
		      FCC('0', '0', 'd', 'b') : FCC('0', '0', 'd', 'c');

	w->fd = open(path, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0644);
	if (w->fd < 0) {
		vfw_set_error("%s: %s", path, strerror(errno));
		free(w->fmt);
		free(w);
		return NULL;
	}

	BYTE zero[56] = { 0 };
	w->riff_pos = begin_list(w, FCC('R', 'I', 'F', 'F'), FCC('A', 'V', 'I', ' '));
	uint64_t hdrl = begin_list(w, FCC('L', 'I', 'S', 'T'), FCC('h', 'd', 'r', 'l'));
	w->avih_pos = w->pos;
	put_chunk(w, FCC('a', 'v', 'i', 'h'), zero, 56);
	uint64_t strl = begin_list(w, FCC('L', 'I', 'S', 'T'), FCC('s', 't', 'r', 'l'));
	w->strh_pos = w->pos;
	put_chunk(w, FCC('s', 't', 'r', 'h'), zero, 56);
	put_chunk(w, FCC('s', 't', 'r', 'f'), fmt, fmt_size);
	w->super_pos = w->pos;
	put_junk(w, 8 + SUPER_BYTES);
	end_list(w, strl);
	w->odml_pos = w->pos;
	put_junk(w, 8 + ODML_BYTES);
	end_list(w, hdrl);
	w->movi_pos = begin_list(w, FCC('L', 'I', 'S', 'T'), FCC('m', 'o', 'v', 'i'));

	if (w->failed) {
		close(w->fd);
		free(w->fmt);
		free(w);
		return NULL;
	}
	return w;
}

/* Writes the ix00 standard index for the current RIFF's frames. */
static int write_std_index(struct avi_writer *w)
{
	size_t n = w->nframes - w->seg_first;
	DWORD cb = 24 + 8 * (DWORD)n;
	BYTE *ix;

	if (w->nsegs == SUPER_ENTRIES) {
		vfw_set_error("too many RIFF segments for the OpenDML index");
		return -1;
	}
	if (!(ix = calloc(1, cb))) {
		vfw_set_error("out of memory");
		return -1;
	}
	/* Offsets are relative to a base; the movi list keeps them 32-bit. */
	uint64_t base = w->movi_pos;
	wr16(ix, 2);				/* longs per entry */
	ix[2] = AVI_INDEX_SUB_DEFAULT;
	ix[3] = AVI_INDEX_OF_CHUNKS;
	wr32(ix + 4, (DWORD)n);
	wr32(ix + 8, w->chunk_id);
	wr64(ix + 12, base);
	for (size_t i = 0; i < n; i++) {
		const struct frame_rec *f = &w->frames[w->seg_first + i];
		wr32(ix + 24 + 8 * i, (DWORD)(f->pos + 8 - base));
		wr32(ix + 28 + 8 * i, f->size | (f->key ? 0 : AVISTDINDEX_DELTAFRAME));
	}
	struct segment *s = &w->segs[w->nsegs++];
	s->ix_pos = w->pos;
	s->ix_size = 8 + cb;
	s->frames = (DWORD)n;
	int r = put_chunk(w, FCC('i', 'x', '0', '0'), ix, cb);
	free(ix);
	return r;
}

static int write_idx1(struct avi_writer *w)
{
	size_t n = w->first_riff_frames;
	BYTE *idx = malloc(n ? n * 16 : 1);
	uint64_t origin = w->opts.idx1_absolute ? 0 : w->movi_pos + 8;
	int r;

	if (!idx) {
		vfw_set_error("out of memory");
		return -1;
	}
	for (size_t i = 0; i < n; i++) {
		const struct frame_rec *f = &w->frames[i];
		wr32(idx + 16 * i, w->chunk_id);
		wr32(idx + 16 * i + 4, f->key ? AVIIF_KEYFRAME : 0);
		wr32(idx + 16 * i + 8, (DWORD)(f->pos - origin));
		wr32(idx + 16 * i + 12, f->size);
	}
	r = put_chunk(w, FCC('i', 'd', 'x', '1'), idx, (DWORD)(n * 16));
	free(idx);
	return r;
}

/* Ends the current RIFF: its index, then the movi list and RIFF sizes. */
static int end_segment(struct avi_writer *w)
{
	int first = w->nsegs == 0;

	if (first)
		w->first_riff_frames = w->nframes;
	if (write_std_index(w) || end_list(w, w->movi_pos))
		return -1;
	if (first && !w->opts.no_idx1 && write_idx1(w))
		return -1;
	return end_list(w, w->riff_pos);
}

int avi_writer_add(struct avi_writer *w, const void *data, DWORD size,
		   int keyframe)
{
	if (w->failed)
		return -1;

	/* Room for this chunk and this RIFF's indexes, or start an AVIX. */
	size_t in_seg = w->nframes - w->seg_first + 1;
	uint64_t need = w->pos - w->riff_pos + 8 + size + (size & 1) +
			32 + 8 * in_seg + (w->nsegs ? 0 : 8 + 16 * in_seg);
	if (in_seg > 1 && need > w->opts.riff_limit) {
		if (end_segment(w))
			return -1;
		w->riff_pos = begin_list(w, FCC('R', 'I', 'F', 'F'),
					 FCC('A', 'V', 'I', 'X'));
		w->movi_pos = begin_list(w, FCC('L', 'I', 'S', 'T'),
					 FCC('m', 'o', 'v', 'i'));
		w->seg_first = w->nframes;
	}

	if (w->nframes == w->cap) {
		size_t cap = w->cap ? w->cap * 2 : 256;
		struct frame_rec *f = realloc(w->frames, cap * sizeof(*f));
		if (!f) {
			vfw_set_error("out of memory");
			return -1;
		}
		w->frames = f;
		w->cap = cap;
	}
	w->frames[w->nframes++] = (struct frame_rec){ w->pos, size, keyframe != 0 };
	if (size > w->max_chunk)
		w->max_chunk = size;
	return put_chunk(w, w->chunk_id, data, size);
}

static int write_headers(struct avi_writer *w)
{
	BYTE b[8 + SUPER_BYTES];
	int odml = w->opts.force_odml || w->nsegs > 1;

	memset(b, 0, 56);
	wr32(b, (DWORD)(1000000ull * w->scale / w->rate));
	wr32(b + 4, (DWORD)((uint64_t)w->max_chunk * w->rate / w->scale));
	wr32(b + 12, w->opts.no_idx1 ? 0 : AVIF_HASINDEX);
	wr32(b + 16, (DWORD)w->first_riff_frames);
	wr32(b + 24, 1);			/* streams */
	wr32(b + 28, w->max_chunk);
	wr32(b + 32, (DWORD)w->width);
	wr32(b + 36, (DWORD)w->height);
	if (put_at(w, w->avih_pos + 8, b, 56))
		return -1;

	memset(b, 0, 56);
	wr32(b, FCC('v', 'i', 'd', 's'));
	wr32(b + 4, w->handler);
	wr32(b + 20, w->scale);
	wr32(b + 24, w->rate);
	wr32(b + 32, (DWORD)w->nframes);	/* length */
	wr32(b + 36, w->max_chunk);
	wr32(b + 40, 0xffffffff);		/* quality: default */
	wr16(b + 52, (WORD)w->width);
	wr16(b + 54, (WORD)w->height);
	if (put_at(w, w->strh_pos + 8, b, 56))
		return -1;

	if (!odml)
		return 0;

	/* Turn the placeholders into the super index and the odml list. */
	memset(b, 0, sizeof(b));
	wr32(b, FCC('i', 'n', 'd', 'x'));
	wr32(b + 4, SUPER_BYTES);
	wr16(b + 8, 4);
	b[10] = AVI_INDEX_SUB_DEFAULT;
	b[11] = AVI_INDEX_OF_INDEXES;
	wr32(b + 12, (DWORD)w->nsegs);
	wr32(b + 16, w->chunk_id);
	for (size_t i = 0; i < w->nsegs; i++) {
		BYTE *e = b + 32 + 16 * i;
		wr64(e, w->segs[i].ix_pos);
		wr32(e + 8, w->segs[i].ix_size);
		wr32(e + 12, w->segs[i].frames);
	}
	if (put_at(w, w->super_pos, b, 8 + SUPER_BYTES))
		return -1;

	memset(b, 0, 8 + ODML_BYTES);
	wr32(b, FCC('L', 'I', 'S', 'T'));
	wr32(b + 4, ODML_BYTES);
	wr32(b + 8, FCC('o', 'd', 'm', 'l'));
	wr32(b + 12, FCC('d', 'm', 'l', 'h'));
	wr32(b + 16, 248);
	wr32(b + 20, (DWORD)w->nframes);	/* dwGrandFrames */
	return put_at(w, w->odml_pos, b, 8 + ODML_BYTES);
}

int avi_writer_close(struct avi_writer *w)
{
	int r = w->failed ? -1 : 0;

	if (!r && (end_segment(w) || write_headers(w)))
		r = -1;
	if (close(w->fd) && !r) {
		vfw_set_error("close: %s", strerror(errno));
		r = -1;
	}
	free(w->frames);
	free(w->fmt);
	free(w);
	return r;
}
