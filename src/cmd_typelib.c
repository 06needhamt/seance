/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * seance info --typelib <file>: list a type library's types, through
 * oleaut32's own LoadTypeLib and the ITypeLib and ITypeInfo it gives.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "commands.h"
#include "common/log.h"
#include "win32/codepage.h"
#include "win32/oleaut.h"
#include "win32/path.h"

static const char *const kinds[] = {
	"enum", "record", "module", "interface", "dispinterface", "coclass",
	"alias", "union",
};

static const char *const invkinds[] = {
	[INVOKE_FUNC] = "", [INVOKE_PROPERTYGET] = "propget ",
	[INVOKE_PROPERTYPUT] = "propput ", [INVOKE_PROPERTYPUTREF] = "propputref ",
};

static const char *vt_name(VARTYPE vt)
{
	static const char *const names[] = {
		[VT_EMPTY] = "EMPTY", [VT_NULL] = "NULL", [VT_I2] = "short",
		[VT_I4] = "long", [VT_R4] = "float", [VT_R8] = "double",
		[VT_CY] = "CURRENCY", [VT_DATE] = "DATE", [VT_BSTR] = "BSTR",
		[VT_DISPATCH] = "IDispatch*", [VT_ERROR] = "SCODE",
		[VT_BOOL] = "VARIANT_BOOL", [VT_VARIANT] = "VARIANT",
		[VT_UNKNOWN] = "IUnknown*", [VT_DECIMAL] = "DECIMAL",
		[VT_I1] = "char", [VT_UI1] = "unsigned char",
		[VT_UI2] = "unsigned short", [VT_UI4] = "unsigned long",
		[VT_I8] = "int64", [VT_UI8] = "uint64", [VT_INT] = "int",
		[VT_UINT] = "unsigned int", [VT_VOID] = "void",
		[VT_HRESULT] = "HRESULT", [VT_LPSTR] = "LPSTR",
		[VT_LPWSTR] = "LPWSTR",
	};

	if (vt < sizeof(names) / sizeof(names[0]) && names[vt])
		return names[vt];
	return NULL;
}

static void put_bstr(BSTR b)
{
	char *s = b ? utf16_to_utf8_dup(b) : NULL;

	fputs(s ? s : "?", stdout);
	free(s);
}

/* Counts references that don't resolve, for the exit status. */
static int unresolved;

static void put_type(ITypeInfo *ti, const TYPEDESC *td, int depth)
{
	const char *n;

	if (depth > 16) {
		fputs("...", stdout);
		return;
	}
	switch (td->vt) {
	case VT_PTR:
		put_type(ti, td->lptdesc, depth + 1);
		putchar('*');
		return;
	case VT_SAFEARRAY:
		fputs("SAFEARRAY(", stdout);
		put_type(ti, td->lptdesc, depth + 1);
		putchar(')');
		return;
	case VT_CARRAY:
		put_type(ti, &td->lpadesc->tdescElem, depth + 1);
		for (USHORT i = 0; i < td->lpadesc->cDims; i++)
			printf("[%lu]",
			       (unsigned long)td->lpadesc->rgbounds[i].cElements);
		return;
	case VT_USERDEFINED: {
		ITypeInfo *r = NULL;
		BSTR name = NULL;

		if (FAILED(ti->lpVtbl->GetRefTypeInfo(ti, td->hreftype, &r)) ||
		    FAILED(r->lpVtbl->GetDocumentation(r, MEMBERID_NIL, &name,
						       NULL, NULL, NULL))) {
			printf("<unresolved %#lx>", (unsigned long)td->hreftype);
			unresolved++;
		} else {
			put_bstr(name);
		}
		oa_SysFreeString(name);
		if (r)
			r->lpVtbl->Release(r);
		return;
	}
	default:
		if ((n = vt_name(td->vt)))
			fputs(n, stdout);
		else
			printf("vt%u", td->vt);
	}
}

static void put_guid(const GUID *g)
{
	printf("{%08lX-%04X-%04X-%02X%02X-%02X%02X%02X%02X%02X%02X}",
	       (unsigned long)g->Data1, g->Data2, g->Data3, g->Data4[0],
	       g->Data4[1], g->Data4[2], g->Data4[3], g->Data4[4], g->Data4[5],
	       g->Data4[6], g->Data4[7]);
}

static void put_value(const VARIANT *v)
{
	switch (v->vt) {
	case VT_BSTR:
		putchar('"');
		put_bstr(v->bstrVal);
		putchar('"');
		break;
	case VT_I1: printf("%d", v->cVal); break;
	case VT_UI1: printf("%u", v->bVal); break;
	case VT_I2: printf("%d", v->iVal); break;
	case VT_UI2: printf("%u", v->uiVal); break;
	case VT_BOOL: printf("%s", v->boolVal ? "true" : "false"); break;
	case VT_I4: case VT_INT: case VT_ERROR: case VT_HRESULT:
		printf("%ld", (long)v->lVal); break;
	case VT_UI4: case VT_UINT: printf("%lu", (unsigned long)v->ulVal); break;
	case VT_R4: printf("%g", v->fltVal); break;
	case VT_R8: printf("%g", v->dblVal); break;
	default: printf("<vt %u>", v->vt);
	}
}

