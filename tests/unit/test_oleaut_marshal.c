/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * oleaut32's user marshal routines: BSTRs, VARIANTs and SAFEARRAYs are
 * sized, written into a buffer and read back, and what comes back is
 * compared with what went in; the BSTR's wire form is checked byte by
 * byte, and an interface's references are counted through the round trip.
 */
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "loader/modules.h"
#include "runtime/runtime.h"
#include "test.h"
#include "win32/reg.h"

static struct module *oa;

static void *get(const char *name)
{
	void *p = mod_get_proc(oa, name);

	if (!p) {
		fprintf(stderr, "oleaut32 has no %s\n", name);
		exit(1);
	}
	return p;
}

static BSTR (WINAPI *pSysAllocString)(LPCOLESTR);
static BSTR (WINAPI *pSysAllocStringByteLen)(LPCSTR, UINT);
static UINT (WINAPI *pSysStringByteLen)(BSTR);
static void (WINAPI *pSysFreeString)(BSTR);
static HRESULT (WINAPI *pVariantClear)(VARIANT *);
static SAFEARRAY *(WINAPI *pSafeArrayCreate)(VARTYPE, UINT,
					      SAFEARRAYBOUND *);
static HRESULT (WINAPI *pSafeArrayDestroy)(SAFEARRAY *);
static HRESULT (WINAPI *pSafeArrayPutElement)(SAFEARRAY *, LONG *, void *);
static HRESULT (WINAPI *pSafeArrayGetElement)(SAFEARRAY *, LONG *, void *);
static HRESULT (WINAPI *pSafeArrayGetVartype)(SAFEARRAY *, VARTYPE *);

typedef ULONG (WINAPI *size_fn)(ULONG *, ULONG, void *);
typedef unsigned char *(WINAPI *io_fn)(ULONG *, unsigned char *, void *);
typedef void (WINAPI *free_fn)(ULONG *, void *);

struct kind {
	size_fn size;
	io_fn marshal, unmarshal;
	free_fn free;
};

static struct kind bstr_k, var_k, sa_k;

static void kind(struct kind *k, const char *prefix)
{
	char n[64];

	snprintf(n, sizeof(n), "%s_UserSize", prefix);
	k->size = (size_fn)get(n);
	snprintf(n, sizeof(n), "%s_UserMarshal", prefix);
	k->marshal = (io_fn)get(n);
	snprintf(n, sizeof(n), "%s_UserUnmarshal", prefix);
	k->unmarshal = (io_fn)get(n);
	snprintf(n, sizeof(n), "%s_UserFree", prefix);
	k->free = (free_fn)get(n);
}

/* An 8-aligned buffer, filled with a pattern so that padding shows. */
static unsigned char buf[16384] __attribute__((aligned(16)));

/*
 * Sizes, writes and reads back *in into *out; the sizes and the ends of
 * writing and reading must agree. Returns the bytes written.
 */
static size_t round_trip(const struct kind *k, void *in, void *out)
{
	ULONG flags = 3;	/* MSHCTX_INPROC, as the NDR engine passes */
	memset(buf, 0xcc, sizeof(buf));
	ULONG size = k->size(&flags, 0, in);
	unsigned char *end = k->marshal(&flags, buf, in);
	CHECK_EQ((size_t)(end - buf), (size_t)size);
	/* A start that isn't aligned is aligned first. */
	CHECK(k->size(&flags, 1, in) >= size);
	unsigned char *rend = k->unmarshal(&flags, buf, out);
	CHECK(rend == end);
	return (size_t)(end - buf);
}

static BSTR ws(const char *s)
{
	WCHAR w[64];
	size_t i = 0;

	for (; s[i] && i < 63; i++)
		w[i] = (BYTE)s[i];
	w[i] = 0;
	return pSysAllocString(w);
}

