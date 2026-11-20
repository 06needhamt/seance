/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * A DLL with a type library as its TYPELIB resource (test_typelib's,
 * written by gen_tlb), implementing the library's Thing coclass the two
 * ways automation servers do:
 *
 * - its IDispatch is CreateStdDispatch's, aggregated, over its IThing
 *   vtable (as MFC and hand-written C servers do);
 * - its IDispatch::Invoke calls ITypeInfo::Invoke itself, as Delphi's
 *   TAutoObject and ATL's IDispatchImpl do.
 *
 * typelib_run loads the library by the DLL's path and as path\1, calls
 * through both objects and the coclass, and registers and unregisters
 * the library. It returns 0, or the number of the first check that
 * failed.
 */
#include "fixture.h"
#include <oleauto.h>

static HMODULE self;

static const GUID libid = { 0x5eace001, 0x1111, 0x2222,
			    { 1, 2, 3, 4, 5, 6, 7, 8 } };
static const GUID iid_thing = { 0x5eace002, 0x1111, 0x2222,
				{ 1, 2, 3, 4, 5, 6, 7, 8 } };
static const GUID clsid_thing = { 0x5eace007, 0x1111, 0x2222,
				  { 1, 2, 3, 4, 5, 6, 7, 8 } };
static const GUID iid_unknown = { 0, 0, 0,
				  { 0xc0, 0, 0, 0, 0, 0, 0, 0x46 } };
static const GUID iid_dispatch = { 0x00020400, 0, 0,
				   { 0xc0, 0, 0, 0, 0, 0, 0, 0x46 } };
static const GUID iid_errinfo = { 0x1cf2b120, 0x547d, 0x101b,
	{ 0x8e, 0x65, 0x08, 0x00, 0x2b, 0x2b, 0xd1, 0x19 } };
static const GUID iid_null;

static int guid_eq(const GUID *a, const GUID *b)
{
	const BYTE *x = (const BYTE *)a, *y = (const BYTE *)b;

	for (int i = 0; i < 16; i++)
		if (x[i] != y[i])
			return 0;
	return 1;
}

static int wstr_eq(const WCHAR *a, const char *b)
{
	if (!a)
		return 0;
	while (*b && *a == (BYTE)*b)
		a++, b++;
	return !*b && !*a;
}

static void wstr_set(WCHAR *d, const char *s)
{
	while ((*d++ = (BYTE)*s++))
		;
}

static void zero(void *p, SIZE_T n)
{
	volatile BYTE *b = p;

	while (n--)
		*b++ = 0;
}

/* IThing's vtable: IDispatch's methods, then its own. */
struct thing;
struct thing_vtbl {
	HRESULT (WINAPI *QueryInterface)(struct thing *, REFIID, void **);
	ULONG (WINAPI *AddRef)(struct thing *);
	ULONG (WINAPI *Release)(struct thing *);
	HRESULT (WINAPI *GetTypeInfoCount)(struct thing *, UINT *);
	HRESULT (WINAPI *GetTypeInfo)(struct thing *, UINT, LCID,
				      ITypeInfo **);
	HRESULT (WINAPI *GetIDsOfNames)(struct thing *, REFIID, LPOLESTR *,
					UINT, LCID, DISPID *);
	HRESULT (WINAPI *Invoke)(struct thing *, DISPID, REFIID, LCID, WORD,
				 DISPPARAMS *, VARIANT *, EXCEPINFO *,
				 UINT *);
	HRESULT (WINAPI *Add)(struct thing *, LONG, LONG, LONG *);
	HRESULT (WINAPI *get_Name)(struct thing *, BSTR *);
	HRESULT (WINAPI *put_Name)(struct thing *, BSTR);
	HRESULT (WINAPI *Check)(struct thing *, GUID *);
};

struct thing {
	const struct thing_vtbl *vtbl;
	LONG refs;
	ITypeInfo *ti;
	IUnknown *std;		/* CreateStdDispatch's, if it has one */
	WCHAR name[32];
	int adds;
};

