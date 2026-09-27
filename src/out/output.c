/*
 * Frame sinks: Y4M, BMP sequences and raw streams.
 */
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <unistd.h>

#include "common/error.h"
#include "out/output.h"

#define MAX_PATH_LEN 4096

struct output {
	enum output_kind kind;
	char *path;
	int fd;
	int w, h, single;
	BYTE *buf;		/* the last frame as written */
	size_t buf_len, buf_cap;
	unsigned long frames;
	int failed;
};

static const struct {
	const char *name;
	enum output_kind kind;
} kinds[] = {
	{ "y4m", OUT_Y4M },
	{ "bmp", OUT_BMP },
	{ "yuv", OUT_YUV },
	{ "i420", OUT_YUV },
	{ "bgr", OUT_BGR },
	{ "bgr24", OUT_BGR },
	{ "rgb", OUT_BGR },
	{ "raw", OUT_RAW },
};

int output_kind_from_name(const char *name, enum output_kind *kind)
{
	for (size_t i = 0; i < sizeof(kinds) / sizeof(kinds[0]); i++)
		if (!strcasecmp(name, kinds[i].name)) {
			*kind = kinds[i].kind;
			return 0;
		}
	vfw_set_error("unknown output format '%s' (y4m, bmp, yuv, bgr or raw)",
		      name);
	return -1;
}

int output_kind_from_path(const char *path, enum output_kind *kind)
{
	const char *slash = strrchr(path, '/');
	const char *dot = strrchr(slash ? slash : path, '.');

	if (!dot || output_kind_from_name(dot + 1, kind)) {
		vfw_set_error("%s: can't tell the output format from the name; "
			      "use .y4m, .bmp, .yuv, .bgr or .raw, or --format",
			      path);
		return -1;
	}
	return 0;
}

const char *output_kind_name(enum output_kind kind)
{
	switch (kind) {
	case OUT_Y4M: return "Y4M";
	case OUT_BMP: return "BMP";
	case OUT_YUV: return "raw I420";
	case OUT_BGR: return "raw BGR24";
	case OUT_RAW: return "raw frames";
	}
	return "?";
}

/*
 * The frame number pattern is parsed here rather than handed to printf,
 * so a path can't smuggle in other conversions.
 */
int output_bmp_path(const char *pattern, unsigned long n, int single,
		    char *buf, size_t size)
{
	size_t len = 0;
	int numbers = 0;

	for (const char *p = pattern; *p; p++) {
		char piece[32];
		const char *add = piece;

		if (*p != '%') {
			piece[0] = *p;
			piece[1] = '\0';
		} else if (p[1] == '%') {
			strcpy(piece, "%");
			p++;
		} else {
			int width = 0, digits = 0;
			const char *q = p + 1;
			if (*q == '0')
				q++;
			while (*q >= '0' && *q <= '9' && digits < 2) {
				width = width * 10 + (*q++ - '0');
				digits++;
			}
			/* A width needs the 0 flag: no spaces in file names. */
			if ((*q != 'd' && *q != 'u') || (p[1] == '0') != (digits > 0)) {
				vfw_set_error("%s: only %%d, %%u, %%0Nd, %%0Nu and %%%% "
					      "are allowed in a BMP path", pattern);
				return -1;
			}
			if (++numbers > 1) {
				vfw_set_error("%s: more than one frame number in "
					      "the BMP path", pattern);
				return -1;
			}
			snprintf(piece, sizeof(piece), "%0*lu", width, n);
			p = q;
		}
		size_t l = strlen(add);
		if (len + l >= size) {
			vfw_set_error("%s: path too long", pattern);
			return -1;
		}
		memcpy(buf + len, add, l);
		len += l;
	}
	buf[len] = '\0';
	if (!numbers && !single) {
		vfw_set_error("%s: a BMP path needs a frame number such as "
			      "%%05d for more than one frame", pattern);
		return -1;
	}
	return 0;
}

static int write_all(int fd, const void *buf, size_t len, const char *what)
{
	const BYTE *p = buf;

	while (len) {
		ssize_t r = write(fd, p, len);
		if (r < 0 && errno == EINTR)
			continue;
		if (r <= 0) {
			vfw_set_error("%s: %s", what, strerror(errno));
			return -1;
		}
		p += r;
		len -= r;
	}
	return 0;
}

static unsigned long gcd(unsigned long a, unsigned long b)
{
	while (b) {
		unsigned long t = a % b;
		a = b;
		b = t;
	}
	return a;
}

static int reserve(struct output *o, size_t len)
{
	if (len > o->buf_cap) {
		BYTE *b = realloc(o->buf, len);
		if (!b) {
			vfw_set_error("out of memory");
			return -1;
		}
		o->buf = b;
		o->buf_cap = len;
	}
	return 0;
}

