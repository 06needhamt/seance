/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * A RealPlayer video codec (rv203260.dll's RV20toYUV420* entry points),
 * which is no VfW driver, DirectShow filter or DMO. It "decodes" a frame
 * of n slices into horizontal bands, band k's luma the first byte of
 * slice k as the slice table places it; chroma 128. A frame whose first
 * byte is 0 gives no picture (as a decoder holding one back does).
 */
#include "fixture.h"

struct rv_init {
	short unknown1, width, height, unknown2;
	unsigned unknown3, subformat, unknown4, format;
};

struct rv_in {
	unsigned len, interpolate, nsegments;
	const unsigned *segments;
	unsigned flags, time;
};

struct rv_out {
	unsigned frames, notes, time, width, height;
};

struct context {
	int width, height;
};

/* One decoder at a time; no C library to allocate from */
static struct context the_context;

/* Fills n bytes (volatile, so the compiler leaves no call to memset) */
static void fill(unsigned char *p, int v, unsigned n)
{
	volatile unsigned char *q = p;
	while (n--)
		*q++ = (unsigned char)v;
}

EXPORT int RV20toYUV420Init(const struct rv_init *init, void **ctx)
{
	struct context *c;

	/* As RealPlayer fills it in, for the version ffmpeg writes */
	if (init->unknown1 != 11 || init->unknown4 != 1 || init->width <= 0 ||
	    init->height <= 0 || init->format != 0x20103001)
		return 0x80040005;
	c = &the_context;
	c->width = init->width;
	c->height = init->height;
	*ctx = c;
	return 0;
}

EXPORT int RV20toYUV420Transform(const unsigned char *data, unsigned char *out,
				 const struct rv_in *in, struct rv_out *o,
				 struct context *c)
{
	unsigned n = in->nsegments + 1, w = (unsigned)c->width;
	unsigned h = (unsigned)c->height;

	if (!in->len)
		return 0x80040005;
	fill((unsigned char *)o, 0, sizeof(*o));
	if (!data[0])
		return 0;
	for (unsigned k = 0; k < n; k++) {
		unsigned at = in->segments[2 * k + 1];
		if (in->segments[2 * k] != 1 || at >= in->len)
			return 0x80040005;
		for (unsigned y = h * k / n; y < h * (k + 1) / n; y++)
			fill(out + y * w, data[at], w);
	}
	fill(out + w * h, 128, w * h / 2);
	o->frames = 1;
	o->width = w;
	o->height = h;
	o->time = in->time;
	return 0;
}

EXPORT int RV20toYUV420CustomMessage(void *msg, struct context *c)
{
	return 0;
}

EXPORT int RV20toYUV420Free(struct context *c)
{
	c->width = c->height = 0;
	return 0;
}

BOOL WINAPI DllMain(HINSTANCE inst, DWORD reason, LPVOID reserved)
{
	return TRUE;
}
