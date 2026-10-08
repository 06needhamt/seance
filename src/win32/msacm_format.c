/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * msacm32.dll: format and filter tags, formats and filters (their details
 * and enumeration), format suggestions, and acmFormatChoose and
 * acmFilterChoose without their dialogs.
 *
 * With no GUI, acmFormatChoose answers from $VFW_ACM_FORMAT: "cancel", or
 * a format tag (a number: 85 or 0x55) optionally followed by a rate,
 * channels and bits ("0x11,22050,1"); the first format the drivers list
 * that matches it, within what the caller lets it choose from, is the
 * answer. Unset, it answers the format it was given if asked to start
 * there (ACMFORMATCHOOSE_STYLEF_INITTOWFXSTRUCT), as pressing OK would,
 * and otherwise cancels. acmFilterChoose answers likewise from the filter
 * it is given, or cancels.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "common/log.h"
#include "win32/codepage.h"
#include "win32/internal.h"
#include "win32/msacm32.h"

/* Each driver to ask: the one open (had), else every usable one. */
struct walk {
	struct acm_driver *given;
	struct acm_id *id;	/* the next to try */
	struct acm_driver *open;	/* opened by the walk */
};

static MMRESULT walk_begin(struct walk *w, HACMDRIVER had)
{
	memset(w, 0, sizeof(*w));
	if (had && !(w->given = acm_find_driver(had)))
		return MMSYSERR_INVALHANDLE;
	w->id = w->given ? NULL : acm_first();
	return MMSYSERR_NOERROR;
}

/* The next driver to ask whose tags pass `want`, open; NULL at the end. */
static struct acm_driver *walk_next(struct walk *w,
				    int (*want)(const struct acm_id *, DWORD),
				    DWORD tag)
{
	if (w->open) {
		acm_driver_close(w->open);
		w->open = NULL;
	}
	if (w->given) {
		struct acm_driver *d = w->given;
		w->given = NULL;
		w->id = NULL;
		return d;
	}
	for (; w->id; ) {
		struct acm_id *i = w->id;
		w->id = i->next;
		if (!acm_usable(i) || (want && !want(i, tag)))
			continue;
		if (!acm_driver_open(i, &w->open))
			return w->open;
	}
	return NULL;
}

static void walk_end(struct walk *w)
{
	if (w->open)
		acm_driver_close(w->open);
	w->open = NULL;
}

static const WCHAR pcm_name[] = { 'P', 'C', 'M', 0 };

/* Format tags. */

MMRESULT STUB_WINAPI msacm_acmFormatTagDetailsW(HACMDRIVER had,
						LPACMFORMATTAGDETAILSW d,
						DWORD flags)
{
	MMRESULT r = ACMERR_NOTPOSSIBLE;
	struct walk w;
	struct acm_driver *drv;

	if (!d || d->cbStruct < sizeof(*d))
		return MMSYSERR_INVALPARAM;
	if (flags & ~(DWORD)ACM_FORMATTAGDETAILSF_QUERYMASK)
		return MMSYSERR_INVALFLAG;
	acm_lock();
	acm_sync();
	if ((r = walk_begin(&w, had)))
		goto done;
	r = ACMERR_NOTPOSSIBLE;
	switch (flags) {
	case ACM_FORMATTAGDETAILSF_FORMATTAG:
		while (r && (drv = walk_next(&w, acm_has_format, d->dwFormatTag)))
			if (acm_has_format(drv->id, d->dwFormatTag) ||
			    drv->id->cached <= 0)
				r = acm_message(drv, ACMDM_FORMATTAG_DETAILS,
						(LPARAM)d, flags);
		break;
	case ACM_FORMATTAGDETAILSF_INDEX:
		/* An index means something only within one driver. */
		if (had && (drv = walk_next(&w, NULL, 0)))
			r = acm_message(drv, ACMDM_FORMATTAG_DETAILS, (LPARAM)d,
					flags);
		break;
	case ACM_FORMATTAGDETAILSF_LARGESTSIZE: {
		ACMFORMATTAGDETAILSW best = { 0 }, t;
		while ((drv = walk_next(&w, NULL, 0))) {
			t = *d;
			if (!acm_message(drv, ACMDM_FORMATTAG_DETAILS,
					 (LPARAM)&t, flags) &&
			    (r || t.cbFormatSize > best.cbFormatSize)) {
				best = t;
				r = MMSYSERR_NOERROR;
			}
		}
		if (!r)
			*d = best;
		break;
	}
	default:
		r = MMSYSERR_INVALFLAG;
	}
	walk_end(&w);
	if (!r && d->dwFormatTag == WAVE_FORMAT_PCM && !d->szFormatTag[0])
		acm_wcopy(d->szFormatTag, ACMFORMATTAGDETAILS_FORMATTAG_CHARS,
			  pcm_name);
done:
	acm_unlock();
	return r;
}

static void tag_w_to_a(ACMFORMATTAGDETAILSA *a, const ACMFORMATTAGDETAILSW *w)
{
	DWORD cb = a->cbStruct;
	memcpy(a, w, offsetof(ACMFORMATTAGDETAILSA, szFormatTag));
	a->cbStruct = cb;
	acm_w_to_a(a->szFormatTag, sizeof(a->szFormatTag), w->szFormatTag);
}

static void tag_a_to_w(ACMFORMATTAGDETAILSW *w, const ACMFORMATTAGDETAILSA *a)
{
	memset(w, 0, sizeof(*w));
	memcpy(w, a, offsetof(ACMFORMATTAGDETAILSA, szFormatTag));
	w->cbStruct = sizeof(*w);
}

WINFN(MMRESULT) msacm_acmFormatTagDetailsA(HACMDRIVER had,
					   LPACMFORMATTAGDETAILSA d,
					   DWORD flags)
{
	ACMFORMATTAGDETAILSW w;

	if (!d || d->cbStruct < sizeof(*d))
		return MMSYSERR_INVALPARAM;
	tag_a_to_w(&w, d);
	MMRESULT r = msacm_acmFormatTagDetailsW(had, &w, flags);
	if (!r)
		tag_w_to_a(d, &w);
	return r;
}

