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
#include <sys/wait.h>
#include <unistd.h>

#include "acm/acm.h"
#include "common/error.h"
#include "common/sanitizer.h"
#include "loader/modules.h"
#include "runtime/crash.h"
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

/* testacm.acm, the fixtures' TSTA driver. */

#define FRAMES 2500		/* two whole packets and a short one */
#define PCM_BYTES (FRAMES * 4)
#define TSTA_BYTES (PCM_BYTES + 3 * 8)

static char testacm[4200];

static int have_testacm(void)
{
	const char *dir = getenv("VFW_FIXTURES");

	snprintf(testacm, sizeof(testacm), "%s/testacm.acm", dir ? dir : ".");
	return !access(testacm, R_OK);
}

/* A count the driver keeps (testacm_open_count, testacm_stream_count). */
static LONG driver_count(const char *name)
{
	struct module *m = mod_load(testacm, NULL);
	LONG (*fn)(void) = m ? mod_get_proc(m, name) : NULL;
	LONG n = fn ? fn() : -1;

	if (m)
		mod_free(m);
	return n;
}

/*
 * Converts all of in through has as a host must: in pieces of chunk
 * bytes, whole blocks at a time, what is left given again with the next
 * piece, and the end flushed. Appends to out; returns the bytes made, or
 * -1 if a call failed or the end left something unconverted.
 */
static long run_stream(HACMSTREAM has, const BYTE *in, size_t n, size_t chunk,
		       BYTE *out, size_t cap)
{
	FN(MMRESULT, acmStreamPrepareHeader, HACMSTREAM, LPACMSTREAMHEADER, DWORD);
	FN(MMRESULT, acmStreamUnprepareHeader, HACMSTREAM, LPACMSTREAMHEADER,
	   DWORD);
	FN(MMRESULT, acmStreamConvert, HACMSTREAM, LPACMSTREAMHEADER, DWORD);
	static BYTE pending[1 << 16], dst[1 << 17];
	size_t pos = 0, held = 0, made = 0;
	int first = 1;

	for (;;) {
		size_t take = n - pos < chunk ? n - pos : chunk;
		memcpy(pending + held, in + pos, take);
		held += take;
		pos += take;
		int end = pos == n;
		ACMSTREAMHEADER h = { .cbStruct = sizeof(h) };
		h.pbSrc = pending;
		h.cbSrcLength = (DWORD)held;
		h.pbDst = dst;
		h.cbDstLength = sizeof(dst);
		if (acmStreamPrepareHeader(has, &h, 0))
			return -1;
		MMRESULT r = acmStreamConvert(has, &h,
			ACM_STREAMCONVERTF_BLOCKALIGN |
			(first ? ACM_STREAMCONVERTF_START : 0) |
			(end ? ACM_STREAMCONVERTF_END : 0));
		if (acmStreamUnprepareHeader(has, &h, 0) || r ||
		    made + h.cbDstLengthUsed > cap)
			return -1;
		first = 0;
		memcpy(out + made, dst, h.cbDstLengthUsed);
		made += h.cbDstLengthUsed;
		memmove(pending, pending + h.cbSrcLengthUsed,
			held - h.cbSrcLengthUsed);
		held -= h.cbSrcLengthUsed;
		if (end)
			return held ? -1 : (long)made;
	}
}

