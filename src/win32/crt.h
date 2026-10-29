/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * Shared between the msvcrt source files.
 */
#ifndef VFW_WIN32_CRT_H
#define VFW_WIN32_CRT_H

#include <stdarg.h>
#include <stddef.h>

/*
 * Formats as msvcrt.dll's printf family does (see crt_printf.c). With
 * wide set, fmt and the result are UTF-16 and %s/%c take UTF-16 (the
 * wprintf family); otherwise they are ANSI. flags is 0 for msvcrt, or
 * CRT_UCRT with the options ucrtbase's __stdio_common_* functions take.
 * On success returns 0 with a malloc()ed buffer of *len characters plus a
 * terminating NUL in *out. Returns -1 on a bad format or no memory.
 */
int crt_vformat(int wide, unsigned flags, const void *fmt, va_list ap,
		void **out, size_t *len);

/*
 * _vsnprintf, or with wide _vsnwprintf: at most n characters in buf,
 * terminated if they fit; the length, or -1 if it was more than n.
 */
int crt_vsnprintf(int wide, void *buf, size_t n, const void *fmt, va_list ap);

/* The low bits are ucrtbase's _CRT_INTERNAL_PRINTF_* options. */
#define CRT_PRINTF_LEGACY_VSPRINTF_NULL_TERMINATION 0x0001
#define CRT_PRINTF_STANDARD_SNPRINTF_BEHAVIOR 0x0002
#define CRT_PRINTF_LEGACY_WIDE_SPECIFIERS 0x0004
#define CRT_PRINTF_LEGACY_MSVCRT_COMPATIBILITY 0x0008
#define CRT_PRINTF_LEGACY_THREE_DIGIT_EXPONENTS 0x0010
#define CRT_PRINTF_OPTIONS 0x001f
/*
 * ucrtbase's ways, unless an option keeps msvcrt's: inf and nan, two
 * exponent digits, %s meaning char * in the wide functions; and the
 * sizes z, t and j and the conversion F.
 */
#define CRT_UCRT 0x10000

/*
 * After the CRT's malloc, calloc or realloc failed to get size bytes:
 * nonzero to try again, when _set_new_mode(1) is in effect and the new
 * handler (see crt_cxx.c) says it freed some memory.
 */
int crt_malloc_retry(size_t size);

/*
 * MSVC's i386 jmp_buf, as _setjmp3 fills it (crt_seh.c): the registers,
 * the fs:[0] frame and its try level at the time, and what longjmp calls
 * to unwind that frame's own levels (__CxxLongjmpUnwind for a C++ frame,
 * UnwindData[0] being its FuncInfo).
 */
struct jump_buffer {
	unsigned long ebp, ebx, edi, esi, esp, eip;
	unsigned long registration;
	long trylevel;
	unsigned long cookie;	/* JMP_COOKIE from _setjmp3 */
	unsigned long unwind_func;
	unsigned long unwind_data[6];
};

#define JMP_COOKIE 0x56433230	/* "VC20" */

/*
 * Runs fn(arg) as __try { fn(arg); } __except (an access violation) {}
 * would (crt_cxx.c): 1 if it faulted, 0 if it returned. A fault only
 * becomes an exception through the harness's fault handler
 * (crash_install), so without it this still crashes.
 */
int crt_guarded(void (*fn)(void *), void *arg);

/*
 * Control goes on in the frame whose %ebp is ebp, after an exception or
 * longjmp (crt_seh.c): the __finally blocks an exception left in it and
 * deeper frames are over, for _abnormal_termination's sake.
 */
void crt_finally_abandon(const void *ebp);

/* Data msvcrt exports, which crtdll.dll exports again under other names. */
extern const unsigned short *crt_pctype_ptr;
extern int crt_mb_cur_max, crt_fmode, crt_commode;

#endif /* VFW_WIN32_CRT_H */
