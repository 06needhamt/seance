/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * Frame sources: Y4M, AVI, BMP files and raw frames.
 */
#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#include "avi/avi.h"
#include "common/error.h"
#include "in/source.h"
#include "out/image.h"
#include "out/output.h"
#include "vfw/icm.h"

#define MAX_DIM    16384
#define MAX_HEADER 4096		/* a Y4M stream or frame header line */
#define MAX_BMP    (512u << 20)

struct source {
	enum source_kind kind;
	char path[4096];
	FILE *f;			/* Y4M, raw */
	struct vfw_format fmt;		/* the frames' format */
	size_t fmt_size;
	struct image_conv *conv;	/* for the format's name; AVI conversion */
	DWORD rate, scale;
	long frames;
	size_t frame_size;		/* 0 when it varies */
	BYTE *buf;
	size_t cap;
	unsigned long next;		/* frames read so far */

	/* AVI */
	struct avi_file *avi;
	const struct avi_stream *stream;
	size_t chunk;
	int convert;			/* decode to BGR24 (RLE, palette changes) */
	struct image_conv *out;		/* BGR24 bottom-up, when converting */
	BYTE *raw;			/* a chunk before conversion */
	size_t raw_cap;
	int have_frame;			/* buf holds a frame (for dropped ones) */

	/* raw: RGB rows are packed in the file, padded in a DIB */
	size_t row_bytes, file_frame;

	/* BMP */
	int pattern;
	unsigned long first;

	/* Padding or cropping: frames read, decoded, resized and packed. */
	struct image_conv *adj_from;	/* the frames as read */
	struct image_conv *adj_to;	/* the new size's format */
	int adj_i420, in_w, in_h, out_w, out_h;
	BYTE *adj_pic, *adj_mid, *adj_out;	/* decoded, resized, packed */
	size_t adj_size;
};

const char *source_kind_name(enum source_kind kind)
{
	switch (kind) {
	case SOURCE_Y4M: return "y4m";
	case SOURCE_AVI: return "avi";
	case SOURCE_BMP: return "bmp";
	case SOURCE_RAW: return "raw";
	default:         return "auto";
	}
}

static int grow(BYTE **buf, size_t *cap, size_t n)
{
	if (n <= *cap)
		return 0;
	BYTE *b = realloc(*buf, n);
	if (!b) {
		vfw_set_error("out of memory");
		return -1;
	}
	*buf = b;
	*cap = n;
	return 0;
}

static int has_ext(const char *path, const char *ext)
{
	size_t n = strlen(path), e = strlen(ext);
	return n > e && !strcasecmp(path + n - e, ext);
}

/* Sets the format's header for an uncompressed frame of w x h. */
static void set_header(struct source *s, LONG w, LONG h, WORD bits,
		       DWORD compression, DWORD image)
{
	BITMAPINFOHEADER *b = &s->fmt.bih;

	memset(&s->fmt, 0, sizeof(s->fmt));
	b->biSize = sizeof(*b);
	b->biWidth = w;
	b->biHeight = h;
	b->biPlanes = 1;
	b->biBitCount = bits;
	b->biCompression = compression;
	b->biSizeImage = image;
	s->fmt_size = sizeof(*b);
}

/* Y4M */

/* Reads a header line (without the newline) into buf. 0 at EOF, -1, 1. */
static int read_line(FILE *f, char *buf, size_t size)
{
	size_t n = 0;
	int c;

	while ((c = getc(f)) != EOF && c != '\n') {
		if (n + 1 >= size) {
			vfw_set_error("Y4M header line too long");
			return -1;
		}
		buf[n++] = (char)c;
	}
	buf[n] = 0;
	if (c == EOF) {
		if (ferror(f)) {
			vfw_set_error("read error: %s", strerror(errno));
			return -1;
		}
		if (n) {
			vfw_set_error("Y4M header cut short");
			return -1;
		}
		return 0;
	}
	return 1;
}

