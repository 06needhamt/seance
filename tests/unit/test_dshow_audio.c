/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * Audio through the DirectShow host (src/dshow, through src/xform.c):
 * testfilter.ax's TSTA encoder and decoder. Finding them by role, types
 * taken and refused, an encoder left to complete its output type, a round
 * trip that gives back the PCM exactly, times carried through, the bit
 * rate reaching the encoder, flushing, and no references left behind
 * (DllCanUnloadNow). Each of the fixture's audio modes (split outputs,
 * output held to the end, a switch to float, a rate change) runs in a
 * child process of its own, since the fixture reads them from the
 * environment as it first sees it.
 */
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "dshow/dshow.h"
#include "loader/modules.h"
#include "media/audio.h"
#include "runtime/runtime.h"
#include "xform.h"
#include "test.h"

#define TAG 0x7354		/* WAVE_FORMAT_TSTA */
#define RATE 44100
#define FRAMES 5000		/* four whole packets of 1024, and 904 */
#define BLOCK 4			/* s16 stereo */
#define PACKETS 5

static BYTE pcm[FRAMES * BLOCK];
static const char *server;

struct got {
	int n;
	BYTE *data;
	size_t size, cap;
	size_t sizes[64];
	REFERENCE_TIME start[64], stop[64];
	int sync[64], timed[64];
	int changes;
	WAVEFORMATEX *changed;	/* the last new type */
	size_t changed_size;
	DWORD refuse_rate;	/* refuse a new type of another rate */
};

static HRESULT collect(void *opaque, const struct xform_output *o,
		       const AM_MEDIA_TYPE *new_type)
{
	struct got *g = opaque;

	if (new_type) {
		free(g->changed);
		g->changed = NULL;
		if (FAILED(audio_mt_to_wfx(new_type, &g->changed,
					   &g->changed_size)))
			return VFW_E_TYPE_NOT_ACCEPTED;
		if (g->refuse_rate && g->changed->nSamplesPerSec != g->refuse_rate)
			return VFW_E_TYPE_NOT_ACCEPTED;
		g->changes++;
	}
	if (g->size + o->size > g->cap) {
		size_t cap = (g->size + o->size) * 2;
		BYTE *d = realloc(g->data, cap);
		if (!d)
			return E_OUTOFMEMORY;
		g->data = d;
		g->cap = cap;
	}
	memcpy(g->data + g->size, o->data, o->size);
	g->size += o->size;
	if (g->n < 64) {
		g->sizes[g->n] = o->size;
		g->start[g->n] = o->start;
		g->stop[g->n] = o->stop;
		g->sync[g->n] = o->sync;
		g->timed[g->n] = o->timed;
	}
	g->n++;
	return S_OK;
}

static void got_free(struct got *g)
{
	free(g->data);
	free(g->changed);
	memset(g, 0, sizeof(*g));
}

static int unloadable(void)
{
	struct module *m = mod_find("testfilter.ax");
	HRESULT (WINAPI *can)(void) = m ? (HRESULT (WINAPI *)(void))
		mod_get_proc(m, "DllCanUnloadNow") : NULL;
	return !can || can() == S_OK;
}

static void pcm_type(AM_MEDIA_TYPE *mt, enum audio_sample s,
		     enum audio_form form)
{
	WAVEFORMATEXTENSIBLE x;
	size_t n = audio_pcm_format(s, 2, RATE, 0, form, &x);
	CHECK_EQ(audio_wfx_to_mt(&x.Format, n, mt), S_OK);
}

static struct xform *open_class(int encoder)
{
	struct xform_class c;
	struct xform *x = NULL;

	CHECK_EQ(xform_find(server, NULL, 1, encoder, &c), S_OK);
	CHECK_EQ(c.dmo, 0);
	CHECK_EQ(c.compression, encoder ? TAG : 0);
	CHECK_EQ(xform_create(server, &c, &x), S_OK);
	return x;
}

/* Encodes all the PCM, in pieces of 777 frames; the stream's format in fmt. */
static void encode(struct got *g, WAVEFORMATEX **fmt, size_t *fmt_size,
		   ULONG bitrate)
{
	struct xform *x = open_class(1);
	AM_MEDIA_TYPE in, out = { 0 };

	if (!x)
		return;
	if (bitrate) {
		struct dshow_encoder_options o = { -1, -1.0, bitrate };
		CHECK_EQ(dshow_encoder_configure(xform_object(x), &o), S_OK);
	}
	pcm_type(&in, AUDIO_S16, AUDIO_FORM_PLAIN);
	/* Partial: the encoder fills in the format. */
	out.majortype = MEDIATYPE_Audio;
	out.subtype = (GUID)WAVE_FORMAT_SUBTYPE_INIT(TAG);
	CHECK_EQ(xform_connect(x, &in, &out, collect, g), S_OK);
	CHECK_EQ(audio_mt_to_wfx(xform_output_type(x), fmt, fmt_size), S_OK);
	for (int f = 0; f < FRAMES; f += 777) {
		int n = FRAMES - f < 777 ? FRAMES - f : 777;
		CHECK_EQ(xform_push(x, pcm + f * BLOCK, n * BLOCK,
				    audio_ticks(f, 1, RATE),
				    audio_ticks(f + n, 1, RATE),
				    AM_SAMPLE_SPLICEPOINT), S_OK);
	}
	CHECK_EQ(xform_end(x), S_OK);
	dshow_free_media_type(&in);
	xform_close(x);
}

