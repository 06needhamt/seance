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

#define NT(ret) static ret STUB_WINAPI

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

/* msvcrt's, as ntdll's are (crt_printf.c). */
static int STUB_CDECL nt__vsnprintf(char *buf, SIZE_T n, const char *fmt,
				    va_list ap)
{
	return crt_vsnprintf(0, buf, n, fmt, ap);
}

static int STUB_CDECL nt__snprintf(char *buf, SIZE_T n, const char *fmt, ...)
{
	va_list ap;
	va_start(ap, fmt);
	int r = crt_vsnprintf(0, buf, n, fmt, ap);
	va_end(ap);
	return r;
}

static int STUB_CDECL nt__vsnwprintf(WCHAR *buf, SIZE_T n, const WCHAR *fmt,
				     va_list ap)
{
	return crt_vsnprintf(1, buf, n, fmt, ap);
}

static int STUB_CDECL nt__snwprintf(WCHAR *buf, SIZE_T n, const WCHAR *fmt,
				    ...)
{
	va_list ap;
	va_start(ap, fmt);
	int r = crt_vsnprintf(1, buf, n, fmt, ap);
	va_end(ap);
	return r;
}

#define E(name) { #name, 0, (void *)nt_##name }

static const struct builtin_export exports[] = {
	E(NtCurrentTeb),
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
	{ "RtlUnwind", 0, (void *)seh_RtlUnwind },
	BUILTIN_END
};

static const struct builtin_export *const tables[] = { exports, NULL };

const struct builtin_dll builtin_ntdll = { "ntdll.dll", NULL, tables, 0 };
