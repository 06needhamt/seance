// SPDX-License-Identifier: GPL-2.0-only
/*
 * RealPlayer's codec DLLs. See real.h.
 */
#include "real/real.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "common/error.h"
#include "common/log.h"
#include "loader/modules.h"
#include "loader/pe.h"
#include "out/image.h"
#include "runtime/crash.h"
#include "runtime/gate.h"
#include "runtime/guard.h"
#include "runtime/runtime.h"

/* The largest PCM a block gives: 8192 samples of 8 channels. */
#define PCM_MAX (8192 * 8 * 2)

/* Calls fn(args...) in the DLL, as `what` in crash reports. */
static uintptr_t call(const char *what, const void *fn, unsigned n,
		      const uintptr_t *args)
{
	struct crash_frame saved;
	TRACE("real: %s", what);
	crash_enter(what, fn, 0, NULL, &saved);
	uintptr_t r = gate_call(fn, n, args);
	crash_leave(&saved);
	TRACE("real: %s = %#lx", what, (unsigned long)r);
	return r;
}
#define CALL(what, fn, ...) ({ \
	const uintptr_t args_[] = { __VA_ARGS__ }; \
	call(what, fn, sizeof(args_) / sizeof(args_[0]), args_); \
})
#define P(x) ((uintptr_t)(x))

struct find {
	int video20, video40, audio, pncodec, helix, renderer;
};

static int note_export(void *ctx, const struct pe_export *e)
{
	struct find *f = ctx;
	const char *n = e->name;

	if (!n)
		return 0;
	if (!strcmp(n, "RV20toYUV420Transform"))
		f->video20 = 1;
	else if (!strcmp(n, "RV40toYUV420Transform"))
		f->video40 = 1;
	else if (!strcmp(n, "RADecode"))
		f->audio = 1;
	else if (!strncmp(n, "PNCodec_", 8))
		f->pncodec = 1;
	else if (!strcmp(n, "RACreateDecoderInstance"))
		f->helix = 1;
	else if (!strcmp(n, "RMACreateInstance"))
		f->renderer = 1;
	return 0;
}

static enum real_kind kind_of(const struct module *m, const char **entry)
{
	struct find f = { 0 };

	if (entry)
		*entry = NULL;
	if (m->img)
		pe_foreach_export(m->img, note_export, &f);
	if (f.video40 || f.video20) {
		/* Both: RealMediaSplitter.ax; RV20's set takes RV10 and RV20 */
		if (entry)
			*entry = f.video20 ? "RV20toYUV420" : "RV40toYUV420";
		return REAL_VIDEO;
	}
	if (f.audio)
		return REAL_AUDIO;
	if (f.pncodec)
		return REAL_PNCODEC;
	if (f.helix)
		return REAL_HELIX;
	if (f.renderer)
		return REAL_RENDERER;
	return REAL_NONE;
}

enum real_kind real_kind(const char *dll, const char **entry)
{
	/* From the file: loading it runs DllMain, which may fail */
	struct pe_image *img = pe_open(dll);
	if (!img)
		return REAL_NONE;
	struct module m = { .img = img };
	enum real_kind k = kind_of(&m, entry);
	pe_free(img);
	return k;
}

const char *real_kind_name(enum real_kind kind)
{
	switch (kind) {
	case REAL_VIDEO: return "RealVideo decoder";
	case REAL_AUDIO: return "RealAudio decoder";
	case REAL_PNCODEC: return "RealPlayer plug-in (PNCodec)";
	case REAL_HELIX: return "Helix audio decoder";
	case REAL_RENDERER: return "RealPlayer renderer plug-in";
	default: return "not a RealPlayer codec";
	}
}

/* Video */

/* RV*toYUV420Init's first argument */
struct rv_init {
	int16_t unknown1;	/* 11 */
	int16_t width, height;
	int16_t unknown2;	/* 0 */
	uint32_t unknown3;	/* 0 */
	uint32_t subformat;	/* the stream's extra data, words 0 and 1 */
	uint32_t unknown4;	/* 1 */
	uint32_t format;
};

/* RV*toYUV420Transform's third and fourth */
struct rv_in {
	uint32_t len;
	uint32_t interpolate;
	uint32_t nsegments;	/* slices less one */
	const uint32_t *segments;	/* (1, offset) a slice */
	uint32_t flags;
	uint32_t time;
};

struct rv_out {
	uint32_t frames;	/* 0: nothing to show for it */
	uint32_t notes;
	uint32_t time;
	uint32_t width, height;
};

