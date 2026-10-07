/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * bcrypt, crypt32 and d3d9, which hold just what video filters and the
 * libraries they carry call, and the msvcrt functions added for them
 * (descriptors, threads, gmtime, ...).
 */
#include <errno.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include "loader/modules.h"
#include "runtime/runtime.h"
#include "test.h"

#define STATUS_SUCCESS       ((LONG)0x00000000)
#define STATUS_INVALID_HANDLE ((LONG)0xC0000008)
#define STATUS_NOT_FOUND     ((LONG)0xC0000225)

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
/* The C runtime's functions are cdecl. */
#define CFN(m, ret, name, ...) \
	ret (*name)(__VA_ARGS__) = (ret (*)(__VA_ARGS__))get(m, #name)

static void test_bcrypt(void)
{
	struct module *m = mod_load("bcrypt.dll", NULL);
	CHECK(m != NULL);
	if (!m)
		return;
	FN(m, LONG, BCryptOpenAlgorithmProvider, void **, LPCWSTR, LPCWSTR, ULONG);
	FN(m, LONG, BCryptCloseAlgorithmProvider, void *, ULONG);
	FN(m, LONG, BCryptGenRandom, void *, BYTE *, ULONG, ULONG);

	static const WCHAR rng[] = { 'r', 'N', 'g', 0 };
	static const WCHAR sha[] = { 'S', 'H', 'A', '2', '5', '6', 0 };
	void *h = NULL;
	BYTE a[64], b[64];

	CHECK_EQ(BCryptOpenAlgorithmProvider(&h, rng, NULL, 0), STATUS_SUCCESS);
	CHECK(h != NULL);
	memset(a, 0, sizeof(a));
	memset(b, 0, sizeof(b));
	CHECK_EQ(BCryptGenRandom(h, a, sizeof(a), 0), STATUS_SUCCESS);
	CHECK_EQ(BCryptGenRandom(h, b, sizeof(b), 0), STATUS_SUCCESS);
	CHECK(memcmp(a, b, sizeof(a)) != 0);
	CHECK(memcmp(a, b, sizeof(a)) != 0 && memcmp(a, "\0\0\0\0\0\0\0\0", 8) != 0);
	CHECK_EQ(BCryptCloseAlgorithmProvider(h, 0), STATUS_SUCCESS);

	/* The system RNG needs no provider; anything else needs a valid one. */
	CHECK_EQ(BCryptGenRandom(NULL, a, sizeof(a), 2), STATUS_SUCCESS);
	CHECK_EQ(BCryptGenRandom(NULL, a, sizeof(a), 0), STATUS_INVALID_HANDLE);
	CHECK_EQ(BCryptCloseAlgorithmProvider(NULL, 0), STATUS_INVALID_HANDLE);

	/* Other algorithms are unknown here. */
	CHECK_EQ(BCryptOpenAlgorithmProvider(&h, sha, NULL, 0), STATUS_NOT_FOUND);
	mod_free(m);
}

static void test_crypt32(void)
{
	struct module *m = mod_load("crypt32.dll", NULL);
	CHECK(m != NULL);
	if (!m)
		return;
	FN(m, HANDLE, CertOpenStore, LPCSTR, DWORD, ULONG_PTR, DWORD, const void *);
	FN(m, BOOL, CertCloseStore, HANDLE, DWORD);
	FN(m, const void *, CertEnumCertificatesInStore, HANDLE, const void *);
	FN(m, const void *, CertEnumCRLsInStore, HANDLE, const void *);
	FN(m, const void *, CertFindCertificateInStore, HANDLE, DWORD, DWORD, DWORD,
	   const void *, const void *);
	FN(m, BOOL, CertFreeCertificateContext, const void *);
	FN(m, BOOL, CertGetCertificateContextProperty, const void *, DWORD, void *,
	   DWORD *);
	FN(m, HANDLE, PFXImportCertStore, const void *, LPCWSTR, DWORD);
	struct module *k = mod_load("kernel32.dll", NULL);
	DWORD (WINAPI *last)(void) = get(k, "GetLastError");

	HANDLE s = CertOpenStore((LPCSTR)10 /* CERT_STORE_PROV_SYSTEM_W */, 0, 0,
				 0x10000, NULL);
	CHECK(s != NULL);
	CHECK(CertEnumCertificatesInStore(s, NULL) == NULL);
	CHECK_EQ(last(), 0x80092004u);
	CHECK(CertEnumCRLsInStore(s, NULL) == NULL);
	CHECK(CertFindCertificateInStore(s, 0x10001, 0, 0, NULL, NULL) == NULL);
	DWORD n = 0;
	CHECK(!CertGetCertificateContextProperty((void *)1, 3, NULL, &n));
	CHECK(CertFreeCertificateContext(NULL));
	CHECK(CertCloseStore(s, 0));
	CHECK(PFXImportCertStore(NULL, NULL, 0) == NULL);
	mod_free(k);
	mod_free(m);
}

static void test_d3d9(void)
{
	struct module *m = mod_load("d3d9.dll", NULL);
	CHECK(m != NULL);
	if (!m)
		return;
	FN(m, void *, Direct3DCreate9, UINT);
	FN(m, HRESULT, Direct3DCreate9Ex, UINT, void **);

	/* No adapter: what a decoder takes as "decode in software". */
	CHECK(Direct3DCreate9(32) == NULL);
	void *d3d = (void *)1;
	CHECK_EQ(Direct3DCreate9Ex(32, &d3d), (HRESULT)0x8876086A);
	CHECK(d3d == NULL);
	mod_free(m);
}

static unsigned WINAPI thread_body(void *arg)
{
	*(int *)arg = 77;
	return 5;
}

static void test_msvcrt(void)
{
	struct module *c = mod_load("msvcrt.dll", NULL);
	struct module *k = mod_load("kernel32.dll", NULL);
	CHECK(c && k);
	if (!c || !k)
		return;

	/* Threads. */
	CFN(c, uintptr_t, _beginthreadex, void *, unsigned, unsigned (WINAPI *)(void *),
	    void *, unsigned, unsigned *);
	FN(k, DWORD, WaitForSingleObject, HANDLE, DWORD);
	FN(k, BOOL, GetExitCodeThread, HANDLE, LPDWORD);
	FN(k, BOOL, CloseHandle, HANDLE);
	int value = 0;
	unsigned tid = 0;
	uintptr_t t = _beginthreadex(NULL, 0, thread_body, &value, 0, &tid);
	CHECK(t != 0);
	CHECK(tid != 0);
	CHECK_EQ(WaitForSingleObject((HANDLE)t, 10000), WAIT_OBJECT_0);
	DWORD code = 0;
	CHECK(GetExitCodeThread((HANDLE)t, &code));
	CHECK_EQ(code, 5);
	CHECK_EQ(value, 77);
	CloseHandle((HANDLE)t);

	/* Descriptors, over a Windows path. */
	CFN(c, int, _open, const char *, int, ...);
	CFN(c, int, _close, int);
	CFN(c, int, _read, int, void *, unsigned);
	CFN(c, int, _write, int, const void *, unsigned);
	CFN(c, long long, _lseeki64, int, long long, int);
	CFN(c, int, _dup, int);
	CFN(c, int, _dup2, int, int);
	CFN(c, int, _isatty, int);
	CFN(c, int, _setmode, int, int);
	CFN(c, intptr_t, _get_osfhandle, int);
	CFN(c, int, _unlink, const char *);
	CFN(c, int, _mkdir, const char *);
	CFN(c, int, _rmdir, const char *);
	CFN(c, int, _sopen, const char *, int, int, ...);

	CHECK_EQ(_mkdir("C:\\WINDOWS\\TEMP\\lowio"), 0);
	int fd = _open("C:\\WINDOWS\\TEMP\\lowio\\f.bin",
		       0x0002 | 0x0100 | 0x8000, 0600);	/* RDWR|CREAT|BINARY */
	CHECK(fd >= 3);
	CHECK_EQ(_write(fd, "hello world", 11), 11);
	CHECK_EQ(_lseeki64(fd, 6, 0), 6);
	char buf[8] = { 0 };
	CHECK_EQ(_read(fd, buf, 5), 5);
	CHECK_STR(buf, "world");
	CHECK_EQ(_setmode(fd, 0x4000), 0x8000);
	CHECK_EQ(_setmode(9999, 0x4000), -1);
	CHECK(!_isatty(fd));
	int copy = _dup(fd);
	CHECK(copy >= 3 && copy != fd);
	CHECK_EQ(_lseeki64(copy, 0, 1), 11);	/* shares the position */
	CHECK_EQ(_dup2(copy, copy + 20), 0);
	CHECK_EQ(_close(copy + 20), 0);
	CHECK_EQ(_close(copy), 0);
	CHECK_EQ(_close(fd), 0);
	CHECK_EQ(_close(fd), -1);
	CHECK_EQ(_get_osfhandle(0), -1);	/* no Win32 handle stands behind it */

	/* O_EXCL refuses an existing file; sopen's sharing mode is ignored. */
	CHECK_EQ(_open("C:\\WINDOWS\\TEMP\\lowio\\f.bin", 0x0100 | 0x0400, 0600), -1);
	fd = _sopen("C:\\WINDOWS\\TEMP\\lowio\\f.bin", 0, 0x40);
	CHECK(fd >= 3);
	CHECK_EQ(_close(fd), 0);
	CHECK_EQ(_unlink("C:\\WINDOWS\\TEMP\\lowio\\f.bin"), 0);
	CHECK_EQ(_unlink("C:\\WINDOWS\\TEMP\\lowio\\f.bin"), -1);
	CHECK_EQ(_rmdir("C:\\WINDOWS\\TEMP\\lowio"), 0);

	/* Time: UTC, nine ints in MSVC's order. */
	CFN(c, int *, gmtime, const time_t *);
	CFN(c, int *, __p__daylight, void);
	CFN(c, long *, __p__timezone, void);
	CFN(c, int *, __p__dstbias, void);
	CFN(c, void, _tzset, void);
	time_t when = 1700000000;	/* 2023-11-14 22:13:20 UTC, a Tuesday */
	int *tm = gmtime(&when);
	CHECK(tm != NULL);
	if (tm) {
		CHECK_EQ(tm[0], 20);	/* sec */
		CHECK_EQ(tm[1], 13);	/* min */
		CHECK_EQ(tm[2], 22);	/* hour */
		CHECK_EQ(tm[3], 14);	/* mday */
		CHECK_EQ(tm[4], 10);	/* mon, from 0 */
		CHECK_EQ(tm[5], 123);	/* years since 1900 */
		CHECK_EQ(tm[6], 2);	/* wday */
		CHECK_EQ(tm[7], 317);	/* yday */
		CHECK_EQ(tm[8], 0);	/* isdst */
	}
	_tzset();
	CHECK_EQ(*__p__daylight(), 0);
	CHECK_EQ(*__p__timezone(), 0);
	CHECK_EQ(*__p__dstbias(), 0);

	/* Strings, allocation, stdio accessors. */
	CFN(c, WCHAR *, wcsrchr, const WCHAR *, WCHAR);
	static const WCHAR path[] = { 'a', '\\', 'b', '\\', 'c', 0 };
	CHECK(wcsrchr(path, '\\') == path + 3);
	CHECK(wcsrchr(path, 'z') == NULL);
	CHECK(wcsrchr(path, 0) == path + 5);
	CFN(c, void *, malloc, size_t);
	CFN(c, void, free, void *);
	CFN(c, size_t, _msize, void *);
	void *p = malloc(100);
	CHECK(_msize(p) >= 100);
	free(p);
	CFN(c, void *, __p__iob, void);
	CFN(c, int *, __p___mb_cur_max, void);
	CFN(c, void *, __iob_func, void);
	CHECK(__p__iob() == __iob_func());
	CHECK_EQ(*__p___mb_cur_max(), 1);
	unsigned *codepage = get(c, "__lc_codepage");
	CHECK_EQ(*codepage, 0);
	CFN(c, void, __setusermatherr, void *);
	__setusermatherr(NULL);

	mod_free(k);
	mod_free(c);
}

int main(void)
{
	char root[256];
	const char *tmpdir = getenv("TMPDIR");

	snprintf(root, sizeof(root), "%s/vfw-extras-XXXXXX",
		 tmpdir && *tmpdir ? tmpdir : "/tmp");
	if (!mkdtemp(root)) {
		perror("mkdtemp");
		return 1;
	}
	setenv("VFW_ROOT", root, 1);
	CHECK_EQ(runtime_enter(), 0);

	test_bcrypt();
	test_crypt32();
	test_d3d9();
	test_msvcrt();

	char cmd[512];
	snprintf(cmd, sizeof(cmd), "rm -rf '%s'", root);
	if (system(cmd))
		fprintf(stderr, "could not remove %s\n", root);
	return test_summary("test_win32_extras");
}
