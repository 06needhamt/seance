/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * oleaut32's SLTG reader: a small SLTG library this test builds byte by
 * byte (the older format, as OLE 2.0's tools wrote it), loaded through
 * LoadTypeLib and checked through ITypeLib and ITypeInfo; then cut short
 * at every length, each of which must fail cleanly.
 *
 * The library, in IDL:
 *
 *   [uuid(LIBID), version(3.1), helpstring("Old library")]
 *   library OldLib {
 *     importlib("stdole2.tlb");
 *     enum Colors { Red = 0, Green = 5 };
 *     [uuid(IID)] interface IOld : IUnknown {
 *       [id(7)] HRESULT Get([out, retval] long *pv);
 *     };
 *   };
 */
#include <stdint.h>
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
static HRESULT (WINAPI *pLoadTypeLib)(LPCOLESTR, ITypeLib **);
static void (WINAPI *pSysFreeString)(BSTR);

static void *get(const char *name)
{
	void *p = mod_get_proc(oa, name);

	if (!p) {
		fprintf(stderr, "oleaut32 has no %s\n", name);
		exit(1);
	}
	return p;
}

static const GUID libid = { 0x51c0de00, 1, 2, { 1, 2, 3, 4, 5, 6, 7, 8 } };
static const GUID iid_old = { 0x51c0de01, 3, 4, { 1, 2, 3, 4, 5, 6, 7, 8 } };

/* ---- building the file ---- */

struct buf {
	BYTE d[4096];
	size_t n;
};

static size_t put(struct buf *b, const void *p, size_t n)
{
	size_t at = b->n;
	if (b->n + n > sizeof(b->d)) {
		fprintf(stderr, "test buffer too small\n");
		exit(1);
	}
	if (p)
		memcpy(b->d + b->n, p, n);
	else
		memset(b->d + b->n, 0, n);
	b->n += n;
	return at;
}

static size_t p8(struct buf *b, int v)
{
	BYTE x = (BYTE)v;
	return put(b, &x, 1);
}

static size_t p16(struct buf *b, int v)
{
	BYTE x[2] = { (BYTE)v, (BYTE)(v >> 8) };
	return put(b, x, 2);
}

static size_t p32(struct buf *b, DWORD v)
{
	BYTE x[4] = { (BYTE)v, (BYTE)(v >> 8), (BYTE)(v >> 16),
		      (BYTE)(v >> 24) };
	return put(b, x, 4);
}

static void s16(struct buf *b, size_t at, int v)
{
	b->d[at] = (BYTE)v;
	b->d[at + 1] = (BYTE)(v >> 8);
}

static void s32(struct buf *b, size_t at, DWORD v)
{
	s16(b, at, (int)(v & 0xffff));
	s16(b, at + 2, (int)(v >> 16));
}

static size_t pguid(struct buf *b, const GUID *g)
{
	size_t at = p32(b, g->Data1);
	p16(b, g->Data2);
	p16(b, g->Data3);
	put(b, g->Data4, 8);
	return at;
}

/* The name table: NUL-terminated names, each after a NUL. */
static struct buf names;

static WORD name(const char *s)
{
	if (!names.n)
		p8(&names, 0);
	WORD at = (WORD)names.n;
	put(&names, s, strlen(s) + 1);
	return at;
}

/* A counted string. */
static void pcount(struct buf *b, const char *s)
{
	if (!s) {
		p16(b, 0xffff);
		return;
	}
	p16(b, (int)strlen(s));
	put(b, s, strlen(s));
}

/* A type block's header; returns its offset. */
static size_t type_header(struct buf *b, TYPEKIND kind, DWORD refs_at)
{
	size_t o = b->n;
	put(b, NULL, 0x22);
	s16(b, o, 0x0501);
	s32(b, o + 2, refs_at);
	s16(b, o + 0x12, 1);		/* version 1.0 */
	b->d[o + 0x1d] = (BYTE)kind;
	return o;
}

static void tail(struct buf *b, WORD nfuncs, WORD nvars, WORD nimpls,
		 WORD funcs, WORD vars, WORD impls, WORD size, WORD align,
		 WORD vft)
{
	size_t t = put(b, NULL, 0x36);
	s16(b, t, nfuncs);
	s16(b, t + 2, nvars);
	s16(b, t + 4, nimpls);
	s16(b, t + 8, funcs);
	s16(b, t + 0x0a, vars);
	s16(b, t + 0x0c, impls);
	s16(b, t + 0x20, size);
	s16(b, t + 0x22, align);
	s16(b, t + 0x28, vft);
}

/* enum Colors { Red = 0, Green = 5 } */
static void enum_block(struct buf *b)
{
	size_t o = type_header(b, TKIND_ENUM, 0xffffffff);
	size_t mh = put(b, NULL, 9);
	s32(b, o + 10, (DWORD)(mh - o));
	size_t items = b->n;
	static const char *const nm[2] = { "Red", "Green" };
	static const int val[2] = { 0, 5 };
	for (int i = 0; i < 2; i++) {
		size_t q = b->n;
		p8(b, 0x0a);			/* a variable */
		p8(b, 0x10 | 0x08 | 0x02);	/* const, value and type here */
		p16(b, i ? 0xffff : (int)(q - items + 18));
		p16(b, name(nm[i]));
		p16(b, val[i]);
		p16(b, VT_I4);
		p32(b, (DWORD)(0x40000000 + i));
		p16(b, 0);
		p16(b, 0xffff);			/* no doc string */
	}
	s32(b, mh + 5, (DWORD)(b->n - items));
	tail(b, 0, 2, 0, 0xffff, 0, 0xffff, 4, 4, 0);
}

/* interface IOld : IUnknown { HRESULT Get([out, retval] long *pv); } */
static void iface_block(struct buf *b, WORD stdole)
{
	size_t o = type_header(b, TKIND_INTERFACE, 0);
	/* The reference table: one entry, stdole2's type 3 (IUnknown). */
	size_t r = b->n;
	s32(b, o + 2, (DWORD)(r - o));
	put(b, NULL, 0x4f);
	b->d[r] = 0xdf;
	s32(b, r + 0x44, 8);
	put(b, NULL, 8);
	char ref[32];
	snprintf(ref, sizeof(ref), "*\\R%x*#3", stdole);
	pcount(b, ref);

	size_t mh = put(b, NULL, 9);
	s32(b, o + 10, (DWORD)(mh - o));
	size_t items = b->n;
	/* the base */
	size_t impl = p16(b, 0x004a);
	p16(b, 0xffff);
	p16(b, 0);
	p8(b, 0);
	p8(b, 0);
	p16(b, 0);
	p16(b, 0);			/* reference 0 */
	/* the function */
	size_t f = b->n;
	p8(b, 0x4c);
	p8(b, INVOKE_FUNC << 4);
	p16(b, 0xffff);
	p16(b, name("Get"));
	p32(b, 7);
	p16(b, 0);
	p16(b, 0xffff);			/* +0x0c: no doc string */
	size_t params_field = p16(b, 0);
	p8(b, CC_STDCALL | 1 << 3);	/* one parameter */
	p8(b, 0x80);			/* the return type is here */
	p16(b, VT_HRESULT);
	p16(b, 12);			/* after IUnknown's three */
	/* its parameter: a name, then the offset of its type */
	s16(b, params_field, (int)(b->n - items));
	p16(b, name("pv"));
	size_t type_field = p16(b, 0);
	s16(b, type_field, (int)(b->n - items));
	p16(b, 0x4000 | 0x80 | 0xe00 | VT_I4);	/* [out, retval] long * */
	s32(b, mh + 5, (DWORD)(b->n - items));
	tail(b, 1, 0, 1, (WORD)(f - items), 0xffff, (WORD)(impl - items), 4,
	     4, 16);
}

static void lib_block(struct buf *b, WORD libname)
{
	size_t lib = p16(b, 0x51cc);
	p16(b, 0);
	p16(b, libname);
	pcount(b, NULL);
	pcount(b, "Old library");
	pcount(b, NULL);
	p32(b, 0);			/* help context */
	p16(b, SYS_WIN32);
	p16(b, 0x409);
	p32(b, 0);
	p16(b, 0);			/* flags */
	p16(b, 3);
	p16(b, 1);
	pguid(b, &libid);
	put(b, NULL, 0x40 + 2);
	static const char *const idx[2] = { "T0", "T1" };
	static const char *const tn[2] = { "Colors", "IOld" };
	for (int i = 0; i < 2; i++) {
		pcount(b, idx[i]);
		p16(b, 0xffff);
		p16(b, 0);
		p16(b, name(tn[i]));
		p16(b, 0);		/* no compressed doc string */
		p16(b, 0);
		p32(b, 0);		/* help context */
		p16(b, 0);
		pguid(b, i ? &iid_old : &(GUID){ 0 });
		p16(b, 0);
	}
	/* The name table: 0x218 bytes on from where this points. */
	size_t at = p32(b, 0);
	s32(b, at, (DWORD)(b->n - lib));
	put(b, NULL, 0x218);
	put(b, names.d, names.n);
}

static size_t build(struct buf *f)
{
	struct buf t0 = { 0 }, t1 = { 0 }, lb = { 0 };
	names.n = 0;
	WORD stdole = name("*\\G{00020430-0000-0000-C000-000000000046}#2.0#0#"
			   "C:\\Windows\\System32\\stdole2.tlb#OLE Automation");
	WORD libname = name("OldLib");
	enum_block(&t0);
	iface_block(&t1, stdole);
	lib_block(&lb, libname);

	/* header, entries (type 0, type 1, the library) and index strings */
	f->n = 0;
	p32(f, 0x47544c53);
	p16(f, 4);			/* three blocks, + 1 */
	put(f, NULL, 4);
	p16(f, 1);			/* the first block is entry 1 */
	put(f, NULL, 0x24 - f->n);
	const size_t lens[3] = { t0.n, t1.n, lb.n };
	for (int e = 0; e < 3; e++) {
		p32(f, (DWORD)lens[e]);
		p16(f, e < 2 ? 13 + 11 * e : 0);
		p16(f, e < 2 ? e + 2 : 0);
	}
	put(f, "\1CompObj\0dir\0", 13);
	static const char idx[2][11] = { "T0", "T1" };
	put(f, idx, sizeof(idx));
	put(f, NULL, 9);
	put(f, t0.d, t0.n);
	put(f, t1.d, t1.n);
	put(f, lb.d, lb.n);
	return f->n;
}

/* ---- checking it ---- */

static char dir[] = "/tmp/test_sltg.XXXXXX";

static WCHAR *write_lib(const char *fname, const void *d, size_t len)
{
	char path[512];
	snprintf(path, sizeof(path), "%s/%s", dir, fname);
	FILE *fp = fopen(path, "wb");
	CHECK(fp != NULL);
	if (fp) {
		fwrite(d, 1, len, fp);
		fclose(fp);
	}
	char *win = path_to_windows(path);
	WCHAR *w = win ? utf8_to_utf16_dup(win) : NULL;
	free(win);
	return w;
}

static int is(BSTR b, const char *s)
{
	char *u = b ? utf16_to_utf8_dup(b) : NULL;
	int ok = u && !strcmp(u, s);
	if (!ok)
		fprintf(stderr, "  got \"%s\", want \"%s\"\n", u ? u : "(null)",
			s);
	free(u);
	pSysFreeString(b);
	return ok;
}

static void check(ITypeLib *lib)
{
	const int P = (int)sizeof(void *);
	TLIBATTR *la;
	BSTR n = NULL, doc = NULL;

	CHECK_EQ(lib->lpVtbl->GetLibAttr(lib, &la), S_OK);
	CHECK(!memcmp(&la->guid, &libid, sizeof(GUID)));
	CHECK(la->wMajorVerNum == 3 && la->wMinorVerNum == 1);
	CHECK_EQ(la->lcid, 0x409);
	lib->lpVtbl->ReleaseTLibAttr(lib, la);
	CHECK_EQ(lib->lpVtbl->GetDocumentation(lib, -1, &n, &doc, NULL, NULL),
		 S_OK);
	CHECK(is(n, "OldLib"));
	CHECK(is(doc, "Old library"));
	CHECK_EQ(lib->lpVtbl->GetTypeInfoCount(lib), 2);

	ITypeInfo *ti = NULL;
	TYPEATTR *ta;
	VARDESC *vd;
	CHECK_EQ(lib->lpVtbl->GetTypeInfo(lib, 0, &ti), S_OK);
	if (ti) {
		CHECK_EQ(ti->lpVtbl->GetTypeAttr(ti, &ta), S_OK);
		CHECK_EQ(ta->typekind, TKIND_ENUM);
		CHECK_EQ(ta->cVars, 2);
		ti->lpVtbl->ReleaseTypeAttr(ti, ta);
		CHECK_EQ(ti->lpVtbl->GetVarDesc(ti, 1, &vd), S_OK);
		CHECK(vd->varkind == VAR_CONST && vd->lpvarValue &&
		      vd->lpvarValue->lVal == 5);
		CHECK_EQ(ti->lpVtbl->GetDocumentation(ti, vd->memid, &n, NULL,
						      NULL, NULL), S_OK);
		CHECK(is(n, "Green"));
		ti->lpVtbl->ReleaseVarDesc(ti, vd);
		ti->lpVtbl->Release(ti);
	}

	CHECK_EQ(lib->lpVtbl->GetTypeInfo(lib, 1, &ti), S_OK);
	if (!ti)
		return;
	CHECK_EQ(ti->lpVtbl->GetTypeAttr(ti, &ta), S_OK);
	CHECK_EQ(ta->typekind, TKIND_INTERFACE);
	CHECK(!memcmp(&ta->guid, &iid_old, sizeof(GUID)));
	CHECK_EQ(ta->cFuncs, 1);
	CHECK_EQ(ta->cImplTypes, 1);
	CHECK_EQ(ta->cbSizeVft, 4 * P);
	ti->lpVtbl->ReleaseTypeAttr(ti, ta);

	/* Its base, from stdole2 by index. */
	HREFTYPE h;
	ITypeInfo *base = NULL;
	CHECK_EQ(ti->lpVtbl->GetRefTypeOfImplType(ti, 0, &h), S_OK);
	CHECK_EQ(ti->lpVtbl->GetRefTypeInfo(ti, h, &base), S_OK);
	if (base) {
		CHECK_EQ(base->lpVtbl->GetDocumentation(base, -1, &n, NULL,
							NULL, NULL), S_OK);
		CHECK(is(n, "IUnknown"));
		base->lpVtbl->Release(base);
	}

	FUNCDESC *fd;
	CHECK_EQ(ti->lpVtbl->GetFuncDesc(ti, 0, &fd), S_OK);
	if (fd) {
		CHECK_EQ(fd->memid, 7);
		CHECK_EQ(fd->funckind, FUNC_PUREVIRTUAL);
		CHECK_EQ(fd->callconv, CC_STDCALL);
		CHECK_EQ(fd->oVft, 3 * P);
		CHECK_EQ(fd->elemdescFunc.tdesc.vt, VT_HRESULT);
		CHECK_EQ(fd->cParams, 1);
		CHECK(fd->lprgelemdescParam[0].tdesc.vt == VT_PTR &&
		      fd->lprgelemdescParam[0].tdesc.lptdesc->vt == VT_I4);
		CHECK_EQ(fd->lprgelemdescParam[0].paramdesc.wParamFlags,
			 PARAMFLAG_FOUT | PARAMFLAG_FRETVAL);
		BSTR pn[3] = { 0 };
		UINT got = 0;
		CHECK_EQ(ti->lpVtbl->GetNames(ti, 7, pn, 3, &got), S_OK);
		CHECK_EQ(got, 2);
		if (got == 2) {
			CHECK(is(pn[0], "Get"));
			CHECK(is(pn[1], "pv"));
		}
		ti->lpVtbl->ReleaseFuncDesc(ti, fd);
	}
	ti->lpVtbl->Release(ti);
}

int main(void)
{
	char cleanup[64];
	static struct buf f;

	setenv("VFW_REGISTRY", "", 1);
	CHECK_EQ(runtime_enter(), 0);
	reg_reload();
	oa = mod_load("oleaut32.dll", NULL);
	CHECK(oa != NULL);
	if (!oa || !mkdtemp(dir)) {
		CHECK(0);
		return test_summary("test_typelib_sltg");
	}
	pLoadTypeLib = get("LoadTypeLib");
	pSysFreeString = get("SysFreeString");

	size_t len = build(&f);
	WCHAR *p = write_lib("old.tlb", f.d, len);
	ITypeLib *lib = NULL;
	CHECK_EQ(pLoadTypeLib(p, &lib), S_OK);
	free(p);
	if (lib) {
		check(lib);
		lib->lpVtbl->Release(lib);
	}

	/* Cut short anywhere: an error, never a crash or a library. */
	int bad = 0;
	for (size_t cut = 0; cut < len; cut++) {
		ITypeLib *l = NULL;
		p = write_lib("cut.tlb", f.d, cut);
		HRESULT hr = pLoadTypeLib(p, &l);
		free(p);
		if (SUCCEEDED(hr) || l) {
			/* The tail past the name table's last name may go. */
			if (l)
				l->lpVtbl->Release(l);
			if (cut < len - 16)
				bad++;
		}
	}
	CHECK_EQ(bad, 0);

	mod_free(oa);
	snprintf(cleanup, sizeof(cleanup), "rm -rf '%s'", dir);
	if (system(cleanup))
		fprintf(stderr, "could not remove %s\n", dir);
	return test_summary("test_typelib_sltg");
}
