/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * Audio formats: tags, checks, descriptions, media types and PCM sample
 * conversion.
 */
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "common/error.h"
#include "media/audio.h"
#include "win32/ole.h"

/* Tags mmreg.h doesn't name that are common in AVI files. */
#define TAG_AAC_FAAD	0x706d
#define TAG_AC3		0x2000
#define TAG_DTS_AVI	0x2001
#define TAG_VORBIS1	0x674f
#define TAG_VORBIS3P	0x6771

static const GUID tag_base = WAVE_FORMAT_SUBTYPE_INIT(0);

static const char *const sample_names[] = {
	[AUDIO_NONE] = "none",
	[AUDIO_U8] = "u8",
	[AUDIO_S16] = "s16",
	[AUDIO_S24] = "s24",
	[AUDIO_S32] = "s32",
	[AUDIO_F32] = "f32",
	[AUDIO_F64] = "f64",
};

static const unsigned sample_bytes[] = {
	[AUDIO_NONE] = 0,
	[AUDIO_U8] = 1,
	[AUDIO_S16] = 2,
	[AUDIO_S24] = 3,
	[AUDIO_S32] = 4,
	[AUDIO_F32] = 4,
	[AUDIO_F64] = 8,
};

const char *audio_sample_name(enum audio_sample s)
{
	return (unsigned)s <= AUDIO_F64 ? sample_names[s] : "none";
}

int audio_sample_from_name(const char *name, enum audio_sample *s)
{
	for (unsigned i = AUDIO_U8; i <= AUDIO_F64; i++)
		if (!strcmp(name, sample_names[i])) {
			*s = i;
			return 0;
		}
	vfw_set_error("unknown sample format \"%s\" (u8, s16, s24, s32, f32 "
		      "or f64)", name);
	return -1;
}

unsigned audio_sample_bytes(enum audio_sample s)
{
	return (unsigned)s <= AUDIO_F64 ? sample_bytes[s] : 0;
}

/* The bytes of extra data the format really has. */
static size_t extra_size(const WAVEFORMATEX *wf, size_t size)
{
	if (size < sizeof(*wf))
		return 0;
	return wf->cbSize < size - sizeof(*wf) ? wf->cbSize :
	       size - sizeof(*wf);
}

static int is_extensible(const WAVEFORMATEX *wf, size_t size)
{
	return wf->wFormatTag == WAVE_FORMAT_EXTENSIBLE &&
	       extra_size(wf, size) >= sizeof(WAVEFORMATEXTENSIBLE) -
				       sizeof(WAVEFORMATEX);
}

WORD audio_format_tag(const WAVEFORMATEX *wf, size_t size)
{
	if (size < 16)
		return WAVE_FORMAT_UNKNOWN;
	if (!is_extensible(wf, size))
		return wf->wFormatTag;

	GUID sub;
	memcpy(&sub, &((const WAVEFORMATEXTENSIBLE *)wf)->SubFormat,
	       sizeof(sub));
	if (sub.Data1 <= 0xffff && sub.Data1 != WAVE_FORMAT_EXTENSIBLE &&
	    !memcmp((BYTE *)&sub + 4, (const BYTE *)&tag_base + 4,
		    sizeof(sub) - 4))
		return (WORD)sub.Data1;
	return WAVE_FORMAT_EXTENSIBLE;
}

