/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * ole32's remaining OLE, called through its export table: storage media,
 * advise holders, the OLE clipboard over user32's, running and locking
 * objects, drag and drop, the registry's OLE keys (user types, MiscStatus,
 * verbs, TreatAs), accelerators (with user32's tables), and Co* helpers.
 */
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "loader/modules.h"
#include "runtime/lowmem.h"
#include "runtime/runtime.h"
#include "win32/codepage.h"
#include "win32/internal.h"
#include "win32/path.h"
#include "win32/reg.h"
#include "test.h"

static struct module *ole, *k32, *u32;

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
#define K32(ret, name, ...) \
	ret (WINAPI *name)(__VA_ARGS__) = \
		(ret (WINAPI *)(__VA_ARGS__))get_from(k32, #name)
#define U32(ret, name, ...) \
	ret (WINAPI *name)(__VA_ARGS__) = \
		(ret (WINAPI *)(__VA_ARGS__))get_from(u32, #name)

#ifndef GMEM_MOVEABLE
#define GMEM_MOVEABLE 2
#endif
#define CF_TEXT 1

static const IID iid_unknown = { 0, 0, 0,
	{ 0xc0, 0, 0, 0, 0, 0, 0, 0x46 } };
static const IID iid_other = { 0x12345678, 0x9abc, 0xdef0,
	{ 1, 2, 3, 4, 5, 6, 7, 8 } };
static const CLSID clsid_a = { 0x0a0a0a0a, 1, 2,
	{ 3, 4, 5, 6, 7, 8, 9, 10 } };
#define CLSID_A "{0A0A0A0A-0001-0002-0304-05060708090A}"
static const CLSID clsid_b = { 0x0b0b0b0b, 1, 2,
	{ 3, 4, 5, 6, 7, 8, 9, 10 } };

static WCHAR *wide(const char *s)
{
	return utf8_to_utf16_dup(s);
}

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

static void set_value(const char *key, const char *value)
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

static HGLOBAL global_text(const char *s)
{
	K32(HGLOBAL, GlobalAlloc, UINT, SIZE_T);
	K32(void *, GlobalLock, HGLOBAL);
	K32(BOOL, GlobalUnlock, HGLOBAL);
	HGLOBAL h = GlobalAlloc(GMEM_MOVEABLE, strlen(s) + 1);
	char *p = h ? GlobalLock(h) : NULL;
	if (p) {
		strcpy(p, s);
		GlobalUnlock(h);
	}
	return h;
}

static int global_is(HGLOBAL h, const char *s)
{
	K32(void *, GlobalLock, HGLOBAL);
	K32(BOOL, GlobalUnlock, HGLOBAL);
	const char *p = h ? GlobalLock(h) : NULL;
	int r = p && !strcmp(p, s);
	if (p)
		GlobalUnlock(h);
	return r;
}

/* ---- a counted object with optional interfaces ---- */

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

/* ---- an advise sink that counts ---- */

struct sink {
	IAdviseSink iface;
	LONG refs;
	int changes, saves, closes, renames;
	DWORD last_tymed;
	char last_text[16];
};

WINAPI static HRESULT sk_qi(IAdviseSink *self, REFIID riid, void **ppv)
{
	*ppv = self;
	((struct sink *)self)->refs++;
	return S_OK;
}

WINAPI static ULONG sk_addref(IAdviseSink *self)
{
	return ++((struct sink *)self)->refs;
}

WINAPI static ULONG sk_release(IAdviseSink *self)
{
	return --((struct sink *)self)->refs;
}

WINAPI static void sk_change(IAdviseSink *self, FORMATETC *f, STGMEDIUM *m)
{
	struct sink *s = (struct sink *)self;
	K32(void *, GlobalLock, HGLOBAL);
	K32(BOOL, GlobalUnlock, HGLOBAL);
	s->changes++;
	s->last_tymed = m->tymed;
	s->last_text[0] = 0;
	if (m->tymed == TYMED_HGLOBAL) {
		const char *p = GlobalLock(m->hGlobal);
		if (p) {
			snprintf(s->last_text, sizeof(s->last_text), "%s", p);
			GlobalUnlock(m->hGlobal);
		}
	}
}

WINAPI static void sk_view(IAdviseSink *self, DWORD a, LONG i)
{
}

WINAPI static void sk_rename(IAdviseSink *self, IMoniker *m)
{
	((struct sink *)self)->renames++;
}

WINAPI static void sk_save(IAdviseSink *self)
{
	((struct sink *)self)->saves++;
}

WINAPI static void sk_close(IAdviseSink *self)
{
	((struct sink *)self)->closes++;
}

static const IAdviseSinkVtbl sink_vtbl = {
	sk_qi, sk_addref, sk_release, sk_change, sk_view, sk_rename, sk_save,
	sk_close,
};

/* ---- a data object with CF_TEXT ---- */

struct data {
	IDataObject iface;
	LONG refs;
	char text[32];
};

WINAPI static HRESULT do_qi(IDataObject *self, REFIID riid, void **ppv)
{
	if (!memcmp(riid, &iid_unknown, sizeof(GUID)) ||
	    !memcmp(riid, &IID_IDataObject, sizeof(GUID))) {
		*ppv = self;
		((struct data *)self)->refs++;
		return S_OK;
	}
	*ppv = NULL;
	return E_NOINTERFACE;
}

WINAPI static ULONG do_addref(IDataObject *self)
{
	return ++((struct data *)self)->refs;
}

WINAPI static ULONG do_release(IDataObject *self)
{
	return --((struct data *)self)->refs;
}

WINAPI static HRESULT do_get(IDataObject *self, FORMATETC *f, STGMEDIUM *m)
{
	if (f->cfFormat != CF_TEXT)
		return DV_E_FORMATETC;
	if (((struct data *)self)->text[0] == '!')	/* "can't render" */
		return E_FAIL;
	if (!(f->tymed & TYMED_HGLOBAL))
		return DV_E_TYMED;
	m->tymed = TYMED_HGLOBAL;
	m->hGlobal = global_text(((struct data *)self)->text);
	m->pUnkForRelease = NULL;
	return m->hGlobal ? S_OK : E_OUTOFMEMORY;
}

WINAPI static HRESULT do_gethere(IDataObject *self, FORMATETC *f,
				 STGMEDIUM *m)
{
	return E_NOTIMPL;
}

WINAPI static HRESULT do_query(IDataObject *self, FORMATETC *f)
{
	return f->cfFormat == CF_TEXT ? S_OK : DV_E_FORMATETC;
}

WINAPI static HRESULT do_canon(IDataObject *self, FORMATETC *a, FORMATETC *b)
{
	return E_NOTIMPL;
}

WINAPI static HRESULT do_set(IDataObject *self, FORMATETC *f, STGMEDIUM *m,
			     BOOL r)
{
	return E_NOTIMPL;
}

static IEnumFORMATETC *text_formats;

WINAPI static HRESULT do_enum(IDataObject *self, DWORD dir,
			      IEnumFORMATETC **e)
{
	text_formats->lpVtbl->AddRef(text_formats);
	text_formats->lpVtbl->Reset(text_formats);
	*e = text_formats;
	return S_OK;
}

WINAPI static HRESULT do_dadvise(IDataObject *self, FORMATETC *f, DWORD a,
				 IAdviseSink *s, DWORD *c)
{
	return OLE_E_ADVISENOTSUPPORTED;
}

WINAPI static HRESULT do_dunadvise(IDataObject *self, DWORD c)
{
	return OLE_E_ADVISENOTSUPPORTED;
}

WINAPI static HRESULT do_enumadvise(IDataObject *self, IEnumSTATDATA **e)
{
	return OLE_E_ADVISENOTSUPPORTED;
}

static const IDataObjectVtbl data_vtbl = {
	do_qi, do_addref, do_release, do_get, do_gethere, do_query, do_canon,
	do_set, do_enum, do_dadvise, do_dunadvise, do_enumadvise,
};


/* A source of three formats: X (CF_TEXT) that may be rendered once, Y
 * (CF_OEMTEXT) that can't be until y_ok, and Z, a storage only. */
struct multi {
	IDataObject iface;
	LONG refs;
	int x_gets, y_ok;
};

static const FORMATETC multi_fmts[3] = {
	{ CF_TEXT, NULL, DVASPECT_CONTENT, -1, TYMED_HGLOBAL },
	{ 7 /* CF_OEMTEXT */, NULL, DVASPECT_CONTENT, -1, TYMED_HGLOBAL },
	{ 0xc0f0, NULL, DVASPECT_CONTENT, -1, TYMED_ISTORAGE },
};

struct fenum {
	IEnumFORMATETC iface;
	LONG refs;
	ULONG pos;
};

WINAPI static HRESULT fe_qi(IEnumFORMATETC *self, REFIID riid, void **ppv)
{
	*ppv = NULL;
	return E_NOINTERFACE;
}
WINAPI static ULONG fe_addref(IEnumFORMATETC *self)
{
	return ++((struct fenum *)self)->refs;
}
WINAPI static ULONG fe_release(IEnumFORMATETC *self)
{
	struct fenum *e = (struct fenum *)self;
	ULONG r = --e->refs;
	if (!r)
		free(e);
	return r;
}
WINAPI static HRESULT fe_next(IEnumFORMATETC *self, ULONG n, FORMATETC *out,
			      ULONG *got)
{
	struct fenum *e = (struct fenum *)self;
	ULONG k = 0;
	while (k < n && e->pos < 3)
		out[k++] = multi_fmts[e->pos++];
	if (got)
		*got = k;
	return k == n ? S_OK : S_FALSE;
}
WINAPI static HRESULT fe_skip(IEnumFORMATETC *self, ULONG n)
{
	return E_NOTIMPL;
}
WINAPI static HRESULT fe_reset(IEnumFORMATETC *self)
{
	((struct fenum *)self)->pos = 0;
	return S_OK;
}
WINAPI static HRESULT fe_clone(IEnumFORMATETC *self, IEnumFORMATETC **out)
{
	return E_NOTIMPL;
}
static const IEnumFORMATETCVtbl fenum_vtbl = {
	fe_qi, fe_addref, fe_release, fe_next, fe_skip, fe_reset, fe_clone,
};

static HRESULT (WINAPI *p_StgCreateDocfile)(LPCOLESTR, DWORD, DWORD,
					    IStorage **);

WINAPI static HRESULT mu_get(IDataObject *self, FORMATETC *f, STGMEDIUM *m)
{
	struct multi *d = (struct multi *)self;
	m->pUnkForRelease = NULL;
	if (f->cfFormat == CF_TEXT && (f->tymed & TYMED_HGLOBAL)) {
		if (d->x_gets++)
			return E_FAIL;		/* asked twice */
		m->tymed = TYMED_HGLOBAL;
		m->hGlobal = global_text("ex");
		return S_OK;
	}
	if (f->cfFormat == 7 /* CF_OEMTEXT */ && (f->tymed & TYMED_HGLOBAL)) {
		if (!d->y_ok)
			return E_FAIL;
		m->tymed = TYMED_HGLOBAL;
		m->hGlobal = global_text("why");
		return S_OK;
	}
	if (f->cfFormat == 0xc0f0 && (f->tymed & TYMED_ISTORAGE)) {
		IStorage *stg = NULL;
		IStream *st = NULL;
		WCHAR sn[] = { 'S', 0 };
		if (FAILED(p_StgCreateDocfile(NULL, STGM_CREATE |
					      STGM_READWRITE |
					      STGM_SHARE_EXCLUSIVE |
					      STGM_DELETEONRELEASE, 0, &stg)))
			return E_FAIL;
		if (SUCCEEDED(stg->lpVtbl->CreateStream(
			    stg, sn, STGM_CREATE | STGM_READWRITE |
			    STGM_SHARE_EXCLUSIVE, 0, 0, &st))) {
			st->lpVtbl->Write(st, "zed", 3, NULL);
			st->lpVtbl->Release(st);
		}
		m->tymed = TYMED_ISTORAGE;
		m->pstg = stg;
		return S_OK;
	}
	return DV_E_FORMATETC;
}

WINAPI static HRESULT mu_enum(IDataObject *self, DWORD dir,
			      IEnumFORMATETC **e)
{
	struct fenum *n = calloc(1, sizeof(*n));
	if (!n)
		return E_OUTOFMEMORY;
	n->iface.lpVtbl = &fenum_vtbl;
	n->refs = 1;
	*e = &n->iface;
	return S_OK;
}

WINAPI static HRESULT mu_query(IDataObject *self, FORMATETC *f)
{
	return S_OK;
}

static const IDataObjectVtbl multi_vtbl = {
	do_qi, do_addref, do_release, mu_get, do_gethere, mu_query, do_canon,
	do_set, mu_enum, do_dadvise, do_dunadvise, do_enumadvise,
};

static void test_media(const char *dir)
{
	FN(void, ReleaseStgMedium, STGMEDIUM *);
	FN(HANDLE, OleDuplicateData, HANDLE, CLIPFORMAT, UINT);
	FN(HRESULT, CreateStreamOnHGlobal, HGLOBAL, BOOL, IStream **);
	FN(LPVOID, CoTaskMemAlloc, SIZE_T);
	K32(SIZE_T, GlobalSize, HGLOBAL);
	K32(HGLOBAL, GlobalFree, HGLOBAL);
	STGMEDIUM m;
	struct obj owner = { { &obj_vtbl }, 1 };

	HGLOBAL h = global_text("medium");
	HGLOBAL d = OleDuplicateData(h, CF_TEXT, 0);
	CHECK(d && d != h && global_is(d, "medium"));
	CHECK(OleDuplicateData(h, 2 /* CF_BITMAP */, 0) == NULL);
	GlobalFree(d);

	/* Released through pUnkForRelease: the memory stays the owner's. */
	memset(&m, 0, sizeof(m));
	m.tymed = TYMED_HGLOBAL;
	m.hGlobal = h;
	m.pUnkForRelease = &owner.iface;
	owner.refs++;
	ReleaseStgMedium(&m);
	CHECK_EQ(owner.refs, 1);
	CHECK(GlobalSize(h) > 0);
	CHECK(m.pUnkForRelease == NULL);
	GlobalFree(h);

	IStream *stm;
	CHECK_EQ(CreateStreamOnHGlobal(NULL, TRUE, &stm), S_OK);
	stm->lpVtbl->AddRef(stm);
	m.tymed = TYMED_ISTREAM;
	m.pstm = stm;
	m.pUnkForRelease = NULL;
	ReleaseStgMedium(&m);
	CHECK_EQ(stm->lpVtbl->Release(stm), 0);

	/* A file medium is deleted, and its name freed. */
	char path[512];
	snprintf(path, sizeof(path), "%s/medium.tmp", dir);
	FILE *f = fopen(path, "wb");
	if (f)
		fclose(f);
	char *win = path_to_windows(path);
	WCHAR *w = wide(win);
	size_t n = wstrlen(w);
	m.tymed = TYMED_FILE;
	m.lpszFileName = CoTaskMemAlloc((n + 1) * sizeof(WCHAR));
	memcpy(m.lpszFileName, w, (n + 1) * sizeof(WCHAR));
	m.pUnkForRelease = NULL;
	ReleaseStgMedium(&m);
	CHECK(access(path, F_OK) != 0);
	free(w);
	free(win);
	ReleaseStgMedium(NULL);
}

static void test_advise(void)
{
	FN(HRESULT, CreateOleAdviseHolder, IOleAdviseHolder **);
	FN(HRESULT, CreateDataAdviseHolder, IDataAdviseHolder **);
	FN(HRESULT, CreateItemMoniker, LPCOLESTR, LPCOLESTR, IMoniker **);
	struct sink a, b;
	IOleAdviseHolder *oh;
	DWORD ca, cb;

	memset(&a, 0, sizeof(a));
	memset(&b, 0, sizeof(b));
	a.iface.lpVtbl = b.iface.lpVtbl = &sink_vtbl;

	CHECK_EQ(CreateOleAdviseHolder(&oh), S_OK);
	CHECK_EQ(oh->lpVtbl->Advise(oh, &a.iface, &ca), S_OK);
	CHECK_EQ(oh->lpVtbl->Advise(oh, &b.iface, &cb), S_OK);
	CHECK(ca && cb && ca != cb);
	CHECK(a.refs == 1 && b.refs == 1);
	CHECK_EQ(oh->lpVtbl->SendOnSave(oh), S_OK);
	CHECK_EQ(oh->lpVtbl->SendOnClose(oh), S_OK);
	CHECK(a.saves == 1 && b.saves == 1 && a.closes == 1);
	IMoniker *mk;
	WCHAR bang[2] = { '!', 0 }, nm[2] = { 'x', 0 };
	CHECK_EQ(CreateItemMoniker(bang, nm, &mk), S_OK);
	CHECK_EQ(oh->lpVtbl->SendOnRename(oh, mk), S_OK);
	CHECK_EQ(b.renames, 1);
	mk->lpVtbl->Release(mk);

	IEnumSTATDATA *e;
	STATDATA sd[3];
	ULONG got = 0;
	CHECK_EQ(oh->lpVtbl->EnumAdvise(oh, &e), S_OK);
	CHECK_EQ(e->lpVtbl->Next(e, 3, sd, &got), S_FALSE);
	CHECK_EQ(got, 2);
	for (ULONG i = 0; i < got; i++)
		sd[i].pAdvSink->lpVtbl->Release(sd[i].pAdvSink);
	e->lpVtbl->Release(e);

	CHECK_EQ(oh->lpVtbl->Unadvise(oh, ca), S_OK);
	CHECK_EQ(oh->lpVtbl->Unadvise(oh, ca), OLE_E_NOCONNECTION);
	CHECK_EQ(a.refs, 0);
	CHECK_EQ(oh->lpVtbl->SendOnSave(oh), S_OK);
	CHECK(a.saves == 1 && b.saves == 2);
	oh->lpVtbl->Release(oh);
	CHECK_EQ(b.refs, 0);

	/* Data changes, with and without the data. */
	struct data src = { { &data_vtbl }, 1, "first" };
	IDataAdviseHolder *dh;
	FORMATETC f = { CF_TEXT, NULL, DVASPECT_CONTENT, -1, TYMED_HGLOBAL };
	memset(&a, 0, sizeof(a));
	memset(&b, 0, sizeof(b));
	a.iface.lpVtbl = b.iface.lpVtbl = &sink_vtbl;
	CHECK_EQ(CreateDataAdviseHolder(&dh), S_OK);
	CHECK_EQ(dh->lpVtbl->Advise(dh, &src.iface, &f, ADVF_PRIMEFIRST,
				    &a.iface, &ca), S_OK);
	CHECK_EQ(a.changes, 1);
	CHECK(!strcmp(a.last_text, "first"));
	CHECK_EQ(dh->lpVtbl->Advise(dh, &src.iface, &f,
				    ADVF_NODATA | ADVF_ONLYONCE, &b.iface,
				    &cb), S_OK);
	CHECK_EQ(b.changes, 0);
	strcpy(src.text, "second");
	CHECK_EQ(dh->lpVtbl->SendOnDataChange(dh, &src.iface, 0, 0), S_OK);
	CHECK(a.changes == 2 && !strcmp(a.last_text, "second"));
	CHECK(b.changes == 1 && b.last_tymed == TYMED_NULL);
	CHECK_EQ(b.refs, 0);	/* only once: gone */
	CHECK_EQ(dh->lpVtbl->Unadvise(dh, cb), OLE_E_NOCONNECTION);
	CHECK_EQ(dh->lpVtbl->SendOnDataChange(dh, &src.iface, 0, 0), S_OK);
	CHECK(a.changes == 3 && b.changes == 1);

	/* A one-shot sink whose data could not be had is kept for the next
	 * change; one that asked for data on stop gets it then. */
	struct sink c;
	memset(&c, 0, sizeof(c));
	memset(&b, 0, sizeof(b));
	b.iface.lpVtbl = c.iface.lpVtbl = &sink_vtbl;
	DWORD cc;
	CHECK_EQ(dh->lpVtbl->Advise(dh, &src.iface, &f, ADVF_ONLYONCE,
				    &b.iface, &cb), S_OK);
	CHECK_EQ(dh->lpVtbl->Advise(dh, &src.iface, &f,
				    ADVF_NODATA | ADVF_DATAONSTOP, &c.iface,
				    &cc), S_OK);
	strcpy(src.text, "!fail");
	CHECK_EQ(dh->lpVtbl->SendOnDataChange(dh, &src.iface, 0, 0), S_OK);
	CHECK_EQ(b.changes, 0);
	CHECK_EQ(b.refs, 1);
	CHECK(c.changes == 1 && c.last_tymed == TYMED_NULL);
	strcpy(src.text, "third");
	CHECK_EQ(dh->lpVtbl->SendOnDataChange(dh, &src.iface, 0,
					      ADVF_DATAONSTOP), S_OK);
	CHECK(b.changes == 1 && !strcmp(b.last_text, "third"));
	CHECK_EQ(b.refs, 0);
	CHECK(c.changes == 2 && c.last_tymed == TYMED_HGLOBAL &&
	      !strcmp(c.last_text, "third"));
	dh->lpVtbl->Release(dh);
	CHECK_EQ(c.refs, 0);
	CHECK_EQ(a.refs, 0);
	CHECK_EQ(src.refs, 1);
}

static void test_clipboard(void)
{
	FN(HRESULT, OleSetClipboard, IDataObject *);
	FN(HRESULT, OleGetClipboard, IDataObject **);
	FN(HRESULT, OleFlushClipboard, void);
	FN(HRESULT, OleIsCurrentClipboard, IDataObject *);
	FN(void, ReleaseStgMedium, STGMEDIUM *);
	U32(BOOL, OpenClipboard, HWND);
	U32(BOOL, CloseClipboard, void);
	U32(HANDLE, GetClipboardData, UINT);
	U32(BOOL, EmptyClipboard, void);
	struct data src = { { &data_vtbl }, 1, "clip text" };
	FORMATETC f = { CF_TEXT, NULL, DVASPECT_CONTENT, -1, TYMED_HGLOBAL };
	IDataObject *got;
	STGMEDIUM m;

	CHECK_EQ(OleSetClipboard(&src.iface), S_OK);
	CHECK_EQ(src.refs, 2);
	CHECK_EQ(OleIsCurrentClipboard(&src.iface), S_OK);
	/* Rendered into user32's clipboard for everyone. */
	CHECK(OpenClipboard(NULL));
	CHECK(global_is(GetClipboardData(CF_TEXT), "clip text"));
	CloseClipboard();

	CHECK_EQ(OleGetClipboard(&got), S_OK);
	memset(&m, 0, sizeof(m));
	CHECK_EQ(got->lpVtbl->GetData(got, &f, &m), S_OK);
	CHECK(m.tymed == TYMED_HGLOBAL && global_is(m.hGlobal, "clip text"));
	ReleaseStgMedium(&m);
	CHECK_EQ(got->lpVtbl->QueryGetData(got, &f), S_OK);

	/* Flushed: the source is let go, the data stays. */
	CHECK_EQ(OleFlushClipboard(), S_OK);
	CHECK_EQ(src.refs, 1);
	CHECK_EQ(OleIsCurrentClipboard(&src.iface), S_FALSE);
	strcpy(src.text, "changed");
	memset(&m, 0, sizeof(m));
	CHECK_EQ(got->lpVtbl->GetData(got, &f, &m), S_OK);
	CHECK(global_is(m.hGlobal, "clip text"));
	ReleaseStgMedium(&m);
	IEnumFORMATETC *e;
	FORMATETC one;
	CHECK_EQ(got->lpVtbl->EnumFormatEtc(got, DATADIR_GET, &e), S_OK);
	CHECK_EQ(e->lpVtbl->Next(e, 1, &one, NULL), S_OK);
	CHECK_EQ(one.cfFormat, CF_TEXT);
	e->lpVtbl->Release(e);

	/* Someone else empties it: nothing there. */
	CHECK(OpenClipboard(NULL) && EmptyClipboard());
	CloseClipboard();
	CHECK_EQ(got->lpVtbl->QueryGetData(got, &f), DV_E_FORMATETC);
	CHECK_EQ(got->lpVtbl->GetData(got, &f, &m), DV_E_FORMATETC);
	got->lpVtbl->Release(got);

	/* OleGetClipboard's object set back is not its own source. */
	CHECK_EQ(OleSetClipboard(&src.iface), S_OK);
	CHECK_EQ(OleGetClipboard(&got), S_OK);
	CHECK_EQ(OleSetClipboard(got), S_OK);
	CHECK_EQ(got->lpVtbl->QueryGetData(got, &f), S_OK);
	CHECK_EQ(OleIsCurrentClipboard(&src.iface), S_OK);
	/* A request the wrapper refuses leaves the source's count alone. */
	IEnumFORMATETC *bad = NULL;
	CHECK_EQ(got->lpVtbl->EnumFormatEtc(got, DATADIR_SET, &bad), E_NOTIMPL);
	CHECK_EQ(src.refs, 2);
	got->lpVtbl->Release(got);
	/* Taken over by a native EmptyClipboard: the source is let go. */
	CHECK(OpenClipboard(NULL) && EmptyClipboard());
	CloseClipboard();
	CHECK_EQ(OleIsCurrentClipboard(&src.iface), S_FALSE);
	CHECK_EQ(src.refs, 1);

	/* Set, then replaced by nothing. */
	CHECK_EQ(OleSetClipboard(&src.iface), S_OK);
	CHECK_EQ(OleSetClipboard(NULL), S_OK);
	CHECK_EQ(src.refs, 1);
	CHECK_EQ(OleIsCurrentClipboard(&src.iface), S_FALSE);

	/* A flush renders only what is still missing, and a storage
	 * format is kept, as a compound file, to be read back as one. */
	FN(HRESULT, StgCreateDocfile, LPCOLESTR, DWORD, DWORD, IStorage **);
	p_StgCreateDocfile = StgCreateDocfile;
	struct multi mu = { { &multi_vtbl }, 1, 0, 0 };
	CHECK_EQ(OleSetClipboard(&mu.iface), S_OK);
	CHECK_EQ(mu.x_gets, 1);
	CHECK_EQ(OleFlushClipboard(), CLIPBRD_E_CANT_SET);	/* Y still not */
	CHECK_EQ(mu.refs, 2);
	mu.y_ok = 1;
	CHECK_EQ(OleFlushClipboard(), S_OK);
	CHECK_EQ(mu.x_gets, 1);		/* X wasn't asked for again */
	CHECK_EQ(mu.refs, 1);
	CHECK(OpenClipboard(NULL));
	CHECK(global_is(GetClipboardData(CF_TEXT), "ex"));
	CHECK(global_is(GetClipboardData(7 /* CF_OEMTEXT */), "why"));
	CloseClipboard();
	FORMATETC zf = { 0xc0f0, NULL, DVASPECT_CONTENT, -1, TYMED_ISTORAGE };
	CHECK_EQ(OleGetClipboard(&got), S_OK);
	memset(&m, 0, sizeof(m));
	CHECK_EQ(got->lpVtbl->GetData(got, &zf, &m), S_OK);
	if (m.tymed == TYMED_ISTORAGE && m.pstg) {
		WCHAR sn[] = { 'S', 0 };
		IStream *st = NULL;
		char buf[4] = { 0 };
		CHECK_EQ(m.pstg->lpVtbl->OpenStream(m.pstg, sn, NULL,
						    STGM_READ |
						    STGM_SHARE_EXCLUSIVE, 0,
						    &st), S_OK);
		if (st) {
			st->lpVtbl->Read(st, buf, 3, NULL);
			st->lpVtbl->Release(st);
		}
		CHECK(!strcmp(buf, "zed"));
	} else {
		CHECK(0);
	}
	ReleaseStgMedium(&m);
	got->lpVtbl->Release(got);
	CHECK(OpenClipboard(NULL) && EmptyClipboard());
	CloseClipboard();
}

/* ---- running objects and external locks ---- */

struct runnable {
	IRunnableObject iface;
	IExternalConnection conn;
	LONG refs;
	int runs, running, locks, contained, connections;
};

static struct runnable *from_conn(IExternalConnection *c)
{
	return (struct runnable *)((char *)c - offsetof(struct runnable, conn));
}

WINAPI static HRESULT ro_qi(IRunnableObject *self, REFIID riid, void **ppv)
{
	struct runnable *r = (struct runnable *)self;
	if (!memcmp(riid, &IID_IExternalConnection, sizeof(GUID)))
		*ppv = &r->conn;
	else if (!memcmp(riid, &iid_unknown, sizeof(GUID)) ||
		 !memcmp(riid, &IID_IRunnableObject, sizeof(GUID)))
		*ppv = self;
	else {
		*ppv = NULL;
		return E_NOINTERFACE;
	}
	r->refs++;
	return S_OK;
}

WINAPI static ULONG ro_addref(IRunnableObject *self)
{
	return ++((struct runnable *)self)->refs;
}

WINAPI static ULONG ro_release(IRunnableObject *self)
{
	return --((struct runnable *)self)->refs;
}

WINAPI static HRESULT ro_class(IRunnableObject *self, CLSID *c)
{
	return E_NOTIMPL;
}

WINAPI static HRESULT ro_run(IRunnableObject *self, IBindCtx *bc)
{
	struct runnable *r = (struct runnable *)self;
	r->runs++;
	r->running = 1;
	return S_OK;
}

WINAPI static BOOL ro_isrunning(IRunnableObject *self)
{
	return ((struct runnable *)self)->running;
}

WINAPI static HRESULT ro_lock(IRunnableObject *self, BOOL lock, BOOL last)
{
	((struct runnable *)self)->locks += lock ? 1 : -1;
	return S_OK;
}

WINAPI static HRESULT ro_contained(IRunnableObject *self, BOOL c)
{
	((struct runnable *)self)->contained = c;
	return S_OK;
}

static const IRunnableObjectVtbl ro_vtbl = {
	ro_qi, ro_addref, ro_release, ro_class, ro_run, ro_isrunning, ro_lock,
	ro_contained,
};

WINAPI static HRESULT ec_qi(IExternalConnection *self, REFIID riid, void **ppv)
{
	return ro_qi(&from_conn(self)->iface, riid, ppv);
}

WINAPI static ULONG ec_addref(IExternalConnection *self)
{
	return ro_addref(&from_conn(self)->iface);
}

WINAPI static ULONG ec_release(IExternalConnection *self)
{
	return ro_release(&from_conn(self)->iface);
}

WINAPI static DWORD ec_add(IExternalConnection *self, DWORD type, DWORD r)
{
	return ++from_conn(self)->connections;
}

WINAPI static DWORD ec_rel(IExternalConnection *self, DWORD type, DWORD r,
			   BOOL last)
{
	return --from_conn(self)->connections;
}

static const IExternalConnectionVtbl ec_vtbl = {
	ec_qi, ec_addref, ec_release, ec_add, ec_rel,
};

static void test_running(void)
{
	FN(HRESULT, OleRun, IUnknown *);
	FN(BOOL, OleIsRunning, IUnknown *);
	FN(HRESULT, OleLockRunning, IUnknown *, BOOL, BOOL);
	FN(HRESULT, OleSetContainedObject, IUnknown *, BOOL);
	FN(HRESULT, CoLockObjectExternal, IUnknown *, BOOL, BOOL);
	FN(HRESULT, CoDisconnectObject, IUnknown *, DWORD);
	struct runnable r;
	memset(&r, 0, sizeof(r));
	r.iface.lpVtbl = &ro_vtbl;
	r.conn.lpVtbl = &ec_vtbl;
	r.refs = 1;
	struct obj plain = { { &obj_vtbl }, 1 };
	IUnknown *u = (IUnknown *)&r.iface;

	CHECK(!OleIsRunning(u));
	CHECK_EQ(OleRun(u), S_OK);
	CHECK(r.runs == 1 && OleIsRunning(u));
	CHECK_EQ(OleLockRunning(u, TRUE, FALSE), S_OK);
	CHECK_EQ(r.locks, 1);
	CHECK_EQ(OleSetContainedObject(u, TRUE), S_OK);
	CHECK_EQ(r.contained, 1);
	CHECK_EQ(r.refs, 1);
	/* Anything else counts as running. */
	CHECK_EQ(OleRun(&plain.iface), S_OK);
	CHECK(OleIsRunning(&plain.iface));

	CHECK_EQ(CoLockObjectExternal(u, TRUE, TRUE), S_OK);
	CHECK_EQ(CoLockObjectExternal(u, TRUE, TRUE), S_OK);
	CHECK(r.refs == 3 && r.connections == 2);
	CHECK_EQ(CoLockObjectExternal(u, FALSE, TRUE), S_OK);
	CHECK_EQ(CoLockObjectExternal(u, FALSE, TRUE), S_OK);
	CHECK(r.refs == 1 && r.connections == 0);
	CHECK_EQ(CoLockObjectExternal(u, FALSE, TRUE), S_OK);	/* no lock */
	CHECK_EQ(r.refs, 1);
	CHECK_EQ(CoDisconnectObject(u, 0), S_OK);
}

/* ---- drag and drop ---- */

struct source {
	IDropSource iface;
	int queries, feedback;
	HRESULT answer;
};

WINAPI static HRESULT ds_qi(IDropSource *self, REFIID riid, void **ppv)
{
	*ppv = self;
	return S_OK;
}

WINAPI static ULONG ds_addref(IDropSource *self)
{
	return 2;
}

WINAPI static ULONG ds_release(IDropSource *self)
{
	return 1;
}

WINAPI static HRESULT ds_query(IDropSource *self, BOOL esc, DWORD keys)
{
	struct source *s = (struct source *)self;
	s->queries++;
	return keys & 3 ? S_OK : s->answer;
}

WINAPI static HRESULT ds_feedback(IDropSource *self, DWORD effect)
{
	((struct source *)self)->feedback++;
	return DRAGDROP_S_USEDEFAULTCURSORS;
}

static const IDropSourceVtbl ds_vtbl = {
	ds_qi, ds_addref, ds_release, ds_query, ds_feedback,
};

static void test_drag_drop(void)
{
	FN(HRESULT, RegisterDragDrop, HWND, IDropTarget *);
	FN(HRESULT, RevokeDragDrop, HWND);
	FN(HRESULT, DoDragDrop, IDataObject *, IDropSource *, DWORD, DWORD *);
	U32(HWND, CreateWindowExA, DWORD, LPCSTR, LPCSTR, DWORD, int, int, int,
	    int, HWND, HMENU, HINSTANCE, LPVOID);
	U32(BOOL, DestroyWindow, HWND);
	struct obj target = { { &obj_vtbl }, 1 };
	IDropTarget *t = (IDropTarget *)&target.iface;
	struct data src = { { &data_vtbl }, 1, "dragged" };
	struct source ds = { { &ds_vtbl }, 0, 0, DRAGDROP_S_DROP };
	DWORD effect = 99;

	HWND w = CreateWindowExA(0, "STATIC", "drop", 0, 0, 0, 10, 10, NULL,
				 NULL, NULL, NULL);
	CHECK(w != NULL);
	CHECK_EQ(RegisterDragDrop((HWND)(uintptr_t)0x1234, t),
		 DRAGDROP_E_INVALIDHWND);
	CHECK_EQ(RegisterDragDrop(w, t), S_OK);
	CHECK_EQ(target.refs, 2);
	CHECK_EQ(RegisterDragDrop(w, t), DRAGDROP_E_ALREADYREGISTERED);
	CHECK_EQ(RevokeDragDrop(w), S_OK);
	CHECK_EQ(target.refs, 1);
	CHECK_EQ(RevokeDragDrop(w), DRAGDROP_E_NOTREGISTERED);

	/* A drop with no target under it is a cancel. */
	CHECK_EQ(DoDragDrop(&src.iface, &ds.iface, DROPEFFECT_COPY, &effect),
		 DRAGDROP_S_CANCEL);
	CHECK_EQ(effect, DROPEFFECT_NONE);
	CHECK(ds.queries == 1 && ds.feedback == 1);
	ds.answer = DRAGDROP_S_CANCEL;
	CHECK_EQ(DoDragDrop(&src.iface, &ds.iface, DROPEFFECT_COPY, &effect),
		 DRAGDROP_S_CANCEL);
	CHECK_EQ(src.refs, 1);
	DestroyWindow(w);
}

/* ---- the registry ---- */

static void test_registry(void)
{
	FN(HRESULT, OleRegGetUserType, REFCLSID, DWORD, LPOLESTR *);
	FN(HRESULT, OleRegGetMiscStatus, REFCLSID, DWORD, DWORD *);
	FN(HRESULT, OleRegEnumVerbs, REFCLSID, IEnumOLEVERB **);
	FN(void, CoTaskMemFree, LPVOID);
	const char *base = "HKEY_LOCAL_MACHINE\\Software\\Classes\\CLSID\\"
			   CLSID_A;
	char key[256];
	LPOLESTR s;
	DWORD st;

	CHECK_EQ(OleRegGetUserType(&clsid_a, USERCLASSTYPE_FULL, &s),
		 REGDB_E_CLASSNOTREG);
	CHECK(s == NULL);
	set_value(base, "Test Document");
	snprintf(key, sizeof(key), "%s\\AuxUserType\\3", base);
	set_value(key, "Test App");
	CHECK_EQ(OleRegGetUserType(&clsid_a, USERCLASSTYPE_FULL, &s), S_OK);
	CHECK(wis(s, "Test Document"));
	CoTaskMemFree(s);
	CHECK_EQ(OleRegGetUserType(&clsid_a, USERCLASSTYPE_APPNAME, &s), S_OK);
	CHECK(wis(s, "Test App"));
	CoTaskMemFree(s);
	/* No short name: the full one. */
	CHECK_EQ(OleRegGetUserType(&clsid_a, USERCLASSTYPE_SHORT, &s), S_OK);
	CHECK(wis(s, "Test Document"));
	CoTaskMemFree(s);

	CHECK_EQ(OleRegGetMiscStatus(&clsid_a, DVASPECT_CONTENT, &st), S_OK);
	CHECK_EQ(st, 0);
	snprintf(key, sizeof(key), "%s\\MiscStatus", base);
	set_value(key, "16");
	snprintf(key, sizeof(key), "%s\\MiscStatus\\4", base);
	set_value(key, "131473");
	CHECK_EQ(OleRegGetMiscStatus(&clsid_a, DVASPECT_CONTENT, &st), S_OK);
	CHECK_EQ(st, 16);
	CHECK_EQ(OleRegGetMiscStatus(&clsid_a, DVASPECT_ICON, &st), S_OK);
	CHECK_EQ(st, 131473);

	IEnumOLEVERB *e;
	CHECK_EQ(OleRegEnumVerbs(&clsid_a, &e), OLEOBJ_E_NOVERBS);
	snprintf(key, sizeof(key), "%s\\Verb\\0", base);
	set_value(key, "&Edit,0,2");
	snprintf(key, sizeof(key), "%s\\Verb\\-1", base);
	set_value(key, "Show,0,0");
	snprintf(key, sizeof(key), "%s\\Verb\\1", base);
	set_value(key, "&Open,8,3");
	CHECK_EQ(OleRegEnumVerbs(&clsid_a, &e), S_OK);
	OLEVERB v[4];
	ULONG got = 0;
	CHECK_EQ(e->lpVtbl->Next(e, 4, v, &got), S_FALSE);
	CHECK_EQ(got, 3);
	if (got == 3) {
		CHECK(v[0].lVerb == -1 && wis(v[0].lpszVerbName, "Show"));
		CHECK(v[1].lVerb == 0 && wis(v[1].lpszVerbName, "&Edit") &&
		      v[1].grfAttribs == 2);
		CHECK(v[2].lVerb == 1 && wis(v[2].lpszVerbName, "&Open") &&
		      v[2].fuFlags == 8 && v[2].grfAttribs == 3);
	}
	for (ULONG i = 0; i < got; i++)
		CoTaskMemFree(v[i].lpszVerbName);
	e->lpVtbl->Release(e);
}

/* ---- TreatAs ---- */

static struct obj made_b = { { &obj_vtbl }, 0 };

WINAPI static HRESULT cf_qi(IClassFactory *self, REFIID riid, void **ppv)
{
	*ppv = self;
	return S_OK;
}

WINAPI static ULONG cf_addref(IClassFactory *self)
{
	return 2;
}

WINAPI static ULONG cf_release(IClassFactory *self)
{
	return 1;
}

WINAPI static HRESULT cf_create(IClassFactory *self, IUnknown *outer,
				REFIID riid, void **ppv)
{
	return obj_qi(&made_b.iface, riid, ppv);
}

WINAPI static HRESULT cf_lock(IClassFactory *self, BOOL lock)
{
	return S_OK;
}

static const IClassFactoryVtbl cf_vtbl = {
	cf_qi, cf_addref, cf_release, cf_create, cf_lock,
};
static IClassFactory factory_b = { &cf_vtbl };

static void test_treat_as(void)
{
	FN(HRESULT, CoRegisterClassObject, REFCLSID, LPUNKNOWN, DWORD, DWORD,
	   LPDWORD);
	FN(HRESULT, CoRevokeClassObject, DWORD);
	FN(HRESULT, CoTreatAsClass, REFCLSID, REFCLSID);
	FN(HRESULT, CoGetTreatAsClass, REFCLSID, CLSID *);
	FN(HRESULT, CoCreateInstance, REFCLSID, LPUNKNOWN, DWORD, REFIID,
	   LPVOID *);
	FN(HRESULT, CoCreateInstanceEx, REFCLSID, IUnknown *, DWORD, void *,
	   DWORD, MULTI_QI *);
	static const CLSID null_clsid;
	DWORD cookie;
	CLSID c;
	void *p;

	CHECK_EQ(CoRegisterClassObject(&clsid_b, (IUnknown *)&factory_b,
				       CLSCTX_INPROC_SERVER, REGCLS_MULTIPLEUSE,
				       &cookie), S_OK);
	CHECK_EQ(CoGetTreatAsClass(&clsid_a, &c), S_FALSE);
	CHECK(!memcmp(&c, &clsid_a, sizeof(c)));
	CHECK_EQ(CoCreateInstance(&clsid_a, NULL, CLSCTX_ALL, &iid_unknown, &p),
		 REGDB_E_CLASSNOTREG);
	CHECK_EQ(CoTreatAsClass(&clsid_a, &clsid_b), S_OK);
	CHECK_EQ(CoGetTreatAsClass(&clsid_a, &c), S_OK);
	CHECK(!memcmp(&c, &clsid_b, sizeof(c)));
	CHECK_EQ(CoCreateInstance(&clsid_a, NULL, CLSCTX_ALL, &iid_unknown, &p),
		 S_OK);
	CHECK(p == &made_b);
	obj_release(&made_b.iface);

	MULTI_QI qi[2] = { { &iid_other, NULL, 0 },
			   { &IID_IDataObject, NULL, 0 } };
	CHECK_EQ(CoCreateInstanceEx(&clsid_a, NULL, CLSCTX_ALL, NULL, 2, qi),
		 CO_S_NOTALLINTERFACES);
	CHECK(qi[0].hr == S_OK && qi[0].pItf == &made_b.iface);
	CHECK(qi[1].hr == E_NOINTERFACE && !qi[1].pItf);
	obj_release(&made_b.iface);
	CHECK_EQ(made_b.refs, 0);

	CHECK_EQ(CoTreatAsClass(&clsid_a, &null_clsid), S_OK);
	CHECK_EQ(CoGetTreatAsClass(&clsid_a, &c), S_FALSE);
	/* With an AutoTreatAs: the class itself goes back to it, but
	 * CLSID_NULL removes emulation all the same. */
	set_value("HKEY_LOCAL_MACHINE\\Software\\Classes\\CLSID\\" CLSID_A
		  "\\AutoTreatAs", "{0B0B0B0B-0001-0002-0304-05060708090A}");
	CHECK_EQ(CoTreatAsClass(&clsid_a, &clsid_a), S_OK);
	CHECK_EQ(CoGetTreatAsClass(&clsid_a, &c), S_OK);
	CHECK(!memcmp(&c, &clsid_b, sizeof(c)));
	CHECK_EQ(CoTreatAsClass(&clsid_a, &null_clsid), S_OK);
	CHECK_EQ(CoGetTreatAsClass(&clsid_a, &c), S_FALSE);
	/* A per-user redirect, which resolution sees first, is cleared
	 * too. */
	set_value("HKEY_CURRENT_USER\\Software\\Classes\\CLSID\\" CLSID_A
		  "\\TreatAs", "{0B0B0B0B-0001-0002-0304-05060708090A}");
	CHECK_EQ(CoGetTreatAsClass(&clsid_a, &c), S_OK);
	CHECK_EQ(CoTreatAsClass(&clsid_a, &null_clsid), S_OK);
	CHECK_EQ(CoGetTreatAsClass(&clsid_a, &c), S_FALSE);
	CHECK_EQ(CoTreatAsClass(&clsid_b, &clsid_a), REGDB_E_CLASSNOTREG);
	CHECK_EQ(CoRevokeClassObject(cookie), S_OK);
}

/* ---- accelerators ---- */

static void test_accelerators(void)
{
	FN(BOOL, IsAccelerator, HACCEL, int, LPMSG, WORD *);
	U32(HACCEL, CreateAcceleratorTableW, LPACCEL, int);
	U32(HACCEL, CreateAcceleratorTableA, LPACCEL, int);
	U32(int, CopyAcceleratorTableW, HACCEL, LPACCEL, int);
	U32(int, CopyAcceleratorTableA, HACCEL, LPACCEL, int);
	U32(BOOL, DestroyAcceleratorTable, HACCEL);
	U32(int, TranslateAcceleratorW, HWND, HACCEL, LPMSG);
	U32(BOOL, SetKeyboardState, PBYTE);
	U32(SHORT, GetKeyState, int);
	U32(BOOL, PostThreadMessageA, DWORD, UINT, WPARAM, LPARAM);
	U32(BOOL, PeekMessageA, LPMSG, HWND, UINT, UINT, UINT);
	K32(DWORD, GetCurrentThreadId, void);
	ACCEL table[] = {
		{ FVIRTKEY | FCONTROL, 'S', 100 },
		{ 0, 'x', 101 },
		{ FVIRTKEY, 0x70 /* VK_F1 */, 102 },
		{ FVIRTKEY | FALT, 'A', 103 },
		{ 0, 0xe9 /* é in ANSI */, 104 },
	};
	MSG m;
	WORD cmd = 0;

	CHECK(CreateAcceleratorTableW(table, 0) == NULL);
	HACCEL h = CreateAcceleratorTableW(table, 4);
	CHECK(h != NULL);
	CHECK_EQ(CopyAcceleratorTableW(h, NULL, 0), 4);
	ACCEL back[4];
	CHECK_EQ(CopyAcceleratorTableW(h, back, 2), 2);
	CHECK(back[1].key == 'x' && back[1].cmd == 101);

	memset(&m, 0, sizeof(m));
	m.message = WM_CHAR;
	m.wParam = 'x';
	CHECK(IsAccelerator(h, 4, &m, &cmd) && cmd == 101);
	m.message = WM_KEYDOWN;
	m.wParam = 0x70;
	CHECK(IsAccelerator(h, 4, &m, &cmd) && cmd == 102);
	CHECK(!IsAccelerator(h, 2, &m, &cmd));	/* past what's searched */
	m.wParam = 'S';				/* without CTRL down */
	CHECK(!IsAccelerator(h, 4, &m, &cmd));
	/* With CTRL down, as the thread's key state says. */
	BYTE keys[256] = { 0 };
	keys[0x11] = 0x80;
	CHECK(SetKeyboardState(keys));
	CHECK(GetKeyState(0x11) < 0);
	CHECK(IsAccelerator(h, 4, &m, &cmd) && cmd == 100);
	keys[0x11] = 0;
	SetKeyboardState(keys);
	CHECK(!IsAccelerator(h, 4, &m, &cmd));
	/* Key state follows the keyboard messages the thread takes. */
	MSG got;
	CHECK(PostThreadMessageA(GetCurrentThreadId(), WM_KEYDOWN, 0x11, 0));
	CHECK(PeekMessageA(&got, NULL, 0, 0, PM_REMOVE));
	CHECK(GetKeyState(0x11) < 0);
	CHECK(IsAccelerator(h, 4, &m, &cmd) && cmd == 100);
	CHECK(PostThreadMessageA(GetCurrentThreadId(), WM_KEYUP, 0x11, 0));
	CHECK(PeekMessageA(&got, NULL, 0, 0, PM_REMOVE));
	CHECK(GetKeyState(0x11) >= 0);
	CHECK(!IsAccelerator(h, 4, &m, &cmd));
	m.message = WM_SYSKEYDOWN;
	m.wParam = 'A';
	m.lParam = 0x20000000;
	CHECK(IsAccelerator(h, 4, &m, &cmd) && cmd == 103);
	m.message = WM_KEYUP;
	CHECK(!IsAccelerator(h, 4, &m, &cmd));
	CHECK_EQ(TranslateAcceleratorW(NULL, h, &m), 0);
	CHECK(DestroyAcceleratorTable(h));
	CHECK(!DestroyAcceleratorTable(h));
	CHECK(!IsAccelerator(h, 4, &m, &cmd));

	/* The A forms take characters in the ANSI code page. */
	h = CreateAcceleratorTableA(&table[4], 1);
	CHECK_EQ(CopyAcceleratorTableW(h, back, 1), 1);
	CHECK_EQ(back[0].key, 0xe9);	/* U+00E9 in 1252 too */
	CHECK_EQ(CopyAcceleratorTableA(h, back, 1), 1);
	CHECK_EQ(back[0].key, 0xe9);
	DestroyAcceleratorTable(h);
}

static void test_helpers(void)
{
	FN(HRESULT, CoWaitForMultipleHandles, DWORD, DWORD, ULONG, HANDLE *,
	   DWORD *);
	FN(HRESULT, CoInitializeSecurity, void *, LONG, void *, void *, DWORD,
	   DWORD, void *, DWORD, void *);
	FN(HRESULT, CoSetProxyBlanket, IUnknown *, DWORD, DWORD, OLECHAR *,
	   DWORD, DWORD, void *, DWORD);
	FN(HRESULT, CoRegisterMessageFilter, IMessageFilter *,
	   IMessageFilter **);
	FN(HOLEMENU, OleCreateMenuDescriptor, HMENU, LPOLEMENUGROUPWIDTHS);
	FN(HRESULT, OleDestroyMenuDescriptor, HOLEMENU);
	FN(HINSTANCE, CoLoadLibrary, LPOLESTR, BOOL);
	FN(void, CoFreeLibrary, HINSTANCE);
	K32(HANDLE, CreateEventA, void *, BOOL, BOOL, LPCSTR);
	K32(BOOL, CloseHandle, HANDLE);
	struct obj o = { { &obj_vtbl }, 1 };
	HANDLE ev[2];
	DWORD idx = 99;

	ev[0] = CreateEventA(NULL, TRUE, FALSE, NULL);
	ev[1] = CreateEventA(NULL, TRUE, TRUE, NULL);
	CHECK_EQ(CoWaitForMultipleHandles(0, 0, 2, ev, &idx), S_OK);
	CHECK_EQ(idx, 1);
	CHECK_EQ(CoWaitForMultipleHandles(COWAIT_WAITALL, 0, 2, ev, &idx),
		 RPC_S_CALLPENDING);
	CHECK_EQ(CoWaitForMultipleHandles(0, 0, 0, ev, &idx), E_INVALIDARG);
	CloseHandle(ev[0]);
	CloseHandle(ev[1]);

	CHECK_EQ(CoInitializeSecurity(NULL, -1, NULL, NULL, 0, 0, NULL, 0, NULL),
		 S_OK);
	CHECK_EQ(CoInitializeSecurity(NULL, -1, NULL, NULL, 0, 0, NULL, 0, NULL),
		 RPC_E_TOO_LATE);
	CHECK_EQ(CoSetProxyBlanket(&o.iface, 0, 0, NULL, 0, 0, NULL, 0),
		 E_NOINTERFACE);

	IMessageFilter *old = (IMessageFilter *)1;
	IMessageFilter *mf = (IMessageFilter *)&o.iface;
	CHECK_EQ(CoRegisterMessageFilter(mf, &old), S_OK);
	CHECK(old == NULL);
	CHECK_EQ(o.refs, 2);
	CHECK_EQ(CoRegisterMessageFilter(NULL, &old), S_OK);
	CHECK(old == mf);
	obj_release(&o.iface);

	OLEMENUGROUPWIDTHS widths = { { 1, 0, 1, 0, 1, 0 } };
	CHECK(OleCreateMenuDescriptor(NULL, &widths) == NULL);
	HOLEMENU hm = OleCreateMenuDescriptor((HMENU)(uintptr_t)0x10, &widths);
	CHECK(hm != NULL);
	CHECK_EQ(OleDestroyMenuDescriptor(hm), S_OK);

	WCHAR *name = wide("kernel32.dll");
	HINSTANCE lib = CoLoadLibrary(name, TRUE);
	CHECK(lib != NULL);
	CoFreeLibrary(lib);
	free(name);
}

int main(void)
{
	char dir[] = "/tmp/vfw-olemisc-XXXXXX";
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
	k32 = mod_load("kernel32.dll", NULL);
	u32 = mod_load("user32.dll", NULL);
	CHECK(ole && k32 && u32);
	if (!ole || !k32 || !u32)
		return test_summary("test_olemisc");

	FN(HRESULT, OleInitialize, LPVOID);
	FN(void, OleUninitialize, void);
	CHECK_EQ(OleInitialize(NULL), S_OK);

	/* What the test data object offers. */
	{
		struct {
			IEnumFORMATETC *e;
		} hold;
		FORMATETC f = { CF_TEXT, NULL, DVASPECT_CONTENT, -1,
				TYMED_HGLOBAL };
		/* An enumerator from OleGetClipboard's would do, but build one
		 * the plain way: through a data advise holder's sibling isn't
		 * exported, so use the clipboard after setting CF_TEXT. */
		U32(BOOL, OpenClipboard, HWND);
		U32(BOOL, EmptyClipboard, void);
		U32(HANDLE, SetClipboardData, UINT, HANDLE);
		U32(BOOL, CloseClipboard, void);
		FN(HRESULT, OleGetClipboard, IDataObject **);
		IDataObject *d;
		CHECK(OpenClipboard(NULL) && EmptyClipboard());
		SetClipboardData(CF_TEXT, global_text("x"));
		CloseClipboard();
		CHECK_EQ(OleGetClipboard(&d), S_OK);
		CHECK_EQ(d->lpVtbl->EnumFormatEtc(d, DATADIR_GET, &hold.e), S_OK);
		d->lpVtbl->Release(d);
		text_formats = hold.e;
		(void)f;
	}

	test_media(dir);
	test_advise();
	test_clipboard();
	test_running();
	test_drag_drop();
	test_registry();
	test_treat_as();
	test_accelerators();
	test_helpers();

	text_formats->lpVtbl->Release(text_formats);
	OleUninitialize();
	mod_free(u32);
	mod_free(k32);
	mod_free(ole);
	char cmd[64];
	snprintf(cmd, sizeof(cmd), "rm -rf '%s'", dir);
	if (system(cmd))
		fprintf(stderr, "could not remove %s\n", dir);
	return test_summary("test_olemisc");
}
