/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * oleaut32's type library creation: a library made through CreateTypeLib2,
 * ICreateTypeLib2 and ICreateTypeInfo2, saved, and read back through
 * LoadTypeLibEx, for Win32 and for Win64.
 */
#include <errno.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "loader/modules.h"
#include "runtime/runtime.h"
#include "test.h"
#include "win32/codepage.h"
#include "win32/path.h"
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

static HRESULT (WINAPI *pCreateTypeLib2)(SYSKIND, LPCOLESTR,
					 ICreateTypeLib2 **);
static HRESULT (WINAPI *pLoadTypeLib)(LPCOLESTR, ITypeLib **);
static HRESULT (WINAPI *pLoadTypeLibEx)(LPCOLESTR, REGKIND, ITypeLib **);
static void (WINAPI *pSysFreeString)(BSTR);
static BSTR (WINAPI *pSysAllocString)(LPCOLESTR);
static HRESULT (WINAPI *pVariantClear)(VARIANT *);

static char dir[] = "/tmp/test_tlbcreate.XXXXXX";

static const GUID libid = { 0x7e57c0de, 1, 2, { 1, 2, 3, 4, 5, 6, 7, 8 } };
static const GUID iid_foo = { 0x7e57c0de, 3, 4, { 1, 2, 3, 4, 5, 6, 7, 8 } };
static const GUID diid_foo = { 0x7e57c0de, 5, 6, { 1, 2, 3, 4, 5, 6, 7, 8 } };
static const GUID clsid_foo = { 0x7e57c0de, 7, 8, { 1, 2, 3, 4, 5, 6, 7, 8 } };
static const GUID cust_guid = { 0xc057da7a, 1, 1, { 9, 9, 9, 9, 9, 9, 9, 9 } };
static const GUID iid_unknown = { 0, 0, 0,
	{ 0xc0, 0, 0, 0, 0, 0, 0, 0x46 } };
static const GUID iid_dispatch = { 0x00020400, 0, 0,
	{ 0xc0, 0, 0, 0, 0, 0, 0, 0x46 } };

static WCHAR *w(const char *s)
{
	static WCHAR buf[8][256];
	static int n;
	WCHAR *o = buf[n++ % 8];
	size_t i = 0;
	for (; s[i] && i < 255; i++)
		o[i] = (BYTE)s[i];
	o[i] = 0;
	return o;
}

static int is(BSTR b, const char *s)
{
	char *u = b ? utf16_to_utf8_dup(b) : NULL;
	int ok = u && !strcmp(u, s);
	if (!ok)
		fprintf(stderr, "  got \"%s\", want \"%s\"\n", u ? u : "(null)", s);
	free(u);
	return ok;
}

/* The name of the type an href leads to. */
static int ref_name_is(ITypeInfo *ti, HREFTYPE href, const char *want)
{
	ITypeInfo *r = NULL;
	BSTR name = NULL;
	int ok = 0;

	if (ti->lpVtbl->GetRefTypeInfo(ti, href, &r) == S_OK && r) {
		if (r->lpVtbl->GetDocumentation(r, MEMBERID_NIL, &name, NULL,
						NULL, NULL) == S_OK)
			ok = is(name, want);
		pSysFreeString(name);
		r->lpVtbl->Release(r);
	}
	return ok;
}

static ICreateTypeInfo *make(ICreateTypeLib2 *ctl, const char *name,
			     TYPEKIND kind)
{
	ICreateTypeInfo *cti = NULL;
	CHECK_EQ(ctl->lpVtbl->CreateTypeInfo(ctl, w(name), kind, &cti), S_OK);
	return cti;
}

