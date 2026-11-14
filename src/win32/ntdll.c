/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * ntdll.dll: the few Rtl functions DLLs import directly, and the C
 * library functions it has its own copies of.
 */
#include <stdarg.h>
#include <string.h>

#include "runtime/builtin.h"
#include "runtime/seh.h"
#include "runtime/teb.h"
#include "win32/crt.h"
#include "win32/winva.h"

#define NT(ret) static ret STUB_WINAPI

typedef LONG NTSTATUS;

NT(void) nt_RtlZeroMemory(void *dst, SIZE_T len)
{
	memset(dst, 0, len);
}

NT(void) nt_RtlFillMemory(void *dst, SIZE_T len, BYTE fill)
{
	memset(dst, fill, len);
}

NT(void) nt_RtlMoveMemory(void *dst, const void *src, SIZE_T len)
{
	memmove(dst, src, len);
}

NT(SIZE_T) nt_RtlCompareMemory(const void *a, const void *b, SIZE_T len)
{
	const BYTE *x = a, *y = b;
	SIZE_T i = 0;

	while (i < len && x[i] == y[i])
		i++;
	return i;
}

NT(DWORD) nt_RtlGetLastWin32Error(void)
{
	struct win_teb *teb = teb_current();
	return teb ? teb->LastErrorValue : 0;
}

NT(void) nt_RtlSetLastWin32Error(DWORD err)
{
	struct win_teb *teb = teb_current();
	if (teb)
		teb->LastErrorValue = err;
}

NT(struct win_teb *) nt_NtCurrentTeb(void)
{
	return teb_current();
}

#define STATUS_SUCCESS              ((NTSTATUS)0x00000000)
#define STATUS_INFO_LENGTH_MISMATCH ((NTSTATUS)0xC0000004)
#define STATUS_INVALID_INFO_CLASS   ((NTSTATUS)0xC0000003)
#define STATUS_ACCESS_VIOLATION     ((NTSTATUS)0xC0000005)

/* Copies len bytes of info out, with the length Windows reports. */
static NTSTATUS info_out(void *buf, ULONG size, ULONG *retlen,
			 const void *info, ULONG len)
{
	if (retlen)
		*retlen = len;
	if (size < len)
		return STATUS_INFO_LENGTH_MISMATCH;
	if (!buf)
		return STATUS_ACCESS_VIOLATION;
	memcpy(buf, info, len);
	return STATUS_SUCCESS;
}

/*
 * The classes runtime libraries and packers ask about: the basic system
 * figures, and whether a kernel debugger is attached (it isn't).
 */
NT(NTSTATUS) nt_NtQuerySystemInformation(ULONG cls, void *buf, ULONG size,
					 ULONG *retlen)
{
	switch (cls) {
	case 0: {	/* SystemBasicInformation */
		struct {
			ULONG Reserved, TimerResolution, PageSize;
			ULONG NumberOfPhysicalPages, LowestPhysicalPageNumber;
			ULONG HighestPhysicalPageNumber, AllocationGranularity;
			ULONG_PTR MinimumUserModeAddress, MaximumUserModeAddress;
			ULONG_PTR ActiveProcessorsAffinityMask;
			CHAR NumberOfProcessors;
		} b = { 0 };
		ULONG n = peb_get()->NumberOfProcessors;
		b.TimerResolution = 156250;
		b.PageSize = 0x1000;
		b.NumberOfPhysicalPages = 0x80000;	/* 2 GB */
		b.LowestPhysicalPageNumber = 1;
		b.HighestPhysicalPageNumber = 0x7ffff;
		b.AllocationGranularity = 0x10000;
		b.MinimumUserModeAddress = 0x10000;
		b.MaximumUserModeAddress = sizeof(void *) == 4 ? 0x7ffeffff :
					   (ULONG_PTR)0x7fffffeffffULL;
		b.ActiveProcessorsAffinityMask = n >= sizeof(ULONG_PTR) * 8 ?
			~(ULONG_PTR)0 : ((ULONG_PTR)1 << n) - 1;
		b.NumberOfProcessors = (CHAR)n;
		return info_out(buf, size, retlen, &b, sizeof(b));
	}
	case 35: {	/* SystemKernelDebuggerInformation */
		BOOLEAN d[2] = { FALSE, TRUE };	/* enabled, not present */
		return info_out(buf, size, retlen, d, sizeof(d));
	}
	default:
		return STATUS_INVALID_INFO_CLASS;
	}
}