/* A custom message: 0x24 gives the sizes a stream may switch between */
struct rv_message {
	uint32_t id;
	uint32_t count;
	const uint32_t *data;
};

struct real_video {
	struct module *mod;
	void *init, *transform, *message, *free_;
	void *context;
	int width, height;
	BYTE *pic;		/* the decoder's output, I420 */
	size_t pic_size;
	uint32_t segments[RM_MAX_SLICES * 2];
};

static void *entry_proc(struct module *m, const char *set, const char *name)
{
	char full[64];
	snprintf(full, sizeof(full), "%s%s", set, name);
	return mod_get_proc(m, full);
}

struct real_video *real_video_open(const char *dll, const struct rm_stream *st)
{
	const char *set;

	if (!st->video) {
		vfw_set_error("not a video stream");
		return NULL;
	}
	if (st->extra_len < 8) {
		vfw_set_error("the stream's header has no codec version");
		return NULL;
	}
	if (runtime_enter())
		return NULL;
	struct real_video *v = calloc(1, sizeof(*v));
	if (!v) {
		vfw_set_error("out of memory");
		return NULL;
	}
	if (!(v->mod = mod_load(dll, NULL)))
		goto fail;
	if (kind_of(v->mod, &set) != REAL_VIDEO) {
		vfw_set_error("%s: no RealVideo decoder in it", v->mod->name);
		goto fail;
	}
	v->init = entry_proc(v->mod, set, "Init");
	v->transform = entry_proc(v->mod, set, "Transform");
	v->message = entry_proc(v->mod, set, "CustomMessage");
	v->free_ = entry_proc(v->mod, set, "Free");
	if (!v->init || !v->transform) {
		vfw_set_error("%s: has %sTransform but not %sInit",
			      v->mod->name, set, set);
		goto fail;
	}
	v->width = (int)st->width;
	v->height = (int)st->height;
	uint32_t subformat = (uint32_t)st->extra[0] << 24 |
			     (uint32_t)st->extra[1] << 16 |
			     (uint32_t)st->extra[2] << 8 | st->extra[3];
	uint32_t format = (uint32_t)st->extra[4] << 24 |
			  (uint32_t)st->extra[5] << 16 |
			  (uint32_t)st->extra[6] << 8 | st->extra[7];
	struct rv_init init = {
		.unknown1 = 11, .width = (int16_t)st->width,
		.height = (int16_t)st->height, .subformat = subformat,
		.unknown4 = 1, .format = format,
	};
	uintptr_t r = CALL("RV*toYUV420Init", v->init, P(&init), P(&v->context));
	if ((uint32_t)r) {
		vfw_set_error("%s: %sInit refused the stream ('%.4s', version "
			      "%#x, subformat %#x): %#x", v->mod->name, set,
			      (const char *)&st->fourcc, format, subformat,
			      (unsigned)r);
		goto fail;
	}
	/*
	 * RV20 from 2.2 on, and RV30, can change size within the stream: the
	 * decoder is told the sizes, from the bytes after the version, a
	 * pair each in units of 4 pixels.
	 */
	unsigned nsizes = format & 7;
	if (v->message && nsizes && (st->fourcc == mmioFOURCC('R', 'V', '3', '0') ||
				     (st->fourcc == mmioFOURCC('R', 'V', '2', '0') &&
				      format >= 0x20200002))) {
		uint32_t sizes[2 + 2 * 8] = { st->width, st->height };
		for (unsigned i = 0; i < 2 * nsizes && 8 + i < st->extra_len; i++)
			sizes[2 + i] = 4u * st->extra[8 + i];
		struct rv_message m = { 0x24, 1 + nsizes, sizes };
		CALL("RV*toYUV420CustomMessage", v->message, P(&m),
		     P(v->context));
		/* The largest size it may switch to */
		for (unsigned i = 0; i < nsizes; i++) {
			if ((int)sizes[2 + 2 * i] > v->width)
				v->width = (int)sizes[2 + 2 * i];
			if ((int)sizes[3 + 2 * i] > v->height)
				v->height = (int)sizes[3 + 2 * i];
		}
	}
	v->pic_size = image_i420_size(v->width, v->height);
	if (!(v->pic = guard_alloc(v->pic_size, "output picture"))) {
		vfw_set_error("out of memory");
		goto fail;
	}
	return v;
fail:
	real_video_close(v);
	return NULL;
}

