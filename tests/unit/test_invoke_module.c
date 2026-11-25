/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * ITypeInfo::Invoke beyond vtables: a module's static functions, found by
 * AddressOfMember through their DLL entries; LPSTR and LPWSTR parameters
 * and results; and records passed by value and by reference, with the
 * IRecordInfo that GetRecordInfoFromTypeInfo gives for them.
 *
 * A library is made with CreateTypeLib2, saved and loaded back. Its
 * modules name real built-in functions:
 *
 *   module Kernel ("kernel32.dll") { int lstrlenA(LPSTR); int lstrlenW(LPWSTR); }
 *   module User ("user32.dll") { BOOL PtInRect(RECTREC *, POINTREC);
 *                                LPSTR CharUpperA(LPSTR); }
 */
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "loader/modules.h"
#include "runtime/runtime.h"
#include "test.h"
#include "win32/codepage.h"
#include "win32/path.h"
#include "win32/reg.h"

static struct module *oa;
static char dir[] = "/tmp/test_invmod.XXXXXX";

static HRESULT (WINAPI *pCreateTypeLib2)(SYSKIND, LPCOLESTR,
					 ICreateTypeLib2 **);
static HRESULT (WINAPI *pLoadTypeLib)(LPCOLESTR, ITypeLib **);
static BSTR (WINAPI *pSysAllocString)(LPCOLESTR);
static void (WINAPI *pSysFreeString)(BSTR);
static HRESULT (WINAPI *pVariantClear)(VARIANT *);
static HRESULT (WINAPI *pGetRecordInfoFromTypeInfo)(ITypeInfo *,
						    IRecordInfo **);

static void *get(const char *name)
{
	void *p = mod_get_proc(oa, name);

	if (!p) {
		fprintf(stderr, "oleaut32 has no %s\n", name);
		exit(1);
	}
	return p;
}

static WCHAR *w(const char *s)
{
	static WCHAR buf[8][128];
	static int n;
	WCHAR *o = buf[n++ % 8];
	size_t i = 0;
	for (; s[i] && i < 127; i++)
		o[i] = (BYTE)s[i];
	o[i] = 0;
	return o;
}

static int is(BSTR b, const char *s)
{
	char *u = b ? utf16_to_utf8_dup(b) : NULL;
	int ok = u && !strcmp(u, s);
	if (!ok)
		fprintf(stderr, "  got \"%s\", want \"%s\"\n",
			u ? u : "(null)", s);
	free(u);
	return ok;
}

static const GUID g_point = { 0x1ec0de01, 1, 1, { 1, 2, 3, 4, 5, 6, 7, 8 } };
static const GUID g_rect = { 0x1ec0de02, 1, 1, { 1, 2, 3, 4, 5, 6, 7, 8 } };
static const GUID g_named = { 0x1ec0de03, 1, 1, { 1, 2, 3, 4, 5, 6, 7, 8 } };

enum { ID_LENA = 1, ID_LENW, ID_PTINRECT, ID_UPPER };

static ICreateTypeInfo *make(ICreateTypeLib2 *ctl, const char *name,
			     TYPEKIND kind, const GUID *g)
{
	ICreateTypeInfo *cti = NULL;
	CHECK_EQ(ctl->lpVtbl->CreateTypeInfo(ctl, w(name), kind, &cti), S_OK);
	if (cti && g)
		CHECK_EQ(cti->lpVtbl->SetGuid(cti, g), S_OK);
	return cti;
}

static void add_var(ICreateTypeInfo *cti, UINT i, VARTYPE vt,
		    const char *name)
{
	VARDESC vd;
	memset(&vd, 0, sizeof(vd));
	vd.memid = (MEMBERID)(0x40000000 + i);
	vd.varkind = VAR_PERINSTANCE;
	vd.elemdescVar.tdesc.vt = vt;
	CHECK_EQ(cti->lpVtbl->AddVarDesc(cti, i, &vd), S_OK);
	CHECK_EQ(cti->lpVtbl->SetVarName(cti, i, w(name)), S_OK);
}