static int open_y4m(struct source *s)
{
	char line[MAX_HEADER];
	long w = 0, h = 0;
	unsigned long rn = 0, rd = 0;
	int grey = 0;

	int r = read_line(s->f, line, sizeof(line));
	if (r <= 0 || strncmp(line, "YUV4MPEG2", 9) || (line[9] && line[9] != ' ')) {
		if (r >= 0)
			vfw_set_error("%s: not a YUV4MPEG2 stream", s->path);
		return -1;
	}
	for (char *t = strtok(line + 9, " "); t; t = strtok(NULL, " ")) {
		char *end;
		switch (t[0]) {
		case 'W':
			w = strtol(t + 1, &end, 10);
			if (*end)
				w = 0;
			break;
		case 'H':
			h = strtol(t + 1, &end, 10);
			if (*end)
				h = 0;
			break;
		case 'F':
			if (sscanf(t + 1, "%lu:%lu", &rn, &rd) != 2)
				rn = rd = 0;
			break;
		case 'C':
			if (!strncmp(t + 1, "420", 3) &&
			    (!t[4] || !strcmp(t + 4, "jpeg") || !strcmp(t + 4, "paldv") ||
			     !strcmp(t + 4, "mpeg2")))
				grey = 0;
			else if (!strcmp(t + 1, "mono"))
				grey = 1;
			else {
				vfw_set_error("%s: Y4M colour space %s is not supported "
					      "(only 4:2:0 and mono)", s->path, t + 1);
				return -1;
			}
			break;
		default:
			break;	/* I (interlacing), A (aspect), X (extensions) */
		}
	}
	if (w <= 0 || h <= 0 || w > MAX_DIM || h > MAX_DIM) {
		vfw_set_error("%s: bad or missing Y4M frame size", s->path);
		return -1;
	}
	if (rn && rd) {
		s->rate = (DWORD)rn;
		s->scale = (DWORD)rd;
	}
	if (grey) {
		s->frame_size = (size_t)w * h;
		set_header(s, w, h, 8, mmioFOURCC('Y', '8', '0', '0'),
			   (DWORD)s->frame_size);
	} else {
		s->frame_size = image_i420_size((int)w, (int)h);
		set_header(s, w, h, 12, mmioFOURCC('I', '4', '2', '0'),
			   (DWORD)s->frame_size);
	}
	s->frames = -1;
	return 0;
}

static int read_y4m(struct source *s)
{
	char line[MAX_HEADER];
	int r = read_line(s->f, line, sizeof(line));

	if (r <= 0)
		return r;
	if (strncmp(line, "FRAME", 5) || (line[5] && line[5] != ' ')) {
		vfw_set_error("%s: frame %lu: expected a FRAME header", s->path,
			      s->next);
		return -1;
	}
	if (fread(s->buf, 1, s->frame_size, s->f) != s->frame_size) {
		vfw_set_error("%s: frame %lu is cut short", s->path, s->next);
		return -1;
	}
	return 1;
}

/* raw */

static int open_raw(struct source *s, const struct source_options *o)
{
	struct vfw_format f;
	const char *name = NULL;
	unsigned i;

	if (!o->raw_format || o->width <= 0 || o->height <= 0 ||
	    o->width > MAX_DIM || o->height > MAX_DIM) {
		vfw_set_error("%s: raw input needs a format and a size", s->path);
		return -1;
	}
	for (i = 0; (name = vfw_std_format(i, o->width, o->height, &f)); i++)
		if (!strcasecmp(name, o->raw_format))
			break;
	if (!name || f.bih.biBitCount == 8) {
		vfw_set_error("unknown raw format '%s'", o->raw_format);
		return -1;
	}
	s->fmt = f;
	s->fmt_size = vfw_format_size(&f.bih);
	s->frame_size = f.bih.biSizeImage;
	s->file_frame = s->frame_size;
	/*
	 * Raw RGB is top-down with no row padding, as the BGR writer and
	 * FFmpeg's rawvideo write it; frames get the DIB's padding on reading.
	 */
	if (f.bih.biCompression == BI_RGB || f.bih.biCompression == BI_BITFIELDS) {
		s->fmt.bih.biHeight = -o->height;
		s->row_bytes = (size_t)o->width * f.bih.biBitCount / 8;
		s->file_frame = s->row_bytes * o->height;
	}
	s->frames = -1;
	if (s->f != stdin && !fseeko(s->f, 0, SEEK_END)) {
		off_t end = ftello(s->f);
		if (end >= 0)
			s->frames = (long)(end / (off_t)s->file_frame);
		rewind(s->f);
	}
	return 0;
}