struct output *output_open(const char *path, enum output_kind kind,
			   int width, int height, DWORD rate, DWORD scale,
			   int single)
{
	struct output *o = calloc(1, sizeof(*o));
	char first[MAX_PATH_LEN];

	if (!o || !(o->path = strdup(path))) {
		free(o);
		vfw_set_error("out of memory");
		return NULL;
	}
	o->kind = kind;
	o->w = width;
	o->h = height;
	o->single = single;
	o->fd = -1;

	/* Start from a black frame, which output_repeat() can use. */
	if (kind == OUT_Y4M || kind == OUT_YUV) {
		size_t y = (size_t)width * height;
		o->buf_len = image_i420_size(width, height);
		if (reserve(o, o->buf_len))
			goto fail;
		memset(o->buf, 16, y);
		memset(o->buf + y, 128, o->buf_len - y);
	} else if (kind == OUT_BGR || kind == OUT_BMP) {
		o->buf_len = image_bgr24_size(width, height);
		if (reserve(o, o->buf_len))
			goto fail;
		memset(o->buf, 0, o->buf_len);
	}

	if (kind == OUT_BMP) {
		/* Check the pattern now rather than after decoding a frame. */
		if (output_bmp_path(path, 0, single, first, sizeof(first)))
			goto fail;
		return o;
	}

	if (!strcmp(path, "-")) {
		o->fd = STDOUT_FILENO;
	} else {
		o->fd = open(path, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0644);
		if (o->fd < 0) {
			vfw_set_error("%s: %s", path, strerror(errno));
			goto fail;
		}
	}

	if (kind == OUT_Y4M) {
		char hdr[128];
		unsigned long g = gcd(rate, scale);
		if (!g)
			g = 1;
		int n = snprintf(hdr, sizeof(hdr),
				 "YUV4MPEG2 W%d H%d F%lu:%lu Ip A1:1 C420jpeg\n",
				 width, height, rate ? rate / g : 25,
				 rate ? scale / g : 1);
		if (write_all(o->fd, hdr, n, path))
			goto fail;
	}
	return o;

fail:
	if (o->fd > STDOUT_FILENO)
		close(o->fd);
	free(o->buf);
	free(o->path);
	free(o);
	return NULL;
}

static int write_bmp(struct output *o)
{
	char path[MAX_PATH_LEN];
	size_t stride = ((size_t)o->w * 3 + 3) & ~(size_t)3;
	size_t image = stride * o->h, total = 54 + image;
	BYTE *f;
	int fd, r;

	if (total > 0xffffffffu) {
		vfw_set_error("frame too large for a BMP");
		return -1;
	}
	if (output_bmp_path(o->path, o->frames, o->single, path, sizeof(path)))
		return -1;
	if (!(f = calloc(1, total))) {
		vfw_set_error("out of memory");
		return -1;
	}

	BITMAPFILEHEADER fh = { 0 };
	BITMAPINFOHEADER ih = { 0 };
	fh.bfType = 'B' | 'M' << 8;
	fh.bfSize = (DWORD)total;
	fh.bfOffBits = 54;
	ih.biSize = sizeof(ih);
	ih.biWidth = o->w;
	ih.biHeight = o->h;			/* bottom-up */
	ih.biPlanes = 1;
	ih.biBitCount = 24;
	ih.biCompression = BI_RGB;
	ih.biSizeImage = (DWORD)image;
	ih.biXPelsPerMeter = ih.biYPelsPerMeter = 2835;	/* 72 dpi */
	memcpy(f, &fh, 14);
	memcpy(f + 14, &ih, 40);
	for (int y = 0; y < o->h; y++)
		memcpy(f + 54 + (size_t)(o->h - 1 - y) * stride,
		       o->buf + (size_t)y * o->w * 3, (size_t)o->w * 3);

	fd = open(path, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0644);
	if (fd < 0) {
		vfw_set_error("%s: %s", path, strerror(errno));
		free(f);
		return -1;
	}
	r = write_all(fd, f, total, path);
	if (close(fd) && !r) {
		vfw_set_error("%s: %s", path, strerror(errno));
		r = -1;
	}
	free(f);
	return r;
}

/* Writes o->buf as the next frame. */
static int emit(struct output *o)
{
	int r;

	switch (o->kind) {
	case OUT_BMP:
		r = write_bmp(o);
		break;
	case OUT_Y4M:
		r = write_all(o->fd, "FRAME\n", 6, o->path) ||
		    write_all(o->fd, o->buf, o->buf_len, o->path) ? -1 : 0;
		break;
	default:
		r = write_all(o->fd, o->buf, o->buf_len, o->path);
		break;
	}
	if (r)
		o->failed = 1;
	else
		o->frames++;
	return r;
}

int output_write(struct output *o, struct image_conv *conv, const void *raw,
		 size_t raw_len)
{
	int r = 0;

	if (o->failed)
		return -1;
	switch (o->kind) {
	case OUT_Y4M:
	case OUT_YUV:
		r = image_to_i420(conv, raw, raw_len, o->buf);
		break;
	case OUT_BGR:
	case OUT_BMP:
		r = image_to_bgr24(conv, raw, raw_len, o->buf);
		break;
	case OUT_RAW:
		if ((r = reserve(o, raw_len ? raw_len : 1)) == 0) {
			if (raw_len)
				memcpy(o->buf, raw, raw_len);
			o->buf_len = raw_len;
		}
		break;
	}
	return r ? -1 : emit(o);
}

int output_repeat(struct output *o)
{
	return o->failed ? -1 : emit(o);
}

unsigned long output_frames(const struct output *o)
{
	return o->frames;
}

int output_close(struct output *o)
{
	int r = o->failed ? -1 : 0;

	if (o->fd > STDOUT_FILENO && close(o->fd) && !r) {
		vfw_set_error("%s: %s", o->path, strerror(errno));
		r = -1;
	}
	free(o->buf);
	free(o->path);
	free(o);
	return r;
}