int audio_format_check(const WAVEFORMATEX *wf, size_t size)
{
	if (!wf || size < 16) {
		vfw_set_error("audio format too short (%zu bytes)", wf ? size : 0);
		return -1;
	}
	if (size >= sizeof(*wf) && wf->cbSize > size - sizeof(*wf)) {
		vfw_set_error("audio format says %u bytes follow it, but %zu do",
			      wf->cbSize, size - sizeof(*wf));
		return -1;
	}
	if (!wf->nChannels || !wf->nSamplesPerSec || !wf->nBlockAlign) {
		vfw_set_error("audio format has no %s",
			      !wf->nChannels ? "channels" :
			      !wf->nSamplesPerSec ? "sample rate" :
			      "block size");
		return -1;
	}
	if (wf->wFormatTag == WAVE_FORMAT_EXTENSIBLE &&
	    !is_extensible(wf, size)) {
		vfw_set_error("WAVE_FORMAT_EXTENSIBLE format is too short");
		return -1;
	}

	WORD tag = audio_format_tag(wf, size);
	if (tag != WAVE_FORMAT_PCM && tag != WAVE_FORMAT_IEEE_FLOAT)
		return 0;
	WORD bits = wf->wBitsPerSample;
	if (!bits || bits % 8 || bits > 64 ||
	    (tag == WAVE_FORMAT_IEEE_FLOAT && bits != 32 && bits != 64)) {
		vfw_set_error("%s audio with %u bits per sample",
			      tag == WAVE_FORMAT_PCM ? "PCM" : "float", bits);
		return -1;
	}
	if (wf->nBlockAlign != wf->nChannels * (bits / 8) ||
	    wf->nAvgBytesPerSec != wf->nSamplesPerSec * wf->nBlockAlign) {
		vfw_set_error("PCM block size %u or byte rate %lu doesn't match "
			      "%u channels of %u bits at %lu Hz",
			      wf->nBlockAlign, (unsigned long)wf->nAvgBytesPerSec,
			      wf->nChannels, bits,
			      (unsigned long)wf->nSamplesPerSec);
		return -1;
	}
	if (is_extensible(wf, size) &&
	    ((const WAVEFORMATEXTENSIBLE *)wf)->Samples.wValidBitsPerSample >
	    bits) {
		vfw_set_error("more valid bits than bits per sample");
		return -1;
	}
	return 0;
}

enum audio_sample audio_format_sample(const WAVEFORMATEX *wf, size_t size)
{
	WORD tag = audio_format_tag(wf, size);

	if (size < 16 || !wf->nChannels ||
	    wf->nBlockAlign != wf->nChannels * (wf->wBitsPerSample / 8))
		return AUDIO_NONE;
	if (tag == WAVE_FORMAT_PCM)
		switch (wf->wBitsPerSample) {
		case 8: return AUDIO_U8;
		case 16: return AUDIO_S16;
		case 24: return AUDIO_S24;
		case 32: return AUDIO_S32;
		}
	if (tag == WAVE_FORMAT_IEEE_FLOAT)
		switch (wf->wBitsPerSample) {
		case 32: return AUDIO_F32;
		case 64: return AUDIO_F64;
		}
	return AUDIO_NONE;
}

const char *audio_tag_name(WORD tag)
{
	switch (tag) {
	case WAVE_FORMAT_PCM:            return "PCM";
	case WAVE_FORMAT_ADPCM:          return "MS ADPCM";
	case WAVE_FORMAT_IEEE_FLOAT:     return "float";
	case WAVE_FORMAT_ALAW:           return "A-law";
	case WAVE_FORMAT_MULAW:          return "mu-law";
	case WAVE_FORMAT_DTS:
	case TAG_DTS_AVI:                return "DTS";
	case WAVE_FORMAT_WMAVOICE9:      return "WMA Voice";
	case WAVE_FORMAT_IMA_ADPCM:      return "IMA ADPCM";
	case WAVE_FORMAT_GSM610:         return "GSM 6.10";
	case WAVE_FORMAT_MPEG:           return "MPEG audio";
	case WAVE_FORMAT_MPEGLAYER3:     return "MP3";
	case WAVE_FORMAT_DOLBY_AC3_SPDIF: return "AC-3 S/PDIF";
	case TAG_AC3:                    return "AC-3";
	case WAVE_FORMAT_RAW_AAC1:
	case WAVE_FORMAT_MPEG_RAW_AAC:
	case TAG_AAC_FAAD:               return "AAC";
	case WAVE_FORMAT_MPEG_ADTS_AAC:  return "AAC (ADTS)";
	case WAVE_FORMAT_MPEG_LOAS:      return "AAC (LOAS)";
	case WAVE_FORMAT_MPEG_HEAAC:     return "HE-AAC";
	case WAVE_FORMAT_WMAUDIO2:       return "WMA";
	case WAVE_FORMAT_WMAUDIO3:       return "WMA Pro";
	case WAVE_FORMAT_WMAUDIO_LOSSLESS: return "WMA Lossless";
	case WAVE_FORMAT_WMASPDIF:       return "WMA S/PDIF";
	case WAVE_FORMAT_OPUS:           return "Opus";
	case WAVE_FORMAT_FLAC:           return "FLAC";
	case WAVE_FORMAT_EXTENSIBLE:     return "extensible";
	}
	if (tag >= TAG_VORBIS1 && tag <= TAG_VORBIS3P)
		return "Vorbis";
	return NULL;
}

