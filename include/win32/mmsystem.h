/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * Installable-driver definitions (the parts of mmsystem.h / mmiscapi.h that
 * VfW codecs use).
 */
#ifndef VFW_WIN32_MMSYSTEM_H
#define VFW_WIN32_MMSYSTEM_H

#include "types.h"

/* Standard driver messages. */
#define DRV_LOAD           0x0001
#define DRV_ENABLE         0x0002
#define DRV_OPEN           0x0003
#define DRV_CLOSE          0x0004
#define DRV_DISABLE        0x0005
#define DRV_FREE           0x0006
#define DRV_CONFIGURE      0x0007
#define DRV_QUERYCONFIGURE 0x0008
#define DRV_INSTALL        0x0009
#define DRV_REMOVE         0x000A
#define DRV_EXITSESSION    0x000B
#define DRV_POWER          0x000F
#define DRV_RESERVED       0x0800
#define DRV_USER           0x4000

/* Return values for DRV_CONFIGURE, DRV_INSTALL and DRV_REMOVE. */
#define DRVCNF_CANCEL  0x0000
#define DRVCNF_OK      0x0001
#define DRVCNF_RESTART 0x0002

/* The Windows SDK's mmsystem.h is byte-packed throughout (pshpack1.h). */
#pragma pack(push, 1)
typedef struct tagDRVCONFIGINFO {
	DWORD dwDCISize;
	LPCWSTR lpszDCISectionName;
	LPCWSTR lpszDCIAliasName;
} DRVCONFIGINFO, *PDRVCONFIGINFO, *LPDRVCONFIGINFO;
#pragma pack(pop)

/* The DriverProc export of an installable driver. */
typedef LRESULT (CALLBACK *DRIVERPROC)(DWORD_PTR dwDriverId, HDRVR hdrvr,
				       UINT msg, LPARAM lParam1, LPARAM lParam2);

/* What the multimedia functions return (mmsyscom.h). */
typedef UINT MMRESULT;

#define MMSYSERR_BASE        0
#define MMSYSERR_NOERROR     0
#define MMSYSERR_ERROR       1
#define MMSYSERR_BADDEVICEID 2
#define MMSYSERR_NOTENABLED  3
#define MMSYSERR_ALLOCATED   4
#define MMSYSERR_INVALHANDLE 5
#define MMSYSERR_NODRIVER    6
#define MMSYSERR_NOMEM       7
#define MMSYSERR_NOTSUPPORTED 8
#define MMSYSERR_BADERRNUM   9
#define MMSYSERR_INVALFLAG   10
#define MMSYSERR_INVALPARAM  11
#define MMSYSERR_HANDLEBUSY  12
#define MMSYSERR_INVALIDALIAS 13
#define MMSYSERR_BADDB       14
#define MMSYSERR_KEYNOTFOUND 15
#define MMSYSERR_READERROR   16
#define MMSYSERR_WRITEERROR  17
#define MMSYSERR_DELETEERROR 18
#define MMSYSERR_VALNOTFOUND 19
#define MMSYSERR_NODRIVERCB  20
#define MMSYSERR_MOREDATA    21
#define MMSYSERR_LASTERROR   21

/* How a stream reports to its owner (the fdwOpen of acmStreamOpen). */
#define CALLBACK_TYPEMASK 0x00070000
#define CALLBACK_WINDOW   0x00010000
#define CALLBACK_TASK     0x00020000
#define CALLBACK_FUNCTION 0x00030000
#define CALLBACK_THREAD   CALLBACK_TASK
#define CALLBACK_EVENT    0x00050000

/* The messages a stream's callback gets (MM_ACM_OPEN and so on). */
#define MM_STREAM_OPEN  0x3D4
#define MM_STREAM_CLOSE 0x3D5
#define MM_STREAM_DONE  0x3D6
#define MM_STREAM_ERROR 0x3D7

/* Multimedia timers (timeapi.h). */
#define TIMERR_NOERROR      0
#define TIMERR_NOCANDO      97

#define TIME_ONESHOT              0x0000
#define TIME_PERIODIC             0x0001
#define TIME_CALLBACK_FUNCTION    0x0000
#define TIME_CALLBACK_EVENT_SET   0x0010
#define TIME_CALLBACK_EVENT_PULSE 0x0020
#define TIME_KILL_SYNCHRONOUS     0x0100

typedef void (CALLBACK *LPTIMECALLBACK)(UINT id, UINT msg, DWORD_PTR user,
					DWORD_PTR dw1, DWORD_PTR dw2);

typedef DWORD FOURCC;

#define WAVE_MAPPER ((UINT)-1)
#define WAVE_FORMAT_PCM 1
#define CALLBACK_NULL 0
#define WHDR_DONE     0x00000001
#define WHDR_PREPARED 0x00000002
#define WHDR_INQUEUE  0x00000010

#pragma pack(push, 1)
/* An audio format: WAVEFORMAT and PCMWAVEFORMAT are the older forms. */
typedef struct waveformat_tag {
	WORD wFormatTag, nChannels;
	DWORD nSamplesPerSec, nAvgBytesPerSec;
	WORD nBlockAlign;
} WAVEFORMAT, *PWAVEFORMAT, *LPWAVEFORMAT;

typedef struct pcmwaveformat_tag {
	WAVEFORMAT wf;
	WORD wBitsPerSample;
} PCMWAVEFORMAT, *PPCMWAVEFORMAT, *LPPCMWAVEFORMAT;

/* cbSize bytes of format-specific data follow (see mmreg.h). */
typedef struct tWAVEFORMATEX {
	WORD wFormatTag, nChannels;
	DWORD nSamplesPerSec, nAvgBytesPerSec;
	WORD nBlockAlign, wBitsPerSample;
	WORD cbSize;
} WAVEFORMATEX, *PWAVEFORMATEX, *LPWAVEFORMATEX;

typedef struct wavehdr_tag {
	LPSTR lpData;
	DWORD dwBufferLength, dwBytesRecorded;
	DWORD_PTR dwUser;
	DWORD dwFlags, dwLoops;
	struct wavehdr_tag *lpNext;
	DWORD_PTR reserved;
} WAVEHDR, *LPWAVEHDR;
#pragma pack(pop)

#define mmioFOURCC(c0, c1, c2, c3) \
	((DWORD)(BYTE)(c0) | ((DWORD)(BYTE)(c1) << 8) | \
	 ((DWORD)(BYTE)(c2) << 16) | ((DWORD)(BYTE)(c3) << 24))

#endif /* VFW_WIN32_MMSYSTEM_H */
