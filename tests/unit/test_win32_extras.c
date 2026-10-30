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

static size_t wcslen16(const WCHAR *s)
{
	size_t n = 0;

	while (s[n])
		n++;
	return n;
}

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
	CFN(c, WCHAR *, wcschr, const WCHAR *, WCHAR);
	CFN(c, WCHAR *, wcsstr, const WCHAR *, const WCHAR *);
	CFN(c, WCHAR *, wcsncpy, WCHAR *, const WCHAR *, size_t);
	CFN(c, WCHAR *, wcsncat, WCHAR *, const WCHAR *, size_t);
	CFN(c, size_t, wcsspn, const WCHAR *, const WCHAR *);
	CFN(c, size_t, wcscspn, const WCHAR *, const WCHAR *);
	CFN(c, WCHAR *, wcspbrk, const WCHAR *, const WCHAR *);
	CFN(c, WCHAR *, _wcsdup, const WCHAR *);
	CFN(c, int, _wcsicmp, const WCHAR *, const WCHAR *);
	CFN(c, int, _wcsnicmp, const WCHAR *, const WCHAR *, size_t);
	CFN(c, WCHAR *, _wcslwr, WCHAR *);
	CFN(c, WCHAR *, _wcsupr, WCHAR *);
	static const WCHAR seps[] = { '\\', 'c', 0 }, bc[] = { 'b', '\\', 'c', 0 };
	static const WCHAR upper[] = { 'A', '\\', 'B', 0 }, none[] = { 0 };
	CHECK(wcschr(path, '\\') == path + 1);
	CHECK(wcschr(path, 'z') == NULL);
	CHECK(wcschr(path, 0) == path + 5);
	CHECK(wcsstr(path, bc) == path + 2);
	CHECK(wcsstr(path, upper) == NULL);
	CHECK(wcsstr(path, none) == path);
	CHECK_EQ(wcsspn(path, (const WCHAR[]){ 'a', '\\', 0 }), 2);
	CHECK_EQ(wcscspn(path, seps), 1);
	CHECK(wcspbrk(path, seps) == path + 1);
	CHECK(wcspbrk(path, (const WCHAR[]){ 'q', 0 }) == NULL);
	WCHAR wbuf[8];
	memset(wbuf, 0xff, sizeof(wbuf));
	CHECK(wcsncpy(wbuf, path, 3) == wbuf);
	CHECK(wbuf[2] == 'b' && wbuf[3] == 0xffff);	/* not ended */
	wcsncpy(wbuf, bc, 6);
	CHECK(wbuf[3] == 0 && wbuf[5] == 0 && wbuf[6] == 0xffff);
	wcsncat(wbuf, path, 2);
	CHECK(wbuf[3] == 'a' && wbuf[4] == '\\' && wbuf[5] == 0);
	CHECK_EQ(_wcsicmp(path, (const WCHAR[]){ 'A', '\\', 'B', '\\', 'C', 0 }), 0);
	CHECK(_wcsicmp(path, upper) > 0);
	CHECK_EQ(_wcsnicmp(path, upper, 3), 0);
	WCHAR *dup = _wcsdup(path);
	CHECK(dup && !_wcsicmp(dup, path));
	_wcsupr(dup);
	CHECK(dup[0] == 'A' && dup[1] == '\\' && dup[4] == 'C');
	_wcslwr(dup);
	CHECK(dup[0] == 'a' && dup[4] == 'c');
	free(dup);
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

	/* kernel32's copies of ntdll's (msrle32.dll imports RtlMoveMemory). */
	FN(k, void, RtlMoveMemory, void *, const void *, SIZE_T);
	FN(k, void, RtlZeroMemory, void *, SIZE_T);
	FN(k, void, RtlFillMemory, void *, SIZE_T, BYTE);
	BYTE mem[8] = { 1, 2, 3, 4, 5, 6, 7, 8 };
	RtlMoveMemory(mem + 1, mem, 4);		/* overlapping */
	CHECK(mem[0] == 1 && mem[1] == 1 && mem[4] == 4 && mem[5] == 6);
	RtlFillMemory(mem, 3, 0xab);
	CHECK(mem[0] == 0xab && mem[2] == 0xab && mem[3] == 3);
	RtlZeroMemory(mem + 4, 4);
	CHECK(mem[3] == 3 && mem[4] == 0 && mem[7] == 0);

	mod_free(k);
	mod_free(c);
}

/* ws2_32: Winsock for a machine with no network. */

#define WSAEINVAL 10022
#define WSAENOTSOCK 10038
#define WSAENETDOWN 10050
#define WSANOTINITIALISED 10093
#define WSAHOST_NOT_FOUND 11001