static int read_raw(struct source *s)
{
	size_t n = fread(s->buf, 1, s->file_frame, s->f);

	if (n != s->file_frame) {
		if (ferror(s->f)) {
			vfw_set_error("%s: read error: %s", s->path, strerror(errno));
			return -1;
		}
		if (!n)
			return 0;
		vfw_set_error("%s: the last frame is cut short (%zu of %zu bytes)",
			      s->path, n, s->file_frame);
		return -1;
	}
	if (s->file_frame != s->frame_size) {
		/* Spread the rows out to the DIB stride, last first. */
		LONG h = -s->fmt.bih.biHeight;
		size_t stride = s->frame_size / (size_t)h;
		for (LONG y = h - 1; y >= 0; y--) {
			memmove(s->buf + y * stride, s->buf + y * s->row_bytes,
				s->row_bytes);
			memset(s->buf + y * stride + s->row_bytes, 0,
			       stride - s->row_bytes);
		}
	}
	return 1;
}

/* BMP */

static uint32_t le32(const BYTE *p)
{
	return p[0] | p[1] << 8 | p[2] << 16 | (uint32_t)p[3] << 24;
}

/*
 * Reads a BMP file: the format into fmt (a 40-byte header plus colours or
 * masks) and the pixels into s->buf. Returns 0, 1 if it doesn't exist and
 * missing_ok, or -1.
 */
static int read_bmp_file(struct source *s, const char *path,
			 struct vfw_format *fmt, size_t *fmt_size,
			 size_t *len, int missing_ok)
{
	FILE *f = fopen(path, "rb");
	BYTE *data = NULL;
	int status = -1;

	if (!f) {
		if (missing_ok && errno == ENOENT)
			return 1;
		vfw_set_error("%s: %s", path, strerror(errno));
		return -1;
	}
	if (fseeko(f, 0, SEEK_END) || ftello(f) < 0) {
		vfw_set_error("%s: can't seek", path);
		goto done;
	}
	off_t size = ftello(f);
	rewind(f);
	if (size < 14 + 40 || size > MAX_BMP) {
		vfw_set_error("%s: not a BMP file", path);
		goto done;
	}
	if (!(data = malloc((size_t)size)) ||
	    fread(data, 1, (size_t)size, f) != (size_t)size) {
		vfw_set_error("%s: %s", path, data ? "read error" : "out of memory");
		goto done;
	}
	uint32_t off = le32(data + 10), hsize = le32(data + 14);
	if (data[0] != 'B' || data[1] != 'M' || hsize < 40 ||
	    (uint64_t)14 + hsize > (uint64_t)size) {
		vfw_set_error("%s: not a BMP file (or an OS/2 one)", path);
		goto done;
	}
	memset(fmt, 0, sizeof(*fmt));
	memcpy(&fmt->bih, data + 14, 40);
	BITMAPINFOHEADER *b = &fmt->bih;
	b->biSize = 40;
	LONG h = b->biHeight < 0 ? -b->biHeight : b->biHeight;
	if (b->biWidth <= 0 || !h || b->biWidth > MAX_DIM || h > MAX_DIM ||
	    (b->biCompression != BI_RGB && b->biCompression != BI_BITFIELDS &&
	     b->biCompression != BI_RLE8 && b->biCompression != BI_RLE4)) {
		vfw_set_error("%s: unsupported BMP (size or compression)", path);
		goto done;
	}
	/* Masks, from a V4/V5 header or after a plain one; then colours. */
	size_t extra = 0;
	const BYTE *after = data + 14 + hsize;
	if (b->biCompression == BI_BITFIELDS) {
		const BYTE *m = hsize >= 52 ? data + 14 + 40 : after;
		if (m + 12 > data + size) {
			vfw_set_error("%s: BMP masks missing", path);
			goto done;
		}
		memcpy(fmt->colors, m, 12);
		extra = 12;
	} else if (b->biBitCount <= 8) {
		size_t n = b->biClrUsed ? b->biClrUsed : 1u << b->biBitCount;
		if (n > 256)
			n = 256;
		if (after + 4 * n > data + size) {
			vfw_set_error("%s: BMP palette cut short", path);
			goto done;
		}
		memcpy(fmt->colors, after, 4 * n);
		b->biClrUsed = (DWORD)n;
		extra = 4 * n;
	}
	*fmt_size = 40 + extra;

	size_t bits;
	if (b->biCompression == BI_RLE8 || b->biCompression == BI_RLE4)
		bits = b->biSizeImage;
	else
		bits = ((size_t)b->biWidth * b->biBitCount + 31) / 32 * 4 * h;
	if (!bits || off > (uint64_t)size || bits > (uint64_t)size - off) {
		vfw_set_error("%s: BMP pixel data cut short", path);
		goto done;
	}
	b->biSizeImage = (DWORD)bits;
	if (grow(&s->buf, &s->cap, bits))
		goto done;
	memcpy(s->buf, data + off, bits);
	*len = bits;
	status = 0;
done:
	free(data);
	fclose(f);
	return status;
}

