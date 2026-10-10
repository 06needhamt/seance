/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * shlwapi.dll string and path helpers.  These deliberately operate on
 * Windows path syntax rather than the host filesystem, except for the two
 * predicates whose documented purpose is to inspect a file.
 */
#include <ctype.h>
#include <fnmatch.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>

#include "win32/codepage.h"
#include "win32/internal.h"
#include "win32/path.h"
#include "win32/reg.h"

static int wi_cmp(const WCHAR *a, const WCHAR *b, size_t n, int fold)
{
	for (size_t i = 0; i < n; i++) {
		unsigned x = a[i], y = b[i];
		if (fold) { x = x < 128 ? tolower(x) : x; y = y < 128 ? tolower(y) : y; }
		if (x != y || !x || !y) return (x > y) - (x < y);
	}
	return 0;
}

WINFN(int) sl_StrCmpNA(LPCSTR a, LPCSTR b, int n) { return strncmp(a, b, n); }
WINFN(int) sl_StrCmpNIA(LPCSTR a, LPCSTR b, int n) { return strncasecmp(a, b, n); }
WINFN(int) sl_StrCmpNW(LPCWSTR a, LPCWSTR b, int n) { return wi_cmp(a, b, n, 0); }
WINFN(int) sl_StrCmpNIW(LPCWSTR a, LPCWSTR b, int n) { return wi_cmp(a, b, n, 1); }
WINFN(LPSTR) sl_StrCpyNA(LPSTR out, LPCSTR in, int n)
{ if (n > 0) { strncpy(out, in, n - 1); out[n - 1] = 0; } return out; }
WINFN(LPWSTR) sl_StrCpyNW(LPWSTR out, LPCWSTR in, int n)
{ int i = 0; if (n > 0) { while (i < n - 1 && in[i]) { out[i] = in[i]; i++; } out[i] = 0; } return out; }
WINFN(LPSTR) sl_StrCatBuffA(LPSTR out, LPCSTR in, int n)
{ size_t l = strlen(out); if ((int)l < n) sl_StrCpyNA(out + l, in, n - l); return out; }
WINFN(LPWSTR) sl_StrCatBuffW(LPWSTR out, LPCWSTR in, int n)
{ size_t l = wstrlen(out); if ((int)l < n) sl_StrCpyNW(out + l, in, n - l); return out; }
WINFN(LPSTR) sl_StrChrA(LPCSTR s, WORD c) { return (LPSTR)strchr(s, (char)c); }
WINFN(LPSTR) sl_StrChrIA(LPCSTR s, WORD c)
{ for (; *s; s++) if (tolower((BYTE)*s) == tolower((BYTE)c)) return (LPSTR)s; return NULL; }
WINFN(LPWSTR) sl_StrChrW(LPCWSTR s, WCHAR c) { for (; *s; s++) if (*s == c) return (LPWSTR)s; return NULL; }
WINFN(LPWSTR) sl_StrChrIW(LPCWSTR s, WCHAR c)
{ for (; *s; s++) if (!wi_cmp(s, &c, 1, 1)) return (LPWSTR)s; return NULL; }
WINFN(LPSTR) sl_StrStrA(LPCSTR s, LPCSTR find) { return (LPSTR)strstr(s, find); }
WINFN(LPSTR) sl_StrStrIA(LPCSTR s, LPCSTR find)
{ size_t n = strlen(find); for (; *s; s++) if (!strncasecmp(s, find, n)) return (LPSTR)s; return !n ? (LPSTR)s : NULL; }
WINFN(LPWSTR) sl_StrStrW(LPCWSTR s, LPCWSTR find)
{ size_t n = wstrlen(find); for (;; s++) { if (!wi_cmp(s, find, n, 0)) return (LPWSTR)s; if (!*s) return NULL; } }
WINFN(LPWSTR) sl_StrStrIW(LPCWSTR s, LPCWSTR find)
{ size_t n = wstrlen(find); for (;; s++) { if (!wi_cmp(s, find, n, 1)) return (LPWSTR)s; if (!*s) return NULL; } }
WINFN(int) sl_StrToIntA(LPCSTR s) { return s ? strtol(s, NULL, 10) : 0; }
WINFN(int) sl_StrToIntW(LPCWSTR s) { char *a = utf16_to_utf8_dup(s); int v = a ? strtol(a, NULL, 10) : 0; free(a); return v; }
WINFN(BOOL) sl_StrToIntExA(LPCSTR s, DWORD flags, int *out)
{ char *end; long v; if (!s || !out) return FALSE; v = strtol(s, &end, flags & STIF_SUPPORT_HEX ? 0 : 10); if (end == s) return FALSE; *out = v; return TRUE; }
WINFN(BOOL) sl_StrToIntExW(LPCWSTR s, DWORD flags, int *out)
{ char *a = utf16_to_utf8_dup(s); BOOL ok = a && sl_StrToIntExA(a, flags, out); free(a); return ok; }