/* PCM through testacm to TSTA and back, byte for byte. */
static void roundtrip(DWORD open_flags)
{
	FN(MMRESULT, acmFormatSuggest, HACMDRIVER, LPWAVEFORMATEX,
	   LPWAVEFORMATEX, DWORD, DWORD);
	FN(MMRESULT, acmStreamOpen, LPHACMSTREAM, HACMDRIVER, LPWAVEFORMATEX,
	   LPWAVEFORMATEX, LPWAVEFILTER, DWORD_PTR, DWORD_PTR, DWORD);
	FN(MMRESULT, acmStreamClose, HACMSTREAM, DWORD);
	static BYTE in[PCM_BYTES], packets[TSTA_BYTES + 64], back[PCM_BYTES + 64];
	BYTE tsta_buf[64] = { 0 };
	WAVEFORMATEX *tsta = (WAVEFORMATEX *)tsta_buf;
	WAVEFORMATEX wf = pcm(2, 44100, 16);
	HACMDRIVERID id;
	HACMSTREAM has = NULL;
	unsigned before = drv_open_count();

	for (size_t i = 0; i < sizeof(in); i++)
		in[i] = (BYTE)(i * 7 + i / 13);
	CHECK_EQ(msacm_add_path(testacm, &id), 0);

	/* Its format for this PCM, as it suggests it. */
	tsta->wFormatTag = 0x7354;
	CHECK_EQ(acmFormatSuggest(NULL, &wf, tsta, sizeof(tsta_buf),
				  ACM_FORMATSUGGESTF_WFORMATTAG), 0);
	CHECK(tsta->nBlockAlign == 8 + 1024 * 4 && tsta->cbSize == 2 &&
	      tsta_buf[18] == 0x00 && tsta_buf[19] == 0x04 &&
	      tsta->nChannels == 2 && tsta->nSamplesPerSec == 44100);

	CHECK_EQ(acmStreamOpen(&has, NULL, &wf, tsta, NULL, 0, 0, open_flags), 0);
	long n = run_stream(has, in, sizeof(in), 2800, packets, sizeof(packets));
	CHECK_EQ(n, TSTA_BYTES);
	CHECK_EQ(acmStreamClose(has, 0), 0);
	/* Three packets: 1024, 1024 and 452 frames. */
	if (n == TSTA_BYTES) {
		CHECK(packets[0] == 'T' && packets[1] == 'A' && packets[2] == 16 &&
		      packets[3] == 2 && packets[4] == 0 && packets[5] == 4);
		CHECK(packets[4104] == 'T' && packets[4104 + 5] == 4);
		CHECK(packets[8208] == 'T' && packets[8208 + 4] == (452 & 0xff) &&
		      packets[8208 + 5] == 452 >> 8);
	}

	CHECK_EQ(acmStreamOpen(&has, NULL, tsta, &wf, NULL, 0, 0, open_flags), 0);
	n = run_stream(has, packets, TSTA_BYTES, 3001, back, sizeof(back));
	CHECK_EQ(n, PCM_BYTES);
	CHECK(n == PCM_BYTES && !memcmp(in, back, PCM_BYTES));
	CHECK_EQ(acmStreamClose(has, 0), 0);

	CHECK_EQ(msacm_stream_count(), 0);
	CHECK_EQ(driver_count("testacm_stream_count"), 0);
	CHECK_EQ(drv_open_count(), before);
}

/* What a stream through the backend (acm.h) hands on, end to end. */
struct collected {
	BYTE data[16384];
	size_t size, outputs;
};

static HRESULT collect_output(void *opaque, const struct acm_output *o)
{
	struct collected *c = opaque;

	if (c->size + o->size > sizeof(c->data))
		return E_OUTOFMEMORY;
	memcpy(c->data + c->size, o->data, o->size);
	c->size += o->size;
	c->outputs++;
	return S_OK;
}

/*
 * 1500 sample frames through TSTA and back by the backend: exact, as the
 * driver makes a short packet at the end; or, when it makes only whole
 * ones (whole), padded with silence to two packets, none of it lost.
 */
static void backend_roundtrip(int whole)
{
	struct acm_codec *c = NULL;
	struct acm_stream *enc = NULL, *dec = NULL;
	static struct collected packets, back;
	WAVEFORMATEX in = pcm(1, 8000, 16), tag = { .wFormatTag = 0x7354 };
	short ramp[1500];
	size_t size;

	memset(&packets, 0, sizeof(packets));
	memset(&back, 0, sizeof(back));
	for (int i = 0; i < 1500; i++)
		ramp[i] = (short)(i * 7 - 5000);
	CHECK_EQ(acm_codec_open(testacm, &c), S_OK);
	if (!c)
		return;
	CHECK_EQ(acm_stream_open(c, &in, sizeof(in), &tag, 0, 0, collect_output,
				 &packets, &enc), S_OK);
	if (enc) {
		/* Pushed in two parts, the first not a whole packet. */
		CHECK_EQ(acm_stream_push(enc, ramp, 1000, 0), S_OK);
		CHECK_EQ(acm_stream_push(enc, (BYTE *)ramp + 1000, 2000, 0),
			 S_OK);
		CHECK_EQ(acm_stream_end(enc), S_OK);
		CHECK_EQ(packets.outputs, 2);
		CHECK_EQ(packets.size, 8 + 1024 * 2 +
			 8 + (whole ? 1024 : 476) * 2);
		const WAVEFORMATEX *wf = acm_stream_format(enc, &size);
		CHECK_EQ(acm_stream_open(c, wf, size, &in, sizeof(in), 0,
					 collect_output, &back, &dec), S_OK);
		acm_stream_close(enc);
	}
	if (dec) {
		CHECK_EQ(acm_stream_push(dec, packets.data, packets.size, 0),
			 S_OK);
		CHECK_EQ(acm_stream_end(dec), S_OK);
		CHECK_EQ(back.size, (whole ? 2048 : 1500) * 2);
		CHECK(!memcmp(back.data, ramp, sizeof(ramp)));
		int silent = 1;
		for (size_t i = sizeof(ramp); i < back.size; i++)
			silent &= !back.data[i];
		CHECK(silent);
		acm_stream_close(dec);
	}
	acm_codec_close(c);
}

