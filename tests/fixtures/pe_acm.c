/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * Uses msacm32 as an application would: acm_selftest counts the drivers
 * with acmDriverEnum (a callback into this DLL) and acmMetrics, then
 * converts 16-bit stereo to 8-bit mono through a stream, with the header
 * prepared, converted with ACM_STREAMCONVERTF_BLOCKALIGN and unprepared.
 * It returns 0, or the number of the first check that failed.
 */
#include "fixture.h"

#include <mmreg.h>
#include <msacm.h>

static void zero(void *p, unsigned n)
{
	volatile BYTE *b = p;

	while (n--)
		*b++ = 0;
}

static BOOL CALLBACK count_driver(HACMDRIVERID id, DWORD_PTR inst,
				  DWORD support)
{
	ACMDRIVERDETAILSA d;

	zero(&d, sizeof(d));
	d.cbStruct = sizeof(d);
	if (!acmDriverDetailsA(id, &d, 0) && d.fdwSupport == support)
		(*(DWORD *)inst)++;
	return TRUE;
}

EXPORT int acm_selftest(void)
{
	DWORD n = 0, m = 0, size = 0;
	WAVEFORMATEX src, dst;
	HACMSTREAM has = NULL;
	ACMSTREAMHEADER h;
	/* Two frames and a half: left and right of 0x1234 and 0x5678. */
	static const short in[5] = { 0x1234, 0x1234, 0x5678, 0x5678, 0x7fff };
	BYTE out[4] = { 0, 0, 0, 0 };

	if ((acmGetVersion() >> 24) != 4)
		return 1;
	if (acmDriverEnum(count_driver, (DWORD_PTR)&n, 0) || !n)
		return 2;
	if (acmMetrics(NULL, ACM_METRIC_COUNT_DRIVERS, &m) || m != n)
		return 3;

	zero(&src, sizeof(src));
	src.wFormatTag = WAVE_FORMAT_PCM;
	src.nChannels = 2;
	src.nSamplesPerSec = 8000;
	src.wBitsPerSample = 16;
	src.nBlockAlign = 4;
	src.nAvgBytesPerSec = 32000;
	dst = src;
	dst.nChannels = 1;
	dst.wBitsPerSample = 8;
	dst.nBlockAlign = 1;
	dst.nAvgBytesPerSec = 8000;
	if (acmStreamOpen(&has, NULL, &src, &dst, NULL, 0, 0,
			  ACM_STREAMOPENF_NONREALTIME))
		return 4;
	if (acmStreamSize(has, sizeof(in), &size, ACM_STREAMSIZEF_SOURCE) ||
	    size != 2)
		return 5;

	zero(&h, sizeof(h));
	h.cbStruct = sizeof(h);
	h.pbSrc = (LPBYTE)in;
	h.cbSrcLength = sizeof(in);
	h.pbDst = out;
	h.cbDstLength = sizeof(out);
	if (acmStreamPrepareHeader(has, &h, 0) ||
	    !(h.fdwStatus & ACMSTREAMHEADER_STATUSF_PREPARED))
		return 6;
	if (acmStreamConvert(has, &h, ACM_STREAMCONVERTF_BLOCKALIGN |
			     ACM_STREAMCONVERTF_START) ||
	    !(h.fdwStatus & ACMSTREAMHEADER_STATUSF_DONE))
		return 7;
	/* Whole frames only; 8 bits keep the high byte, offset by 128. */
	if (h.cbSrcLengthUsed != 8 || h.cbDstLengthUsed != 2 ||
	    out[0] != 0x92 || out[1] != 0xd6 || out[2] != 0)
		return 8;
	if (acmStreamUnprepareHeader(has, &h, 0) ||
	    (h.fdwStatus & ACMSTREAMHEADER_STATUSF_PREPARED))
		return 9;
	if (acmStreamClose(has, 0))
		return 10;
	return 0;
}

BOOL WINAPI DllMain(HINSTANCE inst, DWORD reason, LPVOID reserved)
{
	(void)inst;
	(void)reason;
	(void)reserved;
	return TRUE;
}