static void test_winsock(void)
{
	struct module *m = mod_load("ws2_32.dll", NULL);
	struct module *alias = mod_load("wsock32.dll", NULL);
	CHECK(m != NULL && alias != NULL);
	if (!m)
		return;
	FN(m, int, WSAStartup, WORD, void *);
	FN(m, int, WSACleanup, void);
	FN(m, int, WSAGetLastError, void);
	FN(m, UINT_PTR, socket, int, int, int);
	FN(m, int, closesocket, UINT_PTR);
	FN(m, int, connect, UINT_PTR, const void *, int);
	FN(m, int, select, int, void *, void *, void *, const void *);
	FN(m, int, __WSAFDIsSet, UINT_PTR, void *);
	FN(m, int, getaddrinfo, const char *, const char *, const void *, void **);
	FN(m, void, freeaddrinfo, void *);
	FN(m, int, getnameinfo, const void *, int, char *, DWORD, char *, DWORD, int);
	FN(m, int, gethostname, char *, int);
	FN(m, int, inet_pton, int, const char *, void *);
	FN(m, const char *, inet_ntop, int, const void *, char *, SIZE_T);
	FN(m, unsigned short, htons, unsigned short);
	FN(m, DWORD, ntohl, DWORD);
	FN(m, DWORD, htonl, DWORD);
	FN(m, DWORD, inet_addr, const char *);
	FN(m, char *, inet_ntoa, DWORD);
	FN(m, void *, gethostbyname, const char *);
	struct { WORD ver, high; char d[257], s[129]; unsigned short a, b; char *v; } data;

	/* Nothing works before it is started. */
	CHECK_EQ(socket(2, 1, 6), ~(UINT_PTR)0);
	CHECK_EQ(WSAGetLastError(), WSANOTINITIALISED);
	CHECK_EQ(WSACleanup(), -1);
	CHECK_EQ(WSAStartup(0x0003, &data), 10092);	/* WSAVERNOTSUPPORTED: 3.0 */
	CHECK_EQ(WSAStartup(0x0202, &data), 0);
	CHECK_EQ(data.ver, 0x0202);
	CHECK_EQ(data.high, 0x0202);

	/* There is no network: no socket can be made, and no handle is one. */
	CHECK_EQ(socket(2, 1, 6), ~(UINT_PTR)0);
	CHECK_EQ(WSAGetLastError(), WSAENETDOWN);
	CHECK_EQ(socket(99, 1, 0), ~(UINT_PTR)0);
	CHECK_EQ(WSAGetLastError(), 10047);	/* WSAEAFNOSUPPORT */
	CHECK_EQ(closesocket(5), -1);
	CHECK_EQ(WSAGetLastError(), WSAENOTSOCK);
	CHECK_EQ(connect(5, NULL, 0), -1);
	CHECK_EQ(WSAGetLastError(), WSAENOTSOCK);
	CHECK_EQ(select(0, NULL, NULL, NULL, NULL), -1);
	CHECK_EQ(WSAGetLastError(), WSAEINVAL);
	struct { UINT n; UINT_PTR s[64]; } set = { 2, { 7, 9 } };
	CHECK_EQ(__WSAFDIsSet(9, &set), 1);
	CHECK_EQ(__WSAFDIsSet(8, &set), 0);

	/* The pure functions work. */
	CHECK_EQ(htons(0x1234), 0x3412);
	CHECK_EQ(ntohl(0x01020304), 0x04030201);
	CHECK_EQ(htonl(0x01020304), 0x04030201);
	CHECK_EQ(inet_addr("10.1.2.3"), 0x0302010a);
	CHECK_EQ(inet_addr("10.1.2.300"), 0xffffffffu);	/* INADDR_NONE */
	CHECK_STR(inet_ntoa(0x0302010a), "10.1.2.3");
	struct { char *name; char **aliases; short type, len; char **addrs; } *h =
		gethostbyname("10.1.2.3");
	CHECK(h && h->type == 2 && h->len == 4 && h->addrs[0] &&
	      !memcmp(h->addrs[0], "\x0a\x01\x02\x03", 4) && !h->addrs[1] &&
	      !strcmp(h->name, "10.1.2.3"));
	CHECK(gethostbyname("example.invalid") == NULL);
	CHECK_EQ(WSAGetLastError(), WSAHOST_NOT_FOUND);

	/*
	 * Codecs import wsock32 by ordinal (wmsdmoe2.dll: 8, htonl); its
	 * ordinals part from ws2_32's at 10.
	 */
	if (alias) {
		CHECK(mod_get_ordinal(alias, 8) == (void *)htonl);
		CHECK(mod_get_ordinal(m, 8) == (void *)htonl);
		CHECK(mod_get_ordinal(alias, 10) == (void *)inet_addr);
		CHECK(mod_get_ordinal(m, 11) == (void *)inet_addr);
		CHECK(mod_get_ordinal(m, 10) == get(m, "ioctlsocket"));
		CHECK(mod_get_ordinal(alias, 12) == get(m, "ioctlsocket"));
		CHECK(mod_get_ordinal(alias, 115) == (void *)WSAStartup);
	}
	BYTE addr[16];
	char text[64];
	CHECK_EQ(inet_pton(2, "192.168.1.20", addr), 1);
	CHECK(addr[0] == 192 && addr[3] == 20);
	CHECK_EQ(inet_pton(2, "300.1.1.1", addr), 0);
	CHECK_EQ(inet_pton(23, "::1", addr), 1);	/* Windows' AF_INET6 is 23 */
	CHECK(inet_ntop(23, addr, text, sizeof(text)) == text);
	CHECK_STR(text, "::1");
	CHECK_EQ(inet_pton(99, "x", addr), -1);
	char host[32];
	CHECK_EQ(gethostname(host, sizeof(host)), 0);
	CHECK(host[0] != 0);

	/* Numeric addresses resolve; names don't. */
	struct { int flags, family, type, proto; SIZE_T len; char *canon; void *addr; void *next; } *ai = NULL;
	void *res = NULL;
	CHECK_EQ(getaddrinfo("10.1.2.3", "8080", NULL, &res), 0);
	ai = res;
	CHECK(ai != NULL);
	if (ai) {
		struct { short family; unsigned short port; BYTE addr[4]; } *sa = ai->addr;
		char name[64], serv[16];

		CHECK_EQ(ai->family, 2);
		CHECK_EQ(ai->len, 16);
		CHECK_EQ(sa->family, 2);
		CHECK_EQ(sa->port, htons(8080));
		CHECK(sa->addr[0] == 10 && sa->addr[3] == 3);
		CHECK_EQ(getnameinfo(ai->addr, (int)ai->len, name, sizeof(name), serv,
				     sizeof(serv), 3), 0);
		CHECK_STR(name, "10.1.2.3");
		CHECK_STR(serv, "8080");
		freeaddrinfo(res);
	}
	CHECK_EQ(getaddrinfo("example.invalid", "80", NULL, &res), WSAHOST_NOT_FOUND);
	CHECK(res == NULL);
	CHECK_EQ(getaddrinfo("10.1.2.3", "http", NULL, &res), 10109);

	CHECK_EQ(WSACleanup(), 0);
	CHECK_EQ(WSACleanup(), -1);
	if (alias)
		mod_free(alias);
	mod_free(m);
}