/* An enumeration's callback, for the W and A forms alike. */
typedef BOOL (*tag_fn)(void *ctx, HACMDRIVERID id, ACMFORMATTAGDETAILSW *d,
		       DWORD support);

static MMRESULT format_tag_enum(HACMDRIVER had, ACMFORMATTAGDETAILSW *d,
				tag_fn fn, void *ctx, DWORD flags)
{
	struct walk w;
	struct acm_driver *drv;
	int pcm_done = 0;
	MMRESULT r;

	if (flags)
		return MMSYSERR_INVALFLAG;
	acm_lock();
	acm_sync();
	if ((r = walk_begin(&w, had)))
		goto done;
	int all = !had, stop = 0;
	while (!stop && (drv = walk_next(&w, NULL, 0))) {
		struct acm_id *id = drv->id;
		if (id->cached <= 0)
			continue;
		for (DWORD i = 0; !stop && i < id->details.cFormatTags; i++) {
			d->dwFormatTagIndex = i;
			if (acm_message(drv, ACMDM_FORMATTAG_DETAILS, (LPARAM)d,
					ACM_FORMATTAGDETAILSF_INDEX))
				continue;
			/* Across drivers, PCM is listed once. */
			if (all && d->dwFormatTag == WAVE_FORMAT_PCM) {
				if (pcm_done)
					continue;
				pcm_done = 1;
			}
			if (d->dwFormatTag == WAVE_FORMAT_PCM && !d->szFormatTag[0])
				acm_wcopy(d->szFormatTag,
					  ACMFORMATTAGDETAILS_FORMATTAG_CHARS,
					  pcm_name);
			stop = !fn(ctx, (HACMDRIVERID)id, d, acm_support(id));
		}
	}
	walk_end(&w);
done:
	acm_unlock();
	return r;
}

struct tag_cb {
	void *cb;
	DWORD_PTR inst;
	ACMFORMATTAGDETAILSA *a;
};

static BOOL tag_cb_w(void *ctx, HACMDRIVERID id, ACMFORMATTAGDETAILSW *d,
		     DWORD support)
{
	struct tag_cb *c = ctx;
	return ((ACMFORMATTAGENUMCBW)c->cb)(id, d, c->inst, support);
}

static BOOL tag_cb_a(void *ctx, HACMDRIVERID id, ACMFORMATTAGDETAILSW *d,
		     DWORD support)
{
	struct tag_cb *c = ctx;
	tag_w_to_a(c->a, d);
	return ((ACMFORMATTAGENUMCBA)c->cb)(id, c->a, c->inst, support);
}

MMRESULT STUB_WINAPI msacm_acmFormatTagEnumW(HACMDRIVER had,
					     LPACMFORMATTAGDETAILSW d,
					     ACMFORMATTAGENUMCBW cb,
					     DWORD_PTR inst, DWORD flags)
{
	struct tag_cb c = { (void *)cb, inst, NULL };

	if (!d || d->cbStruct < sizeof(*d) || !cb)
		return MMSYSERR_INVALPARAM;
	return format_tag_enum(had, d, tag_cb_w, &c, flags);
}

WINFN(MMRESULT) msacm_acmFormatTagEnumA(HACMDRIVER had,
					LPACMFORMATTAGDETAILSA d,
					ACMFORMATTAGENUMCBA cb, DWORD_PTR inst,
					DWORD flags)
{
	struct tag_cb c = { (void *)cb, inst, d };
	ACMFORMATTAGDETAILSW w;

	if (!d || d->cbStruct < sizeof(*d) || !cb)
		return MMSYSERR_INVALPARAM;
	tag_a_to_w(&w, d);
	return format_tag_enum(had, &w, tag_cb_a, &c, flags);
}

/* Formats. */

/* "44.100 kHz, 16 Bit, Stereo", as Windows words a format. */
static void describe(WCHAR *out, size_t n, const WAVEFORMATEX *wf)
{
	char s[ACMFORMATDETAILS_FORMAT_CHARS], ch[16];
	int len;

	if (wf->nChannels == 1)
		snprintf(ch, sizeof(ch), "Mono");
	else if (wf->nChannels == 2)
		snprintf(ch, sizeof(ch), "Stereo");
	else
		snprintf(ch, sizeof(ch), "%u Channels", wf->nChannels);
	len = snprintf(s, sizeof(s), "%u.%03u kHz, ",
		       (unsigned)(wf->nSamplesPerSec / 1000),
		       (unsigned)(wf->nSamplesPerSec % 1000));
	if (wf->wBitsPerSample && len > 0 && (size_t)len < sizeof(s))
		len += snprintf(s + len, sizeof(s) - len, "%u Bit, ",
				wf->wBitsPerSample);
	if (len > 0 && (size_t)len < sizeof(s))
		snprintf(s + len, sizeof(s) - len, "%s", ch);
	acm_a_to_w(out, n, s);
}

MMRESULT STUB_WINAPI msacm_acmFormatDetailsW(HACMDRIVER had,
					     LPACMFORMATDETAILSW d,
					     DWORD flags)
{
	MMRESULT r;
	struct walk w;
	struct acm_driver *drv;

	if (!d || d->cbStruct < sizeof(*d) || !d->pwfx ||
	    d->cbwfx < sizeof(PCMWAVEFORMAT))
		return MMSYSERR_INVALPARAM;
	if (flags & ~(DWORD)ACM_FORMATDETAILSF_QUERYMASK)
		return MMSYSERR_INVALFLAG;
	if (flags == ACM_FORMATDETAILSF_FORMAT &&
	    d->dwFormatTag != d->pwfx->wFormatTag)
		return MMSYSERR_INVALPARAM;
	acm_lock();
	acm_sync();
	if ((r = walk_begin(&w, had)))
		goto done;
	r = ACMERR_NOTPOSSIBLE;
	switch (flags) {
	case ACM_FORMATDETAILSF_FORMAT:
		while (r && (drv = walk_next(&w, acm_has_format, d->dwFormatTag)))
			r = acm_message(drv, ACMDM_FORMAT_DETAILS, (LPARAM)d,
					flags);
		break;
	case ACM_FORMATDETAILSF_INDEX:
		if (had && (drv = walk_next(&w, NULL, 0)))
			r = acm_message(drv, ACMDM_FORMAT_DETAILS, (LPARAM)d,
					flags);
		break;
	default:
		r = MMSYSERR_INVALFLAG;
	}
	walk_end(&w);
	if (!r && !d->szFormat[0])
		describe(d->szFormat, ACMFORMATDETAILS_FORMAT_CHARS, d->pwfx);
done:
	acm_unlock();
	return r;
}

