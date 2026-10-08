/* SPDX-License-Identifier: GPL-2.0-only */
/* ACM drivers as transforms (see acm.h). */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "acm/acm.h"
#include "common/error.h"
#include "common/log.h"
#include "common/unicode.h"
#include "media/audio.h"
#include "win32/msacm32.h"

#define MIN_OUTPUT (64 * 1024)
#define MAX_OUTPUT (64u << 20)
#define MAX_ROUNDS 4096		/* flushing calls that take no input */
#define SMALL_BLOCKS 1024

struct acm_codec {
	HACMDRIVERID id;
	HACMDRIVER had;
	size_t max_format;	/* the driver's largest format */
};

struct acm_stream {
	struct acm_codec *c;
	HACMSTREAM has;
	WAVEFORMATEX *out;
	size_t out_size;
	DWORD in_block, out_block;
	int pcm_out;		/* decoding: timed, not cut into blocks */
	int pcm_in;		/* encoding: AUDIO_ sample format of the input */
	DWORD rate;
	acm_output_fn callback;
	void *opaque;
	BYTE *pending;		/* input kept for the next conversion */
	size_t held, pending_cap;
	BYTE *dst;
	size_t dst_cap;
	int started, based;
	REFERENCE_TIME base;
	uint64_t frames_out;
};

static HRESULT hr_of(MMRESULT r)
{
	return r ? ACM_HRESULT(r) : S_OK;
}

/* Where to put a format of the driver's: its largest, or a WAVEFORMATEX. */
static size_t format_room(HACMDRIVER had)
{
	DWORD n = 0;

	msacm_acmMetrics((HACMOBJ)had, ACM_METRIC_MAX_SIZE_FORMAT, &n);
	return n < sizeof(WAVEFORMATEX) + 64 ? sizeof(WAVEFORMATEX) + 64 : n;
}

/*
 * Whether it would open a stream between the two: a suggestion is no
 * promise (Wine's MS ADPCM driver suggests what it can't encode).
 */
static int streams(HACMDRIVER had, WAVEFORMATEX *src, WAVEFORMATEX *dst)
{
	return !msacm_acmStreamOpen(NULL, had, src, dst, NULL, 0, 0,
				    ACM_STREAMOPENF_QUERY |
				    ACM_STREAMOPENF_NONREALTIME);
}

/*
 * Whether one of the tag's first formats becomes PCM. A tag with none to
 * try (Wine's MP3 decoder lists none) is taken at its word: opening a
 * stream from the real format will say.
 */
static int decodes(HACMDRIVER had, DWORD tag, DWORD formats)
{
	size_t room = format_room(had);
	WAVEFORMATEX *wf, *pcm;
	int yes = 0;

	if (!formats)
		return 1;
	wf = calloc(1, room);
	pcm = calloc(1, room);

	for (DWORD i = 0; wf && pcm && !yes && i < formats && i < 16; i++) {
		ACMFORMATDETAILSW d = { .cbStruct = sizeof(d) };
		memset(wf, 0, room);
		d.dwFormatIndex = i;
		d.dwFormatTag = tag;
		d.pwfx = wf;
		d.cbwfx = (DWORD)room;
		if (msacm_acmFormatDetailsW(had, &d, ACM_FORMATDETAILSF_INDEX))
			continue;
		memset(pcm, 0, room);
		pcm->wFormatTag = WAVE_FORMAT_PCM;
		yes = !msacm_acmFormatSuggest(had, wf, pcm, (DWORD)room,
					      ACM_FORMATSUGGESTF_WFORMATTAG) &&
		      streams(had, wf, pcm);
	}
	free(wf);
	free(pcm);
	return yes;
}

/* Whether common PCM becomes the tag. */
static int encodes(HACMDRIVER had, DWORD tag)
{
	static const struct { WORD channels, bits; DWORD rate; } kinds[] = {
		{ 2, 16, 44100 }, { 2, 16, 48000 }, { 1, 16, 44100 },
		{ 1, 16, 22050 }, { 1, 16, 8000 }, { 1, 8, 8000 },
		{ 2, 8, 11025 },
	};
	size_t room = format_room(had);
	WAVEFORMATEX *dst = calloc(1, room);
	int yes = 0;

	for (size_t i = 0; dst && !yes && i < sizeof(kinds) / sizeof(*kinds);
	     i++) {
		WAVEFORMATEXTENSIBLE pcm;
		audio_pcm_format(kinds[i].bits == 8 ? AUDIO_U8 : AUDIO_S16,
				 kinds[i].channels, kinds[i].rate, 0,
				 AUDIO_FORM_PLAIN, &pcm);
		memset(dst, 0, room);
		dst->wFormatTag = (WORD)tag;
		yes = !msacm_acmFormatSuggest(had, &pcm.Format, dst, (DWORD)room,
					      ACM_FORMATSUGGESTF_WFORMATTAG) &&
		      streams(had, &pcm.Format, dst);
	}
	free(dst);
	return yes;
}