/* ncrypt, and the Direct3D family that isn't there. */

static void test_ncrypt_and_graphics(void)
{
	struct module *m = mod_load("ncrypt.dll", NULL);
	CHECK(m != NULL);
	if (m) {
		FN(m, LONG, BCryptGenRandom, void *, BYTE *, ULONG, ULONG);
		FN(m, LONG, NCryptOpenStorageProvider, void **, LPCWSTR, DWORD);
		FN(m, LONG, NCryptOpenKey, void *, void **, LPCWSTR, DWORD, DWORD);
		FN(m, LONG, NCryptFreeObject, void *);
		FN(m, LONG, NCryptSignHash, void *, void *, BYTE *, DWORD, BYTE *, DWORD,
		   DWORD *, DWORD);
		BYTE r[8];
		void *prov = (void *)1, *key = (void *)1;
		DWORD got = 5;

		/* It carries bcrypt's functions, and no key storage provider. */
		CHECK_EQ(BCryptGenRandom(NULL, r, sizeof(r), 2), 0);
		CHECK_EQ(NCryptOpenStorageProvider(&prov, NULL, 0), (LONG)0x80090029);
		CHECK(prov == NULL);
		CHECK_EQ(NCryptOpenKey(NULL, &key, NULL, 0, 0), (LONG)0x80090026);
		CHECK(key == NULL);
		CHECK_EQ(NCryptFreeObject(NULL), (LONG)0x80090026);
		CHECK_EQ(NCryptSignHash(NULL, NULL, r, 8, r, 8, &got, 0), (LONG)0x80090026);
		CHECK_EQ(got, 0);
		mod_free(m);
	}

	/* No adapter: every way in says so, with no object. */
	m = mod_load("dxva2.dll", NULL);
	CHECK(m != NULL);
	if (m) {
		FN(m, HRESULT, DXVA2CreateDirect3DDeviceManager9, UINT *, void **);
		UINT token = 7;
		void *mgr = (void *)1;

		CHECK_EQ(DXVA2CreateDirect3DDeviceManager9(&token, &mgr), (HRESULT)0x8876086A);
		CHECK(mgr == NULL);
		CHECK_EQ(token, 0);
		mod_free(m);
	}
	m = mod_load("d3d11.dll", NULL);
	CHECK(m != NULL);
	if (m) {
		FN(m, HRESULT, D3D11CreateDevice, void *, UINT, HMODULE, UINT, const UINT *,
		   UINT, UINT, void **, UINT *, void **);
		void *device = (void *)1, *context = (void *)1;
		UINT level = 9;

		CHECK_EQ(D3D11CreateDevice(NULL, 1, NULL, 0, NULL, 0, 7, &device, &level,
					   &context), (HRESULT)0x887A0004);
		CHECK(device == NULL && context == NULL);
		CHECK_EQ(level, 0);
		mod_free(m);
	}
	m = mod_load("dxgi.dll", NULL);
	CHECK(m != NULL);
	if (m) {
		FN(m, HRESULT, CreateDXGIFactory, const GUID *, void **);
		FN(m, HRESULT, CreateDXGIFactory1, const GUID *, void **);
		FN(m, HRESULT, CreateDXGIFactory2, UINT, const GUID *, void **);
		void *factory = (void *)1;

		CHECK_EQ(CreateDXGIFactory(NULL, &factory), (HRESULT)0x887A0004);
		CHECK(factory == NULL);
		CHECK_EQ(CreateDXGIFactory1(NULL, &factory), (HRESULT)0x887A0004);
		CHECK_EQ(CreateDXGIFactory2(0, NULL, &factory), (HRESULT)0x887A0004);
		mod_free(m);
	}
}