static int bmp_path(const struct source *s, unsigned long n, char *buf,
		    size_t size)
{
	if (!s->pattern) {
		snprintf(buf, size, "%s", s->path);
		return 0;
	}
	return output_bmp_path(s->path, n, 0, buf, size);
}

static int open_bmp(struct source *s)
{
	char p[4200];
	size_t len;

	s->pattern = strchr(s->path, '%') != NULL;
	if (s->pattern && output_bmp_path(s->path, 0, 0, p, sizeof(p)))
		return -1;
	/* A sequence starts at 0 or 1. */
	for (s->first = 0; s->first < 2; s->first++) {
		bmp_path(s, s->first, p, sizeof(p));
		int r = read_bmp_file(s, p, &s->fmt, &s->fmt_size, &len,
				      s->pattern);
		if (r < 0)
			return -1;
		if (r == 0)
			break;
	}
	if (s->first == 2) {
		vfw_set_error("%s: no files (tried from 0 and 1)", s->path);
		return -1;
	}
	s->frame_size = len;
	s->frames = s->pattern ? -1 : 1;
	return 0;
}

static int read_bmp(struct source *s, size_t *len)
{
	char p[4200];
	struct vfw_format f;
	size_t fsize;

	if (!s->pattern && s->next)
		return 0;
	bmp_path(s, s->first + s->next, p, sizeof(p));
	int r = read_bmp_file(s, p, &f, &fsize, len, 1);
	if (r)
		return r > 0 ? 0 : -1;
	/* Every file has to have the first one's format. */
	f.bih.biSizeImage = s->fmt.bih.biSizeImage;
	if (fsize != s->fmt_size || memcmp(&f, &s->fmt, fsize)) {
		vfw_set_error("%s: a different format from the first file", p);
		return -1;
	}
	return 1;
}

/* AVI */

