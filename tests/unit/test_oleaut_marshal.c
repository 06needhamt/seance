/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * oleaut32's user marshal routines: BSTRs, VARIANTs and SAFEARRAYs are
 * sized, written into a buffer and read back, and what comes back is
 * compared with what went in; the BSTR's wire form is checked byte by
 * byte, and an interface's references are counted through the round trip.
 */
#include <setjmp.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "loader/modules.h"
#include "runtime/runtime.h"
#include "runtime/seh.h"
#include "test.h"
#include "win32/reg.h"

#define RPC_X_BAD_STUB_DATA_TEST 1783

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

/* The head of a MIDL_STUB_MESSAGE and the USER_MARSHAL_CB the NDR
 * engine passes as the flags. */
struct stub_msg {
	void *RpcMsg;
	unsigned char *Buffer, *BufferStart, *BufferEnd;
};

struct umcb {
	ULONG Flags;
	struct stub_msg *pStubMsg;
	void *pReserve;
	ULONG Signature;
	ULONG CBType;
	void *pFormat, *pTypeFormat;
};

#define UMCB_SIGNATURE 0x55535243u	/* 'USRC' */

/* MSHCTX_INPROC, as the NDR engine passes, and no buffer bound. */
#define UMCB_PLAIN { 3, NULL, NULL, 0, 0, NULL, NULL }

typedef ULONG (WINAPI *size_fn)(void *, ULONG, void *);
typedef unsigned char *(WINAPI *io_fn)(void *, unsigned char *, void *);
typedef void (WINAPI *free_fn)(void *, void *);

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
	struct umcb flags = UMCB_PLAIN;
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
	struct umcb flags = UMCB_PLAIN;
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
	struct umcb flags = UMCB_PLAIN;
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
	struct umcb flags = UMCB_PLAIN;
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
	struct umcb flags = UMCB_PLAIN;
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
	struct umcb flags = UMCB_PLAIN;
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

/* ---- what doesn't read raises RPC_X_BAD_STUB_DATA ---- */

static sigjmp_buf raised_at;
static DWORD raised;

static LONG WINAPI catch_raise(EXCEPTION_POINTERS *ep)
{
	raised = ep->ExceptionRecord->ExceptionCode;
	siglongjmp(raised_at, 1);
}

/* Unmarshals buf with the buffer ending at end; the exception code it
 * raised, or 0. */
static DWORD unmarshal_until(const struct kind *k, unsigned char *end,
			     void *out)
{
	struct stub_msg m = { NULL, buf, buf, end };
	static struct umcb cb;

	cb = (struct umcb){ 3, &m, NULL, UMCB_SIGNATURE, 0, NULL, NULL };
	raised = 0;
	LPTOP_LEVEL_EXCEPTION_FILTER prev =
		seh_SetUnhandledExceptionFilter(catch_raise);
	if (!sigsetjmp(raised_at, 1))
		k->unmarshal(&cb, buf, out);
	seh_SetUnhandledExceptionFilter(prev);
	return raised;
}

static void put32(unsigned char *p, DWORD v)
{
	memcpy(p, &v, 4);
}

static DWORD get32(const unsigned char *p)
{
	DWORD v;

	memcpy(&v, p, 4);
	return v;
}