static void test_testacm(void)
{
	FN(MMRESULT, acmMetrics, HACMOBJ, UINT, LPVOID);
	FN(MMRESULT, acmDriverDetailsW, HACMDRIVERID, LPACMDRIVERDETAILSW, DWORD);
	FN(MMRESULT, acmDriverOpen, LPHACMDRIVER, HACMDRIVERID, DWORD);
	FN(MMRESULT, acmDriverClose, HACMDRIVER, DWORD);
	FN(LRESULT, acmDriverMessage, HACMDRIVER, UINT, LPARAM, LPARAM);
	FN(MMRESULT, acmFormatTagEnumW, HACMDRIVER, LPACMFORMATTAGDETAILSW,
	   ACMFORMATTAGENUMCBW, DWORD_PTR, DWORD);
	FN(MMRESULT, acmFormatEnumW, HACMDRIVER, LPACMFORMATDETAILSW,
	   ACMFORMATENUMCBW, DWORD_PTR, DWORD);
	FN(MMRESULT, acmFormatDetailsW, HACMDRIVER, LPACMFORMATDETAILSW, DWORD);
	FN(MMRESULT, acmStreamOpen, LPHACMSTREAM, HACMDRIVER, LPWAVEFORMATEX,
	   LPWAVEFORMATEX, LPWAVEFILTER, DWORD_PTR, DWORD_PTR, DWORD);
	FN(MMRESULT, acmStreamClose, HACMSTREAM, DWORD);
	FN(MMRESULT, acmStreamSize, HACMSTREAM, DWORD, LPDWORD, DWORD);
	char line[4400];
	DWORD v;

	if (!have_testacm()) {
		printf("test_msacm32: testacm.acm not built, skipping it\n");
		return;
	}
	unsigned before = drv_open_count();

	/* Named in vfw.ini, it is one of the drivers. */
	snprintf(line, sizeof(line), "[drivers32]\nmsacm.tsta=%s\n", testacm);
	write_ini(line);
	CHECK_EQ(acmMetrics(NULL, ACM_METRIC_COUNT_CODECS, &v), 0);
	CHECK_EQ(v, 1);
	HACMDRIVERID id = NULL, again = NULL;
	CHECK_EQ(msacm_add_path(testacm, &id), 0);	/* that one */
	CHECK_EQ(acmMetrics(NULL, ACM_METRIC_COUNT_LOCAL_CODECS, &v), 0);
	CHECK_EQ(v, 0);
	CHECK_EQ(msacm_add_path(testacm, &again), 0);
	CHECK(id && again == id);
	ACMDRIVERDETAILSW d = { .cbStruct = sizeof(d) };
	CHECK_EQ(acmDriverDetailsW(id, &d, 0), 0);
	CHECK(wis(d.szShortName, "TSTA") && d.cFormatTags == 2 &&
	      d.fdwSupport == ACMDRIVERDETAILS_SUPPORTF_CODEC);
	CHECK_EQ(acmMetrics(NULL, ACM_METRIC_MAX_SIZE_FORMAT, &v), 0);
	CHECK_EQ(v, sizeof(WAVEFORMATEX) + 2);

	/* Its tags and formats. */
	HACMDRIVER had;
	CHECK_EQ(acmDriverOpen(&had, id, 0), 0);
	int tags = 0;
	ACMFORMATTAGDETAILSW t = { .cbStruct = sizeof(t) };
	CHECK_EQ(acmFormatTagEnumW(had, &t, count_tags, (DWORD_PTR)&tags, 0), 0);
	CHECK_EQ(tags, 2);
	BYTE buf[64];
	WAVEFORMATEX *wf = (WAVEFORMATEX *)buf;
	ACMFORMATDETAILSW f = { .cbStruct = sizeof(f) };
	f.pwfx = wf;
	f.cbwfx = sizeof(buf);
	memset(buf, 0, sizeof(buf));
	wf->wFormatTag = 0x7354;
	struct counted c = { 0 };
	CHECK_EQ(acmFormatEnumW(had, &f, count_formats, (DWORD_PTR)&c,
				ACM_FORMATENUMF_WFORMATTAG), 0);
	CHECK(c.n == 45 && c.mono == 15 && c.at_44100 == 9);
	CHECK_STR(c.first, "8.000 kHz, 8 Bit, Mono");
	/* What 16-bit stereo at 44100 Hz becomes: its one TSTA format. */
	memset(&c, 0, sizeof(c));
	*wf = pcm(2, 44100, 16);
	CHECK_EQ(acmFormatEnumW(had, &f, count_formats, (DWORD_PTR)&c,
				ACM_FORMATENUMF_CONVERT), 0);
	CHECK_EQ(c.n, 1);
	/* pwfx holds the last format tried, not that one: ask for it. */
	BYTE suggested[64] = { 0 };
	((WAVEFORMATEX *)suggested)->wFormatTag = 0x7354;
	WAVEFORMATEX s16 = pcm(2, 44100, 16), s8 = pcm(2, 44100, 8);
	CHECK_EQ(msacm_acmFormatSuggest(had, &s16, (WAVEFORMATEX *)suggested,
					sizeof(suggested),
					ACM_FORMATSUGGESTF_WFORMATTAG), 0);
	memcpy(buf, suggested, sizeof(buf));
	f.dwFormatTag = 0x7354;
	CHECK_EQ(acmFormatDetailsW(NULL, &f, ACM_FORMATDETAILSF_FORMAT), 0);
	CHECK(wis(f.szFormat, "44.100 kHz, 16 Bit, Stereo"));
	/* Not configurable, in this mode. */
	CHECK_EQ(acmDriverMessage(had, DRV_QUERYCONFIGURE, 0, 0), 0);
	CHECK_EQ(acmDriverMessage(had, ACMDM_DRIVER_ABOUT, -1, 0),
		 MMSYSERR_NOTSUPPORTED);
	CHECK_EQ(acmDriverMessage(had, ACMDM_STREAM_OPEN, 0, 0),
		 MMSYSERR_INVALPARAM);	/* not for applications */

	/* What its streams need for a given size, each way. */
	BYTE tsta_buf[64] = { 0 };
	memcpy(tsta_buf, suggested, sizeof(WAVEFORMATEX) + 2);
	HACMSTREAM has;
	CHECK_EQ(acmStreamOpen(&has, had, &s16, (WAVEFORMATEX *)tsta_buf, NULL,
			       0, 0, 0), 0);
	CHECK_EQ(acmStreamSize(has, PCM_BYTES, &v, ACM_STREAMSIZEF_SOURCE), 0);
	CHECK_EQ(v, TSTA_BYTES);
	CHECK_EQ(acmStreamSize(has, TSTA_BYTES, &v, ACM_STREAMSIZEF_DESTINATION),
		 0);
	CHECK_EQ(v, 2 * 4096);	/* whole packets */
	CHECK_EQ(acmStreamClose(has, 0), 0);
	CHECK_EQ(acmStreamOpen(&has, had, (WAVEFORMATEX *)tsta_buf, &s16, NULL,
			       0, 0, 0), 0);
	CHECK_EQ(acmStreamSize(has, TSTA_BYTES, &v, ACM_STREAMSIZEF_SOURCE), 0);
	CHECK_EQ(v, 3 * 4096);
	CHECK_EQ(acmStreamSize(has, PCM_BYTES, &v, ACM_STREAMSIZEF_DESTINATION),
		 0);
	CHECK_EQ(v, 2 * 4104);
	CHECK_EQ(acmStreamSize(has, 4, &v, ACM_STREAMSIZEF_SOURCE),
		 ACMERR_NOTPOSSIBLE);
	CHECK_EQ(acmStreamClose(has, 0), 0);
	/* It doesn't change bits, rate or channels. */
	CHECK_EQ(acmStreamOpen(NULL, had, (WAVEFORMATEX *)tsta_buf, &s8, NULL,
			       0, 0, ACM_STREAMOPENF_QUERY), ACMERR_NOTPOSSIBLE);
	CHECK_EQ(acmStreamOpen(NULL, had, &s16, &s8, NULL, 0, 0,
			       ACM_STREAMOPENF_QUERY), ACMERR_NOTPOSSIBLE);
	CHECK_EQ(acmDriverClose(had, 0), 0);

	roundtrip(0);
	backend_roundtrip(0);
	WORD tag = 0;
	CHECK_EQ(acm_find(testacm, 1, 0, &tag), S_OK);
	CHECK_EQ(tag, 0x7354);
	CHECK_EQ(driver_count("testacm_open_count"), 0);
	CHECK_EQ(drv_open_count(), before);
	write_ini("");
}

