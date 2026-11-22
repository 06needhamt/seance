/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * oleaut32's calling through type information without a library:
 * DispCallFunc on functions and vtable slots of each argument and result
 * type (stdcall and cdecl on i386), then an object described by
 * CreateDispTypeInfo and called through CreateStdDispatch's IDispatch:
 * names, conversions, VT_BYREF arguments and their write-back, property
 * gets, more arguments than registers, and the errors.
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

static HRESULT (WINAPI *pDispCallFunc)(void *, ULONG_PTR, CALLCONV, VARTYPE,
				       UINT, VARTYPE *, VARIANTARG **,
				       VARIANT *);
static HRESULT (WINAPI *pCreateDispTypeInfo)(INTERFACEDATA *, LCID,
					     ITypeInfo **);
static HRESULT (WINAPI *pCreateStdDispatch)(IUnknown *, void *, ITypeInfo *,
					    IUnknown **);
static HRESULT (WINAPI *pDispGetIDsOfNames)(ITypeInfo *, OLECHAR **, UINT,
					    DISPID *);
static HRESULT (WINAPI *pDispInvoke)(void *, ITypeInfo *, DISPID, WORD,
				     DISPPARAMS *, VARIANT *, EXCEPINFO *,
				     UINT *);
static BSTR (WINAPI *pSysAllocString)(LPCOLESTR);
static void (WINAPI *pSysFreeString)(BSTR);
static UINT (WINAPI *pSysStringLen)(BSTR);
static HRESULT (WINAPI *pVariantClear)(VARIANT *);

static const IID iid_idispatch = { 0x00020400, 0, 0,
				   { 0xc0, 0, 0, 0, 0, 0, 0, 0x46 } };
static const IID iid_null;
static const IID iid_other = { 0x5eace0ff, 0, 0, { 0 } };

static WCHAR *w(const char *s)
{
	static WCHAR bufs[16][64];
	static int n;
	WCHAR *b = bufs[n++ % 16];
	size_t i;

	for (i = 0; s[i] && i < 63; i++)
		b[i] = (BYTE)s[i];
	b[i] = 0;
	return b;
}

/* ----------------------------------------------------------------------
 * DispCallFunc.
 */

static int WINAPI f_sub(int a, int b)
{
	return a - b;
}

static int CDECL f_sub_cdecl(int a, int b)
{
	return a - b;
}

static LONGLONG WINAPI f_i8(LONGLONG a, short b, BYTE c)
{
	return a * 2 + b + c;
}

static double WINAPI f_r8(double a, float b, int c, double d, float e)
{
	return a + b + c + d + e;
}

static float WINAPI f_r4(float a, float b)
{
	return a * b;
}

static BYTE WINAPI f_ui1(void)
{
	return 0xfe;
}

static VARIANT_BOOL WINAPI f_bool(VARIANT_BOOL b)
{
	return b ? 0 : -1;
}

/* A VARIANT by value: its type and value. */
static LONG WINAPI f_variant(VARIANT v, int k)
{
	return v.vt == VT_I4 ? v.lVal * k : -1;
}

/* Returning a VARIANT, through the hidden pointer. */
static VARIANT WINAPI f_ret_variant(int a)
{
	VARIANT v;

	memset(&v, 0, sizeof(v));
	v.vt = VT_I4;
	v.lVal = a + 100;
	return v;
}

static void WINAPI f_byref(LONG *p, BSTR *b)
{
	*p *= 3;
	*b = pSysAllocString(w("set"));
}

/* An object: a vtable whose slot 1 takes the object first. */
struct obj {
	const void *const *vtbl;
	int base;
};

static int WINAPI o_dummy(struct obj *o)
{
	return -1;
}

static int WINAPI o_add(struct obj *o, int x)
{
	return o->base + x;
}

/*
 * A method returning a VARIANT, as MSVC compiles one: the object, then the
 * hidden pointer to the result (a C function gets the pointer first).
 */