static int open_avi(struct source *s, const struct source_options *o)
{
	if (!(s->avi = avi_open(s->path)))
		return -1;
	int n = o->stream >= 0 ? (o->stream < (int)s->avi->nstreams ? o->stream : -1) :
		avi_video_stream(s->avi);
	const BITMAPINFOHEADER *bih = n >= 0 ? avi_stream_bih(&s->avi->streams[n]) :
				      NULL;
	if (!bih) {
		vfw_set_error("%s: %s", s->path, o->stream >= 0 ?
			      "no such video stream" : "no video stream");
		return -1;
	}
	s->stream = &s->avi->streams[n];
	s->conv = image_conv_new(bih, s->stream->format_size);
	if (!s->conv) {
		/* Copied: vfw_set_error writes where vfw_error() points. */
		char why[256];
		snprintf(why, sizeof(why), "%s", vfw_error());
		vfw_set_error("%s: the video is compressed or in a format this "
			      "can't read%s%s%s; decode it first", s->path,
			      why[0] ? " (" : "", why, why[0] ? ")" : "");
		return -1;
	}
	s->rate = s->stream->header.dwRate;
	s->scale = s->stream->header.dwScale;
	s->frames = (long)s->stream->frames;

	int palette_changes = s->stream->nchunks != s->stream->frames;
	s->convert = image_is_delta(s->conv) || palette_changes;
	if (!s->convert) {
		size_t n = s->stream->format_size;
		if (n > sizeof(s->fmt))
			n = sizeof(s->fmt);
		memset(&s->fmt, 0, sizeof(s->fmt));
		memcpy(&s->fmt, bih, n);
		s->fmt.bih.biSize = sizeof(s->fmt.bih);
		s->fmt_size = vfw_format_size(&s->fmt.bih);
		s->frame_size = image_frame_size(s->conv);
		return 0;
	}
	int w = image_width(s->conv), h = image_height(s->conv);
	set_header(s, w, h, 24, BI_RGB, (DWORD)(((size_t)w * 3 + 3) / 4 * 4 * h));
	s->frame_size = s->fmt.bih.biSizeImage;
	if (!(s->out = image_conv_new(&s->fmt.bih, s->fmt_size)))
		return -1;
	return 0;
}

static int read_avi(struct source *s)
{
	while (s->chunk < s->stream->nchunks) {
		const struct avi_chunk *k = &s->stream->chunks[s->chunk++];

		if (!avi_chunk_is_frame(k)) {
			if (grow(&s->raw, &s->raw_cap, k->size + 1) ||
			    (k->size && avi_read_chunk(s->avi, k, s->raw)) ||
			    image_palette_change(s->conv, s->raw, k->size))
				return -1;
			continue;
		}
		if (!k->size) {
			/* A dropped frame: the last one again. */
			if (!s->have_frame)
				memset(s->buf, 0, s->frame_size);
			s->have_frame = 1;
			return 1;
		}
		if (!s->convert) {
			if (k->size < s->frame_size) {
				vfw_set_error("%s: frame %lu is cut short", s->path,
					      s->next);
				return -1;
			}
			if (grow(&s->buf, &s->cap, k->size) ||
			    avi_read_chunk(s->avi, k, s->buf))
				return -1;
		} else {
			int w = image_width(s->conv), h = image_height(s->conv);
			size_t bgr = image_bgr24_size(w, h);
			if (k->size > SIZE_MAX - bgr ||
			    grow(&s->raw, &s->raw_cap, k->size + bgr) ||
			    avi_read_chunk(s->avi, k, s->raw) ||
			    image_to_bgr24(s->conv, s->raw, k->size, s->raw + k->size) ||
			    image_pack(s->out, s->raw + k->size, s->buf))
				return -1;
		}
		s->have_frame = 1;
		return 1;
	}
	return 0;
}

/* Sets a source up to pad (or crop) its frames to multiples of n. */
static int adjust_open(struct source *s, int n, int pad)
{
	const BITMAPINFOHEADER *b = &s->fmt.bih;
	DWORD c = b->biCompression;
	int w = b->biWidth, h = b->biHeight < 0 ? -b->biHeight : b->biHeight;

	if (n < 2 || n > 256 || (n & (n - 1))) {
		vfw_set_error("--%s takes 2, 4, 8, ... 256, not %d",
			      pad ? "pad" : "crop", n);
		return -1;
	}
	s->in_w = w;
	s->in_h = h;
	s->out_w = pad ? (w + n - 1) / n * n : w / n * n;
	s->out_h = pad ? (h + n - 1) / n * n : h / n * n;
	if (!s->out_w || !s->out_h) {
		vfw_set_error("%s: %dx%d is smaller than %d: nothing is left to "
			      "crop to", s->path, w, h, n);
		return -1;
	}
	/* YUV stays YUV; RGB, palettes and RLE become RGB24. */
	s->adj_i420 = c != BI_RGB && c != BI_BITFIELDS && c != BI_RLE8 &&
		      c != BI_RLE4;
	struct vfw_format t;
	const char *name, *want = s->adj_i420 ? "I420" : "BGR24";
	unsigned i = 0;
	while ((name = vfw_std_format(i++, s->out_w, s->out_h, &t)) &&
	       strcmp(name, want))
		;
	if (!name) {
		vfw_set_error("no %s format at %dx%d", want, s->out_w, s->out_h);
		return -1;
	}
	if (!(s->adj_from = image_conv_new(&s->fmt.bih, s->fmt_size)) ||
	    !(s->adj_to = image_conv_new(&t.bih, vfw_format_size(&t.bih))))
		return -1;
	size_t in = s->adj_i420 ? image_i420_size(w, h) : image_bgr24_size(w, h);
	size_t mid = s->adj_i420 ? image_i420_size(s->out_w, s->out_h) :
		     image_bgr24_size(s->out_w, s->out_h);
	s->adj_size = image_frame_size(s->adj_to);
	if (!(s->adj_pic = malloc(in)) || !(s->adj_mid = malloc(mid)) ||
	    !(s->adj_out = calloc(1, s->adj_size))) {
		vfw_set_error("out of memory");
		return -1;
	}
	s->fmt = t;
	s->fmt_size = vfw_format_size(&t.bih);
	return 0;
}