static void format_w_to_a(ACMFORMATDETAILSA *a, const ACMFORMATDETAILSW *w)
{
	DWORD cb = a->cbStruct;
	memcpy(a, w, offsetof(ACMFORMATDETAILSA, szFormat));
	a->cbStruct = cb;
	acm_w_to_a(a->szFormat, sizeof(a->szFormat), w->szFormat);
}

static void format_a_to_w(ACMFORMATDETAILSW *w, const ACMFORMATDETAILSA *a)
{
	memset(w, 0, sizeof(*w));
	memcpy(w, a, offsetof(ACMFORMATDETAILSA, szFormat));
	w->cbStruct = sizeof(*w);
}

WINFN(MMRESULT) msacm_acmFormatDetailsA(HACMDRIVER had, LPACMFORMATDETAILSA d,
					DWORD flags)
{
	ACMFORMATDETAILSW w;

	if (!d || d->cbStruct < sizeof(*d))
		return MMSYSERR_INVALPARAM;
	format_a_to_w(&w, d);
	MMRESULT r = msacm_acmFormatDetailsW(had, &w, flags);
	if (!r)
		format_w_to_a(d, &w);
	return r;
}

typedef BOOL (*format_fn)(void *ctx, HACMDRIVERID id, ACMFORMATDETAILSW *d,
			  DWORD support);

#define FORMATENUMF_ALL (ACM_FORMATENUMF_WFORMATTAG | \
	ACM_FORMATENUMF_NCHANNELS | ACM_FORMATENUMF_NSAMPLESPERSEC | \
	ACM_FORMATENUMF_WBITSPERSAMPLE | ACM_FORMATENUMF_CONVERT | \
	ACM_FORMATENUMF_SUGGEST | ACM_FORMATENUMF_HARDWARE | \
	ACM_FORMATENUMF_INPUT | ACM_FORMATENUMF_OUTPUT)

/* Whether a format passes the enumeration's restrictions on ref. */
static int format_wanted(struct acm_driver *drv, const WAVEFORMATEX *wf,
			 WAVEFORMATEX *ref, DWORD flags)
{
	if (((flags & ACM_FORMATENUMF_NCHANNELS) &&
	     wf->nChannels != ref->nChannels) ||
	    ((flags & ACM_FORMATENUMF_NSAMPLESPERSEC) &&
	     wf->nSamplesPerSec != ref->nSamplesPerSec) ||
	    ((flags & ACM_FORMATENUMF_WBITSPERSAMPLE) &&
	     wf->wBitsPerSample != ref->wBitsPerSample))
		return 0;
	/* What ref converts to (or, suggesting, would be converted to). */
	if (flags & (ACM_FORMATENUMF_CONVERT | ACM_FORMATENUMF_SUGGEST))
		return !msacm_acmStreamOpen(NULL, (HACMDRIVER)drv, ref,
					    (LPWAVEFORMATEX)wf, NULL, 0, 0,
					    ACM_STREAMOPENF_QUERY);
	return 1;
}

static MMRESULT format_enum(HACMDRIVER had, ACMFORMATDETAILSW *d,
			    format_fn fn, void *ctx, DWORD flags)
{
	struct walk w;
	struct acm_driver *drv;
	MMRESULT r;

	if (!d->pwfx || d->cbwfx < sizeof(PCMWAVEFORMAT))
		return MMSYSERR_INVALPARAM;
	if (flags & ~(DWORD)FORMATENUMF_ALL)
		return MMSYSERR_INVALFLAG;
	/* No wave devices, so no hardware formats. */
	if (flags & (ACM_FORMATENUMF_HARDWARE | ACM_FORMATENUMF_INPUT |
		     ACM_FORMATENUMF_OUTPUT))
		return MMSYSERR_NOERROR;
	/* The restrictions, before the enumeration writes over pwfx. */
	DWORD rsize = flags & (ACM_FORMATENUMF_CONVERT | ACM_FORMATENUMF_SUGGEST) ?
		      acm_wfx_size(d->pwfx) : sizeof(PCMWAVEFORMAT);
	if (rsize > d->cbwfx)
		return MMSYSERR_INVALPARAM;
	WAVEFORMATEX *ref = calloc(1, rsize < sizeof(WAVEFORMATEX) ?
				      sizeof(WAVEFORMATEX) : rsize);
	if (!ref)
		return MMSYSERR_NOMEM;
	memcpy(ref, d->pwfx, rsize);
	if (ref->wFormatTag == WAVE_FORMAT_PCM)
		ref->cbSize = 0;
	DWORD cbwfx = d->cbwfx;

	acm_lock();
	acm_sync();
	if ((r = walk_begin(&w, had)))
		goto done;
	int stop = 0;
	while (!stop && (drv = walk_next(&w, NULL, 0))) {
		struct acm_id *id = drv->id;
		if (id->cached <= 0)
			continue;
		for (DWORD t = 0; !stop && t < id->details.cFormatTags; t++) {
			ACMFORMATTAGDETAILSW tag = { .cbStruct = sizeof(tag), .dwFormatTagIndex = t };
			if (acm_message(drv, ACMDM_FORMATTAG_DETAILS,
					(LPARAM)&tag, ACM_FORMATTAGDETAILSF_INDEX) ||
			    ((flags & ACM_FORMATENUMF_WFORMATTAG) &&
			     tag.dwFormatTag != ref->wFormatTag))
				continue;
			for (DWORD i = 0; !stop && i < tag.cStandardFormats; i++) {
				d->dwFormatIndex = i;
				d->dwFormatTag = tag.dwFormatTag;
				d->fdwSupport = 0;
				d->cbwfx = cbwfx;
				d->szFormat[0] = 0;
				memset(d->pwfx, 0, cbwfx);
				d->pwfx->wFormatTag = (WORD)tag.dwFormatTag;
				if (acm_message(drv, ACMDM_FORMAT_DETAILS,
						(LPARAM)d,
						ACM_FORMATDETAILSF_INDEX) ||
				    !format_wanted(drv, d->pwfx, ref, flags))
					continue;
				if (!d->szFormat[0])
					describe(d->szFormat,
						 ACMFORMATDETAILS_FORMAT_CHARS,
						 d->pwfx);
				stop = !fn(ctx, (HACMDRIVERID)id, d,
					   acm_support(id));
			}
		}
	}
	walk_end(&w);
done:
	acm_unlock();
	free(ref);
	return r;
}

