/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * UTF-16 (Windows WCHAR) helpers.
 */
#ifndef VFW_COMMON_UNICODE_H
#define VFW_COMMON_UNICODE_H

#include <stddef.h>

#include "win32/types.h"

/*
 * Converts n UTF-16 units (or up to the first NUL if n is (size_t)-1) to
 * NUL-terminated UTF-8 in out, truncating to fit. Unpaired surrogates become
 * U+FFFD. Returns the length written, excluding the NUL.
 */
size_t utf16_to_utf8(const WCHAR *s, size_t n, char *out, size_t outsz);

/* Case-insensitive (ASCII only) compare of n UTF-16 units with a C string. */
int utf16_ascii_casecmp(const WCHAR *s, size_t n, const char *ascii);

#endif /* VFW_COMMON_UNICODE_H */