static void test_wire(void)
{
	struct umcb flags = UMCB_PLAIN;
	SAFEARRAYBOUND b = { 3, 0 };
	SAFEARRAY *in = pSafeArrayCreate(VT_I4, 1, &b), *out = NULL;
	LONG idx[1];

	/* A VARIANT's reserved words go as zero; an array's discriminant
	 * is VT_ARRAY, whatever the element type. */
	VARIANT v, vout;
	memset(&v, 0, sizeof(v));
	memset(&vout, 0, sizeof(vout));
	v.vt = VT_I4;
	v.wReserved1 = 0x1111;
	v.wReserved2 = 0x2222;
	v.wReserved3 = 0x3333;
	v.lVal = 5;
	round_trip(&var_k, &v, &vout);
	CHECK(buf[10] == 0 && buf[11] == 0 && buf[12] == 0 && buf[13] == 0 &&
	      buf[14] == 0 && buf[15] == 0);
	CHECK(vout.vt == VT_I4 && vout.lVal == 5);
	v.vt = VT_ARRAY | VT_I4;
	v.parray = in;
	round_trip(&var_k, &v, &vout);
	CHECK_EQ(get32(buf + 8) & 0xffff, VT_ARRAY | VT_I4);
	CHECK_EQ(get32(buf + 16), VT_ARRAY);
	CHECK(vout.vt == (VT_ARRAY | VT_I4) && vout.parray);
	var_k.free(&flags, &vout);
	SAFEARRAY *pa = NULL;
	v.vt = VT_BYREF | VT_ARRAY | VT_I4;
	v.pparray = &pa;
	round_trip(&var_k, &v, &vout);
	CHECK_EQ(get32(buf + 16), VT_BYREF | VT_ARRAY);
	var_k.free(&flags, &vout);

	/* A SAFEARRAY of three VT_I4s as MS-OAUT lays it out: one SF_
	 * discriminant, then the count. */
	static const unsigned char want[] = {
		0x00, 0x00, 0x02, 0x00,		/* referent ID */
		0x01, 0x00, 0x00, 0x00,		/* conformance: cDims */
		0x01, 0x00, 0x80, 0x00,		/* cDims, FADF_HAVEVARTYPE */
		0x04, 0x00, 0x00, 0x00,		/* cbElements */
		0x00, 0x00, 0x03, 0x00,		/* cLocks, VT_I4 above */
		0x03, 0x00, 0x00, 0x00,		/* SF_I4 */
		0x03, 0x00, 0x00, 0x00,		/* the count */
		0x00, 0x00, 0x02, 0x00,		/* the data's referent ID */
		0x03, 0x00, 0x00, 0x00,		/* cElements */
		0x00, 0x00, 0x00, 0x00,		/* lLbound */
		0x03, 0x00, 0x00, 0x00,		/* the count again */
		0x0a, 0x00, 0x00, 0x00,
		0x14, 0x00, 0x00, 0x00,
		0x1e, 0x00, 0x00, 0x00,
	};
	for (idx[0] = 0; idx[0] < 3; idx[0]++) {
		LONG e = (idx[0] + 1) * 10;
		pSafeArrayPutElement(in, idx, &e);
	}
	CHECK_EQ(round_trip(&sa_k, &in, &out), sizeof(want));
	CHECK(!memcmp(buf, want, sizeof(want)));
	sa_k.free(&flags, &out);
	memset(buf, 0xcc, sizeof(buf));
	memcpy(buf, want, sizeof(want));
	CHECK_EQ(unmarshal_until(&sa_k, buf + sizeof(want), &out), 0);
	CHECK(out && out->cbElements == 4 && out->rgsabound[0].cElements == 3);
	if (out) {
		LONG e = 0;
		idx[0] = 2;
		pSafeArrayGetElement(out, idx, &e);
		CHECK_EQ(e, 30);
	}
	sa_k.free(&flags, &out);
	pSafeArrayDestroy(in);

	/* An interface array with FADF_HAVEIID and no FADF_HAVEVARTYPE has
	 * nothing in cLocks' high word: the SF_ kind gives the type. */
	static const unsigned char iids[] = {
		0x00, 0x00, 0x02, 0x00,
		0x01, 0x00, 0x00, 0x00,
		0x01, 0x00, 0x40, 0x02,		/* FADF_HAVEIID|FADF_UNKNOWN */
		0x04, 0x00, 0x00, 0x00,
		0x00, 0x00, 0x00, 0x00,		/* cLocks */
		0x0d, 0x80, 0x00, 0x00,		/* SF_HAVEIID */
		0x01, 0x00, 0x00, 0x00,
		0x00, 0x00, 0x02, 0x00,
		0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,	/* IUnknown */
		0xc0, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x46,
		0x01, 0x00, 0x00, 0x00,		/* cElements */
		0x00, 0x00, 0x00, 0x00,
		0x01, 0x00, 0x00, 0x00,
		0x00, 0x00, 0x00, 0x00,		/* a NULL element */
	};
	memcpy(buf, iids, sizeof(iids));
	CHECK_EQ(unmarshal_until(&sa_k, buf + sizeof(iids), &out), 0);
	CHECK(out != NULL);
	if (out) {
		VARTYPE vt = 0;
		CHECK_EQ(pSafeArrayGetVartype(out, &vt), S_OK);
		CHECK_EQ(vt, VT_UNKNOWN);
		CHECK_EQ(out->cbElements, sizeof(IUnknown *));
	}
	sa_k.free(&flags, &out);
}

