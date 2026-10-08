/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * The ACM driver side (msacmdrv.h), which mingw-w64 doesn't have: the
 * messages msacm32 sends a driver and the structures they carry, as the
 * Windows DDK (and Wine) define them. The harness's include/win32/msacm.h
 * has the same, with their layout asserted.
 */
#ifndef VFW_FIXTURE_ACMDRV_H
#define VFW_FIXTURE_ACMDRV_H

#include <msacm.h>

#define MAKE_ACM_VERSION(major, minor, build) \
	(((DWORD)(major) << 24) | ((DWORD)(minor) << 16) | (DWORD)(build))

#define ACMDM_DRIVER_NOTIFY             (ACMDM_BASE + 1)
#define ACMDM_DRIVER_DETAILS            (ACMDM_BASE + 10)
#define ACMDM_HARDWARE_WAVE_CAPS_INPUT  (ACMDM_BASE + 20)
#define ACMDM_HARDWARE_WAVE_CAPS_OUTPUT (ACMDM_BASE + 21)
#define ACMDM_FORMATTAG_DETAILS         (ACMDM_BASE + 25)
#define ACMDM_FORMAT_DETAILS            (ACMDM_BASE + 26)
#define ACMDM_FORMAT_SUGGEST            (ACMDM_BASE + 27)
#define ACMDM_FILTERTAG_DETAILS         (ACMDM_BASE + 50)
#define ACMDM_FILTER_DETAILS            (ACMDM_BASE + 51)
#define ACMDM_STREAM_OPEN               (ACMDM_BASE + 76)
#define ACMDM_STREAM_CLOSE              (ACMDM_BASE + 77)
#define ACMDM_STREAM_SIZE               (ACMDM_BASE + 78)
#define ACMDM_STREAM_CONVERT            (ACMDM_BASE + 79)
#define ACMDM_STREAM_RESET              (ACMDM_BASE + 80)
#define ACMDM_STREAM_PREPARE            (ACMDM_BASE + 81)
#define ACMDM_STREAM_UNPREPARE          (ACMDM_BASE + 82)
#define ACMDM_STREAM_UPDATE             (ACMDM_BASE + 83)

#include <pshpack1.h>
typedef struct {
	DWORD cbStruct;
	FOURCC fccType;
	FOURCC fccComp;
	DWORD dwVersion;
	DWORD dwFlags;
	DWORD dwError;
	LPCWSTR pszSectionName;
	LPCWSTR pszAliasName;
	DWORD dnDevNode;
} ACMDRVOPENDESCW;

typedef struct {
	DWORD cbStruct;
	LPWAVEFORMATEX pwfxSrc;
	LPWAVEFORMATEX pwfxDst;
	LPWAVEFILTER pwfltr;
	DWORD_PTR dwCallback;
	DWORD_PTR dwInstance;
	DWORD fdwOpen;
	DWORD fdwDriver;
	DWORD_PTR dwDriver;
	HACMSTREAM has;
} ACMDRVSTREAMINSTANCE;

typedef struct tACMDRVSTREAMHEADER {
	DWORD cbStruct;
	DWORD fdwStatus;
	DWORD_PTR dwUser;
	LPBYTE pbSrc;
	DWORD cbSrcLength;
	DWORD cbSrcLengthUsed;
	DWORD_PTR dwSrcUser;
	LPBYTE pbDst;
	DWORD cbDstLength;
	DWORD cbDstLengthUsed;
	DWORD_PTR dwDstUser;
	DWORD fdwConvert;
	struct tACMDRVSTREAMHEADER *padshNext;
	DWORD fdwDriver;
	DWORD_PTR dwDriver;
	DWORD fdwPrepared;
	DWORD_PTR dwPrepared;
	LPBYTE pbPreparedSrc;
	DWORD cbPreparedSrcLength;
	LPBYTE pbPreparedDst;
	DWORD cbPreparedDstLength;
} ACMDRVSTREAMHEADER;

typedef struct {
	DWORD cbStruct;
	DWORD fdwSize;
	DWORD cbSrcLength;
	DWORD cbDstLength;
} ACMDRVSTREAMSIZE;

typedef struct {
	DWORD cbStruct;
	DWORD fdwSuggest;
	LPWAVEFORMATEX pwfxSrc;
	DWORD cbwfxSrc;
	LPWAVEFORMATEX pwfxDst;
	DWORD cbwfxDst;
} ACMDRVFORMATSUGGEST;
#include <poppack.h>

_Static_assert(sizeof(ACMDRVSTREAMHEADER) == sizeof(ACMSTREAMHEADER),
	       "ACMDRVSTREAMHEADER");

#endif /* VFW_FIXTURE_ACMDRV_H */