struct format_cb {
	void *cb;
	DWORD_PTR inst;
	ACMFORMATDETAILSA *a;
};

static BOOL format_cb_w(void *ctx, HACMDRIVERID id, ACMFORMATDETAILSW *d,
			DWORD support)
{
	struct format_cb *c = ctx;
	return ((ACMFORMATENUMCBW)c->cb)(id, d, c->inst, support);
}

static BOOL format_cb_a(void *ctx, HACMDRIVERID id, ACMFORMATDETAILSW *d,
			DWORD support)
{
	struct format_cb *c = ctx;
	format_w_to_a(c->a, d);
	return ((ACMFORMATENUMCBA)c->cb)(id, c->a, c->inst, support);
}

MMRESULT STUB_WINAPI msacm_acmFormatEnumW(HACMDRIVER had,
					  LPACMFORMATDETAILSW d,
					  ACMFORMATENUMCBW cb, DWORD_PTR inst,
					  DWORD flags)
{
	struct format_cb c = { (void *)cb, inst, NULL };

	if (!d || d->cbStruct < sizeof(*d) || !cb)
		return MMSYSERR_INVALPARAM;
	return format_enum(had, d, format_cb_w, &c, flags);
}

WINFN(MMRESULT) msacm_acmFormatEnumA(HACMDRIVER had, LPACMFORMATDETAILSA d,
				     ACMFORMATENUMCBA cb, DWORD_PTR inst,
				     DWORD flags)
{
	struct format_cb c = { (void *)cb, inst, d };
	ACMFORMATDETAILSW w;

	if (!d || d->cbStruct < sizeof(*d) || !cb)
		return MMSYSERR_INVALPARAM;
	format_a_to_w(&w, d);
	return format_enum(had, &w, format_cb_a, &c, flags);
}

MMRESULT STUB_WINAPI msacm_acmFormatSuggest(HACMDRIVER had,
					    LPWAVEFORMATEX src,
					    LPWAVEFORMATEX dst, DWORD dst_size,
					    DWORD flags)
{
	MMRESULT r;
	struct walk w;
	struct acm_driver *drv;

	if (!src || !dst || dst_size < sizeof(PCMWAVEFORMAT))
		return MMSYSERR_INVALPARAM;
	if (flags & ~(DWORD)ACM_FORMATSUGGESTF_TYPEMASK)
		return MMSYSERR_INVALFLAG;
	ACMDRVFORMATSUGGEST s = {
		sizeof(s), flags, src, acm_wfx_size(src), dst, dst_size,
	};
	acm_lock();
	acm_sync();
	if ((r = walk_begin(&w, had)))
		goto done;
	r = ACMERR_NOTPOSSIBLE;
	while (r && (drv = walk_next(&w, acm_has_format, src->wFormatTag))) {
		if (!had && (flags & ACM_FORMATSUGGESTF_WFORMATTAG) &&
		    !acm_has_format(drv->id, dst->wFormatTag))
			continue;
		r = acm_message(drv, ACMDM_FORMAT_SUGGEST, (LPARAM)&s, 0);
	}
	walk_end(&w);
	if (r && !had)
		r = ACMERR_NOTPOSSIBLE;
done:
	acm_unlock();
	return r;
}

/* Filter tags and filters, as formats are. */

MMRESULT STUB_WINAPI msacm_acmFilterTagDetailsW(HACMDRIVER had,
						LPACMFILTERTAGDETAILSW d,
						DWORD flags)
{
	MMRESULT r;
	struct walk w;
	struct acm_driver *drv;

	if (!d || d->cbStruct < sizeof(*d))
		return MMSYSERR_INVALPARAM;
	if (flags & ~(DWORD)ACM_FILTERTAGDETAILSF_QUERYMASK)
		return MMSYSERR_INVALFLAG;
	acm_lock();
	acm_sync();
	if ((r = walk_begin(&w, had)))
		goto done;
	r = ACMERR_NOTPOSSIBLE;
	switch (flags) {
	case ACM_FILTERTAGDETAILSF_FILTERTAG:
		while (r && (drv = walk_next(&w, acm_has_filter, d->dwFilterTag)))
			r = acm_message(drv, ACMDM_FILTERTAG_DETAILS, (LPARAM)d,
					flags);
		break;
	case ACM_FILTERTAGDETAILSF_INDEX:
		if (had && (drv = walk_next(&w, NULL, 0)))
			r = acm_message(drv, ACMDM_FILTERTAG_DETAILS, (LPARAM)d,
					flags);
		break;
	case ACM_FILTERTAGDETAILSF_LARGESTSIZE: {
		ACMFILTERTAGDETAILSW best = { 0 }, t;
		while ((drv = walk_next(&w, NULL, 0))) {
			if (drv->id->cached > 0 && !drv->id->details.cFilterTags)
				continue;
			t = *d;
			if (!acm_message(drv, ACMDM_FILTERTAG_DETAILS,
					 (LPARAM)&t, flags) &&
			    (r || t.cbFilterSize > best.cbFilterSize)) {
				best = t;
				r = MMSYSERR_NOERROR;
			}
		}
		if (!r)
			*d = best;
		break;
	}
	default:
		r = MMSYSERR_INVALFLAG;
	}
	walk_end(&w);
done:
	acm_unlock();
	return r;
}