/* Decodes the packets of e into g, with PCM of sample format s. */
static HRESULT decode(const struct got *e, const WAVEFORMATEX *fmt,
		      size_t fmt_size, enum audio_sample s, struct got *g)
{
	struct xform *x = open_class(0);
	AM_MEDIA_TYPE in, out;
	HRESULT hr = S_OK;
	size_t at = 0;

	if (!x)
		return E_FAIL;
	CHECK_EQ(audio_wfx_to_mt(fmt, fmt_size, &in), S_OK);
	pcm_type(&out, s, AUDIO_FORM_USUAL);
	hr = xform_connect(x, &in, &out, collect, g);
	for (int i = 0; SUCCEEDED(hr) && i < e->n && i < 64; i++) {
		hr = xform_push(x, e->data + at, e->sizes[i], e->start[i],
				e->stop[i], AM_SAMPLE_SPLICEPOINT);
		at += e->sizes[i];
	}
	if (SUCCEEDED(hr))
		hr = xform_end(x);
	dshow_free_media_type(&in);
	dshow_free_media_type(&out);
	xform_close(x);
	return hr;
}

static void test_encode_decode(void)
{
	struct got e = { 0 }, d = { 0 };
	WAVEFORMATEX *fmt = NULL;
	size_t fmt_size = 0;

	encode(&e, &fmt, &fmt_size, 0);
	CHECK(fmt != NULL);
	if (!fmt)
		return;
	CHECK_EQ(fmt_size, 20);
	CHECK_EQ(fmt->wFormatTag, TAG);
	CHECK_EQ(fmt->nChannels, 2);
	CHECK_EQ(fmt->nSamplesPerSec, RATE);
	CHECK_EQ(fmt->wBitsPerSample, 16);
	CHECK_EQ(fmt->nBlockAlign, 8 + 1024 * BLOCK);
	CHECK_EQ(((BYTE *)(fmt + 1))[0] | ((BYTE *)(fmt + 1))[1] << 8, 1024);

	/* Whole packets, the last short; each a sync point, timed. */
	CHECK_EQ(e.n, PACKETS);
	unsigned bad = 0;
	for (int i = 0; i < e.n && i < PACKETS; i++) {
		size_t frames = i < 4 ? 1024 : 904;
		bad += e.sizes[i] != 8 + frames * BLOCK || !e.sync[i] ||
		       !e.timed[i] ||
		       e.start[i] != audio_ticks(1024 * i, 1, RATE) ||
		       e.stop[i] != audio_ticks(1024 * i + frames, 1, RATE);
	}
	CHECK_EQ(bad, 0);

	/* Back to the same PCM, a sample per packet, at the packets' times. */
	CHECK_EQ(decode(&e, fmt, fmt_size, AUDIO_S16, &d), S_OK);
	CHECK_EQ(d.size, sizeof(pcm));
	CHECK(d.data && !memcmp(d.data, pcm, sizeof(pcm)));
	CHECK_EQ(d.n, PACKETS);
	CHECK_EQ(d.changes, 0);
	CHECK(d.timed[1] && d.start[1] == e.start[1] && d.stop[4] == e.stop[4]);
	got_free(&d);

	/* The decoder gives the stream's own sample format, no other. */
	CHECK(FAILED(decode(&e, fmt, fmt_size, AUDIO_S24, &d)));
	CHECK_EQ(d.n, 0);
	got_free(&d);
	CHECK(unloadable());

	/* The bit rate asked for reaches the encoder and its format. */
	got_free(&e);
	free(fmt);
	fmt = NULL;
	encode(&e, &fmt, &fmt_size, 256000);
	CHECK(fmt && fmt->nAvgBytesPerSec == 32000);
	CHECK_EQ(e.n, PACKETS);
	got_free(&e);
	free(fmt);
	CHECK(unloadable());
}