typedef void (*handler_fn)(int);

static int raised;

static void raise_handler(int sig)
{
	raised = sig;
}

/* The CRT functions added with the rest of the real-world libraries. */
static void test_msvcrt_files(void)
{
	struct module *c = mod_load("msvcrt.dll", NULL);
	CHECK(c != NULL);
	if (!c)
		return;
	CFN(c, int, _mkdir, const char *);
	CFN(c, int, _rmdir, const char *);
	CFN(c, int, _unlink, const char *);
	CFN(c, int, _open, const char *, int, ...);
	CFN(c, int, _write, int, const void *, unsigned);
	CFN(c, int, _close, int);
	CFN(c, int, _stat, const char *, void *);
	CFN(c, int, _wstat, const WCHAR *, void *);
	CFN(c, int, _fstat, int, void *);
	CFN(c, intptr_t, _findfirst, const char *, void *);
	CFN(c, int, _findnext, intptr_t, void *);
	CFN(c, int, _findclose, intptr_t);
	CFN(c, intptr_t, _wfindfirst, const WCHAR *, void *);
	CFN(c, int, _wfindnext, intptr_t, void *);
	CFN(c, char *, _getcwd, char *, int);
	CFN(c, void *, _wfopen, const WCHAR *, const WCHAR *);
	CFN(c, int, fclose, void *);
	CFN(c, WCHAR *, _wfullpath, WCHAR *, const WCHAR *, size_t);
	CFN(c, WCHAR *, _wgetenv, const WCHAR *);
	CFN(c, size_t, wcstombs, char *, const WCHAR *, size_t);
	CFN(c, size_t, mbstowcs, WCHAR *, const char *, size_t);
	CFN(c, handler_fn, signal, int, handler_fn);
	CFN(c, int, raise, int);
	CFN(c, int, _getmaxstdio, void);
	CFN(c, int, _setmaxstdio, int);
	CFN(c, int, _open_osfhandle, intptr_t, int);

	/* A file and a directory, to look at. */
	CHECK_EQ(_mkdir("C:\\WINDOWS\\TEMP\\crtfiles"), 0);
	int fd = _open("C:\\WINDOWS\\TEMP\\crtfiles\\a.txt", 0x0002 | 0x0100, 0640);
	CHECK(fd >= 3);
	CHECK_EQ(_write(fd, "twelve bytes", 12), 12);
	CHECK_EQ(_close(fd), 0);
	CHECK_EQ(_close(_open("C:\\WINDOWS\\TEMP\\crtfiles\\b.log", 0x0002 | 0x0100, 0600)), 0);

	/* struct _stat: dev, 16-bit ino, mode, nlink, uid, gid, rdev, size, times. */
	struct { uint32_t dev; uint16_t ino, mode; int16_t nlink, uid, gid; uint32_t rdev;
		 int32_t size, atime, mtime, ctime; } st;
	CHECK_EQ(sizeof(st), 36);
	CHECK_EQ(_stat("C:\\WINDOWS\\TEMP\\crtfiles\\a.txt", &st), 0);
	CHECK_EQ(st.size, 12);
	CHECK_EQ(st.mode & 0xf000, 0x8000);		/* _S_IFREG */
	CHECK_EQ(st.mode & 0x1c0, 0x180);		/* owner read and write */
	CHECK_EQ(st.nlink, 1);
	CHECK(st.mtime > 1600000000);
	CHECK_EQ(_stat("C:\\WINDOWS\\TEMP\\crtfiles", &st), 0);
	CHECK_EQ(st.mode & 0xf000, 0x4000);		/* _S_IFDIR */
	CHECK_EQ(_stat("C:\\WINDOWS\\TEMP\\crtfiles\\none", &st), -1);
	static const WCHAR wname[] = { 'C', ':', '\\', 'W', 'I', 'N', 'D', 'O', 'W', 'S',
		'\\', 'T', 'E', 'M', 'P', '\\', 'c', 'r', 't', 'f', 'i', 'l', 'e', 's', '\\',
		'a', '.', 't', 'x', 't', 0 };
	CHECK_EQ(_wstat(wname, &st), 0);
	CHECK_EQ(st.size, 12);
	fd = _open("C:\\WINDOWS\\TEMP\\crtfiles\\a.txt", 0);
	CHECK_EQ(_fstat(fd, &st), 0);
	CHECK_EQ(st.size, 12);
	CHECK_EQ(st.mode & 0xf000, 0x8000);
	CHECK_EQ(_close(fd), 0);

	/* _findfirst: names, sizes and attributes, ended by _findclose. */
	struct { unsigned attrib; int32_t tc, ta, tw; uint32_t size; char name[260]; } fd1;
	intptr_t h = _findfirst("C:\\WINDOWS\\TEMP\\crtfiles\\*.txt", &fd1);
	CHECK(h != -1);
	CHECK_STR(fd1.name, "a.txt");
	CHECK_EQ(fd1.size, 12);
	CHECK(!(fd1.attrib & 0x10));
	CHECK_EQ(_findnext(h, &fd1), -1);		/* only one .txt */
	CHECK_EQ(errno, ENOENT);
	CHECK_EQ(_findclose(h), 0);
	h = _findfirst("C:\\WINDOWS\\TEMP\\crtfiles\\*.*", &fd1);
	int names = 0;
	while (h != -1) {
		names++;
		if (_findnext(h, &fd1))
			break;
	}
	CHECK_EQ(names, 4);		/* ".", "..", a.txt, b.log */
	if (h != -1)
		_findclose(h);
	CHECK_EQ(_findfirst("C:\\WINDOWS\\TEMP\\crtfiles\\*.nothing", &fd1), -1);
	static const WCHAR wpat[] = { 'C', ':', '\\', 'W', 'I', 'N', 'D', 'O', 'W', 'S',
		'\\', 'T', 'E', 'M', 'P', '\\', 'c', 'r', 't', 'f', 'i', 'l', 'e', 's', '\\',
		'*', '.', 'l', 'o', 'g', 0 };
	struct { unsigned attrib; int32_t tc, ta, tw; uint32_t size; WCHAR name[260]; } wd;
	h = _wfindfirst(wpat, &wd);
	CHECK(h != -1);
	CHECK(wd.name[0] == 'b' && wd.name[1] == '.' && wd.name[3] == 'o');
	CHECK_EQ(_wfindnext(h, &wd), -1);
	if (h != -1)
		_findclose(h);

	/* _wfopen, and _wfullpath over the current directory. */
	static const WCHAR rb[] = { 'r', 'b', 0 };
	void *f = _wfopen(wname, rb);
	CHECK(f != NULL);
	if (f)
		fclose(f);
	WCHAR full[260];
	static const WCHAR rel[] = { 'x', '.', 'd', 'l', 'l', 0 };
	CHECK(_wfullpath(full, rel, 260) == full);
	CHECK(full[1] == ':' && full[2] == '\\');
	CHECK_EQ(full[wcslen16(full) - 1], 'l');
	CHECK(_wfullpath(full, rel, 3) == NULL);	/* too small */
	WCHAR *dyn = _wfullpath(NULL, rel, 0);
	CHECK(dyn != NULL);
	free(dyn);
	char cwd[260];
	CHECK(_getcwd(cwd, sizeof(cwd)) == cwd);
	CHECK(cwd[1] == ':' && cwd[2] == '\\');
	CHECK(_getcwd(cwd, 2) == NULL);
	char *allocated = _getcwd(NULL, 0);
	CHECK(allocated != NULL && !strcmp(allocated, cwd));
	free(allocated);

	/* The environment, as UTF-16; and the multibyte conversions. */
	setenv("SEANCE_WENV", "value", 1);
	static const WCHAR wvar[] = { 'S', 'E', 'A', 'N', 'C', 'E', '_', 'W', 'E', 'N', 'V', 0 };
	WCHAR *ev = _wgetenv(wvar);
	CHECK(ev != NULL && ev[0] == 'v' && ev[4] == 'e' && ev[5] == 0);
	static const WCHAR wnone[] = { 'N', 'O', 'P', 'E', 0 };
	CHECK(_wgetenv(wnone) == NULL);
	static const WCHAR hello[] = { 'h', 'e', 'l', 'l', 'o', 0 };
	char mb[16];
	CHECK_EQ(wcstombs(NULL, hello, 0), 5);
	CHECK_EQ(wcstombs(mb, hello, sizeof(mb)), 5);
	CHECK_STR(mb, "hello");
	CHECK_EQ(wcstombs(mb, hello, 3), 3);		/* what fits, unterminated */
	WCHAR wide[16];
	CHECK_EQ(mbstowcs(NULL, "hello", 0), 5);
	CHECK_EQ(mbstowcs(wide, "hello", 16), 5);
	CHECK(wide[0] == 'h' && wide[4] == 'o' && wide[5] == 0);

	/* signal and raise, in software. */
	CHECK(signal(2, raise_handler) == (handler_fn)0);	/* SIGINT: was SIG_DFL */
	CHECK_EQ(raise(2), 0);
	CHECK_EQ(raised, 2);
	raised = 0;
	CHECK(signal(2, (handler_fn)1) == (handler_fn)0);	/* the handler is used once */
	CHECK_EQ(raise(2), 0);					/* SIG_IGN */
	CHECK_EQ(raised, 0);
	CHECK(signal(77, raise_handler) == (handler_fn)-1);	/* not a signal */
	CHECK_EQ(raise(77), -1);

	/* The stdio limit is a number. */
	CHECK_EQ(_getmaxstdio(), 512);
	CHECK_EQ(_setmaxstdio(1024), 1024);
	CHECK_EQ(_getmaxstdio(), 1024);
	CHECK_EQ(_setmaxstdio(8), -1);
	CHECK_EQ(_open_osfhandle(0x1234, 0), -1);

	/* The descriptor takes the handle over: reading works through it, and
	 * the handle is closed, so only the descriptor is left to close. */
	{
		struct module *k = mod_load("kernel32.dll", NULL);
		FN(k, HANDLE, CreateFileA, LPCSTR, DWORD, DWORD, void *, DWORD, DWORD, HANDLE);
		FN(k, BOOL, CloseHandle, HANDLE);
		CFN(c, int, _read, int, void *, unsigned);
		char buf[16] = "";
		HANDLE h = CreateFileA("C:\\WINDOWS\\TEMP\\crtfiles\\a.txt", 0x80000000,
				       1, NULL, 3, 0x80, NULL);
		CHECK(h != INVALID_HANDLE_VALUE);
		int ofd = _open_osfhandle((intptr_t)h, 0);
		CHECK(ofd >= 3);
		CHECK_EQ(_read(ofd, buf, 12), 12);
		CHECK(!memcmp(buf, "twelve bytes", 12));
		CHECK(!CloseHandle(h));
		CHECK_EQ(_close(ofd), 0);
		mod_free(k);
	}

	CHECK_EQ(_unlink("C:\\WINDOWS\\TEMP\\crtfiles\\a.txt"), 0);
	CHECK_EQ(_unlink("C:\\WINDOWS\\TEMP\\crtfiles\\b.log"), 0);
	CHECK_EQ(_rmdir("C:\\WINDOWS\\TEMP\\crtfiles"), 0);
	mod_free(c);
}