static DECIMAL WINAPI f_ret_decimal(int a)
{
	DECIMAL d;

	memset(&d, 0, sizeof(d));
	d.scale = 2;
	d.Lo64 = (ULONGLONG)a * 3;
	return d;
}

static VARIANT *WINAPI o_ret_variant(struct obj *o, VARIANT *v, int x)
{
	memset(v, 0, sizeof(*v));
	v->vt = VT_I2;
	v->iVal = (SHORT)(o->base * x);
	return v;
}

static const void *const obj_vtbl[] = {
	(void *)o_dummy, (void *)o_add, (void *)o_ret_variant,
};

static HRESULT call(void *inst, ULONG_PTR off, CALLCONV cc, VARTYPE ret,
		    UINT n, VARTYPE *vt, VARIANT *args, VARIANT *res)
{
	VARIANTARG *p[8];

	for (UINT i = 0; i < n; i++) {
		p[i] = &args[i];
		args[i].vt = vt[i];
	}
	if (res)
		memset(res, 0x55, sizeof(*res));
	return pDispCallFunc(inst, off, cc, ret, n, vt, p, res);
}

static void test_dispcallfunc(void)
{
	VARIANT a[8], r;
	VARTYPE vt[8];

	memset(a, 0, sizeof(a));
	vt[0] = VT_I4;
	vt[1] = VT_I4;
	a[0].lVal = 10;
	a[1].lVal = 3;
	CHECK_EQ(call(NULL, (ULONG_PTR)f_sub, CC_STDCALL, VT_I4, 2, vt, a, &r),
		 S_OK);
	CHECK_EQ(r.vt, VT_I4);
	CHECK_EQ(r.lVal, 7);
	/* Called a second time: the stack is where it was. */
	CHECK_EQ(call(NULL, (ULONG_PTR)f_sub_cdecl, CC_CDECL, VT_I4, 2, vt, a,
		      &r), S_OK);
	CHECK_EQ(r.lVal, 7);
	CHECK_EQ(call(NULL, (ULONG_PTR)f_sub_cdecl, CC_CDECL, VT_I4, 2, vt, a,
		      &r), S_OK);
	CHECK_EQ(r.lVal, 7);
#if !defined(__x86_64__)
	CHECK_EQ(call(NULL, (ULONG_PTR)f_sub, CC_PASCAL, VT_I4, 2, vt, a, &r),
		 E_INVALIDARG);
#endif

	memset(a, 0, sizeof(a));
	vt[0] = VT_I8;
	vt[1] = VT_I2;
	vt[2] = VT_UI1;
	a[0].llVal = 0x100000000LL;
	a[1].iVal = -5;
	a[2].bVal = 200;
	CHECK_EQ(call(NULL, (ULONG_PTR)f_i8, CC_STDCALL, VT_I8, 3, vt, a, &r),
		 S_OK);
	CHECK_EQ(r.vt, VT_I8);
	CHECK(r.llVal == 0x200000000LL - 5 + 200);

	/* Doubles and floats, past the four register arguments. */
	memset(a, 0, sizeof(a));
	vt[0] = VT_R8;
	vt[1] = VT_R4;
	vt[2] = VT_I4;
	vt[3] = VT_R8;
	vt[4] = VT_R4;
	a[0].dblVal = 1.5;
	a[1].fltVal = 2.25f;
	a[2].lVal = 3;
	a[3].dblVal = 0.125;
	a[4].fltVal = 10.0f;
	CHECK_EQ(call(NULL, (ULONG_PTR)f_r8, CC_STDCALL, VT_R8, 5, vt, a, &r),
		 S_OK);
	CHECK_EQ(r.vt, VT_R8);
	CHECK(r.dblVal == 16.875);
	vt[0] = VT_R4;
	vt[1] = VT_R4;
	a[0].fltVal = 1.5f;
	a[1].fltVal = 4.0f;
	CHECK_EQ(call(NULL, (ULONG_PTR)f_r4, CC_STDCALL, VT_R4, 2, vt, a, &r),
		 S_OK);
	CHECK_EQ(r.vt, VT_R4);
	CHECK(r.fltVal == 6.0f);
	/* A double result, as a date. */
	vt[0] = VT_R8;
	vt[1] = VT_R4;
	vt[2] = VT_I4;
	vt[3] = VT_R8;
	vt[4] = VT_R4;
	a[0].dblVal = 1;
	a[1].fltVal = 1;
	a[2].lVal = 1;
	a[3].dblVal = 1;
	a[4].fltVal = 1;
	CHECK_EQ(call(NULL, (ULONG_PTR)f_r8, CC_STDCALL, VT_DATE, 5, vt, a,
		      &r), S_OK);
	CHECK_EQ(r.vt, VT_DATE);
	CHECK(r.date == 5.0);

	/* A narrow result fills only its own bytes. */
	CHECK_EQ(call(NULL, (ULONG_PTR)f_ui1, CC_STDCALL, VT_UI1, 0, vt, a,
		      &r), S_OK);
	CHECK_EQ(r.vt, VT_UI1);
	CHECK_EQ(r.lVal, 0xfe);
	vt[0] = VT_BOOL;
	a[0].boolVal = 0;
	CHECK_EQ(call(NULL, (ULONG_PTR)f_bool, CC_STDCALL, VT_BOOL, 1, vt, a,
		      &r), S_OK);
	CHECK_EQ(r.vt, VT_BOOL);
	CHECK_EQ(r.boolVal, -1);

	/* A VARIANT argument, and a VARIANT result. */
	memset(a, 0, sizeof(a));
	vt[0] = VT_VARIANT;
	vt[1] = VT_I4;
	a[1].lVal = 3;
	{
		VARIANTARG *p[2] = { &a[0], &a[1] };

		a[0].vt = VT_I4;
		a[0].lVal = 14;
		a[1].vt = VT_I4;
		CHECK_EQ(pDispCallFunc(NULL, (ULONG_PTR)f_variant, CC_STDCALL,
				       VT_I4, 2, vt, p, &r), S_OK);
		CHECK_EQ(r.lVal, 42);
	}
	vt[0] = VT_I4;
	a[0].lVal = 5;
	CHECK_EQ(call(NULL, (ULONG_PTR)f_ret_variant, CC_STDCALL, VT_VARIANT,
		      1, vt, a, &r), S_OK);
	CHECK_EQ(r.vt, VT_I4);
	CHECK_EQ(r.lVal, 105);
	CHECK_EQ(call(NULL, (ULONG_PTR)f_ret_variant, CC_STDCALL, VT_VARIANT,
		      1, vt, a, NULL), E_INVALIDARG);

	/* A DECIMAL: written over the whole VARIANT, then tagged. */
	vt[0] = VT_I4;
	a[0].lVal = 7;
	CHECK_EQ(call(NULL, (ULONG_PTR)f_ret_decimal, CC_STDCALL, VT_DECIMAL,
		      1, vt, a, &r), S_OK);
	CHECK_EQ(r.vt, VT_DECIMAL);
	CHECK_EQ(r.decVal.scale, 2);
	CHECK(r.decVal.Lo64 == 21);

	/* VT_BYREF: the pointer itself. */
	{
		LONG l = 7;
		BSTR b = NULL;

		vt[0] = VT_BYREF | VT_I4;
		vt[1] = VT_BYREF | VT_BSTR;
		a[0].plVal = &l;
		a[1].pbstrVal = &b;
		CHECK_EQ(call(NULL, (ULONG_PTR)f_byref, CC_STDCALL, VT_VOID, 2,
			      vt, a, &r), S_OK);
		CHECK_EQ(r.vt, VT_EMPTY);
		CHECK_EQ(l, 21);
		CHECK(b && pSysStringLen(b) == 3);
		pSysFreeString(b);
	}

	/* An object's vtable slot, the object first. */
	{
		struct obj o = { obj_vtbl, 40 };

		vt[0] = VT_I4;
		a[0].lVal = 2;
		CHECK_EQ(call(&o, sizeof(void *), CC_STDCALL, VT_I4, 1, vt, a,
			      &r), S_OK);
		CHECK_EQ(r.lVal, 42);
		a[0].lVal = 3;
		CHECK_EQ(call(&o, 2 * sizeof(void *), CC_STDCALL, VT_VARIANT, 1,
			      vt, a, &r), S_OK);
		CHECK_EQ(r.vt, VT_I2);
		CHECK_EQ(r.iVal, 120);
		CHECK_EQ(call(&o, 1, CC_STDCALL, VT_I4, 1, vt, a, &r),
			 E_INVALIDARG);
	}
	CHECK_EQ(pDispCallFunc(NULL, (ULONG_PTR)f_sub, CC_STDCALL, VT_I4, 2,
			       NULL, NULL, &r), E_INVALIDARG);
}

