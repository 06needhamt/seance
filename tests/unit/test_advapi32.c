/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * The advapi32 registry, called through the export table the way a DLL's
 * imports reach it, and its .reg file; RegGetValue, change notification,
 * key security and the Drivers32 view of the codec table.
 */
#include <pthread.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "common/error.h"
#include "loader/modules.h"
#include "runtime/runtime.h"
#include "vfw/registry.h"
#include "win32/internal.h"
#include "win32/reg.h"
#include "test.h"

static struct module *adv;
static char dir[256], file[300];

static void *get(const char *name)
{
	void *p = mod_get_proc(adv, name);
	if (!p) {
		fprintf(stderr, "advapi32 has no %s\n", name);
		exit(1);
	}
	return p;
}

#define FN(ret, name, ...) \
	ret (WINAPI *name)(__VA_ARGS__) = (ret (WINAPI *)(__VA_ARGS__))get(#name)

static void write_file(const char *path, const void *data, size_t len)
{
	FILE *f = fopen(path, "wb");
	CHECK(f && fwrite(data, 1, len, f) == len);
	if (f)
		fclose(f);
}

static char *read_text(const char *path)
{
	void *buf;
	size_t len;

	if (vfw_read_file(path, &buf, &len))
		return NULL;
	char *s = realloc(buf, len + 1);
	if (s)
		s[len] = '\0';
	return s;
}

static const WCHAR hello_w[] = { 'h', 'e', 'l', 'l', 'o', 0 };

/* A seeded file: regedit's escapes, types, continuation lines, aliases. */
static void test_seed(void)
{
	FN(LONG, RegOpenKeyExA, HKEY, LPCSTR, DWORD, REGSAM, HKEY *);
	FN(LONG, RegQueryValueExA, HKEY, LPCSTR, LPDWORD, LPDWORD, LPBYTE, LPDWORD);
	FN(LONG, RegQueryValueExW, HKEY, LPCWSTR, LPDWORD, LPDWORD, LPBYTE, LPDWORD);
	FN(LONG, RegCloseKey, HKEY);

	static const char seed[] =
		"Windows Registry Editor Version 5.00\r\n"
		"\r\n"
		"; a comment\r\n"
		"[HKEY_CURRENT_USER\\Software\\Seed]\r\n"
		"@=\"default\"\r\n"
		"\"Path\"=\"C:\\\\Codecs\\\\\\\"x\\\"\"\r\n"
		"\"Count\"=dword:0000002a\r\n"
		"\"Blob\"=hex:01,02,\\\r\n"
		"  03,ff\r\n"
		"\"Expand\"=hex(2):25,00,41,00,25,00,00,00\r\n"
		"\"Euro\"=\"\xe2\x82\xac\"\r\n"
		"\"Gone\"=\"x\"\r\n"
		"\"Gone\"=-\r\n"
		"this line is nonsense\r\n"
		"\r\n"
		"[HKLM\\Software\\Seed2]\r\n"
		"\"Q\"=hex(b):01,00,00,00,00,00,00,00\r\n"
		"[-HKLM\\Software\\Seed2]\r\n"
		"[HKCR\\.tst]\r\n"
		"@=\"tstfile\"\r\n";
	write_file(file, seed, sizeof(seed) - 1);
	reg_reload();

	HKEY k;
	char buf[64];
	DWORD type, cb;
	CHECK_EQ(RegOpenKeyExA(HKEY_CURRENT_USER, "Software\\seed", 0, KEY_READ, &k), 0);

	cb = sizeof(buf);
	CHECK_EQ(RegQueryValueExA(k, NULL, NULL, &type, (BYTE *)buf, &cb), 0);
	CHECK_EQ(type, REG_SZ);
	CHECK_STR(buf, "default");
	CHECK_EQ(cb, 8);

	cb = sizeof(buf);
	CHECK_EQ(RegQueryValueExA(k, "path", NULL, &type, (BYTE *)buf, &cb), 0);
	CHECK_STR(buf, "C:\\Codecs\\\"x\"");

	DWORD d = 0;
	cb = sizeof(d);
	CHECK_EQ(RegQueryValueExA(k, "Count", NULL, &type, (BYTE *)&d, &cb), 0);
	CHECK_EQ(type, REG_DWORD);
	CHECK_EQ(d, 42);

	cb = sizeof(buf);
	CHECK_EQ(RegQueryValueExA(k, "Blob", NULL, &type, (BYTE *)buf, &cb), 0);
	CHECK_EQ(type, REG_BINARY);
	CHECK_EQ(cb, 4);
	CHECK(!memcmp(buf, "\x01\x02\x03\xff", 4));

	cb = sizeof(buf);
	CHECK_EQ(RegQueryValueExA(k, "Expand", NULL, &type, (BYTE *)buf, &cb), 0);
	CHECK_EQ(type, REG_EXPAND_SZ);
	CHECK_STR(buf, "%A%");

	/* U+20AC is 0x80 in the ANSI code page, and itself in UTF-16. */
	cb = sizeof(buf);
	CHECK_EQ(RegQueryValueExA(k, "Euro", NULL, NULL, (BYTE *)buf, &cb), 0);
	CHECK_EQ(cb, 2);
	CHECK_EQ((BYTE)buf[0], 0x80);
	WCHAR w[8];
	static const WCHAR euro_name[] = { 'E', 'u', 'r', 'o', 0 };
	cb = sizeof(w);
	CHECK_EQ(RegQueryValueExW(k, euro_name, NULL, NULL, (BYTE *)w, &cb), 0);
	CHECK_EQ(cb, 4);
	CHECK_EQ(w[0], 0x20ac);

	CHECK_EQ(RegQueryValueExA(k, "Gone", NULL, NULL, NULL, NULL),
		 ERROR_FILE_NOT_FOUND);
	CHECK_EQ(RegCloseKey(k), 0);

	CHECK_EQ(RegOpenKeyExA(HKEY_LOCAL_MACHINE, "Software\\Seed2", 0, KEY_READ, &k),
		 ERROR_FILE_NOT_FOUND);
	/* HKCR is HKLM\Software\Classes. */
	CHECK_EQ(RegOpenKeyExA(HKEY_LOCAL_MACHINE, "Software\\Classes\\.tst", 0,
			       KEY_READ, &k), 0);
	cb = sizeof(buf);
	CHECK_EQ(RegQueryValueExA(k, "", NULL, NULL, (BYTE *)buf, &cb), 0);
	CHECK_STR(buf, "tstfile");
	RegCloseKey(k);

	/* The same file as UTF-16 with a BOM, as regedit exports it. */
	static const char utf16[] =
		"\xff\xfe" "[\0H\0K\0C\0U\0\\\0W\0]\0\n\0\"\0v\0\"\0=\0\"\0w\0\"\0\n\0";
	write_file(file, utf16, sizeof(utf16) - 1);
	reg_reload();
	CHECK_EQ(RegOpenKeyExA(HKEY_CURRENT_USER, "W", 0, KEY_READ, &k), 0);
	cb = sizeof(buf);
	CHECK_EQ(RegQueryValueExA(k, "v", NULL, NULL, (BYTE *)buf, &cb), 0);
	CHECK_STR(buf, "w");
	RegCloseKey(k);
	CHECK_EQ(RegOpenKeyExA(HKEY_CURRENT_USER, "Software\\Seed", 0, KEY_READ, &k),
		 ERROR_FILE_NOT_FOUND);

	/* REGEDIT4: ANSI text, and hex(2) data in ANSI. */
	static const char regedit4[] =
		"REGEDIT4\n\n[HKEY_CURRENT_USER\\Old]\n\"A\"=\"\x80\"\n"
		"\"E\"=hex(2):25,41,25,00\n";
	write_file(file, regedit4, sizeof(regedit4) - 1);
	reg_reload();
	CHECK_EQ(RegOpenKeyExA(HKEY_CURRENT_USER, "Old", 0, KEY_READ, &k), 0);
	cb = sizeof(w);
	static const WCHAR a_name[] = { 'A', 0 }, e_name[] = { 'E', 0 };
	CHECK_EQ(RegQueryValueExW(k, a_name, NULL, NULL, (BYTE *)w, &cb), 0);
	CHECK_EQ(w[0], 0x20ac);
	cb = sizeof(w);
	CHECK_EQ(RegQueryValueExW(k, e_name, NULL, &type, (BYTE *)w, &cb), 0);
	CHECK_EQ(type, REG_EXPAND_SZ);
	CHECK_EQ(cb, 8);
	CHECK_EQ(w[1], 'A');
	RegCloseKey(k);
	unlink(file);
}

/* Changes are written to the file straight away and read back. */
static void test_round_trip(void)
{
	FN(LONG, RegCreateKeyExA, HKEY, LPCSTR, DWORD, LPSTR, DWORD, REGSAM,
	   LPSECURITY_ATTRIBUTES, HKEY *, LPDWORD);
	FN(LONG, RegOpenKeyExW, HKEY, LPCWSTR, DWORD, REGSAM, HKEY *);
	FN(LONG, RegSetValueExA, HKEY, LPCSTR, DWORD, DWORD, const BYTE *, DWORD);
	FN(LONG, RegSetValueExW, HKEY, LPCWSTR, DWORD, DWORD, const BYTE *, DWORD);
	FN(LONG, RegQueryValueExA, HKEY, LPCSTR, LPDWORD, LPDWORD, LPBYTE, LPDWORD);
	FN(LONG, RegQueryValueExW, HKEY, LPCWSTR, LPDWORD, LPDWORD, LPBYTE, LPDWORD);
	FN(LONG, RegCloseKey, HKEY);

	reg_reload();
	HKEY k;
	DWORD disp = 0;
	CHECK_EQ(RegCreateKeyExA(HKEY_CURRENT_USER, "Software\\Test\\Codec", 0,
				 NULL, 0, KEY_ALL_ACCESS, NULL, &k, &disp), 0);
	CHECK_EQ(disp, REG_CREATED_NEW_KEY);

	DWORD d = 0x12345678;
	static const WCHAR name_w[] = { 'W', 'i', 'd', 'e', 0 };
	static const char multi[] = "one\0two\0";
	static const BYTE blob[40] = { 1, 2, 3 };
	CHECK_EQ(RegSetValueExA(k, "Quality", 0, REG_DWORD, (BYTE *)&d, 4), 0);
	CHECK_EQ(RegSetValueExA(k, "Name", 0, REG_SZ, (BYTE *)"a \"q\" \\", 8), 0);
	CHECK_EQ(RegSetValueExW(k, name_w, 0, REG_SZ, (const BYTE *)hello_w,
				sizeof(hello_w)), 0);
	CHECK_EQ(RegSetValueExA(k, "Multi", 0, REG_MULTI_SZ, (BYTE *)multi,
				sizeof(multi)), 0);
	CHECK_EQ(RegSetValueExA(k, "Blob", 0, REG_BINARY, blob, sizeof(blob)), 0);
	CHECK_EQ(RegSetValueExA(k, "Lines", 0, REG_SZ, (BYTE *)"a\nb", 4), 0);
	CHECK_EQ(RegSetValueExA(k, NULL, 0, REG_SZ, (BYTE *)"dflt", 5), 0);
	CHECK_EQ(RegSetValueExA(k, "Bad", 0, REG_BINARY, NULL, 4), ERROR_NOACCESS);
	CHECK_EQ(RegCloseKey(k), 0);

	/* A volatile key is never saved. */
	CHECK_EQ(RegCreateKeyExA(HKEY_CURRENT_USER, "Software\\Test\\Volatile", 0,
				 NULL, REG_OPTION_VOLATILE, KEY_ALL_ACCESS, NULL,
				 &k, NULL), 0);
	RegCloseKey(k);

	char *text = read_text(file);
	CHECK(text != NULL);
	if (text) {
		CHECK(strstr(text, "[HKEY_CURRENT_USER\\Software\\Test\\Codec]") != NULL);
		CHECK(strstr(text, "\"Quality\"=dword:12345678") != NULL);
		CHECK(strstr(text, "\"Name\"=\"a \\\"q\\\" \\\\\"") != NULL);
		CHECK(strstr(text, "@=\"dflt\"") != NULL);
		CHECK(strstr(text, "\"Multi\"=hex(7):6f,00,6e,00,65,00,00,00,74") != NULL);
		CHECK(strstr(text, "\"Lines\"=hex(1):") != NULL);
		CHECK(strstr(text, "Volatile") == NULL);
		free(text);
	}

	reg_reload();
	static const WCHAR path_w[] = { 'S', 'O', 'F', 'T', 'W', 'A', 'R', 'E',
		'\\', 't', 'e', 's', 't', '\\', 'c', 'o', 'd', 'e', 'c', 0 };
	CHECK_EQ(RegOpenKeyExW(HKEY_CURRENT_USER, path_w, 0, KEY_READ, &k), 0);
	char buf[64];
	BYTE bin[64];
	DWORD type, cb = sizeof(buf);
	CHECK_EQ(RegQueryValueExA(k, "Name", NULL, &type, (BYTE *)buf, &cb), 0);
	CHECK_STR(buf, "a \"q\" \\");
	cb = sizeof(buf);
	CHECK_EQ(RegQueryValueExA(k, "Wide", NULL, NULL, (BYTE *)buf, &cb), 0);
	CHECK_STR(buf, "hello");
	CHECK_EQ(cb, 6);
	cb = sizeof(buf);
	CHECK_EQ(RegQueryValueExA(k, "Multi", NULL, &type, (BYTE *)buf, &cb), 0);
	CHECK_EQ(type, REG_MULTI_SZ);
	CHECK_EQ(cb, sizeof(multi));
	CHECK(!memcmp(buf, multi, sizeof(multi)));
	cb = sizeof(bin);
	CHECK_EQ(RegQueryValueExA(k, "Blob", NULL, &type, bin, &cb), 0);
	CHECK_EQ(cb, sizeof(blob));
	CHECK(!memcmp(bin, blob, sizeof(blob)));
	cb = sizeof(buf);
	CHECK_EQ(RegQueryValueExA(k, "Lines", NULL, &type, (BYTE *)buf, &cb), 0);
	CHECK_EQ(type, REG_SZ);
	CHECK_STR(buf, "a\nb");

	/* Sizing: a NULL buffer asks, a small one gets ERROR_MORE_DATA. */
	WCHAR w[8];
	cb = 0;
	CHECK_EQ(RegQueryValueExW(k, name_w, NULL, &type, NULL, &cb), 0);
	CHECK_EQ(cb, sizeof(hello_w));
	cb = 4;
	CHECK_EQ(RegQueryValueExW(k, name_w, NULL, NULL, (BYTE *)w, &cb),
		 ERROR_MORE_DATA);
	CHECK_EQ(cb, sizeof(hello_w));
	CHECK_EQ(RegQueryValueExW(k, name_w, NULL, NULL, (BYTE *)w, NULL),
		 ERROR_INVALID_PARAMETER);
	type = 0;
	CHECK_EQ(RegQueryValueExA(k, "Quality", NULL, &type, NULL, NULL), 0);
	CHECK_EQ(type, REG_DWORD);
	CHECK_EQ(RegCloseKey(k), 0);
	CHECK_EQ(RegOpenKeyExW(HKEY_CURRENT_USER, path_w, 0, KEY_READ, &k), 0);
	RegCloseKey(k);

	HKEY v;
	CHECK_EQ(RegCreateKeyExA(HKEY_CURRENT_USER, "Software\\Test\\Volatile", 0,
				 NULL, 0, KEY_ALL_ACCESS, NULL, &v, &disp), 0);
	CHECK_EQ(disp, REG_CREATED_NEW_KEY);
	RegCloseKey(v);
}

static void test_enum_delete(void)
{
	FN(LONG, RegCreateKeyA, HKEY, LPCSTR, HKEY *);
	FN(LONG, RegOpenKeyA, HKEY, LPCSTR, HKEY *);
	FN(LONG, RegOpenKeyExA, HKEY, LPCSTR, DWORD, REGSAM, HKEY *);
	FN(LONG, RegEnumKeyExA, HKEY, DWORD, LPSTR, LPDWORD, LPDWORD, LPSTR,
	   LPDWORD, PFILETIME);
	FN(LONG, RegEnumKeyW, HKEY, DWORD, LPWSTR, DWORD);
	FN(LONG, RegEnumValueA, HKEY, DWORD, LPSTR, LPDWORD, LPDWORD, LPDWORD,
	   LPBYTE, LPDWORD);
	FN(LONG, RegQueryInfoKeyA, HKEY, LPSTR, LPDWORD, LPDWORD, LPDWORD,
	   LPDWORD, LPDWORD, LPDWORD, LPDWORD, LPDWORD, LPDWORD, PFILETIME);
	FN(LONG, RegSetValueA, HKEY, LPCSTR, DWORD, LPCSTR, DWORD);
	FN(LONG, RegQueryValueA, HKEY, LPCSTR, LPSTR, PLONG);
	FN(LONG, RegDeleteKeyA, HKEY, LPCSTR);
	FN(LONG, RegDeleteValueA, HKEY, LPCSTR);
	FN(LONG, RegQueryValueExA, HKEY, LPCSTR, LPDWORD, LPDWORD, LPBYTE, LPDWORD);
	FN(LONG, RegSetValueExA, HKEY, LPCSTR, DWORD, DWORD, const BYTE *, DWORD);
	FN(LONG, RegCloseKey, HKEY);

	HKEY k, sub;
	CHECK_EQ(RegCreateKeyA(HKEY_LOCAL_MACHINE, "Software\\Enum", &k), 0);
	/* Subkeys come back sorted; values in creation order. */
	CHECK_EQ(RegSetValueA(k, "zeta", REG_SZ, "z", 0), 0);
	CHECK_EQ(RegSetValueA(k, "Alpha", REG_SZ, "a", 0), 0);
	CHECK_EQ(RegSetValueA(k, "mid", REG_SZ, "m", 0), 0);
	CHECK_EQ(RegSetValueA(k, "x", REG_DWORD, "m", 0), ERROR_INVALID_PARAMETER);
	CHECK_EQ(RegSetValueExA(k, "v2", 0, REG_BINARY, (BYTE *)"\1", 1), 0);
	CHECK_EQ(RegSetValueExA(k, "v1", 0, REG_SZ, (BYTE *)"long value", 11), 0);

	char name[32];
	DWORD cch = sizeof(name);
	CHECK_EQ(RegEnumKeyExA(k, 0, name, &cch, NULL, NULL, NULL, NULL), 0);
	CHECK_STR(name, "Alpha");
	CHECK_EQ(cch, 5);
	cch = 3;
	CHECK_EQ(RegEnumKeyExA(k, 1, name, &cch, NULL, NULL, NULL, NULL),
		 ERROR_MORE_DATA);
	WCHAR w[16];
	CHECK_EQ(RegEnumKeyW(k, 2, w, 16), 0);
	CHECK(w[0] == 'z' && w[4] == 0);
	CHECK_EQ(RegEnumKeyW(k, 3, w, 16), ERROR_NO_MORE_ITEMS);

	DWORD type, cb;
	BYTE data[32];
	cch = sizeof(name);
	cb = sizeof(data);
	CHECK_EQ(RegEnumValueA(k, 0, name, &cch, NULL, &type, data, &cb), 0);
	CHECK_STR(name, "v2");
	CHECK_EQ(type, REG_BINARY);
	CHECK_EQ(cb, 1);
	cch = sizeof(name);
	cb = 4;
	CHECK_EQ(RegEnumValueA(k, 1, name, &cch, NULL, &type, data, &cb),
		 ERROR_MORE_DATA);
	CHECK_EQ(cb, 11);
	cch = sizeof(name);
	CHECK_EQ(RegEnumValueA(k, 2, name, &cch, NULL, NULL, NULL, NULL),
		 ERROR_NO_MORE_ITEMS);

	DWORD nsub, maxsub, nval, maxname, maxdata;
	CHECK_EQ(RegQueryInfoKeyA(k, NULL, NULL, NULL, &nsub, &maxsub, NULL, &nval,
				  &maxname, &maxdata, NULL, NULL), 0);
	CHECK_EQ(nsub, 3);
	CHECK_EQ(maxsub, 5);
	CHECK_EQ(nval, 2);
	CHECK_EQ(maxname, 2);
	CHECK_EQ(maxdata, 22);

	/* The old default-value calls. */
	char buf[16];
	LONG lcb = sizeof(buf);
	CHECK_EQ(RegQueryValueA(k, "mid", buf, &lcb), 0);
	CHECK_STR(buf, "m");
	CHECK_EQ(lcb, 2);
	CHECK_EQ(RegCreateKeyA(k, "empty", &sub), 0);
	RegCloseKey(sub);
	lcb = sizeof(buf);
	CHECK_EQ(RegQueryValueA(k, "empty", buf, &lcb), 0);
	CHECK_STR(buf, "");
	CHECK_EQ(lcb, 1);
	lcb = sizeof(buf);
	CHECK_EQ(RegQueryValueA(k, "nothere", buf, &lcb), ERROR_FILE_NOT_FOUND);

	/* RegOpenKey of "" hands back the same handle. */
	CHECK_EQ(RegOpenKeyA(k, "", &sub), 0);
	CHECK(sub == k);
	CHECK_EQ(RegOpenKeyExA(k, "\\mid", 0, KEY_READ, &sub), ERROR_BAD_PATHNAME);

	/* A key with subkeys can't be deleted; an open one reads as deleted. */
	CHECK_EQ(RegOpenKeyExA(k, "mid", 0, KEY_READ, &sub), 0);
	CHECK_EQ(RegDeleteKeyA(HKEY_LOCAL_MACHINE, "Software\\Enum"),
		 ERROR_ACCESS_DENIED);
	CHECK_EQ(RegDeleteKeyA(k, "mid"), 0);
	CHECK_EQ(RegQueryValueExA(sub, "", NULL, NULL, NULL, NULL), ERROR_KEY_DELETED);
	CHECK_EQ(RegCloseKey(sub), 0);
	CHECK_EQ(RegDeleteKeyA(k, "mid"), ERROR_FILE_NOT_FOUND);
	CHECK_EQ(RegDeleteValueA(k, "v2"), 0);
	CHECK_EQ(RegDeleteValueA(k, "v2"), ERROR_FILE_NOT_FOUND);
	CHECK_EQ(RegDeleteKeyA(HKEY_LOCAL_MACHINE, "Software"), ERROR_ACCESS_DENIED);
	CHECK_EQ(RegDeleteKeyA(HKEY_LOCAL_MACHINE, ""), ERROR_ACCESS_DENIED);

	CHECK_EQ(RegCloseKey(k), 0);
	CHECK_EQ(RegCloseKey(k), ERROR_INVALID_HANDLE);
	CHECK_EQ(RegCloseKey(HKEY_LOCAL_MACHINE), 0);
	CHECK_EQ(RegQueryValueExA((HKEY)0x1234, "", NULL, NULL, NULL, NULL),
		 ERROR_INVALID_HANDLE);
}

static void test_user_name(void)
{
	FN(BOOL, GetUserNameA, LPSTR, LPDWORD);
	FN(BOOL, GetUserNameW, LPWSTR, LPDWORD);

	setenv("USER", "tester", 1);
	char buf[16];
	WCHAR w[16];
	DWORD n = 3;
	CHECK(!GetUserNameA(buf, &n));
	CHECK_EQ(n, 7);
	n = sizeof(buf);
	CHECK(GetUserNameA(buf, &n));
	CHECK_STR(buf, "tester");
	CHECK_EQ(n, 7);
	n = 16;
	CHECK(GetUserNameW(w, &n));
	CHECK(w[0] == 't' && w[6] == 0);
}

/* VFW_REGISTRY set but empty: nothing is written anywhere. */
static void test_memory_only(void)
{
	FN(LONG, RegCreateKeyA, HKEY, LPCSTR, HKEY *);
	FN(LONG, RegCloseKey, HKEY);
	char other[300];
	struct stat st;

	snprintf(other, sizeof(other), "%s/registry.reg", dir);
	setenv("VFW_ROOT", dir, 1);
	setenv("VFW_REGISTRY", "", 1);
	reg_reload();
	HKEY k;
	CHECK_EQ(RegCreateKeyA(HKEY_CURRENT_USER, "Volatile\\Everything", &k), 0);
	RegCloseKey(k);
	CHECK(stat(other, &st) != 0);

	/* Unset, it defaults to $VFW_ROOT/registry.reg. */
	unsetenv("VFW_REGISTRY");
	reg_reload();
	CHECK_EQ(RegCreateKeyA(HKEY_CURRENT_USER, "Saved", &k), 0);
	RegCloseKey(k);
	CHECK(stat(other, &st) == 0);
	setenv("VFW_REGISTRY", file, 1);
}

/*
 * A sandbox: a copy of the registry that changes like the real one but is
 * never saved, then thrown away.
 */
static void test_sandbox(void)
{
	FN(LONG, RegCreateKeyA, HKEY, LPCSTR, HKEY *);
	FN(LONG, RegOpenKeyA, HKEY, LPCSTR, HKEY *);
	FN(LONG, RegCloseKey, HKEY);
	FN(LONG, RegSetValueExA, HKEY, LPCSTR, DWORD, DWORD, const BYTE *,
	   DWORD);
	FN(LONG, RegQueryValueExA, HKEY, LPCSTR, LPDWORD, LPDWORD, LPBYTE,
	   LPDWORD);
	HKEY real, k;
	DWORD v, size, type;

	reg_reload();
	CHECK_EQ(RegCreateKeyA(HKEY_CURRENT_USER, "Software\\Real", &real), 0);
	v = 1;
	CHECK_EQ(RegSetValueExA(real, "Value", 0, REG_DWORD, (BYTE *)&v,
				sizeof(v)), 0);

	CHECK_EQ(reg_sandbox_begin(), ERROR_SUCCESS);
	v = 2;
	CHECK_EQ(RegSetValueExA(real, "Value", 0, REG_DWORD, (BYTE *)&v,
				sizeof(v)), 0);
	CHECK_EQ(RegCreateKeyA(HKEY_CURRENT_USER, "Software\\Sandboxed", &k), 0);
	RegCloseKey(k);
	CHECK_EQ(RegOpenKeyA(HKEY_CURRENT_USER, "Software\\Sandboxed", &k), 0);
	RegCloseKey(k);
	size = sizeof(v);
	CHECK_EQ(RegQueryValueExA(real, "Value", NULL, &type, (BYTE *)&v,
				  &size), 0);
	CHECK_EQ(v, 2);
	char *text = read_text(file);
	CHECK(text && strstr(text, "Real") && !strstr(text, "Sandboxed"));
	free(text);
	reg_sandbox_end();

	/* Handles opened before keep working on the real tree. */
	size = sizeof(v);
	CHECK_EQ(RegQueryValueExA(real, "Value", NULL, &type, (BYTE *)&v,
				  &size), 0);
	CHECK_EQ(v, 1);
	CHECK(RegOpenKeyA(HKEY_CURRENT_USER, "Software\\Sandboxed", &k) != 0);
	v = 3;
	CHECK_EQ(RegSetValueExA(real, "Value", 0, REG_DWORD, (BYTE *)&v,
				sizeof(v)), 0);
	text = read_text(file);
	CHECK(text && strstr(text, "dword:00000003") &&
	      !strstr(text, "Sandboxed"));
	free(text);
	RegCloseKey(real);
	reg_sandbox_end();	/* outside one: nothing */
}

static void test_fixture(void)
{
	const char *d = getenv("VFW_FIXTURES");
	char path[512];

	snprintf(path, sizeof(path), "%s/pe_reg.dll", d ? d : ".");
	if (!d || access(path, R_OK)) {
		printf("test_advapi32: pe_reg.dll not built, skipping the fixture\n");
		return;
	}
	reg_reload();
	struct module *m = mod_load(path, NULL);
	CHECK(m != NULL);
	if (!m)
		return;
	int (*write)(void) = mod_get_proc(m, "reg_write");
	int (*read)(void) = mod_get_proc(m, "reg_read");
	CHECK(write && read);
	if (write && read) {
		CHECK_EQ(write(), 0);
		/* Read back in a fresh registry, from the file. */
		reg_reload();
		CHECK_EQ(read(), 0);
	}
	mod_free(m);
}

static void test_get_value(void)
{
	FN(LONG, RegCreateKeyExA, HKEY, LPCSTR, DWORD, LPSTR, DWORD, REGSAM,
	   void *, HKEY *, LPDWORD);
	FN(LONG, RegSetValueExA, HKEY, LPCSTR, DWORD, DWORD, const BYTE *, DWORD);
	FN(LONG, RegSetValueExW, HKEY, LPCWSTR, DWORD, DWORD, const BYTE *, DWORD);
	FN(LONG, RegGetValueA, HKEY, LPCSTR, LPCSTR, DWORD, LPDWORD, PVOID, LPDWORD);
	FN(LONG, RegGetValueW, HKEY, LPCWSTR, LPCWSTR, DWORD, LPDWORD, PVOID,
	   LPDWORD);
	FN(LONG, RegCloseKey, HKEY);
	HKEY k;
	char buf[64];
	DWORD t, cb, dw = 42;
	static const WCHAR sz_w[] = { 's', 'z', 0 }, abc[] = { 'a', 'b', 'c' };
	const BYTE bin4[4] = { 1, 2, 3, 4 }, bin3[3] = { 1, 2, 3 };

	CHECK_EQ(RegCreateKeyExA(HKEY_CURRENT_USER, "Software\\GV", 0, NULL, 0,
				 KEY_ALL_ACCESS, NULL, &k, NULL), 0);
	/* "abc" stored without its NUL. */
	CHECK_EQ(RegSetValueExW(k, sz_w, 0, REG_SZ, (const BYTE *)abc,
				sizeof(abc)), 0);
	CHECK_EQ(RegSetValueExA(k, "dw", 0, REG_DWORD, (BYTE *)&dw, 4), 0);
	CHECK_EQ(RegSetValueExA(k, "bin4", 0, REG_BINARY, bin4, 4), 0);
	CHECK_EQ(RegSetValueExA(k, "bin3", 0, REG_BINARY, bin3, 3), 0);
	CHECK_EQ(RegSetValueExA(k, "ex", 0, REG_EXPAND_SZ,
				(const BYTE *)"%SystemRoot%\\x", 15), 0);

	/* Through a subkey, NUL-terminated. */
	cb = sizeof(buf);
	CHECK_EQ(RegGetValueA(HKEY_CURRENT_USER, "Software\\GV", "sz",
			      RRF_RT_REG_SZ, &t, buf, &cb), 0);
	CHECK_STR(buf, "abc");
	CHECK(t == REG_SZ && cb == 4);
	WCHAR wbuf[16];
	cb = sizeof(wbuf);
	CHECK_EQ(RegGetValueW(k, NULL, sz_w, RRF_RT_ANY, &t, wbuf, &cb), 0);
	CHECK(cb == 8 && wbuf[2] == 'c' && wbuf[3] == 0);

	/* The type filter. */
	cb = sizeof(buf);
	CHECK_EQ(RegGetValueA(k, NULL, "dw", RRF_RT_REG_SZ, &t, buf, &cb),
		 ERROR_UNSUPPORTED_TYPE);
	DWORD v = 0;
	cb = sizeof(v);
	CHECK_EQ(RegGetValueA(k, NULL, "dw", RRF_RT_DWORD, &t, &v, &cb), 0);
	CHECK(v == 42 && t == REG_DWORD);
	cb = sizeof(v);
	CHECK_EQ(RegGetValueA(k, NULL, "bin4", RRF_RT_DWORD, &t, &v, &cb), 0);
	CHECK(t == REG_BINARY && v == 0x04030201);
	cb = sizeof(v);
	CHECK_EQ(RegGetValueA(k, NULL, "bin3", RRF_RT_DWORD, &t, &v, &cb),
		 ERROR_DATATYPE_MISMATCH);
	CHECK_EQ(RegGetValueA(k, NULL, "dw", 0, &t, &v, &cb),
		 ERROR_INVALID_PARAMETER);

	/* Expansion, only when any type may come back. */
	cb = sizeof(buf);
	CHECK_EQ(RegGetValueA(k, NULL, "ex", RRF_RT_ANY, &t, buf, &cb), 0);
	CHECK_STR(buf, "C:\\WINDOWS\\x");
	CHECK(t == REG_SZ && cb == 13);
	cb = sizeof(buf);
	CHECK_EQ(RegGetValueA(k, NULL, "ex", RRF_RT_REG_EXPAND_SZ, &t, buf, &cb),
		 ERROR_INVALID_PARAMETER);
	cb = sizeof(buf);
	CHECK_EQ(RegGetValueA(k, NULL, "ex", RRF_RT_REG_EXPAND_SZ | RRF_NOEXPAND,
			      &t, buf, &cb), 0);
	CHECK_STR(buf, "%SystemRoot%\\x");
	CHECK_EQ(t, REG_EXPAND_SZ);

	/* Sizes, short buffers and misses. */
	cb = 0;
	CHECK_EQ(RegGetValueA(k, NULL, "sz", RRF_RT_REG_SZ, NULL, NULL, &cb), 0);
	CHECK_EQ(cb, 4);
	memset(buf, 'x', sizeof(buf));
	cb = 2;
	CHECK_EQ(RegGetValueA(k, NULL, "sz", RRF_RT_REG_SZ | RRF_ZEROONFAILURE,
			      NULL, buf, &cb), ERROR_MORE_DATA);
	CHECK(cb == 4 && buf[0] == 0 && buf[1] == 0 && buf[2] == 'x');
	cb = sizeof(buf);
	CHECK_EQ(RegGetValueA(k, NULL, "nope", RRF_RT_ANY, NULL, buf, &cb),
		 ERROR_FILE_NOT_FOUND);
	CHECK_EQ(RegGetValueA(HKEY_CURRENT_USER, "Software\\NoSuchKey", "sz",
			      RRF_RT_ANY, NULL, buf, &cb), ERROR_FILE_NOT_FOUND);
	/* Zeroed on failure even when the subkey isn't there. */
	memset(buf, 'x', sizeof(buf));
	cb = 8;
	CHECK_EQ(RegGetValueA(HKEY_CURRENT_USER, "Software\\NoSuchKey", "sz",
			      RRF_RT_ANY | RRF_ZEROONFAILURE, NULL, buf, &cb),
		 ERROR_FILE_NOT_FOUND);
	CHECK(!memcmp(buf, "\0\0\0\0\0\0\0\0", 8) && buf[8] == 'x');

	/* Multi-strings end with an empty string: two NULs. */
	CHECK_EQ(RegSetValueExA(k, "ms", 0, REG_MULTI_SZ, (const BYTE *)"a\0b",
				3), 0);
	memset(buf, 'x', sizeof(buf));
	cb = sizeof(buf);
	CHECK_EQ(RegGetValueA(k, NULL, "ms", RRF_RT_REG_MULTI_SZ, &t, buf, &cb),
		 0);
	CHECK(cb == 5 && !memcmp(buf, "a\0b\0\0", 5));
	CHECK_EQ(RegSetValueExA(k, "ms", 0, REG_MULTI_SZ,
				(const BYTE *)"a\0b\0", 4), 0);
	cb = sizeof(buf);
	CHECK_EQ(RegGetValueA(k, NULL, "ms", RRF_RT_REG_MULTI_SZ, &t, buf, &cb),
		 0);
	CHECK(cb == 5 && !memcmp(buf, "a\0b\0\0", 5));
	static const WCHAR ms_w[] = { 'm', 's', 0 }, one[] = { 'x' };
	CHECK_EQ(RegSetValueExW(k, ms_w, 0, REG_MULTI_SZ, (const BYTE *)one,
				sizeof(one)), 0);
	cb = sizeof(wbuf);
	CHECK_EQ(RegGetValueW(k, NULL, ms_w, RRF_RT_ANY, &t, wbuf, &cb), 0);
	CHECK(cb == 6 && wbuf[0] == 'x' && !wbuf[1] && !wbuf[2]);
	CHECK_EQ(RegSetValueExA(k, "ms", 0, REG_MULTI_SZ, (const BYTE *)"", 1),
		 0);
	cb = sizeof(buf);
	CHECK_EQ(RegGetValueA(k, NULL, "ms", RRF_RT_ANY, &t, buf, &cb), 0);
	CHECK(cb == 2 && !buf[0] && !buf[1]);
	RegCloseKey(k);
}

/* Sets a value from a thread of its own, after a pause. */
static void *set_later(void *arg)
{
	usleep(50000);
	reg_lock();
	struct reg_key *k = reg_open("HKEY_CURRENT_USER\\Software\\Watch");
	if (k)
		reg_set(k, "late", REG_DWORD, "\1\0\0\0", 4);
	reg_unlock();
	return NULL;
}

static void test_notify(void)
{
	FN(LONG, RegCreateKeyExA, HKEY, LPCSTR, DWORD, LPSTR, DWORD, REGSAM,
	   void *, HKEY *, LPDWORD);
	FN(LONG, RegOpenKeyExA, HKEY, LPCSTR, DWORD, REGSAM, HKEY *);
	FN(LONG, RegSetValueExA, HKEY, LPCSTR, DWORD, DWORD, const BYTE *, DWORD);
	FN(LONG, RegDeleteKeyA, HKEY, LPCSTR);
	FN(LONG, RegNotifyChangeKeyValue, HKEY, BOOL, DWORD, HANDLE, BOOL);
	FN(LONG, RegCloseKey, HKEY);
	struct module *k32 = mod_load("kernel32.dll", NULL);
	HANDLE (WINAPI *create_event)(void *, BOOL, BOOL, LPCSTR) =
		mod_get_proc(k32, "CreateEventA");
	DWORD (WINAPI *wait)(HANDLE, DWORD) =
		mod_get_proc(k32, "WaitForSingleObject");
	BOOL (WINAPI *reset)(HANDLE) = mod_get_proc(k32, "ResetEvent");
	BOOL (WINAPI *close_handle)(HANDLE) = mod_get_proc(k32, "CloseHandle");
	HKEY k, sub, other;
	DWORD one = 1;

	CHECK(create_event && wait && reset && close_handle);
	if (!create_event || !wait || !reset || !close_handle)
		return;
	HANDLE ev = create_event(NULL, TRUE, FALSE, NULL);
	CHECK_EQ(RegCreateKeyExA(HKEY_CURRENT_USER, "Software\\Watch", 0, NULL,
				 0, KEY_ALL_ACCESS, NULL, &k, NULL), 0);

	/* A value set: once. */
	CHECK_EQ(RegNotifyChangeKeyValue(k, FALSE, REG_NOTIFY_CHANGE_LAST_SET,
					 ev, TRUE), 0);
	CHECK_EQ(wait(ev, 0), WAIT_TIMEOUT);
	CHECK_EQ(RegSetValueExA(k, "a", 0, REG_DWORD, (BYTE *)&one, 4), 0);
	CHECK_EQ(wait(ev, 0), WAIT_OBJECT_0);
	reset(ev);
	CHECK_EQ(RegSetValueExA(k, "b", 0, REG_DWORD, (BYTE *)&one, 4), 0);
	CHECK_EQ(wait(ev, 0), WAIT_TIMEOUT);

	/* The filter: names only, so a value isn't a change. */
	CHECK_EQ(RegNotifyChangeKeyValue(k, FALSE, REG_NOTIFY_CHANGE_NAME, ev,
					 TRUE), 0);
	CHECK_EQ(RegSetValueExA(k, "c", 0, REG_DWORD, (BYTE *)&one, 4), 0);
	CHECK_EQ(wait(ev, 0), WAIT_TIMEOUT);
	CHECK_EQ(RegCreateKeyExA(k, "Sub", 0, NULL, 0, KEY_ALL_ACCESS, NULL,
				 &sub, NULL), 0);
	CHECK_EQ(wait(ev, 0), WAIT_OBJECT_0);
	reset(ev);

	/* Below the key, only with subtree. */
	CHECK_EQ(RegNotifyChangeKeyValue(k, FALSE, REG_NOTIFY_CHANGE_LAST_SET,
					 ev, TRUE), 0);
	CHECK_EQ(RegSetValueExA(sub, "d", 0, REG_DWORD, (BYTE *)&one, 4), 0);
	CHECK_EQ(wait(ev, 0), WAIT_TIMEOUT);
	HANDLE ev2 = create_event(NULL, TRUE, FALSE, NULL);
	CHECK_EQ(RegNotifyChangeKeyValue(k, TRUE, REG_NOTIFY_CHANGE_LAST_SET,
					 ev2, TRUE), 0);
	CHECK_EQ(RegSetValueExA(sub, "e", 0, REG_DWORD, (BYTE *)&one, 4), 0);
	CHECK_EQ(wait(ev2, 0), WAIT_OBJECT_0);
	/* The first watch is still there, and ends with its handle. */
	CHECK_EQ(wait(ev, 0), WAIT_TIMEOUT);

	/* Deleting the key watched, and closing the handle watched on. */
	reset(ev2);
	CHECK_EQ(RegNotifyChangeKeyValue(sub, FALSE, REG_NOTIFY_CHANGE_LAST_SET,
					 ev2, TRUE), 0);
	CHECK_EQ(RegDeleteKeyA(k, "Sub"), 0);
	CHECK_EQ(wait(ev2, 0), WAIT_OBJECT_0);
	RegCloseKey(sub);
	CHECK_EQ(wait(ev, 0), WAIT_TIMEOUT);
	CHECK_EQ(RegOpenKeyExA(HKEY_CURRENT_USER, "Software\\Watch", 0,
			       KEY_READ, &other), 0);
	reset(ev2);
	CHECK_EQ(RegNotifyChangeKeyValue(other, FALSE, REG_NOTIFY_CHANGE_NAME,
					 ev2, TRUE), 0);
	RegCloseKey(other);
	CHECK_EQ(wait(ev2, 0), WAIT_OBJECT_0);

	/* Waiting, until another thread sets a value. */
	pthread_t th;
	CHECK_EQ(pthread_create(&th, NULL, set_later, NULL), 0);
	CHECK_EQ(RegNotifyChangeKeyValue(k, FALSE, REG_NOTIFY_CHANGE_LAST_SET,
					 NULL, FALSE), 0);
	pthread_join(th, NULL);

	CHECK_EQ(RegNotifyChangeKeyValue(k, FALSE, 0, ev, TRUE),
		 ERROR_INVALID_PARAMETER);
	CHECK_EQ(RegNotifyChangeKeyValue(k, FALSE, REG_NOTIFY_CHANGE_NAME, NULL,
					 TRUE), ERROR_INVALID_PARAMETER);
	RegCloseKey(k);
	CHECK_EQ(wait(ev, 0), WAIT_OBJECT_0);	/* its handle closed */
	close_handle(ev);
	close_handle(ev2);
	mod_free(k32);
}

static void test_key_security(void)
{
	FN(LONG, RegOpenKeyExA, HKEY, LPCSTR, DWORD, REGSAM, HKEY *);
	FN(LONG, RegGetKeySecurity, HKEY, SECURITY_INFORMATION,
	   PSECURITY_DESCRIPTOR, LPDWORD);
	FN(LONG, RegSetKeySecurity, HKEY, SECURITY_INFORMATION,
	   PSECURITY_DESCRIPTOR);
	FN(BOOL, IsValidSecurityDescriptor, PSECURITY_DESCRIPTOR);
	FN(LONG, RegOpenCurrentUser, REGSAM, HKEY *);
	FN(LONG, RegGetValueA, HKEY, LPCSTR, LPCSTR, DWORD, LPDWORD, PVOID, LPDWORD);
	FN(LONG, RegDisablePredefinedCache, void);
	FN(LONG, RegCloseKey, HKEY);
	HKEY k, cu;
	BYTE sd[512];
	DWORD cb = 0;
	const SECURITY_INFORMATION all = OWNER_SECURITY_INFORMATION |
		GROUP_SECURITY_INFORMATION | DACL_SECURITY_INFORMATION;

	CHECK_EQ(RegOpenKeyExA(HKEY_CURRENT_USER, "Software\\GV", 0, KEY_READ,
			       &k), 0);
	CHECK_EQ(RegGetKeySecurity(k, all, NULL, &cb), ERROR_INSUFFICIENT_BUFFER);
	CHECK(cb > SECURITY_DESCRIPTOR_MIN_LENGTH && cb <= sizeof(sd));
	DWORD need = cb;
	CHECK_EQ(RegGetKeySecurity(k, all, sd, &cb), 0);
	CHECK_EQ(cb, need);
	CHECK(IsValidSecurityDescriptor(sd));
	CHECK(((SECURITY_DESCRIPTOR_RELATIVE *)sd)->Control & SE_SELF_RELATIVE);
	CHECK(((SECURITY_DESCRIPTOR_RELATIVE *)sd)->Control & SE_DACL_PRESENT);
	CHECK_EQ(RegSetKeySecurity(k, DACL_SECURITY_INFORMATION, sd), 0);
	CHECK_EQ(RegSetKeySecurity(k, DACL_SECURITY_INFORMATION, NULL),
		 ERROR_INVALID_PARAMETER);
	RegCloseKey(k);
	CHECK_EQ(RegGetKeySecurity((HKEY)(ULONG_PTR)0x1234, all, sd, &cb),
		 ERROR_INVALID_HANDLE);

	char buf[16];
	DWORD n = sizeof(buf);
	CHECK_EQ(RegOpenCurrentUser(KEY_READ, &cu), 0);
	CHECK_EQ(RegGetValueA(cu, "Software\\GV", "sz", RRF_RT_REG_SZ, NULL, buf,
			      &n), 0);
	CHECK_STR(buf, "abc");
	RegCloseKey(cu);
	CHECK_EQ(RegDisablePredefinedCache(), 0);
}

#define D32 "Software\\Microsoft\\Windows NT\\CurrentVersion\\Drivers32"

static void test_drivers32(void)
{
	FN(LONG, RegOpenKeyExA, HKEY, LPCSTR, DWORD, REGSAM, HKEY *);
	FN(LONG, RegQueryValueExA, HKEY, LPCSTR, LPDWORD, LPDWORD, LPBYTE, LPDWORD);
	FN(LONG, RegSetValueExA, HKEY, LPCSTR, DWORD, DWORD, const BYTE *, DWORD);
	FN(LONG, RegDeleteValueA, HKEY, LPCSTR);
	FN(LONG, RegCloseKey, HKEY);
	char ini[400], buf[300];
	DWORD cb;
	HKEY k;
	const FOURCC vidc = mmioFOURCC('v', 'i', 'd', 'c');

	snprintf(ini, sizeof(ini), "%s/vfw.ini", dir);
	static const char text[] = "[drivers32]\n"
		"vidc.ABCD=abcd.dll\n"
		"vidc.WXYZ=/opt/codecs/wxyz.dll\n"
		"msacm.tsta=/opt/codecs/tsta.acm\n";
	write_file(ini, text, sizeof(text) - 1);
	setenv("VFW_INI", ini, 1);
	vfw_registry_reload();

	/* vfw.ini's codecs, by the names msvfw32 on NT looks for. */
	CHECK_EQ(RegOpenKeyExA(HKEY_LOCAL_MACHINE, D32, 0, KEY_READ, &k), 0);
	cb = sizeof(buf);
	CHECK_EQ(RegQueryValueExA(k, "vidc.ABCD", NULL, NULL, (BYTE *)buf, &cb),
		 0);
	CHECK_STR(buf, "abcd.dll");
	cb = sizeof(buf);
	CHECK_EQ(RegQueryValueExA(k, "VIDC.wxyz", NULL, NULL, (BYTE *)buf, &cb),
		 0);
	CHECK_STR(buf, "Z:\\opt\\codecs\\wxyz.dll");

	/* ACM drivers too. */
	cb = sizeof(buf);
	CHECK_EQ(RegQueryValueExA(k, "msacm.tsta", NULL, NULL, (BYTE *)buf, &cb),
		 0);
	CHECK_STR(buf, "Z:\\opt\\codecs\\tsta.acm");

	/* Registrations at run time show up... */
	CHECK_EQ(vfw_register(vidc, mmioFOURCC('L', 'I', 'V', 'E'),
			      "/x/live.dll"), 0);
	cb = sizeof(buf);
	CHECK_EQ(RegQueryValueExA(k, "vidc.LIVE", NULL, NULL, (BYTE *)buf, &cb),
		 0);
	CHECK_STR(buf, "Z:\\x\\live.dll");

	/* ...and values set there register, or remove, codecs. */
	CHECK_EQ(RegSetValueExA(k, "vidc.NEW1", 0, REG_SZ,
				(const BYTE *)"Z:\\x\\new1.dll", 14), 0);
	const struct vfw_codec *c = vfw_find(vidc, mmioFOURCC('N', 'E', 'W', '1'));
	CHECK(c && c->path && !strcmp(c->path, "/x/new1.dll"));
	CHECK_EQ(RegDeleteValueA(k, "vidc.ABCD"), 0);
	CHECK(!vfw_find(vidc, mmioFOURCC('A', 'B', 'C', 'D')));
	cb = sizeof(buf);
	CHECK_EQ(RegQueryValueExA(k, "vidc.ABCD", NULL, NULL, (BYTE *)buf, &cb),
		 ERROR_FILE_NOT_FOUND);
	CHECK_EQ(RegSetValueExA(k, "msacm.new2", 0, REG_SZ,
				(const BYTE *)"Z:\\x\\new2.acm", 14), 0);
	char *alias = NULL, *apath = NULL;
	CHECK_EQ(vfw_acm_get(1, &alias, &apath), 0);
	CHECK(alias && !strcmp(alias, "msacm.new2"));
	CHECK(apath && !strcmp(apath, "/x/new2.acm"));
	free(alias);
	free(apath);
	CHECK_EQ(RegDeleteValueA(k, "msacm.tsta"), 0);
	CHECK_EQ(vfw_acm_get(0, &alias, &apath), 0);
	CHECK(alias && !strcmp(alias, "msacm.new2"));
	free(alias);
	free(apath);
	CHECK_EQ(vfw_acm_get(1, &alias, &apath), -1);
	/* Other values stay as they are. */
	CHECK_EQ(RegSetValueExA(k, "wavemapper", 0, REG_SZ,
				(const BYTE *)"msacm32.drv", 12), 0);
	cb = sizeof(buf);
	CHECK_EQ(RegQueryValueExA(k, "wavemapper", NULL, NULL, (BYTE *)buf, &cb),
		 0);
	RegCloseKey(k);

	/* Never saved: vfw.ini is where codecs live. */
	char *saved = read_text(file);
	CHECK(saved && !strstr(saved, "vidc.") && !strstr(saved, "msacm.") &&
	      !strstr(saved, "Drivers32"));
	free(saved);

	/* A Drivers32 key in a registry file registers its codecs. */
	static const char reg[] = "Windows Registry Editor Version 5.00\n\n"
		"[HKEY_LOCAL_MACHINE\\" D32 "]\n"
		"\"vidc.FILE\"=\"Z:\\\\x\\\\file.dll\"\n";
	write_file(file, reg, sizeof(reg) - 1);
	reg_reload();
	CHECK_EQ(RegOpenKeyExA(HKEY_LOCAL_MACHINE, D32, 0, KEY_READ, &k), 0);
	c = vfw_find(vidc, mmioFOURCC('F', 'I', 'L', 'E'));
	CHECK(c && c->path && !strcmp(c->path, "/x/file.dll"));
	cb = sizeof(buf);
	CHECK_EQ(RegQueryValueExA(k, "vidc.WXYZ", NULL, NULL, (BYTE *)buf, &cb),
		 0);
	RegCloseKey(k);

	unsetenv("VFW_INI");
	unlink(ini);
	vfw_registry_reload();
}

/* The CryptoAPI hash, key and parameter calls are not offered. */
static void test_crypt_unsupported(void)
{
	struct module *k = mod_load("kernel32.dll", NULL);
	DWORD (WINAPI *last)(void) = mod_get_proc(k, "GetLastError");
	FN(BOOL, CryptAcquireContextA, ULONG_PTR *, LPCSTR, LPCSTR, DWORD, DWORD);
	FN(BOOL, CryptReleaseContext, ULONG_PTR, DWORD);
	FN(BOOL, CryptCreateHash, ULONG_PTR, UINT, ULONG_PTR, DWORD, ULONG_PTR *);
	FN(BOOL, CryptDestroyHash, ULONG_PTR);
	FN(BOOL, CryptGetHashParam, ULONG_PTR, DWORD, BYTE *, DWORD *, DWORD);
	FN(BOOL, CryptSetHashParam, ULONG_PTR, DWORD, const BYTE *, DWORD);
	FN(BOOL, CryptSignHashA, ULONG_PTR, DWORD, LPCSTR, DWORD, BYTE *, DWORD *);
	FN(BOOL, CryptDecrypt, ULONG_PTR, ULONG_PTR, BOOL, DWORD, BYTE *, DWORD *);
	FN(BOOL, CryptGetProvParam, ULONG_PTR, DWORD, BYTE *, DWORD *, DWORD);
	FN(BOOL, CryptSetProvParam, ULONG_PTR, DWORD, const BYTE *, DWORD);
	ULONG_PTR prov = 0, hash = 99;
	DWORD len = 0;
	BYTE buf[8];

	CHECK(CryptAcquireContextA(&prov, NULL, NULL, 1, 0xf0000000));
	/* No algorithm can be hashed with, so no hash ever exists. */
	CHECK(!CryptCreateHash(prov, 0x8003 /* CALG_MD5 */, 0, 0, &hash));
	CHECK_EQ(last(), (DWORD)NTE_BAD_ALGID);
	CHECK_EQ(hash, 0);
	CHECK(!CryptDestroyHash(5));
	CHECK(!CryptGetHashParam(5, 2, buf, &len, 0));
	CHECK(!CryptSetHashParam(5, 2, buf, 0));
	CHECK(!CryptSignHashA(5, 1, NULL, 0, buf, &len));
	CHECK(!CryptDecrypt(5, 0, TRUE, 0, buf, &len));
	CHECK(!CryptGetProvParam(prov, 1, buf, &len, 0));
	CHECK(!CryptSetProvParam(prov, 1, buf, 0));
	CHECK_EQ(last(), ERROR_INVALID_PARAMETER);
	CHECK(CryptReleaseContext(prov, 0));
	mod_free(k);
}

int main(void)
{
	const char *tmpdir = getenv("TMPDIR");
	snprintf(dir, sizeof(dir), "%s/vfw-adv-XXXXXX",
		 tmpdir && *tmpdir ? tmpdir : "/tmp");
	if (!mkdtemp(dir)) {
		perror("mkdtemp");
		return 1;
	}
	snprintf(file, sizeof(file), "%s/test.reg", dir);
	setenv("VFW_ROOT", dir, 1);
	setenv("VFW_REGISTRY", file, 1);

	CHECK_EQ(runtime_enter(), 0);
	adv = mod_load("advapi32.dll", NULL);
	CHECK(adv != NULL);
	if (!adv)
		return test_summary("test_advapi32");

	test_seed();
	test_round_trip();
	test_enum_delete();
	test_user_name();
	test_get_value();
	test_notify();
	test_key_security();
	test_drivers32();
	test_sandbox();
	test_memory_only();
	test_fixture();
	test_crypt_unsupported();

	mod_free(adv);
	char cmd[512];
	snprintf(cmd, sizeof(cmd), "rm -rf '%s'", dir);
	if (system(cmd))
		fprintf(stderr, "could not remove %s\n", dir);
	return test_summary("test_advapi32");
}
