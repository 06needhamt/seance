/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * msacm32.dll's PCM converter, "MS-PCM": a driver procedure in the driver
 * list like any other. It converts PCM between 8, 16, 24 and 32 bits per
 * sample (through audio_convert, as FFmpeg converts) and between mono and
 * stereo (the two channels averaged, or the one doubled), at one rate: it
 * refuses to resample. It lists the standard formats Wine's converter
 * lists: 8000 to 96000 Hz, mono and stereo, 8, 16 and 24 bits.
 */
#include <stdlib.h>
#include <string.h>

#include "media/audio.h"
#include "win32/internal.h"
#include "win32/msacm32.h"

static const DWORD rates[] = { 8000, 11025, 22050, 44100, 48000, 96000 };
static const struct { WORD channels, bits; } kinds[] = {
	{ 1, 8 }, { 2, 8 }, { 1, 16 }, { 2, 16 }, { 1, 24 }, { 2, 24 },
};
#define NKINDS (sizeof(kinds) / sizeof(*kinds))
#define NFORMATS (sizeof(rates) / sizeof(*rates) * NKINDS)

static enum audio_sample sample_of(WORD bits)
{
	switch (bits) {
	case 8: return AUDIO_U8;
	case 16: return AUDIO_S16;
	case 24: return AUDIO_S24;
	case 32: return AUDIO_S32;
	default: return AUDIO_NONE;
	}
}

/* Plain PCM of a size it converts, consistent with itself. */
static int pcm_ok(const WAVEFORMATEX *wf)
{
	return wf && wf->wFormatTag == WAVE_FORMAT_PCM && wf->nChannels &&
	       wf->nSamplesPerSec && sample_of(wf->wBitsPerSample) != AUDIO_NONE &&
	       wf->nBlockAlign == wf->nChannels * wf->wBitsPerSample / 8 &&
	       wf->nAvgBytesPerSec == wf->nSamplesPerSec * wf->nBlockAlign;
}

static void set_pcm(WAVEFORMATEX *wf, DWORD size, WORD channels, DWORD rate,
		    WORD bits)
{
	wf->wFormatTag = WAVE_FORMAT_PCM;
	wf->nChannels = channels;
	wf->nSamplesPerSec = rate;
	wf->wBitsPerSample = bits;
	wf->nBlockAlign = (WORD)(channels * bits / 8);
	wf->nAvgBytesPerSec = rate * wf->nBlockAlign;
	if (size >= sizeof(WAVEFORMATEX))
		wf->cbSize = 0;
}

/* Whether it converts src to dst: the same rate, and channels it maps. */
static int convertible(const WAVEFORMATEX *src, const WAVEFORMATEX *dst)
{
	return pcm_ok(src) && pcm_ok(dst) &&
	       src->nSamplesPerSec == dst->nSamplesPerSec &&
	       (src->nChannels == dst->nChannels ||
		(src->nChannels <= 2 && dst->nChannels <= 2));
}

static void wcopy_ascii(WCHAR *dst, size_t n, const char *src)
{
	size_t i = 0;

	for (; src[i] && i + 1 < n; i++)
		dst[i] = (BYTE)src[i];
	dst[i] = 0;
}

static LRESULT details(ACMDRIVERDETAILSW *out)
{
	ACMDRIVERDETAILSW d;

	if (!out || out->cbStruct < sizeof(DWORD))
		return MMSYSERR_INVALPARAM;
	memset(&d, 0, sizeof(d));
	d.fccType = ACMDRIVERDETAILS_FCCTYPE_AUDIOCODEC;
	d.fccComp = ACMDRIVERDETAILS_FCCCOMP_UNDEFINED;
	d.vdwACM = ACM_VERSION;
	d.vdwDriver = MAKE_ACM_VERSION(1, 0, 0);
	d.fdwSupport = ACMDRIVERDETAILS_SUPPORTF_CONVERTER;
	d.cFormatTags = 1;
	wcopy_ascii(d.szShortName, ACMDRIVERDETAILS_SHORTNAME_CHARS, "MS-PCM");
	wcopy_ascii(d.szLongName, ACMDRIVERDETAILS_LONGNAME_CHARS,
		    "seance PCM converter");
	wcopy_ascii(d.szFeatures, ACMDRIVERDETAILS_FEATURES_CHARS,
		    "Converts PCM between 8, 16, 24 and 32 bits, and between "
		    "mono and stereo, at one rate.");
	DWORD n = out->cbStruct < sizeof(d) ? out->cbStruct : sizeof(d);
	d.cbStruct = n;
	memcpy(out, &d, n);
	return MMSYSERR_NOERROR;
}

