/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * version.dll, called through its export table: version blocks from
 * files, loaded modules and the built-in DLLs, and VerQueryValue on them.
 */
#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>

#include "loader/modules.h"
#include "loader/pe.h"
#include "runtime/runtime.h"
#include "win32/internal.h"
#include "win32/path.h"
#include "test.h"

static struct module *ver;

static void *get(const char *name)
{
	void *p = mod_get_proc(ver, name);
	if (!p) {
		fprintf(stderr, "version has no %s\n", name);
		exit(1);
	}
	return p;
}

#define FN(ret, name, ...) \
	ret (WINAPI *name)(__VA_ARGS__) = (ret (WINAPI *)(__VA_ARGS__))get(#name)

static const WCHAR product_w[] = {
	'\\', 'S', 't', 'r', 'i', 'n', 'g', 'F', 'i', 'l', 'e', 'I', 'n', 'f',
	'o', '\\', '0', '4', '0', '9', '0', '4', 'B', '0', '\\', 'p', 'r', 'o',
	'd', 'u', 'c', 't', 'n', 'a', 'm', 'e', 0
};

static int wide_is(const WCHAR *w, const char *s)
{
	while (*s && *w == (BYTE)*s)
		w++, s++;
	return !*s && !*w;
}

/* The size of pe_basic.dll's RT_VERSION resource, read by the loader. */
static DWORD resource_size(const char *path)
{
	struct pe_image *img = pe_open(path);
	struct pe_resource res;
	DWORD size = 0;

	if (img && !pe_find_resource(img, RT_VERSION, 0, PE_RES_ANY_LANG, &res))
		size = res.size;
	pe_free(img);
	return size;
}

static void test_file(void)
{
	FN(DWORD, GetFileVersionInfoSizeA, LPCSTR, LPDWORD);
	FN(BOOL, GetFileVersionInfoA, LPCSTR, DWORD, DWORD, LPVOID);
	FN(BOOL, VerQueryValueA, LPCVOID, LPCSTR, LPVOID *, PUINT);
	FN(BOOL, VerQueryValueW, LPCVOID, LPCWSTR, LPVOID *, PUINT);

	const char *unix_path = fixture("pe_basic.dll");
	char *path = path_to_windows(unix_path);
	DWORD handle = 7, res = resource_size(unix_path);
	CHECK(path && res);

	DWORD size = GetFileVersionInfoSizeA(path, &handle);
	CHECK_EQ(size, res * 2 + 4);
	CHECK_EQ(handle, 0);

	/* A KnownDLL's name with a path reads that file, not the built-in. */
	char dir[] = "/tmp/vfw-version-XXXXXX", copy[512];
	if (mkdtemp(dir)) {
		snprintf(copy, sizeof(copy), "%s/msvcrt.dll", dir);
		FILE *in = fopen(unix_path, "rb"), *out = fopen(copy, "wb");
		char chunk[4096];
		size_t got;
		while (in && out && (got = fread(chunk, 1, sizeof(chunk), in)))
			fwrite(chunk, 1, got, out);
		if (in)
			fclose(in);
		if (out)
			fclose(out);
		char *wcopy = path_to_windows(copy);
		CHECK(wcopy && GetFileVersionInfoSizeA(wcopy, NULL) == size);
		free(wcopy);
		unlink(copy);
		rmdir(dir);
	}

	BYTE *block = calloc(1, size);
	CHECK(block && GetFileVersionInfoA(path, 0, size, block));
	WORD total = (WORD)(block[0] | block[1] << 8);
	CHECK(total <= res);
	CHECK(!memcmp(block + total, "FE2X", 4));

	VS_FIXEDFILEINFO *fixed = NULL;
	UINT len = 0;
	CHECK(VerQueryValueA(block, "\\", (void **)&fixed, &len));
	CHECK_EQ(len, sizeof(*fixed));
	CHECK(fixed && fixed->dwSignature == VS_FFI_SIGNATURE);
	CHECK(fixed && fixed->dwFileVersionMS == 0x00010002 &&
	      fixed->dwFileVersionLS == 0x00030004);
	CHECK(fixed && fixed->dwProductVersionMS == 0x00010002 &&
	      fixed->dwProductVersionLS == 0);
	CHECK(fixed && fixed->dwFileType == VFT_DLL);

	WORD *trans = NULL;
	CHECK(VerQueryValueA(block, "\\VarFileInfo\\Translation",
			     (void **)&trans, &len));
	CHECK_EQ(len, 4);
	CHECK(trans && trans[0] == 0x409 && trans[1] == 1200);

	/*
	 * ANSI strings are copied aside, not into the room after the block,
	 * which VerQueryValueA can't know the caller really has.
	 */
	char *s = NULL;
	CHECK(VerQueryValueA(block, "\\StringFileInfo\\040904b0\\FileVersion",
			     (void **)&s, &len));
	CHECK_STR(s, "1.2.3.4");
	CHECK_EQ(len, 8);
	CHECK((BYTE *)s < block || (BYTE *)s >= block + size);
	for (DWORD i = res > total + 4u ? res : total + 4u; i < size; i++)
		if (block[i]) {
			CHECK(!"the second half was written to");
			break;
		}
	/* Keys match case-insensitively; repeated separators are skipped. */
	s = NULL;
	CHECK(VerQueryValueA(block, "StringFileInfo\\\\040904B0\\FILEDESCRIPTION",
			     (void **)&s, &len));
	CHECK_STR(s, "seance PE test fixture");

	WCHAR *w = NULL;
	CHECK(VerQueryValueW(block, product_w, (void **)&w, &len));
	CHECK(w && wide_is(w, "seance"));
	CHECK_EQ(len, 7);
	CHECK((BYTE *)w > block && (BYTE *)w < block + total);

	len = 5;
	s = NULL;
	CHECK(!VerQueryValueA(block, "\\StringFileInfo\\040904b0\\Nope",
			      (void **)&s, &len));
	CHECK_EQ(len, 0);
	CHECK(!VerQueryValueA(block, "\\VarFileInfo\\Translation\\x",
			      (void **)&s, &len));

	/*
	 * A buffer just big enough for the block, with what looks like the
	 * signature after it (in memory that isn't the caller's): nothing
	 * past the buffer is read or written (ASan checks).
	 */
	BYTE *exact = malloc(total + 4);
	CHECK(exact && GetFileVersionInfoA(path, 0, total, exact));
	memcpy(exact + total, "FE2X", 4);
	BYTE *shrunk = realloc(exact, total);
	CHECK(shrunk != NULL);
	exact = shrunk;
	s = NULL;
	CHECK(VerQueryValueA(exact, "\\StringFileInfo\\040904b0\\FileVersion",
			     (void **)&s, &len));
	CHECK_STR(s, "1.2.3.4");
	CHECK((BYTE *)s < exact || (BYTE *)s >= exact + total);
	char *again = NULL;
	CHECK(VerQueryValueA(exact, "\\StringFileInfo\\040904b0\\FileVersion",
			     (void **)&again, &len));
	CHECK(again == s);
	free(exact);

	/* Truncated: the block shrinks to what was copied. */
	exact = malloc(0x62);
	CHECK(exact && GetFileVersionInfoA(path, 0, 0x62, exact));
	CHECK_EQ(exact[0] | exact[1] << 8, 0x60);
	CHECK(VerQueryValueA(exact, "\\", (void **)&fixed, &len));
	CHECK_EQ(len, sizeof(*fixed));
	CHECK(!VerQueryValueA(exact, "\\VarFileInfo\\Translation",
			      (void **)&trans, &len));
	free(exact);

	win_set_error(0);
	CHECK(!GetFileVersionInfoA(path, 0, 0, block));
	CHECK_EQ(win_get_error(), ERROR_INVALID_DATA);
	free(block);
	free(path);
}

/* A raw resource (FindResource + LockResource) is never written to. */
static void test_loaded_module(void)
{
	FN(DWORD, GetFileVersionInfoSizeA, LPCSTR, LPDWORD);
	FN(BOOL, VerQueryValueA, LPCVOID, LPCSTR, LPVOID *, PUINT);

	struct module *m = mod_load(fixture("pe_basic.dll"), NULL);
	CHECK(m && m->img);
	if (!m)
		return;
	/* Found by its module name, loaded from outside the search path. */
	CHECK_EQ(GetFileVersionInfoSizeA("PE_BASIC.DLL", NULL),
		 resource_size(fixture("pe_basic.dll")) * 2 + 4);

	struct pe_resource res;
	CHECK_EQ(pe_find_resource(m->img, RT_VERSION, 0, PE_RES_ANY_LANG, &res), 0);
	BYTE *raw = (BYTE *)pe_ptr(m->img, res.data_rva, res.size);
	CHECK(raw != NULL);
	if (!raw) {
		mod_free(m);
		return;
	}
	/*
	 * Even with what looks like GetFileVersionInfo's signature after it
	 * (planted in the section's padding), the image is left alone.
	 */
	WORD total = (WORD)(raw[0] | raw[1] << 8);
	long page = sysconf(_SC_PAGESIZE);
	BYTE *first = (BYTE *)((uintptr_t)raw & ~(uintptr_t)(page - 1));
	size_t span = (size_t)(raw + total + 4 - first);
	CHECK(raw + total + 4 <= m->base + m->img->map_len);
	CHECK_EQ(mprotect(first, span, PROT_READ | PROT_WRITE), 0);
	memcpy(raw + total, "FE2X", 4);
	BYTE *copy = malloc(m->img->map_len);
	memcpy(copy, m->base, m->img->map_len);

	char *s = NULL;
	UINT len = 0;
	CHECK(VerQueryValueA(raw, "\\StringFileInfo\\040904b0\\ProductName",
			     (void **)&s, &len));
	CHECK_STR(s, "seance");
	CHECK_EQ(len, 7);
	CHECK((const BYTE *)s < m->base || (const BYTE *)s >= m->base + m->img->map_len);
	CHECK(!memcmp(copy, m->base, m->img->map_len));
	free(copy);
	memset(raw + total, 0, 4);
	mprotect(first, span, PROT_READ);
	mod_free(m);
}

static void test_builtin(void)
{
	FN(DWORD, GetFileVersionInfoSizeW, LPCWSTR, LPDWORD);
	FN(BOOL, GetFileVersionInfoW, LPCWSTR, DWORD, DWORD, LPVOID);
	FN(DWORD, GetFileVersionInfoSizeExA, DWORD, LPCSTR, LPDWORD);
	FN(BOOL, GetFileVersionInfoExA, DWORD, LPCSTR, DWORD, DWORD, LPVOID);
	FN(BOOL, VerQueryValueA, LPCVOID, LPCSTR, LPVOID *, PUINT);
	static const WCHAR msvfw_w[] = {
		'M', 'S', 'V', 'F', 'W', '3', '2', '.', 'd', 'l', 'l', 0 };

	DWORD size = GetFileVersionInfoSizeW(msvfw_w, NULL);
	CHECK(size > 0);
	BYTE *block = calloc(1, size);
	CHECK(block && GetFileVersionInfoW(msvfw_w, 0, size, block));

	VS_FIXEDFILEINFO *fixed = NULL;
	UINT len = 0;
	CHECK(VerQueryValueA(block, "\\", (void **)&fixed, &len));
	CHECK_EQ(len, sizeof(*fixed));
	CHECK(fixed && fixed->dwSignature == VS_FFI_SIGNATURE &&
	      fixed->dwFileVersionMS == 0x00050001 &&
	      fixed->dwFileVersionLS == ((2600u << 16) | 5512));
	char *s = NULL;
	CHECK(VerQueryValueA(block, "\\StringFileInfo\\040904b0\\OriginalFilename",
			     (void **)&s, &len));
	CHECK_STR(s, "msvfw32.dll");
	CHECK(VerQueryValueA(block, "\\StringFileInfo\\040904b0\\FileVersion",
			     (void **)&s, &len));
	CHECK_STR(s, "5.1.2600.5512");
	WORD *trans = NULL;
	CHECK(VerQueryValueA(block, "\\VarFileInfo\\Translation",
			     (void **)&trans, &len));
	CHECK(len == 4 && trans && trans[0] == 0x409 && trans[1] == 1200);
	free(block);

	/* By system path too, and by bare name (no DLL on disk). */
	const char *path = "C:\\WINDOWS\\system32\\KERNEL32.DLL";
	size = GetFileVersionInfoSizeExA(0, path, NULL);
	CHECK(size > 0);
	block = calloc(1, size);
	CHECK(block && GetFileVersionInfoExA(0, path, 0, size, block));
	CHECK(VerQueryValueA(block, "\\StringFileInfo\\040904b0\\InternalName",
			     (void **)&s, &len));
	CHECK_STR(s, "kernel32");
	free(block);
	CHECK(GetFileVersionInfoSizeExA(0, "comdlg32.dll", NULL) > 0);
}

static void test_errors(void)
{
	FN(DWORD, GetFileVersionInfoSizeA, LPCSTR, LPDWORD);
	DWORD handle = 3;

	win_set_error(0);
	CHECK_EQ(GetFileVersionInfoSizeA("no-such-file.dll", &handle), 0);
	CHECK_EQ(win_get_error(), ERROR_FILE_NOT_FOUND);
	CHECK_EQ(handle, 0);
	CHECK_EQ(GetFileVersionInfoSizeA("Z:\\no\\such\\dir\\x.dll", NULL), 0);
	CHECK_EQ(win_get_error(), ERROR_FILE_NOT_FOUND);
	CHECK_EQ(GetFileVersionInfoSizeA("", NULL), 0);

	/* No resources at all, and resources but no version. */
	char *path = path_to_windows(fixture("pe_tls.dll"));
	CHECK_EQ(GetFileVersionInfoSizeA(path, NULL), 0);
	CHECK_EQ(win_get_error(), ERROR_RESOURCE_DATA_NOT_FOUND);
	free(path);
	path = path_to_windows(fixture("pe_k32.dll"));
	CHECK_EQ(GetFileVersionInfoSizeA(path, NULL), 0);
	CHECK_EQ(win_get_error(), ERROR_RESOURCE_TYPE_NOT_FOUND);
	free(path);

	/* Not a PE file. */
	char tmp[] = "/tmp/vfw-ver-XXXXXX";
	int fd = mkstemp(tmp);
	CHECK(fd >= 0 && write(fd, "hello", 5) == 5);
	if (fd >= 0)
		close(fd);
	path = path_to_windows(tmp);
	CHECK_EQ(GetFileVersionInfoSizeA(path, NULL), 0);
	CHECK_EQ(win_get_error(), ERROR_RESOURCE_DATA_NOT_FOUND);
	free(path);
	unlink(tmp);

	/* A bare name is searched for on $VFW_PATH. */
	const char *dll = fixture("pe_basic.dll");
	char dir[4096];
	snprintf(dir, sizeof(dir), "%.*s", (int)(strrchr(dll, '/') - dll), dll);
	setenv("VFW_PATH", dir, 1);
	CHECK(GetFileVersionInfoSizeA("pe_com.dll", NULL) > 0);
	unsetenv("VFW_PATH");
}

/* A 16-bit node: length, value length, key, value, children (4-aligned). */
struct ne_buf {
	BYTE b[2048];
	size_t n;
};

static void ne_align(struct ne_buf *nb)
{
	while (nb->n & 3)
		nb->b[nb->n++] = 0;
}

static size_t ne_begin(struct ne_buf *nb, const char *key, const void *val,
		       size_t vlen)
{
	ne_align(nb);
	size_t start = nb->n;
	nb->n += 4;
	memcpy(nb->b + nb->n, key, strlen(key) + 1);
	nb->n += strlen(key) + 1;
	ne_align(nb);
	if (vlen)
		memcpy(nb->b + nb->n, val, vlen);
	nb->n += vlen;
	nb->b[start + 2] = (BYTE)vlen;
	nb->b[start + 3] = (BYTE)(vlen >> 8);
	return start;
}

static void ne_end(struct ne_buf *nb, size_t start)
{
	size_t len = nb->n - start;
	nb->b[start] = (BYTE)len;
	nb->b[start + 1] = (BYTE)(len >> 8);
}

static void ne_string(struct ne_buf *nb, const char *key, const char *val)
{
	ne_end(nb, ne_begin(nb, key, val, strlen(val) + 1));
}

/* An NE executable whose version resource says major.minor. */
static int write_ne(const char *path, WORD major, WORD minor,
		    const char *product)
{
	struct ne_buf v = { .n = 0 };
	VS_FIXEDFILEINFO fixed = {
		.dwSignature = VS_FFI_SIGNATURE,
		.dwStrucVersion = 0x10000,
		.dwFileVersionMS = (DWORD)major << 16 | minor,
		.dwProductVersionMS = (DWORD)major << 16 | minor,
		.dwFileOS = 1,		/* VOS__WINDOWS16 */
		.dwFileType = VFT_DLL,
	};
	const WORD translation[2] = { 0x0409, 1252 };
	size_t root = ne_begin(&v, "VS_VERSION_INFO", &fixed, sizeof(fixed));
	size_t sfi = ne_begin(&v, "StringFileInfo", NULL, 0);
	size_t table = ne_begin(&v, "040904E4", NULL, 0);
	ne_string(&v, "FileVersion", major == 3 ? "3.10" : "2.0");
	ne_string(&v, "ProductName", product);
	ne_end(&v, table);
	ne_end(&v, sfi);
	size_t vfi = ne_begin(&v, "VarFileInfo", NULL, 0);
	ne_end(&v, ne_begin(&v, "Translation", translation,
			    sizeof(translation)));
	ne_end(&v, vfi);
	ne_end(&v, root);

	static BYTE f[0x100 + sizeof(v.b)];
	memset(f, 0, sizeof(f));
	f[0] = 'M';
	f[1] = 'Z';
	f[0x3c] = 0x40;		/* the NE header */
	f[0x40] = 'N';
	f[0x41] = 'E';
	f[0x40 + 0x24] = 0x40;	/* resource table at 0x80 */
	f[0x40 + 0x26] = 0x60;	/* resident names after it */
	BYTE *rt = f + 0x80;
	rt[0] = 4;		/* shift */
	rt[2] = 0x10;		/* RT_VERSION */
	rt[3] = 0x80;
	rt[4] = 1;		/* one of them */
	rt[10] = 0x10;		/* at 0x100 */
	rt[12] = (BYTE)((v.n + 15) >> 4);
	rt[16] = 1;		/* id 1 */
	rt[17] = 0x80;
	memcpy(f + 0x100, v.b, v.n);
	FILE *out = fopen(path, "wb");
	if (!out)
		return -1;
	fwrite(f, 1, 0x100 + ((v.n + 15) & ~(size_t)15), out);
	return fclose(out);
}

static void test_ne(void)
{
	FN(DWORD, GetFileVersionInfoSizeA, LPCSTR, LPDWORD);
	FN(BOOL, GetFileVersionInfoA, LPCSTR, DWORD, DWORD, LPVOID);
	FN(BOOL, VerQueryValueA, LPCVOID, LPCSTR, LPVOID *, PUINT);
	FN(BOOL, VerQueryValueW, LPCVOID, LPCWSTR, LPVOID *, PUINT);
	char dir[] = "/tmp/vfw-version-ne-XXXXXX", path[512];

	if (!mkdtemp(dir))
		return;
	snprintf(path, sizeof(path), "%s/OLD.DLL", dir);
	CHECK_EQ(write_ne(path, 3, 10, "Old Tool"), 0);
	char *w = path_to_windows(path);
	DWORD size = GetFileVersionInfoSizeA(w, NULL);
	CHECK(size > 0);
	BYTE *block = calloc(1, size ? size : 1);
	CHECK(block && GetFileVersionInfoA(w, 0, size, block));

	VS_FIXEDFILEINFO *fixed = NULL;
	UINT len = 0;
	CHECK(VerQueryValueA(block, "\\", (void **)&fixed, &len));
	CHECK(fixed && len == sizeof(*fixed) &&
	      fixed->dwSignature == VS_FFI_SIGNATURE &&
	      fixed->dwFileVersionMS == 0x0003000a);
	char *text = NULL;
	CHECK(VerQueryValueA(block, "\\StringFileInfo\\040904e4\\ProductName",
			     (void **)&text, &len));
	CHECK(text && !strcmp(text, "Old Tool") && len == 9);
	static const WCHAR fv[] = { '\\', 'S', 't', 'r', 'i', 'n', 'g', 'F',
		'i', 'l', 'e', 'I', 'n', 'f', 'o', '\\', '0', '4', '0', '9',
		'0', '4', 'E', '4', '\\', 'F', 'i', 'l', 'e', 'V', 'e', 'r',
		's', 'i', 'o', 'n', 0 };
	WCHAR *wtext = NULL;
	CHECK(VerQueryValueW(block, fv, (void **)&wtext, &len));
	CHECK(wtext && wide_is(wtext, "3.10") && len == 5);
	WORD *tr = NULL;
	CHECK(VerQueryValueA(block, "\\VarFileInfo\\Translation",
			     (void **)&tr, &len));
	CHECK(tr && len == 4 && tr[0] == 0x0409 && tr[1] == 1252);
	free(block);
	free(w);
	unlink(path);
	rmdir(dir);
}

static void test_language_name(void)
{
	FN(DWORD, VerLanguageNameA, DWORD, LPSTR, DWORD);
	FN(DWORD, VerLanguageNameW, DWORD, LPWSTR, DWORD);
	char a[64];
	WCHAR wb[64];

	CHECK_EQ(VerLanguageNameA(0x0409, a, sizeof(a)), 23);
	CHECK_STR(a, "English (United States)");
	CHECK_EQ(VerLanguageNameA(0x0407, a, sizeof(a)), 16);
	CHECK_STR(a, "German (Germany)");
	CHECK_EQ(VerLanguageNameA(0x7777, a, sizeof(a)), 16);
	CHECK_STR(a, "Language Neutral");
	/* Too small: as much as fits, and the length needed. */
	CHECK_EQ(VerLanguageNameA(0x0409, a, 8), 23);
	CHECK_STR(a, "English");
	CHECK_EQ(VerLanguageNameW(0x0411, wb, 64), 8);
	CHECK(wide_is(wb, "Japanese"));
}

/* SZDD: the magic, 'A', the name's last character, the size, then LZSS. */
static void write_szdd(const char *path)
{
	static const BYTE data[] = {
		'S', 'Z', 'D', 'D', 0x88, 0xf0, 0x27, 0x33, 'A', 'L', 9, 0, 0,
		0,
		0x07, 'a', 'b', 'c', 0xf0, 0xf3,	/* abc, then 6 back */
	};
	FILE *f = fopen(path, "wb");
	if (f) {
		fwrite(data, 1, sizeof(data), f);
		fclose(f);
	}
}

static int file_is(const char *path, const char *want, size_t n)
{
	char buf[64];
	FILE *f = fopen(path, "rb");
	size_t got = f ? fread(buf, 1, sizeof(buf), f) : 0;

	if (f)
		fclose(f);
	return got == n && !memcmp(buf, want, n);
}

static void test_install(void)
{
	FN(DWORD, VerFindFileA, DWORD, LPCSTR, LPCSTR, LPCSTR, LPSTR, PUINT,
	   LPSTR, PUINT);
	FN(DWORD, VerFindFileW, DWORD, LPCWSTR, LPCWSTR, LPCWSTR, LPWSTR,
	   PUINT, LPWSTR, PUINT);
	FN(DWORD, VerInstallFileA, DWORD, LPCSTR, LPCSTR, LPCSTR, LPCSTR,
	   LPCSTR, LPSTR, PUINT);
	char src[] = "/tmp/vfw-version-src-XXXXXX";
	char dst[] = "/tmp/vfw-version-dst-XXXXXX";
	char p[600], cur[260], dest[260], tmp[32];
	UINT cl, dl, tl;

	if (!mkdtemp(src) || !mkdtemp(dst))
		return;
	char *wsrc = path_to_windows(src), *wdst = path_to_windows(dst);
	CHECK(wsrc && wdst);

	/* Where it goes: an application's file to its directory. */
	snprintf(p, sizeof(p), "%s/APP.DLL", dst);
	CHECK_EQ(write_ne(p, 3, 10, "App"), 0);
	cl = sizeof(cur);
	dl = sizeof(dest);
	CHECK_EQ(VerFindFileA(0, "APP.DLL", NULL, wdst, cur, &cl, dest, &dl),
		 0);
	CHECK_STR(cur, wdst);
	CHECK_STR(dest, wdst);
	CHECK_EQ(dl, strlen(wdst) + 1);
	/* A shared one to the system directory, away from the copy here. */
	cl = sizeof(cur);
	dl = sizeof(dest);
	CHECK_EQ(VerFindFileA(1, "APP.DLL", NULL, wdst, cur, &cl, dest, &dl),
		 1);	/* VFF_CURNEDEST */
	CHECK_STR(dest, "C:\\WINDOWS\\system32");
	/* None installed; buffers too small. */
	cl = sizeof(cur);
	dl = 4;
	CHECK_EQ(VerFindFileA(0, "NONE.DLL", NULL, wdst, cur, &cl, dest, &dl),
		 4);	/* VFF_BUFFTOOSMALL */
	CHECK_STR(cur, "");
	CHECK_EQ(cl, 1);
	CHECK_EQ(dl, strlen(wdst) + 1);
	WCHAR wname[] = { 'A', 'P', 'P', '.', 'D', 'L', 'L', 0 }, wcur[260],
	      wdest[260];
	WCHAR *wapp = malloc((strlen(wdst) + 1) * sizeof(WCHAR));
	for (size_t i = 0; wapp && i <= strlen(wdst); i++)
		wapp[i] = (BYTE)wdst[i];
	cl = 260;
	dl = 260;
	CHECK_EQ(VerFindFileW(0, wname, NULL, wapp, wcur, &cl, wdest, &dl), 0);
	CHECK(wide_is(wdest, wdst) && dl == strlen(wdst) + 1);
	free(wapp);

	/* A compressed file is expanded on the way. */
	snprintf(p, sizeof(p), "%s/DATA.TX_", src);
	write_szdd(p);
	tmp[0] = '\0';
	tl = sizeof(tmp);
	CHECK_EQ(VerInstallFileA(0, "DATA.TX_", "DATA.TXT", wsrc, wdst, NULL,
				 tmp, &tl), 0);
	snprintf(p, sizeof(p), "%s/DATA.TXT", dst);
	CHECK(file_is(p, "abcabcabc", 9));
	CHECK_STR(tmp, "");
	unlink(p);

	/* An older version is refused, then forced from the temporary file. */
	snprintf(p, sizeof(p), "%s/APP.DLL", src);
	CHECK_EQ(write_ne(p, 2, 0, "App"), 0);
	tmp[0] = '\0';
	tl = sizeof(tmp);
	DWORD r = VerInstallFileA(0, "APP.DLL", "APP.DLL", wsrc, wdst, wdst,
				  tmp, &tl);
	CHECK_EQ(r, 0x7);	/* VIF_TEMPFILE | VIF_MISMATCH | VIF_SRCOLD */
	CHECK(tmp[0] && tl == strlen(tmp) + 1);
	snprintf(p, sizeof(p), "%s/%s", dst, tmp);
	CHECK(!access(p, F_OK));
	tl = sizeof(tmp);
	CHECK_EQ(VerInstallFileA(1, "APP.DLL", "APP.DLL", wsrc, wdst, wdst,
				 tmp, &tl), 0);
	CHECK(access(p, F_OK));		/* the temporary file was renamed */
	CHECK_STR(tmp, "");
	snprintf(p, sizeof(p), "%s/APP.DLL", dst);
	VS_FIXEDFILEINFO *fixed = NULL;
	{
		FN(DWORD, GetFileVersionInfoSizeA, LPCSTR, LPDWORD);
		FN(BOOL, GetFileVersionInfoA, LPCSTR, DWORD, DWORD, LPVOID);
		FN(BOOL, VerQueryValueA, LPCVOID, LPCSTR, LPVOID *, PUINT);
		char *w = path_to_windows(p);
		DWORD size = GetFileVersionInfoSizeA(w, NULL);
		BYTE *block = calloc(1, size ? size : 1);
		UINT len;
		CHECK(block && GetFileVersionInfoA(w, 0, size, block) &&
		      VerQueryValueA(block, "\\", (void **)&fixed, &len) &&
		      fixed->dwFileVersionMS == 0x00020000);
		free(block);
		free(w);
	}

	/* A missing source. */
	tmp[0] = '\0';
	tl = sizeof(tmp);
	CHECK_EQ(VerInstallFileA(0, "NONE.DLL", "NONE.DLL", wsrc, wdst, NULL,
				 tmp, &tl), 0x10000);	/* VIF_CANNOTREADSRC */

	snprintf(p, sizeof(p), "%s/APP.DLL", dst);
	unlink(p);
	snprintf(p, sizeof(p), "%s/APP.DLL", src);
	unlink(p);
	snprintf(p, sizeof(p), "%s/DATA.TX_", src);
	unlink(p);
	rmdir(src);
	rmdir(dst);
	free(wsrc);
	free(wdst);
}

int main(void)
{
	CHECK_EQ(runtime_enter(), 0);
	ver = mod_load("version.dll", NULL);
	CHECK(ver && ver->builtin && !ver->builtin->fallback);
	if (!ver)
		return test_summary("test_version");

	test_file();
	test_loaded_module();
	test_builtin();
	test_errors();
	test_ne();
	test_language_name();
	test_install();

	mod_free(ver);
	return test_summary("test_version");
}