static void test_refused(void)
{
	struct xform *x;
	AM_MEDIA_TYPE pcm16, other = { 0 };
	WAVEFORMATEX tsta = { TAG, 2, RATE, 176400, 4104, 16, 0 };
	AM_MEDIA_TYPE tsta_mt;

	pcm_type(&pcm16, AUDIO_S16, AUDIO_FORM_PLAIN);
	CHECK_EQ(audio_wfx_to_mt(&tsta, sizeof(tsta), &tsta_mt), S_OK);

	/* The encoder takes PCM, and makes only TSTA. */
	x = open_class(1);
	other.majortype = MEDIATYPE_Audio;
	other.subtype = (GUID)WAVE_FORMAT_SUBTYPE_INIT(WAVE_FORMAT_MPEGLAYER3);
	CHECK(FAILED(xform_connect(x, &pcm16, &other, NULL, NULL)));
	CHECK(FAILED(xform_connect(x, &tsta_mt, &tsta_mt, NULL, NULL)));
	CHECK(xform_output_type(x) == NULL);
	/* Float isn't PCM to it. */
	AM_MEDIA_TYPE flt;
	pcm_type(&flt, AUDIO_F32, AUDIO_FORM_PLAIN);
	other.subtype = (GUID)WAVE_FORMAT_SUBTYPE_INIT(TAG);
	CHECK(FAILED(xform_connect(x, &flt, &other, NULL, NULL)));
	/* A whole output type of its own kind is taken as it is. */
	CHECK_EQ(xform_connect(x, &pcm16, &tsta_mt, NULL, NULL), S_OK);
	xform_close(x);

	/* The decoder takes TSTA, not PCM; its output only PCM. */
	x = open_class(0);
	CHECK(FAILED(xform_connect(x, &pcm16, &pcm16, NULL, NULL)));
	CHECK(FAILED(xform_connect(x, &tsta_mt, &flt, NULL, NULL)));
	CHECK(FAILED(xform_connect(x, &tsta_mt, &tsta_mt, NULL, NULL)));
	CHECK_EQ(xform_connect(x, &tsta_mt, &pcm16, NULL, NULL), S_OK);
	/* A packet that isn't TSTA. */
	CHECK(FAILED(xform_push(x, "nonsense", 8, 0, 1, 0)));
	xform_close(x);

	dshow_free_media_type(&pcm16);
	dshow_free_media_type(&flt);
	dshow_free_media_type(&tsta_mt);
	CHECK(unloadable());
}

/* A flush in the middle: the encoder starts its packets and times again. */
static void test_flush(void)
{
	struct xform *x = open_class(1);
	struct got g = { 0 };
	AM_MEDIA_TYPE in, out = { 0 };

	if (!x)
		return;
	pcm_type(&in, AUDIO_S16, AUDIO_FORM_PLAIN);
	out.majortype = MEDIATYPE_Audio;
	out.subtype = (GUID)WAVE_FORMAT_SUBTYPE_INIT(TAG);
	CHECK_EQ(xform_connect(x, &in, &out, collect, &g), S_OK);
	/* 1500 frames: one packet out, 476 kept. */
	CHECK_EQ(xform_push(x, pcm, 1500 * BLOCK, 0, 0, 0), S_OK);
	CHECK_EQ(g.n, 1);
	CHECK_EQ(xform_seek(x, 0, 0), S_OK);
	/* What was kept is gone; times count from the new first sample. */
	CHECK_EQ(xform_push(x, pcm, 1024 * BLOCK, 5000000, 0, 0), S_OK);
	CHECK_EQ(g.n, 2);
	CHECK_EQ(g.start[1], 5000000);
	CHECK_EQ(g.sizes[1], 8 + 1024 * BLOCK);
	CHECK_EQ(xform_end(x), S_OK);
	CHECK_EQ(g.n, 2);
	dshow_free_media_type(&in);
	xform_close(x);
	got_free(&g);
	CHECK(unloadable());
}

/* The modes, each in a process of its own. */

static void encoded(struct got *e, WAVEFORMATEX **fmt, size_t *size)
{
	encode(e, fmt, size, 0);
	CHECK_EQ(e->n, PACKETS);
}

static void mode_split(void)
{
	struct got e = { 0 }, d = { 0 };
	WAVEFORMATEX *fmt = NULL;
	size_t size = 0;

	encoded(&e, &fmt, &size);
	CHECK_EQ(decode(&e, fmt, size, AUDIO_S16, &d), S_OK);
	CHECK(d.size == sizeof(pcm) && !memcmp(d.data, pcm, sizeof(pcm)));
	/* Pieces of 300: 4 x (3 x 300 + 124), then 3 x 300 + 4. */
	CHECK_EQ(d.n, 4 * 4 + 4);
	CHECK_EQ(d.sizes[0], 300 * BLOCK);
	CHECK_EQ(d.sizes[3], 124 * BLOCK);
	CHECK_EQ(d.start[1], audio_ticks(300, 1, RATE));
	got_free(&e);
	got_free(&d);
	free(fmt);
}