static void build(SYSKIND sys, const WCHAR *path, ITypeLib *std)
{
	ICreateTypeLib2 *ctl = NULL;
	ITypeInfo *unk = NULL, *disp = NULL;
	HREFTYPE h_unk, h_disp, h_foo, h_dfoo, h_colors;
	VARIANT v;

	CHECK_EQ(pCreateTypeLib2(sys, path, &ctl), S_OK);
	CHECK_EQ(ctl->lpVtbl->SetName(ctl, w("TestLib")), S_OK);
	CHECK_EQ(ctl->lpVtbl->SetGuid(ctl, &libid), S_OK);
	CHECK_EQ(ctl->lpVtbl->SetVersion(ctl, 2, 5), S_OK);
	CHECK_EQ(ctl->lpVtbl->SetDocString(ctl, w("A test library")), S_OK);
	CHECK_EQ(ctl->lpVtbl->SetHelpFileName(ctl, w("test.hlp")), S_OK);
	CHECK_EQ(ctl->lpVtbl->SetHelpContext(ctl, 7), S_OK);
	CHECK_EQ(ctl->lpVtbl->SetLcid(ctl, 0x409), S_OK);
	CHECK_EQ(ctl->lpVtbl->SetLibFlags(ctl, LIBFLAG_FHASDISKIMAGE), S_OK);
	CHECK_EQ(ctl->lpVtbl->SetHelpStringDll(ctl, w("help.dll")), S_OK);
	v.vt = VT_I4;
	v.lVal = 42;
	CHECK_EQ(ctl->lpVtbl->SetCustData(ctl, &cust_guid, &v), S_OK);

	CHECK_EQ(std->lpVtbl->GetTypeInfoOfGuid(std, &iid_unknown, &unk), S_OK);
	CHECK_EQ(std->lpVtbl->GetTypeInfoOfGuid(std, &iid_dispatch, &disp),
		 S_OK);

	/* interface IFoo : IUnknown */
	ICreateTypeInfo *foo = make(ctl, "IFoo", TKIND_INTERFACE);
	CHECK_EQ(foo->lpVtbl->SetGuid(foo, &iid_foo), S_OK);
	CHECK_EQ(foo->lpVtbl->SetTypeFlags(foo, TYPEFLAG_FOLEAUTOMATION), S_OK);
	CHECK_EQ(foo->lpVtbl->SetDocString(foo, w("The interface")), S_OK);
	CHECK_EQ(foo->lpVtbl->AddRefTypeInfo(foo, unk, &h_unk), S_OK);
	CHECK_EQ(foo->lpVtbl->AddImplType(foo, 0, h_unk), S_OK);
	CHECK_EQ(foo->lpVtbl->AddImplType(foo, 0, h_unk), TYPE_E_BADMODULEKIND);

	ELEMDESC params[2];
	PARAMDESCEX def;
	FUNCDESC fd;
	memset(params, 0, sizeof(params));
	memset(&fd, 0, sizeof(fd));
	params[0].tdesc.vt = VT_I4;
	params[0].paramdesc.wParamFlags = PARAMFLAG_FIN;
	params[1].tdesc.vt = VT_I4;
	params[1].paramdesc.wParamFlags = PARAMFLAG_FIN | PARAMFLAG_FOPT |
					  PARAMFLAG_FHASDEFAULT;
	def.cBytes = sizeof(def);
	def.varDefaultValue.vt = VT_I4;
	def.varDefaultValue.lVal = 5;
	params[1].paramdesc.pparamdescex = &def;
	fd.memid = 1;
	fd.funckind = FUNC_PUREVIRTUAL;
	fd.invkind = INVOKE_FUNC;
	fd.callconv = CC_STDCALL;
	fd.cParams = 2;
	fd.lprgelemdescParam = params;
	fd.elemdescFunc.tdesc.vt = VT_HRESULT;
	CHECK_EQ(foo->lpVtbl->AddFuncDesc(foo, 0, &fd), S_OK);
	CHECK_EQ(foo->lpVtbl->AddFuncDesc(foo, 0, &fd), TYPE_E_DUPLICATEID);
	LPOLESTR add_names[] = { w("Add"), w("a"), w("b") };
	CHECK_EQ(foo->lpVtbl->SetFuncAndParamNames(foo, 0, add_names, 3), S_OK);
	CHECK_EQ(foo->lpVtbl->SetFuncAndParamNames(foo, 0, add_names, 2),
		 TYPE_E_ELEMENTNOTFOUND);
	CHECK_EQ(foo->lpVtbl->SetFuncDocString(foo, 0, w("Adds")), S_OK);
	CHECK_EQ(foo->lpVtbl->SetFuncHelpContext(foo, 0, 99), S_OK);
	v.vt = VT_I2;
	v.iVal = 3;
	CHECK_EQ(foo->lpVtbl->SetFuncCustData(foo, 0, &cust_guid, &v), S_OK);
	v.vt = VT_BSTR;
	v.bstrVal = pSysAllocString(w("param data"));
	CHECK_EQ(foo->lpVtbl->SetParamCustData(foo, 0, 1, &cust_guid, &v),
		 S_OK);
	pVariantClear(&v);

	/* [propget] HRESULT Name([out, retval] BSTR *); [propput] ... */
	TYPEDESC bstr_ptr = { .vt = VT_BSTR };
	params[0].tdesc.vt = VT_PTR;
	params[0].tdesc.lptdesc = &bstr_ptr;
	params[0].paramdesc.wParamFlags = PARAMFLAG_FOUT | PARAMFLAG_FRETVAL;
	fd.memid = 2;
	fd.invkind = INVOKE_PROPERTYGET;
	fd.cParams = 1;
	CHECK_EQ(foo->lpVtbl->AddFuncDesc(foo, 1, &fd), S_OK);
	params[0].tdesc.vt = VT_BSTR;
	params[0].paramdesc.wParamFlags = PARAMFLAG_FIN;
	fd.invkind = INVOKE_PROPERTYPUT;
	CHECK_EQ(foo->lpVtbl->AddFuncDesc(foo, 2, &fd), S_OK);
	LPOLESTR get_names[] = { w("Name"), w("pVal") };
	LPOLESTR put_names[] = { w("Name") };
	CHECK_EQ(foo->lpVtbl->SetFuncAndParamNames(foo, 1, get_names, 2), S_OK);
	CHECK_EQ(foo->lpVtbl->SetFuncAndParamNames(foo, 2, put_names, 1), S_OK);
	/* Another function can't take the property's name. */
	fd.memid = 3;
	fd.invkind = INVOKE_FUNC;
	fd.cParams = 0;
	CHECK_EQ(foo->lpVtbl->AddFuncDesc(foo, 3, &fd), S_OK);
	CHECK_EQ(foo->lpVtbl->SetFuncAndParamNames(foo, 3, put_names, 1),
		 TYPE_E_AMBIGUOUSNAME);
	CHECK_EQ(foo->lpVtbl->DeleteFuncDesc(foo, 3), S_OK);
	CHECK_EQ(foo->lpVtbl->LayOut(foo), S_OK);

	/* dispinterface DFoo */
	ICreateTypeInfo *dfoo = make(ctl, "DFoo", TKIND_DISPATCH);
	CHECK_EQ(dfoo->lpVtbl->SetGuid(dfoo, &diid_foo), S_OK);
	CHECK_EQ(dfoo->lpVtbl->AddRefTypeInfo(dfoo, disp, &h_disp), S_OK);
	CHECK_EQ(dfoo->lpVtbl->AddImplType(dfoo, 0, h_disp), S_OK);
	memset(&fd, 0, sizeof(fd));
	fd.memid = 10;
	fd.funckind = FUNC_DISPATCH;
	fd.invkind = INVOKE_FUNC;
	fd.callconv = CC_STDCALL;
	fd.elemdescFunc.tdesc.vt = VT_I4;
	CHECK_EQ(dfoo->lpVtbl->AddFuncDesc(dfoo, 0, &fd), S_OK);
	fd.funckind = FUNC_PUREVIRTUAL;
	CHECK_EQ(dfoo->lpVtbl->AddFuncDesc(dfoo, 1, &fd), TYPE_E_BADMODULEKIND);
	LPOLESTR go[] = { w("Go") };
	CHECK_EQ(dfoo->lpVtbl->SetFuncAndParamNames(dfoo, 0, go, 1), S_OK);
	VARDESC vd;
	memset(&vd, 0, sizeof(vd));
	vd.memid = 11;
	vd.varkind = VAR_DISPATCH;
	vd.elemdescVar.tdesc.vt = VT_I4;
	CHECK_EQ(dfoo->lpVtbl->AddVarDesc(dfoo, 0, &vd), S_OK);
	CHECK_EQ(dfoo->lpVtbl->SetVarName(dfoo, 0, w("Count")), S_OK);

	/* enum Colors */
	ICreateTypeInfo *colors = make(ctl, "Colors", TKIND_ENUM);
	static const LONG values[] = { 0, 1, 0x7fffffff, -1 };
	static const char *const names[] = { "Red", "Green", "Big", "Neg" };
	for (int i = 0; i < 4; i++) {
		VARIANT c;
		memset(&vd, 0, sizeof(vd));
		c.vt = VT_I4;
		c.lVal = values[i];
		vd.memid = 0x100 + i;
		vd.varkind = VAR_CONST;
		vd.lpvarValue = &c;
		vd.elemdescVar.tdesc.vt = VT_I4;
		CHECK_EQ(colors->lpVtbl->AddVarDesc(colors, (UINT)i, &vd), S_OK);
		CHECK_EQ(colors->lpVtbl->SetVarName(colors, (UINT)i,
						     w(names[i])), S_OK);
	}
	vd.varkind = VAR_PERINSTANCE;
	CHECK_EQ(colors->lpVtbl->AddVarDesc(colors, 0, &vd),
		 TYPE_E_BADMODULEKIND);

	/* struct Point3 */
	ICreateTypeInfo *pt = make(ctl, "Point3", TKIND_RECORD);
	CHECK_EQ(pt->lpVtbl->AddRefTypeInfo(pt, NULL, &h_colors), E_INVALIDARG);
	ITypeInfo *colors_ti = NULL;
	CHECK_EQ(colors->lpVtbl->QueryInterface(colors, &IID_ITypeInfo,
						(void **)&colors_ti), S_OK);
	CHECK_EQ(pt->lpVtbl->AddRefTypeInfo(pt, colors_ti, &h_colors), S_OK);
	CHECK_EQ(h_colors, 2 * 0x64);	/* its own type: an internal href */
	colors_ti->lpVtbl->Release(colors_ti);
	struct {
		VARTYPE vt;
		const char *name;
	} fields[] = { { VT_I2, "x" }, { VT_I4, "y" }, { VT_R8, "z" },
		       { VT_CARRAY, "tag" }, { VT_USERDEFINED, "color" },
		       { VT_PTR, "next" } };
	ARRAYDESC ad = { .tdescElem = { .vt = VT_UI1 }, .cDims = 1,
			 .rgbounds = { { 3, 0 } } };
	TYPEDESC void_td = { .vt = VT_VOID };
	for (int i = 0; i < 6; i++) {
		memset(&vd, 0, sizeof(vd));
		vd.memid = MEMBERID_NIL;
		vd.varkind = VAR_PERINSTANCE;
		vd.elemdescVar.tdesc.vt = fields[i].vt;
		if (fields[i].vt == VT_CARRAY)
			vd.elemdescVar.tdesc.lpadesc = &ad;
		else if (fields[i].vt == VT_USERDEFINED)
			vd.elemdescVar.tdesc.hreftype = h_colors;
		else if (fields[i].vt == VT_PTR)
			vd.elemdescVar.tdesc.lptdesc = &void_td;
		CHECK_EQ(pt->lpVtbl->AddVarDesc(pt, (UINT)i, &vd), S_OK);
		CHECK_EQ(pt->lpVtbl->SetVarName(pt, (UINT)i, w(fields[i].name)),
			 S_OK);
	}
	vd.elemdescVar.tdesc.vt = VT_USERDEFINED;
	vd.elemdescVar.tdesc.hreftype = 0x7777 * 4;
	CHECK_EQ(pt->lpVtbl->AddVarDesc(pt, 6, &vd), TYPE_E_ELEMENTNOTFOUND);
	CHECK_EQ(pt->lpVtbl->SetVarDocString(pt, 0, w("across")), S_OK);
	CHECK_EQ(pt->lpVtbl->LayOut(pt), S_OK);
	memset(&fd, 0, sizeof(fd));
	fd.funckind = FUNC_PUREVIRTUAL;
	fd.invkind = INVOKE_FUNC;
	CHECK_EQ(pt->lpVtbl->AddFuncDesc(pt, 0, &fd), TYPE_E_BADMODULEKIND);

	/* typedef IFoo *PFoo */
	ICreateTypeInfo *pfoo = make(ctl, "PFoo", TKIND_ALIAS);
	ITypeInfo *foo_ti = NULL;
	CHECK_EQ(foo->lpVtbl->QueryInterface(foo, &IID_ITypeInfo,
					     (void **)&foo_ti), S_OK);
	CHECK_EQ(pfoo->lpVtbl->AddRefTypeInfo(pfoo, foo_ti, &h_foo), S_OK);
	CHECK_EQ(h_foo, 0);
	TYPEDESC foo_td = { .hreftype = h_foo, .vt = VT_USERDEFINED };
	TYPEDESC alias = { .lptdesc = &foo_td, .vt = VT_PTR };
	CHECK_EQ(pfoo->lpVtbl->SetTypeDescAlias(pfoo, &alias), S_OK);
	CHECK_EQ(foo->lpVtbl->SetTypeDescAlias(foo, &alias),
		 TYPE_E_WRONGTYPEKIND);

	/* module Funcs */
	ICreateTypeInfo *mod = make(ctl, "Funcs", TKIND_MODULE);
	memset(&fd, 0, sizeof(fd));
	fd.memid = 0x60000000;
	fd.funckind = FUNC_STATIC;
	fd.invkind = INVOKE_FUNC;
	fd.callconv = CC_STDCALL;
	fd.elemdescFunc.tdesc.vt = VT_UI4;
	CHECK_EQ(mod->lpVtbl->AddFuncDesc(mod, 0, &fd), S_OK);
	fd.memid = 0x60000001;
	CHECK_EQ(mod->lpVtbl->AddFuncDesc(mod, 1, &fd), S_OK);
	LPOLESTR tick[] = { w("Tick") }, by_ord[] = { w("ByOrdinal") };
	CHECK_EQ(mod->lpVtbl->SetFuncAndParamNames(mod, 0, tick, 1), S_OK);
	CHECK_EQ(mod->lpVtbl->SetFuncAndParamNames(mod, 1, by_ord, 1), S_OK);
	CHECK_EQ(mod->lpVtbl->DefineFuncAsDllEntry(mod, 0, w("kernel32.dll"),
						   w("GetTickCount")), S_OK);
	CHECK_EQ(mod->lpVtbl->DefineFuncAsDllEntry(mod, 1, w("kernel32.dll"),
						   (LPOLESTR)(ULONG_PTR)5),
		 S_OK);
	CHECK_EQ(mod->lpVtbl->DefineFuncAsDllEntry(mod, 1, w("other.dll"),
						   w("x")), E_INVALIDARG);
	VARIANT hello;
	hello.vt = VT_BSTR;
	hello.bstrVal = pSysAllocString(w("hello"));
	memset(&vd, 0, sizeof(vd));
	vd.memid = 0x60000010;
	vd.varkind = VAR_CONST;
	vd.lpvarValue = &hello;
	vd.elemdescVar.tdesc.vt = VT_BSTR;
	CHECK_EQ(mod->lpVtbl->AddVarDesc(mod, 0, &vd), S_OK);
	CHECK_EQ(mod->lpVtbl->SetVarName(mod, 0, w("Greeting")), S_OK);
	pVariantClear(&hello);

	/* coclass Foo { [default] interface IFoo; [source] dispinterface DFoo; } */
	ICreateTypeInfo *co = make(ctl, "Foo", TKIND_COCLASS);
	CHECK_EQ(co->lpVtbl->SetGuid(co, &clsid_foo), S_OK);
	CHECK_EQ(co->lpVtbl->SetTypeFlags(co, TYPEFLAG_FCANCREATE), S_OK);
	ITypeInfo *dfoo_ti = NULL;
	CHECK_EQ(dfoo->lpVtbl->QueryInterface(dfoo, &IID_ITypeInfo2,
					      (void **)&dfoo_ti), S_OK);
	CHECK_EQ(co->lpVtbl->AddRefTypeInfo(co, foo_ti, &h_foo), S_OK);
	CHECK_EQ(co->lpVtbl->AddRefTypeInfo(co, dfoo_ti, &h_dfoo), S_OK);
	CHECK_EQ(h_dfoo, 0x64);
	CHECK_EQ(co->lpVtbl->AddImplType(co, 0, h_foo), S_OK);
	CHECK_EQ(co->lpVtbl->AddImplType(co, 1, h_dfoo), S_OK);
	CHECK_EQ(co->lpVtbl->SetImplTypeFlags(co, 0, IMPLTYPEFLAG_FDEFAULT),
		 S_OK);
	CHECK_EQ(co->lpVtbl->SetImplTypeFlags(co, 1, IMPLTYPEFLAG_FDEFAULT |
						     IMPLTYPEFLAG_FSOURCE),
		 S_OK);
	foo_ti->lpVtbl->Release(foo_ti);
	dfoo_ti->lpVtbl->Release(dfoo_ti);

	/* No two types of a name. */
	ICreateTypeInfo *dup = NULL;
	CHECK_EQ(ctl->lpVtbl->CreateTypeInfo(ctl, w("ifoo"), TKIND_RECORD,
					     &dup), TYPE_E_NAMECONFLICT);
	CHECK_EQ(co->lpVtbl->SetName(co, w("Point3")), TYPE_E_NAMECONFLICT);

	/* The library so far, as a snapshot. */
	ITypeLib *snap = NULL;
	CHECK_EQ(ctl->lpVtbl->QueryInterface(ctl, &IID_ITypeLib,
					     (void **)&snap), S_OK);
	if (snap) {
		CHECK_EQ(snap->lpVtbl->GetTypeInfoCount(snap), 7);
		snap->lpVtbl->Release(snap);
	}

	CHECK_EQ(ctl->lpVtbl->SaveAllChanges(ctl), S_OK);
	foo->lpVtbl->Release(foo);
	dfoo->lpVtbl->Release(dfoo);
	colors->lpVtbl->Release(colors);
	pt->lpVtbl->Release(pt);
	pfoo->lpVtbl->Release(pfoo);
	mod->lpVtbl->Release(mod);
	co->lpVtbl->Release(co);
	CHECK_EQ(ctl->lpVtbl->Release(ctl), 0);
	unk->lpVtbl->Release(unk);
	disp->lpVtbl->Release(disp);
}

