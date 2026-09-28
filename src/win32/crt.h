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
 * wprintf family); otherwise they are ANSI. On success returns 0 with a
 * malloc()ed buffer of *len characters plus a terminating NUL in *out.
 * Returns -1 on a bad format or no memory.
 */
int crt_vformat(int wide, const void *fmt, va_list ap, void **out,
		size_t *len);

#endif /* VFW_WIN32_CRT_H */
