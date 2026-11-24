/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * ole32's monikers, bind contexts and running object table, called through
 * its export table: building, composing, comparing and saving monikers,
 * parsing display names, and binding through the running object table, an
 * item container, and a class registered for a file extension.
 */
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "loader/modules.h"
#include "runtime/lowmem.h"
#include "runtime/runtime.h"
#include "win32/codepage.h"
#include "win32/internal.h"
#include "win32/path.h"
#include "win32/reg.h"
#include "test.h"

static struct module *ole;

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
		(ret (WINAPI *)(__VA_ARGS__))get_from(ole, #name)

static const IID iid_unknown = { 0, 0, 0,
	{ 0xc0, 0, 0, 0, 0, 0, 0, 0x46 } };
static const IID iid_factory = { 1, 0, 0,
	{ 0xc0, 0, 0, 0, 0, 0, 0, 0x46 } };
static const IID iid_other = { 0x12345678, 0x9abc, 0xdef0,
	{ 1, 2, 3, 4, 5, 6, 7, 8 } };
/* The document class, registered for ".tdoc". */
static const CLSID clsid_doc = { 0x3f2a1b0c, 0x1d2e, 0x4f30,
	{ 0x81, 0x92, 0xa3, 0xb4, 0xc5, 0xd6, 0xe7, 0xf8 } };
#define CLSID_DOC "{3F2A1B0C-1D2E-4F30-8192-A3B4C5D6E7F8}"

static WCHAR *wide(const char *s)
{
	return utf8_to_utf16_dup(s);
}

/* Whether w is s, in UTF-8. */
static int wis(const WCHAR *w, const char *s)
{
	char *u = w ? utf16_to_utf8_dup(w) : NULL;
	int r = u && !strcmp(u, s);
	if (!r)
		fprintf(stderr, "  got \"%s\", want \"%s\"\n", u ? u : "(null)",
			s);
	free(u);
	return r;
}

static void set_class_value(const char *key, const char *value)
{
	WCHAR *w = wide(value);
	LONG err = 0;

	reg_lock();
	struct reg_key *k = reg_create(key, 0, NULL, &err);
	CHECK(k && w);
	if (k && w)
		CHECK_EQ(reg_set(k, "", REG_SZ, w,
				 (DWORD)((wstrlen(w) + 1) * sizeof(WCHAR))), 0);
	reg_unlock();
	free(w);
}

/* ---- a plain object ---- */

struct obj {
	IUnknown iface;
	LONG refs;
};

WINAPI static HRESULT obj_qi(IUnknown *self, REFIID riid, void **ppv)
{
	if (!memcmp(riid, &iid_unknown, sizeof(GUID)) ||
	    !memcmp(riid, &iid_other, sizeof(GUID))) {
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

/* ---- an item container: items are named "Sheet<n>" ---- */

static struct obj sheet = { { &obj_vtbl }, 0 };
static int container_refs, container_gets;
static IMoniker *(*make_item)(const WCHAR *delim, const WCHAR *name);

WINAPI static HRESULT ct_qi(IOleItemContainer *self, REFIID riid, void **ppv)
{
	if (!memcmp(riid, &iid_unknown, sizeof(GUID)) ||
	    !memcmp(riid, &IID_IOleItemContainer, sizeof(GUID)) ||
	    !memcmp(riid, &IID_IOleContainer, sizeof(GUID)) ||
	    !memcmp(riid, &IID_IParseDisplayName, sizeof(GUID))) {
		*ppv = self;
		container_refs++;
		return S_OK;
	}
	*ppv = NULL;
	return E_NOINTERFACE;
}

WINAPI static ULONG ct_addref(IOleItemContainer *self)
{
	return ++container_refs;
}

WINAPI static ULONG ct_release(IOleItemContainer *self)
{
	return --container_refs;
}

/* "!Name" up to the next "!". */
WINAPI static HRESULT ct_parse(IOleItemContainer *self, IBindCtx *bc,
			       LPOLESTR name, ULONG *eaten, IMoniker **out)
{
	*out = NULL;
	*eaten = 0;
	if (name[0] != '!')
		return MK_E_SYNTAX;
	ULONG n = 1;
	while (name[n] && name[n] != '!' && name[n] != '?')
		n++;
	WCHAR delim[2] = { '!', 0 }, item[64];
	if (n - 1 >= 64)
		return MK_E_SYNTAX;
	memcpy(item, name + 1, (n - 1) * sizeof(WCHAR));
	item[n - 1] = 0;
	*out = make_item(delim, item);
	*eaten = n;
	return *out ? S_OK : E_FAIL;
}

WINAPI static HRESULT ct_enum(IOleItemContainer *self, DWORD f, void **e)
{
	return E_NOTIMPL;
}

WINAPI static HRESULT ct_lock(IOleItemContainer *self, BOOL lock)
{
	return S_OK;
}

static int is_sheet(const WCHAR *s)
{
	return s[0] == 'S' && s[1] == 'h' && s[2] == 'e' && s[3] == 'e' &&
	       s[4] == 't' && s[5] >= '0' && s[5] <= '9' && !s[6];
}

WINAPI static HRESULT ct_get(IOleItemContainer *self, LPOLESTR item,
			     DWORD speed, IBindCtx *bc, REFIID riid,
			     void **ppv)
{
	container_gets++;
	if (!is_sheet(item))
		return MK_E_NOOBJECT;
	return obj_qi(&sheet.iface, riid, ppv);
}

WINAPI static HRESULT ct_storage(IOleItemContainer *self, LPOLESTR item,
				 IBindCtx *bc, REFIID riid, void **ppv)
{
	return MK_E_NOSTORAGE;
}

WINAPI static HRESULT ct_running(IOleItemContainer *self, LPOLESTR item)
{
	return is_sheet(item) && item[5] == '1' ? S_OK : S_FALSE;
}

static const IOleItemContainerVtbl ct_vtbl = {
	ct_qi, ct_addref, ct_release, ct_parse, ct_enum, ct_lock, ct_get,
	ct_storage, ct_running,
};
static IOleItemContainer container = { &ct_vtbl };

/* ---- the document class: an IPersistFile that records what it loads ---- */

struct doc {
	IPersistFile iface;
	LONG refs;
	WCHAR loaded[260];
	DWORD mode;
};

static struct doc *last_doc;
static int docs_alive;

WINAPI static HRESULT doc_qi(IPersistFile *self, REFIID riid, void **ppv)
{
	if (!memcmp(riid, &iid_unknown, sizeof(GUID)) ||
	    !memcmp(riid, &IID_IPersistFile, sizeof(GUID)) ||
	    !memcmp(riid, &IID_IPersist, sizeof(GUID))) {
		*ppv = self;
		((struct doc *)self)->refs++;
		return S_OK;
	}
	*ppv = NULL;
	return E_NOINTERFACE;
}

WINAPI static ULONG doc_addref(IPersistFile *self)
{
	return ++((struct doc *)self)->refs;
}

WINAPI static ULONG doc_release(IPersistFile *self)
{
	struct doc *d = (struct doc *)self;
	ULONG r = --d->refs;
	if (!r) {
		if (last_doc == d)
			last_doc = NULL;
		docs_alive--;
		free(d);
	}
	return r;
}

WINAPI static HRESULT doc_classid(IPersistFile *self, CLSID *c)
{
	*c = clsid_doc;
	return S_OK;
}

WINAPI static HRESULT doc_dirty(IPersistFile *self)
{
	return S_FALSE;
}

WINAPI static HRESULT doc_load(IPersistFile *self, LPCOLESTR name, DWORD mode)
{
	struct doc *d = (struct doc *)self;
	size_t n = wstrlen(name);
	if (n >= 260)
		return E_FAIL;
	memcpy(d->loaded, name, (n + 1) * sizeof(WCHAR));
	d->mode = mode;
	return S_OK;
}

WINAPI static HRESULT doc_save(IPersistFile *self, LPCOLESTR name, BOOL r)
{
	return E_NOTIMPL;
}

WINAPI static HRESULT doc_saved(IPersistFile *self, LPCOLESTR name)
{
	return S_OK;
}

WINAPI static HRESULT doc_curfile(IPersistFile *self, LPOLESTR *name)
{
	return E_NOTIMPL;
}

static const IPersistFileVtbl doc_vtbl = {
	doc_qi, doc_addref, doc_release, doc_classid, doc_dirty, doc_load,
	doc_save, doc_saved, doc_curfile,
};

WINAPI static HRESULT df_qi(IClassFactory *self, REFIID riid, void **ppv)
{
	if (!memcmp(riid, &iid_unknown, sizeof(GUID)) ||
	    !memcmp(riid, &iid_factory, sizeof(GUID))) {
		*ppv = self;
		return S_OK;
	}
	*ppv = NULL;
	return E_NOINTERFACE;
}

WINAPI static ULONG df_addref(IClassFactory *self)
{
	return 2;
}

WINAPI static ULONG df_release(IClassFactory *self)
{
	return 1;
}

WINAPI static HRESULT df_create(IClassFactory *self, IUnknown *outer,
				REFIID riid, void **ppv)
{
	struct doc *d = calloc(1, sizeof(*d));
	if (!d)
		return E_OUTOFMEMORY;
	d->iface.lpVtbl = &doc_vtbl;
	d->refs = 1;
	docs_alive++;
	last_doc = d;
	HRESULT hr = doc_qi(&d->iface, riid, ppv);
	doc_release(&d->iface);
	return hr;
}

WINAPI static HRESULT df_lock(IClassFactory *self, BOOL lock)
{
	return S_OK;
}

static const IClassFactoryVtbl df_vtbl = {
	df_qi, df_addref, df_release, df_create, df_lock,
};
static IClassFactory doc_factory = { &df_vtbl };

/* ---- the tests ---- */

static IMoniker *item_of(const WCHAR *delim, const WCHAR *name)
{
	FN(HRESULT, CreateItemMoniker, LPCOLESTR, LPCOLESTR, IMoniker **);
	IMoniker *m = NULL;
	CreateItemMoniker(delim, name, &m);
	return m;
}

static IMoniker *file_of(const char *path)
{
	FN(HRESULT, CreateFileMoniker, LPCOLESTR, IMoniker **);
	WCHAR *w = wide(path);
	IMoniker *m = NULL;
	CHECK_EQ(CreateFileMoniker(w, &m), S_OK);
	free(w);
	return m;
}

static IMoniker *item(const char *name)
{
	WCHAR *w = wide(name), bang[2] = { '!', 0 };
	IMoniker *m = item_of(bang, w);
	free(w);
	return m;
}

/* m's display name is s. */
static int named(IMoniker *m, const char *s)
{
	FN(void, CoTaskMemFree, LPVOID);
	LPOLESTR n = NULL;
	if (!m || FAILED(m->lpVtbl->GetDisplayName(m, NULL, NULL, &n)))
		return 0;
	int r = wis(n, s);
	CoTaskMemFree(n);
	return r;
}

static void release(IMoniker *m)
{
	if (m)
		m->lpVtbl->Release(m);
}

static void test_bind_ctx(void)
{
	FN(HRESULT, CreateBindCtx, DWORD, IBindCtx **);
	FN(void, CoTaskMemFree, LPVOID);
	IBindCtx *bc;
	BIND_OPTS2 o;
	struct obj a = { { &obj_vtbl }, 1 };
	IUnknown *u;

	CHECK_EQ(CreateBindCtx(1, &bc), E_INVALIDARG);
	CHECK_EQ(CreateBindCtx(0, &bc), S_OK);
	memset(&o, 0, sizeof(o));
	o.cbStruct = sizeof(o);
	CHECK_EQ(bc->lpVtbl->GetBindOptions(bc, (BIND_OPTS *)&o), S_OK);
	CHECK_EQ(o.cbStruct, sizeof(o));
	CHECK_EQ(o.grfMode, STGM_READWRITE);
	CHECK_EQ(o.dwClassContext, CLSCTX_SERVER);
	/* A shorter struct reads and writes only its own fields. */
	BIND_OPTS s = { sizeof(s), BIND_MAYBOTHERUSER, STGM_READ, 1234 };
	CHECK_EQ(bc->lpVtbl->SetBindOptions(bc, &s), S_OK);
	memset(&o, 0, sizeof(o));
	o.cbStruct = sizeof(o);
	CHECK_EQ(bc->lpVtbl->GetBindOptions(bc, (BIND_OPTS *)&o), S_OK);
	CHECK(o.grfFlags == BIND_MAYBOTHERUSER && o.grfMode == STGM_READ &&
	      o.dwTickCountDeadline == 1234 &&
	      o.dwClassContext == CLSCTX_SERVER);
	s.cbStruct = 4;
	CHECK_EQ(bc->lpVtbl->SetBindOptions(bc, &s), E_INVALIDARG);

	/* Bound objects are held until released. */
	CHECK_EQ(bc->lpVtbl->RegisterObjectBound(bc, &a.iface), S_OK);
	CHECK_EQ(a.refs, 2);
	CHECK_EQ(bc->lpVtbl->RevokeObjectBound(bc, &a.iface), S_OK);
	CHECK_EQ(a.refs, 1);
	CHECK_EQ(bc->lpVtbl->RevokeObjectBound(bc, &a.iface), MK_E_NOTBOUND);
	CHECK_EQ(bc->lpVtbl->RegisterObjectBound(bc, &a.iface), S_OK);
	CHECK_EQ(bc->lpVtbl->ReleaseBoundObjects(bc), S_OK);
	CHECK_EQ(a.refs, 1);

	/* Parameters by key. */
	WCHAR *k1 = wide("First"), *k2 = wide("Second");
	CHECK_EQ(bc->lpVtbl->RegisterObjectParam(bc, k1, &a.iface), S_OK);
	CHECK_EQ(bc->lpVtbl->RegisterObjectParam(bc, k2, &a.iface), S_OK);
	CHECK_EQ(a.refs, 3);
	CHECK_EQ(bc->lpVtbl->GetObjectParam(bc, k1, &u), S_OK);
	CHECK(u == &a.iface);
	u->lpVtbl->Release(u);
	IEnumString *e;
	LPOLESTR keys[3] = { 0 };
	ULONG got = 0;
	CHECK_EQ(bc->lpVtbl->EnumObjectParam(bc, &e), S_OK);
	CHECK_EQ(e->lpVtbl->Next(e, 3, keys, &got), S_FALSE);
	CHECK_EQ(got, 2);
	CHECK(wis(keys[0], "First") && wis(keys[1], "Second"));
	CoTaskMemFree(keys[0]);
	CoTaskMemFree(keys[1]);
	e->lpVtbl->Release(e);
	CHECK_EQ(bc->lpVtbl->RevokeObjectParam(bc, k1), S_OK);
	CHECK_EQ(bc->lpVtbl->RevokeObjectParam(bc, k1), S_FALSE);
	CHECK_EQ(bc->lpVtbl->GetObjectParam(bc, k1, &u), E_FAIL);
	CHECK(u == NULL);
	CHECK_EQ(a.refs, 2);

	IRunningObjectTable *rot = NULL;
	CHECK_EQ(bc->lpVtbl->GetRunningObjectTable(bc, &rot), S_OK);
	CHECK(rot != NULL);
	if (rot)
		rot->lpVtbl->Release(rot);
	bc->lpVtbl->Release(bc);
	CHECK_EQ(a.refs, 1);	/* the remaining parameter went with it */
	free(k1);
	free(k2);
}

static void test_simple(void)
{
	FN(HRESULT, CreateAntiMoniker, IMoniker **);
	FN(HRESULT, CreateClassMoniker, REFCLSID, IMoniker **);
	FN(HRESULT, CreatePointerMoniker, IUnknown *, IMoniker **);
	FN(HRESULT, CreateGenericComposite, IMoniker *, IMoniker *,
	   IMoniker **);
	FN(HRESULT, MonikerCommonPrefixWith, IMoniker *, IMoniker *,
	   IMoniker **);
	FN(HRESULT, MonikerRelativePathTo, IMoniker *, IMoniker *, IMoniker **,
	   BOOL);
	FN(HRESULT, BindMoniker, IMoniker *, DWORD, REFIID, void **);
	IMoniker *f = file_of("C:\\a\\b\\c.doc"), *g, *c = NULL, *anti, *r;
	DWORD sys, h1, h2;

	CHECK(named(f, "C:\\a\\b\\c.doc"));
	CHECK_EQ(f->lpVtbl->IsSystemMoniker(f, &sys), S_OK);
	CHECK_EQ(sys, MKSYS_FILEMONIKER);
	g = file_of("c:\\A\\B\\C.DOC");
	CHECK_EQ(f->lpVtbl->IsEqual(f, g), S_OK);
	f->lpVtbl->Hash(f, &h1);
	g->lpVtbl->Hash(g, &h2);
	CHECK_EQ(h1, h2);
	release(g);

	/* A relative file joins the one to its left. */
	g = file_of("..\\d.doc");
	CHECK_EQ(f->lpVtbl->ComposeWith(f, g, TRUE, &c), S_OK);
	CHECK(named(c, "C:\\a\\b\\d.doc"));
	release(c);
	release(g);
	g = file_of("..\\..\\..\\..\\..\\x");
	CHECK_EQ(f->lpVtbl->ComposeWith(f, g, TRUE, &c), S_OK);
	CHECK(named(c, "..\\x"));
	release(c);
	release(g);
	g = file_of("D:\\abs");
	c = (IMoniker *)1;
	CHECK_EQ(f->lpVtbl->ComposeWith(f, g, TRUE, &c), MK_E_SYNTAX);
	CHECK(c == NULL);
	release(g);

	/* An anti moniker cancels one moniker; an inverse cancels all. */
	CHECK_EQ(CreateAntiMoniker(&anti), S_OK);
	CHECK(named(anti, "\\.."));
	c = (IMoniker *)1;
	CHECK_EQ(CreateGenericComposite(f, anti, &c), S_OK);
	CHECK(c == NULL);
	CHECK_EQ(f->lpVtbl->Inverse(f, &r), S_OK);
	CHECK_EQ(r->lpVtbl->IsEqual(r, anti), S_OK);
	release(r);
	CHECK_EQ(anti->lpVtbl->Inverse(anti, &r), MK_E_NOINVERSE);
	CHECK_EQ(CreateGenericComposite(anti, anti, &c), S_OK);
	CHECK(named(c, "\\..\\.."));
	release(c);

	/* Prefixes and relative paths between files. */
	g = file_of("C:\\a\\x.doc");
	CHECK_EQ(MonikerCommonPrefixWith(f, g, &c), S_OK);
	CHECK(named(c, "C:\\a"));
	release(c);
	CHECK_EQ(MonikerRelativePathTo(f, g, &r, TRUE), S_OK);
	CHECK(named(r, "..\\..\\x.doc"));
	CHECK_EQ(f->lpVtbl->ComposeWith(f, r, TRUE, &c), S_OK);
	CHECK_EQ(c->lpVtbl->IsEqual(c, g), S_OK);
	release(c);
	release(r);
	release(g);
	g = file_of("C:\\a");
	CHECK_EQ(MonikerCommonPrefixWith(f, g, &c), MK_S_HIM);
	CHECK(c == g);
	release(c);
	CHECK_EQ(MonikerCommonPrefixWith(g, f, &c), MK_S_ME);
	release(c);
	release(g);
	g = file_of("E:\\other");
	CHECK_EQ(MonikerCommonPrefixWith(f, g, &c), MK_E_NOPREFIX);
	CHECK_EQ(MonikerRelativePathTo(f, g, &r, TRUE), MK_S_HIM);
	CHECK(r == g);
	release(r);
	release(g);

	/* Items compare by name, whatever the case and delimiter. */
	IMoniker *i1 = item("Sheet1"), *i2;
	WCHAR slash[2] = { '/', 0 }, *nm = wide("SHEET1");
	i2 = item_of(slash, nm);
	free(nm);
	CHECK(named(i1, "!Sheet1") && named(i2, "/SHEET1"));
	CHECK_EQ(i1->lpVtbl->IsEqual(i1, i2), S_OK);
	CHECK_EQ(i1->lpVtbl->IsSystemMoniker(i1, &sys), S_OK);
	CHECK_EQ(sys, MKSYS_ITEMMONIKER);
	c = (IMoniker *)1;
	CHECK_EQ(i1->lpVtbl->ComposeWith(i1, i2, TRUE, &c), MK_E_NEEDGENERIC);
	CHECK(c == NULL);
	release(i2);

	/* A composite: flat, enumerable, and cancelled by its inverse. */
	IMoniker *i3 = item("Cell"), *fc, *all, *inv;
	CHECK_EQ(CreateGenericComposite(f, i1, &fc), S_OK);
	CHECK_EQ(CreateGenericComposite(fc, i3, &all), S_OK);
	CHECK(named(all, "C:\\a\\b\\c.doc!Sheet1!Cell"));
	CHECK_EQ(all->lpVtbl->IsSystemMoniker(all, &sys), S_OK);
	CHECK_EQ(sys, MKSYS_GENERICCOMPOSITE);
	IEnumMoniker *e;
	IMoniker *parts[4] = { 0 };
	ULONG got = 0;
	CHECK_EQ(all->lpVtbl->Enum(all, FALSE, &e), S_OK);
	CHECK_EQ(e->lpVtbl->Next(e, 4, parts, &got), S_FALSE);
	CHECK_EQ(got, 3);
	CHECK(got == 3 && parts[0]->lpVtbl->IsEqual(parts[0], i3) == S_OK &&
	      parts[2]->lpVtbl->IsEqual(parts[2], f) == S_OK);
	for (ULONG i = 0; i < got; i++)
		release(parts[i]);
	e->lpVtbl->Release(e);
	CHECK_EQ(f->lpVtbl->Enum(f, TRUE, &e), S_OK);
	CHECK(e == NULL);
	CHECK_EQ(CreateGenericComposite(all, anti, &c), S_OK);
	CHECK_EQ(c->lpVtbl->IsEqual(c, fc), S_OK);
	release(c);
	CHECK_EQ(all->lpVtbl->Inverse(all, &inv), S_OK);
	CHECK(named(inv, "\\..\\..\\.."));
	c = (IMoniker *)1;
	CHECK_EQ(CreateGenericComposite(all, inv, &c), S_OK);
	CHECK(c == NULL);
	release(inv);
	CHECK_EQ(MonikerCommonPrefixWith(all, fc, &c), MK_S_HIM);
	release(c);
	CHECK_EQ(MonikerCommonPrefixWith(fc, all, &c), MK_S_ME);
	release(c);
	CHECK_EQ(MonikerCommonPrefixWith(all, all, &c), MK_S_US);
	release(c);
	IMoniker *i4 = item("Other"), *f4;
	CHECK_EQ(CreateGenericComposite(fc, i4, &f4), S_OK);
	CHECK_EQ(MonikerCommonPrefixWith(all, f4, &c), S_OK);
	CHECK_EQ(c->lpVtbl->IsEqual(c, fc), S_OK);
	release(c);
	CHECK_EQ(MonikerRelativePathTo(all, f4, &r, TRUE), S_OK);
	CHECK(named(r, "\\..!Other"));
	release(r);
	release(f4);
	release(i4);
	release(all);
	release(fc);
	release(i3);
	release(i1);

	/* Class and pointer monikers. */
	IMoniker *cm;
	CHECK_EQ(CreateClassMoniker(&clsid_doc, &cm), S_OK);
	CHECK(named(cm, "clsid:3F2A1B0C-1D2E-4F30-8192-A3B4C5D6E7F8:"));
	CHECK_EQ(cm->lpVtbl->IsSystemMoniker(cm, &sys), S_OK);
	CHECK_EQ(sys, MKSYS_CLASSMONIKER);
	release(cm);
	struct obj o = { { &obj_vtbl }, 1 };
	IMoniker *pm;
	void *p;
	CHECK_EQ(CreatePointerMoniker(&o.iface, &pm), S_OK);
	CHECK_EQ(o.refs, 2);
	CHECK_EQ(BindMoniker(pm, 0, &iid_other, &p), S_OK);
	CHECK(p == &o);
	obj_release(&o.iface);
	CHECK_EQ(pm->lpVtbl->IsRunning(pm, NULL, NULL, NULL), S_OK);
	LPOLESTR n;
	CHECK_EQ(pm->lpVtbl->GetDisplayName(pm, NULL, NULL, &n), E_NOTIMPL);
	release(pm);
	CHECK_EQ(o.refs, 1);

	release(anti);
	release(f);
}

static void test_persist(void)
{
	FN(HRESULT, CreateStreamOnHGlobal, HGLOBAL, BOOL, IStream **);
	FN(HRESULT, OleSaveToStream, IPersistStream *, IStream *);
	FN(HRESULT, OleLoadFromStream, IStream *, REFIID, void **);
	FN(HRESULT, CreateGenericComposite, IMoniker *, IMoniker *,
	   IMoniker **);
	FN(HRESULT, CreateClassMoniker, REFCLSID, IMoniker **);
	FN(HRESULT, CreateAntiMoniker, IMoniker **);
	IMoniker *f = file_of("..\\..\\dir\\Datei\xc3\xa4\xe4\xb8\xad.doc");
	IMoniker *i = item("Blatt\xe4\xb8\xad"), *cm, *anti, *a, *b, *all;
	IStream *stm;
	LARGE_INTEGER zero = { 0 };

	CHECK_EQ(CreateClassMoniker(&clsid_doc, &cm), S_OK);
	CHECK_EQ(CreateAntiMoniker(&anti), S_OK);
	CHECK_EQ(CreateGenericComposite(f, i, &a), S_OK);
	CHECK_EQ(CreateGenericComposite(cm, anti, &b), S_OK);	/* NULL */
	CHECK(b == NULL);
	CHECK_EQ(CreateGenericComposite(anti, cm, &b), S_OK);
	/* An anti moniker cancels the moniker before it... */
	CHECK_EQ(CreateGenericComposite(a, b, &all), S_OK);
	CHECK(named(all, "..\\..\\dir\\Datei\xc3\xa4\xe4\xb8\xad.doc"
			 "clsid:3F2A1B0C-1D2E-4F30-8192-A3B4C5D6E7F8:"));
	release(all);
	/* ...and stays when nothing is before it. */
	CHECK_EQ(CreateGenericComposite(b, a, &all), S_OK);
	CHECK(named(all, "\\..clsid:3F2A1B0C-1D2E-4F30-8192-A3B4C5D6E7F8:"
			 "..\\..\\dir\\Datei\xc3\xa4\xe4\xb8\xad.doc!Blatt"
			 "\xe4\xb8\xad"));

	CHECK_EQ(CreateStreamOnHGlobal(NULL, TRUE, &stm), S_OK);
	CHECK_EQ(OleSaveToStream((IPersistStream *)all, stm), S_OK);
	ULARGE_INTEGER max;
	CHECK_EQ(all->lpVtbl->GetSizeMax(all, &max), S_OK);
	ULARGE_INTEGER pos;
	stm->lpVtbl->Seek(stm, zero, 1, &pos);
	CHECK(pos.QuadPart <= max.QuadPart + 16);
	stm->lpVtbl->Seek(stm, zero, 0, NULL);
	IMoniker *back = NULL;
	CHECK_EQ(OleLoadFromStream(stm, &IID_IMoniker, (void **)&back), S_OK);
	CHECK(back && back->lpVtbl->IsEqual(back, all) == S_OK);
	CHECK(named(back, "\\..clsid:3F2A1B0C-1D2E-4F30-8192-A3B4C5D6E7F8:"
			  "..\\..\\dir\\Datei\xc3\xa4\xe4\xb8\xad.doc!Blatt"
			  "\xe4\xb8\xad"));
	release(back);

	/* The file moniker's bytes, as Windows writes them. */
	IMoniker *plain = file_of("..\\x.y");
	stm->lpVtbl->SetSize(stm, (ULARGE_INTEGER){ 0 });
	stm->lpVtbl->Seek(stm, zero, 0, NULL);
	CHECK_EQ(plain->lpVtbl->Save(plain, stm, TRUE), S_OK);
	{
		BYTE want[38] = { 1, 0, 4, 0, 0, 0, 'x', '.', 'y', 0, 0xff,
				  0xff, 0xad, 0xde };
		BYTE got[64];
		ULONG n = 0;
		stm->lpVtbl->Seek(stm, zero, 1, &pos);
		CHECK_EQ(pos.QuadPart, sizeof(want));
		stm->lpVtbl->Seek(stm, zero, 0, NULL);
		stm->lpVtbl->Read(stm, got, sizeof(got), &n);
		CHECK(n == sizeof(want) && !memcmp(got, want, sizeof(want)));
	}
	release(plain);
	stm->lpVtbl->Release(stm);

	release(all);
	release(b);
	release(a);
	release(anti);
	release(cm);
	release(i);
	release(f);
}

static void test_rot(void)
{
	FN(HRESULT, GetRunningObjectTable, DWORD, IRunningObjectTable **);
	IRunningObjectTable *rot;
	struct obj o = { { &obj_vtbl }, 1 };
	IMoniker *f = file_of("C:\\run\\doc.tdoc"), *g, *m;
	DWORD c1 = 0, c2 = 0;
	IUnknown *u;
	FILETIME t, t2 = { 1234, 5678 };

	CHECK_EQ(GetRunningObjectTable(0, &rot), S_OK);
	CHECK_EQ(rot->lpVtbl->IsRunning(rot, f), S_FALSE);
	CHECK_EQ(rot->lpVtbl->Register(rot, 0, &o.iface, f, &c1), S_OK);
	CHECK(c1 != 0);
	CHECK_EQ(o.refs, 2);
	g = file_of("c:\\RUN\\doc.tdoc");
	CHECK_EQ(rot->lpVtbl->IsRunning(rot, g), S_OK);
	CHECK_EQ(f->lpVtbl->IsRunning(f, NULL, NULL, NULL), S_OK);
	CHECK_EQ(rot->lpVtbl->Register(rot, 0, &o.iface, g, &c2),
		 MK_S_MONIKERALREADYREGISTERED);
	CHECK(c2 && c2 != c1);
	CHECK_EQ(rot->lpVtbl->GetObject(rot, g, &u), S_OK);
	CHECK(u == &o.iface);
	u->lpVtbl->Release(u);
	CHECK_EQ(rot->lpVtbl->NoteChangeTime(rot, c1, &t2), S_OK);
	CHECK_EQ(rot->lpVtbl->GetTimeOfLastChange(rot, f, &t), S_OK);
	CHECK(t.dwLowDateTime == 1234 && t.dwHighDateTime == 5678);
	CHECK_EQ(f->lpVtbl->GetTimeOfLastChange(f, NULL, NULL, &t), S_OK);

	IEnumMoniker *e;
	ULONG n = 0;
	CHECK_EQ(rot->lpVtbl->EnumRunning(rot, &e), S_OK);
	while (e->lpVtbl->Next(e, 1, &m, NULL) == S_OK) {
		n++;
		release(m);
	}
	CHECK_EQ(n, 2);
	e->lpVtbl->Release(e);

	CHECK_EQ(rot->lpVtbl->Revoke(rot, c1), S_OK);
	CHECK_EQ(rot->lpVtbl->Revoke(rot, c1), E_INVALIDARG);
	CHECK_EQ(rot->lpVtbl->Revoke(rot, c2), S_OK);
	CHECK_EQ(o.refs, 1);
	CHECK_EQ(rot->lpVtbl->GetObject(rot, f, &u), MK_E_UNAVAILABLE);
	CHECK(u == NULL);
	CHECK_EQ(rot->lpVtbl->Register(rot, 4, &o.iface, f, &c1),
		 E_INVALIDARG);
	rot->lpVtbl->Release(rot);
	release(g);
	release(f);
}

/* Parsing "file!item" and binding it through a running container. */
static void test_parse_bind(void)
{
	FN(HRESULT, CreateBindCtx, DWORD, IBindCtx **);
	FN(HRESULT, MkParseDisplayName, IBindCtx *, LPCOLESTR, ULONG *,
	   IMoniker **);
	FN(HRESULT, GetRunningObjectTable, DWORD, IRunningObjectTable **);
	FN(HRESULT, CoGetObject, LPCWSTR, BIND_OPTS *, REFIID, void **);
	IRunningObjectTable *rot;
	IBindCtx *bc;
	IMoniker *book = file_of("C:\\docs\\book.tdoc"), *m = NULL;
	DWORD cookie;
	ULONG eaten = 99;
	void *p;

	make_item = item_of;
	CHECK_EQ(GetRunningObjectTable(0, &rot), S_OK);
	CHECK_EQ(rot->lpVtbl->Register(rot, 0, (IUnknown *)&container, book,
				       &cookie), S_OK);
	CHECK_EQ(CreateBindCtx(0, &bc), S_OK);

	WCHAR *name = wide("C:\\docs\\book.tdoc!Sheet3");
	CHECK_EQ(MkParseDisplayName(bc, name, &eaten, &m), S_OK);
	CHECK_EQ(eaten, wstrlen(name));
	CHECK(named(m, "C:\\docs\\book.tdoc!Sheet3"));
	/* The item binds through the running container. */
	sheet.refs = 1;
	container_gets = 0;
	CHECK_EQ(m->lpVtbl->BindToObject(m, bc, NULL, &iid_other, &p), S_OK);
	CHECK(p == &sheet);
	CHECK_EQ(container_gets, 1);
	obj_release(&sheet.iface);
	CHECK_EQ(m->lpVtbl->IsRunning(m, bc, NULL, NULL), S_FALSE);
	release(m);
	free(name);
	name = wide("C:\\docs\\book.tdoc!Sheet1");
	CHECK_EQ(MkParseDisplayName(bc, name, &eaten, &m), S_OK);
	CHECK_EQ(m->lpVtbl->IsRunning(m, bc, NULL, NULL), S_OK);
	release(m);
	free(name);

	/* What the container can't parse stops the parse there. */
	name = wide("C:\\docs\\book.tdoc!Sheet1?bad");
	m = (IMoniker *)1;
	CHECK(FAILED(MkParseDisplayName(bc, name, &eaten, &m)));
	CHECK(m == NULL);
	CHECK_EQ(eaten, 24);	/* the file and "!Sheet1" */
	free(name);
	name = wide("C:\\nowhere\\none.tdoc");
	CHECK_EQ(MkParseDisplayName(bc, name, &eaten, &m), MK_E_CANTOPENFILE);
	free(name);

	/* CoGetObject parses and binds in one. */
	name = wide("C:\\docs\\book.tdoc!Sheet4");
	CHECK_EQ(CoGetObject(name, NULL, &iid_other, &p), S_OK);
	CHECK(p == &sheet);
	obj_release(&sheet.iface);
	free(name);

	/* A class moniker by its display name. */
	name = wide("clsid:3f2a1b0c-1d2e-4f30-8192-a3b4c5d6e7f8:");
	CHECK_EQ(MkParseDisplayName(bc, name, &eaten, &m), S_OK);
	CHECK_EQ(eaten, wstrlen(name));
	CHECK(named(m, "clsid:3F2A1B0C-1D2E-4F30-8192-A3B4C5D6E7F8:"));
	/* Binding it gives the class object. */
	CHECK_EQ(m->lpVtbl->BindToObject(m, bc, NULL, &iid_factory, &p), S_OK);
	CHECK(p == &doc_factory);
	release(m);
	free(name);

	bc->lpVtbl->Release(bc);
	CHECK_EQ(rot->lpVtbl->Revoke(rot, cookie), S_OK);
	CHECK_EQ(container_refs, 0);
	rot->lpVtbl->Release(rot);
	release(book);
}

/* A file's class: from a compound file's root, or from its extension. */
static void test_class_file(const char *dir)
{
	FN(HRESULT, GetClassFile, LPCOLESTR, CLSID *);
	FN(HRESULT, BindMoniker, IMoniker *, DWORD, REFIID, void **);
	FN(HRESULT, CoGetInstanceFromFile, void *, CLSID *, IUnknown *, DWORD,
	   DWORD, OLECHAR *, DWORD, MULTI_QI *);
	FN(HRESULT, StgCreateDocfile, LPCOLESTR, DWORD, DWORD, IStorage **);
	char path[512];
	CLSID c;

	snprintf(path, sizeof(path), "%s/report.tdoc", dir);
	FILE *fp = fopen(path, "wb");
	if (fp) {
		fputs("text", fp);
		fclose(fp);
	}
	char *win = path_to_windows(path);
	WCHAR *w = wide(win);
	CHECK_EQ(GetClassFile(w, &c), S_OK);
	CHECK(!memcmp(&c, &clsid_doc, sizeof(c)));

	/* Binding a file moniker loads a new object from it. */
	IMoniker *f = file_of(win);
	IPersistFile *pf = NULL;
	CHECK_EQ(BindMoniker(f, 0, &IID_IPersistFile, (void **)&pf), S_OK);
	CHECK(pf && last_doc && wis(last_doc->loaded, win));
	if (pf)
		pf->lpVtbl->Release(pf);
	CHECK_EQ(docs_alive, 0);
	release(f);

	MULTI_QI qi[2] = { { &IID_IPersistFile, NULL, 0 },
			   { &iid_other, NULL, 0 } };
	CHECK_EQ(CoGetInstanceFromFile(NULL, NULL, NULL, CLSCTX_INPROC_SERVER,
				       STGM_READ, w, 2, qi),
		 CO_S_NOTALLINTERFACES);
	CHECK(qi[0].hr == S_OK && qi[0].pItf);
	CHECK(qi[1].hr == E_NOINTERFACE && !qi[1].pItf);
	CHECK(last_doc && last_doc->mode == STGM_READ);
	if (qi[0].pItf)
		qi[0].pItf->lpVtbl->Release(qi[0].pItf);
	CHECK_EQ(docs_alive, 0);
	free(w);
	free(win);

	/* Unknown extensions, missing files. */
	snprintf(path, sizeof(path), "%s/plain.zzq", dir);
	fp = fopen(path, "wb");
	if (fp)
		fclose(fp);
	win = path_to_windows(path);
	w = wide(win);
	CHECK_EQ(GetClassFile(w, &c), MK_E_INVALIDEXTENSION);
	free(w);
	free(win);
	w = wide("C:\\no\\such.tdoc");
	CHECK_EQ(GetClassFile(w, &c), MK_E_CANTOPENFILE);
	free(w);

	/* A compound file's class wins over its extension. */
	snprintf(path, sizeof(path), "%s/store.zzq", dir);
	win = path_to_windows(path);
	w = wide(win);
	IStorage *stg;
	CHECK_EQ(StgCreateDocfile(w, STGM_CREATE | STGM_READWRITE |
				  STGM_SHARE_EXCLUSIVE, 0, &stg), S_OK);
	CHECK_EQ(stg->lpVtbl->SetClass(stg, &iid_other), S_OK);
	stg->lpVtbl->Release(stg);
	CHECK_EQ(GetClassFile(w, &c), S_OK);
	CHECK(!memcmp(&c, &iid_other, sizeof(c)));
	/* BindToStorage opens it. */
	f = file_of(win);
	IBindCtx *bc;
	FN(HRESULT, CreateBindCtx, DWORD, IBindCtx **);
	CHECK_EQ(CreateBindCtx(0, &bc), S_OK);
	BIND_OPTS o = { sizeof(o), 0, STGM_READ | STGM_SHARE_DENY_WRITE, 0 };
	bc->lpVtbl->SetBindOptions(bc, &o);
	stg = NULL;
	CHECK_EQ(f->lpVtbl->BindToStorage(f, bc, NULL, &IID_IStorage,
					  (void **)&stg), S_OK);
	if (stg)
		stg->lpVtbl->Release(stg);
	bc->lpVtbl->Release(bc);
	release(f);
	free(w);
	free(win);
}

int main(void)
{
	char dir[] = "/tmp/vfw-moniker-XXXXXX";
	if (!mkdtemp(dir)) {
		perror("mkdtemp");
		return 1;
	}
	setenv("VFW_ROOT", dir, 1);
	setenv("VFW_REGISTRY", "", 1);
	CHECK_EQ(lowmem_reserve(), 0);

	CHECK_EQ(runtime_enter(), 0);
	reg_reload();
	ole = mod_load("ole32.dll", NULL);
	CHECK(ole != NULL);
	if (!ole)
		return test_summary("test_moniker");

	FN(HRESULT, CoInitialize, LPVOID);
	FN(void, CoUninitialize, void);
	FN(HRESULT, CoRegisterClassObject, REFCLSID, LPUNKNOWN, DWORD, DWORD,
	   LPDWORD);
	FN(HRESULT, CoRevokeClassObject, DWORD);
	DWORD cookie;
	CHECK_EQ(CoInitialize(NULL), S_OK);
	CHECK_EQ(CoRegisterClassObject(&clsid_doc, (IUnknown *)&doc_factory,
				       CLSCTX_INPROC_SERVER, REGCLS_MULTIPLEUSE,
				       &cookie), S_OK);
	set_class_value("HKEY_LOCAL_MACHINE\\Software\\Classes\\.tdoc",
			"Test.Document");
	set_class_value("HKEY_LOCAL_MACHINE\\Software\\Classes\\Test.Document"
			"\\CLSID", CLSID_DOC);

	test_bind_ctx();
	test_simple();
	test_persist();
	test_rot();
	test_parse_bind();
	test_class_file(dir);

	CHECK_EQ(CoRevokeClassObject(cookie), S_OK);
	CoUninitialize();
	mod_free(ole);
	char cmd[64];
	snprintf(cmd, sizeof(cmd), "rm -rf '%s'", dir);
	if (system(cmd))
		fprintf(stderr, "could not remove %s\n", dir);
	return test_summary("test_moniker");
}
