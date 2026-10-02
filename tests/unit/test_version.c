/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * version.dll, called through its export table: version blocks from
 * files, loaded modules and the built-in DLLs, and VerQueryValue on them.
 */
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>

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
	CHECK_STR(s, "libseance PE test fixture");

	WCHAR *w = NULL;
	CHECK(VerQueryValueW(block, product_w, (void **)&w, &len));
	CHECK(w && wide_is(w, "libseance"));
	CHECK_EQ(len, 10);
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
	CHECK_STR(s, "libseance");
	CHECK_EQ(len, 10);
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

	mod_free(ver);
	return test_summary("test_version");
}
