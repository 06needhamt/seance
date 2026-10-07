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
	return test_summary("test_shlwapi");
}
