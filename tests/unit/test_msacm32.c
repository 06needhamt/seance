/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * The built-in msacm32.dll, as applications and the harness reach it:
 * the driver list (vfw.ini's msacm.* entries, acmDriverAdd, the PCM
 * converter), details, metrics, priorities, format tags and formats and
 * their enumeration, suggestions, streams through the PCM converter, the
 * headless acmFormatChoose, and pe_acm.dll doing the same from Win32 code.
 * A small driver procedure here stands in for a codec.
 */
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "common/error.h"
#include "loader/modules.h"
#include "runtime/lowmem.h"
#include "runtime/runtime.h"
#include "vfw/driver.h"
#include "vfw/registry.h"
#include "win32/msacm32.h"
#include "test.h"

static struct module *acm;
static char root[4096];

static void *get(const char *name)
{
	void *p = mod_get_proc(acm, name);
	if (!p) {
		fprintf(stderr, "msacm32 has no %s\n", name);
		exit(1);
	}
	return p;
}

#define FN(ret, name, ...) \
	ret (WINAPI *name)(__VA_ARGS__) = \
		(ret (WINAPI *)(__VA_ARGS__))get(#name)

static WAVEFORMATEX pcm(WORD channels, DWORD rate, WORD bits)
{
	WAVEFORMATEX wf = { WAVE_FORMAT_PCM, channels, rate, 0,
			    (WORD)(channels * bits / 8), bits, 0 };
	wf.nAvgBytesPerSec = rate * wf.nBlockAlign;
	return wf;
}

static int wis(const WCHAR *w, const char *s)
{
	while (*s && *w == (BYTE)*s)
		w++, s++;
	return !*s && !*w;
}

/* A made-up codec: tag 0xABCD, made from 16-bit stereo PCM only. */
#define TAG_FAKE 0xABCD
static unsigned fake_opens, fake_streams;

static LRESULT WINAPI fake_proc(DWORD_PTR id, HDRVR h, UINT msg, LPARAM l1,
				LPARAM l2)
{
	(void)id;
	(void)h;
	switch (msg) {
	case DRV_LOAD: case DRV_ENABLE: case DRV_DISABLE: case DRV_FREE:
		return 1;
	case DRV_OPEN:
		fake_opens++;
		return 1;
	case DRV_CLOSE:
		fake_opens--;
		return 1;
	case ACMDM_DRIVER_DETAILS: {
		ACMDRIVERDETAILSW *d = (ACMDRIVERDETAILSW *)l1;
		d->fccType = ACMDRIVERDETAILS_FCCTYPE_AUDIOCODEC;
		d->fdwSupport = ACMDRIVERDETAILS_SUPPORTF_CODEC;
		d->cFormatTags = 2;
		d->szShortName[0] = 'F';
		d->szShortName[1] = 0;
		return 0;
	}
	case ACMDM_FORMATTAG_DETAILS: {
		ACMFORMATTAGDETAILSW *t = (ACMFORMATTAGDETAILSW *)l1;
		DWORD i = t->dwFormatTagIndex;
		if ((l2 & 0xf) == ACM_FORMATTAGDETAILSF_FORMATTAG)
			i = t->dwFormatTag == WAVE_FORMAT_PCM ? 0 :
			    t->dwFormatTag == TAG_FAKE ? 1 : 2;
		else if ((l2 & 0xf) != ACM_FORMATTAGDETAILSF_INDEX)
			return MMSYSERR_NOTSUPPORTED;
		if (i > 1)
			return ACMERR_NOTPOSSIBLE;
		t->dwFormatTagIndex = i;
		t->dwFormatTag = i ? TAG_FAKE : WAVE_FORMAT_PCM;
		t->cbFormatSize = i ? 24 : 16;
		t->cStandardFormats = 1;
		t->fdwSupport = ACMDRIVERDETAILS_SUPPORTF_CODEC;
		return 0;
	}
	case ACMDM_FORMAT_DETAILS: {
		ACMFORMATDETAILSW *f = (ACMFORMATDETAILSW *)l1;
		if ((l2 & 0xf) != ACM_FORMATDETAILSF_INDEX || f->dwFormatIndex)
			return ACMERR_NOTPOSSIBLE;
		WAVEFORMATEX wf = pcm(2, 44100, 16);
		if (f->dwFormatTag == TAG_FAKE) {
			wf.wFormatTag = TAG_FAKE;
			wf.cbSize = 6;
		}
		memcpy(f->pwfx, &wf, sizeof(PCMWAVEFORMAT));
		f->fdwSupport = ACMDRIVERDETAILS_SUPPORTF_CODEC;
		return 0;
	}
	case ACMDM_STREAM_OPEN: {
		ACMDRVSTREAMINSTANCE *si = (ACMDRVSTREAMINSTANCE *)l1;
		if (si->pwfxSrc->wFormatTag != WAVE_FORMAT_PCM ||
		    si->pwfxSrc->wBitsPerSample != 16 ||
		    si->pwfxSrc->nChannels != 2 ||
		    si->pwfxDst->wFormatTag != TAG_FAKE)
			return ACMERR_NOTPOSSIBLE;
		if (!(si->fdwOpen & ACM_STREAMOPENF_QUERY))
			fake_streams++;
		return 0;
	}
	case ACMDM_STREAM_CLOSE:
		fake_streams--;
		return 0;
	default:
		return MMSYSERR_NOTSUPPORTED;
	}
}

static BOOL WINAPI collect(HACMDRIVERID id, DWORD_PTR inst, DWORD support)
{
	HACMDRIVERID *ids = (HACMDRIVERID *)inst;
	(void)support;
	for (int i = 0; i < 8; i++)
		if (!ids[i]) {
			ids[i] = id;
			break;
		}
	return TRUE;
}

static BOOL WINAPI count_tags(HACMDRIVERID id, LPACMFORMATTAGDETAILSW d,
			      DWORD_PTR inst, DWORD support)
{
	(void)id;
	(void)support;
	if (d->dwFormatTag == WAVE_FORMAT_PCM)
		CHECK(wis(d->szFormatTag, "PCM"));
	(*(int *)inst)++;
	return TRUE;
}

struct counted {
	int n, mono, at_44100;
	char first[128];
};

static BOOL WINAPI count_formats(HACMDRIVERID id, LPACMFORMATDETAILSW d,
				 DWORD_PTR inst, DWORD support)
{
	struct counted *c = (struct counted *)inst;
	(void)id;
	(void)support;
	if (!c->n)
		for (int i = 0; i < 127 && (c->first[i] = (char)d->szFormat[i]); i++)
			;
	c->n++;
	c->mono += d->pwfx->nChannels == 1;
	c->at_44100 += d->pwfx->nSamplesPerSec == 44100;
	return TRUE;
}

static BOOL WINAPI count_formats_a(HACMDRIVERID id, LPACMFORMATDETAILSA d,
				   DWORD_PTR inst, DWORD support)
{
	(void)id;
	(void)d;
	(void)support;
	(*(int *)inst)++;
	return TRUE;
}

static void write_ini(const char *text)
{
	char path[4200];

	snprintf(path, sizeof(path), "%s/vfw.ini", root);
	FILE *f = fopen(path, "w");
	CHECK(f != NULL);
	if (f) {
		fputs(text, f);
		fclose(f);
	}
	vfw_registry_reload();
}

static HACMDRIVERID pcm_id(void)
{
	FN(MMRESULT, acmDriverEnum, ACMDRIVERENUMCB, DWORD_PTR, DWORD);
	HACMDRIVERID ids[8] = { 0 };
	CHECK_EQ(acmDriverEnum(collect, (DWORD_PTR)ids, 0), 0);
	for (int i = 0; i < 8 && ids[i]; i++) {
		ACMDRIVERDETAILSW d = { .cbStruct = sizeof(d) };
		if (!msacm_acmDriverDetailsW(ids[i], &d, 0) &&
		    wis(d.szShortName, "MS-PCM"))
			return ids[i];
	}
	return NULL;
}

static void test_drivers(void)
{
	FN(DWORD, acmGetVersion, void);
	FN(MMRESULT, acmMetrics, HACMOBJ, UINT, LPVOID);
	FN(MMRESULT, acmDriverEnum, ACMDRIVERENUMCB, DWORD_PTR, DWORD);
	FN(MMRESULT, acmDriverDetailsW, HACMDRIVERID, LPACMDRIVERDETAILSW, DWORD);
	FN(MMRESULT, acmDriverDetailsA, HACMDRIVERID, LPACMDRIVERDETAILSA, DWORD);
	FN(MMRESULT, acmDriverOpen, LPHACMDRIVER, HACMDRIVERID, DWORD);
	FN(MMRESULT, acmDriverClose, HACMDRIVER, DWORD);
	FN(MMRESULT, acmDriverID, HACMOBJ, LPHACMDRIVERID, DWORD);
	DWORD v = 0;

	CHECK_EQ(acmGetVersion(), 0x04000565);

	/* A driver the table names but can't be loaded takes no part. */
	write_ini("[drivers32]\nmsacm.gone=/nonexistent/gone.acm\n");
	CHECK_EQ(acmMetrics(NULL, ACM_METRIC_COUNT_DRIVERS, &v), 0);
	CHECK_EQ(v, 1);		/* the PCM converter */
	CHECK_EQ(acmMetrics(NULL, ACM_METRIC_COUNT_CONVERTERS, &v), 0);
	CHECK_EQ(v, 1);
	CHECK_EQ(acmMetrics(NULL, ACM_METRIC_COUNT_CODECS, &v), 0);
	CHECK_EQ(v, 0);
	CHECK_EQ(acmMetrics(NULL, ACM_METRIC_COUNT_DISABLED, &v), 0);
	CHECK_EQ(v, 0);
	CHECK_EQ(acmMetrics(NULL, ACM_METRIC_MAX_SIZE_FORMAT, &v), 0);
	CHECK_EQ(v, sizeof(PCMWAVEFORMAT));
	CHECK_EQ(acmMetrics(NULL, ACM_METRIC_HARDWARE_WAVE_INPUT, &v),
		 MMSYSERR_NOTSUPPORTED);
	CHECK_EQ(acmMetrics(NULL, ACM_METRIC_COUNT_DRIVERS, NULL),
		 MMSYSERR_INVALPARAM);

	HACMDRIVERID ids[8] = { 0 };
	CHECK_EQ(acmDriverEnum(collect, (DWORD_PTR)ids, 0), 0);
	CHECK(ids[0] && !ids[1]);
	CHECK_EQ(acmDriverEnum(NULL, 0, 0), MMSYSERR_INVALPARAM);
	CHECK_EQ(acmDriverEnum(collect, (DWORD_PTR)ids, 1), MMSYSERR_INVALFLAG);
	HACMDRIVERID id = ids[0];
	CHECK_EQ(acmMetrics((HACMOBJ)id, ACM_METRIC_COUNT_DRIVERS, &v),
		 MMSYSERR_INVALHANDLE);
	CHECK_EQ(acmMetrics((HACMOBJ)id, ACM_METRIC_DRIVER_SUPPORT, &v), 0);
	CHECK_EQ(v, ACMDRIVERDETAILS_SUPPORTF_CONVERTER);
	CHECK_EQ(acmMetrics((HACMOBJ)id, ACM_METRIC_DRIVER_PRIORITY, &v), 0);
	CHECK_EQ(v, 1);

	ACMDRIVERDETAILSW dw = { .cbStruct = sizeof(dw) };
	CHECK_EQ(acmDriverDetailsW(id, &dw, 0), 0);
	CHECK(wis(dw.szShortName, "MS-PCM"));
	CHECK_EQ(dw.fccType, ACMDRIVERDETAILS_FCCTYPE_AUDIOCODEC);
	CHECK_EQ(dw.cFormatTags, 1);
	CHECK_EQ(dw.cbStruct, sizeof(dw));
	ACMDRIVERDETAILSA da = { .cbStruct = sizeof(da) };
	CHECK_EQ(acmDriverDetailsA(id, &da, 0), 0);
	CHECK_STR(da.szShortName, "MS-PCM");
	CHECK_STR(da.szLongName, "seance PCM converter");
	/* Only as much as the caller has room for. */
	ACMDRIVERDETAILSW small;
	memset(&small, 0xee, sizeof(small));
	small.cbStruct = 12;
	CHECK_EQ(acmDriverDetailsW(id, &small, 0), 0);
	CHECK_EQ(small.cbStruct, 12);
	CHECK_EQ(small.wMid, 0xeeee);
	CHECK_EQ(acmDriverDetailsW(id, &dw, 1), MMSYSERR_INVALFLAG);
	CHECK_EQ(acmDriverDetailsW((HACMDRIVERID)&v, &dw, 0),
		 MMSYSERR_INVALHANDLE);

	unsigned before = drv_open_count();
	HACMDRIVER had = NULL;
	CHECK_EQ(acmDriverOpen(&had, id, 0), 0);
	CHECK(had != NULL);
	CHECK_EQ(drv_open_count(), before + 1);
	HACMDRIVERID back = NULL;
	CHECK_EQ(acmDriverID((HACMOBJ)had, &back, 0), 0);
	CHECK(back == id);
	CHECK_EQ(acmDriverClose(had, 0), 0);
	CHECK_EQ(acmDriverClose(had, 0), MMSYSERR_INVALHANDLE);
	CHECK_EQ(drv_open_count(), before);
}

/* A function driver, priorities and removal. */
static void test_function_driver(void)
{
	FN(MMRESULT, acmDriverAddW, LPHACMDRIVERID, HINSTANCE, LPARAM, DWORD,
	   DWORD);
	FN(MMRESULT, acmDriverAddA, LPHACMDRIVERID, HINSTANCE, LPARAM, DWORD,
	   DWORD);
	FN(MMRESULT, acmDriverRemove, HACMDRIVERID, DWORD);
	FN(MMRESULT, acmDriverPriority, HACMDRIVERID, DWORD, DWORD);
	FN(MMRESULT, acmDriverEnum, ACMDRIVERENUMCB, DWORD_PTR, DWORD);
	FN(MMRESULT, acmMetrics, HACMOBJ, UINT, LPVOID);
	FN(MMRESULT, acmStreamOpen, LPHACMSTREAM, HACMDRIVER, LPWAVEFORMATEX,
	   LPWAVEFORMATEX, LPWAVEFILTER, DWORD_PTR, DWORD_PTR, DWORD);
	FN(MMRESULT, acmStreamClose, HACMSTREAM, DWORD);
	FN(MMRESULT, acmDriverOpen, LPHACMDRIVER, HACMDRIVERID, DWORD);
	FN(MMRESULT, acmDriverClose, HACMDRIVER, DWORD);
	HACMDRIVERID fake = NULL;
	DWORD v;

	CHECK_EQ(acmDriverAddW(&fake, NULL, (LPARAM)fake_proc, 1,
			       ACM_DRIVERADDF_FUNCTION), MMSYSERR_INVALPARAM);
	CHECK_EQ(acmDriverAddW(&fake, NULL, 0, 0, ACM_DRIVERADDF_NOTIFYHWND),
		 MMSYSERR_NOTSUPPORTED);
	CHECK_EQ(acmDriverAddA(&fake, NULL, (LPARAM)"msacm.nosuch", 0,
			       ACM_DRIVERADDF_NAME), MMSYSERR_INVALPARAM);
	CHECK_EQ(acmDriverAddW(&fake, NULL, (LPARAM)fake_proc, 0,
			       ACM_DRIVERADDF_FUNCTION), 0);
	CHECK(fake != NULL);
	CHECK_EQ(fake_opens, 0);	/* opened for its details, and closed */
	CHECK_EQ(acmMetrics(NULL, ACM_METRIC_COUNT_CODECS, &v), 0);
	CHECK_EQ(v, 1);
	CHECK_EQ(acmMetrics(NULL, ACM_METRIC_COUNT_LOCAL_CODECS, &v), 0);
	CHECK_EQ(v, 1);
	CHECK_EQ(acmMetrics(NULL, ACM_METRIC_MAX_SIZE_FORMAT, &v), 0);
	CHECK_EQ(v, 24);
	CHECK_EQ(acmMetrics((HACMOBJ)fake, ACM_METRIC_DRIVER_SUPPORT, &v), 0);
	CHECK_EQ(v, ACMDRIVERDETAILS_SUPPORTF_CODEC |
		    ACMDRIVERDETAILS_SUPPORTF_LOCAL);
	/* Before the PCM converter. */
	HACMDRIVERID ids[8] = { 0 };
	CHECK_EQ(acmDriverEnum(collect, (DWORD_PTR)ids, 0), 0);
	CHECK(ids[0] == fake && ids[1] && !ids[2]);
	memset(ids, 0, sizeof(ids));
	CHECK_EQ(acmDriverEnum(collect, (DWORD_PTR)ids,
			       ACM_DRIVERENUMF_NOLOCAL), 0);
	CHECK(ids[0] && ids[0] != fake && !ids[1]);

	/* A stream with no driver named finds the one that takes it. */
	WAVEFORMATEX src = pcm(2, 44100, 16), dst = pcm(2, 44100, 16);
	dst.wFormatTag = TAG_FAKE;
	HACMSTREAM has = NULL;
	CHECK_EQ(acmStreamOpen(&has, NULL, &src, &dst, NULL, 0, 0, 0), 0);
	CHECK_EQ(fake_streams, 1);
	CHECK_EQ(fake_opens, 1);	/* opened for the stream */
	CHECK_EQ(acmDriverRemove(fake, 0), ACMERR_BUSY);
	CHECK_EQ(acmStreamClose(has, 0), 0);
	CHECK_EQ(fake_streams, 0);
	CHECK_EQ(fake_opens, 0);
	src = pcm(1, 44100, 16);
	CHECK_EQ(acmStreamOpen(NULL, NULL, &src, &dst, NULL, 0, 0,
			       ACM_STREAMOPENF_QUERY), ACMERR_NOTPOSSIBLE);

	/* Disabled, it is skipped; local, its place can't change. */
	CHECK_EQ(acmDriverPriority(fake, 0, ACM_DRIVERPRIORITYF_DISABLE), 0);
	src = pcm(2, 44100, 16);
	CHECK_EQ(acmStreamOpen(NULL, NULL, &src, &dst, NULL, 0, 0,
			       ACM_STREAMOPENF_QUERY), ACMERR_NOTPOSSIBLE);
	CHECK_EQ(acmMetrics(NULL, ACM_METRIC_COUNT_DISABLED, &v), 0);
	CHECK_EQ(v, 1);
	memset(ids, 0, sizeof(ids));
	CHECK_EQ(acmDriverEnum(collect, (DWORD_PTR)ids, 0), 0);
	CHECK(ids[0] && ids[0] != fake && !ids[1]);
	memset(ids, 0, sizeof(ids));
	CHECK_EQ(acmDriverEnum(collect, (DWORD_PTR)ids,
			       ACM_DRIVERENUMF_DISABLED), 0);
	CHECK(ids[0] == fake && ids[1]);
	CHECK_EQ(acmDriverPriority(fake, 0, ACM_DRIVERPRIORITYF_ENABLE), 0);
	CHECK_EQ(acmStreamOpen(NULL, NULL, &src, &dst, NULL, 0, 0,
			       ACM_STREAMOPENF_QUERY), 0);
	CHECK_EQ(acmDriverPriority(fake, 2, 0), MMSYSERR_NOTSUPPORTED);
	CHECK_EQ(acmDriverPriority(fake, 0, ACM_DRIVERPRIORITYF_ABLEMASK),
		 MMSYSERR_INVALFLAG);
	CHECK_EQ(acmDriverPriority(NULL, 0, ACM_DRIVERPRIORITYF_BEGIN), 0);
	CHECK_EQ(acmDriverPriority(NULL, 0, ACM_DRIVERPRIORITYF_END), 0);
	/* The PCM converter, not local, can be moved first and back. */
	HACMDRIVERID p = pcm_id();
	CHECK_EQ(acmDriverPriority(p, 1, 0), 0);
	CHECK_EQ(acmMetrics((HACMOBJ)p, ACM_METRIC_DRIVER_PRIORITY, &v), 0);
	CHECK_EQ(v, 1);
	CHECK_EQ(acmDriverPriority(p, (DWORD)-1, 0), 0);
	CHECK_EQ(acmMetrics((HACMOBJ)p, ACM_METRIC_DRIVER_PRIORITY, &v), 0);
	CHECK_EQ(v, 2);

	/* An open driver is busy; once closed it can go. */
	HACMDRIVER had;
	CHECK_EQ(acmDriverOpen(&had, fake, 0), 0);
	CHECK_EQ(acmStreamOpen(&has, had, &src, &dst, NULL, 0, 0, 0), 0);
	CHECK_EQ(acmDriverClose(had, 0), ACMERR_BUSY);	/* its stream */
	CHECK_EQ(acmStreamClose(has, 0), 0);
	CHECK_EQ(acmDriverClose(had, 0), 0);
	CHECK_EQ(acmDriverRemove(fake, 1), MMSYSERR_INVALFLAG);
	CHECK_EQ(acmDriverRemove(fake, 0), 0);
	CHECK_EQ(acmDriverRemove(fake, 0), MMSYSERR_INVALHANDLE);
	CHECK_EQ(acmMetrics(NULL, ACM_METRIC_COUNT_CODECS, &v), 0);
	CHECK_EQ(v, 0);
	CHECK_EQ(fake_opens, 0);
}

static void test_formats(void)
{
	FN(MMRESULT, acmFormatTagDetailsW, HACMDRIVER, LPACMFORMATTAGDETAILSW,
	   DWORD);
	FN(MMRESULT, acmFormatTagDetailsA, HACMDRIVER, LPACMFORMATTAGDETAILSA,
	   DWORD);
	FN(MMRESULT, acmFormatTagEnumW, HACMDRIVER, LPACMFORMATTAGDETAILSW,
	   ACMFORMATTAGENUMCBW, DWORD_PTR, DWORD);
	FN(MMRESULT, acmFormatDetailsW, HACMDRIVER, LPACMFORMATDETAILSW, DWORD);
	FN(MMRESULT, acmFormatEnumW, HACMDRIVER, LPACMFORMATDETAILSW,
	   ACMFORMATENUMCBW, DWORD_PTR, DWORD);
	FN(MMRESULT, acmFormatEnumA, HACMDRIVER, LPACMFORMATDETAILSA,
	   ACMFORMATENUMCBA, DWORD_PTR, DWORD);
	FN(MMRESULT, acmFormatSuggest, HACMDRIVER, LPWAVEFORMATEX,
	   LPWAVEFORMATEX, DWORD, DWORD);
	FN(MMRESULT, acmDriverOpen, LPHACMDRIVER, HACMDRIVERID, DWORD);
	FN(MMRESULT, acmDriverClose, HACMDRIVER, DWORD);

	ACMFORMATTAGDETAILSW t = { .cbStruct = sizeof(t) };
	t.dwFormatTag = WAVE_FORMAT_PCM;
	CHECK_EQ(acmFormatTagDetailsW(NULL, &t, ACM_FORMATTAGDETAILSF_FORMATTAG),
		 0);
	CHECK(wis(t.szFormatTag, "PCM"));
	CHECK_EQ(t.cStandardFormats, 36);
	CHECK_EQ(t.cbFormatSize, sizeof(PCMWAVEFORMAT));
	t.dwFormatTag = WAVE_FORMAT_ADPCM;
	CHECK_EQ(acmFormatTagDetailsW(NULL, &t, ACM_FORMATTAGDETAILSF_FORMATTAG),
		 ACMERR_NOTPOSSIBLE);
	t.dwFormatTag = WAVE_FORMAT_UNKNOWN;
	CHECK_EQ(acmFormatTagDetailsW(NULL, &t,
				      ACM_FORMATTAGDETAILSF_LARGESTSIZE), 0);
	CHECK_EQ(t.dwFormatTag, WAVE_FORMAT_PCM);
	/* An index means nothing without a driver. */
	t.dwFormatTagIndex = 0;
	CHECK_EQ(acmFormatTagDetailsW(NULL, &t, ACM_FORMATTAGDETAILSF_INDEX),
		 ACMERR_NOTPOSSIBLE);
	HACMDRIVER had;
	CHECK_EQ(acmDriverOpen(&had, pcm_id(), 0), 0);
	CHECK_EQ(acmFormatTagDetailsW(had, &t, ACM_FORMATTAGDETAILSF_INDEX), 0);
	CHECK_EQ(t.dwFormatTag, WAVE_FORMAT_PCM);
	CHECK_EQ(acmFormatTagDetailsW(had, &t, 0x10), MMSYSERR_INVALFLAG);
	t.cbStruct = 8;
	CHECK_EQ(acmFormatTagDetailsW(had, &t, 0), MMSYSERR_INVALPARAM);
	ACMFORMATTAGDETAILSA ta = { .cbStruct = sizeof(ta) };
	ta.dwFormatTag = WAVE_FORMAT_PCM;
	CHECK_EQ(acmFormatTagDetailsA(NULL, &ta,
				      ACM_FORMATTAGDETAILSF_FORMATTAG), 0);
	CHECK_STR(ta.szFormatTag, "PCM");
	CHECK_EQ(ta.cbStruct, sizeof(ta));

	int n = 0;
	t.cbStruct = sizeof(t);
	CHECK_EQ(acmFormatTagEnumW(NULL, &t, count_tags, (DWORD_PTR)&n, 0), 0);
	CHECK_EQ(n, 1);
	CHECK_EQ(acmFormatTagEnumW(NULL, &t, count_tags, (DWORD_PTR)&n, 1),
		 MMSYSERR_INVALFLAG);

	/* Formats: details, words, and enumeration by what they share. */
	WAVEFORMATEX wf = pcm(2, 44100, 16);
	ACMFORMATDETAILSW f = { .cbStruct = sizeof(f) };
	f.dwFormatTag = WAVE_FORMAT_PCM;
	f.pwfx = &wf;
	f.cbwfx = sizeof(wf);
	CHECK_EQ(acmFormatDetailsW(NULL, &f, ACM_FORMATDETAILSF_FORMAT), 0);
	CHECK(wis(f.szFormat, "44.100 kHz, 16 Bit, Stereo"));
	wf.nBlockAlign = 3;	/* not PCM that adds up */
	CHECK_EQ(acmFormatDetailsW(NULL, &f, ACM_FORMATDETAILSF_FORMAT),
		 ACMERR_NOTPOSSIBLE);
	f.dwFormatTag = WAVE_FORMAT_ADPCM;
	CHECK_EQ(acmFormatDetailsW(NULL, &f, ACM_FORMATDETAILSF_FORMAT),
		 MMSYSERR_INVALPARAM);	/* the tags disagree */
	f.dwFormatTag = WAVE_FORMAT_PCM;
	f.dwFormatIndex = 5;
	CHECK_EQ(acmFormatDetailsW(had, &f, ACM_FORMATDETAILSF_INDEX), 0);
	CHECK(wf.nSamplesPerSec == 8000 && wf.nChannels == 2 &&
	      wf.wBitsPerSample == 24 && wf.nBlockAlign == 6 && !wf.cbSize);
	CHECK(wis(f.szFormat, "8.000 kHz, 24 Bit, Stereo"));

	struct counted c = { 0 };
	CHECK_EQ(acmFormatEnumW(NULL, &f, count_formats, (DWORD_PTR)&c, 0), 0);
	CHECK_EQ(c.n, 36);
	CHECK_STR(c.first, "8.000 kHz, 8 Bit, Mono");
	memset(&c, 0, sizeof(c));
	wf = pcm(1, 22050, 8);
	CHECK_EQ(acmFormatEnumW(NULL, &f, count_formats, (DWORD_PTR)&c,
				ACM_FORMATENUMF_NCHANNELS), 0);
	CHECK(c.n == 18 && c.mono == 18);
	/* What 16-bit stereo at 44100 Hz converts to: that rate only. */
	memset(&c, 0, sizeof(c));
	wf = pcm(2, 44100, 16);
	CHECK_EQ(acmFormatEnumW(NULL, &f, count_formats, (DWORD_PTR)&c,
				ACM_FORMATENUMF_CONVERT), 0);
	CHECK(c.n == 6 && c.at_44100 == 6);
	CHECK_EQ(acmFormatEnumW(NULL, &f, count_formats, (DWORD_PTR)&c,
				ACM_FORMATENUMF_HARDWARE), 0);
	CHECK_EQ(acmFormatEnumW(NULL, &f, count_formats, (DWORD_PTR)&c,
				0x10000000), MMSYSERR_INVALFLAG);
	ACMFORMATDETAILSA fa = { .cbStruct = sizeof(fa) };
	fa.pwfx = &wf;
	fa.cbwfx = sizeof(wf);
	n = 0;
	CHECK_EQ(acmFormatEnumA(had, &fa, count_formats_a, (DWORD_PTR)&n, 0), 0);
	CHECK_EQ(n, 36);
	CHECK_EQ(acmDriverClose(had, 0), 0);

	/* Suggestions within what the converter does. */
	WAVEFORMATEX src = pcm(2, 44100, 16), dst = { 0 };
	dst.wFormatTag = WAVE_FORMAT_PCM;
	dst.wBitsPerSample = 8;
	CHECK_EQ(acmFormatSuggest(NULL, &src, &dst, sizeof(dst),
				  ACM_FORMATSUGGESTF_WFORMATTAG |
				  ACM_FORMATSUGGESTF_WBITSPERSAMPLE), 0);
	CHECK(dst.nChannels == 2 && dst.nSamplesPerSec == 44100 &&
	      dst.wBitsPerSample == 8 && dst.nBlockAlign == 2 &&
	      dst.nAvgBytesPerSec == 88200);
	dst.nChannels = 1;
	CHECK_EQ(acmFormatSuggest(NULL, &src, &dst, sizeof(dst),
				  ACM_FORMATSUGGESTF_NCHANNELS), 0);
	CHECK(dst.nChannels == 1 && dst.wBitsPerSample == 16);
	dst.nSamplesPerSec = 22050;
	CHECK_EQ(acmFormatSuggest(NULL, &src, &dst, sizeof(dst),
				  ACM_FORMATSUGGESTF_NSAMPLESPERSEC),
		 ACMERR_NOTPOSSIBLE);
	dst.wFormatTag = WAVE_FORMAT_ADPCM;
	CHECK_EQ(acmFormatSuggest(NULL, &src, &dst, sizeof(dst),
				  ACM_FORMATSUGGESTF_WFORMATTAG),
		 ACMERR_NOTPOSSIBLE);
	CHECK_EQ(acmFormatSuggest(NULL, &src, &dst, 8, 0), MMSYSERR_INVALPARAM);
	CHECK_EQ(acmFormatSuggest(NULL, &src, &dst, sizeof(dst), 1),
		 MMSYSERR_INVALFLAG);
}

static void test_streams(void)
{
	FN(MMRESULT, acmStreamOpen, LPHACMSTREAM, HACMDRIVER, LPWAVEFORMATEX,
	   LPWAVEFORMATEX, LPWAVEFILTER, DWORD_PTR, DWORD_PTR, DWORD);
	FN(MMRESULT, acmStreamClose, HACMSTREAM, DWORD);
	FN(MMRESULT, acmStreamSize, HACMSTREAM, DWORD, LPDWORD, DWORD);
	FN(MMRESULT, acmStreamPrepareHeader, HACMSTREAM, LPACMSTREAMHEADER, DWORD);
	FN(MMRESULT, acmStreamUnprepareHeader, HACMSTREAM, LPACMSTREAMHEADER,
	   DWORD);
	FN(MMRESULT, acmStreamConvert, HACMSTREAM, LPACMSTREAMHEADER, DWORD);
	FN(MMRESULT, acmStreamReset, HACMSTREAM, DWORD);
	FN(MMRESULT, acmStreamMessage, HACMSTREAM, UINT, LPARAM, LPARAM);
	unsigned before = drv_open_count();
	HACMSTREAM has = NULL;
	DWORD n = 0;

	/* Queries. */
	WAVEFORMATEX src = pcm(2, 48000, 16), dst = pcm(1, 48000, 8);
	CHECK_EQ(acmStreamOpen(NULL, NULL, &src, &dst, NULL, 0, 0,
			       ACM_STREAMOPENF_QUERY), 0);
	CHECK_EQ(acmStreamOpen(&has, NULL, &src, &dst, NULL, 0, 0,
			       ACM_STREAMOPENF_QUERY), MMSYSERR_INVALPARAM);
	CHECK_EQ(acmStreamOpen(NULL, NULL, &src, &dst, NULL, 0, 0, 0),
		 MMSYSERR_INVALPARAM);
	dst = pcm(1, 44100, 8);		/* no resampling */
	CHECK_EQ(acmStreamOpen(NULL, NULL, &src, &dst, NULL, 0, 0,
			       ACM_STREAMOPENF_QUERY), ACMERR_NOTPOSSIBLE);
	src = pcm(6, 48000, 16);
	dst = pcm(2, 48000, 16);	/* no downmixing beyond stereo */
	CHECK_EQ(acmStreamOpen(NULL, NULL, &src, &dst, NULL, 0, 0,
			       ACM_STREAMOPENF_QUERY), ACMERR_NOTPOSSIBLE);
	dst = pcm(6, 48000, 32);
	CHECK_EQ(acmStreamOpen(NULL, NULL, &src, &dst, NULL, 0, 0,
			       ACM_STREAMOPENF_QUERY), 0);
	dst = pcm(6, 48000, 16);
	dst.wFormatTag = WAVE_FORMAT_ADPCM;
	CHECK_EQ(acmStreamOpen(NULL, NULL, &src, &dst, NULL, 0, 0,
			       ACM_STREAMOPENF_QUERY), ACMERR_NOTPOSSIBLE);
	CHECK_EQ(drv_open_count(), before);
	CHECK_EQ(msacm_stream_count(), 0);

	/* Stereo to mono: the two averaged. */
	src = pcm(2, 8000, 16);
	dst = pcm(1, 8000, 16);
	CHECK_EQ(acmStreamOpen(&has, NULL, &src, &dst, NULL, 0, 0,
			       ACM_STREAMOPENF_NONREALTIME), 0);
	CHECK_EQ(msacm_stream_count(), 1);
	CHECK_EQ(acmStreamSize(has, 400, &n, ACM_STREAMSIZEF_SOURCE), 0);
	CHECK_EQ(n, 200);
	CHECK_EQ(acmStreamSize(has, 101, &n, ACM_STREAMSIZEF_DESTINATION), 0);
	CHECK_EQ(n, 200);
	CHECK_EQ(acmStreamSize(has, 3, &n, ACM_STREAMSIZEF_SOURCE),
		 ACMERR_NOTPOSSIBLE);
	CHECK_EQ(acmStreamSize(has, 4, &n, 7), MMSYSERR_INVALFLAG);

	short in[7] = { 1000, 3000, -2000, -4000, 32767, 32767, 99 };
	short out[4] = { 0, 0, 0, 0x5555 };
	ACMSTREAMHEADER h = { .cbStruct = sizeof(h) };
	h.pbSrc = (BYTE *)in;
	h.cbSrcLength = sizeof(in);	/* three frames and half of one */
	h.pbDst = (BYTE *)out;
	h.cbDstLength = sizeof(out);
	CHECK_EQ(acmStreamConvert(has, &h, 0), ACMERR_UNPREPARED);
	CHECK_EQ(acmStreamPrepareHeader(has, &h, 0), 0);
	CHECK(h.fdwStatus & ACMSTREAMHEADER_STATUSF_PREPARED);
	CHECK_EQ(acmStreamConvert(has, &h, 0x1000), MMSYSERR_INVALFLAG);
	CHECK_EQ(acmStreamConvert(has, &h, ACM_STREAMCONVERTF_BLOCKALIGN |
				  ACM_STREAMCONVERTF_START), 0);
	CHECK(h.fdwStatus & ACMSTREAMHEADER_STATUSF_DONE);
	CHECK_EQ(h.cbSrcLengthUsed, 12);
	CHECK_EQ(h.cbDstLengthUsed, 6);
	CHECK(out[0] == 2000 && out[1] == -3000 && out[2] == 32767 &&
	      out[3] == 0x5555);
	/* Buffers that aren't the ones prepared are refused. */
	h.pbSrc++;
	CHECK_EQ(acmStreamConvert(has, &h, 0), MMSYSERR_INVALPARAM);
	CHECK_EQ(acmStreamUnprepareHeader(has, &h, 0), MMSYSERR_INVALPARAM);
	h.pbSrc--;
	CHECK_EQ(acmStreamReset(has, 0), 0);
	CHECK_EQ(acmStreamReset(has, 1), MMSYSERR_INVALFLAG);
	CHECK_EQ(acmStreamMessage(has, ACMDM_USER, 0, 0), MMSYSERR_NOTSUPPORTED);
	CHECK_EQ(acmStreamUnprepareHeader(has, &h, 0), 0);
	CHECK(!(h.fdwStatus & (ACMSTREAMHEADER_STATUSF_PREPARED |
			       ACMSTREAMHEADER_STATUSF_DONE)));
	CHECK_EQ(acmStreamUnprepareHeader(has, &h, 0), ACMERR_UNPREPARED);
	CHECK_EQ(acmStreamClose(has, 0), 0);
	CHECK_EQ(acmStreamClose(has, 0), MMSYSERR_INVALHANDLE);

	/* Mono to stereo, 16 bits to 8: doubled, high bytes kept. */
	src = pcm(1, 8000, 16);
	dst = pcm(2, 8000, 8);
	CHECK_EQ(acmStreamOpen(&has, NULL, &src, &dst, NULL, 0, 0, 0), 0);
	short mono[2] = { 0x1234, -0x5600 };
	BYTE stereo[4];
	ACMSTREAMHEADER h2 = { .cbStruct = sizeof(h2) };
	h2.pbSrc = (BYTE *)mono;
	h2.cbSrcLength = sizeof(mono);
	h2.pbDst = stereo;
	h2.cbDstLength = sizeof(stereo);
	CHECK_EQ(acmStreamPrepareHeader(has, &h2, 0), 0);
	CHECK_EQ(acmStreamConvert(has, &h2, ACM_STREAMCONVERTF_BLOCKALIGN), 0);
	CHECK(stereo[0] == 0x92 && stereo[1] == 0x92 && stereo[2] == 0x2a &&
	      stereo[3] == 0x2a);
	CHECK_EQ(acmStreamUnprepareHeader(has, &h2, 0), 0);
	CHECK_EQ(acmStreamClose(has, 0), 0);
	CHECK_EQ(msacm_stream_count(), 0);
	CHECK_EQ(drv_open_count(), before);
}

static void test_choose(void)
{
	FN(MMRESULT, acmFormatChooseW, LPACMFORMATCHOOSEW);
	FN(MMRESULT, acmFormatChooseA, LPACMFORMATCHOOSEA);
	WAVEFORMATEX wf = pcm(2, 44100, 16);
	ACMFORMATCHOOSEW c = { .cbStruct = sizeof(c) };

	c.pwfx = &wf;
	c.cbwfx = sizeof(wf);
	unsetenv("VFW_ACM_FORMAT");
	CHECK_EQ(acmFormatChooseW(&c), ACMERR_CANCELED);
	c.fdwStyle = ACMFORMATCHOOSE_STYLEF_INITTOWFXSTRUCT;
	CHECK_EQ(acmFormatChooseW(&c), 0);
	CHECK(wis(c.szFormat, "44.100 kHz, 16 Bit, Stereo"));
	CHECK(wis(c.szFormatTag, "PCM"));
	setenv("VFW_ACM_FORMAT", "1,22050,1,8", 1);
	CHECK_EQ(acmFormatChooseW(&c), 0);
	CHECK(wf.nSamplesPerSec == 22050 && wf.nChannels == 1 &&
	      wf.wBitsPerSample == 8);
	setenv("VFW_ACM_FORMAT", "0x1,96000", 1);
	ACMFORMATCHOOSEA ca = { .cbStruct = sizeof(ca) };
	ca.pwfx = &wf;
	ca.cbwfx = sizeof(wf);
	CHECK_EQ(acmFormatChooseA(&ca), 0);
	CHECK(wf.nSamplesPerSec == 96000 && wf.nChannels == 1 &&
	      wf.wBitsPerSample == 8);
	CHECK_STR(ca.szFormat, "96.000 kHz, 8 Bit, Mono");
	CHECK_STR(ca.szFormatTag, "PCM");
	setenv("VFW_ACM_FORMAT", "0x55", 1);	/* nothing makes MP3 */
	CHECK_EQ(acmFormatChooseW(&c), ACMERR_CANCELED);
	setenv("VFW_ACM_FORMAT", "pcm", 1);
	CHECK_EQ(acmFormatChooseW(&c), ACMERR_CANCELED);
	setenv("VFW_ACM_FORMAT", "cancel", 1);
	CHECK_EQ(acmFormatChooseW(&c), ACMERR_CANCELED);
	unsetenv("VFW_ACM_FORMAT");
	c.cbStruct = 4;
	CHECK_EQ(acmFormatChooseW(&c), MMSYSERR_INVALPARAM);
}

/* The same, from inside a DLL that imports msacm32. */
static void test_fixture(void)
{
	const char *dir = getenv("VFW_FIXTURES");
	char path[4200];

	snprintf(path, sizeof(path), "%s/pe_acm.dll", dir ? dir : ".");
	if (access(path, R_OK)) {
		printf("test_msacm32: pe_acm.dll not built, skipping it\n");
		return;
	}
	struct module *m = mod_load(path, NULL);
	CHECK(m != NULL);
	if (!m)
		return;
	int (*selftest)(void) = mod_get_proc(m, "acm_selftest");
	CHECK(selftest != NULL);
	if (selftest)
		CHECK_EQ(selftest(), 0);
	mod_free(m);

	/* A VfW codec is a driver, but not an ACM one. */
	snprintf(path, sizeof(path), "%s/testcodec.dll", dir ? dir : ".");
	HACMDRIVERID id = (HACMDRIVERID)1;
	CHECK_EQ(msacm_add_path(path, &id), MMSYSERR_ERROR);
	CHECK(id == NULL);
	CHECK(strstr(vfw_error(), "not an ACM driver") != NULL);
	CHECK_EQ(msacm_add_path("/nonexistent/x.acm", &id), MMSYSERR_ERROR);
}

static void test_names(void)
{
	char buf[24];

	CHECK_STR(msacm_error_name(MMSYSERR_NOERROR, buf), "MMSYSERR_NOERROR");
	CHECK_STR(msacm_error_name(MMSYSERR_INVALFLAG, buf),
		  "MMSYSERR_INVALFLAG");
	CHECK_STR(msacm_error_name(ACMERR_NOTPOSSIBLE, buf),
		  "ACMERR_NOTPOSSIBLE");
	CHECK_STR(msacm_error_name(ACMERR_CANCELED, buf), "ACMERR_CANCELED");
	CHECK_STR(msacm_error_name(999, buf), "error 999");
	CHECK_STR(drv_msg_name(ACMDM_STREAM_CONVERT, buf),
		  "ACMDM_STREAM_CONVERT");
}

int main(void)
{
	const char *t = getenv("TMPDIR");

	snprintf(root, sizeof(root), "%s/vfw-acm-XXXXXX", t && *t ? t : "/tmp");
	if (!mkdtemp(root)) {
		perror("mkdtemp");
		return 1;
	}
	setenv("VFW_ROOT", root, 1);
	unsetenv("VFW_INI");
	setenv("VFW_REGISTRY", "", 1);
	CHECK_EQ(lowmem_reserve(), 0);
	CHECK_EQ(runtime_enter(), 0);
	acm = mod_load("msacm32.dll", NULL);
	CHECK(acm && acm->builtin);
	if (!acm)
		return test_summary("test_msacm32");

	test_drivers();
	test_function_driver();
	test_formats();
	test_streams();
	test_choose();
	test_fixture();
	test_names();

	mod_free(acm);
	char cmd[4200];
	snprintf(cmd, sizeof(cmd), "rm -rf '%s'", root);
	if (system(cmd))
		fprintf(stderr, "test_msacm32: can't remove %s\n", root);
	return test_summary("test_msacm32");
}