const char *audio_format_desc(const WAVEFORMATEX *wf, size_t size, char *buf)
{
	enum audio_sample s;
	WORD tag;
	int n;

	if (size < 16) {
		snprintf(buf, AUDIO_DESC_SIZE, "%zu-byte audio format", size);
		return buf;
	}
	tag = audio_format_tag(wf, size);
	s = audio_format_sample(wf, size);
	if (s != AUDIO_NONE) {
		n = snprintf(buf, AUDIO_DESC_SIZE, "PCM %s", audio_sample_name(s));
	} else if (tag == WAVE_FORMAT_EXTENSIBLE) {
		const GUID *g = &((const WAVEFORMATEXTENSIBLE *)wf)->SubFormat;
		n = snprintf(buf, AUDIO_DESC_SIZE,
			     "extensible {%08lx-%04x-%04x-...}",
			     (unsigned long)g->Data1, g->Data2, g->Data3);
	} else if (audio_tag_name(tag)) {
		n = snprintf(buf, AUDIO_DESC_SIZE, "%s (0x%04x)",
			     audio_tag_name(tag), tag);
	} else {
		n = snprintf(buf, AUDIO_DESC_SIZE, "wave format 0x%04x", tag);
	}
	if (n < 0 || n >= AUDIO_DESC_SIZE)
		return buf;
	if (is_extensible(wf, size)) {
		const WAVEFORMATEXTENSIBLE *x = (const void *)wf;
		WORD valid = x->Samples.wValidBitsPerSample;
		if (s != AUDIO_NONE && valid && valid != wf->wBitsPerSample)
			n += snprintf(buf + n, AUDIO_DESC_SIZE - n,
				      " (%u valid bits)", valid);
		else if (s != AUDIO_NONE)
			n += snprintf(buf + n, AUDIO_DESC_SIZE - n,
				      " extensible");
	}
	if (n < 0 || n >= AUDIO_DESC_SIZE)
		return buf;
	n += snprintf(buf + n, AUDIO_DESC_SIZE - n, ", %u channel%s, %lu Hz",
		      wf->nChannels, wf->nChannels == 1 ? "" : "s",
		      (unsigned long)wf->nSamplesPerSec);
	if (s == AUDIO_NONE && wf->nAvgBytesPerSec && n > 0 &&
	    n < AUDIO_DESC_SIZE)
		snprintf(buf + n, AUDIO_DESC_SIZE - n, ", %lu kb/s",
			 (unsigned long)((wf->nAvgBytesPerSec * 8ull + 500) /
					 1000));
	return buf;
}

DWORD audio_channel_mask(WORD channels)
{
	static const DWORD masks[] = {
		0,
		SPEAKER_FRONT_CENTER,
		SPEAKER_FRONT_LEFT | SPEAKER_FRONT_RIGHT,
		SPEAKER_FRONT_LEFT | SPEAKER_FRONT_RIGHT | SPEAKER_FRONT_CENTER,
		SPEAKER_FRONT_LEFT | SPEAKER_FRONT_RIGHT | SPEAKER_BACK_LEFT |
			SPEAKER_BACK_RIGHT,
		SPEAKER_FRONT_LEFT | SPEAKER_FRONT_RIGHT | SPEAKER_FRONT_CENTER |
			SPEAKER_BACK_LEFT | SPEAKER_BACK_RIGHT,
		/* 5.1 */
		0x3f,
		/* 6.1: 5.1 and the back centre */
		0x3f | SPEAKER_BACK_CENTER,
		/* 7.1: 5.1 and the sides */
		0x3f | SPEAKER_SIDE_LEFT | SPEAKER_SIDE_RIGHT,
	};

	return channels < sizeof(masks) / sizeof(masks[0]) ? masks[channels] : 0;
}