static int sep(char c) { return c == '\\' || c == '/'; }
static char *root_end_a(const char *p)
{
	if (!p) return NULL;
	if (p[0] && p[1] == ':' && sep(p[2])) return (char *)p + 3;
	if (sep(p[0]) && sep(p[1])) { p += 2; while (*p && !sep(*p)) p++; if (*p) p++; while (*p && !sep(*p)) p++; return (char *)(*p ? p + 1 : p); }
	return sep(*p) ? (char *)p + 1 : NULL;
}
WINFN(LPSTR) sl_PathFindFileNameA(LPCSTR p)
{ LPCSTR last = p; if (!p) return NULL; for (; *p; p++) if (sep(*p)) last = p + 1; return (LPSTR)last; }
WINFN(LPWSTR) sl_PathFindFileNameW(LPCWSTR p)
{ LPCWSTR last = p; if (!p) return NULL; for (; *p; p++) if (*p == '\\' || *p == '/') last = p + 1; return (LPWSTR)last; }
WINFN(LPSTR) sl_PathFindExtensionA(LPCSTR p)
{ LPSTR f = sl_PathFindFileNameA(p), dot = NULL; for (char *q = f; *q; q++) if (*q == '.') dot = q; return dot ? dot : (LPSTR)p + strlen(p); }
WINFN(LPWSTR) sl_PathFindExtensionW(LPCWSTR p)
{ LPWSTR f = sl_PathFindFileNameW(p), dot = NULL; for (WCHAR *q = f; *q; q++) if (*q == '.') dot = q; return dot ? dot : (LPWSTR)p + wstrlen(p); }
WINFN(BOOL) sl_PathRemoveExtensionA(LPSTR p) { LPSTR x = sl_PathFindExtensionA(p); if (*x) { *x = 0; return TRUE; } return FALSE; }
WINFN(BOOL) sl_PathRemoveExtensionW(LPWSTR p) { LPWSTR x = sl_PathFindExtensionW(p); if (*x) { *x = 0; return TRUE; } return FALSE; }
WINFN(BOOL) sl_PathRenameExtensionA(LPSTR p, LPCSTR ext)
{ LPSTR x = sl_PathFindExtensionA(p); if (x - p + strlen(ext) >= MAX_PATH) return FALSE; strcpy(x, ext); return TRUE; }
WINFN(BOOL) sl_PathRenameExtensionW(LPWSTR p, LPCWSTR ext)
{ LPWSTR x = sl_PathFindExtensionW(p); if (x - p + wstrlen(ext) >= MAX_PATH) return FALSE; while ((*x++ = *ext++)); return TRUE; }
WINFN(LPSTR) sl_PathAddBackslashA(LPSTR p)
{ size_t n = strlen(p); if (!n || sep(p[n-1])) return p+n; if (n >= MAX_PATH-1) return NULL; p[n]='\\'; p[n+1]=0; return p+n+1; }
WINFN(LPWSTR) sl_PathAddBackslashW(LPWSTR p)
{ size_t n=wstrlen(p); if (!n || p[n-1]=='\\' || p[n-1]=='/') return p+n; if (n>=MAX_PATH-1) return NULL; p[n]='\\'; p[n+1]=0; return p+n+1; }
WINFN(LPSTR) sl_PathRemoveBackslashA(LPSTR p)
{ size_t n=strlen(p); char *r=root_end_a(p); if (n && sep(p[n-1]) && p+n != r) p[--n]=0; return p+n; }
WINFN(BOOL) sl_PathRemoveFileSpecA(LPSTR p)
{ char *r=root_end_a(p), *q=p+strlen(p); while (q>p && !sep(q[-1])) q--; if (q<=p || (r && q<=r)) return FALSE; while (q>p && sep(q[-1]) && (!r || q>r)) q--; *q=0; return TRUE; }
WINFN(void) sl_PathStripPathA(LPSTR p) { LPSTR f=sl_PathFindFileNameA(p); memmove(p,f,strlen(f)+1); }
WINFN(BOOL) sl_PathIsRootA(LPCSTR p) { char *r=root_end_a(p); return r && !*r; }
WINFN(LPSTR) sl_PathSkipRootA(LPCSTR p) { return root_end_a(p); }
WINFN(BOOL) sl_PathIsRelativeA(LPCSTR p) { return !(p && ((p[0] && p[1]==':') || sep(p[0]))); }
WINFN(BOOL) sl_PathIsURLA(LPCSTR p)
{
	LPCSTR start = p;
	if (!p || !isalpha((BYTE)*p))
		return FALSE;
	while (isalnum((BYTE)*p) || *p == '+' || *p == '-' || *p == '.')
		p++;
	/* A single letter followed by a colon is a Windows drive, not a URL. */
	return p - start > 1 && *p == ':';
}
WINFN(BOOL) sl_PathFileExistsA(LPCSTR p)
{ char *u=path_to_unix(p); struct stat st; BOOL ok=u && !stat(u,&st); free(u); return ok; }
WINFN(BOOL) sl_PathIsDirectoryA(LPCSTR p)
{ char *u=path_to_unix(p); struct stat st; BOOL ok=u && !stat(u,&st) && S_ISDIR(st.st_mode); free(u); return ok; }
WINFN(BOOL) sl_PathAppendA(LPSTR p, LPCSTR more)
{ while (sep(*more)) more++; if (!sl_PathAddBackslashA(p)) return FALSE; if (strlen(p)+strlen(more)>=MAX_PATH) return FALSE; strcat(p,more); return TRUE; }
/* "\\x" after a directory is on its drive (or share): C:\\x. */
WINFN(LPSTR) sl_PathCombineA(LPSTR out, LPCSTR dir, LPCSTR file)
{
	char tmp[MAX_PATH];
	if (!out || !file) return NULL;
	if (dir && *dir && sep(file[0]) && !sep(file[1])) {
		char *r = root_end_a(dir);
		int n = r ? (int)(r - dir) - (sep(r[-1]) ? 1 : 0) : 0;
		if (snprintf(tmp, sizeof(tmp), "%.*s%s", n, dir, file) >= (int)sizeof(tmp)) return NULL;
	} else if (!dir || !*dir || !sl_PathIsRelativeA(file)) {
		if (snprintf(tmp, sizeof(tmp), "%s", file) >= (int)sizeof(tmp)) return NULL;
	} else {
		snprintf(tmp, sizeof(tmp), "%s", dir);
		if (strlen(dir) >= sizeof(tmp) || !sl_PathAppendA(tmp, file)) return NULL;
	}
	strcpy(out, tmp);
	return out;
}
WINFN(BOOL) sl_PathMatchSpecA(LPCSTR p, LPCSTR specs)
{ char *copy=strdup(specs ? specs : ""), *save=NULL; BOOL ok=FALSE; for(char *s=strtok_r(copy,";",&save);s;s=strtok_r(NULL,";",&save)) if(!fnmatch(s,p,FNM_CASEFOLD)){ok=TRUE;break;} free(copy); return ok; }
WINFN(void) sl_PathQuoteSpacesA(LPSTR p) { if (strchr(p,' ') && *p!='\"') { size_t n=strlen(p); memmove(p+1,p,n+1); p[0]='\"'; p[n+1]='\"'; p[n+2]=0; } }
WINFN(void) sl_PathUnquoteSpacesA(LPSTR p) { size_t n=strlen(p); if(n>=2&&p[0]=='\"'&&p[n-1]=='\"'){memmove(p,p+1,n-2);p[n-2]=0;} }

