/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * AVI writer: video and audio streams, AVI 1.0 idx1 plus OpenDML indexes.
 *
 * Layout:
 *   RIFF 'AVI '
 *     LIST 'hdrl'  avih, LIST 'strl' (strh, strf, indx) per stream,
 *                  LIST 'odml' (dmlh)
 *     LIST 'movi'  ##dc/##db/##wb chunks, ix## per stream
 *     idx1
 *   RIFF 'AVIX'    LIST 'movi' (chunks, ix##)   ... when over riff_limit
 *
 * The indx and odml chunks are reserved as JUNK and only filled in when
 * the file ends up as OpenDML, so small files are plain AVI 1.0 (the ix##
 * chunks inside movi are ignored by readers that don't know them).
 */
#include <errno.h>
#include <fcntl.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "avi/avi.h"
#include "common/error.h"
#include "media/audio.h"

#define FCC(a, b, c, d) mmioFOURCC(a, b, c, d)
#define SUPER_ENTRIES 256		/* RIFF segments the super index can hold */
#define SUPER_BYTES   (24 + 16 * SUPER_ENTRIES)
#define ODML_BYTES    (4 + 8 + 248)	/* 'odml' + the dmlh chunk */
#define DEFAULT_RIFF_LIMIT (1u << 30)
#define MAX_RIFF_LIMIT     (2u << 30)
#define MAX_STREAMS        16

struct chunk_rec {
	uint64_t pos;		/* of the chunk header */
	DWORD size;
	BYTE stream;
	BYTE key;
};

struct segment {
	uint64_t ix_pos;
	DWORD ix_size;		/* whole chunk, header included */
	DWORD duration;		/* in the stream's units */
};

struct wstream {
	FOURCC type, handler, chunk_id;
	BYTE *fmt;
	DWORD fmt_size;
	DWORD rate, scale, sample_size;
	LONG width, height;
	uint64_t strh_pos, super_pos;
	struct segment segs[SUPER_ENTRIES];
	size_t nsegs;
	DWORD max_chunk;
	uint64_t length;	/* in units: chunks, or bytes / sample_size */
	uint64_t seg_start;	/* length when the current RIFF began */
	size_t nchunks;
	size_t first_riff_chunks;
};

struct avi_writer {
	int fd;
	struct avi_writer_options opts;
	struct wstream streams[MAX_STREAMS];
	unsigned nstreams;
	int started;

	uint64_t pos;		/* end of the file so far */
	uint64_t riff_pos;	/* current RIFF chunk */
	uint64_t movi_pos;	/* current LIST 'movi' */
	uint64_t avih_pos, odml_pos;
	unsigned riffs;		/* RIFF chunks finished */

	struct chunk_rec *chunks;
	size_t nchunks, cap;
	size_t seg_first;	/* first chunk of the current RIFF */
	size_t first_riff_chunks;
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

/* "##xx": the stream number in two digits, then the kind. */
static FOURCC chunk_id(unsigned stream, char a, char b)
{
	return FCC('0' + stream / 10, '0' + stream % 10, a, b);
}

struct avi_writer *avi_writer_create(const char *path,
				     const struct avi_writer_options *opts)
{
	struct avi_writer *w = calloc(1, sizeof(*w));

	if (!w) {
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
	w->fd = open(path, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0644);
	if (w->fd < 0) {
		vfw_set_error("%s: %s", path, strerror(errno));
		free(w);
		return NULL;
	}
	return w;
}

static struct wstream *new_stream(struct avi_writer *w, const void *fmt,
				  DWORD fmt_size)
{
	struct wstream *s;

	if (w->started) {
		vfw_set_error("streams must be added before the first chunk");
		return NULL;
	}
	if (w->nstreams == MAX_STREAMS) {
		vfw_set_error("too many streams");
		return NULL;
	}
	s = &w->streams[w->nstreams];
	memset(s, 0, sizeof(*s));
	if (!(s->fmt = malloc(fmt_size))) {
		vfw_set_error("out of memory");
		return NULL;
	}
	memcpy(s->fmt, fmt, fmt_size);
	s->fmt_size = fmt_size;
	return s;
}

int avi_writer_add_video(struct avi_writer *w, const void *fmt,
			 DWORD fmt_size, FOURCC handler, DWORD rate,
			 DWORD scale)
{
	const BITMAPINFOHEADER *bih = fmt;
	struct wstream *s;

	if (fmt_size < sizeof(*bih) || !rate || !scale) {
		vfw_set_error("bad video format or frame rate");
		return -1;
	}
	if (!(s = new_stream(w, fmt, fmt_size)))
		return -1;
	s->type = FCC('v', 'i', 'd', 's');
	s->handler = handler;
	s->rate = rate;
	s->scale = scale;
	s->width = bih->biWidth;
	s->height = bih->biHeight < 0 ? -bih->biHeight : bih->biHeight;
	s->chunk_id = (bih->biCompression == BI_RGB ||
		       bih->biCompression == BI_BITFIELDS) ?
		      chunk_id(w->nstreams, 'd', 'b') :
		      chunk_id(w->nstreams, 'd', 'c');
	return (int)w->nstreams++;
}

int avi_writer_add_audio(struct avi_writer *w, const void *fmt,
			 DWORD fmt_size, DWORD rate, DWORD scale,
			 DWORD sample_size)
{
	const WAVEFORMATEX *wf = fmt;
	struct wstream *s;

	if (fmt_size < 16 || !wf->nChannels || !wf->nSamplesPerSec ||
	    !wf->nBlockAlign) {
		vfw_set_error("bad audio format");
		return -1;
	}
	if (!rate || !scale) {
		/*
		 * Constant rate, counted in blocks: for PCM a block is a
		 * sample frame, so the rate is the sample rate; otherwise
		 * it is the byte rate over the block size.
		 */
		if (audio_format_sample(wf, fmt_size) != AUDIO_NONE) {
			scale = 1;
			rate = wf->nSamplesPerSec;
		} else if (wf->nAvgBytesPerSec) {
			scale = wf->nBlockAlign;
			rate = wf->nAvgBytesPerSec;
		} else {
			vfw_set_error("audio format without a byte rate");
			return -1;
		}
		sample_size = wf->nBlockAlign;
	}
	if (!(s = new_stream(w, fmt, fmt_size)))
		return -1;
	s->type = FCC('a', 'u', 'd', 's');
	s->rate = rate;
	s->scale = scale;
	s->sample_size = sample_size;
	s->chunk_id = chunk_id(w->nstreams, 'w', 'b');
	return (int)w->nstreams++;
}

/* Lays out the headers, to be filled in on close. */
static int start(struct avi_writer *w)
{
	BYTE zero[56] = { 0 };

	if (!w->nstreams) {
		vfw_set_error("an AVI file needs a stream");
		w->failed = 1;
		return -1;
	}
	w->started = 1;
	w->riff_pos = begin_list(w, FCC('R', 'I', 'F', 'F'), FCC('A', 'V', 'I', ' '));
	uint64_t hdrl = begin_list(w, FCC('L', 'I', 'S', 'T'), FCC('h', 'd', 'r', 'l'));
	w->avih_pos = w->pos;
	put_chunk(w, FCC('a', 'v', 'i', 'h'), zero, 56);
	for (unsigned i = 0; i < w->nstreams; i++) {
		struct wstream *s = &w->streams[i];
		uint64_t strl = begin_list(w, FCC('L', 'I', 'S', 'T'),
					   FCC('s', 't', 'r', 'l'));
		s->strh_pos = w->pos;
		put_chunk(w, FCC('s', 't', 'r', 'h'), zero, 56);
		put_chunk(w, FCC('s', 't', 'r', 'f'), s->fmt, s->fmt_size);
		s->super_pos = w->pos;
		put_junk(w, 8 + SUPER_BYTES);
		end_list(w, strl);
	}
	w->odml_pos = w->pos;
	put_junk(w, 8 + ODML_BYTES);
	end_list(w, hdrl);
	w->movi_pos = begin_list(w, FCC('L', 'I', 'S', 'T'), FCC('m', 'o', 'v', 'i'));
	return w->failed ? -1 : 0;
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
	if (!(w = avi_writer_create(path, opts)))
		return NULL;
	if (avi_writer_add_video(w, fmt, fmt_size, handler, rate, scale) < 0 ||
	    start(w)) {
		close(w->fd);
		free(w->streams[0].fmt);
		free(w);
		return NULL;
	}
	return w;
}

/* Writes stream n's ix## standard index for the current RIFF's chunks. */
static int write_std_index(struct avi_writer *w, unsigned n)
{
	struct wstream *s = &w->streams[n];
	size_t count = 0;
	DWORD cb;
	BYTE *ix;

	for (size_t i = w->seg_first; i < w->nchunks; i++)
		count += w->chunks[i].stream == n;
	if (!count)
		return 0;
	if (s->nsegs == SUPER_ENTRIES) {
		vfw_set_error("too many RIFF segments for the OpenDML index");
		return -1;
	}
	cb = 24 + 8 * (DWORD)count;
	if (!(ix = calloc(1, cb))) {
		vfw_set_error("out of memory");
		return -1;
	}
	/* Offsets are relative to a base; the movi list keeps them 32-bit. */
	uint64_t base = w->movi_pos;
	wr16(ix, 2);				/* longs per entry */
	ix[2] = AVI_INDEX_SUB_DEFAULT;
	ix[3] = AVI_INDEX_OF_CHUNKS;
	wr32(ix + 4, (DWORD)count);
	wr32(ix + 8, s->chunk_id);
	wr64(ix + 12, base);
	BYTE *e = ix + 24;
	for (size_t i = w->seg_first; i < w->nchunks; i++) {
		const struct chunk_rec *c = &w->chunks[i];
		if (c->stream != n)
			continue;
		wr32(e, (DWORD)(c->pos + 8 - base));
		wr32(e + 4, c->size | (c->key ? 0 : AVISTDINDEX_DELTAFRAME));
		e += 8;
	}
	struct segment *g = &s->segs[s->nsegs++];
	g->ix_pos = w->pos;
	g->ix_size = 8 + cb;
	g->duration = (DWORD)(s->length - s->seg_start);
	s->seg_start = s->length;
	int r = put_chunk(w, FCC('i', 'x', '0' + n / 10, '0' + n % 10), ix, cb);
	free(ix);
	return r;
}

static int write_idx1(struct avi_writer *w)
{
	size_t n = w->first_riff_chunks;
	BYTE *idx = malloc(n ? n * 16 : 1);
	uint64_t origin = w->opts.idx1_absolute ? 0 : w->movi_pos + 8;
	int r;

	if (!idx) {
		vfw_set_error("out of memory");
		return -1;
	}
	for (size_t i = 0; i < n; i++) {
		const struct chunk_rec *c = &w->chunks[i];
		wr32(idx + 16 * i, w->streams[c->stream].chunk_id);
		wr32(idx + 16 * i + 4, c->key ? AVIIF_KEYFRAME : 0);
		wr32(idx + 16 * i + 8, (DWORD)(c->pos - origin));
		wr32(idx + 16 * i + 12, c->size);
	}
	r = put_chunk(w, FCC('i', 'd', 'x', '1'), idx, (DWORD)(n * 16));
	free(idx);
	return r;
}

/* Ends the current RIFF: its indexes, then the movi list and RIFF sizes. */
static int end_segment(struct avi_writer *w)
{
	int first = w->riffs == 0;

	if (first) {
		w->first_riff_chunks = w->nchunks;
		for (unsigned i = 0; i < w->nstreams; i++)
			w->streams[i].first_riff_chunks = w->streams[i].nchunks;
	}
	for (unsigned i = 0; i < w->nstreams; i++)
		if (write_std_index(w, i))
			return -1;
	if (end_list(w, w->movi_pos))
		return -1;
	if (first && !w->opts.no_idx1 && write_idx1(w))
		return -1;
	w->riffs++;
	return end_list(w, w->riff_pos);
}

int avi_writer_write(struct avi_writer *w, unsigned stream, const void *data,
		     DWORD size, int keyframe)
{
	if (w->failed)
		return -1;
	if (stream >= w->nstreams) {
		vfw_set_error("no stream %u", stream);
		return -1;
	}
	if (!w->started && start(w))
		return -1;

	struct wstream *s = &w->streams[stream];
	/* Room for this chunk and this RIFF's indexes, or start an AVIX. */
	size_t in_seg = w->nchunks - w->seg_first + 1;
	uint64_t need = w->pos - w->riff_pos + 8 + size + (size & 1) +
			32 * w->nstreams + 8 * in_seg +
			(w->riffs ? 0 : 8 + 16 * in_seg);
	if (in_seg > 1 && need > w->opts.riff_limit) {
		if (end_segment(w))
			return -1;
		w->riff_pos = begin_list(w, FCC('R', 'I', 'F', 'F'),
					 FCC('A', 'V', 'I', 'X'));
		w->movi_pos = begin_list(w, FCC('L', 'I', 'S', 'T'),
					 FCC('m', 'o', 'v', 'i'));
		w->seg_first = w->nchunks;
	}

	if (w->nchunks == w->cap) {
		size_t cap = w->cap ? w->cap * 2 : 256;
		struct chunk_rec *c = realloc(w->chunks, cap * sizeof(*c));
		if (!c) {
			vfw_set_error("out of memory");
			return -1;
		}
		w->chunks = c;
		w->cap = cap;
	}
	w->chunks[w->nchunks++] = (struct chunk_rec){
		w->pos, size, (BYTE)stream,
		s->type == FCC('a', 'u', 'd', 's') || keyframe };
	s->nchunks++;
	s->length += s->sample_size ? size / s->sample_size : 1;
	if (size > s->max_chunk)
		s->max_chunk = size;
	return put_chunk(w, s->chunk_id, data, size);
}

int avi_writer_add(struct avi_writer *w, const void *data, DWORD size,
		   int keyframe)
{
	return avi_writer_write(w, 0, data, size, keyframe);
}

/* The stream avih describes: the first video stream, else the first. */
static const struct wstream *main_stream(const struct avi_writer *w)
{
	for (unsigned i = 0; i < w->nstreams; i++)
		if (w->streams[i].type == FCC('v', 'i', 'd', 's'))
			return &w->streams[i];
	return &w->streams[0];
}

static int write_headers(struct avi_writer *w)
{
	BYTE b[8 + SUPER_BYTES];
	const struct wstream *m = main_stream(w);
	int video = m->type == FCC('v', 'i', 'd', 's');
	int odml = w->opts.force_odml || w->riffs > 1;
	uint64_t bytes_per_sec = 0;
	DWORD max_chunk = 0;

	for (unsigned i = 0; i < w->nstreams; i++) {
		const struct wstream *s = &w->streams[i];
		bytes_per_sec += s->sample_size ?
				 (uint64_t)s->sample_size * s->rate / s->scale :
				 (uint64_t)s->max_chunk * s->rate / s->scale;
		if (s->max_chunk > max_chunk)
			max_chunk = s->max_chunk;
	}

	memset(b, 0, 56);
	wr32(b, video ? (DWORD)(1000000ull * m->scale / m->rate) : 0);
	wr32(b + 4, bytes_per_sec > 0xffffffff ? 0xffffffff :
		    (DWORD)bytes_per_sec);
	wr32(b + 12, w->opts.no_idx1 ? 0 : AVIF_HASINDEX);
	/* Frames: of the video, so none for audio alone. */
	wr32(b + 16, video ? (DWORD)m->first_riff_chunks : 0);
	wr32(b + 24, w->nstreams);
	wr32(b + 28, max_chunk);
	wr32(b + 32, (DWORD)m->width);
	wr32(b + 36, (DWORD)m->height);
	if (put_at(w, w->avih_pos + 8, b, 56))
		return -1;

	for (unsigned i = 0; i < w->nstreams; i++) {
		const struct wstream *s = &w->streams[i];

		memset(b, 0, 56);
		wr32(b, s->type);
		wr32(b + 4, s->handler);
		wr32(b + 20, s->scale);
		wr32(b + 24, s->rate);
		wr32(b + 32, s->length > 0xffffffff ? 0xffffffff :
			     (DWORD)s->length);
		wr32(b + 36, s->max_chunk);
		wr32(b + 40, 0xffffffff);	/* quality: default */
		wr32(b + 44, s->sample_size);
		wr16(b + 52, (WORD)s->width);
		wr16(b + 54, (WORD)s->height);
		if (put_at(w, s->strh_pos + 8, b, 56))
			return -1;
	}

	if (!odml)
		return 0;

	/* Turn the placeholders into the super indexes and the odml list. */
	for (unsigned i = 0; i < w->nstreams; i++) {
		const struct wstream *s = &w->streams[i];

		memset(b, 0, sizeof(b));
		wr32(b, FCC('i', 'n', 'd', 'x'));
		wr32(b + 4, SUPER_BYTES);
		wr16(b + 8, 4);
		b[10] = AVI_INDEX_SUB_DEFAULT;
		b[11] = AVI_INDEX_OF_INDEXES;
		wr32(b + 12, (DWORD)s->nsegs);
		wr32(b + 16, s->chunk_id);
		for (size_t g = 0; g < s->nsegs; g++) {
			BYTE *e = b + 32 + 16 * g;
			wr64(e, s->segs[g].ix_pos);
			wr32(e + 8, s->segs[g].ix_size);
			wr32(e + 12, s->segs[g].duration);
		}
		if (put_at(w, s->super_pos, b, 8 + SUPER_BYTES))
			return -1;
	}

	memset(b, 0, 8 + ODML_BYTES);
	wr32(b, FCC('L', 'I', 'S', 'T'));
	wr32(b + 4, ODML_BYTES);
	wr32(b + 8, FCC('o', 'd', 'm', 'l'));
	wr32(b + 12, FCC('d', 'm', 'l', 'h'));
	wr32(b + 16, 248);
	wr32(b + 20, video ? (DWORD)m->nchunks : 0);	/* dwGrandFrames */
	return put_at(w, w->odml_pos, b, 8 + ODML_BYTES);
}

int avi_writer_close(struct avi_writer *w)
{
	int r = w->failed ? -1 : 0;

	if (!r && !w->started && start(w))
		r = -1;
	if (!r && (end_segment(w) || write_headers(w)))
		r = -1;
	if (close(w->fd) && !r) {
		vfw_set_error("close: %s", strerror(errno));
		r = -1;
	}
	for (unsigned i = 0; i < w->nstreams; i++)
		free(w->streams[i].fmt);
	free(w->chunks);
	free(w);
	return r;
}
