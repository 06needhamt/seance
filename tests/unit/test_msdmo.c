/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * The built-in msdmo.dll, as a DMO's DllRegisterServer and a DMO host reach
 * it: DMORegister writing (and DMOUnregister removing) what Windows writes,
 * DMOGetTypes, DMOGetName and DMOEnum reading it back, capture mode inside
 * a registry sandbox, and the Mo*MediaType helpers.
 */
#include <stdlib.h>
#include <string.h>

#include "loader/modules.h"
#include "runtime/lowmem.h"
#include "runtime/runtime.h"
#include "win32/codepage.h"
#include "win32/internal.h"
#include "win32/msdmo.h"
#include "win32/ole.h"
#include "win32/reg.h"
#include "test.h"

static struct module *dmo;

static void *get_from(struct module *m, const char *name)
{
	void *p = mod_get_proc(m, name);
	if (!p) {
		fprintf(stderr, "%s has no %s\n", m->name, name);
		exit(1);
	}
	return p;
}

#define FN(ret, name, ...) \
	ret (WINAPI *name)(__VA_ARGS__) = \
		(ret (WINAPI *)(__VA_ARGS__))get_from(dmo, #name)

#define OBJECTS "HKEY_LOCAL_MACHINE\\Software\\Classes\\DirectShow\\MediaObjects\\"
#define USER_OBJECTS "HKEY_CURRENT_USER\\Software\\Classes\\DirectShow\\MediaObjects\\"
#define VIDEO_DECODER "{4A69B442-28BE-4991-969C-B500ADF5D8A8}"
#define VIDEO_ENCODER "{33D9A760-90C8-11D0-BD43-00A0C911CE86}"

/* Made-up DMOs. */
static const CLSID clsid_a = { 0x6b2d0a01, 0x1c3e, 0x4f5a,
	{ 0x9b, 0x01, 0x10, 0x20, 0x30, 0x40, 0x50, 0x60 } };
#define DMO_A "{6B2D0A01-1C3E-4F5A-9B01-102030405060}"
static const CLSID clsid_b = { 0x6b2d0a02, 0x1c3e, 0x4f5a,
	{ 0x9b, 0x01, 0x10, 0x20, 0x30, 0x40, 0x50, 0x60 } };
#define DMO_B "{6B2D0A02-1C3E-4F5A-9B01-102030405060}"
static const CLSID clsid_user = { 0x6b2d0a03, 0x1c3e, 0x4f5a,
	{ 0x9b, 0x01, 0x10, 0x20, 0x30, 0x40, 0x50, 0x60 } };
#define DMO_USER "{6B2D0A03-1C3E-4F5A-9B01-102030405060}"
static const GUID guid_null;

static const DMO_PARTIAL_MEDIATYPE tst0 = {
	FOURCC_SUBTYPE_INIT(0x73646976), FOURCC_SUBTYPE_INIT(0x30545354) };
static const DMO_PARTIAL_MEDIATYPE yuy2 = {
	FOURCC_SUBTYPE_INIT(0x73646976), FOURCC_SUBTYPE_INIT(0x32595559) };
static const DMO_PARTIAL_MEDIATYPE pcm = {
	FOURCC_SUBTYPE_INIT(0x73647561), WAVE_FORMAT_SUBTYPE_INIT(1) };

static int guid_eq(const GUID *a, const GUID *b)
{
	return !memcmp(a, b, sizeof(GUID));
}

static int types_eq(const DMO_PARTIAL_MEDIATYPE *a,
		    const DMO_PARTIAL_MEDIATYPE *b)
{
	return guid_eq(&a->type, &b->type) && guid_eq(&a->subtype, &b->subtype);
}

static int key_exists(const char *path)
{
	reg_lock();
	int r = reg_open(path) != NULL;
	reg_unlock();
	return r;
}

/* A value's type and size, or 0 and -1 if it is missing. */
static DWORD value_size(const char *path, const char *name, DWORD *type)
{
	DWORD size = (DWORD)-1;

	*type = 0;
	reg_lock();
	struct reg_key *k = reg_open(path);
	struct reg_value *v = k ? reg_get(k, name) : NULL;
	if (v) {
		*type = v->type;
		size = v->size;
	}
	reg_unlock();
	return size;
}

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

static int str_eq_w(const WCHAR *w, const char *s)
{
	char *u = w ? utf16_to_utf8_dup(w) : NULL;
	int eq = u && !strcmp(u, s);

	free(u);
	return eq;
}

/* The CLSIDs DMOEnum gives, at most max, and their names in order. */
static size_t enum_dmos(REFGUID category, DWORD flags, DWORD nin,
			const DMO_PARTIAL_MEDIATYPE *in, DWORD nout,
			const DMO_PARTIAL_MEDIATYPE *out, CLSID *clsids,
			size_t max)
{
	FN(HRESULT, DMOEnum, REFGUID, DWORD, DWORD,
	   const DMO_PARTIAL_MEDIATYPE *, DWORD, const DMO_PARTIAL_MEDIATYPE *,
	   IEnumDMO **);
	IEnumDMO *e = NULL;
	size_t n = 0;

	CHECK_EQ(DMOEnum(category, flags, nin, in, nout, out, &e), S_OK);
	if (!e)
		return 0;
	for (;;) {
		DWORD got = 0;
		CLSID c;
		WCHAR *name = NULL;
		HRESULT hr = e->lpVtbl->Next(e, 1, &c, &name, &got);
		if (hr != S_OK) {
			CHECK_EQ(hr, S_FALSE);
			CHECK_EQ(got, 0);
			break;
		}
		CHECK(name != NULL);
		ole_taskmem_free(name);
		if (n < max)
			clsids[n] = c;
		n++;
	}
	CHECK_EQ(e->lpVtbl->Release(e), 0);
	return n;
}

static int has(const CLSID *list, size_t n, const CLSID *c)
{
	for (size_t i = 0; i < n; i++)
		if (guid_eq(&list[i], c))
			return 1;
	return 0;
}

static void test_register(void)
{
	FN(HRESULT, DMORegister, LPCWSTR, REFCLSID, REFGUID, DWORD, DWORD,
	   const DMO_PARTIAL_MEDIATYPE *, DWORD, const DMO_PARTIAL_MEDIATYPE *);
	FN(HRESULT, DMOGetTypes, REFCLSID, ULONG, ULONG *,
	   DMO_PARTIAL_MEDIATYPE *, ULONG, ULONG *, DMO_PARTIAL_MEDIATYPE *);
	FN(HRESULT, DMOGetName, REFCLSID, WCHAR *);
	WCHAR *name_a = utf8_to_utf16_dup("seance test DMO decoder");
	WCHAR *name_b = utf8_to_utf16_dup(
		"a keyed encoder whose name is longer than the eighty "
		"characters DMOGetName gives back");
	DMO_PARTIAL_MEDIATYPE outs[2] = { yuy2, pcm };
	DWORD type;

	/* A decoder, TST0 in, YUY2 or PCM out. */
	CHECK_EQ(DMORegister(name_a, &clsid_a, &DMOCATEGORY_VIDEO_DECODER, 0,
			     1, &tst0, 2, outs), S_OK);
	char *s = sz_value(OBJECTS DMO_A, "");
	CHECK(s && !strcmp(s, "seance test DMO decoder"));
	free(s);
	CHECK_EQ(value_size(OBJECTS DMO_A, "InputTypes", &type), 32);
	CHECK_EQ(type, REG_BINARY);
	CHECK_EQ(value_size(OBJECTS DMO_A, "OutputTypes", &type), 64);
	CHECK(!key_exists(OBJECTS DMO_A "\\Keyed"));
	CHECK(key_exists(OBJECTS "Categories\\" VIDEO_DECODER "\\" DMO_A));

	/* A keyed encoder in two categories, with no input types. */
	CHECK_EQ(DMORegister(name_b, &clsid_b, &DMOCATEGORY_VIDEO_ENCODER,
			     DMO_REGISTERF_IS_KEYED, 0, NULL, 1, &tst0), S_OK);
	CHECK_EQ(DMORegister(name_b, &clsid_b, &DMOCATEGORY_VIDEO_DECODER,
			     DMO_REGISTERF_IS_KEYED, 0, NULL, 1, &tst0), S_OK);
	CHECK(key_exists(OBJECTS DMO_B "\\Keyed"));
	CHECK_EQ(value_size(OBJECTS DMO_B, "InputTypes", &type), 0);
	CHECK(key_exists(OBJECTS "Categories\\" VIDEO_ENCODER "\\" DMO_B));

	/* One registered under the user's classes, by hand. */
	reg_lock();
	LONG err;
	struct reg_key *k = reg_create(USER_OBJECTS DMO_USER, 0, NULL, &err);
	CHECK(k != NULL);
	if (k)
		CHECK_EQ(reg_set(k, "", REG_SZ, name_a,
				 (DWORD)((wstrlen(name_a) + 1) * 2)), 0);
	reg_unlock();

	ULONG nin = 9, nout = 9;
	DMO_PARTIAL_MEDIATYPE in[4], out[4];
	CHECK_EQ(DMOGetTypes(&clsid_a, 4, &nin, in, 1, &nout, out), S_OK);
	CHECK_EQ(nin, 1);
	CHECK_EQ(nout, 1);	/* only as many as asked for */
	CHECK(types_eq(&in[0], &tst0) && types_eq(&out[0], &yuy2));
	CHECK_EQ(DMOGetTypes(&clsid_b, 4, &nin, in, 4, &nout, out), S_OK);
	CHECK_EQ(nin, 0);
	CHECK_EQ(nout, 1);
	CHECK_EQ(DMOGetTypes(&guid_null, 4, &nin, in, 4, &nout, out),
		 HRESULT_FROM_WIN32(ERROR_FILE_NOT_FOUND));

	WCHAR name[80];
	CHECK_EQ(DMOGetName(&clsid_a, name), S_OK);
	CHECK(str_eq_w(name, "seance test DMO decoder"));
	CHECK_EQ(DMOGetName(&clsid_b, name), S_OK);
	CHECK_EQ(wstrlen(name), 79);
	CHECK_EQ(DMOGetName(&clsid_user, name), S_OK);

	/* Every DMO is read, from both roots, once per category. */
	struct dmo_reg *all = NULL;
	size_t n = msdmo_registered(&all), seen_b = 0, seen_user = 0;
	for (size_t i = 0; i < n; i++) {
		if (guid_eq(&all[i].clsid, &clsid_b)) {
			seen_b++;
			CHECK_EQ(all[i].flags, DMO_REGISTERF_IS_KEYED);
		}
		if (guid_eq(&all[i].clsid, &clsid_user)) {
			seen_user++;
			CHECK(guid_eq(&all[i].category, &guid_null));
		}
	}
	CHECK_EQ(seen_b, 2);
	CHECK_EQ(seen_user, 1);
	dmo_regs_free(all, n);

	free(name_a);
	free(name_b);
}

static void test_enum(void)
{
	FN(HRESULT, DMOEnum, REFGUID, DWORD, DWORD,
	   const DMO_PARTIAL_MEDIATYPE *, DWORD, const DMO_PARTIAL_MEDIATYPE *,
	   IEnumDMO **);
	CLSID c[8];
	size_t n;

	/* Keyed DMOs only when asked for. */
	n = enum_dmos(&DMOCATEGORY_VIDEO_DECODER, 0, 0, NULL, 0, NULL, c, 8);
	CHECK_EQ(n, 1);
	CHECK(has(c, n, &clsid_a));
	n = enum_dmos(&DMOCATEGORY_VIDEO_DECODER, DMO_ENUMF_INCLUDE_KEYED, 0,
		      NULL, 0, NULL, c, 8);
	CHECK_EQ(n, 2);

	/* Every category, each DMO once. */
	n = enum_dmos(&guid_null, DMO_ENUMF_INCLUDE_KEYED, 0, NULL, 0, NULL,
		      c, 8);
	CHECK_EQ(n, 3);
	CHECK(has(c, n, &clsid_a) && has(c, n, &clsid_b) &&
	      has(c, n, &clsid_user));

	/* By type: TST0 in finds the decoder; GUID_NULL matches anything. */
	n = enum_dmos(&guid_null, 0, 1, &tst0, 0, NULL, c, 8);
	CHECK_EQ(n, 1);
	CHECK(has(c, n, &clsid_a));
	n = enum_dmos(&guid_null, 0, 1, &yuy2, 0, NULL, c, 8);
	CHECK_EQ(n, 0);
	n = enum_dmos(&guid_null, 0, 0, NULL, 1, &pcm, c, 8);
	CHECK_EQ(n, 1);
	DMO_PARTIAL_MEDIATYPE any_video = { tst0.type, { 0 } };
	n = enum_dmos(&guid_null, DMO_ENUMF_INCLUDE_KEYED, 0, NULL, 1,
		      &any_video, c, 8);
	CHECK_EQ(n, 2);

	/* Next with several at once, Skip, Reset and Clone. */
	IEnumDMO *e = NULL, *copy = NULL;
	CHECK_EQ(DMOEnum(&guid_null, DMO_ENUMF_INCLUDE_KEYED, 0, NULL, 0, NULL,
			 &e), S_OK);
	if (!e)
		return;
	CLSID got[4];
	DWORD fetched = 0;
	CHECK_EQ(e->lpVtbl->Next(e, 4, got, NULL, &fetched), S_FALSE);
	CHECK_EQ(fetched, 3);
	CHECK_EQ(e->lpVtbl->Next(e, 2, got, NULL, NULL), E_POINTER);
	CHECK_EQ(e->lpVtbl->Reset(e), S_OK);
	CHECK_EQ(e->lpVtbl->Skip(e, 2), S_OK);
	CHECK_EQ(e->lpVtbl->Clone(e, &copy), S_OK);
	CHECK_EQ(e->lpVtbl->Skip(e, 5), S_FALSE);
	if (copy) {
		CHECK_EQ(copy->lpVtbl->Next(copy, 4, got, NULL, &fetched),
			 S_FALSE);
		CHECK_EQ(fetched, 1);
		CHECK_EQ(copy->lpVtbl->Release(copy), 0);
	}
	CHECK_EQ(e->lpVtbl->Release(e), 0);
	CHECK_EQ(DMOEnum(&guid_null, 0, 0, NULL, 0, NULL, NULL), E_POINTER);
}

static void test_unregister(void)
{
	FN(HRESULT, DMOUnregister, REFCLSID, REFGUID);

	/* Out of one category, then out of every one. */
	CHECK_EQ(DMOUnregister(&clsid_b, &DMOCATEGORY_VIDEO_DECODER), S_OK);
	CHECK(!key_exists(OBJECTS "Categories\\" VIDEO_DECODER "\\" DMO_B));
	CHECK(!key_exists(OBJECTS DMO_B));
	CHECK(key_exists(OBJECTS "Categories\\" VIDEO_ENCODER "\\" DMO_B));
	CHECK_EQ(DMOUnregister(&clsid_b, &guid_null), S_OK);
	CHECK(!key_exists(OBJECTS "Categories\\" VIDEO_ENCODER "\\" DMO_B));
	CHECK_EQ(DMOUnregister(&clsid_b, &guid_null), S_FALSE);

	CHECK_EQ(DMOUnregister(&clsid_a, &DMOCATEGORY_VIDEO_DECODER), S_OK);
	CHECK(!key_exists(OBJECTS DMO_A));
	CHECK_EQ(DMOUnregister(&clsid_user, &guid_null), S_OK);
	CHECK(!key_exists(USER_OBJECTS DMO_USER));

	struct dmo_reg *all = NULL;
	CHECK_EQ(msdmo_registered(&all), 0);
	dmo_regs_free(all, 0);
}

static void test_capture(void)
{
	FN(HRESULT, DMORegister, LPCWSTR, REFCLSID, REFGUID, DWORD, DWORD,
	   const DMO_PARTIAL_MEDIATYPE *, DWORD, const DMO_PARTIAL_MEDIATYPE *);
	FN(HRESULT, DMOUnregister, REFCLSID, REFGUID);
	WCHAR *name = utf8_to_utf16_dup("captured");

	CHECK_EQ(reg_sandbox_begin(), 0);
	msdmo_capture_begin();
	CHECK_EQ(DMORegister(name, &clsid_a, &DMOCATEGORY_VIDEO_DECODER, 0, 1,
			     &tst0, 1, &yuy2), S_OK);
	/* Registering again replaces it; unregistering drops it. */
	CHECK_EQ(DMORegister(name, &clsid_a, &DMOCATEGORY_VIDEO_DECODER, 0, 1,
			     &tst0, 1, &pcm), S_OK);
	CHECK_EQ(DMORegister(name, &clsid_b, &DMOCATEGORY_VIDEO_ENCODER, 0, 1,
			     &yuy2, 1, &tst0), S_OK);
	CHECK_EQ(DMORegister(name, &clsid_user, &DMOCATEGORY_AUDIO_DECODER, 0,
			     0, NULL, 0, NULL), S_OK);
	CHECK_EQ(DMOUnregister(&clsid_user, &guid_null), S_OK);
	CHECK(key_exists(OBJECTS DMO_A));
	struct dmo_reg *d = NULL;
	size_t n = msdmo_capture_end(&d);
	reg_sandbox_end();

	CHECK_EQ(n, 2);
	if (n == 2) {
		CHECK(guid_eq(&d[0].clsid, &clsid_a));
		CHECK(guid_eq(&d[0].category, &DMOCATEGORY_VIDEO_DECODER));
		CHECK(str_eq_w(d[0].name, "captured"));
		CHECK(d[0].nout == 1 && types_eq(&d[0].out[0], &pcm));
		CHECK(guid_eq(&d[1].clsid, &clsid_b));
		CHECK(d[1].nin == 1 && types_eq(&d[1].in[0], &yuy2));
	}
	dmo_regs_free(d, n);
	/* The sandbox kept it all out of the registry. */
	CHECK(!key_exists(OBJECTS DMO_A));

	/* Outside capture mode nothing is recorded. */
	CHECK_EQ(reg_sandbox_begin(), 0);
	CHECK_EQ(DMORegister(name, &clsid_a, &DMOCATEGORY_VIDEO_DECODER, 0, 1,
			     &tst0, 1, &yuy2), S_OK);
	reg_sandbox_end();
	msdmo_capture_begin();
	CHECK_EQ(msdmo_capture_end(&d), 0);
	free(name);
}

/* An IUnknown counting its references, for the media type helpers. */
static LONG unk_refs;
static HRESULT WINAPI unk_qi(IUnknown *u, REFIID riid, void **ppv)
{
	*ppv = NULL;
	return E_NOINTERFACE;
}
static ULONG WINAPI unk_addref(IUnknown *u) { return (ULONG)++unk_refs; }
static ULONG WINAPI unk_release(IUnknown *u) { return (ULONG)--unk_refs; }
static const IUnknownVtbl unk_vtbl = { unk_qi, unk_addref, unk_release };
static IUnknown unk = { &unk_vtbl };

static void test_media_types(void)
{
	FN(HRESULT, MoInitMediaType, DMO_MEDIA_TYPE *, DWORD);
	FN(HRESULT, MoFreeMediaType, DMO_MEDIA_TYPE *);
	FN(HRESULT, MoCopyMediaType, DMO_MEDIA_TYPE *, const DMO_MEDIA_TYPE *);
	FN(HRESULT, MoCreateMediaType, DMO_MEDIA_TYPE **, DWORD);
	FN(HRESULT, MoDeleteMediaType, DMO_MEDIA_TYPE *);
	FN(HRESULT, MoDuplicateMediaType, DMO_MEDIA_TYPE **,
	   const DMO_MEDIA_TYPE *);
	DMO_MEDIA_TYPE a, b;

	memset(&a, 0xaa, sizeof(a));
	CHECK_EQ(MoInitMediaType(&a, 40), S_OK);
	CHECK(a.pbFormat != NULL && a.cbFormat == 40 && !a.pUnk &&
	      !a.lSampleSize && guid_eq(&a.majortype, &guid_null));
	memset(a.pbFormat, 7, 40);
	a.majortype = tst0.type;
	a.pUnk = &unk;
	unk_refs = 1;

	CHECK_EQ(MoCopyMediaType(&b, &a), S_OK);
	CHECK(b.pbFormat && b.pbFormat != a.pbFormat && b.cbFormat == 40 &&
	      b.pbFormat[39] == 7 && guid_eq(&b.majortype, &tst0.type));
	CHECK_EQ(unk_refs, 2);
	CHECK_EQ(MoFreeMediaType(&b), S_OK);
	CHECK(!b.pbFormat && !b.cbFormat && !b.pUnk);
	CHECK_EQ(unk_refs, 1);

	DMO_MEDIA_TYPE *dup = NULL, *made = NULL;
	CHECK_EQ(MoDuplicateMediaType(&dup, &a), S_OK);
	CHECK(dup && dup->cbFormat == 40 && dup->pbFormat[0] == 7);
	CHECK_EQ(unk_refs, 2);
	CHECK_EQ(MoDeleteMediaType(dup), S_OK);
	CHECK_EQ(unk_refs, 1);
	CHECK_EQ(MoFreeMediaType(&a), S_OK);
	CHECK_EQ(unk_refs, 0);

	CHECK_EQ(MoCreateMediaType(&made, 0), S_OK);
	CHECK(made && !made->pbFormat && !made->cbFormat);
	CHECK_EQ(MoDeleteMediaType(made), S_OK);
	CHECK_EQ(MoInitMediaType(NULL, 0), E_POINTER);
	CHECK_EQ(MoCopyMediaType(&b, NULL), E_POINTER);
}

int main(void)
{
	setenv("VFW_REGISTRY", "", 1);
	CHECK_EQ(lowmem_reserve(), 0);
	CHECK_EQ(runtime_enter(), 0);
	reg_reload();
	dmo = mod_load("msdmo.dll", NULL);
	CHECK(dmo && dmo->builtin);
	if (!dmo)
		return test_summary("test_msdmo");

	test_register();
	test_enum();
	test_unregister();
	test_capture();
	test_media_types();

	mod_free(dmo);
	return test_summary("test_msdmo");
}