/* The current process's basic information, and that no debugger runs it. */
NT(NTSTATUS) nt_NtQueryInformationProcess(HANDLE process, ULONG cls,
					  void *buf, ULONG size, ULONG *retlen)
{
	(void)process;
	switch (cls) {
	case 0: {	/* ProcessBasicInformation */
		struct {
			NTSTATUS ExitStatus;
			void *PebBaseAddress;
			ULONG_PTR AffinityMask;
			LONG BasePriority;
			ULONG_PTR UniqueProcessId, InheritedFromUniqueProcessId;
		} b = { 0 };
		b.ExitStatus = 0x103;	/* STILL_ACTIVE */
		b.PebBaseAddress = peb_get();
		b.AffinityMask = 1;
		b.BasePriority = 8;
		b.UniqueProcessId = teb_current() ? teb_current()->UniqueProcess : 0;
		return info_out(buf, size, retlen, &b, sizeof(b));
	}
	case 7: {	/* ProcessDebugPort */
		ULONG_PTR port = 0;
		return info_out(buf, size, retlen, &port, sizeof(port));
	}
	case 0x1f: {	/* ProcessDebugFlags: 1 = not being debugged */
		ULONG flags = 1;
		return info_out(buf, size, retlen, &flags, sizeof(flags));
	}
	default:
		return STATUS_INVALID_INFO_CLASS;
	}
}

/* msvcrt's, as ntdll's are (crt_printf.c). */
static int STUB_CDECL nt__vsnprintf(char *buf, SIZE_T n, const char *fmt,
				    win_va_list ap)
{
	return crt_vsnprintf(0, buf, n, fmt, ap);
}

static int STUB_CDECL nt__snprintf(char *buf, SIZE_T n, const char *fmt, ...)
{
	win_va_list ap;
	win_va_start(ap, fmt);
	int r = crt_vsnprintf(0, buf, n, fmt, ap);
	win_va_end(ap);
	return r;
}

static int STUB_CDECL nt__vsnwprintf(WCHAR *buf, SIZE_T n, const WCHAR *fmt,
				     win_va_list ap)
{
	return crt_vsnprintf(1, buf, n, fmt, ap);
}

static int STUB_CDECL nt__snwprintf(WCHAR *buf, SIZE_T n, const WCHAR *fmt,
				    ...)
{
	win_va_list ap;
	win_va_start(ap, fmt);
	int r = crt_vsnprintf(1, buf, n, fmt, ap);
	win_va_end(ap);
	return r;
}

#define E(name) { #name, 0, (void *)nt_##name }

#define E2(name) { #name, 0, (void *)seh_##name }

static const struct builtin_export exports[] = {
	E(NtCurrentTeb),
	E(NtQueryInformationProcess),
	E(NtQuerySystemInformation),
	{ "ZwQueryInformationProcess", 0, (void *)nt_NtQueryInformationProcess },
	{ "ZwQuerySystemInformation", 0, (void *)nt_NtQuerySystemInformation },
	E(_snprintf),
	E(_snwprintf),
	E(_vsnprintf),
	E(_vsnwprintf),
	E(RtlCompareMemory),
	E(RtlFillMemory),
	E(RtlGetLastWin32Error),
	E(RtlMoveMemory),
	E(RtlSetLastWin32Error),
	E(RtlZeroMemory),
#if !defined(__x86_64__)
	{ "RtlUnwind", 0, (void *)seh_RtlUnwind },
#else
	E2(RtlLookupFunctionEntry),
	E2(RtlVirtualUnwind),
	E2(RtlAddFunctionTable),
	E2(RtlDeleteFunctionTable),
	E2(RtlCaptureContext),
	E2(RtlRestoreContext),
	E2(RtlUnwindEx),
	E2(RtlUnwind),
	E2(RtlPcToFileHeader),
	{ "__C_specific_handler", 0, (void *)seh_C_specific_handler },
#endif
	BUILTIN_END
};

static const struct builtin_export *const tables[] = { exports, NULL };

const struct builtin_dll builtin_ntdll = { "ntdll.dll", NULL, tables, 0 };