/* Fuzzed formats. */

static unsigned fuzz_state = 20260;

static unsigned fuzz(unsigned n)
{
	fuzz_state = fuzz_state * 1103515245u + 12345u;
	return (fuzz_state >> 8) % n;
}

/* One of the values, or now and then anything. */
static DWORD fuzz_pick(const DWORD *values, size_t n)
{
	return fuzz(8) ? values[fuzz((unsigned)n)] :
	       fuzz(0x10000) | (DWORD)fuzz(0x10000) << 16;
}
#define PICK(...) fuzz_pick((const DWORD[]){ __VA_ARGS__ }, \
	sizeof((const DWORD[]){ __VA_ARGS__ }) / sizeof(DWORD))

/*
 * A format of a tag the drivers know or don't: half of them sound, then
 * now and then one field broken; the rest anything. *size is what the
 * buffer holds (its cbSize's worth).
 */
static WAVEFORMATEX *fuzz_format(size_t *size)
{
	int sound = fuzz(2);
	WORD tag = sound ? (fuzz(2) ? WAVE_FORMAT_PCM : 0x7354) :
		   (WORD)PICK(WAVE_FORMAT_PCM, 0x7354, WAVE_FORMAT_IEEE_FLOAT,
			      WAVE_FORMAT_EXTENSIBLE, 0x0055, 0, 0xffff);
	WORD cb = fuzz(4) ? (tag == 0x7354 ? 2 : 0) : (WORD)fuzz(80);
	WAVEFORMATEX *wf = calloc(1, sizeof(*wf) + cb);

	*size = sizeof(*wf) + cb;
	if (!wf)
		return NULL;
	wf->wFormatTag = tag;
	if (sound) {
		static const DWORD rates[] = { 8000, 11025, 22050, 44100, 48000 };
		static const WORD channels[] = { 1, 2, 6 }, bits[] = { 8, 16, 24 };
		wf->nChannels = channels[fuzz(3)];
		wf->nSamplesPerSec = rates[fuzz(5)];
		wf->wBitsPerSample = bits[fuzz(3)];
	} else {
		wf->nChannels = (WORD)PICK(1, 2, 6, 0, 8, 255, 0xffff);
		wf->nSamplesPerSec = PICK(8000, 44100, 48000, 0, 1, 0xffffffffu);
		wf->wBitsPerSample = (WORD)PICK(8, 16, 24, 0, 1, 32, 33, 0xffff);
	}
	wf->nBlockAlign = (WORD)(wf->nChannels * (wf->wBitsPerSample / 8));
	if (tag == 0x7354)
		wf->nBlockAlign = (WORD)(8 + 1024 * wf->nBlockAlign);
	wf->nAvgBytesPerSec = wf->nSamplesPerSec * wf->nBlockAlign;
	wf->cbSize = cb;
	for (WORD i = 0; i < cb; i++)
		((BYTE *)(wf + 1))[i] = (BYTE)fuzz(256);
	if (tag == 0x7354 && cb >= 2 && (sound || fuzz(2))) {
		((BYTE *)(wf + 1))[0] = 0;
		((BYTE *)(wf + 1))[1] = 4;	/* TSTA's version */
	}
	if (!fuzz(4)) {
		/* A field broken. */
		DWORD v = PICK(0, 1, 2, 0xffff, 0xffffffffu);
		switch (fuzz(5)) {
		case 0: wf->nChannels = (WORD)v; break;
		case 1: wf->nSamplesPerSec = v; break;
		case 2: wf->wBitsPerSample = (WORD)v; break;
		case 3: wf->nBlockAlign = (WORD)v; break;
		default: wf->nAvgBytesPerSec = v; break;
		}
	}
	return wf;
}

