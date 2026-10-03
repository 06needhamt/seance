/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * The built-in quartz.dll, through COM as a filter's DllRegisterServer
 * reaches it: both filter mappers writing (and removing) what Windows
 * writes, FilterData read back, capture mode inside a registry sandbox,
 * the empty device enumerator and AMGetErrorText.
 */
#include <stdlib.h>
#include <string.h>

#include "loader/modules.h"
#include "runtime/lowmem.h"
#include "runtime/runtime.h"
#include "win32/codepage.h"
#include "win32/internal.h"
#include "win32/quartz.h"
#include "win32/reg.h"
#include "test.h"

static struct module *ole, *qz;

static void *get_from(struct module *m, const char *name)
{
	void *p = mod_get_proc(m, name);
	if (!p) {
		fprintf(stderr, "%s has no %s\n", m->name, name);
		exit(1);
	}
	return p;
}

#define FN(m, ret, name, ...) \
	ret (WINAPI *name)(__VA_ARGS__) = \
		(ret (WINAPI *)(__VA_ARGS__))get_from(m, #name)

#define CLASSES "HKEY_LOCAL_MACHINE\\Software\\Classes\\"
#define LEGACY "{083863F1-70DE-11D0-BD40-00A0C911CE86}"

/* A made-up filter, pin category and medium. */
static const CLSID clsid_filter = { 0x7a3c1e01, 0x4d2b, 0x4c11,
	{ 0x8e, 0x01, 0x12, 0x34, 0x56, 0x78, 0x9a, 0xbc } };
#define FILTER "{7A3C1E01-4D2B-4C11-8E01-123456789ABC}"
static const CLSID clsid_category = { 0x7a3c1e02, 0x4d2b, 0x4c11,
	{ 0x8e, 0x01, 0x12, 0x34, 0x56, 0x78, 0x9a, 0xbc } };
#define CATEGORY "{7A3C1E02-4D2B-4C11-8E01-123456789ABC}"
static const GUID guid_pin_category = { 0x7a3c1e03, 0x4d2b, 0x4c11,
	{ 0x8e, 0x01, 0x12, 0x34, 0x56, 0x78, 0x9a, 0xbc } };
static const GUID guid_null;

static int guid_eq(const GUID *a, const GUID *b)
{
	return !memcmp(a, b, sizeof(GUID));
}

static WCHAR *wide(const char *s)
{
	return utf8_to_utf16_dup(s);
}

static int key_exists(const char *path)
{
	reg_lock();
	int r = reg_open(path) != NULL;
	reg_unlock();
	return r;
}

/* A string value as UTF-8 (malloc()ed), or NULL. */
static char *sz_value(const char *path, const char *name)
{
	char *out = NULL;

	reg_lock();
	struct reg_key *k = reg_open(path);
	struct reg_value *v = k ? reg_get(k, name) : NULL;
	if (v && v->type == REG_SZ)
		out = utf16_to_utf8_dup((const WCHAR *)v->data);
	reg_unlock();
	return out;
}

static int dword_value(const char *path, const char *name, DWORD *out)
{
	int ok = 0;

	reg_lock();
	struct reg_key *k = reg_open(path);
	struct reg_value *v = k ? reg_get(k, name) : NULL;
	if (v && v->type == REG_DWORD && v->size == sizeof(DWORD)) {
		memcpy(out, v->data, sizeof(DWORD));
		ok = 1;
	}
	reg_unlock();
	return ok;
}

/* A binary value, copied (malloc()ed), or NULL. */
static BYTE *binary_value(const char *path, const char *name, DWORD *size)
{
	BYTE *out = NULL;

	reg_lock();
	struct reg_key *k = reg_open(path);
	struct reg_value *v = k ? reg_get(k, name) : NULL;
	if (v && v->type == REG_BINARY && (out = malloc(v->size + 1))) {
		memcpy(out, v->data, v->size);
		*size = v->size;
	}
	reg_unlock();
	return out;
}

static IFilterMapper2 *new_mapper(const CLSID *clsid)
{
	FN(ole, HRESULT, CoCreateInstance, REFCLSID, LPUNKNOWN, DWORD, REFIID,
	   LPVOID *);
	IFilterMapper2 *fm = NULL;

	/* No registry entry: ole32 knows quartz serves the class. */
	CHECK_EQ(CoCreateInstance(clsid, NULL, CLSCTX_INPROC_SERVER,
				  &IID_IFilterMapper2, (void **)&fm), S_OK);
	return fm;
}

/* An input pin with two types and a medium, and an output pin. */
static const REGPINTYPES in_types[] = {
	{ &MEDIATYPE_Video, &MEDIASUBTYPE_YUY2 },
	{ &MEDIATYPE_Video, NULL },
};
static const REGPINTYPES out_types[] = {
	{ &MEDIATYPE_Video, &MEDIASUBTYPE_RGB24 },
};
static const REGPINMEDIUM medium = {
	{ 0x11111111, 0x2222, 0x3333, { 4, 4, 5, 5, 6, 6, 7, 7 } }, 8, 9,
};
static const REGFILTERPINS2 pins2[] = {
	{ 0, 1, 2, in_types, 1, &medium, NULL },
	{ REG_PINFLAG_B_OUTPUT, 0, 1, out_types, 0, NULL, &guid_pin_category },
};

static void check_parsed(const struct qz_filter *f)
{
	CHECK_EQ(f->merit, MERIT_NORMAL);
	CHECK_EQ(f->npins, 2);
	if (f->npins != 2)
		return;
	const struct qz_pin *in = &f->pins[0], *out = &f->pins[1];
	CHECK_EQ(in->flags, 0);
	CHECK_EQ(in->instances, 1);
	CHECK(!in->has_category);
	CHECK_EQ(in->ntypes, 2);
	if (in->ntypes == 2) {
		CHECK(guid_eq(&in->types[0].major, &MEDIATYPE_Video));
		CHECK(guid_eq(&in->types[0].minor, &MEDIASUBTYPE_YUY2));
		CHECK(guid_eq(&in->types[1].minor, &guid_null));
	}
	CHECK_EQ(in->nmediums, 1);
	if (in->nmediums == 1)
		CHECK(!memcmp(&in->mediums[0], &medium, sizeof(medium)));
	CHECK_EQ(out->flags, REG_PINFLAG_B_OUTPUT);
	CHECK(out->has_category &&
	      guid_eq(&out->category, &guid_pin_category));
	CHECK_EQ(out->ntypes, 1);
	if (out->ntypes == 1)
		CHECK(guid_eq(&out->types[0].minor, &MEDIASUBTYPE_RGB24));
	CHECK_EQ(out->nmediums, 0);
}

static void test_register2(void)
{
	IFilterMapper2 *fm = new_mapper(&CLSID_FilterMapper2);
	if (!fm)
		return;
	REGFILTER2 rf2 = { .dwVersion = 2, .dwMerit = MERIT_NORMAL };
	rf2.cPins2 = 2;
	rf2.rgPins2 = pins2;
	WCHAR *name = wide("Test Decoder");
	IMoniker *mon = (IMoniker *)&rf2;

	CHECK_EQ(fm->lpVtbl->RegisterFilter(fm, &clsid_filter, name, &mon,
					    NULL, NULL, &rf2), S_OK);
	CHECK(mon == NULL);

	const char *path = CLASSES "CLSID\\" LEGACY "\\Instance\\" FILTER;
	char *s = sz_value(path, "FriendlyName");
	CHECK_STR(s, "Test Decoder");
	free(s);
	s = sz_value(path, "CLSID");
	CHECK_STR(s, FILTER);
	free(s);

	DWORD size = 0;
	BYTE *data = binary_value(path, "FilterData", &size);
	CHECK(data != NULL);
	if (data) {
		/* Windows' layout: the header, then "0pi3" and "0ty3". */
		DWORD h[4];
		memcpy(h, data, sizeof(h));
		CHECK_EQ(h[0], 2);
		CHECK_EQ(h[1], MERIT_NORMAL);
		CHECK_EQ(h[2], 2);
		CHECK_EQ(h[3], 0);
		CHECK(!memcmp(data + 16, "0pi3", 4));
		CHECK(!memcmp(data + 40, "0ty3", 4));
		CHECK(!memcmp(data + 56, "1ty3", 4));

		struct qz_filter f;
		CHECK_EQ(qz_filterdata_parse(data, size, &f), 0);
		check_parsed(&f);
		qz_filter_clear(&f);

		/* Every truncation is rejected cleanly. */
		for (DWORD n = 0; n < size; n++)
			CHECK_EQ(qz_filterdata_parse(data, n, &f), -1);
		/* So is an offset past the end. */
		data[40 + 8] = 0xff;
		CHECK_EQ(qz_filterdata_parse(data, size, &f), -1);
		free(data);
	}

	/* A category and instance name of the filter's choosing. */
	WCHAR *inst = wide("Second");
	CHECK_EQ(fm->lpVtbl->RegisterFilter(fm, &clsid_filter, name, NULL,
					    &clsid_category, inst, &rf2), S_OK);
	CHECK(key_exists(CLASSES "CLSID\\" CATEGORY "\\Instance\\Second"));
	CHECK_EQ(fm->lpVtbl->UnregisterFilter(fm, &clsid_category, inst,
					      &clsid_filter), S_OK);
	CHECK(!key_exists(CLASSES "CLSID\\" CATEGORY "\\Instance\\Second"));
	free(inst);

	/* Version 1 pins become version 2 ones. */
	REGFILTERPINS pins1[] = {
		{ NULL, FALSE, TRUE, TRUE, FALSE, NULL, NULL, 1, out_types },
	};
	REGFILTER2 rf1 = { .dwVersion = 1, .dwMerit = MERIT_UNLIKELY };
	rf1.cPins = 1;
	rf1.rgPins = pins1;
	CHECK_EQ(fm->lpVtbl->RegisterFilter(fm, &clsid_filter, name, NULL,
					    &clsid_category, NULL, &rf1), S_OK);
	const char *path1 = CLASSES "CLSID\\" CATEGORY "\\Instance\\" FILTER;
	data = binary_value(path1, "FilterData", &size);
	if (data) {
		struct qz_filter f;
		CHECK_EQ(qz_filterdata_parse(data, size, &f), 0);
		CHECK_EQ(f.merit, MERIT_UNLIKELY);
		CHECK(f.npins == 1 && f.pins[0].flags ==
		      (REG_PINFLAG_B_OUTPUT | REG_PINFLAG_B_ZERO));
		qz_filter_clear(&f);
		free(data);
	} else {
		CHECK(data != NULL);
	}
	CHECK_EQ(fm->lpVtbl->UnregisterFilter(fm, &clsid_category, NULL,
					      &clsid_filter), S_OK);

	rf2.dwVersion = 3;
	CHECK_EQ(fm->lpVtbl->RegisterFilter(fm, &clsid_filter, name, NULL,
					    NULL, NULL, &rf2), E_INVALIDARG);
	CHECK_EQ(fm->lpVtbl->RegisterFilter(fm, &clsid_filter, NULL, NULL,
					    NULL, NULL, &rf2), E_POINTER);

	CHECK_EQ(fm->lpVtbl->UnregisterFilter(fm, NULL, NULL, &clsid_filter),
		 S_OK);
	CHECK(!key_exists(path));
	CHECK(FAILED(fm->lpVtbl->UnregisterFilter(fm, NULL, NULL,
						  &clsid_filter)));

	/* Categories. */
	WCHAR *desc = wide("Test Category");
	CHECK_EQ(fm->lpVtbl->CreateCategory(fm, &clsid_category, MERIT_NORMAL,
					    desc), S_OK);
	const char *cat = CLASSES "CLSID\\{DA4E3DA0-D07D-11D0-BD50-00A0C911CE86}"
			  "\\Instance\\" CATEGORY;
	s = sz_value(cat, "FriendlyName");
	CHECK_STR(s, "Test Category");
	free(s);
	DWORD merit = 0;
	CHECK(dword_value(cat, "Merit", &merit) && merit == MERIT_NORMAL);
	free(desc);

	IEnumMoniker *e = (IEnumMoniker *)fm;
	CHECK_EQ(fm->lpVtbl->EnumMatchingFilters(fm, &e, 0, FALSE, 0, FALSE,
						 0, NULL, NULL, NULL, FALSE,
						 FALSE, 0, NULL, NULL, NULL),
		 E_NOTIMPL);
	CHECK(e == NULL);
	free(name);
	CHECK_EQ(fm->lpVtbl->Release(fm), 0);
}

static void test_register1(void)
{
	/* The legacy mapper, from either class, and from the new one. */
	IFilterMapper2 *fm2 = new_mapper(&CLSID_FilterMapper);
	IFilterMapper *fm = NULL;
	if (!fm2)
		return;
	CHECK_EQ(fm2->lpVtbl->QueryInterface(fm2, &IID_IFilterMapper,
					     (void **)&fm), S_OK);
	fm2->lpVtbl->Release(fm2);
	if (!fm)
		return;

	WCHAR *name = wide("Old Filter"), *pin = wide("Input");
	CHECK_EQ(fm->lpVtbl->RegisterFilter(fm, clsid_filter, name,
					    MERIT_DO_NOT_USE), S_OK);
	char *s = sz_value(CLASSES "Filter\\" FILTER, "");
	CHECK_STR(s, "Old Filter");
	free(s);
	DWORD v = 0;
	CHECK(dword_value(CLASSES "CLSID\\" FILTER, "Merit", &v) &&
	      v == MERIT_DO_NOT_USE);

	CHECK_EQ(fm->lpVtbl->RegisterPin(fm, clsid_filter, pin, FALSE, FALSE,
					 TRUE, FALSE, guid_null, NULL), S_OK);
	const char *pins = CLASSES "CLSID\\" FILTER "\\Pins\\Input";
	CHECK(dword_value(pins, "AllowedZero", &v) && v == 1);
	CHECK(dword_value(pins, "Direction", &v) && v == 0);
	CHECK_EQ(fm->lpVtbl->RegisterPinType(fm, clsid_filter, pin,
					     MEDIATYPE_Video,
					     MEDIASUBTYPE_YUY2), S_OK);
	CHECK(key_exists(CLASSES "CLSID\\" FILTER "\\Pins\\Input\\Types\\"
			 "{73646976-0000-0010-8000-00AA00389B71}\\"
			 "{32595559-0000-0010-8000-00AA00389B71}"));

	CHECK_EQ(fm->lpVtbl->UnregisterPin(fm, clsid_filter, pin), S_OK);
	CHECK(!key_exists(pins));
	CHECK_EQ(fm->lpVtbl->UnregisterPin(fm, clsid_filter, NULL),
		 E_INVALIDARG);
	CHECK_EQ(fm->lpVtbl->UnregisterFilter(fm, clsid_filter), S_OK);
	CHECK(!key_exists(CLASSES "Filter\\" FILTER));
	CHECK(!dword_value(CLASSES "CLSID\\" FILTER, "Merit", &v));
	CHECK_EQ(fm->lpVtbl->RegisterFilterInstance(fm, clsid_filter, name,
						    NULL), E_NOTIMPL);
	free(name);
	free(pin);
	CHECK_EQ(fm->lpVtbl->Release(fm), 0);
}

static void test_capture(void)
{
	IFilterMapper2 *fm2 = new_mapper(&CLSID_FilterMapper2);
	IFilterMapper *fm = NULL;
	if (!fm2)
		return;
	fm2->lpVtbl->QueryInterface(fm2, &IID_IFilterMapper, (void **)&fm);

	/* Something already there stays, whatever the sandbox does to it. */
	LONG err = 0;
	reg_lock();
	struct reg_key *k = reg_create(CLASSES "Kept", 0, NULL, &err);
	CHECK(k != NULL);
	reg_unlock();

	CHECK_EQ(reg_sandbox_begin(), ERROR_SUCCESS);
	CHECK_EQ(reg_sandbox_begin(), ERROR_BUSY);
	quartz_capture_begin();

	REGFILTER2 rf2 = { .dwVersion = 2, .dwMerit = MERIT_NORMAL };
	rf2.cPins2 = 2;
	rf2.rgPins2 = pins2;
	WCHAR *name = wide("Captured"), *old = wide("Legacy"),
	      *pin = wide("Out");
	CHECK_EQ(fm2->lpVtbl->RegisterFilter(fm2, &clsid_filter, name, NULL,
					     &clsid_category, NULL, &rf2),
		 S_OK);
	CHECK(key_exists(CLASSES "CLSID\\" CATEGORY "\\Instance\\" FILTER));
	CHECK(key_exists(CLASSES "Kept"));
	reg_lock();
	reg_delete(reg_open(CLASSES "Kept"));
	reg_unlock();

	if (fm) {
		CHECK_EQ(fm->lpVtbl->RegisterFilter(fm, clsid_category, old,
						    MERIT_PREFERRED), S_OK);
		CHECK_EQ(fm->lpVtbl->RegisterPin(fm, clsid_category, pin, TRUE,
						 TRUE, FALSE, TRUE, guid_null,
						 NULL), S_OK);
		CHECK_EQ(fm->lpVtbl->RegisterPinType(fm, clsid_category, pin,
						     MEDIATYPE_Video,
						     MEDIASUBTYPE_RGB32), S_OK);
	}

	struct qz_filter *f = NULL;
	size_t n = quartz_capture_end(&f);
	reg_sandbox_end();

	/* The sandbox is gone, and the real registry is as it was. */
	CHECK(!key_exists(CLASSES "CLSID\\" CATEGORY "\\Instance\\" FILTER));
	CHECK(key_exists(CLASSES "Kept"));

	CHECK_EQ(n, fm ? 2 : 1);
	if (n >= 1) {
		CHECK(guid_eq(&f[0].clsid, &clsid_filter));
		CHECK(guid_eq(&f[0].category, &clsid_category));
		char *s = utf16_to_utf8_dup(f[0].name);
		CHECK_STR(s, "Captured");
		free(s);
		check_parsed(&f[0]);
	}
	if (n >= 2) {
		CHECK(guid_eq(&f[1].clsid, &clsid_category));
		CHECK(guid_eq(&f[1].category, &CLSID_LegacyAmFilterCategory));
		CHECK_EQ(f[1].merit, MERIT_PREFERRED);
		CHECK_EQ(f[1].npins, 1);
		if (f[1].npins == 1) {
			const struct qz_pin *p = &f[1].pins[0];
			char *s = utf16_to_utf8_dup(p->name);
			CHECK_STR(s, "Out");
			free(s);
			CHECK_EQ(p->flags, REG_PINFLAG_B_OUTPUT |
				 REG_PINFLAG_B_RENDERER | REG_PINFLAG_B_MANY);
			CHECK(p->ntypes == 1 && guid_eq(&p->types[0].minor,
							&MEDIASUBTYPE_RGB32));
		}
	}
	qz_filters_free(f, n);

	/* Nothing is recorded outside capture mode. */
	CHECK_EQ(fm2->lpVtbl->RegisterFilter(fm2, &clsid_filter, name, NULL,
					     NULL, NULL, &rf2), S_OK);
	CHECK_EQ(quartz_capture_end(&f), 0);
	CHECK(f == NULL);
	fm2->lpVtbl->UnregisterFilter(fm2, NULL, NULL, &clsid_filter);

	free(name);
	free(old);
	free(pin);
	if (fm)
		fm->lpVtbl->Release(fm);
	CHECK_EQ(fm2->lpVtbl->Release(fm2), 0);
}

static void test_devenum(void)
{
	FN(ole, HRESULT, CoCreateInstance, REFCLSID, LPUNKNOWN, DWORD, REFIID,
	   LPVOID *);
	ICreateDevEnum *de = NULL;

	CHECK_EQ(CoCreateInstance(&CLSID_SystemDeviceEnum, NULL,
				  CLSCTX_INPROC_SERVER, &IID_ICreateDevEnum,
				  (void **)&de), S_OK);
	if (!de)
		return;
	IEnumMoniker *e = (IEnumMoniker *)de;
	CHECK_EQ(de->lpVtbl->CreateClassEnumerator(de,
		 &CLSID_VideoCompressorCategory, &e, 0), S_FALSE);
	CHECK(e == NULL);
	IUnknown *u = NULL;
	CHECK_EQ(de->lpVtbl->QueryInterface(de, &IID_IFilterMapper2,
					    (void **)&u), E_NOINTERFACE);
	CHECK_EQ(de->lpVtbl->Release(de), 0);
}

static void test_class_objects(void)
{
	FN(qz, HRESULT, DllGetClassObject, REFCLSID, REFIID, LPVOID *);
	FN(qz, HRESULT, DllCanUnloadNow, void);
	IClassFactory *cf = NULL;
	IUnknown *u = NULL;

	CHECK_EQ(DllCanUnloadNow(), S_OK);
	CHECK_EQ(DllGetClassObject(&clsid_filter, &IID_IClassFactory,
				   (void **)&cf), CLASS_E_CLASSNOTAVAILABLE);
	CHECK_EQ(DllGetClassObject(&CLSID_FilterMapper2, &IID_IClassFactory,
				   (void **)&cf), S_OK);
	if (!cf)
		return;
	CHECK_EQ(cf->lpVtbl->CreateInstance(cf, (IUnknown *)cf,
					    &IID_IUnknown, (void **)&u),
		 CLASS_E_NOAGGREGATION);
	CHECK_EQ(cf->lpVtbl->CreateInstance(cf, NULL, &IID_IUnknown,
					    (void **)&u), S_OK);
	CHECK_EQ(DllCanUnloadNow(), S_FALSE);
	if (u)
		u->lpVtbl->Release(u);
	CHECK_EQ(DllCanUnloadNow(), S_OK);
	cf->lpVtbl->LockServer(cf, TRUE);
	CHECK_EQ(DllCanUnloadNow(), S_FALSE);
	cf->lpVtbl->LockServer(cf, FALSE);
	CHECK_EQ(DllCanUnloadNow(), S_OK);
	cf->lpVtbl->Release(cf);
}

static void test_error_text(void)
{
	FN(qz, DWORD, AMGetErrorTextA, HRESULT, LPSTR, DWORD);
	FN(qz, DWORD, AMGetErrorTextW, HRESULT, LPWSTR, DWORD);
	char buf[160];
	WCHAR wbuf[160];

	DWORD n = AMGetErrorTextA(VFW_E_NOT_CONNECTED, buf, sizeof(buf));
	CHECK_EQ(n, strlen(buf));
	CHECK(strstr(buf, "not connected") && strstr(buf, "VFW_E_NOT_CONNECTED"));
	CHECK_EQ(AMGetErrorTextA(VFW_E_NOT_CONNECTED, buf, n), 0);
	CHECK_EQ(AMGetErrorTextA(VFW_E_NOT_CONNECTED, buf, n + 1), n);
	CHECK_EQ(AMGetErrorTextA(VFW_E_NOT_CONNECTED, NULL, 100), 0);

	CHECK(AMGetErrorTextA(VFW_E_DVD_INVALIDDOMAIN, buf, sizeof(buf)) > 0);
	CHECK_STR(buf, "VFW_E_DVD_INVALIDDOMAIN");
	CHECK(AMGetErrorTextA((HRESULT)0x80001234, buf, sizeof(buf)) > 0);
	CHECK_STR(buf, "Error 0x80001234.");

	n = AMGetErrorTextW(VFW_S_STATE_INTERMEDIATE, wbuf, 160);
	CHECK(n > 0 && wstrlen(wbuf) == n);
	char *s = utf16_to_utf8_dup(wbuf);
	CHECK(s && strstr(s, "VFW_S_STATE_INTERMEDIATE"));
	free(s);
	CHECK_EQ(AMGetErrorTextW(VFW_S_STATE_INTERMEDIATE, wbuf, n), 0);
}

int main(void)
{
	setenv("VFW_REGISTRY", "", 1);
	CHECK_EQ(lowmem_reserve(), 0);
	CHECK_EQ(runtime_enter(), 0);
	reg_reload();
	ole = mod_load("ole32.dll", NULL);
	qz = mod_load("quartz.dll", NULL);
	CHECK(ole && qz && qz->builtin);
	if (!ole || !qz)
		return test_summary("test_quartz");

	FN(ole, HRESULT, CoInitialize, LPVOID);
	FN(ole, void, CoUninitialize, void);
	CHECK_EQ(CoInitialize(NULL), S_OK);

	test_register2();
	test_register1();
	test_capture();
	test_devenum();
	test_class_objects();
	test_error_text();

	CoUninitialize();
	mod_free(qz);
	mod_free(ole);
	return test_summary("test_quartz");
}