/* ----------------------------------------------------------------------
 * CreateDispTypeInfo and CreateStdDispatch.
 */

struct calc {
	const void *const *vtbl;
	LONG value;
	int calls;
};

static double WINAPI c_mul(struct calc *c, LONG x, double y)
{
	c->calls++;
	return x * y;
}

static void WINAPI c_twice(struct calc *c, LONG *v)
{
	c->calls++;
	*v *= 2;
}

static LONG WINAPI c_value(struct calc *c)
{
	return c->value;
}

static float WINAPI c_sum5(struct calc *c, float a, float b, float d,
			   float e, float f)
{
	return a + b + d + e + f;
}

static double WINAPI c_mix(struct calc *c, LONGLONG a, SHORT b, double d,
			   BSTR s, LONG e)
{
	return (double)a + b + d + (s ? pSysStringLen(s) : 100) + e;
}

static void WINAPI c_set(struct calc *c, LONG v)
{
	c->value = v;
}

/* Adds one to a DECIMAL, writing the whole of it as a callee does. */
static void WINAPI c_inc_dec(struct calc *c, DECIMAL *d)
{
	DECIMAL n = *d;

	n.wReserved = 0;
	n.Lo64++;
	*d = n;
}

static const void *const calc_vtbl[] = {
	(void *)c_mul, (void *)c_twice, (void *)c_value, (void *)c_sum5,
	(void *)c_mix, (void *)c_set, (void *)c_inc_dec,
};