HRESULT acm_find(const char *path, int encoder, WORD want, WORD *tag)
{
	HACMDRIVERID id;
	HACMDRIVER had;
	ACMDRIVERDETAILSW dd = { .cbStruct = sizeof(dd) };
	int matches = 0;

	*tag = 0;
	if (msacm_add_path(path, &id) ||
	    msacm_acmDriverDetailsW(id, &dd, 0) ||
	    msacm_acmDriverOpen(&had, id, 0))
		return CLASS_E_CLASSNOTAVAILABLE;
	for (DWORD i = 0; i < dd.cFormatTags; i++) {
		ACMFORMATTAGDETAILSW t = { .cbStruct = sizeof(t) };
		t.dwFormatTagIndex = i;
		if (msacm_acmFormatTagDetailsW(had, &t,
					       ACM_FORMATTAGDETAILSF_INDEX) ||
		    t.dwFormatTag == WAVE_FORMAT_PCM ||
		    (want && t.dwFormatTag != want))
			continue;
		if (encoder ? encodes(had, t.dwFormatTag) :
		    decodes(had, t.dwFormatTag, t.cStandardFormats)) {
			if (!matches++)
				*tag = (WORD)t.dwFormatTag;
			if (!encoder)
				break;
		}
	}
	msacm_acmDriverClose(had, 0);
	if (!matches)
		return CLASS_E_CLASSNOTAVAILABLE;
	return matches > 1 ? VFW_E_DUPLICATE_NAME : S_OK;
}

const char *acm_support_names(DWORD support, char *buf, size_t size)
{
	static const struct { DWORD flag; const char *name; } names[] = {
		{ ACMDRIVERDETAILS_SUPPORTF_CODEC, "codec" },
		{ ACMDRIVERDETAILS_SUPPORTF_CONVERTER, "converter" },
		{ ACMDRIVERDETAILS_SUPPORTF_FILTER, "filter" },
		{ ACMDRIVERDETAILS_SUPPORTF_HARDWARE, "hardware" },
		{ ACMDRIVERDETAILS_SUPPORTF_ASYNC, "async" },
		{ ACMDRIVERDETAILS_SUPPORTF_LOCAL, "local" },
		{ ACMDRIVERDETAILS_SUPPORTF_DISABLED, "disabled" },
	};
	size_t at = 0;

	buf[0] = 0;
	for (size_t i = 0; i < sizeof(names) / sizeof(*names); i++)
		if ((support & names[i].flag) && at < size)
			at += (size_t)snprintf(buf + at, size - at, "%s%s",
					       at ? ", " : "", names[i].name);
	if (!buf[0])
		snprintf(buf, size, "nothing it says");
	return buf;
}

int acm_describe(HACMDRIVERID id, char *buf, size_t size)
{
	ACMDRIVERDETAILSW d = { .cbStruct = sizeof(d) };
	HACMDRIVER had;
	char shortname[80], longname[300], flags[100];
	size_t at;

	if (msacm_acmDriverDetailsW(id, &d, 0))
		return -1;
	d.szShortName[ACMDRIVERDETAILS_SHORTNAME_CHARS - 1] = 0;
	d.szLongName[ACMDRIVERDETAILS_LONGNAME_CHARS - 1] = 0;
	utf16_to_utf8(d.szShortName, (size_t)-1, shortname, sizeof(shortname));
	utf16_to_utf8(d.szLongName, (size_t)-1, longname, sizeof(longname));
	at = (size_t)snprintf(buf, size, "%s%s%s%s, %s", shortname,
			      longname[0] ? " (" : "", longname,
			      longname[0] ? ")" : "",
			      acm_support_names(d.fdwSupport, flags,
						sizeof(flags)));
	if (at < size && !msacm_acmDriverOpen(&had, id, 0)) {
		for (DWORD i = 0; i < d.cFormatTags && at < size; i++) {
			ACMFORMATTAGDETAILSW t = { .cbStruct = sizeof(t) };
			char tag[100];
			t.dwFormatTagIndex = i;
			if (msacm_acmFormatTagDetailsW(had, &t,
						       ACM_FORMATTAGDETAILSF_INDEX))
				continue;
			t.szFormatTag[ACMFORMATTAGDETAILS_FORMATTAG_CHARS - 1] = 0;
			utf16_to_utf8(t.szFormatTag, (size_t)-1, tag, sizeof(tag));
			at += (size_t)snprintf(buf + at, size - at, "%s0x%04x %s",
					       i ? ", " : ": ",
					       (unsigned)t.dwFormatTag, tag);
		}
		msacm_acmDriverClose(had, 0);
	}
	return 0;
}