/* Wide path entry points use the same rules via temporary UTF-8 strings. */
static BOOL wide_pred(LPCWSTR p, BOOL (WINAPI *fn)(LPCSTR))
{
	char *a = utf16_to_utf8_dup(p);
	BOOL r = a && fn(a);
	free(a);
	return r;
}
WINFN(BOOL) sl_PathFileExistsW(LPCWSTR p) { return wide_pred(p, sl_PathFileExistsA); }
WINFN(BOOL) sl_PathIsDirectoryW(LPCWSTR p) { return wide_pred(p, sl_PathIsDirectoryA); }
WINFN(BOOL) sl_PathIsRelativeW(LPCWSTR p) { return p && !((p[0]&&p[1]==':')||p[0]=='\\'||p[0]=='/'); }
WINFN(BOOL) sl_PathIsURLW(LPCWSTR p) { char *a=utf16_to_utf8_dup(p); BOOL r=a&&sl_PathIsURLA(a); free(a); return r; }

/* "\\server\share": UNC, but not "\\?\c:\..." (Windows 7's rule). */
WINFN(BOOL) sl_PathIsUNCA(LPCSTR p)
{ return p && p[0]=='\\' && p[1]=='\\' && p[2]!='?'; }
WINFN(BOOL) sl_PathIsUNCW(LPCWSTR p)
{ return p && p[0]=='\\' && p[1]=='\\' && p[2]!='?'; }
/* As PathCombineA, through UTF-8 (so at most MAX_PATH bytes of it). */
WINFN(LPWSTR) sl_PathCombineW(LPWSTR out, LPCWSTR dir, LPCWSTR file)
{
	char *d = dir ? utf16_to_utf8_dup(dir) : NULL;
	char *f = file ? utf16_to_utf8_dup(file) : NULL;
	char a[MAX_PATH];
	WCHAR *w = NULL;
	LPWSTR r = NULL;

	if (out && f && (!dir || d) && sl_PathCombineA(a, d, f) &&
	    (w = utf8_to_utf16_dup(a)) && wstrlen(w) < MAX_PATH)
		r = memcpy(out, w, (wstrlen(w) + 1) * sizeof(WCHAR));
	else if (out)
		out[0] = 0;
	free(w);
	free(d);
	free(f);
	return r;
}