static HREFTYPE ref_to(ICreateTypeInfo *from, ICreateTypeInfo *to)
{
	ITypeInfo *ti = NULL;
	HREFTYPE h = 0;
	CHECK_EQ(to->lpVtbl->QueryInterface(to, &IID_ITypeInfo, (void **)&ti),
		 S_OK);
	if (ti) {
		CHECK_EQ(from->lpVtbl->AddRefTypeInfo(from, ti, &h), S_OK);
		ti->lpVtbl->Release(ti);
	}
	return h;
}

/* A static stdcall function of the module with these parameter types. */
static void add_func(ICreateTypeInfo *m, UINT i, MEMBERID id, VARTYPE ret,
		     UINT n, TYPEDESC *types, const char *dll,
		     const char *entry)
{
	FUNCDESC fd;
	ELEMDESC prm[4];
	memset(&fd, 0, sizeof(fd));
	memset(prm, 0, sizeof(prm));
	for (UINT k = 0; k < n; k++) {
		prm[k].tdesc = types[k];
		prm[k].paramdesc.wParamFlags = PARAMFLAG_FIN;
	}
	fd.memid = id;
	fd.funckind = FUNC_STATIC;
	fd.invkind = INVOKE_FUNC;
	fd.callconv = CC_STDCALL;
	fd.cParams = (SHORT)n;
	fd.lprgelemdescParam = prm;
	fd.elemdescFunc.tdesc.vt = ret;
	CHECK_EQ(m->lpVtbl->AddFuncDesc(m, i, &fd), S_OK);
	CHECK_EQ(m->lpVtbl->DefineFuncAsDllEntry(m, i, w(dll), w(entry)),
		 S_OK);
}

static void build(const WCHAR *path)
{
	ICreateTypeLib2 *ctl = NULL;
	/* Records are laid out for the library's pointers: this process's. */
	CHECK_EQ(pCreateTypeLib2(sizeof(void *) == 8 ? SYS_WIN64 : SYS_WIN32,
				 path, &ctl), S_OK);
	if (!ctl)
		return;
	CHECK_EQ(ctl->lpVtbl->SetName(ctl, w("Mods")), S_OK);

	ICreateTypeInfo *pt = make(ctl, "POINTREC", TKIND_RECORD, &g_point);
	add_var(pt, 0, VT_I4, "x");
	add_var(pt, 1, VT_I4, "y");
	ICreateTypeInfo *rc = make(ctl, "RECTREC", TKIND_RECORD, &g_rect);
	add_var(rc, 0, VT_I4, "left");
	add_var(rc, 1, VT_I4, "top");
	add_var(rc, 2, VT_I4, "right");
	add_var(rc, 3, VT_I4, "bottom");
	ICreateTypeInfo *nm = make(ctl, "NAMED", TKIND_RECORD, &g_named);
	add_var(nm, 0, VT_BSTR, "name");
	add_var(nm, 1, VT_I2, "n");
	CHECK_EQ(pt->lpVtbl->LayOut(pt), S_OK);
	CHECK_EQ(rc->lpVtbl->LayOut(rc), S_OK);
	CHECK_EQ(nm->lpVtbl->LayOut(nm), S_OK);

	ICreateTypeInfo *k = make(ctl, "Kernel", TKIND_MODULE, NULL);
	TYPEDESC t[2];
	memset(t, 0, sizeof(t));
	t[0].vt = VT_LPSTR;
	add_func(k, 0, ID_LENA, VT_INT, 1, t, "kernel32.dll", "lstrlenA");
	t[0].vt = VT_LPWSTR;
	add_func(k, 1, ID_LENW, VT_INT, 1, t, "kernel32.dll", "lstrlenW");

	ICreateTypeInfo *u = make(ctl, "User", TKIND_MODULE, NULL);
	TYPEDESC rect = { .vt = VT_USERDEFINED };
	rect.hreftype = ref_to(u, rc);
	t[0].vt = VT_PTR;
	t[0].lptdesc = &rect;
	t[1].vt = VT_USERDEFINED;
	t[1].hreftype = ref_to(u, pt);
	add_func(u, 0, ID_PTINRECT, VT_INT, 2, t, "user32.dll", "PtInRect");
	memset(t, 0, sizeof(t));
	t[0].vt = VT_LPSTR;
	add_func(u, 1, ID_UPPER, VT_LPSTR, 1, t, "user32.dll", "CharUpperA");

	CHECK_EQ(ctl->lpVtbl->SaveAllChanges(ctl), S_OK);
	pt->lpVtbl->Release(pt);
	rc->lpVtbl->Release(rc);
	nm->lpVtbl->Release(nm);
	k->lpVtbl->Release(k);
	u->lpVtbl->Release(u);
	CHECK_EQ(ctl->lpVtbl->Release(ctl), 0);
}