static ITypeInfo *type_named(ITypeLib *lib, UINT i, const char *want)
{
	ITypeInfo *ti = NULL;
	BSTR name = NULL;
	CHECK_EQ(lib->lpVtbl->GetTypeInfo(lib, i, &ti), S_OK);
	if (!ti)
		return NULL;
	CHECK_EQ(ti->lpVtbl->GetDocumentation(ti, MEMBERID_NIL, &name, NULL,
					      NULL, NULL), S_OK);
	CHECK(is(name, want));
	pSysFreeString(name);
	return ti;
}

static void check(SYSKIND sys, const WCHAR *path)
{
	ITypeLib *lib = NULL;
	TLIBATTR *la;
	TYPEATTR *ta;
	FUNCDESC *fd;
	VARDESC *vd;
	BSTR name = NULL, doc = NULL, file = NULL;
	DWORD ctx = 0;
	VARIANT v;
	int ptr = sizeof(void *);

	CHECK_EQ(pLoadTypeLibEx(path, REGKIND_NONE, &lib), S_OK);
	if (!lib)
		return;
	ITypeLib2 *lib2 = (ITypeLib2 *)lib;
	CHECK_EQ(lib->lpVtbl->GetTypeInfoCount(lib), 7);
	CHECK_EQ(lib->lpVtbl->GetLibAttr(lib, &la), S_OK);
	CHECK(!memcmp(&la->guid, &libid, sizeof(GUID)));
	CHECK(la->wMajorVerNum == 2 && la->wMinorVerNum == 5);
	CHECK_EQ(la->lcid, 0x409);
	CHECK_EQ(la->syskind, sys);
	CHECK_EQ(la->wLibFlags, LIBFLAG_FHASDISKIMAGE);
	lib->lpVtbl->ReleaseTLibAttr(lib, la);
	CHECK_EQ(lib->lpVtbl->GetDocumentation(lib, -1, &name, &doc, &ctx,
					       &file), S_OK);
	CHECK(is(name, "TestLib") && is(doc, "A test library") &&
	      is(file, "test.hlp") && ctx == 7);
	pSysFreeString(name);
	pSysFreeString(doc);
	pSysFreeString(file);
	BSTR dll = NULL;
	CHECK_EQ(lib2->lpVtbl->GetDocumentation2(lib2, -1, 0x409, NULL, NULL,
						 &dll), S_OK);
	CHECK(is(dll, "help.dll"));
	pSysFreeString(dll);
	v.vt = VT_EMPTY;
	CHECK_EQ(lib2->lpVtbl->GetCustData(lib2, &cust_guid, &v), S_OK);
	CHECK(v.vt == VT_I4 && v.lVal == 42);

	/* IFoo */
	ITypeInfo *foo = type_named(lib, 0, "IFoo");
	CHECK_EQ(foo->lpVtbl->GetTypeAttr(foo, &ta), S_OK);
	CHECK_EQ(ta->typekind, TKIND_INTERFACE);
	CHECK(!memcmp(&ta->guid, &iid_foo, sizeof(GUID)));
	CHECK_EQ(ta->cFuncs, 3);
	CHECK_EQ(ta->cImplTypes, 1);
	CHECK_EQ(ta->cbSizeVft, 6 * ptr);
	CHECK_EQ(ta->wTypeFlags, TYPEFLAG_FOLEAUTOMATION);
	foo->lpVtbl->ReleaseTypeAttr(foo, ta);
	HREFTYPE href = 0;
	CHECK_EQ(foo->lpVtbl->GetRefTypeOfImplType(foo, 0, &href), S_OK);
	CHECK(ref_name_is(foo, href, "IUnknown"));
	CHECK_EQ(foo->lpVtbl->GetFuncDesc(foo, 0, &fd), S_OK);
	CHECK_EQ(fd->memid, 1);
	CHECK_EQ(fd->cParams, 2);
	CHECK_EQ(fd->oVft, 3 * ptr);
	CHECK_EQ(fd->funckind, FUNC_PUREVIRTUAL);
	CHECK_EQ(fd->elemdescFunc.tdesc.vt, VT_HRESULT);
	CHECK(fd->lprgelemdescParam[1].paramdesc.wParamFlags &
	      PARAMFLAG_FHASDEFAULT);
	CHECK(fd->lprgelemdescParam[1].paramdesc.pparamdescex &&
	      fd->lprgelemdescParam[1].paramdesc.pparamdescex->varDefaultValue.lVal
	      == 5);
	foo->lpVtbl->ReleaseFuncDesc(foo, fd);
	BSTR names[4] = { 0 };
	UINT n = 0;
	CHECK_EQ(foo->lpVtbl->GetNames(foo, 1, names, 4, &n), S_OK);
	CHECK(n == 3 && is(names[0], "Add") && is(names[1], "a") &&
	      is(names[2], "b"));
	for (UINT i = 0; i < n; i++)
		pSysFreeString(names[i]);
	CHECK_EQ(foo->lpVtbl->GetDocumentation(foo, 1, NULL, &doc, &ctx, NULL),
		 S_OK);
	CHECK(is(doc, "Adds") && ctx == 99);
	pSysFreeString(doc);
	CHECK_EQ(foo->lpVtbl->GetFuncDesc(foo, 1, &fd), S_OK);
	CHECK(fd->invkind == INVOKE_PROPERTYGET && fd->oVft == 4 * ptr &&
	      fd->lprgelemdescParam[0].tdesc.vt == VT_PTR &&
	      fd->lprgelemdescParam[0].tdesc.lptdesc->vt == VT_BSTR);
	foo->lpVtbl->ReleaseFuncDesc(foo, fd);
	CHECK_EQ(foo->lpVtbl->GetFuncDesc(foo, 2, &fd), S_OK);
	CHECK(fd->invkind == INVOKE_PROPERTYPUT && fd->oVft == 5 * ptr);
	foo->lpVtbl->ReleaseFuncDesc(foo, fd);
	ITypeInfo2 *foo2 = (ITypeInfo2 *)foo;
	v.vt = VT_EMPTY;
	CHECK_EQ(foo2->lpVtbl->GetFuncCustData(foo2, 0, &cust_guid, &v), S_OK);
	CHECK(v.vt == VT_I2 && v.iVal == 3);
	v.vt = VT_EMPTY;
	CHECK_EQ(foo2->lpVtbl->GetParamCustData(foo2, 0, 1, &cust_guid, &v),
		 S_OK);
	CHECK(v.vt == VT_BSTR && is(v.bstrVal, "param data"));
	pVariantClear(&v);
	foo->lpVtbl->Release(foo);

	/* DFoo */
	ITypeInfo *dfoo = type_named(lib, 1, "DFoo");
	CHECK_EQ(dfoo->lpVtbl->GetTypeAttr(dfoo, &ta), S_OK);
	CHECK(ta->typekind == TKIND_DISPATCH && ta->cFuncs == 1 &&
	      ta->cVars == 1 && ta->cbSizeVft == 7 * ptr);
	dfoo->lpVtbl->ReleaseTypeAttr(dfoo, ta);
	CHECK_EQ(dfoo->lpVtbl->GetRefTypeOfImplType(dfoo, 0, &href), S_OK);
	CHECK(ref_name_is(dfoo, href, "IDispatch"));
	CHECK_EQ(dfoo->lpVtbl->GetVarDesc(dfoo, 0, &vd), S_OK);
	CHECK(vd->memid == 11 && vd->varkind == VAR_DISPATCH);
	dfoo->lpVtbl->ReleaseVarDesc(dfoo, vd);
	MEMBERID id = 0;
	LPOLESTR count = w("count");
	CHECK_EQ(dfoo->lpVtbl->GetIDsOfNames(dfoo, &count, 1, &id), S_OK);
	CHECK_EQ(id, 11);
	dfoo->lpVtbl->Release(dfoo);

	/* Colors */
	ITypeInfo *colors = type_named(lib, 2, "Colors");
	static const LONG values[] = { 0, 1, 0x7fffffff, -1 };
	for (UINT i = 0; i < 4; i++) {
		CHECK_EQ(colors->lpVtbl->GetVarDesc(colors, i, &vd), S_OK);
		CHECK(vd->varkind == VAR_CONST && vd->lpvarValue &&
		      vd->lpvarValue->vt == VT_I4 &&
		      vd->lpvarValue->lVal == values[i]);
		colors->lpVtbl->ReleaseVarDesc(colors, vd);
	}
	colors->lpVtbl->Release(colors);

	/* Point3: laid out for the library's pointers */
	ITypeInfo *pt = type_named(lib, 3, "Point3");
	CHECK_EQ(pt->lpVtbl->GetTypeAttr(pt, &ta), S_OK);
	ULONG want_size = 32;	/* 28 rounded up to 8 */
	CHECK(ta->typekind == TKIND_RECORD && ta->cVars == 6);
	CHECK_EQ(ta->cbSizeInstance, want_size);
	CHECK_EQ(ta->cbAlignment, 8);
	pt->lpVtbl->ReleaseTypeAttr(pt, ta);
	static const ULONG offs[] = { 0, 4, 8, 16, 20, 24 };
	for (UINT i = 0; i < 6; i++) {
		CHECK_EQ(pt->lpVtbl->GetVarDesc(pt, i, &vd), S_OK);
		CHECK_EQ(vd->oInst, offs[i]);
		if (i == 3)
			CHECK(vd->elemdescVar.tdesc.vt == VT_CARRAY &&
			      vd->elemdescVar.tdesc.lpadesc->cDims == 1 &&
			      vd->elemdescVar.tdesc.lpadesc->rgbounds[0]
				      .cElements == 3);
		if (i == 4)
			CHECK(ref_name_is(pt, vd->elemdescVar.tdesc.hreftype,
					  "Colors"));
		pt->lpVtbl->ReleaseVarDesc(pt, vd);
	}
	CHECK_EQ(pt->lpVtbl->GetDocumentation(pt, 0x40000000, &name, &doc,
					      NULL, NULL), S_OK);
	CHECK(is(name, "x") && is(doc, "across"));
	pSysFreeString(name);
	pSysFreeString(doc);
	pt->lpVtbl->Release(pt);

	/* PFoo */
	ITypeInfo *pfoo = type_named(lib, 4, "PFoo");
	CHECK_EQ(pfoo->lpVtbl->GetTypeAttr(pfoo, &ta), S_OK);
	CHECK(ta->typekind == TKIND_ALIAS && ta->tdescAlias.vt == VT_PTR &&
	      ta->tdescAlias.lptdesc->vt == VT_USERDEFINED);
	if (ta->tdescAlias.vt == VT_PTR)
		CHECK(ref_name_is(pfoo, ta->tdescAlias.lptdesc->hreftype,
				  "IFoo"));
	pfoo->lpVtbl->ReleaseTypeAttr(pfoo, ta);
	pfoo->lpVtbl->Release(pfoo);

	/* Funcs */
	ITypeInfo *mod = type_named(lib, 5, "Funcs");
	WORD ord = 0;
	BSTR entry = NULL;
	CHECK_EQ(mod->lpVtbl->GetDllEntry(mod, 0x60000000, INVOKE_FUNC, &dll,
					  &entry, &ord), S_OK);
	CHECK(is(dll, "kernel32.dll") && is(entry, "GetTickCount"));
	pSysFreeString(dll);
	pSysFreeString(entry);
	entry = NULL;
	CHECK_EQ(mod->lpVtbl->GetDllEntry(mod, 0x60000001, INVOKE_FUNC, &dll,
					  &entry, &ord), S_OK);
	CHECK(entry == NULL && ord == 5);
	pSysFreeString(dll);
	CHECK_EQ(mod->lpVtbl->GetVarDesc(mod, 0, &vd), S_OK);
	CHECK(vd->lpvarValue && vd->lpvarValue->vt == VT_BSTR &&
	      is(vd->lpvarValue->bstrVal, "hello"));
	mod->lpVtbl->ReleaseVarDesc(mod, vd);
	mod->lpVtbl->Release(mod);

	/* Foo */
	ITypeInfo *co = type_named(lib, 6, "Foo");
	CHECK_EQ(co->lpVtbl->GetTypeAttr(co, &ta), S_OK);
	CHECK(ta->typekind == TKIND_COCLASS && ta->cImplTypes == 2 &&
	      ta->wTypeFlags == TYPEFLAG_FCANCREATE);
	co->lpVtbl->ReleaseTypeAttr(co, ta);
	INT flags = 0;
	CHECK_EQ(co->lpVtbl->GetImplTypeFlags(co, 0, &flags), S_OK);
	CHECK_EQ(flags, IMPLTYPEFLAG_FDEFAULT);
	CHECK_EQ(co->lpVtbl->GetImplTypeFlags(co, 1, &flags), S_OK);
	CHECK_EQ(flags, IMPLTYPEFLAG_FDEFAULT | IMPLTYPEFLAG_FSOURCE);
	CHECK_EQ(co->lpVtbl->GetRefTypeOfImplType(co, 0, &href), S_OK);
	CHECK(ref_name_is(co, href, "IFoo"));
	CHECK_EQ(co->lpVtbl->GetRefTypeOfImplType(co, 1, &href), S_OK);
	CHECK(ref_name_is(co, href, "DFoo"));
	co->lpVtbl->Release(co);

	ITypeInfo *by_guid = NULL;
	CHECK_EQ(lib->lpVtbl->GetTypeInfoOfGuid(lib, &clsid_foo, &by_guid),
		 S_OK);
	if (by_guid)
		by_guid->lpVtbl->Release(by_guid);
	lib->lpVtbl->Release(lib);
}

