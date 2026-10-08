/* SPDX-License-Identifier: GPL-2.0-only */
/* shlwapi is exercised through its built-in DLL export table. */
#include <stdlib.h>
#include <string.h>

#include "loader/modules.h"
#include "runtime/runtime.h"
#include "win32/codepage.h"
#include "test.h"

static struct module *dll;

static void *get(const char *name)
{
	void *p = mod_get_proc(dll, name);
	CHECK(p != NULL);
	return p;
}

#define FN(ret, name, ...) ret (WINAPI *name)(__VA_ARGS__) = \
	(ret (WINAPI *)(__VA_ARGS__))get(#name)

static void test_strings(void)
{
	FN(LPSTR, StrCpyNA, LPSTR, LPCSTR, int);
	FN(LPSTR, StrCatBuffA, LPSTR, LPCSTR, int);
	FN(LPSTR, StrStrIA, LPCSTR, LPCSTR);
	FN(int, StrCmpNIA, LPCSTR, LPCSTR, int);
	FN(BOOL, StrToIntExA, LPCSTR, DWORD, int *);
	char b[8];
	int value = 0;

	CHECK(StrCpyNA(b, "abcdefghi", sizeof(b)) == b);
	CHECK_STR(b, "abcdefg");
	strcpy(b, "ab");
	CHECK(StrCatBuffA(b, "cdefgh", sizeof(b)) == b);
	CHECK_STR(b, "abcdefg");
	const char *haystack = "One Codec";
	CHECK(StrStrIA(haystack, "CODE") == haystack + 4);
	CHECK_EQ(StrCmpNIA("AbCd", "aBcX", 3), 0);
	CHECK(StrToIntExA("0x2a", STIF_SUPPORT_HEX, &value));
	CHECK_EQ(value, 42);
	CHECK(!StrToIntExA("no", 0, &value));
}

static void test_paths(void)
{
	FN(LPSTR, PathFindFileNameA, LPCSTR);
	FN(LPSTR, PathFindExtensionA, LPCSTR);
	FN(BOOL, PathRemoveFileSpecA, LPSTR);
	FN(BOOL, PathRenameExtensionA, LPSTR, LPCSTR);
	FN(LPSTR, PathCombineA, LPSTR, LPCSTR, LPCSTR);
	FN(BOOL, PathIsRelativeA, LPCSTR);
	FN(BOOL, PathIsURLA, LPCSTR);
	FN(BOOL, PathMatchSpecA, LPCSTR, LPCSTR);
	FN(void, PathQuoteSpacesA, LPSTR);
	FN(void, PathUnquoteSpacesA, LPSTR);
	char b[MAX_PATH] = "C:\\Program Files\\codec.ax";

	CHECK_STR(PathFindFileNameA(b), "codec.ax");
	CHECK_STR(PathFindExtensionA(b), ".ax");
	CHECK(PathRenameExtensionA(b, ".dll"));
	CHECK_STR(b, "C:\\Program Files\\codec.dll");
	CHECK(PathRemoveFileSpecA(b));
	CHECK_STR(b, "C:\\Program Files");

	/* UrlGetPart: one piece of a URL. */
	FN(HRESULT, UrlGetPartA, LPCSTR, LPSTR, LPDWORD, DWORD, DWORD);
	FN(HRESULT, UrlGetPartW, LPCWSTR, LPWSTR, LPDWORD, DWORD, DWORD);
	{
		const char *url = "http://user:pw@example.com:8080/path/x.avi?a=1&b=2#frag";
		char part[64];
		DWORD n = sizeof(part);

		CHECK_EQ(UrlGetPartA(url, part, &n, 1, 0), S_OK);	/* scheme */
		CHECK_STR(part, "http");
		CHECK_EQ(n, 4);
		n = sizeof(part);
		CHECK_EQ(UrlGetPartA(url, part, &n, 2, 0), S_OK);	/* host */
		CHECK_STR(part, "example.com");
		n = sizeof(part);
		CHECK_EQ(UrlGetPartA(url, part, &n, 3, 0), S_OK);	/* user */
		CHECK_STR(part, "user");
		n = sizeof(part);
		CHECK_EQ(UrlGetPartA(url, part, &n, 4, 0), S_OK);	/* password */
		CHECK_STR(part, "pw");
		n = sizeof(part);
		CHECK_EQ(UrlGetPartA(url, part, &n, 5, 0), S_OK);	/* port */
		CHECK_STR(part, "8080");
		n = sizeof(part);
		CHECK_EQ(UrlGetPartA(url, part, &n, 6, 0), S_OK);	/* query */
		CHECK_STR(part, "a=1&b=2");
		n = sizeof(part);
		CHECK_EQ(UrlGetPartA(url, part, &n, 2, 1), S_OK);	/* keeping the scheme */
		CHECK_STR(part, "http:example.com");
		/* Pieces a URL doesn't have; a bad one; a buffer too small. */
		n = sizeof(part);
		CHECK_EQ(UrlGetPartA("http://example.com/", part, &n, 5, 0), S_FALSE);
		CHECK_EQ(n, 0);
		n = sizeof(part);
		CHECK_EQ(UrlGetPartA("http://[::1]:99/", part, &n, 2, 0), S_OK);
		CHECK_STR(part, "[::1]");
		n = sizeof(part);
		CHECK_EQ(UrlGetPartA("not a url", part, &n, 1, 0), (HRESULT)0x80041001);
		n = 4;
		CHECK_EQ(UrlGetPartA(url, part, &n, 2, 0), E_POINTER);
		CHECK_EQ(n, 12);			/* "example.com" and its NUL */
		static const WCHAR wurl[] = { 'f', 't', 'p', ':', '/', '/', 'h', '.', 'x', '/', 0 };
		WCHAR wpart[16];
		n = 16;
		CHECK_EQ(UrlGetPartW(wurl, wpart, &n, 2, 0), S_OK);
		CHECK(n == 3 && wpart[0] == 'h' && wpart[1] == '.' && wpart[2] == 'x' && wpart[3] == 0);
	}

	/* A file spec is a name with no directory or drive. */
	FN(BOOL, PathIsFileSpecA, LPCSTR);
	FN(BOOL, PathIsFileSpecW, LPCWSTR);
	FN(BOOL, PathRemoveFileSpecW, LPWSTR);
	CHECK(PathIsFileSpecA("codec.ax"));
	CHECK(!PathIsFileSpecA("dir\\codec.ax"));
	CHECK(!PathIsFileSpecA("C:codec.ax"));
	static const WCHAR plain[] = { 'c', '.', 'a', 'x', 0 };
	static const WCHAR rooted[] = { '\\', 'c', '.', 'a', 'x', 0 };
	CHECK(PathIsFileSpecW(plain));
	CHECK(!PathIsFileSpecW(rooted));
	WCHAR w[MAX_PATH] = { 'C', ':', '\\', 'P', 'r', 'o', 'g', '\\', 'x', '.', 'a', 'x', 0 };
	CHECK(PathRemoveFileSpecW(w));
	CHECK_EQ(w[7], 0);
	CHECK_EQ(w[6], 'g');
	/* Non-ASCII names shorten by characters, not bytes. */
	WCHAR u[MAX_PATH] = { 'd', 0x00e9, 'c', '\\', 0x00e9, '.', 'a', 'x', 0 };
	CHECK(PathRemoveFileSpecW(u));
	CHECK_EQ(u[3], 0);
	CHECK_EQ(u[1], 0x00e9);
	WCHAR bare[MAX_PATH] = { 'x', '.', 'a', 'x', 0 };
	CHECK(!PathRemoveFileSpecW(bare));
	CHECK(PathCombineA(b, "C:\\Windows", "system32\\x.dll") == b);
	CHECK_STR(b, "C:\\Windows\\system32\\x.dll");
	CHECK(PathCombineA(b, "C:\\Windows", "\\x.dll") == b);
	CHECK_STR(b, "C:\\x.dll");
	CHECK(PathCombineA(b, "\\\\srv\\share\\dir", "\\x.dll") == b);
	CHECK_STR(b, "\\\\srv\\share\\x.dll");
	CHECK(PathIsRelativeA("plugins\\x.dll"));
	CHECK(!PathIsRelativeA("C:\\x.dll"));
	CHECK(PathIsURLA("https://example.invalid/a"));
	CHECK(!PathIsURLA("C:\\x.dll"));
	CHECK(PathMatchSpecA("MOVIE.AVI", "*.mp4;*.avi"));
	strcpy(b, "C:\\Program Files\\x.dll");
	PathQuoteSpacesA(b);
	CHECK_STR(b, "\"C:\\Program Files\\x.dll\"");
	PathUnquoteSpacesA(b);
	CHECK_STR(b, "C:\\Program Files\\x.dll");
}

static void test_wide(void)
{
	FN(LPWSTR, PathFindExtensionW, LPCWSTR);
	FN(BOOL, PathRenameExtensionW, LPWSTR, LPCWSTR);
	FN(LPWSTR, StrStrIW, LPCWSTR, LPCWSTR);
	WCHAR *p = utf8_to_utf16_dup("C:\\codecs\\film.avi");
	WCHAR *avi = utf8_to_utf16_dup(".avi");
	WCHAR *dll_ext = utf8_to_utf16_dup(".dll");
	WCHAR *film = utf8_to_utf16_dup("FILM");

	CHECK(PathFindExtensionW(p) == p + 14);
	CHECK(!memcmp(PathFindExtensionW(p), avi, 5 * sizeof(WCHAR)));
	CHECK(StrStrIW(p, film) == p + 10);
	CHECK(PathRenameExtensionW(p, dll_ext));
	char *a = utf16_to_utf8_dup(p);
	CHECK_STR(a, "C:\\codecs\\film.dll");
	free(a); free(film); free(dll_ext); free(avi); free(p);

	FN(LPWSTR, PathCombineW, LPWSTR, LPCWSTR, LPCWSTR);
	FN(BOOL, PathIsUNCW, LPCWSTR);
	FN(BOOL, PathIsUNCA, LPCSTR);
	WCHAR out[MAX_PATH], *dir = utf8_to_utf16_dup("C:\\Windows\\");
	WCHAR *rel = utf8_to_utf16_dup("system32\\\u00e9.dll");
	WCHAR *rooted = utf8_to_utf16_dup("\\x.dll");
	WCHAR *abs = utf8_to_utf16_dup("D:\\x.acm");
	CHECK(PathCombineW(out, dir, rel) == out);
	a = utf16_to_utf8_dup(out);
	CHECK_STR(a, "C:\\Windows\\system32\\\u00e9.dll");
	free(a);
	CHECK(PathCombineW(out, dir, abs) == out);
	a = utf16_to_utf8_dup(out);
	CHECK_STR(a, "D:\\x.acm");
	free(a);
	CHECK(PathCombineW(out, dir, rooted) == out);
	a = utf16_to_utf8_dup(out);
	CHECK_STR(a, "C:\\x.dll");
	free(a);
	CHECK(PathCombineW(out, NULL, abs) == out);
	CHECK(!PathCombineW(out, dir, NULL) && !out[0]);
	CHECK(PathIsUNCA("\\\\server\\share") && !PathIsUNCA("C:\\x") &&
	      !PathIsUNCA("\\\\?\\C:\\x") && !PathIsUNCA(NULL));
	WCHAR *unc = utf8_to_utf16_dup("\\\\s\\x");
	CHECK(PathIsUNCW(unc) && !PathIsUNCW(dir));
	free(unc); free(dir); free(rel); free(rooted); free(abs);
}

/* kernelbase.dll: kernel32's functions and shlwapi's path ones. */
static void test_kernelbase(void)
{
	struct module *kb = mod_load("kernelbase.dll", NULL);
	CHECK(kb && kb->builtin);
	if (!kb)
		return;
	CHECK(mod_get_proc(kb, "HeapAlloc") != NULL);
	CHECK(mod_get_proc(kb, "GetProcAddress") != NULL);
	CHECK(mod_get_proc(kb, "PathCombineW") != NULL);
	CHECK(mod_get_proc(kb, "PathIsUNCW") != NULL);
	CHECK(mod_get_proc(kb, "StrStrIW") != NULL);
	mod_free(kb);
}

int main(void)
{
	CHECK_EQ(runtime_enter(), 0);
	dll = mod_load("SHLWAPI.DLL", NULL);
	CHECK(dll && dll->builtin && !dll->builtin->fallback);
	if (dll) {
		test_strings();
		test_paths();
		test_wide();
		mod_free(dll);
	}
	test_kernelbase();
	return test_summary("test_shlwapi");
}