static HRESULT WINAPI t_qi(struct thing *t, REFIID riid, void **ppv)
{
	if (guid_eq(riid, &iid_dispatch) && t->std)
		return t->std->lpVtbl->QueryInterface(t->std, riid, ppv);
	if (guid_eq(riid, &iid_unknown) || guid_eq(riid, &iid_thing) ||
	    guid_eq(riid, &iid_dispatch)) {
		*ppv = t;
		t->refs++;
		return S_OK;
	}
	*ppv = NULL;
	return E_NOINTERFACE;
}

static ULONG WINAPI t_addref(struct thing *t)
{
	return ++t->refs;
}

static ULONG WINAPI t_release(struct thing *t)
{
	return --t->refs;
}

static HRESULT WINAPI t_count(struct thing *t, UINT *n)
{
	*n = 1;
	return S_OK;
}

static HRESULT WINAPI t_info(struct thing *t, UINT i, LCID lcid,
			     ITypeInfo **out)
{
	if (i)
		return DISP_E_BADINDEX;
	t->ti->lpVtbl->AddRef(t->ti);
	*out = t->ti;
	return S_OK;
}

/* Delphi's way: IDispatch from the type info, by hand. */
static HRESULT WINAPI t_ids(struct thing *t, REFIID riid, LPOLESTR *names,
			    UINT n, LCID lcid, DISPID *ids)
{
	return DispGetIDsOfNames(t->ti, names, n, ids);
}

static HRESULT WINAPI t_invoke(struct thing *t, DISPID id, REFIID riid,
			       LCID lcid, WORD flags, DISPPARAMS *dp,
			       VARIANT *res, EXCEPINFO *ex, UINT *err)
{
	if (!guid_eq(riid, &iid_null))
		return DISP_E_UNKNOWNINTERFACE;
	return t->ti->lpVtbl->Invoke(t->ti, t, id, flags, dp, res, ex, err);
}

static HRESULT WINAPI t_add(struct thing *t, LONG a, LONG b, LONG *r)
{
	t->adds++;
	if (a < 0) {
		static const WCHAR neg[] = { 'n', 'e', 'g', 0 };
		ICreateErrorInfo *ce = NULL;
		IErrorInfo *ei = NULL;

		if (CreateErrorInfo(&ce) == S_OK) {
			ce->lpVtbl->SetDescription(ce, (LPOLESTR)neg);
			ce->lpVtbl->QueryInterface(ce, &iid_errinfo,
						   (void **)&ei);
			SetErrorInfo(0, ei);
			if (ei)
				ei->lpVtbl->Release(ei);
			ce->lpVtbl->Release(ce);
		}
		return E_INVALIDARG;
	}
	*r = a + b;
	return S_OK;
}

static HRESULT WINAPI t_get_name(struct thing *t, BSTR *v)
{
	*v = SysAllocString(t->name);
	return *v ? S_OK : E_OUTOFMEMORY;
}

static HRESULT WINAPI t_put_name(struct thing *t, BSTR v)
{
	int i = 0;

	for (; v && v[i] && i < 31; i++)
		t->name[i] = v[i];
	t->name[i] = 0;
	return S_OK;
}

static HRESULT WINAPI t_check(struct thing *t, GUID *g)
{
	return guid_eq(g, &iid_thing) ? S_OK : E_FAIL;
}

static const struct thing_vtbl thing_vtbl = {
	t_qi, t_addref, t_release, t_count, t_info, t_ids, t_invoke,
	t_add, t_get_name, t_put_name, t_check,
};