/*
 * Nonsense formats through every call that takes one, and data through
 * any stream that opens, the testacm driver's and the PCM converter's
 * (and the backend's): nothing may crash or overrun (the sanitizer build
 * checks that), or claim more than it was given; errors are fine.
 */
static void test_fuzz(void)
{
	FN(MMRESULT, acmFormatSuggest, HACMDRIVER, LPWAVEFORMATEX,
	   LPWAVEFORMATEX, DWORD, DWORD);
	FN(MMRESULT, acmFormatDetailsW, HACMDRIVER, LPACMFORMATDETAILSW, DWORD);
	FN(MMRESULT, acmStreamOpen, LPHACMSTREAM, HACMDRIVER, LPWAVEFORMATEX,
	   LPWAVEFORMATEX, LPWAVEFILTER, DWORD_PTR, DWORD_PTR, DWORD);
	FN(MMRESULT, acmStreamClose, HACMSTREAM, DWORD);
	FN(MMRESULT, acmStreamSize, HACMSTREAM, DWORD, LPDWORD, DWORD);
	FN(MMRESULT, acmStreamPrepareHeader, HACMSTREAM, LPACMSTREAMHEADER, DWORD);
	FN(MMRESULT, acmStreamUnprepareHeader, HACMSTREAM, LPACMSTREAMHEADER,
	   DWORD);
	FN(MMRESULT, acmStreamConvert, HACMSTREAM, LPACMSTREAMHEADER, DWORD);
	static BYTE src_data[9000], dst_data[20000];
	HACMDRIVERID id;
	struct acm_codec *codec = NULL;
	int suggested = 0, opened = 0, converted = 0, backend = 0;

	if (!have_testacm())
		return;
	write_ini("");
	CHECK_EQ(msacm_add_path(testacm, &id), 0);
	CHECK_EQ(acm_codec_open(testacm, &codec), S_OK);
	for (size_t i = 0; i < sizeof(src_data); i++)
		src_data[i] = (BYTE)fuzz(256);

	for (int iter = 0; iter < 3000; iter++) {
		size_t size, size2;
		WAVEFORMATEX *wf = fuzz_format(&size), *wf2 = fuzz_format(&size2);
		BYTE dst_buf[256];
		if (!wf || !wf2) {
			free(wf);
			free(wf2);
			continue;
		}

		/* A suggestion fills no more than its buffer, sensibly. */
		DWORD room = 18 + fuzz(sizeof(dst_buf) - 18);
		WAVEFORMATEX *to = wf2;
		size_t to_size = size2;
		memset(dst_buf, 0, sizeof(dst_buf));
		((WAVEFORMATEX *)dst_buf)->wFormatTag = wf2->wFormatTag;
		if (!acmFormatSuggest(NULL, wf, (WAVEFORMATEX *)dst_buf, room,
				      fuzz(2) ? ACM_FORMATSUGGESTF_WFORMATTAG :
				      0)) {
			suggested++;
			/* Mostly, where it suggests: streams open then. */
			if (fuzz(4)) {
				to = (WAVEFORMATEX *)dst_buf;
				to_size = room;
			}
			CHECK(sizeof(WAVEFORMATEX) +
			      ((WAVEFORMATEX *)dst_buf)->cbSize <= room ||
			      ((WAVEFORMATEX *)dst_buf)->wFormatTag ==
			      WAVE_FORMAT_PCM);
		}

		ACMFORMATDETAILSW d = { .cbStruct = sizeof(d) };
		d.dwFormatTag = wf->wFormatTag;
		d.pwfx = wf;
		d.cbwfx = (DWORD)size;
		acmFormatDetailsW(NULL, &d, ACM_FORMATDETAILSF_FORMAT);

		HACMSTREAM has = NULL;
		DWORD flags = fuzz(2) ? ACM_STREAMOPENF_NONREALTIME : 0;
		if (!fuzz(4))
			flags |= ACM_STREAMOPENF_QUERY;
		MMRESULT r = acmStreamOpen(flags & ACM_STREAMOPENF_QUERY ?
					   NULL : &has, NULL, wf, to, NULL, 0,
					   0, flags);
		if (!r && has) {
			opened++;
			DWORD v = 0;
			acmStreamSize(has, fuzz(10000), &v,
				      fuzz(2) ? ACM_STREAMSIZEF_SOURCE :
				      ACM_STREAMSIZEF_DESTINATION);
			for (int k = 0; k < 3; k++) {
				ACMSTREAMHEADER h = { .cbStruct = sizeof(h) };
				h.pbSrc = src_data;
				h.cbSrcLength = fuzz(sizeof(src_data));
				h.pbDst = dst_data;
				h.cbDstLength = fuzz(sizeof(dst_data));
				if (acmStreamPrepareHeader(has, &h, 0))
					continue;
				DWORD cf = (fuzz(2) ? ACM_STREAMCONVERTF_BLOCKALIGN :
					    0) | (!k ? ACM_STREAMCONVERTF_START :
					    0) | (k == 2 ? ACM_STREAMCONVERTF_END : 0);
				if (!acmStreamConvert(has, &h, cf)) {
					converted++;
					CHECK(h.cbSrcLengthUsed <= h.cbSrcLength);
					CHECK(h.cbDstLengthUsed <= h.cbDstLength);
				}
				CHECK_EQ(acmStreamUnprepareHeader(has, &h, 0), 0);
			}
			CHECK_EQ(acmStreamClose(has, 0), 0);
		}

		/* The backend, from the format to the other's tag. */
		struct acm_stream *st = NULL;
		if (codec && !fuzz(3) &&
		    acm_stream_open(codec, wf, size, to, fuzz(2) ? to_size : 0,
				    fuzz(2) ? 0 : fuzz(400000), NULL, NULL,
				    &st) == S_OK) {
			backend++;
			acm_stream_push(st, src_data, fuzz(sizeof(src_data)), 0);
			acm_stream_end(st);
			acm_stream_close(st);
		}
		free(wf);
		free(wf2);
	}
	acm_codec_close(codec);
	/* Enough of them made sense to go somewhere. */
	CHECK(suggested > 20 && opened > 20 && converted > 20 && backend > 5);
	printf("test_msacm32: fuzz: %d suggested, %d opened, %d converted, "
	       "%d through the backend\n", suggested, opened, converted,
	       backend);
	CHECK_EQ(msacm_stream_count(), 0);
	CHECK_EQ(driver_count("testacm_stream_count"), 0);
}