/*
 * Memory comes back zeroed from malloc, operator new and HeapAlloc, as it
 * usually does from a fresh Windows heap: the Intel Quick Sync library, for
 * one, destroys a member it never initialised if its set-up fails.
 */
static void test_zeroed_allocation(void)
{
	struct module *c = mod_load("msvcrt.dll", NULL);
	struct module *k = mod_load("kernel32.dll", NULL);
	CHECK(c && k);
	if (!c || !k)
		return;
	CFN(c, void *, malloc, size_t);
	CFN(c, void, free, void *);
	void *(*operator_new)(size_t) = (void *(*)(size_t))get(c, "??2@YAPAXI@Z");
	FN(k, HANDLE, GetProcessHeap, void);
	FN(k, LPVOID, HeapAlloc, HANDLE, DWORD, SIZE_T);
	FN(k, BOOL, HeapFree, HANDLE, DWORD, LPVOID);

	/* Dirty a block and give it back, so the next one is a reused one. */
	for (int round = 0; round < 3; round++) {
		BYTE *p = malloc(300);
		int clean = 1;

		memset(p, 0xaa, 300);
		free(p);
		p = malloc(300);
		for (int i = 0; i < 300; i++)
			clean &= p[i] == 0;
		CHECK(clean);
		free(p);

		p = operator_new(300);
		clean = 1;
		memset(p, 0xaa, 300);
		free(p);
		p = operator_new(300);
		for (int i = 0; i < 300; i++)
			clean &= p[i] == 0;
		CHECK(clean);
		free(p);

		p = HeapAlloc(GetProcessHeap(), 0, 300);
		clean = 1;
		memset(p, 0xaa, 300);
		HeapFree(GetProcessHeap(), 0, p);
		p = HeapAlloc(GetProcessHeap(), 0, 300);
		for (int i = 0; i < 300; i++)
			clean &= p[i] == 0;
		CHECK(clean);
		HeapFree(GetProcessHeap(), 0, p);
	}
	mod_free(k);
	mod_free(c);
}

