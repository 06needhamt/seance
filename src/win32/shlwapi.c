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
WINFN(LPSTR) sl_PathCombineA(LPSTR out, LPCSTR dir, LPCSTR file)
{ char tmp[MAX_PATH]; if (!out || !file) return NULL; if (!dir || !*dir || !sl_PathIsRelativeA(file)) snprintf(tmp,sizeof(tmp),"%s",file); else { snprintf(tmp,sizeof(tmp),"%s",dir); if (!sl_PathAppendA(tmp,file)) return NULL; } strcpy(out,tmp); return out; }
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

WINFN(HRESULT) sl_DllGetVersion(DLLVERSIONINFO *v)
{ if(!v || v->cbSize<sizeof(*v)) return E_INVALIDARG; v->dwMajorVersion=6; v->dwMinorVersion=0; v->dwBuildNumber=2900; v->dwPlatformID=2; return S_OK; }

#define E(n) { #n, 0, (void *)sl_##n }
static const struct builtin_export exports[] = {
	E(DllGetVersion), E(PathAddBackslashA), E(PathAddBackslashW), E(PathAppendA),
	E(PathCombineA), E(PathFileExistsA), E(PathFileExistsW), E(PathFindExtensionA),
	E(PathFindExtensionW), E(PathFindFileNameA), E(PathFindFileNameW),
	E(PathIsDirectoryA), E(PathIsDirectoryW), E(PathIsRelativeA), E(PathIsRelativeW),
	E(PathIsRootA), E(PathIsURLA), E(PathIsURLW), E(PathMatchSpecA), E(PathQuoteSpacesA),
	E(PathRemoveBackslashA), E(PathRemoveExtensionA), E(PathRemoveExtensionW),
	E(PathRemoveFileSpecA), E(PathRenameExtensionA), E(PathRenameExtensionW),
	E(PathSkipRootA), E(PathStripPathA), E(PathUnquoteSpacesA), E(StrCatBuffA),
	E(StrCatBuffW), E(StrChrA), E(StrChrIA), E(StrChrIW), E(StrChrW), E(StrCmpNA),
	E(StrCmpNIA), E(StrCmpNIW), E(StrCmpNW), E(StrCpyNA), E(StrCpyNW), E(StrStrA),
	E(StrStrIA), E(StrStrIW), E(StrStrW), E(StrToIntA), E(StrToIntExA),
	E(StrToIntExW), E(StrToIntW), BUILTIN_END
};
static const struct builtin_export *const tables[] = { exports, NULL };
const struct builtin_dll builtin_shlwapi = { "shlwapi.dll", NULL, tables, 0 };