HRESULT acm_codec_open(const char *path, struct acm_codec **out)
{
	struct acm_codec *c = calloc(1, sizeof(*c));
	MMRESULT r;

	*out = NULL;
	if (!c)
		return E_OUTOFMEMORY;
	if ((r = msacm_add_path(path, &c->id)) ||
	    (r = msacm_acmDriverOpen(&c->had, c->id, 0))) {
		free(c);
		return hr_of(r);
	}
	c->max_format = format_room(c->had);
	*out = c;
	return S_OK;
}

void acm_codec_close(struct acm_codec *c)
{
	if (!c)
		return;
	msacm_acmDriverClose(c->had, 0);
	free(c);
}

/* Choosing a format by bit rate. */

struct pick {
	DWORD tag;
	DWORD want;		/* bytes per second */
	WAVEFORMATEX *best;
	DWORD best_size, distance;
};

static BOOL WINAPI pick_format(HACMDRIVERID id, LPACMFORMATDETAILSW d,
			       DWORD_PTR inst, DWORD support)
{
	struct pick *p = (struct pick *)inst;
	DWORD rate = d->pwfx->nAvgBytesPerSec;
	DWORD distance = rate > p->want ? rate - p->want : p->want - rate;

	(void)id;
	(void)support;
	if (d->dwFormatTag != p->tag)
		return TRUE;
	if (!p->best_size || distance < p->distance) {
		DWORD n = d->pwfx->wFormatTag == WAVE_FORMAT_PCM ?
			  sizeof(PCMWAVEFORMAT) :
			  sizeof(WAVEFORMATEX) + d->pwfx->cbSize;
		if (n <= d->cbwfx) {
			memcpy(p->best, d->pwfx, n);
			p->best_size = n;
			p->distance = distance;
		}
	}
	return TRUE;
}

/*
 * The format of tag for in (in_size bytes, as its cbSize says) nearest
 * bitrate, into out; 0 or -1.
 */
static int by_bitrate(struct acm_codec *c, const WAVEFORMATEX *in,
		      size_t in_size, WORD tag, DWORD bitrate,
		      WAVEFORMATEX *out, size_t room)
{
	WAVEFORMATEX *wf = calloc(1, room > in_size ? room : in_size);
	struct pick p = { tag, bitrate / 8, out, 0, 0 };
	ACMFORMATDETAILSW d = { .cbStruct = sizeof(d) };

	if (!wf)
		return -1;
	/* Formats in converts to: the restriction is in itself. */
	memcpy(wf, in, in_size);
	d.pwfx = wf;
	d.cbwfx = (DWORD)room;
	msacm_acmFormatEnumW(c->had, &d, pick_format, (DWORD_PTR)&p,
			     ACM_FORMATENUMF_CONVERT);
	free(wf);
	return p.best_size ? 0 : -1;
}

