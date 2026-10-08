/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * testacm.acm: an ACM driver for TSTA, the fixtures' lossless audio codec
 * (tsta.h), so the harness's msacm32 and its ACM backend can be tested the
 * way a real codec would use them, and its packets checked against the
 * TSTA filter's and DMO's.
 *
 * It has two format tags, PCM and TSTA, each with 45 standard formats
 * (8000, 11025, 22050, 44100 and 48000 Hz; 1, 2 and 6 channels; 8, 16 and
 * 24 bits), and converts PCM to TSTA and back at the same rate, channels
 * and bits; it suggests one for the other. It keeps nothing between calls:
 *
 *   PCM -> TSTA  a packet for each 1024 sample frames given. With
 *                ACM_STREAMCONVERTF_BLOCKALIGN only whole packets are
 *                made, the rest left for the caller to give again; with
 *                ACM_STREAMCONVERTF_END, or without BLOCKALIGN, the rest
 *                becomes a short packet too.
 *   TSTA -> PCM  each whole packet given, as many as the output holds; a
 *                packet cut short is left for the caller.
 *
 * Settings in its (Windows) environment, read at DRV_LOAD, change how it
 * behaves, for the harness's sake:
 *
 *   TESTACM_STRICT       every pointer it is handed, and its own stack,
 *                        must be below 2 GB, or it fails
 *   TESTACM_NOTREALTIME  streams must be opened ACM_STREAMOPENF_NONREALTIME
 *   TESTACM_SMALL        ACMDM_STREAM_SIZE answers 1 byte for a source size
 *                        (less than any block), which callers mustn't trust
 *   TESTACM_PREPARE      it prepares headers itself (ACMDM_STREAM_PREPARE)
 *                        and won't convert with one it didn't prepare
 *   TESTACM_CONFIG       it says it can be configured (DRV_QUERYCONFIGURE)
 *                        and answers DRV_CONFIGURE, with no dialog
 *   TESTACM_CRASH        ACMDM_STREAM_CONVERT writes through a null pointer
 *   TESTACM_HANG         ACMDM_STREAM_CONVERT never returns
 *   TESTACM_WHOLE        it encodes only whole packets, even at the end
 *                        (as IMA ADPCM drivers do): the caller pads the rest
 *   TESTACM_NOENCODE     it suggests TSTA for PCM but won't open a stream
 *                        to make it (as Wine's MS ADPCM driver does)
 *
 * It allocates from the process heap as Wine's drivers do, taking it
 * from the PEB rather than calling GetProcessHeap.
 *
 * Built like old MSVC code, with a 4-byte stack boundary, and without a C
 * runtime.
 */
#include "fixture.h"
#include <mmreg.h>
#include <msacm.h>
#include "acmdrv.h"

#include "tsta.h"

#define NRATES 5
#define NCHANNELS 3
#define NBITS 3
#define NFORMATS (NRATES * NCHANNELS * NBITS)
#define INSTANCE_MAGIC 0x41435354	/* 'TSCA' */
#define STREAM_MAGIC 0x4d525453		/* 'STRM' */
#define PREPARED_MAGIC 0x50524550	/* 'PREP' */

static const DWORD rates[NRATES] = { 8000, 11025, 22050, 44100, 48000 };
static const WORD channel_counts[NCHANNELS] = { 1, 2, 6 };
static const WORD bit_depths[NBITS] = { 8, 16, 24 };

static int strict, notrealtime, small, prepare, config, crash, hang,
	   whole, noencode;
static LONG opens, streams;

struct instance {
	DWORD magic;
};

struct stream {
	DWORD magic;
	int encode;		/* PCM -> TSTA, else TSTA -> PCM */
	DWORD block;		/* of one PCM sample frame */
	WORD bits, channels;
	DWORD converts;
};

EXPORT LONG testacm_open_count(void)
{
	return opens;
}

EXPORT LONG testacm_stream_count(void)
{
	return streams;
}

static void zero(void *p, DWORD n)
{
	volatile BYTE *b = p;

	while (n--)
		*b++ = 0;
}

static void copy(void *dst, const void *src, DWORD n)
{
	BYTE *d = dst;
	const BYTE *s = src;

	while (n--)
		*d++ = *s++;
}

static void wcopy(WCHAR *dst, DWORD n, const char *src)
{
	DWORD i = 0;

	for (; src[i] && i + 1 < n; i++)
		dst[i] = (BYTE)src[i];
	dst[i] = 0;
}

/* NtCurrentTeb()->Peb->ProcessHeap, as Wine's GetProcessHeap is inline. */
static HANDLE process_heap(void)
{
	BYTE *peb;

	__asm__ volatile("movl %%fs:0x30, %0" : "=r"(peb));
	return *(HANDLE *)(peb + 0x18);
}

static int env_on(const char *name)
{
	char v[8];

	return GetEnvironmentVariableA(name, v, sizeof(v)) == 1 && v[0] == '1';
}

/* In strict mode, whether any of the n pointers is at or above 2 GB. */
static int high(int n, ...)
{
	__builtin_va_list ap;
	int i, bad;
	char here;

	if (!strict)
		return 0;
	bad = (DWORD)&here >= 0x80000000u;
	__builtin_va_start(ap, n);
	for (i = 0; i < n; i++)
		bad |= (DWORD)__builtin_va_arg(ap, const void *) >= 0x80000000u;
	__builtin_va_end(ap);
	return bad;
}

/* The bits a PCM format has, if it is PCM this driver takes. */
static int pcm_ok(const WAVEFORMATEX *wf)
{
	DWORD block = tsta_pcm_block(wf->wBitsPerSample, wf->nChannels);

	return wf->wFormatTag == WAVE_FORMAT_PCM && block &&
	       wf->nSamplesPerSec && wf->nBlockAlign == block &&
	       wf->nAvgBytesPerSec == wf->nSamplesPerSec * block;
}

static int tsta_ok(const WAVEFORMATEX *wf)
{
	DWORD block = tsta_pcm_block(wf->wBitsPerSample, wf->nChannels);
	const BYTE *extra = (const BYTE *)(wf + 1);

	/* Without its two bytes, a whole packet is taken as 1024 frames. */
	return wf->wFormatTag == WAVE_FORMAT_TSTA && block &&
	       wf->nSamplesPerSec &&
	       wf->nBlockAlign == TSTA_HEADER + TSTA_FRAMES * block &&
	       (wf->cbSize < 2 || (extra[0] | extra[1] << 8) == TSTA_FRAMES);
}

static void set_pcm(WAVEFORMATEX *wf, DWORD size, WORD channels, DWORD rate,
		    WORD bits)
{
	wf->wFormatTag = WAVE_FORMAT_PCM;
	wf->nChannels = channels;
	wf->nSamplesPerSec = rate;
	wf->wBitsPerSample = bits;
	wf->nBlockAlign = (WORD)tsta_pcm_block(bits, channels);
	wf->nAvgBytesPerSec = rate * wf->nBlockAlign;
	if (size >= sizeof(WAVEFORMATEX))
		wf->cbSize = 0;
}

static LRESULT driver_details(ACMDRIVERDETAILSW *out)
{
	ACMDRIVERDETAILSW d;
	DWORD n;

	if (!out || out->cbStruct < sizeof(DWORD) || high(1, out))
		return MMSYSERR_INVALPARAM;
	zero(&d, sizeof(d));
	d.fccType = ACMDRIVERDETAILS_FCCTYPE_AUDIOCODEC;
	d.fccComp = ACMDRIVERDETAILS_FCCCOMP_UNDEFINED;
	d.vdwACM = MAKE_ACM_VERSION(3, 50, 0);
	d.vdwDriver = MAKE_ACM_VERSION(1, 0, 0);
	d.fdwSupport = ACMDRIVERDETAILS_SUPPORTF_CODEC;
	d.cFormatTags = 2;
	wcopy(d.szShortName, ACMDRIVERDETAILS_SHORTNAME_CHARS, "TSTA");
	wcopy(d.szLongName, ACMDRIVERDETAILS_LONGNAME_CHARS,
	      "seance TSTA test codec");
	wcopy(d.szCopyright, ACMDRIVERDETAILS_COPYRIGHT_CHARS,
	      "GPL-2.0-only; part of seance's tests");
	wcopy(d.szFeatures, ACMDRIVERDETAILS_FEATURES_CHARS,
	      "Lossless TSTA packets of 1024 sample frames, to and from PCM.");
	n = out->cbStruct < sizeof(d) ? out->cbStruct : sizeof(d);
	d.cbStruct = n;
	copy(out, &d, n);
	return MMSYSERR_NOERROR;
}

static LRESULT tag_details(ACMFORMATTAGDETAILSW *t, DWORD flags)
{
	DWORD i;

	if (high(1, t) || t->cbStruct < sizeof(*t))
		return MMSYSERR_INVALPARAM;
	switch (flags & ACM_FORMATTAGDETAILSF_QUERYMASK) {
	case ACM_FORMATTAGDETAILSF_INDEX:
		i = t->dwFormatTagIndex;
		break;
	case ACM_FORMATTAGDETAILSF_FORMATTAG:
		i = t->dwFormatTag == WAVE_FORMAT_PCM ? 0 :
		    t->dwFormatTag == WAVE_FORMAT_TSTA ? 1 : 2;
		break;
	case ACM_FORMATTAGDETAILSF_LARGESTSIZE:
		i = t->dwFormatTag == WAVE_FORMAT_PCM ? 0 :
		    t->dwFormatTag == WAVE_FORMAT_TSTA ||
		    t->dwFormatTag == WAVE_FORMAT_UNKNOWN ? 1 : 2;
		break;
	default:
		return MMSYSERR_INVALFLAG;
	}
	if (i > 1)
		return ACMERR_NOTPOSSIBLE;
	t->dwFormatTagIndex = i;
	t->dwFormatTag = i ? WAVE_FORMAT_TSTA : WAVE_FORMAT_PCM;
	t->cbFormatSize = i ? sizeof(WAVEFORMATEX) + 2 : sizeof(PCMWAVEFORMAT);
	t->fdwSupport = ACMDRIVERDETAILS_SUPPORTF_CODEC;
	t->cStandardFormats = NFORMATS;
	wcopy(t->szFormatTag, ACMFORMATTAGDETAILS_FORMATTAG_CHARS,
	      i ? "TSTA" : "PCM");
	return MMSYSERR_NOERROR;
}

static LRESULT format_details(ACMFORMATDETAILSW *f, DWORD flags)
{
	if (high(2, f, f->pwfx) || f->cbStruct < sizeof(*f))
		return MMSYSERR_INVALPARAM;
	if (f->dwFormatTag != WAVE_FORMAT_PCM &&
	    f->dwFormatTag != WAVE_FORMAT_TSTA)
		return ACMERR_NOTPOSSIBLE;
	DWORD need = f->dwFormatTag == WAVE_FORMAT_PCM ? sizeof(PCMWAVEFORMAT) :
		     sizeof(WAVEFORMATEX) + 2;
	if (f->cbwfx < need)
		return MMSYSERR_INVALPARAM;
	switch (flags & ACM_FORMATDETAILSF_QUERYMASK) {
	case ACM_FORMATDETAILSF_INDEX: {
		DWORD i = f->dwFormatIndex;
		if (i >= NFORMATS)
			return ACMERR_NOTPOSSIBLE;
		DWORD rate = rates[i / (NCHANNELS * NBITS)];
		WORD ch = channel_counts[i / NBITS % NCHANNELS];
		WORD bits = bit_depths[i % NBITS];
		if (f->dwFormatTag == WAVE_FORMAT_PCM)
			set_pcm(f->pwfx, f->cbwfx, ch, rate, bits);
		else
			tsta_format(f->pwfx, bits, ch, rate, 0);
		break;
	}
	case ACM_FORMATDETAILSF_FORMAT:
		if (f->pwfx->wFormatTag != f->dwFormatTag ||
		    (f->dwFormatTag == WAVE_FORMAT_PCM ? !pcm_ok(f->pwfx) :
		     !tsta_ok(f->pwfx)))
			return ACMERR_NOTPOSSIBLE;
		break;
	default:
		return MMSYSERR_INVALFLAG;
	}
	f->fdwSupport = ACMDRIVERDETAILS_SUPPORTF_CODEC;
	f->szFormat[0] = 0;
	return MMSYSERR_NOERROR;
}

/* PCM for TSTA and TSTA for PCM, with what the caller fixes the same. */
static LRESULT suggest(ACMDRVFORMATSUGGEST *s)
{
	const WAVEFORMATEX *src = s->pwfxSrc;
	WAVEFORMATEX *dst = s->pwfxDst;
	DWORD f = s->fdwSuggest;
	int to_tsta;

	if (high(3, s, src, dst))
		return MMSYSERR_INVALPARAM;
	if (pcm_ok(src))
		to_tsta = 1;
	else if (tsta_ok(src))
		to_tsta = 0;
	else
		return ACMERR_NOTPOSSIBLE;
	if (((f & ACM_FORMATSUGGESTF_WFORMATTAG) &&
	     dst->wFormatTag != (to_tsta ? WAVE_FORMAT_TSTA : WAVE_FORMAT_PCM)) ||
	    ((f & ACM_FORMATSUGGESTF_NCHANNELS) &&
	     dst->nChannels != src->nChannels) ||
	    ((f & ACM_FORMATSUGGESTF_NSAMPLESPERSEC) &&
	     dst->nSamplesPerSec != src->nSamplesPerSec) ||
	    ((f & ACM_FORMATSUGGESTF_WBITSPERSAMPLE) &&
	     dst->wBitsPerSample != src->wBitsPerSample))
		return ACMERR_NOTPOSSIBLE;
	if (s->cbwfxDst < (to_tsta ? sizeof(WAVEFORMATEX) + 2 :
			   sizeof(PCMWAVEFORMAT)))
		return ACMERR_NOTPOSSIBLE;
	if (to_tsta)
		tsta_format(dst, src->wBitsPerSample, src->nChannels,
			    src->nSamplesPerSec, 0);
	else
		set_pcm(dst, s->cbwfxDst, src->nChannels, src->nSamplesPerSec,
			src->wBitsPerSample);
	return MMSYSERR_NOERROR;
}

static LRESULT stream_open(ACMDRVSTREAMINSTANCE *si)
{
	const WAVEFORMATEX *src, *dst;
	struct stream *s;
	int encode;

	if (high(3, si, si->pwfxSrc, si->pwfxDst))
		return MMSYSERR_INVALPARAM;
	src = si->pwfxSrc;
	dst = si->pwfxDst;
	if (si->pwfltr)
		return ACMERR_NOTPOSSIBLE;
	if (pcm_ok(src) && tsta_ok(dst))
		encode = 1;
	else if (tsta_ok(src) && pcm_ok(dst))
		encode = 0;
	else
		return ACMERR_NOTPOSSIBLE;
	if (src->nChannels != dst->nChannels ||
	    src->nSamplesPerSec != dst->nSamplesPerSec ||
	    src->wBitsPerSample != dst->wBitsPerSample)
		return ACMERR_NOTPOSSIBLE;
	if (notrealtime && !(si->fdwOpen & ACM_STREAMOPENF_NONREALTIME))
		return ACMERR_NOTPOSSIBLE;
	if (si->fdwOpen & ACM_STREAMOPENF_ASYNC)
		return ACMERR_NOTPOSSIBLE;
	if (encode && noencode)
		return MMSYSERR_NOTSUPPORTED;
	if (si->fdwOpen & ACM_STREAMOPENF_QUERY)
		return MMSYSERR_NOERROR;	/* nothing allocated */
	s = HeapAlloc(process_heap(), HEAP_ZERO_MEMORY, sizeof(*s));
	if (!s)
		return MMSYSERR_NOMEM;
	s->magic = STREAM_MAGIC;
	s->encode = encode;
	s->bits = src->wBitsPerSample;
	s->channels = src->nChannels;
	s->block = tsta_pcm_block(s->bits, s->channels);
	si->dwDriver = (DWORD_PTR)s;
	InterlockedIncrement(&streams);
	return MMSYSERR_NOERROR;
}

static struct stream *stream_of(ACMDRVSTREAMINSTANCE *si)
{
	struct stream *s = si ? (struct stream *)si->dwDriver : NULL;

	return s && s->magic == STREAM_MAGIC ? s : NULL;
}

static LRESULT stream_close(ACMDRVSTREAMINSTANCE *si)
{
	struct stream *s = stream_of(si);

	if (!s)
		return MMSYSERR_INVALHANDLE;
	s->magic = 0;
	HeapFree(process_heap(), 0, s);
	si->dwDriver = 0;
	InterlockedDecrement(&streams);
	return MMSYSERR_NOERROR;
}

static LRESULT stream_size(ACMDRVSTREAMINSTANCE *si, ACMDRVSTREAMSIZE *z)
{
	struct stream *s = stream_of(si);
	DWORD packet, pcm_packet;

	if (!s || high(2, si, z))
		return MMSYSERR_INVALPARAM;
	packet = TSTA_HEADER + TSTA_FRAMES * s->block;
	pcm_packet = TSTA_FRAMES * s->block;
	switch (z->fdwSize & ACM_STREAMSIZEF_QUERYMASK) {
	case ACM_STREAMSIZEF_SOURCE:
		if (small) {
			z->cbDstLength = 1;
			return MMSYSERR_NOERROR;
		}
		if (s->encode) {
			/* Every frame, and a header for each packet begun. */
			DWORD frames = z->cbSrcLength / s->block;
			if (!frames)
				return ACMERR_NOTPOSSIBLE;
			z->cbDstLength = frames * s->block + TSTA_HEADER *
					 ((frames + TSTA_FRAMES - 1) / TSTA_FRAMES);
		} else {
			/* At most a whole packet's frames per packet begun. */
			if (z->cbSrcLength < TSTA_HEADER + s->block)
				return ACMERR_NOTPOSSIBLE;
			z->cbDstLength = (z->cbSrcLength + packet - 1) / packet *
					 pcm_packet;
		}
		return MMSYSERR_NOERROR;
	case ACM_STREAMSIZEF_DESTINATION:
		/* What, in whole packets, makes no more than that. */
		if (s->encode) {
			DWORD n = z->cbDstLength / packet;
			if (!n)
				return ACMERR_NOTPOSSIBLE;
			z->cbSrcLength = n * pcm_packet;
		} else {
			DWORD n = z->cbDstLength / pcm_packet;
			if (!n)
				return ACMERR_NOTPOSSIBLE;
			z->cbSrcLength = n * packet;
		}
		return MMSYSERR_NOERROR;
	default:
		return MMSYSERR_INVALFLAG;
	}
}

static LRESULT encode(struct stream *s, ACMDRVSTREAMHEADER *h)
{
	DWORD frames = h->cbSrcLength / s->block, used = 0, made = 0;
	int all = !whole && ((h->fdwConvert & ACM_STREAMCONVERTF_END) ||
			     !(h->fdwConvert & ACM_STREAMCONVERTF_BLOCKALIGN));

	while (frames) {
		DWORD n = frames < TSTA_FRAMES ? frames : TSTA_FRAMES;
		if (n < TSTA_FRAMES && !all)
			break;
		if (h->cbDstLength - made < TSTA_HEADER + n * s->block)
			break;
		made += tsta_encode(h->pbSrc + used * s->block, n, s->bits,
				    s->channels, h->pbDst + made);
		used += n;
		frames -= n;
	}
	h->cbSrcLengthUsed = used * s->block;
	h->cbDstLengthUsed = made;
	return MMSYSERR_NOERROR;
}

static LRESULT decode(struct stream *s, ACMDRVSTREAMHEADER *h)
{
	DWORD used = 0, made = 0;

	while (h->cbSrcLength - used >= TSTA_HEADER) {
		const BYTE *p = h->pbSrc + used;
		DWORD frames = p[4] | (DWORD)p[5] << 8 | (DWORD)p[6] << 16 |
			       (DWORD)p[7] << 24;
		DWORD len;
		LONG got;
		if (frames > TSTA_FRAMES)
			return ACMERR_NOTPOSSIBLE;	/* not a packet */
		len = TSTA_HEADER + frames * s->block;
		if (h->cbSrcLength - used < len ||
		    h->cbDstLength - made < frames * s->block)
			break;	/* the rest of it, or room, next time */
		got = tsta_decode(p, len, s->bits, s->channels,
				  h->pbDst + made, frames);
		if (got < 0)
			return ACMERR_NOTPOSSIBLE;
		used += len;
		made += (DWORD)got * s->block;
	}
	h->cbSrcLengthUsed = used;
	h->cbDstLengthUsed = made;
	return MMSYSERR_NOERROR;
}

static LRESULT stream_convert(ACMDRVSTREAMINSTANCE *si, ACMDRVSTREAMHEADER *h)
{
	struct stream *s = stream_of(si);

	if (crash)
		*(volatile DWORD *)(DWORD_PTR)si->pwfltr = 1;	/* it is NULL */
	while (hang)
		Sleep(1000);
	if (!s || !h || high(4, si, h, h->pbSrc, h->pbDst))
		return MMSYSERR_INVALPARAM;
	if (prepare && h->dwDriver != PREPARED_MAGIC)
		return ACMERR_UNPREPARED;
	s->converts++;
	return s->encode ? encode(s, h) : decode(s, h);
}

static LRESULT stream_prepare(ACMDRVSTREAMHEADER *h, int on)
{
	if (!prepare)
		return MMSYSERR_NOTSUPPORTED;	/* msacm32 does it */
	if (high(1, h))
		return MMSYSERR_INVALPARAM;
	if (!on && h->dwDriver != PREPARED_MAGIC)
		return ACMERR_UNPREPARED;
	h->dwDriver = on ? PREPARED_MAGIC : 0;
	return MMSYSERR_NOERROR;
}

LRESULT WINAPI DriverProc(DWORD_PTR id, HDRVR hdrvr, UINT msg, LPARAM l1,
			  LPARAM l2)
{
	struct instance *inst = (struct instance *)id;

	switch (msg) {
	case DRV_LOAD:
		strict = env_on("TESTACM_STRICT");
		notrealtime = env_on("TESTACM_NOTREALTIME");
		small = env_on("TESTACM_SMALL");
		prepare = env_on("TESTACM_PREPARE");
		config = env_on("TESTACM_CONFIG");
		crash = env_on("TESTACM_CRASH");
		hang = env_on("TESTACM_HANG");
		whole = env_on("TESTACM_WHOLE");
		noencode = env_on("TESTACM_NOENCODE");
		return 1;
	case DRV_FREE:
	case DRV_ENABLE:
	case DRV_DISABLE:
		return 1;
	case DRV_OPEN: {
		ACMDRVOPENDESCW *d = (ACMDRVOPENDESCW *)l2;
		if (high(1, d))
			return 0;
		/* Opened as anything but an audio codec, it declines. */
		if (d && d->fccType != ACMDRIVERDETAILS_FCCTYPE_AUDIOCODEC)
			return 0;
		inst = HeapAlloc(process_heap(), HEAP_ZERO_MEMORY,
				 sizeof(*inst));
		if (!inst) {
			if (d)
				d->dwError = MMSYSERR_NOMEM;
			return 0;
		}
		inst->magic = INSTANCE_MAGIC;
		if (d)
			d->dwError = MMSYSERR_NOERROR;
		InterlockedIncrement(&opens);
		return (LRESULT)inst;
	}
	case DRV_CLOSE:
		if (inst && inst->magic == INSTANCE_MAGIC) {
			inst->magic = 0;
			HeapFree(process_heap(), 0, inst);
			InterlockedDecrement(&opens);
		}
		return 1;
	case DRV_QUERYCONFIGURE:
		return config;
	case DRV_CONFIGURE:
		return config ? DRVCNF_OK : DRVCNF_CANCEL;
	}

	if (msg < DRV_USER || !inst || inst->magic != INSTANCE_MAGIC)
		return DefDriverProc(id, hdrvr, msg, l1, l2);

	switch (msg) {
	case ACMDM_DRIVER_DETAILS:
		return driver_details((ACMDRIVERDETAILSW *)l1);
	case ACMDM_DRIVER_ABOUT:
		return MMSYSERR_NOTSUPPORTED;	/* no about box */
	case ACMDM_FORMATTAG_DETAILS:
		return tag_details((ACMFORMATTAGDETAILSW *)l1, (DWORD)l2);
	case ACMDM_FORMAT_DETAILS:
		return format_details((ACMFORMATDETAILSW *)l1, (DWORD)l2);
	case ACMDM_FORMAT_SUGGEST:
		return suggest((ACMDRVFORMATSUGGEST *)l1);
	case ACMDM_STREAM_OPEN:
		return stream_open((ACMDRVSTREAMINSTANCE *)l1);
	case ACMDM_STREAM_CLOSE:
		return stream_close((ACMDRVSTREAMINSTANCE *)l1);
	case ACMDM_STREAM_SIZE:
		return stream_size((ACMDRVSTREAMINSTANCE *)l1,
				   (ACMDRVSTREAMSIZE *)l2);
	case ACMDM_STREAM_CONVERT:
		return stream_convert((ACMDRVSTREAMINSTANCE *)l1,
				      (ACMDRVSTREAMHEADER *)l2);
	case ACMDM_STREAM_PREPARE:
		return stream_prepare((ACMDRVSTREAMHEADER *)l2, 1);
	case ACMDM_STREAM_UNPREPARE:
		return stream_prepare((ACMDRVSTREAMHEADER *)l2, 0);
	case ACMDM_STREAM_RESET:
		return MMSYSERR_NOERROR;
	default:
		return MMSYSERR_NOTSUPPORTED;
	}
}

BOOL WINAPI DllMain(HINSTANCE dll, DWORD reason, LPVOID reserved)
{
	(void)dll;
	(void)reason;
	(void)reserved;
	return TRUE;
}