/* The type info's functions, with their parameters. */
static void put_funcs(ITypeInfo *ti, const TYPEATTR *a)
{
	for (UINT i = 0; i < a->cFuncs; i++) {
		FUNCDESC *f;
		BSTR names[64] = { 0 };
		UINT n = 0;

		if (FAILED(ti->lpVtbl->GetFuncDesc(ti, i, &f))) {
			printf("    function %u: unreadable\n", i);
			unresolved++;
			continue;
		}
		ti->lpVtbl->GetNames(ti, f->memid, names, 64, &n);
		printf("    [id(%#lx)%s] %s", (unsigned long)f->memid,
		       a->typekind == TKIND_DISPATCH ? "" : "",
		       f->invkind < sizeof(invkinds) / sizeof(invkinds[0]) &&
				       invkinds[f->invkind]
			       ? invkinds[f->invkind]
			       : "");
		put_type(ti, &f->elemdescFunc.tdesc, 0);
		putchar(' ');
		put_bstr(n ? names[0] : NULL);
		putchar('(');
		for (SHORT j = 0; j < f->cParams; j++) {
			const ELEMDESC *e = &f->lprgelemdescParam[j];
			USHORT pf = e->paramdesc.wParamFlags;

			if (j)
				fputs(", ", stdout);
			if (pf & (PARAMFLAG_FIN | PARAMFLAG_FOUT |
				  PARAMFLAG_FRETVAL | PARAMFLAG_FOPT))
				printf("[%s%s%s%s] ",
				       pf & PARAMFLAG_FIN ? "in" : "",
				       pf & PARAMFLAG_FOUT ? (pf & PARAMFLAG_FIN ? ",out" : "out") : "",
				       pf & PARAMFLAG_FRETVAL ? ",retval" : "",
				       pf & PARAMFLAG_FOPT ? ",optional" : "");
			put_type(ti, &e->tdesc, 0);
			if ((UINT)j + 1 < n) {
				putchar(' ');
				put_bstr(names[j + 1]);
			}
			if ((pf & PARAMFLAG_FHASDEFAULT) &&
			    e->paramdesc.pparamdescex) {
				fputs(" = ", stdout);
				put_value(&e->paramdesc.pparamdescex
						    ->varDefaultValue);
			}
		}
		puts(")");
		for (UINT j = 0; j < n; j++)
			oa_SysFreeString(names[j]);
		ti->lpVtbl->ReleaseFuncDesc(ti, f);
	}
}

static void put_vars(ITypeInfo *ti, const TYPEATTR *a)
{
	for (UINT i = 0; i < a->cVars; i++) {
		VARDESC *v;
		BSTR name = NULL;
		UINT n = 0;

		if (FAILED(ti->lpVtbl->GetVarDesc(ti, i, &v))) {
			printf("    variable %u: unreadable\n", i);
			unresolved++;
			continue;
		}
		ti->lpVtbl->GetNames(ti, v->memid, &name, 1, &n);
		fputs("    ", stdout);
		put_type(ti, &v->elemdescVar.tdesc, 0);
		putchar(' ');
		put_bstr(name);
		if (v->varkind == VAR_CONST && v->lpvarValue) {
			fputs(" = ", stdout);
			put_value(v->lpvarValue);
		} else if (v->varkind == VAR_PERINSTANCE) {
			printf(" (offset %lu)", (unsigned long)v->oInst);
		}
		putchar('\n');
		oa_SysFreeString(name);
		ti->lpVtbl->ReleaseVarDesc(ti, v);
	}
}

static void put_impls(ITypeInfo *ti, const TYPEATTR *a)
{
	for (UINT i = 0; i < a->cImplTypes; i++) {
		HREFTYPE h;
		INT flags = 0;
		TYPEDESC td = { .vt = VT_USERDEFINED };

		if (FAILED(ti->lpVtbl->GetRefTypeOfImplType(ti, i, &h))) {
			unresolved++;
			continue;
		}
		ti->lpVtbl->GetImplTypeFlags(ti, i, &flags);
		td.hreftype = h;
		printf("    %s ", a->typekind == TKIND_COCLASS
				      ? "implements"
				      : "inherits");
		put_type(ti, &td, 0);
		if (flags & IMPLTYPEFLAG_FDEFAULT)
			fputs(" [default]", stdout);
		if (flags & IMPLTYPEFLAG_FSOURCE)
			fputs(" [source]", stdout);
		putchar('\n');
	}
}