HRESULT acm_stream_open(struct acm_codec *c, const WAVEFORMATEX *in,
			size_t in_size, const WAVEFORMATEX *out,
			size_t out_size, DWORD bitrate, acm_output_fn callback,
			void *opaque, struct acm_stream **sp)
{
	char name[24];
	MMRESULT r;

	*sp = NULL;
	if (!c || !in || !out || in_size < sizeof(PCMWAVEFORMAT))
		return E_POINTER;
	size_t room = c->max_format > out_size ? c->max_format : out_size;
	if (room < sizeof(WAVEFORMATEX))
		room = sizeof(WAVEFORMATEX);
	struct acm_stream *s = calloc(1, sizeof(*s));
	WAVEFORMATEX *fmt = calloc(1, room);
	/* A copy of in with room for its cbSize, for drivers that read it. */
	WAVEFORMATEX *src = calloc(1, in_size < sizeof(WAVEFORMATEX) ?
				      sizeof(WAVEFORMATEX) : in_size);
	if (!s || !fmt || !src) {
		free(s);
		free(fmt);
		free(src);
		return E_OUTOFMEMORY;
	}
	memcpy(src, in, in_size);
	/* What follows it is what there is, whatever its cbSize says. */
	if (in_size < sizeof(WAVEFORMATEX))
		src->cbSize = 0;
	else if (src->cbSize > in_size - sizeof(WAVEFORMATEX))
		src->cbSize = (WORD)(in_size - sizeof(WAVEFORMATEX));
	size_t src_size = in_size < sizeof(WAVEFORMATEX) ?
			  sizeof(WAVEFORMATEX) : in_size;

	if (out_size) {
		memcpy(fmt, out, out_size);
	} else {
		WORD tag = out->wFormatTag;
		r = ACMERR_NOTPOSSIBLE;
		if (bitrate &&
		    !by_bitrate(c, src, src_size, tag, bitrate, fmt, room))
			r = MMSYSERR_NOERROR;
		if (r) {
			memset(fmt, 0, room);
			fmt->wFormatTag = tag;
			r = msacm_acmFormatSuggest(c->had, src, fmt, (DWORD)room,
						   ACM_FORMATSUGGESTF_WFORMATTAG);
		}
		if (r) {
			TRACE("acm: no format of tag 0x%04x for this input: %s",
			      tag, msacm_error_name(r, name));
			goto refused;
		}
	}
	s->out_size = fmt->wFormatTag == WAVE_FORMAT_PCM && !fmt->cbSize ?
		      sizeof(PCMWAVEFORMAT) :
		      sizeof(WAVEFORMATEX) + fmt->cbSize;
	if (s->out_size < sizeof(WAVEFORMATEX))
		s->out_size = sizeof(WAVEFORMATEX);
	if (s->out_size > room)
		goto refused;
	r = msacm_acmStreamOpen(&s->has, c->had, src, fmt, NULL, 0, 0,
				ACM_STREAMOPENF_NONREALTIME);
	if (r) {
		TRACE("acm: no stream from tag 0x%04x to 0x%04x: %s",
		      src->wFormatTag, fmt->wFormatTag,
		      msacm_error_name(r, name));
		goto refused;
	}
	free(src);
	s->c = c;
	s->out = fmt;
	s->in_block = in->nBlockAlign ? in->nBlockAlign : 1;
	s->out_block = fmt->nBlockAlign ? fmt->nBlockAlign : 1;
	s->pcm_out = audio_format_sample(fmt, s->out_size) != AUDIO_NONE;
	s->pcm_in = audio_format_sample(in, in_size);
	s->rate = fmt->nSamplesPerSec;
	s->callback = callback;
	s->opaque = opaque;
	*sp = s;
	return S_OK;

refused:
	free(s);
	free(fmt);
	free(src);
	return VFW_E_TYPE_NOT_ACCEPTED;
}

const WAVEFORMATEX *acm_stream_format(const struct acm_stream *s, size_t *size)
{
	if (size)
		*size = s ? s->out_size : 0;
	return s ? s->out : NULL;
}

static int reserve(BYTE **buf, size_t *cap, size_t n)
{
	if (n <= *cap)
		return 0;
	BYTE *b = realloc(*buf, n);
	if (!b)
		return -1;
	*buf = b;
	*cap = n;
	return 0;
}

/*
 * Hands on what a conversion made: PCM timed, else block by block, small
 * blocks (G.711's are a sample) together up to SMALL_BLOCKS bytes, lest an
 * AVI get a chunk for each.
 */
static HRESULT deliver(struct acm_stream *s, const BYTE *data, size_t n)
{
	size_t piece = s->pcm_out || s->out_block <= 1 ? n : s->out_block;

	if (!s->pcm_out && s->out_block > 1 && piece < SMALL_BLOCKS)
		piece *= SMALL_BLOCKS / piece;

	for (size_t at = 0; at < n; at += piece) {
		struct acm_output o = { data + at, n - at < piece ? n - at : piece,
					0, 0, 0 };
		if (s->pcm_out) {
			uint64_t frames = o.size / s->out_block;
			o.timed = 1;
			o.start = s->base + audio_ticks(s->frames_out, 1, s->rate);
			o.stop = s->base + audio_ticks(s->frames_out + frames, 1,
						       s->rate);
			s->frames_out += frames;
		}
		HRESULT hr = s->callback ? s->callback(s->opaque, &o) : S_OK;
		if (FAILED(hr))
			return hr;
	}
	return S_OK;
}

/*
 * An encoder's last part of a block, padded with silence to a whole one
 * as Windows applications do, since drivers convert only whole blocks
 * (IMA ADPCM's of 2041 samples would lose up to 2040). Whether it was.
 */