static void ftag_w_to_a(ACMFILTERTAGDETAILSA *a, const ACMFILTERTAGDETAILSW *w)
{
	DWORD cb = a->cbStruct;
	memcpy(a, w, offsetof(ACMFILTERTAGDETAILSA, szFilterTag));
	a->cbStruct = cb;
	acm_w_to_a(a->szFilterTag, sizeof(a->szFilterTag), w->szFilterTag);
}

static void ftag_a_to_w(ACMFILTERTAGDETAILSW *w, const ACMFILTERTAGDETAILSA *a)
{
	memset(w, 0, sizeof(*w));
	memcpy(w, a, offsetof(ACMFILTERTAGDETAILSA, szFilterTag));
	w->cbStruct = sizeof(*w);
}

WINFN(MMRESULT) msacm_acmFilterTagDetailsA(HACMDRIVER had,
					   LPACMFILTERTAGDETAILSA d,
					   DWORD flags)
{
	ACMFILTERTAGDETAILSW w;

	if (!d || d->cbStruct < sizeof(*d))
		return MMSYSERR_INVALPARAM;
	ftag_a_to_w(&w, d);
	MMRESULT r = msacm_acmFilterTagDetailsW(had, &w, flags);
	if (!r)
		ftag_w_to_a(d, &w);
	return r;
}

typedef BOOL (*ftag_fn)(void *ctx, HACMDRIVERID id, ACMFILTERTAGDETAILSW *d,
			DWORD support);

static MMRESULT filter_tag_enum(HACMDRIVER had, ACMFILTERTAGDETAILSW *d,
				ftag_fn fn, void *ctx, DWORD flags)
{
	struct walk w;
	struct acm_driver *drv;
	MMRESULT r;

	if (flags)
		return MMSYSERR_INVALFLAG;
	acm_lock();
	acm_sync();
	if ((r = walk_begin(&w, had)))
		goto done;
	int stop = 0;
	while (!stop && (drv = walk_next(&w, NULL, 0))) {
		struct acm_id *id = drv->id;
		if (id->cached <= 0)
			continue;
		for (DWORD i = 0; !stop && i < id->details.cFilterTags; i++) {
			d->dwFilterTagIndex = i;
			if (!acm_message(drv, ACMDM_FILTERTAG_DETAILS, (LPARAM)d,
					 ACM_FILTERTAGDETAILSF_INDEX))
				stop = !fn(ctx, (HACMDRIVERID)id, d,
					   acm_support(id));
		}
	}
	walk_end(&w);
done:
	acm_unlock();
	return r;
}

struct ftag_cb {
	void *cb;
	DWORD_PTR inst;
	ACMFILTERTAGDETAILSA *a;
};

static BOOL ftag_cb_w(void *ctx, HACMDRIVERID id, ACMFILTERTAGDETAILSW *d,
		      DWORD support)
{
	struct ftag_cb *c = ctx;
	return ((ACMFILTERTAGENUMCBW)c->cb)(id, d, c->inst, support);
}

static BOOL ftag_cb_a(void *ctx, HACMDRIVERID id, ACMFILTERTAGDETAILSW *d,
		      DWORD support)
{
	struct ftag_cb *c = ctx;
	ftag_w_to_a(c->a, d);
	return ((ACMFILTERTAGENUMCBA)c->cb)(id, c->a, c->inst, support);
}

WINFN(MMRESULT) msacm_acmFilterTagEnumW(HACMDRIVER had,
					LPACMFILTERTAGDETAILSW d,
					ACMFILTERTAGENUMCBW cb, DWORD_PTR inst,
					DWORD flags)
{
	struct ftag_cb c = { (void *)cb, inst, NULL };

	if (!d || d->cbStruct < sizeof(*d) || !cb)
		return MMSYSERR_INVALPARAM;
	return filter_tag_enum(had, d, ftag_cb_w, &c, flags);
}

WINFN(MMRESULT) msacm_acmFilterTagEnumA(HACMDRIVER had,
					LPACMFILTERTAGDETAILSA d,
					ACMFILTERTAGENUMCBA cb, DWORD_PTR inst,
					DWORD flags)
{
	struct ftag_cb c = { (void *)cb, inst, d };
	ACMFILTERTAGDETAILSW w;

	if (!d || d->cbStruct < sizeof(*d) || !cb)
		return MMSYSERR_INVALPARAM;
	ftag_a_to_w(&w, d);
	return filter_tag_enum(had, &w, ftag_cb_a, &c, flags);
}

MMRESULT STUB_WINAPI msacm_acmFilterDetailsW(HACMDRIVER had,
					     LPACMFILTERDETAILSW d,
					     DWORD flags)
{
	MMRESULT r;
	struct walk w;
	struct acm_driver *drv;

	if (!d || d->cbStruct < sizeof(*d) || !d->pwfltr ||
	    d->cbwfltr < sizeof(WAVEFILTER))
		return MMSYSERR_INVALPARAM;
	if (flags & ~(DWORD)ACM_FILTERDETAILSF_QUERYMASK)
		return MMSYSERR_INVALFLAG;
	if (flags == ACM_FILTERDETAILSF_FILTER &&
	    d->dwFilterTag != d->pwfltr->dwFilterTag)
		return MMSYSERR_INVALPARAM;
	acm_lock();
	acm_sync();
	if ((r = walk_begin(&w, had)))
		goto done;
	r = ACMERR_NOTPOSSIBLE;
	switch (flags) {
	case ACM_FILTERDETAILSF_FILTER:
		while (r && (drv = walk_next(&w, acm_has_filter, d->dwFilterTag)))
			r = acm_message(drv, ACMDM_FILTER_DETAILS, (LPARAM)d,
					flags);
		break;
	case ACM_FILTERDETAILSF_INDEX:
		if (had && (drv = walk_next(&w, NULL, 0)))
			r = acm_message(drv, ACMDM_FILTER_DETAILS, (LPARAM)d,
					flags);
		break;
	default:
		r = MMSYSERR_INVALFLAG;
	}
	walk_end(&w);
done:
	acm_unlock();
	return r;
}

static void filter_w_to_a(ACMFILTERDETAILSA *a, const ACMFILTERDETAILSW *w)
{
	DWORD cb = a->cbStruct;
	memcpy(a, w, offsetof(ACMFILTERDETAILSA, szFilter));
	a->cbStruct = cb;
	acm_w_to_a(a->szFilter, sizeof(a->szFilter), w->szFilter);
}