static int bstr_eq(BSTR a, BSTR b)
{
	if (!a || !b)
		return a == b;
	UINT n = pSysStringByteLen(a);
	return n == pSysStringByteLen(b) && !memcmp(a, b, n);
}

/* ---- a counted object ---- */

static const GUID iid_unknown = { 0, 0, 0,
	{ 0xc0, 0, 0, 0, 0, 0, 0, 0x46 } };
static const GUID iid_dispatch = { 0x00020400, 0, 0,
	{ 0xc0, 0, 0, 0, 0, 0, 0, 0x46 } };

struct obj {
	IUnknown iface;
	LONG refs;
};

WINAPI static HRESULT obj_qi(IUnknown *self, REFIID riid, void **ppv)
{
	if (!memcmp(riid, &iid_unknown, sizeof(GUID))) {
		*ppv = self;
		((struct obj *)self)->refs++;
		return S_OK;
	}
	*ppv = NULL;
	return E_NOINTERFACE;
}

WINAPI static ULONG obj_addref(IUnknown *self)
{
	return ++((struct obj *)self)->refs;
}

WINAPI static ULONG obj_release(IUnknown *self)
{
	return --((struct obj *)self)->refs;
}

static const IUnknownVtbl obj_vtbl = { obj_qi, obj_addref, obj_release };

/* ---- the tests ---- */

static void test_bstr(void)
{
	ULONG flags = 3;
	BSTR in = ws("hi"), out = NULL;

	/* The wire form. */
	CHECK_EQ(bstr_k.size(&flags, 0, &in), 16);
	memset(buf, 0xcc, sizeof(buf));
	CHECK(bstr_k.marshal(&flags, buf, &in) == buf + 16);
	static const BYTE want[16] = { 2, 0, 0, 0, 4, 0, 0, 0, 2, 0, 0, 0,
				       'h', 0, 'i', 0 };
	CHECK(!memcmp(buf, want, sizeof(want)));
	/* Aligned to 4 from where it starts. */
	CHECK_EQ(bstr_k.size(&flags, 1, &in), 4 + 16);
	CHECK(bstr_k.unmarshal(&flags, buf, &out) == buf + 16);
	CHECK(bstr_eq(in, out));
	bstr_k.free(&flags, &out);
	CHECK(out == NULL);

	/* An odd byte length keeps its length. */
	BSTR odd = pSysAllocStringByteLen("abc", 3);
	CHECK_EQ(round_trip(&bstr_k, &odd, &out), 16);
	CHECK(bstr_eq(odd, out));
	CHECK_EQ(pSysStringByteLen(out), 3);
	bstr_k.free(&flags, &out);
	pSysFreeString(odd);

	/* NULL and empty differ. */
	BSTR null = NULL, empty = ws("");
	out = ws("old");	/* replaced */
	CHECK_EQ(round_trip(&bstr_k, &null, &out), 12);
	CHECK(out == NULL);
	CHECK_EQ(buf[4], 0xff);
	CHECK_EQ(round_trip(&bstr_k, &empty, &out), 12);
	CHECK(out && pSysStringByteLen(out) == 0);
	bstr_k.free(&flags, &out);
	pSysFreeString(empty);
	pSysFreeString(in);
}