/* The driver's modes, each in a child process with it set. */

static void mode_strict(void)
{
	/* Sanitizer builds don't keep memory below 2 GB (lowmem.c). */
	if (!lowmem_active()) {
		printf("test_msacm32: no low-memory reservation, skipping "
		       "strict\n");
		return;
	}
	roundtrip(0);
}

static void mode_notrealtime(void)
{
	FN(MMRESULT, acmStreamOpen, LPHACMSTREAM, HACMDRIVER, LPWAVEFORMATEX,
	   LPWAVEFORMATEX, LPWAVEFILTER, DWORD_PTR, DWORD_PTR, DWORD);
	BYTE tsta_buf[20] = { 0 };
	WAVEFORMATEX wf = pcm(1, 8000, 8);
	HACMDRIVERID id;

	CHECK_EQ(msacm_add_path(testacm, &id), 0);
	memcpy(tsta_buf, &wf, sizeof(wf));
	WAVEFORMATEX *tsta = (WAVEFORMATEX *)tsta_buf;
	tsta->wFormatTag = 0x7354;
	tsta->nBlockAlign = 8 + 1024;
	tsta->cbSize = 2;
	tsta_buf[19] = 4;
	CHECK_EQ(acmStreamOpen(NULL, NULL, &wf, tsta, NULL, 0, 0,
			       ACM_STREAMOPENF_QUERY), ACMERR_NOTPOSSIBLE);
	CHECK_EQ(acmStreamOpen(NULL, NULL, &wf, tsta, NULL, 0, 0,
			       ACM_STREAMOPENF_QUERY |
			       ACM_STREAMOPENF_NONREALTIME), 0);
	roundtrip(ACM_STREAMOPENF_NONREALTIME);
}