static ITypeInfo *named(ITypeLib *lib, const char *name)
{
	ITypeInfo *ti = NULL;
	MEMBERID id;
	USHORT found = 1;
	WCHAR *n = w(name);
	CHECK_EQ(lib->lpVtbl->FindName(lib, n, 0, &ti, &id, &found), S_OK);
	CHECK_EQ(found, 1);
	return found ? ti : NULL;
}

static HRESULT call(ITypeInfo *ti, MEMBERID id, UINT n, VARIANT *args,
		    VARIANT *result, UINT *err)
{
	DISPPARAMS dp = { args, NULL, n, 0 };
	*err = (UINT)-1;
	return ti->lpVtbl->Invoke(ti, NULL, id, DISPATCH_METHOD, &dp, result,
				  NULL, err);
}

static void test_strings(ITypeLib *lib)
{
	ITypeInfo *k = named(lib, "Kernel"), *u = named(lib, "User");
	VARIANT arg, res;
	UINT err;
	void *addr = NULL;

	if (!k || !u)
		return;
	CHECK_EQ(k->lpVtbl->AddressOfMember(k, ID_LENA, INVOKE_FUNC, &addr),
		 S_OK);
	CHECK(addr != NULL);
	CHECK_EQ(k->lpVtbl->AddressOfMember(k, 99, INVOKE_FUNC, &addr),
		 TYPE_E_ELEMENTNOTFOUND);

	memset(&arg, 0, sizeof(arg));
	memset(&res, 0, sizeof(res));
	arg.vt = VT_BSTR;
	arg.bstrVal = pSysAllocString(w("h\xe9llo"));
	CHECK_EQ(call(k, ID_LENA, 1, &arg, &res, &err), S_OK);
	CHECK(res.vt == VT_INT && res.intVal == 5);
	CHECK_EQ(call(k, ID_LENW, 1, &arg, &res, &err), S_OK);
	CHECK(res.vt == VT_INT && res.intVal == 5);

	/* Converted to a string first; a NULL BSTR is a NULL pointer. */
	pVariantClear(&arg);
	arg.vt = VT_I4;
	arg.lVal = 12345;
	CHECK_EQ(call(k, ID_LENW, 1, &arg, &res, &err), S_OK);
	CHECK(res.vt == VT_INT && res.intVal == 5);
	arg.vt = VT_BSTR;
	arg.bstrVal = NULL;
	CHECK_EQ(call(k, ID_LENA, 1, &arg, &res, &err), S_OK);
	CHECK(res.vt == VT_INT && res.intVal == 0);

	/* An LPSTR result comes back as a BSTR. */
	arg.bstrVal = pSysAllocString(w("mixed Case"));
	CHECK_EQ(call(u, ID_UPPER, 1, &arg, &res, &err), S_OK);
	CHECK(res.vt == VT_BSTR && is(res.bstrVal, "MIXED CASE"));
	pVariantClear(&res);
	/* (the caller's BSTR is untouched: the callee had a copy) */
	CHECK(is(arg.bstrVal, "mixed Case"));
	pVariantClear(&arg);

	/* No such argument: an error, not a call. */
	arg.vt = VT_DISPATCH;
	arg.pdispVal = NULL;
	CHECK(FAILED(call(k, ID_LENA, 1, &arg, &res, &err)));
	CHECK_EQ(err, 0);
	k->lpVtbl->Release(k);
	u->lpVtbl->Release(u);
}