/* A file name alone: no directory, drive or separator in it. */
WINFN(BOOL) sl_PathIsFileSpecA(LPCSTR p)
{ if (!p) return FALSE; for (; *p; p++) if (sep(*p) || *p == ':') return FALSE; return TRUE; }
WINFN(BOOL) sl_PathIsFileSpecW(LPCWSTR p)
{ if (!p) return FALSE; for (; *p; p++) if (*p == '\\' || *p == '/' || *p == ':') return FALSE; return TRUE; }
/* Removing only shortens, so the UTF-8 answer's length gives the UTF-16 one. */
WINFN(BOOL) sl_PathRemoveFileSpecW(LPWSTR p)
{
	char *a = utf16_to_utf8_dup(p);
	BOOL ok = a && sl_PathRemoveFileSpecA(a);

	if (ok) {
		int n = cp_to_utf16(CP_UTF8, a, -1, NULL, 0, 0);
		if (n > 0)
			p[n - 1] = 0;
	}
	free(a);
	return ok;
}

/*
 * UrlGetPart: one piece of scheme://user:password@host:port/path?query.
 * S_FALSE if the URL has none, E_POINTER (with the size needed, counting the
 * NUL) if the buffer is too small, URL_E_INVALID_SYNTAX if it isn't a URL.
 */
#define URL_PART_SCHEME     1
#define URL_PART_HOSTNAME   2
#define URL_PART_USERNAME   3
#define URL_PART_PASSWORD   4
#define URL_PART_PORT       5
#define URL_PART_QUERY      6
#define URL_PARTFLAG_KEEPSCHEME 1
#define URL_E_INVALID_SYNTAX ((HRESULT)0x80041001)