enum { ID_MUL = 1, ID_TWICE, ID_VALUE, ID_SUM5, ID_MIX, ID_INCDEC };

static PARAMDATA mul_params[] = {
	{ NULL, VT_I4 }, { NULL, VT_R8 },
};
static PARAMDATA twice_params[] = { { NULL, VT_I4 | VT_BYREF } };
static PARAMDATA sum5_params[] = {
	{ NULL, VT_R4 }, { NULL, VT_R4 }, { NULL, VT_R4 }, { NULL, VT_R4 },
	{ NULL, VT_R4 },
};
static PARAMDATA mix_params[] = {
	{ NULL, VT_I8 }, { NULL, VT_I2 }, { NULL, VT_R8 }, { NULL, VT_BSTR },
	{ NULL, VT_I4 },
};
static PARAMDATA set_params[] = { { NULL, VT_I4 } };
static PARAMDATA incdec_params[] = { { NULL, VT_DECIMAL | VT_BYREF } };

static HRESULT invoke(IDispatch *d, DISPID id, WORD flags, UINT n,
		      VARIANT *args, VARIANT *res, UINT *err)
{
	DISPPARAMS dp = { args, NULL, n, 0 };
	DISPID put = DISPID_PROPERTYPUT;

	if (flags & DISPATCH_PROPERTYPUT) {
		dp.rgdispidNamedArgs = &put;
		dp.cNamedArgs = 1;
	}
	if (res)
		memset(res, 0, sizeof(*res));
	return d->lpVtbl->Invoke(d, id, &iid_null, 0, flags, &dp, res, NULL,
				 err);
}