void real_video_close(struct real_video *v)
{
	if (!v)
		return;
	if (v->context && v->free_)
		CALL("RV*toYUV420Free", v->free_, P(v->context));
	guard_free(v->pic);
	if (v->mod)
		mod_free(v->mod);
	free(v);
}

int real_video_decode(struct real_video *v, const struct rm_frame *f,
		      const BYTE **pic, int *w, int *h)
{
	if (!f->nslices || f->nslices > RM_MAX_SLICES) {
		vfw_set_error("a frame of %u slices", f->nslices);
		return -1;
	}
	for (unsigned i = 0; i < f->nslices; i++) {
		v->segments[2 * i] = 1;
		v->segments[2 * i + 1] = f->offsets[i];
	}
	struct rv_in in = {
		.len = (uint32_t)f->len, .nsegments = f->nslices - 1,
		.segments = v->segments, .flags = 0, .time = f->time,
	};
	struct rv_out out = { 0 };
	uintptr_t r = CALL("RV*toYUV420Transform", v->transform, P(f->data),
			   P(v->pic), P(&in), P(&out), P(v->context));
	if ((uint32_t)r) {
		vfw_set_error("the decoder refused the frame at %u ms: %#x",
			      f->time, (unsigned)r);
		return -1;
	}
	if (!out.frames)
		return 0;
	*w = out.width ? (int)out.width : v->width;
	*h = out.height ? (int)out.height : v->height;
	if (image_i420_size(*w, *h) > v->pic_size) {
		vfw_set_error("the decoder gave %dx%d, larger than its %dx%d",
			      *w, *h, v->width, v->height);
		return -1;
	}
	*pic = v->pic;
	return 1;
}

/* Audio */

/* RAInitDecoder's second argument */
struct ra_init {
	uint32_t rate;
	uint16_t bits;
	uint16_t channels;
	uint16_t quality;	/* 100 */
	uint16_t pad;
	uint32_t bits_per_frame;	/* the sub-packet size */
	uint32_t packet_size;		/* the coded frame size */
	uint32_t extra_len;
	const void *extra;
};

struct real_audio {
	struct module *mod;
	void *decode, *free_, *close;
	void *context;
	WAVEFORMATEX wf;
	BYTE *pcm;
	BYTE *in;		/* a copy of the block: decoders may write it */
	size_t in_cap;
};

/*
 * The DLL's directory as it sees it, ending in a separator: RAOpenCodec2
 * looks there for the parts some codecs load (cook's and sipr's).
 */
static void module_dir(struct module *m, char *dir, size_t size)
{
	struct module *k32 = mod_load("kernel32.dll", NULL);
	void *get = k32 ? mod_get_proc(k32, "GetModuleFileNameA") : NULL;

	dir[0] = 0;
	if (get) {
		uintptr_t n = CALL("GetModuleFileNameA", get, P(m->base),
				   P(dir), (uintptr_t)size);
		if (n >= size)
			dir[0] = 0;
	}
	if (k32)
		mod_free(k32);
	char *slash = strrchr(dir, '\\'), *fwd = strrchr(dir, '/');
	if (fwd && (!slash || fwd > slash))
		slash = fwd;
	if (slash)
		slash[1] = 0;
	else
		dir[0] = 0;
}