static void mode_small(void)
{
	FN(MMRESULT, acmStreamOpen, LPHACMSTREAM, HACMDRIVER, LPWAVEFORMATEX,
	   LPWAVEFORMATEX, LPWAVEFILTER, DWORD_PTR, DWORD_PTR, DWORD);
	FN(MMRESULT, acmStreamSize, HACMSTREAM, DWORD, LPDWORD, DWORD);
	FN(MMRESULT, acmStreamClose, HACMSTREAM, DWORD);
	FN(MMRESULT, acmFormatSuggest, HACMDRIVER, LPWAVEFORMATEX,
	   LPWAVEFORMATEX, DWORD, DWORD);
	BYTE tsta_buf[64] = { 0 };
	WAVEFORMATEX wf = pcm(2, 44100, 16);
	HACMDRIVERID id;
	HACMSTREAM has;
	DWORD v = 0;

	CHECK_EQ(msacm_add_path(testacm, &id), 0);
	((WAVEFORMATEX *)tsta_buf)->wFormatTag = 0x7354;
	CHECK_EQ(acmFormatSuggest(NULL, &wf, (WAVEFORMATEX *)tsta_buf,
				  sizeof(tsta_buf),
				  ACM_FORMATSUGGESTF_WFORMATTAG), 0);
	CHECK_EQ(acmStreamOpen(&has, NULL, (WAVEFORMATEX *)tsta_buf, &wf, NULL,
			       0, 0, 0), 0);
	/* Passed on as the driver says, too small as it is. */
	CHECK_EQ(acmStreamSize(has, 4104, &v, ACM_STREAMSIZEF_SOURCE), 0);
	CHECK_EQ(v, 1);
	CHECK_EQ(acmStreamClose(has, 0), 0);
	roundtrip(0);
}

static void mode_prepare(void)
{
	roundtrip(0);
}

static void mode_config(void)
{
	FN(MMRESULT, acmDriverOpen, LPHACMDRIVER, HACMDRIVERID, DWORD);
	FN(MMRESULT, acmDriverClose, HACMDRIVER, DWORD);
	FN(LRESULT, acmDriverMessage, HACMDRIVER, UINT, LPARAM, LPARAM);
	HACMDRIVERID id;
	HACMDRIVER had;

	CHECK_EQ(msacm_add_path(testacm, &id), 0);
	CHECK_EQ(acmDriverOpen(&had, id, 0), 0);
	CHECK_EQ(acmDriverMessage(had, DRV_QUERYCONFIGURE, 0, 0), 1);
	CHECK_EQ(acmDriverMessage(had, DRV_CONFIGURE, 0, 0), DRVCNF_OK);
	CHECK_EQ(acmDriverClose(had, 0), 0);
	CHECK_EQ(driver_count("testacm_open_count"), 0);
}

static void mode_whole(void)
{
	backend_roundtrip(1);
}

