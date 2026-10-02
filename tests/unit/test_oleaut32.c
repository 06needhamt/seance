/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * oleaut32 through its export table: BSTRs, VARIANTs and their
 * conversions, dates, SAFEARRAYs, error information (also through
 * ole32), DispGetParam and the headless rest; and its ordinals.
 */
#include <math.h>
#include <stdlib.h>
#include <string.h>

#include "common/sanitizer.h"
#include "loader/modules.h"
#include "runtime/runtime.h"
#include "test.h"
#include "win32/codepage.h"

static struct module *oa;

/* Where Win32 code can use it (the sanitizers' allocator isn't kept low). */
static int low(const void *p)
{
	return VFW_SANITIZER_BUILD || (uintptr_t)p < 0x80000000u;
}

static void *get(const char *name)
{
	void *p = mod_get_proc(oa, name);
	if (!p) {
		fprintf(stderr, "oleaut32 has no %s\n", name);
		exit(1);
	}
	return p;
}

#define FN(ret, name, ...) \
	ret (WINAPI *name)(__VA_ARGS__) = (ret (WINAPI *)(__VA_ARGS__))get(#name)

/* A BSTR's text as UTF-8, for comparing (static buffer). */
static const char *text(BSTR b)
{
	static char buf[4][256];
	static int next;
	char *s = buf[next++ % 4];
	size_t i = 0;

	if (!b)
		return "(null)";
	for (; b[i] && i < 255; i++)
		s[i] = b[i] < 128 ? (char)b[i] : '?';
	s[i] = '\0';
	return s;
}

static WCHAR *w(const char *s)
{
	static WCHAR buf[8][128];
	static int next;
	WCHAR *o = buf[next++ % 8];
	size_t i = 0;

	for (; s[i] && i < 127; i++)
		o[i] = (BYTE)s[i];
	o[i] = 0;
	return o;
}

/* Fake COM objects that count references. */

static LONG unk_refs;
static IDispatch fake_disp;

static HRESULT WINAPI unk_qi(IUnknown *u, REFIID riid, void **out)
{
	static const IID dispatch = { 0x00020400, 0, 0,
				      { 0xc0, 0, 0, 0, 0, 0, 0, 0x46 } };
	if (!memcmp(riid, &dispatch, sizeof(IID))) {
		*out = &fake_disp;
		unk_refs++;
		return S_OK;
	}
	*out = u;
	unk_refs++;
	return S_OK;
}

static ULONG WINAPI unk_addref(IUnknown *u)
{
	return (ULONG)++unk_refs;
}

static ULONG WINAPI unk_release(IUnknown *u)
{
	return (ULONG)--unk_refs;
}

static const IUnknownVtbl unk_vtbl = { unk_qi, unk_addref, unk_release };
static IUnknown fake_unk = { &unk_vtbl };

/* An object whose value property (DISPID_VALUE) is the I4 7. */
static HRESULT WINAPI disp_invoke(IDispatch *d, DISPID id, REFIID riid,
				  LCID lcid, WORD flags, DISPPARAMS *p,
				  VARIANT *result, EXCEPINFO *e, UINT *err)
{
	if (id != DISPID_VALUE || !(flags & DISPATCH_PROPERTYGET) || !result)
		return DISP_E_MEMBERNOTFOUND;
	result->vt = VT_I4;
	result->lVal = 7;
	return S_OK;
}

static const IDispatchVtbl disp_vtbl = {
	(void *)unk_qi, (void *)unk_addref, (void *)unk_release, NULL, NULL,
	NULL, disp_invoke,
};
static IDispatch fake_disp = { &disp_vtbl };

static void test_bstr(void)
{
	FN(BSTR, SysAllocString, LPCOLESTR);
	FN(BSTR, SysAllocStringLen, const OLECHAR *, UINT);
	FN(BSTR, SysAllocStringByteLen, LPCSTR, UINT);
	FN(INT, SysReAllocString, BSTR *, LPCOLESTR);
	FN(INT, SysReAllocStringLen, BSTR *, const OLECHAR *, UINT);
	FN(void, SysFreeString, BSTR);
	FN(UINT, SysStringLen, BSTR);
	FN(UINT, SysStringByteLen, BSTR);
	FN(HRESULT, VarBstrCat, BSTR, BSTR, BSTR *);
	FN(HRESULT, VarBstrCmp, BSTR, BSTR, LCID, ULONG);

	BSTR b = SysAllocString(w("hello"));
	CHECK(b && low(b));
	CHECK(SysStringLen(b) == 5 && SysStringByteLen(b) == 10);
	CHECK(((DWORD *)b)[-1] == 10 && b[5] == 0);
	CHECK_STR(text(b), "hello");
	SysFreeString(b);
	CHECK(!SysAllocString(NULL));
	CHECK_EQ(SysStringLen(NULL), 0);
	SysFreeString(NULL);

	b = SysAllocStringLen(NULL, 3);
	CHECK(b && SysStringLen(b) == 3 && !b[0] && !b[3]);
	SysFreeString(b);
	b = SysAllocStringLen(w("hello"), 2);
	CHECK_STR(text(b), "he");
	/* Odd byte counts still end in a NUL WCHAR. */
	BSTR c = SysAllocStringByteLen("abc", 3);
	CHECK(c && SysStringByteLen(c) == 3 && SysStringLen(c) == 1);
	CHECK(!memcmp(c, "abc", 3) && !((BYTE *)c)[3] && !((BYTE *)c)[4]);
	SysFreeString(c);

	CHECK(SysReAllocString(&b, w("xyz")));
	CHECK_STR(text(b), "xyz");
	CHECK(SysReAllocStringLen(&b, b + 1, 2));	/* from itself */
	CHECK_STR(text(b), "yz");
	CHECK(!SysReAllocString(NULL, w("q")));

	BSTR cat = NULL;
	CHECK_EQ(VarBstrCat(b, NULL, &cat), S_OK);
	CHECK_STR(text(cat), "yz");
	SysFreeString(cat);
	BSTR x = SysAllocString(w("abc")), y = SysAllocString(w("abd")),
	     z = SysAllocString(w("ABC"));
	CHECK_EQ(VarBstrCat(x, y, &cat), S_OK);
	CHECK_STR(text(cat), "abcabd");
	CHECK_EQ(VarBstrCmp(x, y, 0, 0), VARCMP_LT);
	CHECK_EQ(VarBstrCmp(y, x, 0, 0), VARCMP_GT);
	CHECK_EQ(VarBstrCmp(x, z, 0, 0), VARCMP_GT);
	CHECK_EQ(VarBstrCmp(x, z, 0, 1 /* NORM_IGNORECASE */), VARCMP_EQ);
	CHECK_EQ(VarBstrCmp(NULL, SysAllocStringLen(NULL, 0), 0, 0), VARCMP_EQ);
	SysFreeString(cat);
	SysFreeString(x);
	SysFreeString(y);
	SysFreeString(z);
	SysFreeString(b);
}

static void test_variants(void)
{
	FN(void, VariantInit, VARIANTARG *);
	FN(HRESULT, VariantClear, VARIANTARG *);
	FN(HRESULT, VariantCopy, VARIANTARG *, const VARIANTARG *);
	FN(HRESULT, VariantCopyInd, VARIANTARG *, const VARIANTARG *);
	FN(BSTR, SysAllocString, LPCOLESTR);
	VARIANT a, b;

	memset(&a, 0xcc, sizeof(a));
	VariantInit(&a);
	CHECK_EQ(a.vt, VT_EMPTY);
	a.vt = VT_BSTR;
	a.bstrVal = SysAllocString(w("text"));
	VariantInit(&b);
	CHECK_EQ(VariantCopy(&b, &a), S_OK);
	CHECK(b.vt == VT_BSTR && b.bstrVal != a.bstrVal);
	CHECK_STR(text(b.bstrVal), "text");
	CHECK_EQ(VariantClear(&a), S_OK);
	CHECK_EQ(a.vt, VT_EMPTY);
	CHECK_EQ(VariantCopy(&b, &b), S_OK);
	VariantClear(&b);

	/* Types no VARIANT may have. */
	a.vt = 15;
	CHECK_EQ(VariantClear(&a), DISP_E_BADVARTYPE);
	a.vt = VT_I4 | VT_VECTOR;
	CHECK_EQ(VariantClear(&a), DISP_E_BADVARTYPE);
	a.vt = VT_NULL | VT_BYREF;
	CHECK_EQ(VariantCopy(&b, &a), DISP_E_BADVARTYPE);

	/* By reference: copied as such, or through. */
	LONG n = 7;
	BSTR s = SysAllocString(w("ref"));
	a.vt = VT_I4 | VT_BYREF;
	a.plVal = &n;
	VariantInit(&b);
	CHECK_EQ(VariantCopy(&b, &a), S_OK);
	CHECK(b.vt == (VT_I4 | VT_BYREF) && b.plVal == &n);
	CHECK_EQ(VariantCopyInd(&b, &a), S_OK);
	CHECK(b.vt == VT_I4 && b.lVal == 7);
	a.vt = VT_BSTR | VT_BYREF;
	a.pbstrVal = &s;
	CHECK_EQ(VariantCopyInd(&b, &a), S_OK);
	CHECK(b.vt == VT_BSTR && b.bstrVal != s);
	CHECK_STR(text(b.bstrVal), "ref");
	VariantClear(&b);
	VARIANT inner = { .vt = VT_I2, .iVal = -3 }, outer;
	outer.vt = VT_VARIANT | VT_BYREF;
	outer.pvarVal = &inner;
	CHECK_EQ(VariantCopyInd(&b, &outer), S_OK);
	CHECK(b.vt == VT_I2 && b.iVal == -3);
	a.vt = VT_BSTR;
	a.bstrVal = s;
	VariantClear(&a);

	/* Interfaces: AddRef on copy, Release on clear, not by reference. */
	unk_refs = 1;
	a.vt = VT_UNKNOWN;
	a.punkVal = &fake_unk;
	VariantInit(&b);
	CHECK_EQ(VariantCopy(&b, &a), S_OK);
	CHECK_EQ(unk_refs, 2);
	VariantClear(&b);
	CHECK_EQ(unk_refs, 1);
	IUnknown *pu = &fake_unk;
	a.vt = VT_UNKNOWN | VT_BYREF;
	a.ppunkVal = &pu;
	VariantClear(&a);
	CHECK_EQ(unk_refs, 1);
}

static HRESULT change(VARIANT *out, VARTYPE from_vt, VARIANT in, VARTYPE vt,
		      USHORT flags)
{
	HRESULT (WINAPI *cvt)(VARIANTARG *, const VARIANTARG *, USHORT,
			      VARTYPE) = get("VariantChangeType");
	in.vt = from_vt;
	out->vt = VT_EMPTY;
	return cvt(out, &in, flags, vt);
}

/* A value converted to a BSTR's text. */
static const char *as_text(VARIANT in, VARTYPE from, USHORT flags)
{
	HRESULT (WINAPI *clear)(VARIANTARG *) = get("VariantClear");
	static char buf[4][80];
	static int next;
	char *s = buf[next++ % 4];
	VARIANT out;

	HRESULT hr = change(&out, from, in, VT_BSTR, flags);
	if (FAILED(hr)) {
		snprintf(s, 80, "error 0x%08lx", (unsigned long)hr);
		return s;
	}
	snprintf(s, 80, "%s", text(out.bstrVal));
	clear(&out);
	return s;
}

/* A string converted to type vt; *hr its result. */
static VARIANT from_text(const char *str, VARTYPE vt, HRESULT *hr)
{
	FN(BSTR, SysAllocString, LPCOLESTR);
	FN(HRESULT, VariantClear, VARIANTARG *);
	VARIANT in = { .bstrVal = SysAllocString(w(str)) }, out;

	*hr = change(&out, VT_BSTR, in, vt, 0);
	in.vt = VT_BSTR;
	VariantClear(&in);
	return out;
}

static void test_conversions(void)
{
	FN(HRESULT, VariantClear, VARIANTARG *);
	FN(HRESULT, VariantChangeType, VARIANTARG *, const VARIANTARG *, USHORT,
	   VARTYPE);
	FN(HRESULT, VariantChangeTypeEx, VARIANTARG *, const VARIANTARG *, LCID,
	   USHORT, VARTYPE);
	FN(BSTR, SysAllocString, LPCOLESTR);
	FN(BSTR, SysAllocStringByteLen, LPCSTR, UINT);
	FN(UINT, SysStringByteLen, BSTR);
	VARIANT o;
	HRESULT hr;

	/* To text. */
	CHECK_STR(as_text((VARIANT){ .lVal = 42 }, VT_I4, 0), "42");
	CHECK_STR(as_text((VARIANT){ .llVal = -5000000000LL }, VT_I8, 0),
		  "-5000000000");
	CHECK_STR(as_text((VARIANT){ .ulVal = 4000000000u }, VT_UI4, 0),
		  "4000000000");
	CHECK_STR(as_text((VARIANT){ .dblVal = 0.1 }, VT_R8, 0), "0.1");
	CHECK_STR(as_text((VARIANT){ .dblVal = 1e20 }, VT_R8, 0), "1E+20");
	CHECK_STR(as_text((VARIANT){ .dblVal = 1.0 / 3 }, VT_R8, 0),
		  "0.333333333333333");
	CHECK_STR(as_text((VARIANT){ .fltVal = 0.1f }, VT_R4, 0), "0.1");
	CHECK_STR(as_text((VARIANT){ .cyVal.int64 = 15000 }, VT_CY, 0), "1.5");
	CHECK_STR(as_text((VARIANT){ .cyVal.int64 = -20000 }, VT_CY, 0), "-2");
	CHECK_STR(as_text((VARIANT){ .boolVal = VARIANT_TRUE }, VT_BOOL, 0), "-1");
	CHECK_STR(as_text((VARIANT){ .boolVal = VARIANT_TRUE }, VT_BOOL,
			  VARIANT_ALPHABOOL), "True");
	CHECK_STR(as_text((VARIANT){ .date = 36526.5 }, VT_DATE, 0),
		  "1/1/2000 12:00:00 PM");
	CHECK_STR(as_text((VARIANT){ .date = 36526 }, VT_DATE, 0), "1/1/2000");
	CHECK_STR(as_text((VARIANT){ .date = 0.25 }, VT_DATE, 0), "6:00:00 AM");
	CHECK_STR(as_text((VARIANT){ .date = 0 }, VT_DATE, 0), "12:00:00 AM");
	CHECK_STR(as_text((VARIANT){ .date = -1.25 }, VT_DATE, 0),
		  "12/29/1899 6:00:00 AM");
	CHECK_STR(as_text((VARIANT){ 0 }, VT_EMPTY, 0), "");
	CHECK(!strncmp(as_text((VARIANT){ 0 }, VT_NULL, 0), "error 0x80020005",
		       16));

	/* From text: US English numbers. */
	o = from_text("  1,234  ", VT_I4, &hr);
	CHECK(hr == S_OK && o.lVal == 1234);
	o = from_text("2.5", VT_I4, &hr);
	CHECK(hr == S_OK && o.lVal == 2);
	o = from_text("3.5", VT_I4, &hr);
	CHECK(hr == S_OK && o.lVal == 4);
	o = from_text("-2.5", VT_I4, &hr);
	CHECK(hr == S_OK && o.lVal == -2);
	o = from_text("&HFF", VT_UI1, &hr);
	CHECK(hr == S_OK && o.bVal == 255);
	o = from_text("&HFFFF", VT_I2, &hr);
	CHECK(hr == S_OK && o.iVal == -1);
	o = from_text("&O17", VT_I4, &hr);
	CHECK(hr == S_OK && o.lVal == 15);
	from_text("&H10000", VT_I2, &hr);
	CHECK_EQ(hr, DISP_E_OVERFLOW);
	o = from_text("(5)", VT_I4, &hr);
	CHECK(hr == S_OK && o.lVal == -5);
	o = from_text("$3.25", VT_CY, &hr);
	CHECK(hr == S_OK && o.cyVal.int64 == 32500);
	o = from_text("1e3", VT_I4, &hr);
	CHECK(hr == S_OK && o.lVal == 1000);
	o = from_text("-9223372036854775808", VT_I8, &hr);
	CHECK(hr == S_OK && o.llVal == INT64_MIN);
	o = from_text("18446744073709551615", VT_UI8, &hr);
	CHECK(hr == S_OK && o.ullVal == UINT64_MAX);
	from_text("18446744073709551615", VT_I8, &hr);
	CHECK_EQ(hr, DISP_E_OVERFLOW);
	from_text("40000", VT_I2, &hr);
	CHECK_EQ(hr, DISP_E_OVERFLOW);
	from_text("abc", VT_I4, &hr);
	CHECK_EQ(hr, DISP_E_TYPEMISMATCH);
	from_text("", VT_R8, &hr);
	CHECK_EQ(hr, DISP_E_TYPEMISMATCH);
	from_text("1.2.3", VT_R8, &hr);
	CHECK_EQ(hr, DISP_E_TYPEMISMATCH);
	o = from_text("True", VT_BOOL, &hr);
	CHECK(hr == S_OK && o.boolVal == VARIANT_TRUE);
	o = from_text(" false ", VT_BOOL, &hr);
	CHECK(hr == S_OK && o.boolVal == VARIANT_FALSE);
	o = from_text("12", VT_BOOL, &hr);
	CHECK(hr == S_OK && o.boolVal == VARIANT_TRUE);
	o = from_text("1.5", VT_R4, &hr);
	CHECK(hr == S_OK && o.fltVal == 1.5f);

	/* From text: dates. */
	o = from_text("1/2/2000", VT_DATE, &hr);
	CHECK(hr == S_OK && o.date == 36527);
	o = from_text("2000-01-02 13:30", VT_DATE, &hr);
	CHECK(hr == S_OK && fabs(o.date - (36527 + 13.5 / 24)) < 1e-9);
	o = from_text("Jan 2, 2000", VT_DATE, &hr);
	CHECK(hr == S_OK && o.date == 36527);
	o = from_text("2000 Jan 2", VT_DATE, &hr);
	CHECK(hr == S_OK && o.date == 36527);
	o = from_text("12/29/1899 6:00 AM", VT_DATE, &hr);
	CHECK(hr == S_OK && o.date == -1.25);
	o = from_text("2 January 2000 1:30 PM", VT_DATE, &hr);
	CHECK(hr == S_OK && fabs(o.date - (36527 + 13.5 / 24)) < 1e-9);
	o = from_text("13:30:00", VT_DATE, &hr);
	CHECK(hr == S_OK && fabs(o.date - 0.5625) < 1e-9);
	o = from_text("12:00 AM", VT_DATE, &hr);
	CHECK(hr == S_OK && o.date == 0);
	o = from_text("1/2/99", VT_DATE, &hr);
	CHECK(hr == S_OK && o.date == 36162);	/* 1999 */
	o = from_text("1/2/29", VT_DATE, &hr);
	CHECK(hr == S_OK && o.date == 47120);	/* 2029 */
	from_text("2/30/2000", VT_DATE, &hr);
	CHECK_EQ(hr, DISP_E_TYPEMISMATCH);
	from_text("25:00", VT_DATE, &hr);
	CHECK_EQ(hr, DISP_E_TYPEMISMATCH);
	from_text("5", VT_DATE, &hr);
	CHECK_EQ(hr, DISP_E_TYPEMISMATCH);

	/* Numbers: rounding to even, ranges, VARIANT_TRUE. */
	CHECK(change(&o, VT_R8, (VARIANT){ .dblVal = 2.5 }, VT_I4, 0) == S_OK &&
	      o.lVal == 2);
	CHECK(change(&o, VT_R8, (VARIANT){ .dblVal = -0.5 }, VT_I4, 0) == S_OK &&
	      o.lVal == 0);
	CHECK(change(&o, VT_R8, (VARIANT){ .dblVal = 255.4 }, VT_UI1, 0) ==
	      S_OK && o.bVal == 255);
	CHECK_EQ(change(&o, VT_R8, (VARIANT){ .dblVal = 255.5 }, VT_UI1, 0),
		 DISP_E_OVERFLOW);
	CHECK_EQ(change(&o, VT_R8, (VARIANT){ .dblVal = 1e10 }, VT_I4, 0),
		 DISP_E_OVERFLOW);
	CHECK_EQ(change(&o, VT_R8, (VARIANT){ .dblVal = -1 }, VT_UI4, 0),
		 DISP_E_OVERFLOW);
	CHECK_EQ(change(&o, VT_R8, (VARIANT){ .dblVal = 3.4e39 }, VT_R4, 0),
		 DISP_E_OVERFLOW);
	CHECK_EQ(change(&o, VT_I4, (VARIANT){ .lVal = -1 }, VT_UI2, 0),
		 DISP_E_OVERFLOW);
	CHECK(change(&o, VT_BOOL, (VARIANT){ .boolVal = VARIANT_TRUE }, VT_I4,
		     0) == S_OK && o.lVal == -1);
	CHECK(change(&o, VT_BOOL, (VARIANT){ .boolVal = VARIANT_TRUE }, VT_UI1,
		     0) == S_OK && o.bVal == 255);
	CHECK(change(&o, VT_BOOL, (VARIANT){ .boolVal = VARIANT_TRUE }, VT_UI4,
		     0) == S_OK && o.ulVal == 0xffffffffu);
	CHECK(change(&o, VT_BOOL, (VARIANT){ .boolVal = VARIANT_TRUE }, VT_CY,
		     0) == S_OK && o.cyVal.int64 == -10000);
	CHECK(change(&o, VT_I4, (VARIANT){ .lVal = 3 }, VT_BOOL, 0) == S_OK &&
	      o.boolVal == VARIANT_TRUE);
	CHECK(change(&o, VT_CY, (VARIANT){ .cyVal.int64 = 25000 }, VT_I4, 0) ==
	      S_OK && o.lVal == 2);
	CHECK(change(&o, VT_CY, (VARIANT){ .cyVal.int64 = 35000 }, VT_I4, 0) ==
	      S_OK && o.lVal == 4);
	CHECK(change(&o, VT_CY, (VARIANT){ .cyVal.int64 = -25000 }, VT_I4, 0) ==
	      S_OK && o.lVal == -2);
	CHECK(change(&o, VT_R8, (VARIANT){ .dblVal = 1.23456 }, VT_CY, 0) ==
	      S_OK && o.cyVal.int64 == 12346);
	CHECK_EQ(change(&o, VT_I8, (VARIANT){ .llVal = 1LL << 40 }, VT_I4, 0),
		 DISP_E_OVERFLOW);
	CHECK(change(&o, VT_UI8, (VARIANT){ .ullVal = UINT64_MAX }, VT_R8, 0) ==
	      S_OK && o.dblVal == 18446744073709551616.0);
	CHECK_EQ(change(&o, VT_UI8, (VARIANT){ .ullVal = UINT64_MAX }, VT_I8, 0),
		 DISP_E_OVERFLOW);
	CHECK_EQ(change(&o, VT_R8, (VARIANT){ .dblVal = 3e6 }, VT_DATE, 0),
		 DISP_E_OVERFLOW);
	CHECK(change(&o, VT_I4, (VARIANT){ .lVal = 5 }, VT_INT, 0) == S_OK &&
	      o.vt == VT_INT && o.intVal == 5);

	/* Empty and null; errors only to errors. */
	CHECK(change(&o, VT_EMPTY, (VARIANT){ 0 }, VT_I4, 0) == S_OK &&
	      o.lVal == 0);
	CHECK(change(&o, VT_EMPTY, (VARIANT){ 0 }, VT_NULL, 0) == S_OK &&
	      o.vt == VT_NULL);
	CHECK_EQ(change(&o, VT_NULL, (VARIANT){ 0 }, VT_I4, 0),
		 DISP_E_TYPEMISMATCH);
	CHECK_EQ(change(&o, VT_I4, (VARIANT){ .lVal = 1 }, VT_NULL, 0),
		 DISP_E_TYPEMISMATCH);
	CHECK_EQ(change(&o, VT_I4, (VARIANT){ .lVal = 1 }, VT_ERROR, 0),
		 DISP_E_TYPEMISMATCH);
	CHECK(change(&o, VT_ERROR, (VARIANT){ .scode = 5 }, VT_ERROR, 0) ==
	      S_OK && o.scode == 5);
	CHECK_EQ(change(&o, VT_I4, (VARIANT){ .lVal = 1 }, 15, 0),
		 DISP_E_BADVARTYPE);

	/* In place, by reference, and through an object's value. */
	VARIANT v = { .vt = VT_BSTR, .bstrVal = SysAllocString(w("5")) };
	CHECK_EQ(VariantChangeType(&v, &v, 0, VT_I4), S_OK);
	CHECK(v.vt == VT_I4 && v.lVal == 5);
	SHORT sh = -9;
	v.vt = VT_I2 | VT_BYREF;
	v.piVal = &sh;
	CHECK_EQ(VariantChangeTypeEx(&o, &v, 0x409, 0, VT_R8), S_OK);
	CHECK(o.vt == VT_R8 && o.dblVal == -9);
	v.vt = VT_I2 | VT_BYREF;
	v.piVal = NULL;
	CHECK_EQ(VariantChangeType(&o, &v, 0, VT_R8), DISP_E_TYPEMISMATCH);
	unk_refs = 1;
	v.vt = VT_DISPATCH;
	v.pdispVal = &fake_disp;
	CHECK_STR(as_text(v, VT_DISPATCH, 0), "7");
	CHECK_EQ(unk_refs, 1);
	CHECK_EQ(change(&o, VT_DISPATCH, v, VT_I4, VARIANT_NOVALUEPROP),
		 DISP_E_TYPEMISMATCH);
	v.vt = VT_UNKNOWN;
	v.punkVal = &fake_unk;
	CHECK(change(&o, VT_UNKNOWN, v, VT_DISPATCH, 0) == S_OK &&
	      o.vt == VT_DISPATCH && o.pdispVal == &fake_disp);
	CHECK_EQ(unk_refs, 2);
	VariantClear(&o);
	CHECK_EQ(unk_refs, 1);
	CHECK_EQ(change(&o, VT_UNKNOWN, v, VT_I4, 0), DISP_E_TYPEMISMATCH);

	/* Arrays: to BSTRs as bytes, and back. */
	v.vt = VT_BSTR;
	v.bstrVal = SysAllocStringByteLen("abc", 3);
	CHECK_EQ(VariantChangeType(&o, &v, 0, VT_ARRAY | VT_UI1), S_OK);
	CHECK(o.vt == (VT_ARRAY | VT_UI1) && o.parray->rgsabound[0].cElements == 3 &&
	      !memcmp(o.parray->pvData, "abc", 3));
	VARIANT back;
	back.vt = VT_EMPTY;
	CHECK_EQ(VariantChangeType(&back, &o, 0, VT_BSTR), S_OK);
	CHECK(back.vt == VT_BSTR && SysStringByteLen(back.bstrVal) == 3 &&
	      !memcmp(back.bstrVal, "abc", 3));
	CHECK_EQ(VariantChangeType(&back, &o, 0, VT_I4), DISP_E_TYPEMISMATCH);
	VariantClear(&back);
	VariantClear(&o);
	VariantClear(&v);
}

/* The Var*From* family, a few of each shape. */
static void test_var_from(void)
{
	FN(HRESULT, VarI4FromStr, OLECHAR *, LCID, ULONG, LONG *);
	FN(HRESULT, VarUI1FromI4, LONG, BYTE *);
	FN(HRESULT, VarI2FromR8, double, SHORT *);
	FN(HRESULT, VarR8FromCy, int64_t, double *);
	FN(HRESULT, VarCyFromR8, double, CY *);
	FN(HRESULT, VarBoolFromI4, LONG, VARIANT_BOOL *);
	FN(HRESULT, VarDateFromStr, OLECHAR *, LCID, ULONG, double *);
	FN(HRESULT, VarBstrFromR8, double, LCID, ULONG, BSTR *);
	FN(HRESULT, VarBstrFromBool, VARIANT_BOOL, LCID, ULONG, BSTR *);
	FN(HRESULT, VarBstrFromDate, double, LCID, ULONG, BSTR *);
	FN(HRESULT, VarBstrFromUI8, uint64_t, LCID, ULONG, BSTR *);
	FN(HRESULT, VarI4FromDisp, IDispatch *, LCID, LONG *);
	FN(HRESULT, VarUI8FromStr, OLECHAR *, LCID, ULONG, uint64_t *);
	FN(HRESULT, VarR4FromR8, double, FLOAT *);
	FN(void, SysFreeString, BSTR);
	LONG l = 0;
	BYTE by = 0;
	SHORT sh = 0;
	double d = 0;
	CY cy;
	VARIANT_BOOL vb = 0;
	BSTR b = NULL;
	uint64_t u = 0;
	FLOAT f = 0;

	CHECK(VarI4FromStr(w("123"), 0, 0, &l) == S_OK && l == 123);
	CHECK_EQ(VarUI1FromI4(256, &by), DISP_E_OVERFLOW);
	CHECK(VarUI1FromI4(255, &by) == S_OK && by == 255);
	CHECK(VarI2FromR8(-32768.4, &sh) == S_OK && sh == -32768);
	CHECK(VarR8FromCy(15000, &d) == S_OK && d == 1.5);
	CHECK(VarCyFromR8(-1.23456, &cy) == S_OK && cy.int64 == -12346);
	CHECK(VarBoolFromI4(0, &vb) == S_OK && vb == VARIANT_FALSE);
	CHECK(VarDateFromStr(w("1/2/2000 6:00 AM"), 0, 0, &d) == S_OK &&
	      d == 36527.25);
	CHECK(VarDateFromStr(w("1/2/2000 6:00 AM"), 0, VAR_DATEVALUEONLY, &d) ==
	      S_OK && d == 36527);
	CHECK(VarR4FromR8(0.5, &f) == S_OK && f == 0.5f);
	CHECK(VarUI8FromStr(w("&HFFFFFFFFFFFFFFFF"), 0, 0, &u) == S_OK &&
	      u == UINT64_MAX);
	CHECK(VarBstrFromR8(2.5, 0, 0, &b) == S_OK && !strcmp(text(b), "2.5"));
	SysFreeString(b);
	CHECK(VarBstrFromBool(VARIANT_TRUE, 0, 0, &b) == S_OK &&
	      !strcmp(text(b), "True"));
	SysFreeString(b);
	CHECK(VarBstrFromDate(36527.25, 0, VAR_DATEVALUEONLY, &b) == S_OK &&
	      !strcmp(text(b), "1/2/2000"));
	SysFreeString(b);
	CHECK(VarBstrFromDate(36527.25, 0, VAR_TIMEVALUEONLY, &b) == S_OK &&
	      !strcmp(text(b), "6:00:00 AM"));
	SysFreeString(b);
	CHECK(VarBstrFromUI8(UINT64_MAX, 0, 0, &b) == S_OK &&
	      !strcmp(text(b), "18446744073709551615"));
	SysFreeString(b);
	unk_refs = 1;
	CHECK(VarI4FromDisp(&fake_disp, 0, &l) == S_OK && l == 7);
	CHECK_EQ(unk_refs, 1);
	CHECK_EQ(VarI4FromDisp(NULL, 0, &l), DISP_E_TYPEMISMATCH);
}

static void test_dates(void)
{
	FN(INT, SystemTimeToVariantTime, SYSTEMTIME *, double *);
	FN(INT, VariantTimeToSystemTime, double, SYSTEMTIME *);
	FN(INT, DosDateTimeToVariantTime, USHORT, USHORT, double *);
	FN(INT, VariantTimeToDosDateTime, double, USHORT *, USHORT *);
	SYSTEMTIME st = { 2000, 1, 0, 1, 12, 0, 0, 0 };
	double d = 0;
	USHORT dd, dt;

	CHECK(SystemTimeToVariantTime(&st, &d) && d == 36526.5);
	memset(&st, 0, sizeof(st));
	CHECK(VariantTimeToSystemTime(36526.5, &st));
	CHECK(st.wYear == 2000 && st.wMonth == 1 && st.wDay == 1 &&
	      st.wHour == 12 && st.wMinute == 0 && st.wDayOfWeek == 6);
	CHECK(VariantTimeToSystemTime(-1.25, &st));
	CHECK(st.wYear == 1899 && st.wMonth == 12 && st.wDay == 29 &&
	      st.wHour == 6);
	/* To the nearest second: this rounds into the next day. */
	CHECK(VariantTimeToSystemTime(0.9999999, &st));
	CHECK(st.wDay == 31 && st.wHour == 0 && st.wSecond == 0);
	CHECK(!VariantTimeToSystemTime(3e6, &st));

	/* Before 1899-12-30 the fraction still counts forward. */
	st = (SYSTEMTIME){ 1899, 12, 0, 29, 6, 0, 0, 0 };
	CHECK(SystemTimeToVariantTime(&st, &d) && d == -1.25);
	st = (SYSTEMTIME){ 99, 1, 0, 2, 0, 0, 0, 0 };
	CHECK(SystemTimeToVariantTime(&st, &d) && d == 36162);	/* 1999 */
	st.wMonth = 13;
	CHECK(!SystemTimeToVariantTime(&st, &d));
	/* Days past a month's end roll over, as on Windows. */
	st = (SYSTEMTIME){ 2000, 2, 0, 31, 0, 0, 0, 0 };
	CHECK(SystemTimeToVariantTime(&st, &d) && d == 36587);	/* 2 March */

	/* DOS: 1 January 2000, 12:00:00. */
	CHECK(DosDateTimeToVariantTime((20 << 9) | (1 << 5) | 1, 12 << 11, &d) &&
	      d == 36526.5);
	CHECK(VariantTimeToDosDateTime(36526.5, &dd, &dt));
	CHECK(dd == ((20 << 9) | (1 << 5) | 1) && dt == 12 << 11);
	CHECK(!DosDateTimeToVariantTime((20 << 9) | (13 << 5) | 1, 0, &d));
	CHECK(!DosDateTimeToVariantTime((20 << 9) | (1 << 5) | 1, 24 << 11, &d));
	CHECK(!VariantTimeToDosDateTime(1.0, &dd, &dt));	/* 1899 */
}

static void test_safearrays(void)
{
	FN(SAFEARRAY *, SafeArrayCreate, VARTYPE, UINT, SAFEARRAYBOUND *);
	FN(SAFEARRAY *, SafeArrayCreateVector, VARTYPE, LONG, ULONG);
	FN(SAFEARRAY *, SafeArrayCreateEx, VARTYPE, UINT, SAFEARRAYBOUND *,
	   void *);
	FN(HRESULT, SafeArrayDestroy, SAFEARRAY *);
	FN(UINT, SafeArrayGetDim, SAFEARRAY *);
	FN(UINT, SafeArrayGetElemsize, SAFEARRAY *);
	FN(HRESULT, SafeArrayGetLBound, SAFEARRAY *, UINT, LONG *);
	FN(HRESULT, SafeArrayGetUBound, SAFEARRAY *, UINT, LONG *);
	FN(HRESULT, SafeArrayGetVartype, SAFEARRAY *, VARTYPE *);
	FN(HRESULT, SafeArrayPutElement, SAFEARRAY *, LONG *, void *);
	FN(HRESULT, SafeArrayGetElement, SAFEARRAY *, LONG *, void *);
	FN(HRESULT, SafeArrayPtrOfIndex, SAFEARRAY *, LONG *, void **);
	FN(HRESULT, SafeArrayLock, SAFEARRAY *);
	FN(HRESULT, SafeArrayUnlock, SAFEARRAY *);
	FN(HRESULT, SafeArrayAccessData, SAFEARRAY *, void **);
	FN(HRESULT, SafeArrayUnaccessData, SAFEARRAY *);
	FN(HRESULT, SafeArrayCopy, SAFEARRAY *, SAFEARRAY **);
	FN(HRESULT, SafeArrayCopyData, SAFEARRAY *, SAFEARRAY *);
	FN(HRESULT, SafeArrayRedim, SAFEARRAY *, SAFEARRAYBOUND *);
	FN(HRESULT, SafeArrayGetIID, SAFEARRAY *, GUID *);
	FN(HRESULT, SafeArrayAllocDescriptor, UINT, SAFEARRAY **);
	FN(HRESULT, SafeArrayAllocDescriptorEx, VARTYPE, UINT, SAFEARRAY **);
	FN(HRESULT, SafeArrayAllocData, SAFEARRAY *);
	FN(HRESULT, SafeArrayDestroyData, SAFEARRAY *);
	FN(HRESULT, SafeArrayDestroyDescriptor, SAFEARRAY *);
	FN(HRESULT, VariantClear, VARIANTARG *);
	FN(HRESULT, VariantCopy, VARIANTARG *, const VARIANTARG *);
	FN(BSTR, SysAllocString, LPCOLESTR);
	FN(void, SysFreeString, BSTR);

	/* Two dimensions: 1..3 by 0..1, the leftmost index fastest. */
	SAFEARRAYBOUND bounds[2] = { { 3, 1 }, { 2, 0 } };
	SAFEARRAY *psa = SafeArrayCreate(VT_I4, 2, bounds);
	LONG lo, hi, idx[2];
	VARTYPE vt;
	CHECK(psa && low(psa));
	if (!psa)
		return;
	CHECK(SafeArrayGetDim(psa) == 2 && SafeArrayGetElemsize(psa) == 4);
	CHECK(psa->rgsabound[0].cElements == 2 && psa->rgsabound[1].lLbound == 1);
	CHECK(SafeArrayGetLBound(psa, 1, &lo) == S_OK && lo == 1);
	CHECK(SafeArrayGetUBound(psa, 1, &hi) == S_OK && hi == 3);
	CHECK(SafeArrayGetUBound(psa, 2, &hi) == S_OK && hi == 1);
	CHECK_EQ(SafeArrayGetUBound(psa, 3, &hi), DISP_E_BADINDEX);
	CHECK(SafeArrayGetVartype(psa, &vt) == S_OK && vt == VT_I4);
	CHECK(psa->fFeatures & FADF_HAVEVARTYPE);
	LONG v42 = 42, got = 0;
	idx[0] = 2;
	idx[1] = 1;
	CHECK_EQ(SafeArrayPutElement(psa, idx, &v42), S_OK);
	CHECK_EQ(((LONG *)psa->pvData)[(2 - 1) + 1 * 3], 42);
	void *p = NULL;
	CHECK(SafeArrayPtrOfIndex(psa, idx, &p) == S_OK &&
	      p == (LONG *)psa->pvData + 4);
	CHECK(SafeArrayGetElement(psa, idx, &got) == S_OK && got == 42);
	idx[0] = 4;
	CHECK_EQ(SafeArrayGetElement(psa, idx, &got), DISP_E_BADINDEX);
	idx[0] = 0;
	CHECK_EQ(SafeArrayPutElement(psa, idx, &v42), DISP_E_BADINDEX);

	/* Locks. */
	CHECK_EQ(SafeArrayLock(psa), S_OK);
	CHECK_EQ(SafeArrayDestroy(psa), DISP_E_ARRAYISLOCKED);
	CHECK_EQ(SafeArrayUnlock(psa), S_OK);
	CHECK_EQ(SafeArrayUnlock(psa), E_UNEXPECTED);
	CHECK(SafeArrayAccessData(psa, &p) == S_OK && p == psa->pvData &&
	      psa->cLocks == 1);
	CHECK_EQ(SafeArrayUnaccessData(psa), S_OK);

	/* The rightmost dimension grows, keeping what was there. */
	SAFEARRAYBOUND more = { 3, 0 };
	CHECK_EQ(SafeArrayRedim(psa, &more), S_OK);
	idx[0] = 2;
	idx[1] = 1;
	got = 0;
	CHECK(SafeArrayGetElement(psa, idx, &got) == S_OK && got == 42);
	idx[1] = 2;
	CHECK(SafeArrayGetElement(psa, idx, &got) == S_OK && got == 0);
	CHECK_EQ(SafeArrayDestroy(psa), S_OK);
	CHECK_EQ(SafeArrayDestroy(NULL), S_OK);

	/* BSTRs are copied in and out, and freed with the array. */
	SAFEARRAY *bs = SafeArrayCreateVector(VT_BSTR, 0, 2);
	CHECK(bs && (bs->fFeatures & FADF_BSTR) && bs->cbElements == 4);
	BSTR s = SysAllocString(w("one")), out = NULL;
	LONG i0 = 0, i1 = 1;
	CHECK_EQ(SafeArrayPutElement(bs, &i0, s), S_OK);
	CHECK(((BSTR *)bs->pvData)[0] != s);
	CHECK_EQ(SafeArrayPutElement(bs, &i0, s), S_OK);	/* replaces */
	CHECK(SafeArrayGetElement(bs, &i0, &out) == S_OK && out != s &&
	      !strcmp(text(out), "one"));
	SysFreeString(out);
	SAFEARRAY *copy = NULL;
	CHECK_EQ(SafeArrayCopy(bs, &copy), S_OK);
	CHECK(copy && ((BSTR *)copy->pvData)[0] != ((BSTR *)bs->pvData)[0]);
	CHECK(!((BSTR *)copy->pvData)[1]);
	CHECK_STR(text(((BSTR *)copy->pvData)[0]), "one");
	SAFEARRAYBOUND one = { 1, 0 };
	CHECK_EQ(SafeArrayRedim(copy, &one), S_OK);	/* frees nothing held */
	CHECK_EQ(SafeArrayRedim(bs, &one), S_OK);
	CHECK_EQ(SafeArrayGetElement(bs, &i1, &out), DISP_E_BADINDEX);
	SafeArrayDestroy(copy);
	SafeArrayDestroy(bs);
	SysFreeString(s);

	/* VARIANTs, and interfaces with their references. */
	SAFEARRAY *vs = SafeArrayCreateVector(VT_VARIANT, 0, 1);
	VARIANT v = { .vt = VT_BSTR, .bstrVal = SysAllocString(w("var")) }, o;
	CHECK_EQ(SafeArrayPutElement(vs, &i0, &v), S_OK);
	VariantClear(&v);
	memset(&o, 0xcc, sizeof(o));
	CHECK(SafeArrayGetElement(vs, &i0, &o) == S_OK && o.vt == VT_BSTR &&
	      !strcmp(text(o.bstrVal), "var"));
	VariantClear(&o);
	/* A VARIANT holding an array: copied and destroyed with it. */
	v.vt = VT_ARRAY | VT_VARIANT;
	v.parray = vs;
	VARIANT v2 = { .vt = VT_EMPTY };
	CHECK_EQ(VariantCopy(&v2, &v), S_OK);
	CHECK(v2.parray && v2.parray != vs);
	VariantClear(&v2);
	VariantClear(&v);

	static const GUID some = { 0x12345678, 1, 2, { 3, 4, 5, 6, 7, 8, 9, 10 } };
	SAFEARRAY *us = SafeArrayCreateEx(VT_UNKNOWN, 1, &one, (void *)&some);
	GUID g;
	CHECK(us && (us->fFeatures & FADF_HAVEIID) &&
	      SafeArrayGetIID(us, &g) == S_OK && !memcmp(&g, &some, sizeof(g)));
	CHECK(SafeArrayGetVartype(us, &vt) == S_OK && vt == VT_UNKNOWN);
	unk_refs = 1;
	CHECK_EQ(SafeArrayPutElement(us, &i0, &fake_unk), S_OK);
	CHECK_EQ(unk_refs, 2);
	IUnknown *pu = NULL;
	CHECK(SafeArrayGetElement(us, &i0, &pu) == S_OK && pu == &fake_unk &&
	      unk_refs == 3);
	pu->lpVtbl->Release(pu);
	SafeArrayDestroy(us);
	CHECK_EQ(unk_refs, 1);
	SAFEARRAY *ds = SafeArrayCreateVector(VT_DISPATCH, 0, 1);
	CHECK(ds && SafeArrayGetVartype(ds, &vt) == S_OK && vt == VT_DISPATCH);
	SafeArrayDestroy(ds);

	/* Descriptors and data by hand; mismatched copies. */
	SAFEARRAY *h = NULL;
	CHECK_EQ(SafeArrayAllocDescriptor(0, &h), E_INVALIDARG);
	CHECK_EQ(SafeArrayAllocDescriptorEx(VT_R8, 1, &h), S_OK);
	h->rgsabound[0].cElements = 4;
	CHECK(SafeArrayAllocData(h) == S_OK && h->pvData && h->cbElements == 8);
	SAFEARRAY *h2 = SafeArrayCreateVector(VT_R8, 0, 3);
	CHECK_EQ(SafeArrayCopyData(h, h2), E_INVALIDARG);
	CHECK_EQ(SafeArrayDestroyData(h), S_OK);
	CHECK(!h->pvData);
	CHECK_EQ(SafeArrayDestroyDescriptor(h), S_OK);
	SafeArrayDestroy(h2);

	/* A BSTR descriptor made by hand owns its strings, as created ones do. */
	CHECK_EQ(SafeArrayAllocDescriptorEx(VT_BSTR, 1, &h), S_OK);
	CHECK_EQ(h->fFeatures, FADF_HAVEVARTYPE | FADF_BSTR);
	h->rgsabound[0].cElements = 1;
	CHECK_EQ(SafeArrayAllocData(h), S_OK);
	s = SysAllocString(w("hand"));
	CHECK_EQ(SafeArrayPutElement(h, &i0, s), S_OK);
	CHECK(((BSTR *)h->pvData)[0] != s);
	SysFreeString(s);
	CHECK_STR(text(((BSTR *)h->pvData)[0]), "hand");
	/* Copying onto itself keeps what it holds. */
	CHECK_EQ(SafeArrayCopyData(h, h), S_OK);
	CHECK_STR(text(((BSTR *)h->pvData)[0]), "hand");
	/* Released cells are zeroed (a static array keeps its memory). */
	h->fFeatures |= FADF_STATIC;
	CHECK_EQ(SafeArrayDestroyData(h), S_OK);
	CHECK(h->pvData && !((BSTR *)h->pvData)[0]);
	h->fFeatures &= ~FADF_STATIC;
	CHECK_EQ(SafeArrayDestroyData(h), S_OK);
	CHECK_EQ(SafeArrayDestroyDescriptor(h), S_OK);
	CHECK_EQ(SafeArrayAllocDescriptorEx(VT_VARIANT, 1, &h), S_OK);
	CHECK_EQ(h->fFeatures, FADF_HAVEVARTYPE | FADF_VARIANT);
	CHECK_EQ(SafeArrayDestroyDescriptor(h), S_OK);

	/* A variant whose array is locked isn't cleared. */
	v.vt = VT_ARRAY | VT_I4;
	v.parray = SafeArrayCreateVector(VT_I4, 0, 1);
	CHECK_EQ(SafeArrayLock(v.parray), S_OK);
	CHECK_EQ(VariantClear(&v), DISP_E_ARRAYISLOCKED);
	CHECK(v.vt == (VT_ARRAY | VT_I4) && v.parray);
	CHECK_EQ(SafeArrayUnlock(v.parray), S_OK);
	CHECK_EQ(VariantClear(&v), S_OK);
	CHECK_EQ(v.vt, VT_EMPTY);
	CHECK(!SafeArrayCreate(VT_EMPTY, 1, &one));
	CHECK(!SafeArrayCreate(VT_I4, 1, NULL));
}

static void test_errinfo(void)
{
	FN(HRESULT, CreateErrorInfo, ICreateErrorInfo **);
	FN(HRESULT, SetErrorInfo, ULONG, IErrorInfo *);
	FN(HRESULT, GetErrorInfo, ULONG, IErrorInfo **);
	FN(void, SysFreeString, BSTR);
	static const IID iid_errinfo = { 0x1cf2b120, 0x547d, 0x101b,
		{ 0x8e, 0x65, 0x08, 0x00, 0x2b, 0x2b, 0xd1, 0x19 } };
	static const GUID guid = { 1, 2, 3, { 4, 5, 6, 7, 8, 9, 10, 11 } };
	ICreateErrorInfo *ce = NULL;
	IErrorInfo *ei = NULL, *got = NULL;
	BSTR s = NULL;
	GUID g;
	DWORD ctx = 0;

	CHECK(CreateErrorInfo(&ce) == S_OK && ce);
	if (!ce)
		return;
	CHECK_EQ(ce->lpVtbl->SetDescription(ce, w("it broke")), S_OK);
	CHECK_EQ(ce->lpVtbl->SetSource(ce, w("codec")), S_OK);
	CHECK_EQ(ce->lpVtbl->SetGUID(ce, &guid), S_OK);
	CHECK_EQ(ce->lpVtbl->SetHelpContext(ce, 5), S_OK);
	CHECK_EQ(ce->lpVtbl->QueryInterface(ce, &iid_errinfo, (void **)&ei),
		 S_OK);
	CHECK_EQ(SetErrorInfo(0, ei), S_OK);
	ei->lpVtbl->Release(ei);
	ce->lpVtbl->Release(ce);

	/* Handed over once, through ole32 as through oleaut32. */
	struct module *ole = mod_load("ole32.dll", NULL);
	HRESULT (WINAPI *ole_get)(ULONG, IErrorInfo **) =
		ole ? mod_get_proc(ole, "GetErrorInfo") : NULL;
	CHECK(ole_get == (void *)GetErrorInfo);
	CHECK(GetErrorInfo(0, &got) == S_OK && got);
	if (got) {
		CHECK(got->lpVtbl->GetDescription(got, &s) == S_OK &&
		      !strcmp(text(s), "it broke"));
		SysFreeString(s);
		CHECK(got->lpVtbl->GetSource(got, &s) == S_OK &&
		      !strcmp(text(s), "codec"));
		SysFreeString(s);
		CHECK(got->lpVtbl->GetHelpFile(got, &s) == S_OK && !s);
		CHECK(got->lpVtbl->GetGUID(got, &g) == S_OK &&
		      !memcmp(&g, &guid, sizeof(g)));
		CHECK(got->lpVtbl->GetHelpContext(got, &ctx) == S_OK && ctx == 5);
		CHECK_EQ(got->lpVtbl->Release(got), 0);
	}
	CHECK(GetErrorInfo(0, &got) == S_FALSE && !got);
	CHECK_EQ(SetErrorInfo(1, NULL), E_INVALIDARG);
	mod_free(ole);
}

static void test_misc(void)
{
	FN(HRESULT, DispGetParam, DISPPARAMS *, UINT, VARTYPE, VARIANT *,
	   UINT *);
	FN(HRESULT, LoadTypeLib, LPCOLESTR, void **);
	FN(HRESULT, LoadRegTypeLib, REFGUID, WORD, WORD, LCID, void **);
	FN(HRESULT, RegisterTypeLib, void *, LPCOLESTR, LPCOLESTR);
	FN(HRESULT, DllGetClassObject, REFCLSID, REFIID, void **);
	FN(HRESULT, OleTranslateColor, OLE_COLOR, HPALETTE, COLORREF *);
	FN(HRESULT, OleCreatePropertyFrame, HWND, UINT, UINT, LPCOLESTR, ULONG,
	   IUnknown **, ULONG, CLSID *, LCID, DWORD, void *);
	FN(HRESULT, OleCreateFontIndirect, void *, REFIID, void **);
	FN(HRESULT, VariantClear, VARIANTARG *);
	FN(BSTR, SysAllocString, LPCOLESTR);

	/* Named first, then the positional ones in reverse. */
	VARIANT args[3] = {
		{ .vt = VT_BOOL, .boolVal = VARIANT_TRUE },
		{ .vt = VT_I4, .lVal = 5 },
		{ .vt = VT_BSTR, .bstrVal = SysAllocString(w("7")) },
	};
	DISPID named = 10;
	DISPPARAMS dp = { args, &named, 3, 1 };
	VARIANT r;
	UINT err = 99;
	CHECK(DispGetParam(&dp, 0, VT_I4, &r, &err) == S_OK && r.lVal == 7);
	CHECK(DispGetParam(&dp, 1, VT_R8, &r, &err) == S_OK && r.dblVal == 5);
	CHECK(DispGetParam(&dp, 10, VT_I2, &r, &err) == S_OK && r.iVal == -1);
	CHECK_EQ(DispGetParam(&dp, 2, VT_I4, &r, &err), DISP_E_PARAMNOTFOUND);
	CHECK_EQ(DispGetParam(&dp, 0, VT_DISPATCH, &r, &err),
		 DISP_E_TYPEMISMATCH);
	CHECK_EQ(err, 2);
	VariantClear(&args[2]);

	void *lib = (void *)1;
	CHECK_EQ(LoadTypeLib(w("C:\\x.tlb"), &lib), TYPE_E_CANTLOADLIBRARY);
	CHECK(!lib);
	static const GUID g0;
	CHECK_EQ(LoadRegTypeLib(&g0, 1, 0, 0, &lib), TYPE_E_LIBNOTREGISTERED);
	CHECK_EQ(RegisterTypeLib(NULL, w("x"), NULL), E_INVALIDARG);
	CHECK_EQ(DllGetClassObject(&g0, &g0, &lib), CLASS_E_CLASSNOTAVAILABLE);

	struct module *u32 = mod_load("user32.dll", NULL);
	DWORD (WINAPI *sys_color)(int) = u32 ? mod_get_proc(u32, "GetSysColor") :
					  NULL;
	COLORREF c = 0;
	CHECK(sys_color && OleTranslateColor(0x80000005, NULL, &c) == S_OK &&
	      c == sys_color(5));
	CHECK(OleTranslateColor(0x00123456, NULL, &c) == S_OK && c == 0x123456);
	CHECK_EQ(OleTranslateColor(0x80000040, NULL, &c), E_INVALIDARG);
	CHECK_EQ(OleTranslateColor(0x03000000, NULL, &c), E_INVALIDARG);
	CHECK_EQ(OleTranslateColor(0x00123456, NULL, NULL), S_OK);
	mod_free(u32);

	CHECK_EQ(OleCreatePropertyFrame(NULL, 0, 0, w("Codec settings"), 0, NULL,
					1, NULL, 0, 0, NULL), S_OK);
	void *font = (void *)1;
	CHECK_EQ(OleCreateFontIndirect(NULL, &g0, &font), E_NOTIMPL);
	CHECK(!font);
}

/* The ordinals MSVC-built code imports by, as Windows numbers them. */
static void test_ordinals(void)
{
	static const struct {
		WORD ord;
		const char *name;
	} known[] = {
		{ 2, "SysAllocString" }, { 6, "SysFreeString" },
		{ 7, "SysStringLen" }, { 8, "VariantInit" },
		{ 9, "VariantClear" }, { 10, "VariantCopy" },
		{ 12, "VariantChangeType" }, { 15, "SafeArrayCreate" },
		{ 16, "SafeArrayDestroy" }, { 23, "SafeArrayAccessData" },
		{ 25, "SafeArrayGetElement" }, { 26, "SafeArrayPutElement" },
		{ 64, "VarI4FromStr" }, { 116, "VarBstrFromBool" },
		{ 147, "VariantChangeTypeEx" }, { 148, "SafeArrayPtrOfIndex" },
		{ 149, "SysStringByteLen" }, { 150, "SysAllocStringByteLen" },
		{ 161, "LoadTypeLib" }, { 184, "SystemTimeToVariantTime" },
		{ 185, "VariantTimeToSystemTime" }, { 200, "GetErrorInfo" },
		{ 201, "SetErrorInfo" }, { 202, "CreateErrorInfo" },
		{ 277, "VarUI4FromStr" }, { 313, "VarBstrCat" },
		{ 411, "SafeArrayCreateVector" }, { 417, "OleCreatePropertyFrame" },
		{ 421, "OleTranslateColor" }, { 440, "VarUI8FromUI4" },
	};

	for (size_t i = 0; i < sizeof(known) / sizeof(known[0]); i++) {
		void *by_ord = mod_get_ordinal(oa, known[i].ord);
		void *by_name = mod_get_proc(oa, known[i].name);
		if (!by_name || by_ord != by_name)
			fprintf(stderr, "  ordinal %u: %s\n", known[i].ord,
				known[i].name);
		CHECK(by_name && by_ord == by_name);
	}
	CHECK(mod_get_ordinal(oa, 1) == NULL);
}

/* pe_oleaut.dll, which imports oleaut32 by ordinal as MSVC code does. */
static void test_fixture(void)
{
	char path[4096];
	const char *dir = getenv("VFW_FIXTURES");

	snprintf(path, sizeof(path), "%s/pe_oleaut.dll", dir ? dir : ".");
	struct module *m = mod_load(path, NULL);
	if (!m) {
		printf("pe_oleaut.dll not built: skipping\n");
		return;
	}
	int (WINAPI *run)(void) = mod_get_proc(m, "oleaut_run");
	CHECK(run != NULL);
	if (run) {
		int r = run();
		if (r)
			fprintf(stderr, "  oleaut_run failed at check %d\n", r);
		CHECK_EQ(r, 0);
	}
	mod_free(m);
}

int main(void)
{
	CHECK_EQ(runtime_enter(), 0);
	oa = mod_load("oleaut32.dll", NULL);
	CHECK(oa && oa->builtin && !oa->builtin->fallback);
	if (!oa)
		return test_summary("test_oleaut32");

	test_bstr();
	test_variants();
	test_conversions();
	test_var_from();
	test_dates();
	test_safearrays();
	test_errinfo();
	test_misc();
	test_ordinals();
	test_fixture();

	mod_free(oa);
	return test_summary("test_oleaut32");
}