/* crtdll.dll: msvcrt's functions, and its data under its own names. */
static void test_crtdll(void)
{
	struct module *c = mod_load("crtdll.dll", NULL);
	struct module *v = mod_load("msvcrt.dll", NULL);
	CHECK(c && v);
	if (!c || !v)
		return;
	CHECK(get(c, "memcpy") == get(v, "memcpy"));
	CHECK(get(c, "_stricmp") == get(v, "_stricmp"));
	CHECK(get(c, "_pctype_dll") == get(v, "_pctype"));
	CHECK(get(c, "__mb_cur_max_dll") == get(v, "__mb_cur_max"));
	CHECK_EQ(*(unsigned *)get(c, "_winver_dll"), 0x0501);
	CHECK_EQ(*(unsigned *)get(c, "_osver_dll"), 2600);
	CHECK(**(char ***)get(c, "_environ_dll") == NULL);
	mod_free(v);
	mod_free(c);
}

/* psapi.dll, and kernel32's K32 copies: the one process's modules. */
static void test_psapi(void)
{
	struct module *p = mod_load("psapi.dll", NULL);
	struct module *k = mod_load("kernel32.dll", NULL);
	CHECK(p && k);
	if (!p || !k)
		return;
	FN(p, BOOL, EnumProcessModules, HANDLE, HMODULE *, DWORD, DWORD *);
	FN(p, DWORD, GetModuleBaseNameA, HANDLE, HMODULE, LPSTR, DWORD);
	FN(p, DWORD, GetModuleBaseNameW, HANDLE, HMODULE, LPWSTR, DWORD);
	FN(p, DWORD, GetModuleFileNameExA, HANDLE, HMODULE, LPSTR, DWORD);
	FN(p, BOOL, GetModuleInformation, HANDLE, HMODULE, LPMODULEINFO, DWORD);
	FN(p, BOOL, GetProcessMemoryInfo, HANDLE, PPROCESS_MEMORY_COUNTERS, DWORD);
	FN(p, BOOL, EnumProcesses, DWORD *, DWORD, DWORD *);
	FN(k, HANDLE, GetCurrentProcess, void);
	HANDLE self = GetCurrentProcess();
	HMODULE mods[256];
	DWORD needed = 0;

	CHECK(EnumProcessModules(self, mods, sizeof(mods), &needed));
	CHECK(needed >= 3 * sizeof(HMODULE) && needed <= sizeof(mods));
	int found = 0;
	for (DWORD i = 0; i < needed / sizeof(HMODULE); i++)
		found += mods[i] == (HMODULE)k->base;
	CHECK_EQ(found, 1);
	/* Asked with no room, it says how much. */
	DWORD again = 0;
	CHECK(EnumProcessModules(self, NULL, 0, &again) && again == needed);
	CHECK(!EnumProcessModules((HANDLE)0x1234, mods, sizeof(mods), &needed));

	char name[64];
	WCHAR wname[64];
	CHECK_EQ(GetModuleBaseNameA(self, (HMODULE)k->base, name, sizeof(name)), 12);
	CHECK_STR(name, "kernel32.dll");
	CHECK_EQ(GetModuleBaseNameW(self, (HMODULE)k->base, wname, 64), 12);
	CHECK(wname[0] == 'k' && wname[12] == 0);
	CHECK_EQ(GetModuleBaseNameA(self, (HMODULE)k->base, name, 5), 4);
	CHECK_STR(name, "kern");
	char path[300];
	CHECK(GetModuleFileNameExA(self, (HMODULE)k->base, path, sizeof(path)) > 12);
	CHECK(strstr(path, "kernel32.dll") != NULL);

	MODULEINFO mi;
	CHECK(GetModuleInformation(self, (HMODULE)k->base, &mi, sizeof(mi)));
	CHECK(mi.lpBaseOfDll == k->base && mi.SizeOfImage >= 0x1000);
	PROCESS_MEMORY_COUNTERS pmc;
	CHECK(GetProcessMemoryInfo(self, &pmc, sizeof(pmc)));
	CHECK(pmc.cb == sizeof(pmc) && pmc.WorkingSetSize > 0);
	DWORD pids[4];
	CHECK(EnumProcesses(pids, sizeof(pids), &needed) && needed == 4 &&
	      pids[0] == (DWORD)getpid());

	CHECK(get(k, "K32EnumProcessModules") == (void *)EnumProcessModules);
	CHECK(get(k, "K32GetModuleInformation") == (void *)GetModuleInformation);
	mod_free(k);
	mod_free(p);
}