static HRESULT url_part(const char *url, DWORD part, DWORD flags, char **out)
{
	const char *p = url, *rest, *auth_end, *auth, *at, *colon;
	const char *start = NULL;
	size_t len = 0, scheme_len;

	*out = NULL;
	while (*p && *p != ':' && (isalnum((BYTE)*p) || *p == '+' || *p == '-' || *p == '.'))
		p++;
	if (*p != ':' || p == url || !isalpha((BYTE)*url))
		return URL_E_INVALID_SYNTAX;
	scheme_len = (size_t)(p - url);
	rest = p + 1;
	if (part == URL_PART_SCHEME) {
		start = url;
		len = scheme_len;
	} else if (part == URL_PART_QUERY) {
		const char *q = strchr(rest, '?');
		const char *hash;

		if (q) {
			start = q + 1;
			hash = strchr(start, '#');
			len = hash ? (size_t)(hash - start) : strlen(start);
		}
	} else if (part >= URL_PART_HOSTNAME && part <= URL_PART_PORT) {
		if (rest[0] == '/' && rest[1] == '/') {
			auth = rest + 2;
			auth_end = auth + strcspn(auth, "/?#");
			at = memchr(auth, '@', (size_t)(auth_end - auth));
			const char *host = at ? at + 1 : auth;

			if (part == URL_PART_USERNAME || part == URL_PART_PASSWORD) {
				if (at) {
					colon = memchr(auth, ':', (size_t)(at - auth));
					if (part == URL_PART_USERNAME) {
						start = auth;
						len = (size_t)((colon ? colon : at) - auth);
					} else if (colon) {
						start = colon + 1;
						len = (size_t)(at - colon - 1);
					}
				}
			} else {
				const char *host_end = auth_end;

				if (*host == '[') {		/* an IPv6 address */
					const char *close = memchr(host, ']', (size_t)(auth_end - host));

					host_end = close ? close + 1 : auth_end;
					colon = host_end < auth_end && *host_end == ':' ? host_end : NULL;
				} else {
					colon = memchr(host, ':', (size_t)(auth_end - host));
					if (colon)
						host_end = colon;
				}
				if (part == URL_PART_HOSTNAME) {
					start = host;
					len = (size_t)(host_end - host);
				} else if (colon) {
					start = colon + 1;
					len = (size_t)(auth_end - colon - 1);
				}
			}
		}
	} else
		return E_INVALIDARG;
	if (!len)
		return S_FALSE;
	/* With the scheme kept, the scheme and a colon go first. */
	if ((flags & URL_PARTFLAG_KEEPSCHEME) && part != URL_PART_SCHEME &&
	    part != URL_PART_QUERY) {
		*out = malloc(scheme_len + 1 + len + 1);
		if (!*out)
			return E_OUTOFMEMORY;
		memcpy(*out, url, scheme_len + 1);
		memcpy(*out + scheme_len + 1, start, len);
		(*out)[scheme_len + 1 + len] = '\0';
	} else {
		*out = malloc(len + 1);
		if (!*out)
			return E_OUTOFMEMORY;
		memcpy(*out, start, len);
		(*out)[len] = '\0';
	}
	return S_OK;
}

WINFN(HRESULT) sl_UrlGetPartA(LPCSTR url, LPSTR buf, LPDWORD size, DWORD part,
			      DWORD flags)
{
	char *piece;
	HRESULT hr;

	if (!url || !buf || !size || !*size)
		return E_INVALIDARG;
	hr = url_part(url, part, flags, &piece);
	if (hr == S_OK) {
		DWORD n = (DWORD)strlen(piece);

		if (n >= *size) {
			*size = n + 1;
			hr = E_POINTER;
		} else {
			memcpy(buf, piece, n + 1);
			*size = n;
		}
	} else if (hr == S_FALSE) {
		buf[0] = '\0';
		*size = 0;
	}
	free(piece);
	return hr;
}