static void test_scalars(void)
{
	ULONG flags = 3;
	VARIANT in, out;

	memset(&out, 0, sizeof(out));
	memset(&in, 0, sizeof(in));
	in.vt = VT_I4;
	in.lVal = -123456;
	round_trip(&var_k, &in, &out);
	CHECK(out.vt == VT_I4 && out.lVal == -123456);
	/* The header: size in 8-byte units, vt twice. */
	CHECK_EQ(buf[0], 3);
	CHECK(buf[8] == VT_I4 && buf[16] == VT_I4);

	in.vt = VT_R8;
	in.dblVal = 2.5;
	round_trip(&var_k, &in, &out);
	CHECK(out.vt == VT_R8 && out.dblVal == 2.5);

	in.vt = VT_BOOL;
	in.boolVal = VARIANT_TRUE;
	round_trip(&var_k, &in, &out);
	CHECK(out.vt == VT_BOOL && out.boolVal == VARIANT_TRUE);

	in.vt = VT_UI1;
	in.bVal = 0xa5;
	round_trip(&var_k, &in, &out);
	CHECK(out.vt == VT_UI1 && out.bVal == 0xa5);

	in.vt = VT_CY;
	in.cyVal.int64 = 12345678901234LL;
	round_trip(&var_k, &in, &out);
	CHECK(out.vt == VT_CY && out.cyVal.int64 == 12345678901234LL);

	in.vt = VT_NULL;
	round_trip(&var_k, &in, &out);
	CHECK(out.vt == VT_NULL);

	memset(&in, 0, sizeof(in));
	in.decVal.scale = 2;
	in.decVal.sign = 0x80;
	in.decVal.Lo64 = 31415;
	in.decVal.Hi32 = 7;
	in.vt = VT_DECIMAL;
	round_trip(&var_k, &in, &out);
	CHECK(out.vt == VT_DECIMAL && out.decVal.scale == 2 &&
	      out.decVal.sign == 0x80 && out.decVal.Lo64 == 31415 &&
	      out.decVal.Hi32 == 7);

	in.vt = VT_BSTR;
	in.bstrVal = ws("text");
	round_trip(&var_k, &in, &out);
	CHECK(out.vt == VT_BSTR && bstr_eq(in.bstrVal, out.bstrVal));
	var_k.free(&flags, &out);
	CHECK_EQ(out.vt, VT_EMPTY);
	pVariantClear(&in);

	/* A NULL BSTR stays NULL. */
	in.vt = VT_BSTR;
	in.bstrVal = NULL;
	round_trip(&var_k, &in, &out);
	CHECK(out.vt == VT_BSTR && !out.bstrVal);
}

static void test_byref(void)
{
	ULONG flags = 3;
	VARIANT in, out, inner;
	LONG l = 77;
	BSTR b = ws("ref");

	memset(&out, 0, sizeof(out));
	memset(&in, 0, sizeof(in));
	in.vt = VT_BYREF | VT_I4;
	in.plVal = &l;
	round_trip(&var_k, &in, &out);
	CHECK(out.vt == (VT_BYREF | VT_I4) && out.plVal && out.plVal != &l &&
	      *out.plVal == 77);
	var_k.free(&flags, &out);
	CHECK_EQ(out.vt, VT_EMPTY);

	in.vt = VT_BYREF | VT_BSTR;
	in.pbstrVal = &b;
	round_trip(&var_k, &in, &out);
	CHECK(out.vt == (VT_BYREF | VT_BSTR) && out.pbstrVal &&
	      bstr_eq(*out.pbstrVal, b));
	var_k.free(&flags, &out);

	memset(&inner, 0, sizeof(inner));
	inner.vt = VT_BSTR;
	inner.bstrVal = b;
	in.vt = VT_BYREF | VT_VARIANT;
	in.pvarVal = &inner;
	round_trip(&var_k, &in, &out);
	CHECK(out.vt == (VT_BYREF | VT_VARIANT) && out.pvarVal &&
	      out.pvarVal->vt == VT_BSTR &&
	      bstr_eq(out.pvarVal->bstrVal, b));
	var_k.free(&flags, &out);

	/* A NULL reference. */
	in.vt = VT_BYREF | VT_I4;
	in.plVal = NULL;
	round_trip(&var_k, &in, &out);
	CHECK(out.vt == (VT_BYREF | VT_I4) && !out.plVal);
	pSysFreeString(b);
}