static void filter_a_to_w(ACMFILTERDETAILSW *w, const ACMFILTERDETAILSA *a)
{
	memset(w, 0, sizeof(*w));
	memcpy(w, a, offsetof(ACMFILTERDETAILSA, szFilter));
	w->cbStruct = sizeof(*w);
}

WINFN(MMRESULT) msacm_acmFilterDetailsA(HACMDRIVER had, LPACMFILTERDETAILSA d,
					DWORD flags)
{
	ACMFILTERDETAILSW w;

	if (!d || d->cbStruct < sizeof(*d))
		return MMSYSERR_INVALPARAM;
	filter_a_to_w(&w, d);
	MMRESULT r = msacm_acmFilterDetailsW(had, &w, flags);
	if (!r)
		filter_w_to_a(d, &w);
	return r;
}

typedef BOOL (*filter_fn)(void *ctx, HACMDRIVERID id, ACMFILTERDETAILSW *d,
			  DWORD support);

static MMRESULT filter_enum(HACMDRIVER had, ACMFILTERDETAILSW *d,
			    filter_fn fn, void *ctx, DWORD flags)
{
	struct walk w;
	struct acm_driver *drv;
	MMRESULT r;

	if (!d->pwfltr || d->cbwfltr < sizeof(WAVEFILTER))
		return MMSYSERR_INVALPARAM;
	if (flags & ~(DWORD)ACM_FILTERENUMF_DWFILTERTAG)
		return MMSYSERR_INVALFLAG;
	DWORD only = d->pwfltr->dwFilterTag, cbwfltr = d->cbwfltr;
	acm_lock();
	acm_sync();
	if ((r = walk_begin(&w, had)))
		goto done;
	int stop = 0;
	while (!stop && (drv = walk_next(&w, NULL, 0))) {
		struct acm_id *id = drv->id;
		if (id->cached <= 0)
			continue;
		for (DWORD t = 0; !stop && t < id->details.cFilterTags; t++) {
			ACMFILTERTAGDETAILSW tag = { .cbStruct = sizeof(tag), .dwFilterTagIndex = t };
			if (acm_message(drv, ACMDM_FILTERTAG_DETAILS,
					(LPARAM)&tag, ACM_FILTERTAGDETAILSF_INDEX) ||
			    ((flags & ACM_FILTERENUMF_DWFILTERTAG) &&
			     tag.dwFilterTag != only))
				continue;
			for (DWORD i = 0; !stop && i < tag.cStandardFilters; i++) {
				d->dwFilterIndex = i;
				d->dwFilterTag = tag.dwFilterTag;
				d->fdwSupport = 0;
				d->cbwfltr = cbwfltr;
				d->szFilter[0] = 0;
				memset(d->pwfltr, 0, cbwfltr);
				d->pwfltr->cbStruct = cbwfltr;
				d->pwfltr->dwFilterTag = tag.dwFilterTag;
				if (!acm_message(drv, ACMDM_FILTER_DETAILS,
						 (LPARAM)d,
						 ACM_FILTERDETAILSF_INDEX))
					stop = !fn(ctx, (HACMDRIVERID)id, d,
						   acm_support(id));
			}
		}
	}
	walk_end(&w);
done:
	acm_unlock();
	return r;
}

struct filter_cb {
	void *cb;
	DWORD_PTR inst;
	ACMFILTERDETAILSA *a;
};

static BOOL filter_cb_w(void *ctx, HACMDRIVERID id, ACMFILTERDETAILSW *d,
			DWORD support)
{
	struct filter_cb *c = ctx;
	return ((ACMFILTERENUMCBW)c->cb)(id, d, c->inst, support);
}

static BOOL filter_cb_a(void *ctx, HACMDRIVERID id, ACMFILTERDETAILSW *d,
			DWORD support)
{
	struct filter_cb *c = ctx;
	filter_w_to_a(c->a, d);
	return ((ACMFILTERENUMCBA)c->cb)(id, c->a, c->inst, support);
}

WINFN(MMRESULT) msacm_acmFilterEnumW(HACMDRIVER had, LPACMFILTERDETAILSW d,
				     ACMFILTERENUMCBW cb, DWORD_PTR inst,
				     DWORD flags)
{
	struct filter_cb c = { (void *)cb, inst, NULL };

	if (!d || d->cbStruct < sizeof(*d) || !cb)
		return MMSYSERR_INVALPARAM;
	return filter_enum(had, d, filter_cb_w, &c, flags);
}

WINFN(MMRESULT) msacm_acmFilterEnumA(HACMDRIVER had, LPACMFILTERDETAILSA d,
				     ACMFILTERENUMCBA cb, DWORD_PTR inst,
				     DWORD flags)
{
	struct filter_cb c = { (void *)cb, inst, d };
	ACMFILTERDETAILSW w;

	if (!d || d->cbStruct < sizeof(*d) || !cb)
		return MMSYSERR_INVALPARAM;
	filter_a_to_w(&w, d);
	return filter_enum(had, &w, filter_cb_a, &c, flags);
}

/* The choosers. */

/* What $VFW_ACM_FORMAT asks for. */
struct wanted {
	DWORD tag, rate;
	WORD channels, bits;
	int n;			/* how many of those it gives */
	WAVEFORMATEX *found;
	DWORD size;
	ACMFORMATDETAILSW details;
};

static int parse_wanted(const char *s, struct wanted *w)
{
	unsigned long v[4];
	char *end;

	memset(w, 0, sizeof(*w));
	for (w->n = 0; w->n < 4; w->n++) {
		v[w->n] = strtoul(s, &end, 0);
		if (end == s)
			return -1;
		s = end;
		if (!*s)
			break;
		if (*s++ != ',')
			return -1;
	}
	if (w->n == 4)
		return -1;
	w->n++;
	w->tag = v[0];
	w->rate = w->n > 1 ? v[1] : 0;
	w->channels = w->n > 2 ? (WORD)v[2] : 0;
	w->bits = w->n > 3 ? (WORD)v[3] : 0;
	return 0;
}