/* It suggests TSTA for PCM, but the stream decides: it only decodes. */
static void mode_noencode(void)
{
	FN(MMRESULT, acmFormatSuggest, HACMDRIVER, LPWAVEFORMATEX,
	   LPWAVEFORMATEX, DWORD, DWORD);
	BYTE tsta_buf[64] = { 0 };
	WAVEFORMATEX wf = pcm(2, 44100, 16);
	HACMDRIVERID id;
	WORD tag = 1;

	CHECK_EQ(msacm_add_path(testacm, &id), 0);
	((WAVEFORMATEX *)tsta_buf)->wFormatTag = 0x7354;
	CHECK_EQ(acmFormatSuggest(NULL, &wf, (WAVEFORMATEX *)tsta_buf,
				  sizeof(tsta_buf),
				  ACM_FORMATSUGGESTF_WFORMATTAG), 0);
	CHECK_EQ(acm_find(testacm, 1, 0, &tag), CLASS_E_CLASSNOTAVAILABLE);
	CHECK_EQ(tag, 0);
	CHECK_EQ(acm_find(testacm, 0, 0, &tag), S_OK);
	CHECK_EQ(tag, 0x7354);
}

/* Crashes in the driver's ACMDM_STREAM_CONVERT, for the report. */
static void mode_crash(void)
{
	roundtrip(0);
	CHECK(!"the driver should have crashed");
}

/* Hangs there, for the timeout's report. */
static void mode_hang(void)
{
	roundtrip(0);
	CHECK(!"the driver should have hung");
}

enum ending { PASSES, CRASHES, HANGS };

static const struct {
	const char *name, *variable;
	void (*run)(void);
	enum ending ending;
} modes[] = {
	{ "strict", "STRICT", mode_strict, PASSES },
	{ "notrealtime", "NOTREALTIME", mode_notrealtime, PASSES },
	{ "small", "SMALL", mode_small, PASSES },
	{ "prepare", "PREPARE", mode_prepare, PASSES },
	{ "config", "CONFIG", mode_config, PASSES },
	{ "crash", "CRASH", mode_crash, CRASHES },
	{ "hang", "HANG", mode_hang, HANGS },
	{ "whole", "WHOLE", mode_whole, PASSES },
	{ "noencode", "NOENCODE", mode_noencode, PASSES },
};
#define NMODES (sizeof(modes) / sizeof(*modes))

/* A mode, run by main below 2 GB, as the commands run. */
static int run_mode(int argc, char **argv)
{
	CHECK_EQ(runtime_enter(), 0);
	CHECK(argc == 2 && have_testacm());
	for (size_t i = 0; argc == 2 && i < NMODES; i++)
		if (!strcmp(argv[1], modes[i].name))
			modes[i].run();
	return 0;
}

static void test_modes(const char *self)
{
	char cmd[4400], out[8192];

	if (!have_testacm())
		return;
	for (size_t i = 0; i < NMODES; i++) {
		enum ending ending = modes[i].ending;
		snprintf(cmd, sizeof(cmd), "%sVFW_ENV_TESTACM_%s=1 %s'%s' %s 2>&1",
			 ending == HANGS ? "VFW_TIMEOUT=1 " : "",
			 modes[i].variable, test_rerun_prefix(), self,
			 modes[i].name);
		FILE *p = popen(cmd, "r");
		size_t n = p ? fread(out, 1, sizeof(out) - 1, p) : 0;
		out[n] = 0;
		int status = p ? pclose(p) : -1;
		int ok;
		if (ending == PASSES) {
			ok = status == 0;
		} else if (ending == CRASHES) {
			/*
			 * It dies, and the report names the driver's message;
			 * a sanitizer reports faults itself.
			 */
			ok = status != 0 && (VFW_SANITIZER_BUILD ||
			     (strstr(out, "ACMDM_STREAM_CONVERT") &&
			      strstr(out, "testacm.acm")));
		} else {
			/* The watchdog's, in any build: exit 124. */
			ok = WIFEXITED(status) && WEXITSTATUS(status) == 124 &&
			     strstr(out, "seance: timeout:") &&
			     strstr(out, "ACMDM_STREAM_CONVERT") &&
			     strstr(out, "testacm.acm");
		}
		CHECK(ok);
		if (!ok)
			fprintf(stderr, "test_msacm32: mode %s failed (status "
				"%d):\n%s", modes[i].name, status, out);
	}
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

int main(int argc, char **argv)
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

	if (argc == 2) {
		/* A mode of testacm's, run by test_modes. */
		CHECK_EQ(crash_install(), 0);
		lowmem_run(run_mode, argc, argv);
		int r = test_summary(argv[1]);
		char cmd[4200];
		snprintf(cmd, sizeof(cmd), "rm -rf '%s'", root);
		if (system(cmd))
			fprintf(stderr, "test_msacm32: can't remove %s\n", root);
		return r;
	}

	test_drivers();
	test_function_driver();
	test_formats();
	test_streams();
	test_choose();
	test_fixture();
	test_testacm();
	test_fuzz();
	test_modes(argv[0]);
	test_names();

	mod_free(acm);
	char cmd[4200];
	snprintf(cmd, sizeof(cmd), "rm -rf '%s'", root);
	if (system(cmd))
		fprintf(stderr, "test_msacm32: can't remove %s\n", root);
	return test_summary("test_msacm32");
}
