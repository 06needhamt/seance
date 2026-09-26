/*
 * Code page conversion: the ANSI code page is Windows-1252, the OEM code
 * page 437, plus UTF-8, Latin-1 and ASCII.
 */
#ifndef VFW_WIN32_CODEPAGE_H
#define VFW_WIN32_CODEPAGE_H

#include "win32.h"

#define CODEPAGE_ANSI 1252
#define CODEPAGE_OEM  437

/* Maps CP_ACP, CP_OEMCP and friends to a real code page. */
UINT cp_resolve(UINT cp);
int cp_valid(UINT cp);

/*
 * MultiByteToWideChar without the Win32 wrapping: converts srclen bytes
 * (srclen < 0: up to and including the NUL) into dst, or just counts when
 * dstlen is 0. Returns the WCHARs written or needed, or -1 if dst is too
 * small or (strict) the input is invalid.
 */
int cp_to_utf16(UINT cp, const char *src, int srclen, WCHAR *dst, int dstlen,
		int strict);

/* WideCharToMultiByte likewise; unmappable characters become defchar. */
int cp_from_utf16(UINT cp, const WCHAR *src, int srclen, char *dst,
		  int dstlen, char defchar, int *used_default);

/* malloc()ed conversions; NULL in, NULL out. */
char *acp_to_utf8(const char *s);
char *utf16_to_utf8_dup(const WCHAR *s);
WCHAR *utf8_to_utf16_dup(const char *s);
char *utf8_to_acp_dup(const char *s);

#endif /* VFW_WIN32_CODEPAGE_H */