/* Add(a, b) through an IDispatch; *sum gets the result. */
static HRESULT call_add(IDispatch *d, LONG a, LONG b, int nargs, LONG *sum,
			EXCEPINFO *ex)
{
	VARIANT args[2], r;
	DISPPARAMS dp = { args, NULL, (UINT)nargs, 0 };
	UINT err = 0;
	HRESULT hr;

	zero(args, sizeof(args));
	zero(&r, sizeof(r));
	args[nargs - 1].vt = VT_I4;		/* a, first */
	args[nargs - 1].lVal = a;
	if (nargs == 2) {
		args[0].vt = VT_I4;
		args[0].lVal = b;
	}
	hr = d->lpVtbl->Invoke(d, 1, &iid_null, 0, DISPATCH_METHOD, &dp, &r,
			       ex, &err);
	if (hr == S_OK && r.vt != VT_I4)
		return E_UNEXPECTED;
	*sum = r.lVal;
	return hr;
}

/* What both objects must do through their IDispatch: from check base. */
static int exercise(IDispatch *d, struct thing *t, int base)
{
	static const WCHAR add[] = { 'A', 'D', 'D', 0 };
	static const WCHAR name[] = { 'N', 'a', 'm', 'e', 0 };
	LPOLESTR names[1] = { (LPOLESTR)add };
	DISPID id = 0, put = DISPID_PROPERTYPUT;
	EXCEPINFO ex;
	VARIANT v, r;
	DISPPARAMS dp;
	ITypeInfo *ti = NULL;
	UINT n = 0, err;
	LONG sum = 0;

	if (d->lpVtbl->GetTypeInfoCount(d, &n) != S_OK || n != 1)
		return base + 1;
	if (d->lpVtbl->GetTypeInfo(d, 0, 0, &ti) != S_OK || ti != t->ti)
		return base + 2;
	ti->lpVtbl->Release(ti);
	if (d->lpVtbl->GetIDsOfNames(d, &iid_null, names, 1, 0, &id) != S_OK ||
	    id != 1)
		return base + 3;
	if (call_add(d, 30, 12, 2, &sum, NULL) != S_OK || sum != 42)
		return base + 4;
	if (call_add(d, 5, 0, 1, &sum, NULL) != S_OK || sum != 7)
		return base + 5;		/* b's default is 2 */
	zero(&ex, sizeof(ex));
	if (call_add(d, -1, 0, 2, &sum, &ex) != DISP_E_EXCEPTION ||
	    ex.scode != E_INVALIDARG || !wstr_eq(ex.bstrDescription, "neg"))
		return base + 6;
	SysFreeString(ex.bstrDescription);
	SysFreeString(ex.bstrSource);

	/* The Name property, put and got. */
	names[0] = (LPOLESTR)name;
	if (d->lpVtbl->GetIDsOfNames(d, &iid_null, names, 1, 0, &id) != S_OK ||
	    id != 2)
		return base + 7;
	zero(&v, sizeof(v));
	v.vt = VT_BSTR;
	{
		WCHAR s[8];

		wstr_set(s, "gadget");
		v.bstrVal = SysAllocString(s);
	}
	dp.rgvarg = &v;
	dp.rgdispidNamedArgs = &put;
	dp.cArgs = 1;
	dp.cNamedArgs = 1;
	if (d->lpVtbl->Invoke(d, 2, &iid_null, 0, DISPATCH_PROPERTYPUT, &dp,
			      NULL, NULL, &err) != S_OK ||
	    !wstr_eq(t->name, "gadget"))
		return base + 8;
	VariantClear(&v);
	dp.cArgs = dp.cNamedArgs = 0;
	zero(&r, sizeof(r));
	if (d->lpVtbl->Invoke(d, 2, &iid_null, 0, DISPATCH_PROPERTYGET, &dp,
			      &r, NULL, &err) != S_OK || r.vt != VT_BSTR ||
	    !wstr_eq(r.bstrVal, "gadget"))
		return base + 9;
	VariantClear(&r);
	if (d->lpVtbl->Invoke(d, 2, &iid_dispatch, 0, DISPATCH_PROPERTYGET,
			      &dp, &r, NULL, &err) != DISP_E_UNKNOWNINTERFACE)
		return base + 10;
	if (t->adds != 3)
		return base + 11;
	return 0;
}