/* Review cases: identity, IDispatch bases, duals, slots, member IDs. */
static void test_more(ITypeLib *std)
{
	/* The reader gives slots in this process's pointer size. */
	const int P = (int)sizeof(void *);
	char path[512];
	snprintf(path, sizeof(path), "%s/more.tlb", dir);
	char *win = path_to_windows(path);
	WCHAR *wpath = utf8_to_utf16_dup(win);
	ICreateTypeLib2 *ctl = NULL;
	ITypeInfo *unk = NULL, *disp = NULL;
	HREFTYPE h;

	CHECK_EQ(pCreateTypeLib2(SYS_WIN32, wpath, &ctl), S_OK);
	free(wpath);
	free(win);
	if (!ctl)
		return;
	CHECK_EQ(ctl->lpVtbl->SetName(ctl, w("More")), S_OK);
	std->lpVtbl->GetTypeInfoOfGuid(std, &iid_unknown, &unk);
	std->lpVtbl->GetTypeInfoOfGuid(std, &iid_dispatch, &disp);

	FUNCDESC fd;
	ELEMDESC prm[1];
	memset(&fd, 0, sizeof(fd));
	memset(prm, 0, sizeof(prm));
	fd.funckind = FUNC_DISPATCH;
	fd.invkind = INVOKE_FUNC;
	fd.callconv = CC_STDCALL;
	fd.elemdescFunc.tdesc.vt = VT_HRESULT;

	/* A dispinterface with no base given still has IDispatch's. */
	ICreateTypeInfo *bare = make(ctl, "DBare", TKIND_DISPATCH);
	fd.memid = 1;
	CHECK_EQ(bare->lpVtbl->AddFuncDesc(bare, 0, &fd), S_OK);
	CHECK_EQ(bare->lpVtbl->LayOut(bare), S_OK);

	/* A dual interface on IDispatch. */
	ICreateTypeInfo *dual = make(ctl, "IDual", TKIND_INTERFACE);
	CHECK_EQ(dual->lpVtbl->SetTypeFlags(dual, TYPEFLAG_FDUAL |
					    TYPEFLAG_FOLEAUTOMATION), S_OK);
	CHECK_EQ(dual->lpVtbl->AddRefTypeInfo(dual, disp, &h), S_OK);
	CHECK_EQ(dual->lpVtbl->AddImplType(dual, 0, h), S_OK);
	fd.funckind = FUNC_PUREVIRTUAL;
	fd.memid = 1;
	CHECK_EQ(dual->lpVtbl->AddFuncDesc(dual, 0, &fd), S_OK);
	CHECK_EQ(dual->lpVtbl->LayOut(dual), S_OK);

	/* An interface with a slot of its own choosing, and members whose
	 * IDs are made for them. */
	ICreateTypeInfo *slots = make(ctl, "ISlots", TKIND_INTERFACE);
	CHECK_EQ(slots->lpVtbl->AddRefTypeInfo(slots, unk, &h), S_OK);
	CHECK_EQ(slots->lpVtbl->AddImplType(slots, 0, h), S_OK);
	fd.memid = MEMBERID_NIL;
	fd.oVft = 6;	/* not a pointer's multiple */
	CHECK_EQ(slots->lpVtbl->AddFuncDesc(slots, 0, &fd), E_INVALIDARG);
	fd.oVft = 20;
	CHECK_EQ(slots->lpVtbl->AddFuncDesc(slots, 0, &fd), S_OK);
	fd.oVft = 0;
	CHECK_EQ(slots->lpVtbl->AddFuncDesc(slots, 1, &fd), S_OK);
	CHECK_EQ(slots->lpVtbl->DeleteFuncDesc(slots, 0), S_OK);
	CHECK_EQ(slots->lpVtbl->AddFuncDesc(slots, 1, &fd), S_OK);
	/* A vararg function: the last parameter takes the rest. */
	TYPEDESC var_td = { .vt = VT_VARIANT };
	prm[0].tdesc.vt = VT_SAFEARRAY;
	prm[0].tdesc.lptdesc = &var_td;
	prm[0].paramdesc.wParamFlags = PARAMFLAG_FIN;
	fd.cParams = 1;
	fd.cParamsOpt = -1;
	fd.lprgelemdescParam = prm;
	CHECK_EQ(slots->lpVtbl->AddFuncDesc(slots, 2, &fd), S_OK);
	fd.cParamsOpt = -2;
	CHECK_EQ(slots->lpVtbl->AddFuncDesc(slots, 2, &fd), E_INVALIDARG);
	fd.cParams = 0;
	fd.cParamsOpt = 0;
	fd.oVft = 32;		/* a slot given: kept */
	CHECK_EQ(slots->lpVtbl->AddFuncDesc(slots, 3, &fd), S_OK);
	fd.oVft = 0;
	CHECK_EQ(slots->lpVtbl->LayOut(slots), S_OK);

	/* The snapshot is the creator, as COM sees it. */
	ITypeLib *tl = NULL;
	IUnknown *u1 = NULL, *u2 = NULL;
	ICreateTypeLib2 *back = NULL;
	CHECK_EQ(ctl->lpVtbl->QueryInterface(ctl, &IID_ITypeLib, (void **)&tl),
		 S_OK);
	if (!tl)
		goto done;
	CHECK_EQ(ctl->lpVtbl->QueryInterface(ctl, &iid_unknown, (void **)&u1),
		 S_OK);
	CHECK_EQ(tl->lpVtbl->QueryInterface(tl, &iid_unknown, (void **)&u2),
		 S_OK);
	CHECK(u1 && u1 == u2);
	CHECK_EQ(tl->lpVtbl->QueryInterface(tl, &IID_ICreateTypeLib2,
					    (void **)&back), S_OK);
	CHECK(back == ctl);
	if (u1)
		u1->lpVtbl->Release(u1);
	if (u2)
		u2->lpVtbl->Release(u2);
	if (back)
		back->lpVtbl->Release(back);

	ITypeInfo *ti = NULL;
	TYPEATTR *ta;
	CHECK_EQ(tl->lpVtbl->GetTypeInfo(tl, 0, &ti), S_OK);
	if (ti) {
		CHECK_EQ(ti->lpVtbl->GetRefTypeOfImplType(ti, 0, &h), S_OK);
		CHECK(ref_name_is(ti, h, "IDispatch"));
		ICreateTypeInfo *ci = NULL;
		CHECK_EQ(ti->lpVtbl->QueryInterface(ti, &IID_ICreateTypeInfo,
						    (void **)&ci), S_OK);
		CHECK(ci == bare);
		if (ci)
			ci->lpVtbl->Release(ci);
		ti->lpVtbl->Release(ti);
	}
	CHECK_EQ(tl->lpVtbl->GetTypeInfo(tl, 1, &ti), S_OK);
	if (ti) {
		CHECK_EQ(ti->lpVtbl->GetTypeAttr(ti, &ta), S_OK);
		CHECK_EQ(ta->typekind, TKIND_DISPATCH);
		CHECK(ta->wTypeFlags & TYPEFLAG_FDUAL);
		ti->lpVtbl->ReleaseTypeAttr(ti, ta);
		ITypeInfo *half = NULL;
		CHECK_EQ(ti->lpVtbl->GetRefTypeOfImplType(ti, (UINT)-1, &h),
			 S_OK);
		CHECK_EQ(ti->lpVtbl->GetRefTypeInfo(ti, h, &half), S_OK);
		if (half) {
			CHECK_EQ(half->lpVtbl->GetTypeAttr(half, &ta), S_OK);
			CHECK_EQ(ta->typekind, TKIND_INTERFACE);
			CHECK_EQ(ta->cFuncs, 1);
			CHECK_EQ(ta->cbSizeVft, 8 * P);	/* IDispatch's 7 + 1 */
			half->lpVtbl->ReleaseTypeAttr(half, ta);
			half->lpVtbl->Release(half);
		}
		ti->lpVtbl->Release(ti);
	}
	CHECK_EQ(tl->lpVtbl->GetTypeInfo(tl, 2, &ti), S_OK);
	if (ti) {
		FUNCDESC *f;
		CHECK_EQ(ti->lpVtbl->GetFuncDesc(ti, 0, &f), S_OK);
		CHECK_EQ(f->oVft, 3 * P);	/* after IUnknown's 3 */
		CHECK_EQ(f->memid, 0x60000001);
		ti->lpVtbl->ReleaseFuncDesc(ti, f);
		CHECK_EQ(ti->lpVtbl->GetFuncDesc(ti, 1, &f), S_OK);
		CHECK_EQ(f->memid, 0x60000002);	/* the next free one */
		ti->lpVtbl->ReleaseFuncDesc(ti, f);
		CHECK_EQ(ti->lpVtbl->GetFuncDesc(ti, 2, &f), S_OK);
		CHECK_EQ(f->cParamsOpt, -1);
		ti->lpVtbl->ReleaseFuncDesc(ti, f);
		CHECK_EQ(ti->lpVtbl->GetFuncDesc(ti, 3, &f), S_OK);
		CHECK_EQ(f->oVft, 8 * P);
		ti->lpVtbl->ReleaseFuncDesc(ti, f);
		CHECK_EQ(ti->lpVtbl->GetTypeAttr(ti, &ta), S_OK);
		CHECK_EQ(ta->cbSizeVft, 9 * P);
		ti->lpVtbl->ReleaseTypeAttr(ti, ta);
		ti->lpVtbl->Release(ti);
	}
	tl->lpVtbl->Release(tl);
done:
	bare->lpVtbl->Release(bare);
	dual->lpVtbl->Release(dual);
	slots->lpVtbl->Release(slots);
	unk->lpVtbl->Release(unk);
	disp->lpVtbl->Release(disp);
	ctl->lpVtbl->Release(ctl);
}