static void put_typeinfo(ITypeInfo *ti, UINT index)
{
	TYPEATTR *a;
	BSTR name = NULL, doc = NULL;

	if (FAILED(ti->lpVtbl->GetTypeAttr(ti, &a))) {
		printf("  type %u: unreadable\n", index);
		unresolved++;
		return;
	}
	ti->lpVtbl->GetDocumentation(ti, MEMBERID_NIL, &name, &doc, NULL,
				     NULL);
	printf("  %s ", a->typekind < TKIND_MAX ? kinds[a->typekind] : "?");
	put_bstr(name);
	if (a->typekind == TKIND_ALIAS) {
		fputs(" = ", stdout);
		put_type(ti, &a->tdescAlias, 0);
	}
	if (memcmp(&a->guid, &oa_IID_NULL, sizeof(GUID))) {
		putchar(' ');
		put_guid(&a->guid);
	}
	if (a->wTypeFlags)
		printf(" flags %#x", a->wTypeFlags);
	if (a->typekind == TKIND_INTERFACE || a->typekind == TKIND_DISPATCH)
		printf(" vtable %u", a->cbSizeVft);
	if (a->typekind == TKIND_RECORD || a->typekind == TKIND_UNION)
		printf(" size %lu", (unsigned long)a->cbSizeInstance);
	putchar('\n');
	if (doc) {
		fputs("    \"", stdout);
		put_bstr(doc);
		puts("\"");
	}
	put_impls(ti, a);
	put_funcs(ti, a);
	put_vars(ti, a);

	/* A dual interface's other half: its vtable. */
	HREFTYPE h;
	ITypeInfo *iface;
	if (SUCCEEDED(ti->lpVtbl->GetRefTypeOfImplType(ti, (UINT)-1, &h)) &&
	    SUCCEEDED(ti->lpVtbl->GetRefTypeInfo(ti, h, &iface))) {
		TYPEATTR *b;

		if (SUCCEEDED(iface->lpVtbl->GetTypeAttr(iface, &b))) {
			printf("  dual: interface with vtable %u\n",
			       b->cbSizeVft);
			put_impls(iface, b);
			put_funcs(iface, b);
			iface->lpVtbl->ReleaseTypeAttr(iface, b);
		} else {
			unresolved++;
		}
		iface->lpVtbl->Release(iface);
	}
	oa_SysFreeString(name);
	oa_SysFreeString(doc);
	ti->lpVtbl->ReleaseTypeAttr(ti, a);
}

int cmd_typelib(const char *path)
{
	char *abs = realpath(path, NULL);
	char *win = abs ? path_to_windows(abs) : NULL;
	WCHAR *w = win ? utf8_to_utf16_dup(win) : NULL;
	ITypeLib *lib = NULL;
	TLIBATTR *la;
	BSTR name = NULL, doc = NULL;
	HRESULT hr;

	vfw_log_init();
	free(win);
	free(abs);
	if (!w) {
		fprintf(stderr, "seance: %s: not a path seance can name\n",
			path);
		return 1;
	}
	hr = oa_LoadTypeLibEx(w, REGKIND_NONE, &lib);
	free(w);
	if (FAILED(hr)) {
		fprintf(stderr, "seance: %s: no type library (%#x)\n", path,
			(unsigned)hr);
		return 1;
	}
	unresolved = 0;
	lib->lpVtbl->GetLibAttr(lib, &la);
	lib->lpVtbl->GetDocumentation(lib, -1, &name, &doc, NULL, NULL);
	fputs("library ", stdout);
	put_bstr(name);
	putchar(' ');
	put_guid(&la->guid);
	printf(" version %u.%u lcid %#lx %s\n", la->wMajorVerNum,
	       la->wMinorVerNum, (unsigned long)la->lcid,
	       la->syskind == SYS_WIN64 ? "win64" : la->syskind == SYS_WIN32 ? "win32" : "win16");
	if (doc) {
		fputs("  \"", stdout);
		put_bstr(doc);
		puts("\"");
	}
	lib->lpVtbl->ReleaseTLibAttr(lib, la);
	oa_SysFreeString(name);
	oa_SysFreeString(doc);

	UINT n = lib->lpVtbl->GetTypeInfoCount(lib);
	for (UINT i = 0; i < n; i++) {
		ITypeInfo *ti;

		if (FAILED(lib->lpVtbl->GetTypeInfo(lib, i, &ti))) {
			printf("  type %u: unreadable\n", i);
			unresolved++;
			continue;
		}
		put_typeinfo(ti, i);
		ti->lpVtbl->Release(ti);
	}
	lib->lpVtbl->Release(lib);
	if (unresolved)
		fprintf(stderr, "seance: %s: %d references don't resolve\n",
			path, unresolved);
	return unresolved ? 1 : 0;
}