struct source *source_open(const char *path, const struct source_options *o)
{
	struct source_options def = { .stream = -1 };
	struct source *s = calloc(1, sizeof(*s));

	if (!o)
		o = &def;
	if (!s) {
		vfw_set_error("out of memory");
		return NULL;
	}
	snprintf(s->path, sizeof(s->path), "%s", path);
	s->kind = o->kind;
	if (s->kind == SOURCE_AUTO) {
		if (o->raw_format)
			s->kind = SOURCE_RAW;
		else if (!strcmp(path, "-") || has_ext(path, ".y4m"))
			s->kind = SOURCE_Y4M;
		else if (has_ext(path, ".bmp") || strchr(path, '%'))
			s->kind = SOURCE_BMP;
		else if (has_ext(path, ".avi"))
			s->kind = SOURCE_AVI;
		else {
			/* By the first bytes. */
			BYTE m[12] = { 0 };
			FILE *f = fopen(path, "rb");
			size_t n = f ? fread(m, 1, sizeof(m), f) : 0;
			if (f)
				fclose(f);
			if (n >= 9 && !memcmp(m, "YUV4MPEG2", 9))
				s->kind = SOURCE_Y4M;
			else if (n == 12 && !memcmp(m, "RIFF", 4) &&
				 !memcmp(m + 8, "AVI ", 4))
				s->kind = SOURCE_AVI;
			else if (n >= 2 && m[0] == 'B' && m[1] == 'M')
				s->kind = SOURCE_BMP;
			else {
				vfw_set_error("%s: %s", path, f ? "not a Y4M, AVI or BMP "
					      "file (for raw frames, give the format)" :
					      strerror(errno));
				free(s);
				return NULL;
			}
		}
	}

	int r;
	switch (s->kind) {
	case SOURCE_Y4M:
	case SOURCE_RAW:
		s->f = !strcmp(path, "-") ? stdin : fopen(path, "rb");
		if (!s->f) {
			vfw_set_error("%s: %s", path, strerror(errno));
			r = -1;
			break;
		}
		r = s->kind == SOURCE_Y4M ? open_y4m(s) : open_raw(s, o);
		break;
	case SOURCE_BMP:
		r = open_bmp(s);
		break;
	case SOURCE_AVI:
		r = open_avi(s, o);
		break;
	default:
		vfw_set_error("bad source kind");
		r = -1;
	}
	if (!r && !s->conv && !(s->conv = image_conv_new(&s->fmt.bih, s->fmt_size)))
		r = -1;
	if (!r && s->kind != SOURCE_BMP && grow(&s->buf, &s->cap, s->frame_size))
		r = -1;
	if (!r && (o->pad || o->crop))
		r = adjust_open(s, o->pad ? o->pad : o->crop, !!o->pad);
	if (r) {
		source_close(s);
		return NULL;
	}
	return s;
}