static void test_iface(void)
{
	ULONG flags = 3;
	struct obj o = { { (IUnknownVtbl *)&obj_vtbl }, 1 };
	VARIANT in, out;

	memset(&in, 0, sizeof(in));
	memset(&out, 0, sizeof(out));
	in.vt = VT_UNKNOWN;
	in.punkVal = &o.iface;
	round_trip(&var_k, &in, &out);
	CHECK(out.vt == VT_UNKNOWN && out.punkVal == &o.iface);
	CHECK_EQ(o.refs, 2);	/* the original's and the copy's */
	var_k.free(&flags, &out);
	CHECK_EQ(o.refs, 1);

	in.punkVal = NULL;
	round_trip(&var_k, &in, &out);
	CHECK(out.vt == VT_UNKNOWN && !out.punkVal);

	IUnknown *p = &o.iface;
	in.vt = VT_BYREF | VT_UNKNOWN;
	in.ppunkVal = &p;
	round_trip(&var_k, &in, &out);
	CHECK(out.vt == (VT_BYREF | VT_UNKNOWN) && out.ppunkVal &&
	      *out.ppunkVal == &o.iface);
	CHECK_EQ(o.refs, 2);
	var_k.free(&flags, &out);
	CHECK_EQ(o.refs, 1);
}

static void test_arrays(void)
{
	ULONG flags = 3;
	SAFEARRAYBOUND b2[2] = { { 3, 1 }, { 2, -1 } };
	SAFEARRAY *in = pSafeArrayCreate(VT_I2, 2, b2), *out = NULL;
	LONG idx[2];
	VARTYPE vt;

	CHECK(in != NULL);
	if (!in)
		return;
	/* (the indices leftmost dimension first, as the bounds went in) */
	for (idx[0] = 1; idx[0] < 4; idx[0]++)
		for (idx[1] = -1; idx[1] < 1; idx[1]++) {
			SHORT v = (SHORT)(idx[0] * 10 + idx[1]);
			CHECK_EQ(pSafeArrayPutElement(in, idx, &v), S_OK);
		}
	round_trip(&sa_k, &in, &out);
	CHECK(out && out != in && out->cDims == 2 && out->cbElements == 2);
	if (out) {
		CHECK(!memcmp(out->rgsabound, in->rgsabound,
			      sizeof(b2)));
		CHECK_EQ(pSafeArrayGetVartype(out, &vt), S_OK);
		CHECK_EQ(vt, VT_I2);
		int same = 1;
		for (idx[0] = 1; idx[0] < 4; idx[0]++)
			for (idx[1] = -1; idx[1] < 1; idx[1]++) {
				SHORT v = 0;
				pSafeArrayGetElement(out, idx, &v);
				same &= v == idx[0] * 10 + idx[1];
			}
		CHECK(same);
	}
	sa_k.free(&flags, &out);
	CHECK(out == NULL);
	pSafeArrayDestroy(in);

	/* A NULL array. */
	in = NULL;
	CHECK_EQ(round_trip(&sa_k, &in, &out), 4);
	CHECK(out == NULL);

	/* BSTRs, inside a VARIANT. */
	SAFEARRAYBOUND b1 = { 3, 0 };
	VARIANT vin, vout;
	memset(&vin, 0, sizeof(vin));
	memset(&vout, 0, sizeof(vout));
	vin.vt = VT_ARRAY | VT_BSTR;
	vin.parray = pSafeArrayCreate(VT_BSTR, 1, &b1);
	static const char *const words[3] = { "one", "", "three" };
	for (idx[0] = 0; idx[0] < 3; idx[0]++) {
		BSTR w = ws(words[idx[0]]);
		pSafeArrayPutElement(vin.parray, idx, w);
		pSysFreeString(w);
	}
	round_trip(&var_k, &vin, &vout);
	CHECK(vout.vt == (VT_ARRAY | VT_BSTR) && vout.parray);
	if (vout.parray) {
		int same = 1;
		for (idx[0] = 0; idx[0] < 3; idx[0]++) {
			BSTR a = NULL, c = NULL;
			pSafeArrayGetElement(vin.parray, idx, &a);
			pSafeArrayGetElement(vout.parray, idx, &c);
			same &= bstr_eq(a, c);
			pSysFreeString(a);
			pSysFreeString(c);
		}
		CHECK(same);
	}
	var_k.free(&flags, &vout);
	pVariantClear(&vin);

	/* VARIANTs, by reference. */
	SAFEARRAY *va = pSafeArrayCreate(VT_VARIANT, 1, &b1);
	VARIANT e;
	memset(&e, 0, sizeof(e));
	for (idx[0] = 0; idx[0] < 3; idx[0]++) {
		e.vt = VT_I4;
		e.lVal = idx[0] * 100;
		pSafeArrayPutElement(va, idx, &e);
	}
	vin.vt = VT_BYREF | VT_ARRAY | VT_VARIANT;
	vin.pparray = &va;
	round_trip(&var_k, &vin, &vout);
	CHECK(vout.vt == (VT_BYREF | VT_ARRAY | VT_VARIANT) &&
	      vout.pparray && *vout.pparray);
	if (vout.pparray && *vout.pparray) {
		VARIANT got;
		memset(&got, 0, sizeof(got));
		idx[0] = 2;
		pSafeArrayGetElement(*vout.pparray, idx, &got);
		CHECK(got.vt == VT_I4 && got.lVal == 200);
	}
	var_k.free(&flags, &vout);
	pSafeArrayDestroy(va);

	/* Interfaces keep their IID. */
	struct obj o = { { (IUnknownVtbl *)&obj_vtbl }, 1 };
	SAFEARRAY *ua = pSafeArrayCreate(VT_UNKNOWN, 1, &b1);
	idx[0] = 1;
	pSafeArrayPutElement(ua, idx, &o.iface);
	CHECK_EQ(o.refs, 2);
	round_trip(&sa_k, &ua, &out);
	CHECK_EQ(o.refs, 3);
	if (out) {
		IUnknown *got = NULL;
		CHECK_EQ(pSafeArrayGetVartype(out, &vt), S_OK);
		CHECK_EQ(vt, VT_UNKNOWN);
		pSafeArrayGetElement(out, idx, &got);
		CHECK(got == &o.iface);
		if (got)
			got->lpVtbl->Release(got);
	}
	sa_k.free(&flags, &out);
	pSafeArrayDestroy(ua);
	CHECK_EQ(o.refs, 1);
	(void)iid_dispatch;
}