size_t audio_pcm_format(enum audio_sample s, WORD channels, DWORD rate,
			DWORD mask, enum audio_form form,
			WAVEFORMATEXTENSIBLE *out)
{
	unsigned bytes = audio_sample_bytes(s);
	int is_float = s == AUDIO_F32 || s == AUDIO_F64;

	memset(out, 0, sizeof(*out));
	if (!bytes)
		return 0;
	if (form == AUDIO_FORM_USUAL)
		form = channels > 2 || s == AUDIO_S24 || s == AUDIO_S32 ?
		       AUDIO_FORM_EXTENSIBLE : AUDIO_FORM_PLAIN;

	WAVEFORMATEX *wf = &out->Format;
	wf->wFormatTag = is_float ? WAVE_FORMAT_IEEE_FLOAT : WAVE_FORMAT_PCM;
	wf->nChannels = channels;
	wf->nSamplesPerSec = rate;
	wf->nBlockAlign = (WORD)(channels * bytes);
	wf->nAvgBytesPerSec = rate * wf->nBlockAlign;
	wf->wBitsPerSample = (WORD)(bytes * 8);
	if (form == AUDIO_FORM_PLAIN)
		return sizeof(*wf);

	wf->wFormatTag = WAVE_FORMAT_EXTENSIBLE;
	wf->cbSize = sizeof(*out) - sizeof(*wf);
	out->Samples.wValidBitsPerSample = wf->wBitsPerSample;
	out->dwChannelMask = mask ? mask : audio_channel_mask(channels);
	out->SubFormat = is_float ? KSDATAFORMAT_SUBTYPE_IEEE_FLOAT :
		       KSDATAFORMAT_SUBTYPE_PCM;
	return sizeof(*out);
}

size_t audio_output_preferences(const WAVEFORMATEX *in, size_t size,
				enum audio_sample want,
				WAVEFORMATEXTENSIBLE *out, size_t max)
{
	static const enum audio_sample usual[] = {
		AUDIO_S16, AUDIO_S24, AUDIO_S32, AUDIO_F32, AUDIO_U8,
	};
	const enum audio_sample *list = want != AUDIO_NONE ? &want : usual;
	size_t nlist = want != AUDIO_NONE ? 1 : sizeof(usual) / sizeof(usual[0]);
	size_t n = 0;
	DWORD mask = 0;

	if (size < 16 || !in->nChannels || !in->nSamplesPerSec)
		return 0;
	if (is_extensible(in, size))
		mask = ((const WAVEFORMATEXTENSIBLE *)in)->dwChannelMask;

	for (size_t i = 0; i < nlist; i++) {
		enum audio_form forms[2] = {
			AUDIO_FORM_PLAIN, AUDIO_FORM_EXTENSIBLE,
		};
		if (in->nChannels > 2) {
			forms[0] = AUDIO_FORM_EXTENSIBLE;
			forms[1] = AUDIO_FORM_PLAIN;
		}
		for (int f = 0; f < 2 && n < max; f++)
			if (audio_pcm_format(list[i], in->nChannels,
					     in->nSamplesPerSec, mask, forms[f],
					     &out[n]))
				n++;
	}
	return n;
}

HRESULT audio_wfx_to_mt(const WAVEFORMATEX *wf, size_t size,
			AM_MEDIA_TYPE *mt)
{
	WORD tag;
	size_t extra, cb;

	memset(mt, 0, sizeof(*mt));
	if (!wf || size < 16)
		return E_INVALIDARG;
	extra = extra_size(wf, size);
	cb = sizeof(WAVEFORMATEX) + extra;
	if (!(mt->pbFormat = ole_taskmem_alloc(cb)))
		return E_OUTOFMEMORY;
	memset(mt->pbFormat, 0, cb);
	memcpy(mt->pbFormat, wf, size < cb ? size : cb);
	((WAVEFORMATEX *)mt->pbFormat)->cbSize = (WORD)extra;
	mt->cbFormat = (ULONG)cb;

	tag = audio_format_tag(wf, size);
	mt->majortype = MEDIATYPE_Audio;
	if (tag == WAVE_FORMAT_EXTENSIBLE)
		mt->subtype = ((const WAVEFORMATEXTENSIBLE *)wf)->SubFormat;
	else
		mt->subtype = (GUID)WAVE_FORMAT_SUBTYPE_INIT(tag);
	mt->formattype = FORMAT_WaveFormatEx;
	if (audio_format_sample(wf, size) != AUDIO_NONE) {
		mt->bFixedSizeSamples = TRUE;
		mt->lSampleSize = wf->nBlockAlign;
	}
	return S_OK;
}

HRESULT audio_mt_to_wfx(const AM_MEDIA_TYPE *mt, WAVEFORMATEX **wf,
			size_t *size)
{
	size_t cb, extra;
	WAVEFORMATEX *w;

	*wf = NULL;
	*size = 0;
	if (memcmp(&mt->majortype, &MEDIATYPE_Audio, sizeof(GUID)) ||
	    memcmp(&mt->formattype, &FORMAT_WaveFormatEx, sizeof(GUID)) ||
	    mt->cbFormat < 16 || !mt->pbFormat)
		return E_INVALIDARG;
	cb = mt->cbFormat;
	extra = extra_size((const WAVEFORMATEX *)mt->pbFormat, cb);
	if (!(w = calloc(1, sizeof(*w) + extra + (cb < sizeof(*w) ?
						   sizeof(*w) - cb : 0))))
		return E_OUTOFMEMORY;
	memcpy(w, mt->pbFormat, cb < sizeof(*w) + extra ? cb :
	       sizeof(*w) + extra);
	w->cbSize = (WORD)extra;
	*wf = w;
	*size = sizeof(*w) + extra;
	return S_OK;
}