void source_close(struct source *s)
{
	if (!s)
		return;
	if (s->f && s->f != stdin)
		fclose(s->f);
	avi_close(s->avi);
	image_conv_free(s->conv);
	image_conv_free(s->out);
	image_conv_free(s->adj_from);
	image_conv_free(s->adj_to);
	free(s->adj_pic);
	free(s->adj_mid);
	free(s->adj_out);
	free(s->buf);
	free(s->raw);
	free(s);
}

const BITMAPINFOHEADER *source_format(const struct source *s, size_t *size)
{
	if (size)
		*size = s->fmt_size;
	return &s->fmt.bih;
}

const char *source_format_name(const struct source *s)
{
	if (s->adj_to)
		return image_format_name(s->adj_to);
	return s->convert ? image_format_name(s->out) : image_format_name(s->conv);
}

enum source_kind source_kind(const struct source *s)
{
	return s->kind;
}

void source_rate(const struct source *s, DWORD *rate, DWORD *scale)
{
	*rate = s->rate;
	*scale = s->scale;
}

long source_frames(const struct source *s)
{
	return s->frames;
}

static int read_frame(struct source *s, const BYTE **frame, size_t *len);

/*
 * Each frame decoded to I420 or BGR24 at the size read, then each pixel
 * of the new size taken from the nearest one there is (so padding repeats
 * the last row and column), then packed in the new size's format.
 */
int source_read(struct source *s, const BYTE **frame, size_t *len)
{
	int r = read_frame(s, frame, len);

	if (r != 1 || !s->adj_to)
		return r;
	int ok = s->adj_i420 ?
		 image_to_i420(s->adj_from, *frame, *len, s->adj_pic) :
		 image_to_bgr24(s->adj_from, *frame, *len, s->adj_pic);
	if (ok)
		return -1;
	BYTE *dst = s->adj_mid;
	const BYTE *src = s->adj_pic;
	if (s->adj_i420) {
		/* Y, then U and V at half size (rounded up). */
		int planes[3][4] = {
			{ s->in_w, s->in_h, s->out_w, s->out_h },
			{ (s->in_w + 1) / 2, (s->in_h + 1) / 2,
			  (s->out_w + 1) / 2, (s->out_h + 1) / 2 },
			{ (s->in_w + 1) / 2, (s->in_h + 1) / 2,
			  (s->out_w + 1) / 2, (s->out_h + 1) / 2 },
		};
		BYTE *d = dst;
		for (int p = 0; p < 3; p++) {
			int iw = planes[p][0], ih = planes[p][1];
			int ow = planes[p][2], oh = planes[p][3];
			for (int y = 0; y < oh; y++) {
				const BYTE *row = src + (size_t)(y < ih ? y : ih - 1) * iw;
				for (int x = 0; x < ow; x++)
					*d++ = row[x < iw ? x : iw - 1];
			}
			src += (size_t)iw * ih;
		}
	} else {
		BYTE *d = dst;
		for (int y = 0; y < s->out_h; y++) {
			const BYTE *row = src + (size_t)(y < s->in_h ? y : s->in_h - 1) *
					  s->in_w * 3;
			for (int x = 0; x < s->out_w; x++) {
				const BYTE *px = row + (size_t)(x < s->in_w ? x : s->in_w - 1) * 3;
				*d++ = px[0];
				*d++ = px[1];
				*d++ = px[2];
			}
		}
	}
	if (image_pack(s->adj_to, dst, s->adj_out))
		return -1;
	*frame = s->adj_out;
	*len = s->adj_size;
	return 1;
}

static int read_frame(struct source *s, const BYTE **frame, size_t *len)
{
	size_t n = s->frame_size;
	int r;

	switch (s->kind) {
	case SOURCE_Y4M: r = read_y4m(s); break;
	case SOURCE_RAW: r = read_raw(s); break;
	case SOURCE_BMP: r = read_bmp(s, &n); break;
	case SOURCE_AVI: r = read_avi(s); break;
	default:         r = -1;
	}
	if (r == 1) {
		s->next++;
		*frame = s->buf;
		*len = n;
	}
	return r;
}
