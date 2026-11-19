/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * A RealPlayer audio codec (cook.dll's RAOpenCodec2 / RAInitDecoder /
 * RADecode / ...). It takes 8 kHz mono 16-bit flavor 1, and "decodes"
 * each byte b of a block into the sample (b - 128) * 256.
 */
#include "fixture.h"

struct ra_init {
	unsigned rate;
	unsigned short bits, channels, quality, pad;
	unsigned bits_per_frame, packet_size, extra_len;
	const void *extra;
};

struct context {
	int inited, flavor;
};

/* One decoder at a time; no C library to allocate from */
static struct context the_context;

EXPORT int RAOpenCodec2(struct context **ctx, const char *dir)
{
	/* RealPlayer gives the codec's directory, ending in a separator */
	size_t n = 0;
	while (dir && dir[n])
		n++;
	if (!n || (dir[n - 1] != '\\' && dir[n - 1] != '/'))
		return 0x80004005;
	the_context.inited = the_context.flavor = 0;
	*ctx = &the_context;
	return 0;
}

EXPORT int RAInitDecoder(struct context *c, const struct ra_init *init)
{
	if (init->rate != 8000 || init->bits != 16 || init->channels != 1 ||
	    init->quality != 100)
		return 0x80004005;
	c->inited = 1;
	return 0;
}

EXPORT int RASetFlavor(struct context *c, int flavor)
{
	if (flavor != 1)
		return 0x80004005;
	c->flavor = flavor;
	return 0;
}

EXPORT int RADecode(struct context *c, const unsigned char *in, unsigned len,
		    short *out, unsigned *out_len, int flags)
{
	if (!c->inited || !c->flavor)
		return 0x80004005;
	for (unsigned i = 0; i < len; i++)
		out[i] = (short)((in[i] - 128) * 256);
	*out_len = 2 * len;
	return 0;
}

EXPORT int RAFreeDecoder(struct context *c)
{
	c->inited = 0;
	return 0;
}

EXPORT int RACloseCodec(struct context *c)
{
	c->flavor = 0;
	return 0;
}

BOOL WINAPI DllMain(HINSTANCE inst, DWORD reason, LPVOID reserved)
{
	return TRUE;
}