static int pad_tail(struct acm_stream *s)
{
	DWORD whole = 0;

	if (s->pcm_in == AUDIO_NONE || !s->held ||
	    msacm_acmStreamSize(s->has, s->out_block, &whole,
				ACM_STREAMSIZEF_DESTINATION) ||
	    whole <= s->held || whole > MAX_OUTPUT ||
	    reserve(&s->pending, &s->pending_cap, whole))
		return 0;
	memset(s->pending + s->held, s->pcm_in == AUDIO_U8 ? 0x80 : 0,
	       whole - s->held);
	TRACE("acm: %lu bytes of silence to finish the last block",
	      (unsigned long)(whole - s->held));
	s->held = whole;
	return 1;
}

static HRESULT convert(struct acm_stream *s, int end)
{
	char name[24];
	int padded = 0;

again:
	for (int idle = 0; idle < MAX_ROUNDS; ) {
		DWORD want = 0;
		if (s->held)
			msacm_acmStreamSize(s->has, (DWORD)s->held, &want,
					    ACM_STREAMSIZEF_SOURCE);
		size_t need = want;
		if (need < MIN_OUTPUT)
			need = MIN_OUTPUT;
		if (need < 4 * (size_t)s->out_block)
			need = 4 * (size_t)s->out_block;
		if (need > MAX_OUTPUT)
			need = MAX_OUTPUT;
		if (reserve(&s->dst, &s->dst_cap, need) ||
		    reserve(&s->pending, &s->pending_cap, 1))
			return E_OUTOFMEMORY;

		ACMSTREAMHEADER h = { .cbStruct = sizeof(h) };
		h.pbSrc = s->pending;
		h.cbSrcLength = (DWORD)s->held;
		h.pbDst = s->dst;
		h.cbDstLength = (DWORD)s->dst_cap;
		MMRESULT r = msacm_acmStreamPrepareHeader(s->has, &h, 0);
		if (!r) {
			r = msacm_acmStreamConvert(s->has, &h,
				ACM_STREAMCONVERTF_BLOCKALIGN |
				(!s->started ? ACM_STREAMCONVERTF_START : 0) |
				(end ? ACM_STREAMCONVERTF_END : 0));
			MMRESULT u = msacm_acmStreamUnprepareHeader(s->has, &h, 0);
			if (!r)
				r = u;
		}
		if (r) {
			vfw_set_error("the ACM driver's conversion failed (%s)",
				      msacm_error_name(r, name));
			return hr_of(r);
		}
		s->started = 1;
		HRESULT hr = deliver(s, s->dst, h.cbDstLengthUsed);
		if (FAILED(hr))
			return hr;
		memmove(s->pending, s->pending + h.cbSrcLengthUsed,
			s->held - h.cbSrcLengthUsed);
		s->held -= h.cbSrcLengthUsed;
		if (!h.cbSrcLengthUsed && !h.cbDstLengthUsed)
			break;		/* it wants more input, or is done */
		if (!s->held && !end)
			break;
		idle = h.cbSrcLengthUsed ? 0 : idle + 1;
	}
	if (end && !padded && pad_tail(s)) {
		padded = 1;
		goto again;
	}
	if (end && s->held)
		TRACE("acm: %lu bytes at the end the driver didn't take",
		      (unsigned long)s->held);
	return S_OK;
}

HRESULT acm_stream_push(struct acm_stream *s, const void *data, size_t size,
			REFERENCE_TIME start)
{
	if (!s)
		return E_POINTER;
	if (!s->based) {
		s->base = start > 0 ? start : 0;
		s->based = 1;
	}
	if (reserve(&s->pending, &s->pending_cap, s->held + size + 1))
		return E_OUTOFMEMORY;
	memcpy(s->pending + s->held, data, size);
	s->held += size;
	return convert(s, 0);
}

HRESULT acm_stream_end(struct acm_stream *s)
{
	return s ? convert(s, 1) : E_POINTER;
}

HRESULT acm_stream_reset(struct acm_stream *s)
{
	if (!s)
		return E_POINTER;
	MMRESULT r = msacm_acmStreamReset(s->has, 0);
	s->held = 0;
	s->started = 0;
	s->based = 0;
	s->frames_out = 0;
	return hr_of(r);
}

void acm_stream_close(struct acm_stream *s)
{
	if (!s)
		return;
	msacm_acmStreamClose(s->has, 0);
	free(s->out);
	free(s->pending);
	free(s->dst);
	free(s);
}