EXPORT int typelib_run(void)
{
	WCHAR path[MAX_PATH + 4];
	ITypeLib *lib = NULL, *again = NULL;
	ITypeInfo *ti = NULL, *cls = NULL;
	struct thing a, b;
	IDispatch *d = NULL;
	BSTR reg = NULL;
	DWORD len;
	int r;

	len = GetModuleFileNameW(self, path, MAX_PATH);
	if (!len || len >= MAX_PATH)
		return 1;
	if (LoadTypeLib(path, &lib) != S_OK || !lib)
		return 2;
	/* As path\1: the same resource, the same library. */
	path[len] = '\\';
	path[len + 1] = '1';
	path[len + 2] = 0;
	if (LoadTypeLibEx(path, REGKIND_NONE, &again) != S_OK || again != lib)
		return 3;
	again->lpVtbl->Release(again);
	path[len + 1] = '2';
	again = NULL;
	if (SUCCEEDED(LoadTypeLibEx(path, REGKIND_NONE, &again)) || again)
		return 4;
	path[len] = 0;
	if (lib->lpVtbl->GetTypeInfoOfGuid(lib, &iid_thing, &ti) != S_OK)
		return 5;

	/* CreateStdDispatch's IDispatch, aggregated. */
	zero(&a, sizeof(a));
	a.vtbl = &thing_vtbl;
	a.refs = 1;
	a.ti = ti;
	if (CreateStdDispatch((IUnknown *)&a, &a, ti, &a.std) != S_OK ||
	    !a.std)
		return 6;
	if (a.vtbl->QueryInterface(&a, &iid_dispatch, (void **)&d) != S_OK ||
	    (void *)d == (void *)&a)
		return 7;
	if ((r = exercise(d, &a, 10)))
		return r;
	d->lpVtbl->Release(d);
	a.std->lpVtbl->Release(a.std);

	/* Its own IDispatch, over ITypeInfo::Invoke. */
	zero(&b, sizeof(b));
	b.vtbl = &thing_vtbl;
	b.refs = 1;
	b.ti = ti;
	if (b.vtbl->QueryInterface(&b, &iid_dispatch, (void **)&d) != S_OK ||
	    (void *)d != (void *)&b)
		return 8;
	if ((r = exercise(d, &b, 30)))
		return r;

	/* Through the coclass: its default interface. */
	if (lib->lpVtbl->GetTypeInfoOfGuid(lib, &clsid_thing, &cls) != S_OK)
		return 50;
	{
		VARIANT args[2], res;
		DISPPARAMS dp = { args, NULL, 2, 0 };

		zero(args, sizeof(args));
		zero(&res, sizeof(res));
		args[0].vt = VT_I4;
		args[0].lVal = 1;
		args[1].vt = VT_I4;
		args[1].lVal = 2;
		if (DispInvoke(&b, cls, 1, DISPATCH_METHOD, &dp, &res, NULL,
			       NULL) != S_OK || res.vt != VT_I4 ||
		    res.lVal != 3)
			return 51;
	}
	cls->lpVtbl->Release(cls);

	/* Registered as ATL's DllRegisterServer does, and found again. */
	if (RegisterTypeLib(lib, path, NULL) != S_OK)
		return 60;
	if (QueryPathOfRegTypeLib(&libid, 1, 2, 0x409, &reg) != S_OK ||
	    !reg)
		return 61;
	for (DWORD i = 0; i <= len; i++)
		if (reg[i] != path[i])
			return 62;
	SysFreeString(reg);
	if (UnRegisterTypeLib(&libid, 1, 2, 0x409, SYS_WIN32 +
			      (sizeof(void *) == 8 ? 2 : 0)) != S_OK)
		return 63;
	if (QueryPathOfRegTypeLib(&libid, 1, 2, 0x409, &reg) == S_OK)
		return 64;

	ti->lpVtbl->Release(ti);
	lib->lpVtbl->Release(lib);
	return 0;
}

BOOL WINAPI DllMain(HINSTANCE inst, DWORD reason, LPVOID reserved)
{
	if (reason == DLL_PROCESS_ATTACH)
		self = inst;
	return TRUE;
}
