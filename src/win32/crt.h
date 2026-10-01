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

/*
 * After the CRT's malloc, calloc or realloc failed to get size bytes:
 * nonzero to try again, when _set_new_mode(1) is in effect and the new
 * handler (see crt_cxx.c) says it freed some memory.
 */
int crt_malloc_retry(size_t size);

#endif /* VFW_WIN32_CRT_H */