static void test_records(ITypeLib *lib)
{
	ITypeInfo *pti = named(lib, "POINTREC"), *rti = named(lib, "RECTREC");
	ITypeInfo *nti = named(lib, "NAMED"), *u = named(lib, "User");
	IRecordInfo *pri = NULL, *rri = NULL, *nri = NULL;
	ULONG size = 0;

	if (!pti || !rti || !nti || !u)
		return;
	CHECK_EQ(pGetRecordInfoFromTypeInfo(pti, &pri), S_OK);
	CHECK_EQ(pGetRecordInfoFromTypeInfo(rti, &rri), S_OK);
	CHECK_EQ(pGetRecordInfoFromTypeInfo(nti, &nri), S_OK);
	CHECK_EQ(pGetRecordInfoFromTypeInfo(u, &(IRecordInfo *){ NULL }),
		 E_INVALIDARG);
	if (!pri || !rri || !nri)
		return;

	/* The record info itself. */
	CHECK_EQ(pri->lpVtbl->GetSize(pri, &size), S_OK);
	CHECK_EQ(size, 8);
	GUID g;
	CHECK_EQ(pri->lpVtbl->GetGuid(pri, &g), S_OK);
	CHECK(!memcmp(&g, &g_point, sizeof(g)));
	BSTR name = NULL;
	CHECK_EQ(pri->lpVtbl->GetName(pri, &name), S_OK);
	CHECK(is(name, "POINTREC"));
	pSysFreeString(name);
	ULONG count = 0;
	CHECK_EQ(rri->lpVtbl->GetFieldNames(rri, &count, NULL), S_OK);
	CHECK_EQ(count, 4);
	BSTR names[4] = { 0 };
	CHECK_EQ(rri->lpVtbl->GetFieldNames(rri, &count, names), S_OK);
	CHECK(is(names[2], "right"));
	for (int i = 0; i < 4; i++)
		pSysFreeString(names[i]);
	CHECK(!pri->lpVtbl->IsMatchingType(pri, rri));
	IRecordInfo *again = NULL;
	CHECK_EQ(pGetRecordInfoFromTypeInfo(pti, &again), S_OK);
	if (again) {
		CHECK(pri->lpVtbl->IsMatchingType(pri, again));
		again->lpVtbl->Release(again);
	}

	/* Fields of a record that owns a BSTR: put, get, copy, clear. */
	BYTE *rec = nri->lpVtbl->RecordCreate(nri);
	CHECK(rec != NULL);
	VARIANT v;
	memset(&v, 0, sizeof(v));
	v.vt = VT_BSTR;
	v.bstrVal = pSysAllocString(w("field"));
	CHECK_EQ(nri->lpVtbl->PutField(nri, INVOKE_PROPERTYPUT, rec,
				       w("name"), &v), S_OK);
	pVariantClear(&v);
	v.vt = VT_R8;
	v.dblVal = 41.6;	/* converted, rounded */
	CHECK_EQ(nri->lpVtbl->PutField(nri, INVOKE_PROPERTYPUT, rec, w("n"),
				       &v), S_OK);
	CHECK_EQ(nri->lpVtbl->PutField(nri, INVOKE_PROPERTYPUT, rec,
				       w("nope"), &v), TYPE_E_FIELDNOTFOUND);
	void *copy = NULL;
	CHECK_EQ(nri->lpVtbl->RecordCreateCopy(nri, rec, &copy), S_OK);
	CHECK_EQ(nri->lpVtbl->RecordClear(nri, rec), S_OK);
	memset(&v, 0, sizeof(v));
	CHECK_EQ(nri->lpVtbl->GetField(nri, copy, w("name"), &v), S_OK);
	CHECK(v.vt == VT_BSTR && is(v.bstrVal, "field"));
	pVariantClear(&v);
	CHECK_EQ(nri->lpVtbl->GetField(nri, copy, w("n"), &v), S_OK);
	CHECK(v.vt == VT_I2 && v.iVal == 42);
	CHECK_EQ(nri->lpVtbl->GetFieldNoCopy(nri, copy, w("n"), &v, NULL),
		 S_OK);
	CHECK(v.vt == (VT_BYREF | VT_I2) && *v.piVal == 42);
	CHECK_EQ(nri->lpVtbl->RecordDestroy(nri, copy), S_OK);
	CHECK_EQ(nri->lpVtbl->RecordDestroy(nri, rec), S_OK);

	/* PtInRect(&rect, point): a record by reference, one by value. */
	LONG r[4] = { 10, 20, 30, 40 };
	LONG p[2] = { 15, 25 };
	VARIANT args[2], res;
	UINT err;
	memset(args, 0, sizeof(args));
	memset(&res, 0, sizeof(res));
	/* (in reverse order: the point is args[0]) */
	args[0].vt = VT_RECORD;
	args[0].pvRecord = p;
	args[0].pRecInfo = pri;
	args[1].vt = VT_RECORD | VT_BYREF;
	args[1].pvRecord = r;
	args[1].pRecInfo = rri;
	CHECK_EQ(call(u, ID_PTINRECT, 2, args, &res, &err), S_OK);
	CHECK(res.vt == VT_INT && res.intVal != 0);
	p[0] = 30;	/* the right edge is outside */
	CHECK_EQ(call(u, ID_PTINRECT, 2, args, &res, &err), S_OK);
	CHECK(res.vt == VT_INT && res.intVal == 0);
	/* A record of another type: refused, and which one said. */
	args[0].pRecInfo = rri;
	args[0].pvRecord = r;
	CHECK_EQ(call(u, ID_PTINRECT, 2, args, &res, &err),
		 DISP_E_TYPEMISMATCH);
	CHECK_EQ(err, 0);
	args[0].vt = VT_I4;
	CHECK_EQ(call(u, ID_PTINRECT, 2, args, &res, &err),
		 DISP_E_TYPEMISMATCH);

	pri->lpVtbl->Release(pri);
	rri->lpVtbl->Release(rri);
	nri->lpVtbl->Release(nri);
	pti->lpVtbl->Release(pti);
	rti->lpVtbl->Release(rti);
	nti->lpVtbl->Release(nti);
	u->lpVtbl->Release(u);
}

