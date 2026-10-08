/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * The built-in msacm32.dll: the Audio Compression Manager, after Wine's
 * dlls/msacm32. Its functions are the DLL's exports and what the harness
 * calls to drive ACM drivers; the msacm_ functions are the harness's own.
 *
 * The drivers are, in priority order: the "msacm.<name>" entries of
 * vfw.ini and the Drivers32 view (vfw_acm_get), followed as they change;
 * those added at run time (acmDriverAdd, msacm_add_path); and last the
 * built-in PCM converter. A driver is opened (DRV_OPEN with an
 * ACMDRVOPENDESCW) for each acmDriverOpen and for each search through the
 * drivers, and its details and format and filter tags are read once, the
 * first time anything needs them; one that won't open, or doesn't answer
 * as an ACM driver, takes no further part.
 *
 * Calls are serialised by one recursive lock, held while drivers run, so
 * an enumeration callback may call back in.
 */
#ifndef VFW_WIN32_MSACM32_H
#define VFW_WIN32_MSACM32_H

#include "runtime/builtin.h"
#include "win32.h"

#define ACM_VERSION 0x04000565	/* 4.0.1381, as Wine gives on NT */

/* The API, as exported (stdcall). */
DWORD STUB_WINAPI msacm_acmGetVersion(void);
MMRESULT STUB_WINAPI msacm_acmMetrics(HACMOBJ hao, UINT metric, LPVOID out);
MMRESULT STUB_WINAPI msacm_acmDriverEnum(ACMDRIVERENUMCB cb, DWORD_PTR inst,
					 DWORD flags);
MMRESULT STUB_WINAPI msacm_acmDriverAddW(LPHACMDRIVERID out, HINSTANCE module,
					 LPARAM param, DWORD priority,
					 DWORD flags);
MMRESULT STUB_WINAPI msacm_acmDriverRemove(HACMDRIVERID hadid, DWORD flags);
MMRESULT STUB_WINAPI msacm_acmDriverDetailsW(HACMDRIVERID hadid,
					     LPACMDRIVERDETAILSW details,
					     DWORD flags);
MMRESULT STUB_WINAPI msacm_acmDriverOpen(LPHACMDRIVER out, HACMDRIVERID hadid,
					 DWORD flags);
MMRESULT STUB_WINAPI msacm_acmDriverClose(HACMDRIVER had, DWORD flags);
LRESULT STUB_WINAPI msacm_acmDriverMessage(HACMDRIVER had, UINT msg,
					   LPARAM l1, LPARAM l2);
MMRESULT STUB_WINAPI msacm_acmDriverID(HACMOBJ hao, LPHACMDRIVERID out,
				       DWORD flags);
MMRESULT STUB_WINAPI msacm_acmDriverPriority(HACMDRIVERID hadid,
					     DWORD priority, DWORD flags);
MMRESULT STUB_WINAPI msacm_acmFormatTagDetailsW(HACMDRIVER had,
						LPACMFORMATTAGDETAILSW d,
						DWORD flags);
MMRESULT STUB_WINAPI msacm_acmFormatTagEnumW(HACMDRIVER had,
					     LPACMFORMATTAGDETAILSW d,
					     ACMFORMATTAGENUMCBW cb,
					     DWORD_PTR inst, DWORD flags);
MMRESULT STUB_WINAPI msacm_acmFormatDetailsW(HACMDRIVER had,
					     LPACMFORMATDETAILSW d,
					     DWORD flags);
MMRESULT STUB_WINAPI msacm_acmFormatEnumW(HACMDRIVER had,
					  LPACMFORMATDETAILSW d,
					  ACMFORMATENUMCBW cb, DWORD_PTR inst,
					  DWORD flags);
MMRESULT STUB_WINAPI msacm_acmFormatSuggest(HACMDRIVER had,
					    LPWAVEFORMATEX src,
					    LPWAVEFORMATEX dst, DWORD dst_size,
					    DWORD flags);
MMRESULT STUB_WINAPI msacm_acmFilterTagDetailsW(HACMDRIVER had,
						LPACMFILTERTAGDETAILSW d,
						DWORD flags);
MMRESULT STUB_WINAPI msacm_acmFilterDetailsW(HACMDRIVER had,
					     LPACMFILTERDETAILSW d,
					     DWORD flags);
MMRESULT STUB_WINAPI msacm_acmStreamOpen(LPHACMSTREAM out, HACMDRIVER had,
					 LPWAVEFORMATEX src,
					 LPWAVEFORMATEX dst,
					 LPWAVEFILTER filter,
					 DWORD_PTR callback, DWORD_PTR inst,
					 DWORD flags);