static void test_bounds(void)
{
	struct umcb flags = UMCB_PLAIN;
	SAFEARRAYBOUND b = { 3, 0 };
	SAFEARRAY *in = pSafeArrayCreate(VT_BSTR, 1, &b), *out = NULL;
	LONG idx[1];

	static const char *const words[3] = { "alpha", "beta", "gamma" };
	for (idx[0] = 0; idx[0] < 3; idx[0]++) {
		BSTR w = ws(words[idx[0]]);
		pSafeArrayPutElement(in, idx, w);
		pSysFreeString(w);
	}
	size_t n = round_trip(&sa_k, &in, &out);
	sa_k.free(&flags, &out);

	/* Every truncation raises, and takes nothing (ASan's leak check
	 * sees any BSTR left behind). */
	int all = 1;
	for (size_t cut = 0; cut < n; cut++) {
		out = NULL;
		all &= unmarshal_until(&sa_k, buf + cut, &out) ==
			RPC_X_BAD_STUB_DATA_TEST && !out;
	}
	CHECK(all);
	CHECK_EQ(unmarshal_until(&sa_k, buf + n, &out), 0);
	CHECK(out != NULL);
	sa_k.free(&flags, &out);

	/* The second BSTR's referent ID gone: the first, already read, is
	 * let go of too, and the destination keeps what it had. */
	size_t ids = 4 + 7 * 4 + 8 + 4;		/* after the bounds and count */
	CHECK_EQ(get32(buf + ids + 4), 0x00020000u);
	put32(buf + ids + 4, 0);
	SAFEARRAY *keep = pSafeArrayCreate(VT_BSTR, 1, &b);
	out = keep;
	CHECK_EQ(unmarshal_until(&sa_k, buf + n, &out),
		 RPC_X_BAD_STUB_DATA_TEST);
	CHECK(out == keep);
	pSafeArrayDestroy(keep);
	pSafeArrayDestroy(in);

	/* A VARIANT that doesn't read leaves the old one as it was. */
	VARIANT v, vout;
	memset(&v, 0, sizeof(v));
	memset(&vout, 0, sizeof(vout));
	v.vt = VT_BSTR;
	v.bstrVal = ws("whole");
	n = round_trip(&var_k, &v, &vout);
	CHECK(vout.vt == VT_BSTR && bstr_eq(vout.bstrVal, v.bstrVal));
	CHECK_EQ(unmarshal_until(&var_k, buf + n - 1, &vout),
		 RPC_X_BAD_STUB_DATA_TEST);
	CHECK(vout.vt == VT_BSTR && bstr_eq(vout.bstrVal, v.bstrVal));
	var_k.free(&flags, &vout);
	pVariantClear(&v);
}

/* A by-reference VARIANT of the same type is read into the storage
 * the destination already points to. */
static void test_byref_reuse(void)
{
	VARIANT in, out;
	SHORT src = 42, dst = 7;

	memset(&in, 0, sizeof(in));
	memset(&out, 0, sizeof(out));
	in.vt = VT_BYREF | VT_I2;
	in.piVal = &src;
	out.vt = VT_BYREF | VT_I2;
	out.piVal = &dst;
	round_trip(&var_k, &in, &out);
	CHECK(out.vt == (VT_BYREF | VT_I2) && out.piVal == &dst);
	CHECK_EQ(dst, 42);

	BSTR a = ws("first"), c = ws("second");
	BSTR slot = a;
	in.vt = VT_BYREF | VT_BSTR;
	in.pbstrVal = &c;
	out.vt = VT_BYREF | VT_BSTR;
	out.pbstrVal = &slot;
	round_trip(&var_k, &in, &out);	/* (lets go of "first") */
	CHECK(out.pbstrVal == &slot && bstr_eq(slot, c));
	pSysFreeString(slot);
	pSysFreeString(c);
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
	test_wire();
	test_bounds();
	test_byref_reuse();

	mod_free(oa);
	return test_summary("test_oleaut_marshal");
}