/*
 * oledlg.dll's dialogs are cancelled, and found by Windows' ordinals
 * (vp7vfw.dll imports OleUIBusyA as #8); dbghelp.dll gives names back as
 * they are and has no symbols.
 */
static void test_oledlg_dbghelp(void)
{
	struct module *o = mod_load("oledlg.dll", NULL);
	struct module *d = mod_load("dbghelp.dll", NULL);
	CHECK(o && d);
	if (!o || !d)
		return;
	FN(o, UINT, OleUIBusyA, void *);
	FN(o, UINT, OleUIInsertObjectW, void *);
	FN(o, BOOL, OleUIAddVerbMenuA, void *, LPCSTR, HMENU, UINT, UINT, UINT,
	   BOOL, UINT, HMENU *);
	CHECK(mod_get_ordinal(o, 8) == (void *)OleUIBusyA);
	CHECK(mod_get_ordinal(o, 1) == (void *)OleUIAddVerbMenuA);
	CHECK(mod_get_ordinal(o, 12) == get(o, "OleUIChangeSourceA"));
	CHECK_EQ(OleUIBusyA(NULL), 2);			/* OLEUI_CANCEL */
	CHECK_EQ(OleUIInsertObjectW(NULL), 2);
	HMENU popup = (HMENU)1;
	CHECK(!OleUIAddVerbMenuA(NULL, NULL, NULL, 0, 0, 0, FALSE, 0, &popup));
	CHECK(popup == NULL);

	FN(d, DWORD, UnDecorateSymbolName, PCSTR, PSTR, DWORD, DWORD);
	FN(d, BOOL, SymInitialize, HANDLE, PCSTR, BOOL);
	FN(d, DWORD, SymSetOptions, DWORD);
	FN(d, DWORD, SymGetOptions, void);
	char out[8];
	CHECK_EQ(UnDecorateSymbolName("?f@@YAXXZ", out, sizeof(out), 0), 7);
	CHECK_STR(out, "?f@@YAX");
	CHECK_EQ(UnDecorateSymbolName("main", out, sizeof(out), 0), 4);
	CHECK_STR(out, "main");
	CHECK_EQ(UnDecorateSymbolName("main", out, 0, 0), 0);
	CHECK(!SymInitialize((HANDLE)-1, NULL, TRUE));
	CHECK_EQ(SymSetOptions(0x16), 0x16);
	CHECK_EQ(SymGetOptions(), 0x16);
}

