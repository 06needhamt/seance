/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * Variable arguments as Windows code passes them, for the exports that take
 * "..." or a va_list (printf, scanf, wsprintf, _open, ...).
 *
 * On i386 they are the host's own: both put the arguments on the stack and
 * a va_list is a pointer to them. On x86-64 they are not: Win64's va_list
 * is a pointer into the caller's argument slots, the SysV one a structure
 * over saved registers, so these functions use gcc's ms_abi va_list. A
 * win_va_list must never be passed to the host's vprintf family.
 */
#ifndef VFW_WIN32_WINVA_H
#define VFW_WIN32_WINVA_H

#include <stdarg.h>

#if defined(__x86_64__)
typedef __builtin_ms_va_list win_va_list;
#define win_va_start(ap, last) __builtin_ms_va_start(ap, last)
#define win_va_end(ap)         __builtin_ms_va_end(ap)
#define win_va_copy(dst, src)  __builtin_ms_va_copy(dst, src)
#else
typedef va_list win_va_list;
#define win_va_start(ap, last) va_start(ap, last)
#define win_va_end(ap)         va_end(ap)
#define win_va_copy(dst, src)  va_copy(dst, src)
#endif
/* gcc's va_arg reads either kind. */
#define win_va_arg(ap, type)   va_arg(ap, type)

#endif /* VFW_WIN32_WINVA_H */