static LRESULT tag_details(ACMFORMATTAGDETAILSW *t, DWORD flags)
{
	switch (flags & ACM_FORMATTAGDETAILSF_QUERYMASK) {
	case ACM_FORMATTAGDETAILSF_INDEX:
		if (t->dwFormatTagIndex != 0)
			return ACMERR_NOTPOSSIBLE;
		break;
	case ACM_FORMATTAGDETAILSF_FORMATTAG:
		if (t->dwFormatTag != WAVE_FORMAT_PCM)
			return ACMERR_NOTPOSSIBLE;
		break;
	case ACM_FORMATTAGDETAILSF_LARGESTSIZE:
		if (t->dwFormatTag != WAVE_FORMAT_PCM &&
		    t->dwFormatTag != WAVE_FORMAT_UNKNOWN)
			return ACMERR_NOTPOSSIBLE;
		break;
	default:
		return MMSYSERR_INVALFLAG;
	}
	t->dwFormatTagIndex = 0;
	t->dwFormatTag = WAVE_FORMAT_PCM;
	t->cbFormatSize = sizeof(PCMWAVEFORMAT);
	t->fdwSupport = ACMDRIVERDETAILS_SUPPORTF_CONVERTER;
	t->cStandardFormats = NFORMATS;
	wcopy_ascii(t->szFormatTag, ACMFORMATTAGDETAILS_FORMATTAG_CHARS, "PCM");
	return MMSYSERR_NOERROR;
}

static LRESULT format_details(ACMFORMATDETAILSW *f, DWORD flags)
{
	if (f->dwFormatTag != WAVE_FORMAT_PCM)
		return ACMERR_NOTPOSSIBLE;
	if (f->cbwfx < sizeof(PCMWAVEFORMAT))
		return MMSYSERR_INVALPARAM;
	switch (flags & ACM_FORMATDETAILSF_QUERYMASK) {
	case ACM_FORMATDETAILSF_INDEX: {
		if (f->dwFormatIndex >= NFORMATS)
			return ACMERR_NOTPOSSIBLE;
		DWORD rate = rates[f->dwFormatIndex / NKINDS];
		unsigned k = f->dwFormatIndex % NKINDS;
		set_pcm(f->pwfx, f->cbwfx, kinds[k].channels, rate,
			kinds[k].bits);
		break;
	}
	case ACM_FORMATDETAILSF_FORMAT:
		if (!pcm_ok(f->pwfx))
			return ACMERR_NOTPOSSIBLE;
		break;
	default:
		return MMSYSERR_INVALFLAG;
	}
	f->fdwSupport = ACMDRIVERDETAILS_SUPPORTF_CONVERTER;
	f->szFormat[0] = 0;	/* msacm32 words it */
	return MMSYSERR_NOERROR;
}

static LRESULT suggest(ACMDRVFORMATSUGGEST *s)
{
	const WAVEFORMATEX *src = s->pwfxSrc;
	WAVEFORMATEX *dst = s->pwfxDst;
	DWORD f = s->fdwSuggest;

	if (!pcm_ok(src) || s->cbwfxDst < sizeof(PCMWAVEFORMAT))
		return ACMERR_NOTPOSSIBLE;
	if ((f & ACM_FORMATSUGGESTF_WFORMATTAG) &&
	    dst->wFormatTag != WAVE_FORMAT_PCM)
		return ACMERR_NOTPOSSIBLE;
	WORD channels = f & ACM_FORMATSUGGESTF_NCHANNELS ? dst->nChannels :
			src->nChannels;
	DWORD rate = f & ACM_FORMATSUGGESTF_NSAMPLESPERSEC ?
		     dst->nSamplesPerSec : src->nSamplesPerSec;
	WORD bits = f & ACM_FORMATSUGGESTF_WBITSPERSAMPLE ? dst->wBitsPerSample :
		    src->wBitsPerSample;
	WAVEFORMATEX want;
	set_pcm(&want, sizeof(want), channels, rate, bits);
	if (!convertible(src, &want))
		return ACMERR_NOTPOSSIBLE;
	set_pcm(dst, s->cbwfxDst, channels, rate, bits);
	return MMSYSERR_NOERROR;
}

static LRESULT stream_open(ACMDRVSTREAMINSTANCE *si)
{
	if (si->pwfltr || !convertible(si->pwfxSrc, si->pwfxDst))
		return ACMERR_NOTPOSSIBLE;
	return MMSYSERR_NOERROR;
}

