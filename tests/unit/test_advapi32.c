/*
 * The advapi32 registry, called through the export table the way a DLL's
 * imports reach it, and its .reg file.
 */
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#include "common/error.h"
#include "loader/modules.h"
#include "runtime/runtime.h"
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
	test_memory_only();
	test_fixture();

	mod_free(adv);
	char cmd[512];
	snprintf(cmd, sizeof(cmd), "rm -rf '%s'", dir);
	if (system(cmd))
		fprintf(stderr, "could not remove %s\n", dir);
	return test_summary("test_advapi32");
}