int main(void)
{
	char cleanup[64];

	setenv("VFW_REGISTRY", "", 1);
	CHECK_EQ(runtime_enter(), 0);
	reg_reload();
	oa = mod_load("oleaut32.dll", NULL);
	CHECK(oa != NULL);
	if (!oa || !mkdtemp(dir)) {
		CHECK(0);
		return test_summary("test_typelib_create");
	}
	pCreateTypeLib2 = get("CreateTypeLib2");
	pLoadTypeLib = get("LoadTypeLib");
	pLoadTypeLibEx = get("LoadTypeLibEx");
	pSysFreeString = get("SysFreeString");
	pSysAllocString = get("SysAllocString");
	pVariantClear = get("VariantClear");
	CHECK(mod_get_proc(oa, "CreateTypeLib2") == mod_get_ordinal(oa, 180));
	CHECK(mod_get_proc(oa, "CreateTypeLib") == mod_get_ordinal(oa, 160));

	ITypeLib *std = NULL;
	CHECK_EQ(pLoadTypeLib(w("stdole2.tlb"), &std), S_OK);
	if (!std)
		return test_summary("test_typelib_create");

	static const SYSKIND kinds[] = { SYS_WIN32, SYS_WIN64 };
	for (int k = 0; k < 2; k++) {
		char path[512];
		snprintf(path, sizeof(path), "%s/made%d.tlb", dir, k);
		char *win = path_to_windows(path);
		WCHAR *wpath = utf8_to_utf16_dup(win);
		build(kinds[k], wpath, std);
		check(kinds[k], wpath);
		free(wpath);
		free(win);
	}

	test_more(std);

	/* Nowhere to write. */
	ICreateTypeLib2 *ctl = NULL;
	CHECK_EQ(pCreateTypeLib2(SYS_WIN32, w("Q:\\no\\such\\dir\\x.tlb"),
				 &ctl), S_OK);
	if (ctl) {
		CHECK(FAILED(ctl->lpVtbl->SaveAllChanges(ctl)));
		ctl->lpVtbl->Release(ctl);
	}
	CHECK_EQ(pCreateTypeLib2(SYS_WIN32, NULL, &ctl), E_INVALIDARG);

	std->lpVtbl->Release(std);
	mod_free(oa);
	snprintf(cleanup, sizeof(cleanup), "rm -rf '%s'", dir);
	if (system(cleanup))
		fprintf(stderr, "could not remove %s\n", dir);
	return test_summary("test_typelib_create");
}