int main(void)
{
	setenv("VFW_REGISTRY", "", 1);
	CHECK_EQ(runtime_enter(), 0);
	reg_reload();
	oa = mod_load("oleaut32.dll", NULL);
	CHECK(oa != NULL);
	if (!oa)
		return test_summary("test_oleaut_marshal");
	pSysAllocString = get("SysAllocString");
	pSysAllocStringByteLen = get("SysAllocStringByteLen");
	pSysStringByteLen = get("SysStringByteLen");
	pSysFreeString = get("SysFreeString");
	pVariantClear = get("VariantClear");
	pSafeArrayCreate = get("SafeArrayCreate");
	pSafeArrayDestroy = get("SafeArrayDestroy");
	pSafeArrayPutElement = get("SafeArrayPutElement");
	pSafeArrayGetElement = get("SafeArrayGetElement");
	pSafeArrayGetVartype = get("SafeArrayGetVartype");
	kind(&bstr_k, "BSTR");
	kind(&var_k, "VARIANT");
	kind(&sa_k, "LPSAFEARRAY");
	CHECK(mod_get_proc(oa, "BSTR_UserSize") == mod_get_ordinal(oa, 283));
	CHECK(mod_get_proc(oa, "LPSAFEARRAY_UserFree") ==
	      mod_get_ordinal(oa, 294));

	test_bstr();
	test_scalars();
	test_byref();
	test_iface();
	test_arrays();

	mod_free(oa);
	return test_summary("test_oleaut_marshal");
}