/* PCM samples as left-justified 32-bit integers, or as doubles. */
static int32_t get_int(enum audio_sample s, const BYTE *p)
{
	int16_t v16;
	int32_t v32;

	switch (s) {
	case AUDIO_U8:
		return (int32_t)((uint32_t)(p[0] ^ 0x80) << 24);
	case AUDIO_S16:
		memcpy(&v16, p, 2);
		return (int32_t)((uint32_t)(uint16_t)v16 << 16);
	case AUDIO_S24:
		return (int32_t)((uint32_t)p[0] << 8 | (uint32_t)p[1] << 16 |
				 (uint32_t)p[2] << 24);
	default:
		memcpy(&v32, p, 4);
		return v32;
	}
}

static double get_float(enum audio_sample s, const BYTE *p)
{
	float f;
	double d;

	if (s == AUDIO_F32) {
		memcpy(&f, p, 4);
		return f;
	}
	memcpy(&d, p, 8);
	return d;
}

static void put_int(enum audio_sample s, BYTE *p, int32_t v)
{
	uint32_t u = (uint32_t)v;
	int16_t v16;

	switch (s) {
	case AUDIO_U8:
		p[0] = (BYTE)((u >> 24) ^ 0x80);
		break;
	case AUDIO_S16:
		v16 = (int16_t)(u >> 16);
		memcpy(p, &v16, 2);
		break;
	case AUDIO_S24:
		p[0] = (BYTE)(u >> 8);
		p[1] = (BYTE)(u >> 16);
		p[2] = (BYTE)(u >> 24);
		break;
	default:
		memcpy(p, &v, 4);
	}
}

/* A float in [-1, 1) as an integer of `bits` bits, rounded and clipped. */
static int32_t float_to_int(double f, unsigned bits)
{
	double scale = ldexp(1.0, (int)bits - 1);
	double v = nearbyint(f * scale);

	if (!(v >= -scale))	/* NaN too */
		v = isnan(v) ? 0 : -scale;
	if (v > scale - 1)
		v = scale - 1;
	/* Left-justified, as get_int gives. */
	return (int32_t)((uint32_t)(int32_t)v << (32 - bits));
}

void audio_convert(enum audio_sample from, const void *in,
		   enum audio_sample to, void *out, size_t count)
{
	unsigned ib = audio_sample_bytes(from), ob = audio_sample_bytes(to);
	int ifloat = from == AUDIO_F32 || from == AUDIO_F64;
	int ofloat = to == AUDIO_F32 || to == AUDIO_F64;
	const BYTE *ip = in;
	BYTE *op = out;

	if (!ib || !ob)
		return;
	if (from == to) {
		if (out != in)
			memmove(out, in, count * ib);
		return;
	}
	for (size_t i = 0; i < count; i++, ip += ib, op += ob) {
		if (ofloat) {
			double d = ifloat ? get_float(from, ip) :
				   get_int(from, ip) / 2147483648.0;
			if (to == AUDIO_F32) {
				float f = (float)d;
				memcpy(op, &f, 4);
			} else {
				memcpy(op, &d, 8);
			}
		} else {
			int32_t v = ifloat ? float_to_int(get_float(from, ip),
							  ob * 8) :
				    get_int(from, ip);
			put_int(to, op, v);
		}
	}
}

size_t audio_converted_size(enum audio_sample from, enum audio_sample to,
			    size_t bytes)
{
	unsigned ib = audio_sample_bytes(from), ob = audio_sample_bytes(to);

	return ib ? bytes / ib * ob : 0;
}

int64_t audio_ticks(uint64_t value, DWORD scale, DWORD rate)
{
	if (!rate)
		return 0;
	/* value * scale * 10^7 / rate, a step at a time to stay in range. */
	uint64_t q = value / rate, r = value % rate;
	uint64_t t = r * scale;		/* < 2^64 */
	uint64_t a = t / rate, b = t % rate;

	return (int64_t)(q * scale * 10000000u + a * 10000000u +
			 b * 10000000u / rate);
}