static BOOL choose_cb(void *ctx, HACMDRIVERID id, ACMFORMATDETAILSW *d,
		      DWORD support)
{
	struct wanted *w = ctx;
	const WAVEFORMATEX *wf = d->pwfx;

	(void)id;
	(void)support;
	if (wf->wFormatTag != w->tag ||
	    (w->n > 1 && wf->nSamplesPerSec != w->rate) ||
	    (w->n > 2 && wf->nChannels != w->channels) ||
	    (w->n > 3 && wf->wBitsPerSample != w->bits))
		return TRUE;
	DWORD size = acm_wfx_size(wf);
	if ((w->found = malloc(size))) {
		memcpy(w->found, wf, size);
		w->size = size;
		w->details = *d;
	}
	return FALSE;
}

/* The tag's name, for the answer. */
static void tag_name(DWORD tag, WCHAR *out, size_t n)
{
	ACMFORMATTAGDETAILSW t = { .cbStruct = sizeof(t) };

	t.dwFormatTag = tag;
	out[0] = 0;
	if (!msacm_acmFormatTagDetailsW(NULL, &t,
					ACM_FORMATTAGDETAILSF_FORMATTAG))
		acm_wcopy(out, n, t.szFormatTag);
}

WINFN(MMRESULT) msacm_acmFormatChooseW(LPACMFORMATCHOOSEW c)
{
	struct wanted w;
	const char *env = getenv("VFW_ACM_FORMAT");
	char *title = NULL, desc[ACMFORMATDETAILS_FORMAT_CHARS];
	MMRESULT r = MMSYSERR_NOERROR;

	if (!c || c->cbStruct < sizeof(*c) || !c->pwfx ||
	    c->cbwfx < sizeof(PCMWAVEFORMAT))
		return MMSYSERR_INVALPARAM;
	title = c->pszTitle ? utf16_to_utf8_dup(c->pszTitle) : NULL;
	const char *t = title ? title : "";

	if (!env || !*env) {
		if (!(c->fdwStyle & ACMFORMATCHOOSE_STYLEF_INITTOWFXSTRUCT)) {
			fprintf(stderr, "acmFormatChoose [%s]: (no GUI: cancelled; "
				"set VFW_ACM_FORMAT to answer)\n", t);
			r = ACMERR_CANCELED;
			goto done;
		}
		/* OK on the format it starts with. */
		ACMFORMATDETAILSW d = { .cbStruct = sizeof(d) };
		d.dwFormatTag = c->pwfx->wFormatTag;
		d.pwfx = c->pwfx;
		d.cbwfx = c->cbwfx;
		if ((r = msacm_acmFormatDetailsW(NULL, &d,
						 ACM_FORMATDETAILSF_FORMAT))) {
			fprintf(stderr, "acmFormatChoose [%s]: (no GUI: the format "
				"it starts with isn't one; cancelled)\n", t);
			r = ACMERR_CANCELED;
			goto done;
		}
		acm_wcopy(c->szFormat, ACMFORMATDETAILS_FORMAT_CHARS, d.szFormat);
		tag_name(c->pwfx->wFormatTag, c->szFormatTag,
			 ACMFORMATTAGDETAILS_FORMATTAG_CHARS);
		acm_w_to_a(desc, sizeof(desc), d.szFormat);
		fprintf(stderr, "acmFormatChoose [%s]: (no GUI: answering %s, "
			"as given)\n", t, desc);
		goto named;
	}
	if (!strcmp(env, "cancel")) {
		fprintf(stderr, "acmFormatChoose [%s]: (no GUI: cancelled, as "
			"VFW_ACM_FORMAT says)\n", t);
		r = ACMERR_CANCELED;
		goto done;
	}
	if (parse_wanted(env, &w)) {
		fprintf(stderr, "acmFormatChoose [%s]: VFW_ACM_FORMAT=%s: not "
			"\"cancel\" or tag[,rate[,channels[,bits]]]\n", t, env);
		r = ACMERR_CANCELED;
		goto done;
	}
	/* Among what the caller lets it choose from. */
	DWORD enum_flags = c->fdwEnum & (ACM_FORMATENUMF_WFORMATTAG |
					 ACM_FORMATENUMF_NCHANNELS |
					 ACM_FORMATENUMF_NSAMPLESPERSEC |
					 ACM_FORMATENUMF_WBITSPERSAMPLE |
					 ACM_FORMATENUMF_CONVERT |
					 ACM_FORMATENUMF_SUGGEST);
	DWORD max = 0;
	msacm_acmMetrics(NULL, ACM_METRIC_MAX_SIZE_FORMAT, &max);
	if (max < sizeof(WAVEFORMATEX))
		max = sizeof(WAVEFORMATEX);
	if (enum_flags && c->pwfxEnum && acm_wfx_size(c->pwfxEnum) > max)
		max = acm_wfx_size(c->pwfxEnum);
	WAVEFORMATEX *buf = calloc(1, max);
	ACMFORMATDETAILSW d = { .cbStruct = sizeof(d) };
	if (!buf) {
		r = MMSYSERR_NOMEM;
		goto done;
	}
	if (enum_flags && c->pwfxEnum)
		memcpy(buf, c->pwfxEnum, acm_wfx_size(c->pwfxEnum));
	else
		enum_flags = 0;
	d.pwfx = buf;
	d.cbwfx = max;
	format_enum(NULL, &d, (format_fn)choose_cb, &w, enum_flags);
	free(buf);
	if (!w.found) {
		fprintf(stderr, "acmFormatChoose [%s]: VFW_ACM_FORMAT=%s: no "
			"such format to choose; cancelled\n", t, env);
		r = ACMERR_CANCELED;
		goto done;
	}
	if (w.size > c->cbwfx) {
		free(w.found);
		r = MMSYSERR_INVALPARAM;
		goto done;
	}
	memcpy(c->pwfx, w.found, w.size);
	free(w.found);
	acm_wcopy(c->szFormat, ACMFORMATDETAILS_FORMAT_CHARS,
		  w.details.szFormat);
	tag_name(w.tag, c->szFormatTag, ACMFORMATTAGDETAILS_FORMATTAG_CHARS);
	acm_w_to_a(desc, sizeof(desc), w.details.szFormat);
	fprintf(stderr, "acmFormatChoose [%s]: (no GUI: answering %s, as "
		"VFW_ACM_FORMAT says)\n", t, desc);
named:
	if (c->pszName && c->cchName)
		c->pszName[0] = 0;
done:
	free(title);
	return r;
}