/* netapi32.dll shares nothing (siaviref.dll lists the shares). */
static void test_netapi32(void)
{
	struct module *m = mod_load("netapi32.dll", NULL);
	CHECK(m != NULL);
	if (!m)
		return;
	FN(m, DWORD, NetShareEnum, LPWSTR, DWORD, BYTE **, DWORD, DWORD *,
	   DWORD *, DWORD *);
	FN(m, DWORD, NetApiBufferAllocate, DWORD, void **);
	FN(m, DWORD, NetApiBufferFree, void *);
	BYTE *buf = (BYTE *)1;
	DWORD read = 9, total = 9, resume = 9;
	CHECK_EQ(NetShareEnum(NULL, 2, &buf, (DWORD)-1, &read, &total, &resume), 0);
	CHECK(buf == NULL && !read && !total && !resume);
	CHECK_EQ(NetApiBufferFree(buf), 0);
	void *p = NULL;
	CHECK_EQ(NetApiBufferAllocate(16, &p), 0);
	CHECK(p != NULL);
	CHECK_EQ(NetApiBufferFree(p), 0);
}

/* _splitpath and _wsplitpath (siaviref.dll). */
static void test_splitpath(void)
{
	struct module *m = mod_load("msvcrt.dll", NULL);
	CHECK(m != NULL);
	if (!m)
		return;
	CFN(m, void, _splitpath, const char *, char *, char *, char *, char *);
	CFN(m, void, _wsplitpath, const WCHAR *, WCHAR *, WCHAR *, WCHAR *,
	    WCHAR *);
	char drive[8], dir[64], name[64], ext[16];

	_splitpath("C:\\dir\\sub/file.tar.gz", drive, dir, name, ext);
	CHECK_STR(drive, "C:");
	CHECK_STR(dir, "\\dir\\sub/");
	CHECK_STR(name, "file.tar");
	CHECK_STR(ext, ".gz");
	_splitpath("dir.d\\noext", drive, dir, name, ext);
	CHECK_STR(drive, "");
	CHECK_STR(dir, "dir.d\\");
	CHECK_STR(name, "noext");
	CHECK_STR(ext, "");
	_splitpath("a.avi", NULL, NULL, name, NULL);
	CHECK_STR(name, "a");

	static const WCHAR wpath[] = { 'D', ':', 'x', '\\', 'y', '.', 'z', 0 };
	WCHAR wd[8], wdir[16], wn[16], we[16];
	_wsplitpath(wpath, wd, wdir, wn, we);
	CHECK(wd[0] == 'D' && wd[1] == ':' && !wd[2]);
	CHECK(wdir[0] == 'x' && wdir[1] == '\\' && !wdir[2]);
	CHECK(wn[0] == 'y' && !wn[1]);
	CHECK(we[0] == '.' && we[1] == 'z' && !we[2]);
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
	test_msvcrt_files();
	test_winsock();
	test_zeroed_allocation();
	test_ncrypt_and_graphics();
	test_crtdll();
	test_psapi();
	test_oledlg_dbghelp();
	test_netapi32();
	test_splitpath();

	char cmd[512];
	snprintf(cmd, sizeof(cmd), "rm -rf '%s'", root);
	if (system(cmd))
		fprintf(stderr, "could not remove %s\n", root);
	return test_summary("test_win32_extras");
}