MMRESULT STUB_WINAPI msacm_acmStreamClose(HACMSTREAM has, DWORD flags);
MMRESULT STUB_WINAPI msacm_acmStreamSize(HACMSTREAM has, DWORD in,
					 LPDWORD out, DWORD flags);
MMRESULT STUB_WINAPI msacm_acmStreamPrepareHeader(HACMSTREAM has,
						  LPACMSTREAMHEADER h,
						  DWORD flags);
MMRESULT STUB_WINAPI msacm_acmStreamUnprepareHeader(HACMSTREAM has,
						    LPACMSTREAMHEADER h,
						    DWORD flags);
MMRESULT STUB_WINAPI msacm_acmStreamConvert(HACMSTREAM has,
					    LPACMSTREAMHEADER h, DWORD flags);
MMRESULT STUB_WINAPI msacm_acmStreamReset(HACMSTREAM has, DWORD flags);

/*
 * Adds the ACM driver in a DLL (a path, or a bare name the loader
 * searches for) for this process, as "msacm.<its base name>", or finds it
 * if it is already there; its details are read straight away. 0, or an
 * MMSYSERR_ with vfw_error() saying why.
 */
MMRESULT msacm_add_path(const char *path, HACMDRIVERID *out);

/* "ACMERR_NOTPOSSIBLE", "MMSYSERR_INVALPARAM", ... or the number. */
const char *msacm_error_name(MMRESULT r, char buf[24]);

/* Open streams (for tests; drv_open_count() counts the drivers). */
unsigned msacm_stream_count(void);

/* Internals shared by msacm32.c, msacm_format.c and msacm_pcm.c. */

/* A format or filter tag a driver has, and the largest format of it. */
struct acm_tag {
	DWORD tag, size;
};

/* A driver: an HACMDRIVERID. */
struct acm_id {
	struct acm_id *next;
	WCHAR *alias;		/* DRV_OPEN's lParam1: L"msacm.imaadpcm" */
	char *name;		/* the alias in UTF-8 */
	char *path;		/* the DLL, or NULL for a procedure */
	DRIVERPROC proc;
	int from_table;		/* a vfw.ini / Drivers32 entry */
	int local;		/* added for this process */
	int removed;		/* acmDriverRemove, or gone from the table */
	int disabled;		/* acmDriverPriority */
	int cached;		/* 1: details read; -1: not a driver that opens */
	ACMDRIVERDETAILSW details;
	struct acm_tag *formats, *filters;	/* details' counts of each */
	struct acm_driver *open;	/* its open instances */
};

/* An open driver: an HACMDRIVER. */
struct acm_driver {
	struct acm_driver *next;	/* the id's next open instance */
	struct acm_id *id;
	HDRVR drv;
	ACMDRVOPENDESCW desc;
};

void acm_lock(void);
void acm_unlock(void);
/* Brings the driver list in step with the table; under the lock. */
void acm_sync(void);
/* Every driver, in priority order (removed ones too). */
struct acm_id *acm_first(void);
/* Whether a driver takes part: not removed or disabled, details read. */
int acm_usable(struct acm_id *id);
/* fdwSupport as the API reports it (with DISABLED and LOCAL). */
DWORD acm_support(const struct acm_id *id);
/* Whether it has a format (or filter) tag. */
int acm_has_format(const struct acm_id *id, DWORD tag);
int acm_has_filter(const struct acm_id *id, DWORD tag);
/* Handles: NULL when not valid. */
struct acm_id *acm_find_id(HACMDRIVERID h);
struct acm_driver *acm_find_driver(HACMDRIVER h);
/* Opens and closes an instance; a message to it. */
MMRESULT acm_driver_open(struct acm_id *id, struct acm_driver **out);
void acm_driver_close(struct acm_driver *d);
MMRESULT acm_message(struct acm_driver *d, UINT msg, LPARAM l1, LPARAM l2);

/* A format's size as the caller gives one: PCM is a PCMWAVEFORMAT. */
DWORD acm_wfx_size(const WAVEFORMATEX *wf);

/* WCHAR strings in fixed fields, and their ANSI (code page 1252) forms. */
void acm_wcopy(WCHAR *dst, size_t n, const WCHAR *src);
void acm_w_to_a(char *dst, size_t n, const WCHAR *src);
void acm_a_to_w(WCHAR *dst, size_t n, const char *src);

/* The PCM converter's DriverProc. */
LRESULT STUB_WINAPI msacm_pcm_proc(DWORD_PTR id, HDRVR h, UINT msg,
				   LPARAM l1, LPARAM l2);

extern const struct builtin_export msacm_format_exports[];

#endif /* VFW_WIN32_MSACM32_H */
