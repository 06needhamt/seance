/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * Finding DirectShow filters (src/dshow/discover.c): what testfilter.ax's
 * DllRegisterServer registers, seen in a sandbox and then for real, the
 * filters read back from the registry (including a .reg file written the
 * way Windows' legacy mapper writes), and the roles given to filters.
 */
#include <limits.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "common/error.h"
#include "dshow/discover.h"
#include "loader/modules.h"
#include "runtime/lowmem.h"
#include "runtime/runtime.h"
#include "win32/codepage.h"
#include "win32/path.h"
#include "win32/reg.h"
#include "test.h"

#define CLASSES "HKEY_LOCAL_MACHINE\\Software\\Classes\\"
#define DECODER "{5E6C9B31-3A7D-4F0E-9C11-0D2B7A6E4F80}"
#define ENCODER "{5E6C9B32-3A7D-4F0E-9C11-0D2B7A6E4F80}"

static const GUID clsid_decoder = { 0x5e6c9b31, 0x3a7d, 0x4f0e,
	{ 0x9c, 0x11, 0x0d, 0x2b, 0x7a, 0x6e, 0x4f, 0x80 } };
static const GUID clsid_encoder = { 0x5e6c9b32, 0x3a7d, 0x4f0e,
	{ 0x9c, 0x11, 0x0d, 0x2b, 0x7a, 0x6e, 0x4f, 0x80 } };
static const GUID st_tst0 = FOURCC_SUBTYPE_INIT(mmioFOURCC('T', 'S', 'T', '0'));
static const GUID guid_null;

static char regfile[300];

static int guid_eq(const GUID *a, const GUID *b)
{
	return !memcmp(a, b, sizeof(GUID));
}

static int key_exists(const char *path)
{
	reg_lock();
	int r = reg_open(path) != NULL;
	reg_unlock();
	return r;
}

static int file_exists(const char *path)
{
	struct stat st;
	return stat(path, &st) == 0;
}

static char *name_of(const struct qz_filter *f)
{
	return f->name ? utf16_to_utf8_dup(f->name) : NULL;
}

/* testfilter.ax's two filters, as registered (in either order). */
static void check_filters(struct qz_filter *f, size_t n)
{
	CHECK_EQ(n, 2);
	for (size_t i = 0; i < n; i++) {
		char *name = name_of(&f[i]);
		if (guid_eq(&f[i].clsid, &clsid_decoder)) {
			CHECK_STR(name, "seance test decoder");
			CHECK_EQ(dshow_role(&f[i]), DSHOW_DECODER);
			CHECK_EQ(f[i].merit, MERIT_NORMAL);
			CHECK(f[i].npins == 2 && f[i].pins[0].ntypes == 1 &&
			      guid_eq(&f[i].pins[0].types[0].minor, &st_tst0));
		} else {
			CHECK(guid_eq(&f[i].clsid, &clsid_encoder));
			CHECK_STR(name, "seance test encoder");
			CHECK_EQ(dshow_role(&f[i]), DSHOW_ENCODER);
			CHECK_EQ(f[i].merit, MERIT_DO_NOT_USE);
			CHECK_EQ(f[i].npins, 2);
			if (f[i].npins == 2) {
				char *pin = utf16_to_utf8_dup(f[i].pins[1].name);
				CHECK_STR(pin, "Output");
				free(pin);
				CHECK(f[i].pins[1].flags & REG_PINFLAG_B_OUTPUT);
			}
		}
		CHECK(guid_eq(&f[i].category, &CLSID_LegacyAmFilterCategory));
		free(name);
	}
}

static void test_is_server(const char *filter)
{
	struct pe_image *img = pe_open(filter);
	CHECK(img && dshow_is_server(img));
	pe_free(img);
	img = pe_open(fixture("testcodec.dll"));
	CHECK(img && !dshow_is_server(img));
	pe_free(img);
	img = pe_open(fixture("pe_basic.dll"));
	CHECK(img && !dshow_is_server(img));
	pe_free(img);
}

/* In a sandbox: everything is found, and nothing is left behind. */
static void test_dry_run(const char *filter)
{
	struct qz_filter *f = NULL;
	size_t n = 0;
	HRESULT hr = E_FAIL;

	CHECK_EQ(dshow_discover(filter, &f, &n, &hr), 0);
	CHECK_EQ(hr, S_OK);
	check_filters(f, n);
	qz_filters_free(f, n);

	CHECK(!key_exists(CLASSES "CLSID\\" DECODER));
	CHECK(!key_exists(CLASSES "Filter\\" ENCODER));
	CHECK(!file_exists(regfile));
	CHECK(mod_find("testfilter.ax") == NULL);

	struct dshow_registered *r = NULL;
	CHECK_EQ(dshow_registered_filters(&r), 0);
	dshow_registered_free(r, 0);
}

/* Through a link of another name, the filters are still its own. */
static void test_link(const char *filter, const char *dir)
{
	char link[300], real[PATH_MAX];
	struct qz_filter *f = NULL;
	size_t n = 0;
	HRESULT hr;

	snprintf(link, sizeof(link), "%s/renamed.ax", dir);
	CHECK(realpath(filter, real) && !symlink(real, link));
	CHECK_EQ(dshow_discover(link, &f, &n, &hr), 0);
	CHECK_EQ(hr, S_OK);
	CHECK_EQ(n, 2);
	qz_filters_free(f, n);
	unlink(link);
}

static void test_register(const char *filter)
{
	struct qz_filter *f = NULL;
	size_t n = 0;
	HRESULT hr = E_FAIL;

	CHECK_EQ(dshow_register(filter, 0, &f, &n, &hr), 0);
	CHECK_EQ(hr, S_OK);
	check_filters(f, n);
	qz_filters_free(f, n);
	CHECK(key_exists(CLASSES "CLSID\\" DECODER "\\InprocServer32"));
	CHECK(file_exists(regfile));

	/* Read back from the registry, with the server's path. */
	struct dshow_registered *r = NULL;
	size_t nr = dshow_registered_filters(&r);
	CHECK_EQ(nr, 2);
	char real[PATH_MAX];
	CHECK(realpath(filter, real) != NULL);
	for (size_t i = 0; i < nr; i++) {
		CHECK(r[i].path != NULL);
		char *unix_path = r[i].path ? path_to_unix(r[i].path) : NULL;
		CHECK_STR(unix_path, real);
		free(unix_path);
	}
	struct qz_filter *copy = calloc(nr, sizeof(*copy));
	for (size_t i = 0; copy && i < nr; i++) {
		copy[i] = r[i].f;
		memset(&r[i].f, 0, sizeof(r[i].f));
	}
	if (copy)
		check_filters(copy, nr);
	qz_filters_free(copy, nr);
	dshow_registered_free(r, nr);

	/* COM finds the classes through their InprocServer32 paths. */
	struct module *ole = mod_load("ole32.dll", NULL);
	HRESULT (WINAPI *CoInitialize)(LPVOID) =
		(HRESULT (WINAPI *)(LPVOID))mod_get_proc(ole, "CoInitialize");
	void (WINAPI *CoUninitialize)(void) =
		(void (WINAPI *)(void))mod_get_proc(ole, "CoUninitialize");
	HRESULT (WINAPI *CoGetClassObject)(REFCLSID, DWORD, LPVOID, REFIID,
					   LPVOID *) =
		(HRESULT (WINAPI *)(REFCLSID, DWORD, LPVOID, REFIID, LPVOID *))
		mod_get_proc(ole, "CoGetClassObject");
	CHECK_EQ(CoInitialize(NULL), S_OK);
	IClassFactory *cf = NULL;
	CHECK_EQ(CoGetClassObject(&clsid_encoder, CLSCTX_INPROC_SERVER, NULL,
				  &IID_IClassFactory, (void **)&cf), S_OK);
	if (cf) {
		IUnknown *u = (IUnknown *)cf;
		CHECK_EQ(cf->lpVtbl->CreateInstance(cf, NULL, &IID_IUnknown,
						    (void **)&u), E_NOTIMPL);
		CHECK(u == NULL);
		cf->lpVtbl->Release(cf);
	}
	CoUninitialize();
	mod_free(ole);

	/* Unregistering takes it all away again. */
	CHECK_EQ(dshow_register(filter, 1, &f, &n, &hr), 0);
	CHECK_EQ(hr, S_OK);
	CHECK_EQ(n, 0);
	CHECK(!key_exists(CLASSES "CLSID\\" DECODER));
	CHECK(!key_exists(CLASSES "CLSID\\" ENCODER));
	CHECK(!key_exists(CLASSES "Filter\\" ENCODER));
	CHECK_EQ(dshow_registered_filters(&r), 0);
	dshow_registered_free(r, 0);
}

static void test_not_servers(void)
{
	struct qz_filter *f = (struct qz_filter *)1;
	size_t n = 1;
	HRESULT hr = S_OK;

	CHECK_EQ(dshow_discover(fixture("testcodec.dll"), &f, &n, &hr), -1);
	CHECK(strstr(vfw_error(), "DllRegisterServer") != NULL);
	CHECK(f == NULL && n == 0 && hr == E_FAIL);
	CHECK_EQ(dshow_register("/nonexistent/x.ax", 0, &f, &n, &hr), -1);
}

/* A filter registered the legacy way, as a .reg file exported on Windows. */
static void test_legacy_reg_file(void)
{
	static const char text[] =
		"Windows Registry Editor Version 5.00\n"
		"\n"
		"[HKEY_CLASSES_ROOT\\Filter\\{11111111-2222-3333-4444-555555555555}]\n"
		"@=\"Imported Decoder\"\n"
		"\n"
		"[HKEY_CLASSES_ROOT\\CLSID\\{11111111-2222-3333-4444-555555555555}]\n"
		"\"Merit\"=dword:00800000\n"
		"\n"
		"[HKEY_CLASSES_ROOT\\CLSID\\{11111111-2222-3333-4444-555555555555}"
		"\\InprocServer32]\n"
		"@=\"C:\\\\Windows\\\\System32\\\\imported.ax\"\n"
		"\n"
		"[HKEY_CLASSES_ROOT\\CLSID\\{11111111-2222-3333-4444-555555555555}"
		"\\Pins\\In\\Types\\{73646976-0000-0010-8000-00AA00389B71}"
		"\\{31435657-0000-0010-8000-00AA00389B71}]\n"
		"\n"
		"[HKEY_CLASSES_ROOT\\CLSID\\{11111111-2222-3333-4444-555555555555}"
		"\\Pins\\Out]\n"
		"\"Direction\"=dword:00000001\n"
		"\n"
		"[HKEY_CLASSES_ROOT\\CLSID\\{11111111-2222-3333-4444-555555555555}"
		"\\Pins\\Out\\Types\\{73646976-0000-0010-8000-00AA00389B71}"
		"\\{32595559-0000-0010-8000-00AA00389B71}]\n";

	reg_lock();
	CHECK_EQ(reg_import(text, sizeof(text) - 1), 0);
	reg_unlock();

	struct dshow_registered *r = NULL;
	size_t n = dshow_registered_filters(&r);
	CHECK_EQ(n, 1);
	if (n == 1) {
		char *name = name_of(&r[0].f);
		CHECK_STR(name, "Imported Decoder");
		free(name);
		CHECK_STR(r[0].path, "C:\\Windows\\System32\\imported.ax");
		CHECK_EQ(r[0].f.merit, MERIT_PREFERRED);
		CHECK_EQ(r[0].f.npins, 2);
		CHECK_EQ(dshow_role(&r[0].f), DSHOW_DECODER);
	}
	dshow_registered_free(r, n);
}

static void test_roles(void)
{
	struct qz_type compressed = { MEDIATYPE_Video, st_tst0 };
	struct qz_type raw = { MEDIATYPE_Video, MEDIASUBTYPE_RGB32 };
	struct qz_type any = { MEDIATYPE_Video, { 0 } };
	struct qz_pin pins[2] = {
		{ .ntypes = 1, .types = &compressed },
		{ .flags = REG_PINFLAG_B_OUTPUT, .ntypes = 1, .types = &any },
	};
	struct qz_filter f = { .npins = 2, .pins = pins };

	CHECK_EQ(dshow_role(&f), DSHOW_DECODER);
	pins[0].types = &raw;
	pins[1].types = &compressed;
	CHECK_EQ(dshow_role(&f), DSHOW_ENCODER);
	pins[1].types = &raw;
	CHECK_EQ(dshow_role(&f), DSHOW_OTHER);
	f.category = CLSID_VideoCompressorCategory;
	CHECK_EQ(dshow_role(&f), DSHOW_ENCODER);
	f.npins = 0;
	f.category = guid_null;
	CHECK_EQ(dshow_role(&f), DSHOW_OTHER);
	CHECK_STR(dshow_role_name(DSHOW_DECODER), "video decoder");

	char buf[DSHOW_GUID_NAME_SIZE];
	CHECK_STR(dshow_guid_name(&MEDIATYPE_Video, buf), "MEDIATYPE_Video");
	CHECK_STR(dshow_guid_name(&st_tst0, buf), "'TST0'");
	CHECK_STR(dshow_guid_name(&guid_null, buf), "any");
	CHECK_STR(dshow_guid_name(&clsid_decoder, buf), DECODER);
}

int main(void)
{
	const char *filter = fixture("testfilter.ax");
	char dir[] = "/tmp/vfw-discover-XXXXXX";
	if (!mkdtemp(dir)) {
		perror("mkdtemp");
		return 1;
	}
	snprintf(regfile, sizeof(regfile), "%s/registry.reg", dir);
	setenv("VFW_ROOT", dir, 1);
	setenv("VFW_REGISTRY", regfile, 1);
	CHECK_EQ(lowmem_reserve(), 0);
	CHECK_EQ(runtime_enter(), 0);
	reg_reload();

	test_roles();
	test_is_server(filter);
	test_dry_run(filter);
	test_link(filter, dir);
	test_register(filter);
	test_not_servers();
	test_legacy_reg_file();

	char cmd[128];
	snprintf(cmd, sizeof(cmd), "rm -rf '%s'", dir);
	if (system(cmd))
		fprintf(stderr, "could not remove %s\n", dir);
	return test_summary("test_discover");
}