static void test_std_dispatch(void)
{
	METHODDATA m[] = {
		{ NULL, mul_params, ID_MUL, 0, CC_STDCALL, 2, DISPATCH_METHOD,
		  VT_R8 },
		{ NULL, twice_params, ID_TWICE, 1, CC_STDCALL, 1,
		  DISPATCH_METHOD, VT_VOID },
		{ NULL, NULL, ID_VALUE, 2, CC_STDCALL, 0, DISPATCH_PROPERTYGET,
		  VT_I4 },
		{ NULL, sum5_params, ID_SUM5, 3, CC_STDCALL, 5, DISPATCH_METHOD,
		  VT_R4 },
		{ NULL, mix_params, ID_MIX, 4, CC_STDCALL, 5, DISPATCH_METHOD,
		  VT_R8 },
		{ NULL, set_params, ID_VALUE, 5, CC_STDCALL, 1,
		  DISPATCH_PROPERTYPUT, VT_VOID },
		{ NULL, incdec_params, ID_INCDEC, 6, CC_STDCALL, 1,
		  DISPATCH_METHOD, VT_VOID },
	};
	INTERFACEDATA idata = { m, sizeof(m) / sizeof(m[0]) };
	struct calc c = { calc_vtbl, 9, 0 };
	ITypeInfo *ti = NULL, *ref = NULL, *got = NULL;
	TYPEATTR *attr;
	IUnknown *unk = NULL;
	IDispatch *d = NULL;
	HREFTYPE h;
	VARIANT a[5], r;
	DISPID ids[2];
	UINT err, count;
	BSTR bs;

	m[0].szName = w("Mul");
	mul_params[0].szName = w("x");
	mul_params[1].szName = w("y");
	m[1].szName = w("Twice");
	m[2].szName = w("Value");
	m[3].szName = w("Sum5");
	m[4].szName = w("Mix");
	m[5].szName = w("Value");
	m[6].szName = w("IncDec");
	CHECK_EQ(pCreateDispTypeInfo(&idata, 0x409, &ti), S_OK);
	if (!ti)
		return;
	CHECK_EQ(ti->lpVtbl->GetTypeAttr(ti, &attr), S_OK);
	CHECK_EQ(attr->typekind, TKIND_COCLASS);
	CHECK_EQ(attr->cImplTypes, 1);
	CHECK_EQ(attr->lcid, 0x409);
	ti->lpVtbl->ReleaseTypeAttr(ti, attr);
	CHECK_EQ(ti->lpVtbl->GetRefTypeOfImplType(ti, 0, &h), S_OK);
	CHECK_EQ(ti->lpVtbl->GetRefTypeInfo(ti, h, &ref), S_OK);
	if (ref) {
		FUNCDESC *fd;

		CHECK_EQ(ref->lpVtbl->GetTypeAttr(ref, &attr), S_OK);
		CHECK_EQ(attr->typekind, TKIND_INTERFACE);
		CHECK_EQ(attr->cFuncs, 7);
		CHECK_EQ(attr->cbSizeVft, 7 * sizeof(void *));
		ref->lpVtbl->ReleaseTypeAttr(ref, attr);
		CHECK_EQ(ref->lpVtbl->GetFuncDesc(ref, 1, &fd), S_OK);
		CHECK_EQ(fd->memid, ID_TWICE);
		CHECK_EQ(fd->funckind, FUNC_VIRTUAL);
		CHECK_EQ(fd->oVft, sizeof(void *));
		CHECK_EQ(fd->cParams, 1);
		CHECK_EQ(fd->lprgelemdescParam[0].tdesc.vt, VT_PTR);
		CHECK_EQ(fd->lprgelemdescParam[0].tdesc.lptdesc->vt, VT_I4);
		ref->lpVtbl->ReleaseFuncDesc(ref, fd);
		ref->lpVtbl->Release(ref);
	}

	{
		OLECHAR *names[2] = { w("mul"), w("Y") };

		CHECK_EQ(pDispGetIDsOfNames(ti, names, 2, ids), S_OK);
		CHECK_EQ(ids[0], ID_MUL);
		CHECK_EQ(ids[1], 1);
		names[0] = w("Nothing");
		CHECK_EQ(pDispGetIDsOfNames(ti, names, 1, ids),
			 DISP_E_UNKNOWNNAME);
	}

	/* DispInvoke straight onto the object. */
	memset(a, 0, sizeof(a));
	a[0].vt = VT_R8;
	a[0].dblVal = 0.5;
	a[1].vt = VT_I4;
	a[1].lVal = 7;
	{
		DISPPARAMS dp = { a, NULL, 2, 0 };

		memset(&r, 0, sizeof(r));
		CHECK_EQ(pDispInvoke(&c, ti, ID_MUL, DISPATCH_METHOD, &dp, &r,
				     NULL, &err), S_OK);
		CHECK_EQ(r.vt, VT_R8);
		CHECK(r.dblVal == 3.5);
	}

	CHECK_EQ(pCreateStdDispatch(NULL, &c, ti, &unk), S_OK);
	if (!unk) {
		ti->lpVtbl->Release(ti);
		return;
	}
	CHECK_EQ(unk->lpVtbl->QueryInterface(unk, &iid_idispatch, (void **)&d),
		 S_OK);
	CHECK_EQ(unk->lpVtbl->QueryInterface(unk, &iid_other, (void **)&got),
		 E_NOINTERFACE);
	CHECK_EQ(d->lpVtbl->GetTypeInfoCount(d, &count), S_OK);
	CHECK_EQ(count, 1);
	CHECK_EQ(d->lpVtbl->GetTypeInfo(d, 0, 0, (void **)&got), S_OK);
	CHECK(got == ti);
	if (got)
		got->lpVtbl->Release(got);
	CHECK_EQ(d->lpVtbl->GetTypeInfo(d, 1, 0, (void **)&got),
		 DISP_E_BADINDEX);
	{
		OLECHAR *names[1] = { w("Twice") };

		CHECK_EQ(d->lpVtbl->GetIDsOfNames(d, &iid_null, names, 1, 0,
						  ids), S_OK);
		CHECK_EQ(ids[0], ID_TWICE);
		CHECK_EQ(d->lpVtbl->GetIDsOfNames(d, &iid_other, names, 1, 0,
						  ids), DISP_E_UNKNOWNINTERFACE);
	}

	/* Converted: a string and a short. */
	bs = pSysAllocString(w("6"));
	a[0].vt = VT_I2;
	a[0].iVal = 4;
	a[1].vt = VT_BSTR;
	a[1].bstrVal = bs;
	CHECK_EQ(invoke(d, ID_MUL, DISPATCH_METHOD, 2, a, &r, &err), S_OK);
	CHECK_EQ(r.vt, VT_R8);
	CHECK(r.dblVal == 24.0);
	CHECK_EQ(a[1].vt, VT_BSTR);		/* the caller's, unchanged */
	CHECK(a[1].bstrVal == bs);
	pSysFreeString(bs);
	bs = pSysAllocString(w("six"));
	a[1].bstrVal = bs;
	err = 99;
	CHECK_EQ(invoke(d, ID_MUL, DISPATCH_METHOD, 2, a, &r, &err),
		 DISP_E_TYPEMISMATCH);
	CHECK_EQ(err, 1);
	pSysFreeString(bs);
	CHECK_EQ(invoke(d, ID_MUL, DISPATCH_METHOD, 1, a, &r, &err),
		 DISP_E_PARAMNOTOPTIONAL);
	CHECK_EQ(invoke(d, ID_MUL, DISPATCH_METHOD, 3, a, &r, &err),
		 DISP_E_BADPARAMCOUNT);
	CHECK_EQ(invoke(d, 77, DISPATCH_METHOD, 0, a, &r, &err),
		 DISP_E_MEMBERNOTFOUND);
	CHECK_EQ(invoke(d, ID_MUL, DISPATCH_PROPERTYGET, 0, a, &r, &err),
		 DISP_E_MEMBERNOTFOUND);
	{
		DISPPARAMS dp = { a, NULL, 0, 0 };

		CHECK_EQ(d->lpVtbl->Invoke(d, ID_MUL, &iid_other, 0,
					   DISPATCH_METHOD, &dp, &r, NULL,
					   &err), DISP_E_UNKNOWNINTERFACE);
	}

	/* VT_BYREF parameters: the caller's own value, written in place. */
	{
		LONG l = 5;
		VARIANT inner;

		a[0].vt = VT_BYREF | VT_I4;
		a[0].plVal = &l;
		CHECK_EQ(invoke(d, ID_TWICE, DISPATCH_METHOD, 1, a, &r, &err),
			 S_OK);
		CHECK_EQ(l, 10);
		CHECK_EQ(r.vt, VT_EMPTY);

		/* In a VARIANT of the caller's, as VBScript passes them. */
		memset(&inner, 0, sizeof(inner));
		inner.vt = VT_I4;
		inner.lVal = 21;
		a[0].vt = VT_BYREF | VT_VARIANT;
		a[0].pvarVal = &inner;
		CHECK_EQ(invoke(d, ID_TWICE, DISPATCH_METHOD, 1, a, &r, &err),
			 S_OK);
		CHECK_EQ(inner.vt, VT_I4);
		CHECK_EQ(inner.lVal, 42);

		/* Of another type: converted, and put back as the type. */
		inner.vt = VT_BSTR;
		inner.bstrVal = pSysAllocString(w("8"));
		CHECK_EQ(invoke(d, ID_TWICE, DISPATCH_METHOD, 1, a, &r, &err),
			 S_OK);
		CHECK_EQ(inner.vt, VT_I4);
		CHECK_EQ(inner.lVal, 16);

		/* A DECIMAL in it: the callee gets the whole DECIMAL, and the
		 * vt it overwrote is put back. */
		memset(&inner, 0, sizeof(inner));
		inner.decVal.scale = 1;
		inner.decVal.Lo64 = 41;
		inner.vt = VT_DECIMAL;
		CHECK_EQ(invoke(d, ID_INCDEC, DISPATCH_METHOD, 1, a, &r, &err),
			 S_OK);
		CHECK_EQ(inner.vt, VT_DECIMAL);
		CHECK_EQ(inner.decVal.scale, 1);
		CHECK(inner.decVal.Lo64 == 42);
		/* (Empty: a zero DECIMAL in a temporary, then put back.) */
		memset(&inner, 0, sizeof(inner));
		CHECK_EQ(invoke(d, ID_INCDEC, DISPATCH_METHOD, 1, a, &r, &err),
			 S_OK);
		CHECK_EQ(inner.vt, VT_DECIMAL);
		CHECK(inner.decVal.Lo64 == 1);
		/* Of another type: converted to a DECIMAL, and put back as one. */
		memset(&inner, 0, sizeof(inner));
		inner.vt = VT_BSTR;
		inner.bstrVal = pSysAllocString(w("-2.5"));
		CHECK_EQ(invoke(d, ID_INCDEC, DISPATCH_METHOD, 1, a, &r, &err),
			 S_OK);
		CHECK_EQ(inner.vt, VT_DECIMAL);
		CHECK_EQ(inner.decVal.scale, 1);
		CHECK_EQ(inner.decVal.sign, 0x80);
		CHECK(inner.decVal.Lo64 == 26);	/* the callee adds 1 to 25 */

		/* Empty, as an [out] one is: a zero. */
		inner.vt = VT_EMPTY;
		CHECK_EQ(invoke(d, ID_TWICE, DISPATCH_METHOD, 1, a, &r, &err),
			 S_OK);
		CHECK_EQ(inner.vt, VT_I4);
		CHECK_EQ(inner.lVal, 0);

		/* A pointer to another type can't be passed. */
		{
			SHORT sh = 3;

			a[0].vt = VT_BYREF | VT_I2;
			a[0].piVal = &sh;
			err = 99;
			CHECK_EQ(invoke(d, ID_TWICE, DISPATCH_METHOD, 1, a, &r,
					&err), DISP_E_TYPEMISMATCH);
			CHECK_EQ(err, 0);
			CHECK_EQ(sh, 3);
		}
	}

	/* A property get, a result that isn't an HRESULT, and a put. */
	CHECK_EQ(invoke(d, ID_VALUE, DISPATCH_PROPERTYGET, 0, a, &r, &err),
		 S_OK);
	CHECK_EQ(r.vt, VT_I4);
	CHECK_EQ(r.lVal, 9);
	a[0].vt = VT_I4;
	a[0].lVal = 31;
	CHECK_EQ(invoke(d, ID_VALUE, DISPATCH_PROPERTYPUT, 1, a, NULL, &err),
		 S_OK);
	CHECK_EQ(c.value, 31);
	CHECK_EQ(invoke(d, ID_VALUE, DISPATCH_METHOD | DISPATCH_PROPERTYGET,
			0, a, &r, &err), S_OK);
	CHECK_EQ(r.lVal, 31);

	/* Five floats: four in registers, one on the stack (x86-64). */
	for (int i = 0; i < 5; i++) {
		a[i].vt = VT_R4;
		a[i].fltVal = (float)(1 << i);	/* reversed: 16, 8, 4, 2, 1 */
	}
	CHECK_EQ(invoke(d, ID_SUM5, DISPATCH_METHOD, 5, a, &r, &err), S_OK);
	CHECK_EQ(r.vt, VT_R4);
	CHECK(r.fltVal == 31.0f);

	/* Eight-byte arguments among four-byte ones. */
	bs = pSysAllocString(w("abcd"));
	a[4].vt = VT_I8;
	a[4].llVal = 1000000;
	a[3].vt = VT_I2;
	a[3].iVal = -3;
	a[2].vt = VT_R8;
	a[2].dblVal = 0.25;
	a[1].vt = VT_BSTR;
	a[1].bstrVal = bs;
	a[0].vt = VT_I4;
	a[0].lVal = 6;
	CHECK_EQ(invoke(d, ID_MIX, DISPATCH_METHOD, 5, a, &r, &err), S_OK);
	CHECK(r.dblVal == 1000000 - 3 + 0.25 + 4 + 6);
	pSysFreeString(bs);

	CHECK_EQ(c.calls, 6);
	d->lpVtbl->Release(d);
	CHECK_EQ(unk->lpVtbl->Release(unk), 0);
	ti->lpVtbl->Release(ti);

	CHECK_EQ(pCreateStdDispatch(NULL, NULL, ti, &unk), E_INVALIDARG);
	CHECK_EQ(pCreateDispTypeInfo(NULL, 0, &ti), E_INVALIDARG);
}

int main(void)
{
	setenv("VFW_REGISTRY", "", 1);
	CHECK_EQ(runtime_enter(), 0);
	reg_reload();
	oa = mod_load("oleaut32.dll", NULL);
	CHECK(oa != NULL);
	if (!oa)
		return test_summary("test_dispatch");
	pDispCallFunc = get("DispCallFunc");
	pCreateDispTypeInfo = get("CreateDispTypeInfo");
	pCreateStdDispatch = get("CreateStdDispatch");
	pDispGetIDsOfNames = get("DispGetIDsOfNames");
	pDispInvoke = get("DispInvoke");
	pSysAllocString = get("SysAllocString");
	pSysFreeString = get("SysFreeString");
	pSysStringLen = get("SysStringLen");
	pVariantClear = get("VariantClear");

	test_dispcallfunc();
	test_std_dispatch();

	mod_free(oa);
	return test_summary("test_dispatch");
}