WINFN(HRESULT) sl_UrlGetPartW(LPCWSTR url, LPWSTR buf, LPDWORD size, DWORD part,
			      DWORD flags)
{
	char *u = url ? utf16_to_utf8_dup(url) : NULL;
	char *piece = NULL;
	HRESULT hr;

	if (!u || !buf || !size || !*size) {
		free(u);
		return E_INVALIDARG;
	}
	hr = url_part(u, part, flags, &piece);
	if (hr == S_OK) {
		int need = cp_to_utf16(CP_UTF8, piece, -1, NULL, 0, 0);

		if (need > 0 && (DWORD)need > *size) {
			*size = (DWORD)need;
			hr = E_POINTER;
		} else {
			cp_to_utf16(CP_UTF8, piece, -1, buf, (int)*size, 0);
			*size = (DWORD)need - 1;
		}
	} else if (hr == S_FALSE) {
		buf[0] = 0;
		*size = 0;
	}
	free(piece);
	free(u);
	return hr;
}

/* A key and everything under it (a filter's DllUnregisterServer uses it). */
WINFN(DWORD) sl_SHDeleteKeyA(HKEY h, LPCSTR sub)
{
	char *s = acp_to_utf8(sub ? sub : "");
	LONG r = s ? adv_delete_tree(h, s) : ERROR_NOT_ENOUGH_MEMORY;
	free(s);
	return (DWORD)r;
}

WINFN(DWORD) sl_SHDeleteKeyW(HKEY h, LPCWSTR sub)
{
	char *s = sub ? utf16_to_utf8_dup(sub) : strdup("");
	LONG r = s ? adv_delete_tree(h, s) : ERROR_NOT_ENOUGH_MEMORY;
	free(s);
	return (DWORD)r;
}

WINFN(HRESULT) sl_DllGetVersion(DLLVERSIONINFO *v)
{ if(!v || v->cbSize<sizeof(*v)) return E_INVALIDARG; v->dwMajorVersion=6; v->dwMinorVersion=0; v->dwBuildNumber=2900; v->dwPlatformID=2; return S_OK; }

#define E(n) { #n, 0, (void *)sl_##n }
/* kernelbase.dll (kernel32.c) serves these too, as Windows 7's does. */
const struct builtin_export shlwapi_exports[] = {
	E(DllGetVersion), E(SHDeleteKeyA), E(SHDeleteKeyW), E(PathAddBackslashA), E(PathAddBackslashW), E(PathAppendA),
	E(PathCombineA), E(PathCombineW), E(PathFileExistsA), E(PathFileExistsW), E(PathFindExtensionA),
	E(PathFindExtensionW), E(PathFindFileNameA), E(PathFindFileNameW),
	E(PathIsDirectoryA), E(PathIsDirectoryW), E(PathIsFileSpecA), E(PathIsFileSpecW),
	E(PathIsRelativeA), E(PathIsRelativeW),
	E(PathIsRootA), E(PathIsUNCA), E(PathIsUNCW), E(PathIsURLA), E(PathIsURLW), E(PathMatchSpecA), E(PathQuoteSpacesA),
	E(PathRemoveBackslashA), E(PathRemoveExtensionA), E(PathRemoveExtensionW),
	E(PathRemoveFileSpecA), E(PathRemoveFileSpecW), E(PathRenameExtensionA),
	E(PathRenameExtensionW),
	E(PathSkipRootA), E(PathStripPathA), E(PathUnquoteSpacesA), E(StrCatBuffA),
	E(StrCatBuffW), E(StrChrA), E(StrChrIA), E(StrChrIW), E(StrChrW), E(StrCmpNA),
	E(StrCmpNIA), E(StrCmpNIW), E(StrCmpNW), E(StrCpyNA), E(StrCpyNW), E(StrStrA),
	E(StrStrIA), E(StrStrIW), E(StrStrW), E(StrToIntA), E(StrToIntExA),
	E(StrToIntExW), E(StrToIntW), E(UrlGetPartA), E(UrlGetPartW), BUILTIN_END
};
static const struct builtin_export *const tables[] = { shlwapi_exports, NULL };
const struct builtin_dll builtin_shlwapi = { "shlwapi.dll", NULL, tables, 0 };