static void mode_hold(void)
{
	struct got e = { 0 }, d = { 0 };
	WAVEFORMATEX *fmt = NULL;
	size_t size = 0;

	encoded(&e, &fmt, &size);
	/* One packet in gives nothing yet; the rest come out at the end. */
	struct xform *x = open_class(0);
	AM_MEDIA_TYPE in, out;
	CHECK_EQ(audio_wfx_to_mt(fmt, size, &in), S_OK);
	pcm_type(&out, AUDIO_S16, AUDIO_FORM_PLAIN);
	CHECK_EQ(xform_connect(x, &in, &out, collect, &d), S_OK);
	CHECK_EQ(xform_push(x, e.data, e.sizes[0], e.start[0], e.stop[0], 0),
		 S_OK);
	CHECK_EQ(d.n, 0);
	CHECK_EQ(xform_end(x), S_OK);
	CHECK_EQ(d.n, 1);
	CHECK_EQ(d.size, 1024 * BLOCK);
	dshow_free_media_type(&in);
	dshow_free_media_type(&out);
	xform_close(x);
	got_free(&d);

	CHECK_EQ(decode(&e, fmt, size, AUDIO_S16, &d), S_OK);
	CHECK(d.size == sizeof(pcm) && !memcmp(d.data, pcm, sizeof(pcm)));
	CHECK_EQ(d.n, PACKETS);
	got_free(&e);
	got_free(&d);
	free(fmt);
}

static void mode_float(void)
{
	struct got e = { 0 }, d = { 0 };
	WAVEFORMATEX *fmt = NULL;
	size_t size = 0;

	encoded(&e, &fmt, &size);
	CHECK_EQ(decode(&e, fmt, size, AUDIO_S16, &d), S_OK);
	/* The first packet as it was, then float, announced once. */
	CHECK_EQ(d.changes, 1);
	CHECK(d.changed && audio_format_sample(d.changed, d.changed_size) ==
			   AUDIO_F32);
	CHECK_EQ(d.size, 1024 * BLOCK + (FRAMES - 1024) * 8);
	if (d.size == 1024 * BLOCK + (FRAMES - 1024) * 8) {
		int16_t back[(FRAMES - 1024) * 2];
		CHECK(!memcmp(d.data, pcm, 1024 * BLOCK));
		audio_convert(AUDIO_F32, d.data + 1024 * BLOCK, AUDIO_S16, back,
			      (FRAMES - 1024) * 2);
		CHECK(!memcmp(back, pcm + 1024 * BLOCK, sizeof(back)));
	}
	got_free(&e);
	got_free(&d);
	free(fmt);
}

static void mode_rate(void)
{
	struct got e = { 0 }, d = { 0 };
	WAVEFORMATEX *fmt = NULL;
	size_t size = 0;

	encoded(&e, &fmt, &size);
	/* A sink that can't follow the change stops the stream there. */
	d.refuse_rate = RATE;
	CHECK_EQ(decode(&e, fmt, size, AUDIO_S16, &d), VFW_E_TYPE_NOT_ACCEPTED);
	CHECK_EQ(d.n, 1);
	CHECK(d.changed && d.changed->nSamplesPerSec == 2 * RATE);
	got_free(&e);
	got_free(&d);
	free(fmt);
}

static const struct {
	const char *name, *variable;
	void (*run)(void);
} modes[] = {
	{ "split", "TESTFILTER_AUDIO_SPLIT", mode_split },
	{ "hold", "TESTFILTER_AUDIO_HOLD", mode_hold },
	{ "float", "TESTFILTER_AUDIO_FLOAT", mode_float },
	{ "rate", "TESTFILTER_AUDIO_RATE", mode_rate },
};
#define NMODES (sizeof(modes) / sizeof(*modes))

int main(int argc, char **argv)
{
	server = fixture("testfilter.ax");
	for (size_t i = 0; i < sizeof(pcm); i++)
		pcm[i] = (BYTE)(i * 13 + (i >> 9) * 5);
	CHECK_EQ(runtime_enter(), 0);
	/* Kept loaded, so DllCanUnloadNow can be asked between tests. */
	struct module *m = mod_load(server, NULL);
	CHECK(m != NULL);

	if (argc == 2) {
		for (size_t i = 0; i < NMODES; i++)
			if (!strcmp(argv[1], modes[i].name))
				modes[i].run();
		CHECK(unloadable());
		return test_summary(argv[1]);
	}

	test_encode_decode();
	test_refused();
	test_flush();
	for (size_t i = 0; i < NMODES; i++) {
		char cmd[4400];
		snprintf(cmd, sizeof(cmd), "VFW_ENV_%s=1 '%s' %s",
			 modes[i].variable, argv[0], modes[i].name);
		int status = system(cmd);
		CHECK_EQ(status, 0);
		if (status)
			fprintf(stderr, "test_dshow_audio: mode %s failed\n",
				modes[i].name);
	}
	return test_summary("test_dshow_audio");
}