struct real_audio *real_audio_open(const char *dll, const struct rm_stream *st)
{
	if (!st->audio) {
		vfw_set_error("not an audio stream");
		return NULL;
	}
	if (st->bits != 16 || !st->channels || st->channels > 8 || !st->rate) {
		vfw_set_error("a stream of %u-bit, %u-channel audio at %u Hz",
			      st->bits, st->channels, st->rate);
		return NULL;
	}
	if (runtime_enter())
		return NULL;
	struct real_audio *a = calloc(1, sizeof(*a));
	if (!a) {
		vfw_set_error("out of memory");
		return NULL;
	}
	if (!(a->mod = mod_load(dll, NULL)))
		goto fail;
	if (kind_of(a->mod, NULL) != REAL_AUDIO) {
		vfw_set_error("%s: no RealAudio decoder in it", a->mod->name);
		goto fail;
	}
	void *open2 = mod_get_proc(a->mod, "RAOpenCodec2");
	void *open1 = mod_get_proc(a->mod, "RAOpenCodec");
	void *init = mod_get_proc(a->mod, "RAInitDecoder");
	void *flavor = mod_get_proc(a->mod, "RASetFlavor");
	a->decode = mod_get_proc(a->mod, "RADecode");
	a->free_ = mod_get_proc(a->mod, "RAFreeDecoder");
	a->close = mod_get_proc(a->mod, "RACloseCodec");
	if ((!open2 && !open1) || !init) {
		vfw_set_error("%s: has RADecode but not RAOpenCodec and "
			      "RAInitDecoder", a->mod->name);
		goto fail;
	}
	uintptr_t r;
	if (open2) {
		char dir[1024];
		module_dir(a->mod, dir, sizeof(dir));
		r = CALL("RAOpenCodec2", open2, P(&a->context), P(dir));
	} else {
		r = CALL("RAOpenCodec", open1, P(&a->context));
	}
	if ((uint32_t)r || !a->context) {
		vfw_set_error("%s: RAOpenCodec%s failed: %#x", a->mod->name,
			      open2 ? "2" : "", (unsigned)r);
		a->context = NULL;
		goto fail;
	}
	struct ra_init ri = {
		.rate = st->rate, .bits = (uint16_t)st->bits,
		.channels = (uint16_t)st->channels, .quality = 100,
		.bits_per_frame = st->sub_packet_size,
		.packet_size = st->coded_frame_size,
		.extra_len = (uint32_t)st->codec_data_len,
		.extra = st->codec_data,
	};
	r = CALL("RAInitDecoder", init, P(a->context), P(&ri));
	if ((uint32_t)r) {
		vfw_set_error("%s: RAInitDecoder refused '%.4s' (%u Hz, %u "
			      "channels, flavor %u): %#x", a->mod->name,
			      (const char *)&st->fourcc, st->rate, st->channels,
			      st->flavor, (unsigned)r);
		goto fail;
	}
	if (flavor) {
		r = CALL("RASetFlavor", flavor, P(a->context), st->flavor);
		if ((uint32_t)r) {
			vfw_set_error("%s: RASetFlavor(%u) failed: %#x",
				      a->mod->name, st->flavor, (unsigned)r);
			goto fail;
		}
	}
	if (!(a->pcm = guard_alloc(PCM_MAX, "PCM buffer"))) {
		vfw_set_error("out of memory");
		goto fail;
	}
	a->wf.wFormatTag = WAVE_FORMAT_PCM;
	a->wf.nChannels = (WORD)st->channels;
	a->wf.nSamplesPerSec = st->rate;
	a->wf.wBitsPerSample = 16;
	a->wf.nBlockAlign = (WORD)(2 * st->channels);
	a->wf.nAvgBytesPerSec = st->rate * a->wf.nBlockAlign;
	return a;
fail:
	real_audio_close(a);
	return NULL;
}

void real_audio_close(struct real_audio *a)
{
	if (!a)
		return;
	if (a->context && a->free_)
		CALL("RAFreeDecoder", a->free_, P(a->context));
	if (a->context && a->close)
		CALL("RACloseCodec", a->close, P(a->context));
	guard_free(a->pcm);
	free(a->in);
	if (a->mod)
		mod_free(a->mod);
	free(a);
}

const WAVEFORMATEX *real_audio_format(const struct real_audio *a)
{
	return &a->wf;
}

int real_audio_decode(struct real_audio *a, const BYTE *in, size_t len,
		      const BYTE **pcm, size_t *pcm_len)
{
	uint32_t got = 0;

	if (!len) {
		/* Nothing to decode: no call, no PCM */
		*pcm = a->pcm;
		*pcm_len = 0;
		return 0;
	}
	if (len > a->in_cap) {
		BYTE *b = realloc(a->in, len);
		if (!b) {
			vfw_set_error("out of memory");
			return -1;
		}
		a->in = b;
		a->in_cap = len;
	}
	memcpy(a->in, in, len);
	/*
	 * RADecode takes no size for the PCM buffer, and how much a block
	 * gives is the codec's business. The buffer is guard_alloc()ed: a
	 * DLL that writes past PCM_MAX (and the slack) hits the guard page
	 * inside the call, which the crash handler reports, and never the
	 * heap. The check after the call catches one that says it gave more.
	 */
	uintptr_t r = CALL("RADecode", a->decode, P(a->context), P(a->in),
			   (uintptr_t)len, P(a->pcm), P(&got),
			   (uintptr_t)-1);
	if ((uint32_t)r) {
		vfw_set_error("RADecode refused a block of %zu bytes: %#x", len,
			      (unsigned)r);
		return -1;
	}
	if (got > PCM_MAX) {
		vfw_set_error("RADecode says it gave %u bytes, more than "
			      "it had room for", got);
		return -1;
	}
	*pcm = a->pcm;
	*pcm_len = got - got % a->wf.nBlockAlign;
	return 0;
}