static LRESULT stream_size(ACMDRVSTREAMINSTANCE *si, ACMDRVSTREAMSIZE *s)
{
	DWORD sblock = si->pwfxSrc->nBlockAlign, dblock = si->pwfxDst->nBlockAlign;

	switch (s->fdwSize & ACM_STREAMSIZEF_QUERYMASK) {
	case ACM_STREAMSIZEF_SOURCE:
		if (s->cbSrcLength < sblock)
			return ACMERR_NOTPOSSIBLE;
		s->cbDstLength = s->cbSrcLength / sblock * dblock;
		return MMSYSERR_NOERROR;
	case ACM_STREAMSIZEF_DESTINATION:
		if (s->cbDstLength < dblock)
			return ACMERR_NOTPOSSIBLE;
		s->cbSrcLength = s->cbDstLength / dblock * sblock;
		return MMSYSERR_NOERROR;
	default:
		return MMSYSERR_INVALFLAG;
	}
}

static LRESULT convert(ACMDRVSTREAMINSTANCE *si, ACMDRVSTREAMHEADER *h)
{
	const WAVEFORMATEX *src = si->pwfxSrc, *dst = si->pwfxDst;
	enum audio_sample from = sample_of(src->wBitsPerSample);
	enum audio_sample to = sample_of(dst->wBitsPerSample);
	size_t frames = h->cbSrcLength / src->nBlockAlign;
	size_t room = h->cbDstLength / dst->nBlockAlign;

	if (room < frames)
		frames = room;
	if (src->nChannels == dst->nChannels) {
		audio_convert(from, h->pbSrc, to, h->pbDst,
			      frames * src->nChannels);
	} else if (frames) {
		/* Through float: one channel doubled, or two averaged. */
		float *f = malloc(frames * 2 * sizeof(float));
		if (!f)
			return MMSYSERR_NOMEM;
		audio_convert(from, h->pbSrc, AUDIO_F32, f,
			      frames * src->nChannels);
		if (src->nChannels == 1) {
			for (size_t i = frames; i-- > 0;)
				f[2 * i] = f[2 * i + 1] = f[i];
		} else {
			for (size_t i = 0; i < frames; i++)
				f[i] = (f[2 * i] + f[2 * i + 1]) / 2;
		}
		audio_convert(AUDIO_F32, f, to, h->pbDst,
			      frames * dst->nChannels);
		free(f);
	}
	h->cbSrcLengthUsed = (DWORD)(frames * src->nBlockAlign);
	h->cbDstLengthUsed = (DWORD)(frames * dst->nBlockAlign);
	return MMSYSERR_NOERROR;
}

LRESULT STUB_WINAPI msacm_pcm_proc(DWORD_PTR id, HDRVR h, UINT msg,
				   LPARAM l1, LPARAM l2)
{
	(void)id;
	(void)h;
	switch (msg) {
	case DRV_LOAD:
	case DRV_ENABLE:
	case DRV_DISABLE:
	case DRV_FREE:
	case DRV_CLOSE:
		return 1;
	case DRV_OPEN: {
		const ACMDRVOPENDESCW *d = (const ACMDRVOPENDESCW *)l2;
		return !d || d->fccType == ACMDRIVERDETAILS_FCCTYPE_AUDIOCODEC;
	}
	case DRV_QUERYCONFIGURE:
		return 0;
	case ACMDM_DRIVER_DETAILS:
		return details((ACMDRIVERDETAILSW *)l1);
	case ACMDM_FORMATTAG_DETAILS:
		return tag_details((ACMFORMATTAGDETAILSW *)l1, (DWORD)l2);
	case ACMDM_FORMAT_DETAILS:
		return format_details((ACMFORMATDETAILSW *)l1, (DWORD)l2);
	case ACMDM_FORMAT_SUGGEST:
		return suggest((ACMDRVFORMATSUGGEST *)l1);
	case ACMDM_STREAM_OPEN:
		return stream_open((ACMDRVSTREAMINSTANCE *)l1);
	case ACMDM_STREAM_CLOSE:
		return MMSYSERR_NOERROR;
	case ACMDM_STREAM_SIZE:
		return stream_size((ACMDRVSTREAMINSTANCE *)l1,
				   (ACMDRVSTREAMSIZE *)l2);
	case ACMDM_STREAM_CONVERT:
		return convert((ACMDRVSTREAMINSTANCE *)l1,
			       (ACMDRVSTREAMHEADER *)l2);
	default:
		return msg < DRV_USER ? 0 : MMSYSERR_NOTSUPPORTED;
	}
}