WINFN(MMRESULT) msacm_acmFormatChooseA(LPACMFORMATCHOOSEA c)
{
	ACMFORMATCHOOSEW w;
	WCHAR *title = NULL;

	if (!c || c->cbStruct < sizeof(*c))
		return MMSYSERR_INVALPARAM;
	memset(&w, 0, sizeof(w));
	w.cbStruct = sizeof(w);
	w.fdwStyle = c->fdwStyle;
	w.hwndOwner = c->hwndOwner;
	w.pwfx = c->pwfx;
	w.cbwfx = c->cbwfx;
	if (c->pszTitle) {
		char *u = acp_to_utf8(c->pszTitle);
		title = u ? utf8_to_utf16_dup(u) : NULL;
		free(u);
		w.pszTitle = title;
	}
	w.fdwEnum = c->fdwEnum;
	w.pwfxEnum = c->pwfxEnum;
	MMRESULT r = msacm_acmFormatChooseW(&w);
	free(title);
	if (!r) {
		acm_w_to_a(c->szFormatTag, sizeof(c->szFormatTag), w.szFormatTag);
		acm_w_to_a(c->szFormat, sizeof(c->szFormat), w.szFormat);
		if (c->pszName && c->cchName)
			c->pszName[0] = 0;
	}
	return r;
}

/* A filter: the one it starts with, if asked to; else cancelled. */
static MMRESULT choose_filter(DWORD style, LPWAVEFILTER f, DWORD size,
			      const char *title, ACMFILTERDETAILSW *d)
{
	if (!(style & ACMFILTERCHOOSE_STYLEF_INITTOFILTERSTRUCT) || !f ||
	    size < sizeof(*f)) {
		fprintf(stderr, "acmFilterChoose [%s]: (no GUI: cancelled)\n",
			title);
		return ACMERR_CANCELED;
	}
	memset(d, 0, sizeof(*d));
	d->cbStruct = sizeof(*d);
	d->dwFilterTag = f->dwFilterTag;
	d->pwfltr = f;
	d->cbwfltr = size;
	if (msacm_acmFilterDetailsW(NULL, d, ACM_FILTERDETAILSF_FILTER)) {
		fprintf(stderr, "acmFilterChoose [%s]: (no GUI: the filter it "
			"starts with isn't one; cancelled)\n", title);
		return ACMERR_CANCELED;
	}
	fprintf(stderr, "acmFilterChoose [%s]: (no GUI: answering the filter "
		"given)\n", title);
	return MMSYSERR_NOERROR;
}

WINFN(MMRESULT) msacm_acmFilterChooseW(LPACMFILTERCHOOSEW c)
{
	ACMFILTERDETAILSW d;
	ACMFILTERTAGDETAILSW t = { .cbStruct = sizeof(t) };

	if (!c || c->cbStruct < sizeof(*c))
		return MMSYSERR_INVALPARAM;
	char *title = c->pszTitle ? utf16_to_utf8_dup(c->pszTitle) : NULL;
	MMRESULT r = choose_filter(c->fdwStyle, c->pwfltr, c->cbwfltr,
				   title ? title : "", &d);
	free(title);
	if (!r) {
		acm_wcopy(c->szFilter, ACMFILTERDETAILS_FILTER_CHARS, d.szFilter);
		t.dwFilterTag = c->pwfltr->dwFilterTag;
		c->szFilterTag[0] = 0;
		if (!msacm_acmFilterTagDetailsW(NULL, &t,
						ACM_FILTERTAGDETAILSF_FILTERTAG))
			acm_wcopy(c->szFilterTag,
				  ACMFILTERTAGDETAILS_FILTERTAG_CHARS,
				  t.szFilterTag);
		if (c->pszName && c->cchName)
			c->pszName[0] = 0;
	}
	return r;
}

WINFN(MMRESULT) msacm_acmFilterChooseA(LPACMFILTERCHOOSEA c)
{
	ACMFILTERDETAILSW d;
	ACMFILTERTAGDETAILSW t = { .cbStruct = sizeof(t) };

	if (!c || c->cbStruct < sizeof(*c))
		return MMSYSERR_INVALPARAM;
	char *title = c->pszTitle ? acp_to_utf8(c->pszTitle) : NULL;
	MMRESULT r = choose_filter(c->fdwStyle, c->pwfltr, c->cbwfltr,
				   title ? title : "", &d);
	free(title);
	if (!r) {
		acm_w_to_a(c->szFilter, sizeof(c->szFilter), d.szFilter);
		t.dwFilterTag = c->pwfltr->dwFilterTag;
		c->szFilterTag[0] = 0;
		if (!msacm_acmFilterTagDetailsW(NULL, &t,
						ACM_FILTERTAGDETAILSF_FILTERTAG))
			acm_w_to_a(c->szFilterTag, sizeof(c->szFilterTag),
				   t.szFilterTag);
		if (c->pszName && c->cchName)
			c->pszName[0] = 0;
	}
	return r;
}

#define E(name) { #name, 0, (void *)msacm_##name }

const struct builtin_export msacm_format_exports[] = {
	E(acmFilterChooseA),
	E(acmFilterChooseW),
	E(acmFilterDetailsA),
	E(acmFilterDetailsW),
	E(acmFilterEnumA),
	E(acmFilterEnumW),
	E(acmFilterTagDetailsA),
	E(acmFilterTagDetailsW),
	E(acmFilterTagEnumA),
	E(acmFilterTagEnumW),
	E(acmFormatChooseA),
	E(acmFormatChooseW),
	E(acmFormatDetailsA),
	E(acmFormatDetailsW),
	E(acmFormatEnumA),
	E(acmFormatEnumW),
	E(acmFormatSuggest),
	E(acmFormatTagDetailsA),
	E(acmFormatTagDetailsW),
	E(acmFormatTagEnumA),
	E(acmFormatTagEnumW),
	BUILTIN_END
};
