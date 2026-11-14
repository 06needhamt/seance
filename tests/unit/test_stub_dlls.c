/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * The system DLLs that only say their hardware or network isn't there
 * (ddraw, dsound, dinput, iphlpapi, msimg32, urlmon, wininet, gdiplus), and
 * wininet's URL helpers, which really work.
 */
#include <stdlib.h>
#include <string.h>

#include "loader/modules.h"
#include "runtime/runtime.h"
#include "test.h"

static void *get(struct module *m, const char *name)
{
	void *p = mod_get_proc(m, name);
	if (!p) {
		fprintf(stderr, "no %s\n", name);
		exit(1);
	}
	return p;
}

#define FN(m, ret, name, ...) \
	ret (WINAPI *name)(__VA_ARGS__) = (ret (WINAPI *)(__VA_ARGS__))get(m, #name)

static void test_directx(void)
{
	struct module *dd = mod_load("ddraw.dll", NULL);
	struct module *ds = mod_load("dsound.dll", NULL);
	struct module *di = mod_load("dinput.dll", NULL);
	struct module *di8 = mod_load("dinput8.dll", NULL);
	CHECK(dd && ds && di && di8);
	if (!dd || !ds || !di || !di8)
		return;
	FN(dd, HRESULT, DirectDrawCreate, GUID *, void **, IUnknown *);
	FN(dd, HRESULT, DirectDrawCreateEx, GUID *, void **, REFIID, IUnknown *);
	FN(dd, HRESULT, DirectDrawEnumerateA, void *, void *);
	FN(di, HRESULT, DirectInputCreateA, HINSTANCE, DWORD, void **, IUnknown *);
	FN(di8, HRESULT, DirectInput8Create, HINSTANCE, DWORD, REFIID, void **,
	   IUnknown *);
	void *obj = (void *)1;

	CHECK_EQ(DirectDrawCreate(NULL, &obj, NULL), (HRESULT)0x88760208);
	CHECK(obj == NULL);
	obj = (void *)1;
	CHECK_EQ(DirectDrawCreateEx(NULL, &obj, &IID_IUnknown, NULL), (HRESULT)0x88760208);
	CHECK(obj == NULL);
	CHECK_EQ(DirectDrawEnumerateA(NULL, NULL), S_OK);

	/* dsound is imported by ordinal: 1 is DirectSoundCreate. */
	HRESULT (WINAPI *create)(const GUID *, void **, IUnknown *) =
		(HRESULT (WINAPI *)(const GUID *, void **, IUnknown *))
		mod_get_ordinal(ds, 1);
	CHECK(create != NULL);
	CHECK(create == mod_get_proc(ds, "DirectSoundCreate"));
	obj = (void *)1;
	if (create)
		CHECK_EQ(create(NULL, &obj, NULL), (HRESULT)0x88780078);
	CHECK(obj == NULL);
	CHECK(mod_get_ordinal(ds, 11) == mod_get_proc(ds, "DirectSoundCreate8"));

	obj = (void *)1;
	CHECK(FAILED(DirectInputCreateA(NULL, 0x0700, &obj, NULL)) && !obj);
	obj = (void *)1;
	CHECK(FAILED(DirectInput8Create(NULL, 0x0800, &IID_IUnknown, &obj, NULL)) && !obj);
	mod_free(di8);
	mod_free(di);
	mod_free(ds);
	mod_free(dd);
}

static void test_iphlpapi_msimg32(void)
{
	struct module *ip = mod_load("iphlpapi.dll", NULL);
	struct module *mi = mod_load("msimg32.dll", NULL);
	CHECK(ip && mi);
	if (!ip || !mi)
		return;
	FN(ip, DWORD, GetAdaptersInfo, void *, ULONG *);
	FN(ip, DWORD, GetNumberOfInterfaces, DWORD *);
	FN(mi, BOOL, AlphaBlend, HDC, int, int, int, int, HDC, int, int, int, int, DWORD);
	ULONG size = 0;
	DWORD n = 5;

	CHECK_EQ(GetAdaptersInfo(NULL, &size), 232);	/* ERROR_NO_DATA */
	CHECK_EQ(GetNumberOfInterfaces(&n), 0);
	CHECK_EQ(n, 0);
	CHECK(!AlphaBlend(NULL, 0, 0, 1, 1, NULL, 0, 0, 1, 1, 0));
	mod_free(mi);
	mod_free(ip);
}

static void test_urlmon(void)
{
	struct module *m = mod_load("urlmon.dll", NULL);
	CHECK(m != NULL);
	if (!m)
		return;
	FN(m, HRESULT, URLDownloadToFileA, void *, LPCSTR, LPCSTR, DWORD, void *);
	FN(m, HRESULT, CreateURLMoniker, void *, LPCWSTR, void **);
	FN(m, HRESULT, UrlMkSetSessionOption, DWORD, void *, DWORD, DWORD);
	WCHAR url[] = { 'h', 't', 't', 'p', ':', '/', '/', 'x', 0 };
	void *mk = (void *)1;

	CHECK_EQ(URLDownloadToFileA(NULL, "http://x/y", "C:\\y", 0, NULL),
		 (HRESULT)0x800C0008);
	CHECK(FAILED(CreateURLMoniker(NULL, url, &mk)) && !mk);
	CHECK_EQ(UrlMkSetSessionOption(0, NULL, 0, 0), S_OK);
	mod_free(m);
}

static int wstr_eq(const WCHAR *w, const char *s)
{
	while (*s && *w == (BYTE)*s) {
		w++;
		s++;
	}
	return !*s && !*w;
}

static void test_wininet(void)
{
	struct module *m = mod_load("wininet.dll", NULL);
	struct module *k = mod_load("kernel32.dll", NULL);
	CHECK(m && k);
	if (!m || !k)
		return;
	FN(m, HINTERNET, InternetOpenA, LPCSTR, DWORD, LPCSTR, LPCSTR, DWORD);
	FN(m, HINTERNET, InternetOpenUrlA, HINTERNET, LPCSTR, LPCSTR, DWORD, DWORD,
	   DWORD_PTR);
	FN(m, HINTERNET, InternetConnectA, HINTERNET, LPCSTR, INTERNET_PORT, LPCSTR,
	   LPCSTR, DWORD, DWORD, DWORD_PTR);
	FN(m, BOOL, InternetCloseHandle, HINTERNET);
	FN(m, BOOL, InternetReadFile, HINTERNET, LPVOID, DWORD, LPDWORD);
	FN(m, BOOL, InternetGetConnectedState, LPDWORD, DWORD);
	FN(m, BOOL, InternetCrackUrlA, LPCSTR, DWORD, DWORD, LPURL_COMPONENTSA);
	FN(m, BOOL, InternetCrackUrlW, LPCWSTR, DWORD, DWORD, LPURL_COMPONENTSW);
	FN(m, BOOL, InternetCanonicalizeUrlA, LPCSTR, LPSTR, LPDWORD, DWORD);
	FN(m, BOOL, InternetCombineUrlA, LPCSTR, LPCSTR, LPSTR, LPDWORD, DWORD);
	FN(m, BOOL, InternetCreateUrlA, LPURL_COMPONENTSA, DWORD, LPSTR, LPDWORD);
	FN(k, DWORD, GetLastError, void);

	/* Offline: a session opens, connections don't. */
	HINTERNET s = InternetOpenA("seance", 0, NULL, NULL, 0);
	CHECK(s != NULL);
	CHECK(InternetOpenUrlA(s, "http://example.com/", NULL, 0, 0, 0) == NULL);
	CHECK_EQ(GetLastError(), ERROR_INTERNET_DISCONNECTED);
	CHECK(InternetConnectA(s, "example.com", 80, NULL, NULL, 3, 0, 0) == NULL);
	DWORD n = 1, flags = 0;
	char buf[256];
	CHECK(!InternetReadFile(s, buf, 1, &n) && n == 0);
	CHECK_EQ(GetLastError(), ERROR_INTERNET_INCORRECT_HANDLE_TYPE);
	CHECK(!InternetGetConnectedState(&flags, 0));
	CHECK_EQ(flags, INTERNET_CONNECTION_OFFLINE);
	CHECK(InternetCloseHandle(s));
	CHECK(!InternetCloseHandle((HINTERNET)0x1234));

	/* Cracking, into the URL and into buffers. */
	const char *url = "http://user:pw@www.example.com:8080/dir/file.avi?x=1#top";
	URL_COMPONENTSA c;
	char host[64], path[64];
	memset(&c, 0, sizeof(c));
	c.dwStructSize = sizeof(c);
	c.dwSchemeLength = 1;
	c.dwUserNameLength = 1;
	c.dwPasswordLength = 1;
	c.lpszHostName = host;
	c.dwHostNameLength = sizeof(host);
	c.lpszUrlPath = path;
	c.dwUrlPathLength = sizeof(path);
	c.dwExtraInfoLength = 1;
	CHECK(InternetCrackUrlA(url, 0, 0, &c));
	CHECK(c.lpszScheme == url && c.dwSchemeLength == 4);
	CHECK_EQ(c.nScheme, INTERNET_SCHEME_HTTP);
	CHECK_EQ(c.nPort, 8080);
	CHECK(c.dwUserNameLength == 4 && !strncmp(c.lpszUserName, "user", 4));
	CHECK(c.dwPasswordLength == 2 && !strncmp(c.lpszPassword, "pw", 2));
	CHECK_STR(host, "www.example.com");
	CHECK_EQ(c.dwHostNameLength, 15);
	CHECK_STR(path, "/dir/file.avi");
	CHECK(c.dwExtraInfoLength == 8 && !strncmp(c.lpszExtraInfo, "?x=1#top", 8));
	/* A buffer too small. */
	char tiny[4];
	memset(&c, 0, sizeof(c));
	c.dwStructSize = sizeof(c);
	c.lpszHostName = tiny;
	c.dwHostNameLength = sizeof(tiny);
	CHECK(!InternetCrackUrlA(url, 0, 0, &c));
	CHECK_EQ(GetLastError(), ERROR_INSUFFICIENT_BUFFER);
	/* Default ports, and no scheme at all. */
	memset(&c, 0, sizeof(c));
	c.dwStructSize = sizeof(c);
	CHECK(InternetCrackUrlA("https://h/", 0, 0, &c) && c.nPort == 443);
	CHECK(!InternetCrackUrlA("no scheme", 0, 0, &c));
	/* The wide form points into the wide string. */
	WCHAR wurl[] = { 'f', 't', 'p', ':', '/', '/', 'h', 'o', 's', 't', '/', 'a', 0 };
	URL_COMPONENTSW cw;
	memset(&cw, 0, sizeof(cw));
	cw.dwStructSize = sizeof(cw);
	cw.dwHostNameLength = 1;
	cw.dwUrlPathLength = 1;
	CHECK(InternetCrackUrlW(wurl, 0, 0, &cw));
	CHECK(cw.lpszHostName == wurl + 6 && cw.dwHostNameLength == 4);
	CHECK(cw.lpszUrlPath == wurl + 10 && cw.dwUrlPathLength == 2);
	CHECK(cw.nScheme == INTERNET_SCHEME_FTP && cw.nPort == 21);

	n = sizeof(buf);
	CHECK(InternetCanonicalizeUrlA("http://h/a b<c>", buf, &n, 0));
	CHECK_STR(buf, "http://h/a%20b%3Cc%3E");
	CHECK_EQ(n, strlen(buf));
	n = sizeof(buf);
	CHECK(InternetCanonicalizeUrlA("http://h/a%20b", buf, &n, ICU_DECODE | ICU_NO_ENCODE));
	CHECK_STR(buf, "http://h/a b");
	n = 3;
	CHECK(!InternetCanonicalizeUrlA("http://h/", buf, &n, 0));
	CHECK_EQ(GetLastError(), ERROR_INSUFFICIENT_BUFFER);
	CHECK_EQ(n, 10);

	static const struct {
		const char *base, *rel, *want;
	} combos[] = {
		{ "http://h/d/f.avi", "g.srt", "http://h/d/g.srt" },
		{ "http://h/d/f.avi", "/r/g", "http://h/r/g" },
		{ "http://h/d/f.avi", "ftp://o/x", "ftp://o/x" },
		{ "http://h/d/f.avi", "//o/x", "http://o/x" },
		{ "http://h", "x", "http://h/x" },
		{ "http://h/d/f?q", "?r", "http://h/d/f?r" },
	};
	for (size_t i = 0; i < sizeof(combos) / sizeof(combos[0]); i++) {
		n = sizeof(buf);
		CHECK(InternetCombineUrlA(combos[i].base, combos[i].rel, buf, &n, 0));
		CHECK_STR(buf, combos[i].want);
	}

	memset(&c, 0, sizeof(c));
	c.dwStructSize = sizeof(c);
	c.nScheme = INTERNET_SCHEME_HTTP;
	c.lpszHostName = "example.com";
	c.nPort = 80;
	c.lpszUrlPath = "/a.avi";
	n = sizeof(buf);
	CHECK(InternetCreateUrlA(&c, 0, buf, &n));
	CHECK_STR(buf, "http://example.com/a.avi");
	c.nPort = 81;
	c.lpszUserName = "u";
	n = sizeof(buf);
	CHECK(InternetCreateUrlA(&c, 0, buf, &n));
	CHECK_STR(buf, "http://u@example.com:81/a.avi");
	(void)wstr_eq;
	mod_free(k);
	mod_free(m);
}

typedef struct {
	UINT GdiplusVersion;
	void *DebugEventCallback;
	BOOL SuppressBackgroundThread;
	BOOL SuppressExternalCodecs;
} GdiplusStartupInput;

static void test_gdiplus(void)
{
	struct module *m = mod_load("gdiplus.dll", NULL);
	CHECK(m != NULL);
	if (!m)
		return;
	FN(m, int, GdiplusStartup, ULONG_PTR *, const GdiplusStartupInput *, void *);
	FN(m, void, GdiplusShutdown, ULONG_PTR);
	FN(m, void *, GdipAlloc, size_t);
	FN(m, void, GdipFree, void *);
	FN(m, int, GdipCreateBitmapFromScan0, int, int, int, int, BYTE *, void **);
	FN(m, int, GdipGetImageEncodersSize, UINT *, UINT *);
	FN(m, int, GdipDisposeImage, void *);
	FN(m, int, GdipDrawImageRectRectI, void *, void *, int, int, int, int, int,
	   int, int, int, int, void *, void *, void *);
	ULONG_PTR token = 0;
	GdiplusStartupInput in = { 1, NULL, FALSE, FALSE };

	CHECK_EQ(GdiplusStartup(&token, &in, NULL), 0);
	CHECK(token != 0);
	void *p = GdipAlloc(16);
	CHECK(p != NULL);
	memset(p, 0, 16);
	GdipFree(p);
	void *bmp = (void *)1;
	CHECK_EQ(GdipCreateBitmapFromScan0(16, 16, 64, 0x26200a, NULL, &bmp), 6);
	CHECK(bmp == NULL);
	UINT num = 9, size = 9;
	CHECK_EQ(GdipGetImageEncodersSize(&num, &size), 0);
	CHECK(num == 0 && size == 0);
	CHECK_EQ(GdipDisposeImage(NULL), 2);
	/* Fourteen stdcall arguments, popped by the callee. */
	CHECK_EQ(GdipDrawImageRectRectI((void *)1, (void *)1, 0, 0, 1, 1, 0, 0, 1,
					1, 2, NULL, NULL, NULL), 6);
	GdiplusShutdown(token);
	in.GdiplusVersion = 9;
	CHECK_EQ(GdiplusStartup(&token, &in, NULL), 2);
	mod_free(m);
}

int main(void)
{
	char root[256];
	const char *tmpdir = getenv("TMPDIR");

	snprintf(root, sizeof(root), "%s/vfw-stubs-XXXXXX",
		 tmpdir && *tmpdir ? tmpdir : "/tmp");
	if (!mkdtemp(root)) {
		perror("mkdtemp");
		return 1;
	}
	setenv("VFW_ROOT", root, 1);
	CHECK_EQ(runtime_enter(), 0);

	test_directx();
	test_iphlpapi_msimg32();
	test_urlmon();
	test_wininet();
	test_gdiplus();

	char cmd[512];
	snprintf(cmd, sizeof(cmd), "rm -rf '%s'", root);
	if (system(cmd))
		fprintf(stderr, "could not remove %s\n", root);
	return test_summary("test_stub_dlls");
}