int main(void)
{
	char cleanup[64], path[512];

	setenv("VFW_REGISTRY", "", 1);
	CHECK_EQ(runtime_enter(), 0);
	reg_reload();
	oa = mod_load("oleaut32.dll", NULL);
	CHECK(oa != NULL);
	if (!oa || !mkdtemp(dir)) {
		CHECK(0);
		return test_summary("test_invoke_module");
	}
	pCreateTypeLib2 = get("CreateTypeLib2");
	pLoadTypeLib = get("LoadTypeLib");
	pSysAllocString = get("SysAllocString");
	pSysFreeString = get("SysFreeString");
	pVariantClear = get("VariantClear");
	pGetRecordInfoFromTypeInfo = get("GetRecordInfoFromTypeInfo");
	CHECK(pGetRecordInfoFromTypeInfo == mod_get_ordinal(oa, 323));
	CHECK(mod_get_proc(oa, "GetRecordInfoFromGuids") ==
	      mod_get_ordinal(oa, 322));

	snprintf(path, sizeof(path), "%s/mods.tlb", dir);
	char *win = path_to_windows(path);
	WCHAR *wpath = utf8_to_utf16_dup(win);
	build(wpath);
	ITypeLib *lib = NULL;
	CHECK_EQ(pLoadTypeLib(wpath, &lib), S_OK);
	free(wpath);
	free(win);
	if (lib) {
		test_strings(lib);
		test_records(lib);
		lib->lpVtbl->Release(lib);
	}

	mod_free(oa);
	snprintf(cleanup, sizeof(cleanup), "rm -rf '%s'", dir);
	if (system(cleanup))
		fprintf(stderr, "could not remove %s\n", dir);
	return test_summary("test_invoke_module");
}
