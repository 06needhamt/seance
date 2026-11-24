/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * The rest of ole32's OLE: storage media, advise holders, the OLE
 * clipboard, running and locking objects, drag and drop, the registry's
 * OLE keys (user types, MiscStatus, verbs, TreatAs), message filters,
 * security, menu descriptors and accelerators, and a few Co* helpers.
 *
 * There is one process and no desktop: an object is never a proxy, so the
 * security calls have nothing to set; the OLE clipboard renders global
 * memory formats into the user32 clipboard when set, and hands its data
 * object back to this process; DoDragDrop ends as soon as it starts, with
 * no mouse button down, and drops nowhere.
 */
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "common/log.h"
#include "runtime/builtin.h"
#include "win32/codepage.h"
#include "win32/internal.h"
#include "win32/ole.h"
#include "win32/reg.h"
#include "win32/user.h"

extern const struct builtin_dll builtin_kernel32, builtin_user32,
	builtin_gdi32, builtin_ole32;

#define DATA_S_SAMEFORMATETC ((HRESULT)0x00040130)
#define GMEM_MOVEABLE 0x0002

static int guid_eq(const GUID *a, const GUID *b)
{
	return !memcmp(a, b, sizeof(*a));
}

static void *k32(const char *name)
{
	return builtin_export(&builtin_kernel32, name);
}

static void *u32(const char *name)
{
	return builtin_export(&builtin_user32, name);
}

static void *g32(const char *name)
{
	return builtin_export(&builtin_gdi32, name);
}

/* ---- global memory ---- */

static HGLOBAL global_copy(HGLOBAL h, UINT flags)
{
	SIZE_T (WINAPI *size)(HGLOBAL) = k32("GlobalSize");
	HGLOBAL (WINAPI *alloc)(UINT, SIZE_T) = k32("GlobalAlloc");
	void *(WINAPI *lock)(HGLOBAL) = k32("GlobalLock");
	BOOL (WINAPI *unlock)(HGLOBAL) = k32("GlobalUnlock");
	HGLOBAL (WINAPI *gfree)(HGLOBAL) = k32("GlobalFree");

	if (!h || !size || !alloc || !lock || !unlock || !gfree)
		return NULL;
	SIZE_T n = size(h);
	HGLOBAL d = alloc(flags ? flags : GMEM_MOVEABLE, n ? n : 1);
	if (!d)
		return NULL;
	void *src = lock(h), *dst = lock(d);
	if (!src || !dst) {
		if (src)
			unlock(h);
		if (dst)
			unlock(d);
		gfree(d);
		return NULL;
	}
	memcpy(dst, src, n);
	unlock(h);
	unlock(d);
	return d;
}

static void global_free(HGLOBAL h)
{
	HGLOBAL (WINAPI *gfree)(HGLOBAL) = k32("GlobalFree");
	if (h && gfree)
		gfree(h);
}

/* ---- storage media ---- */

WINFN(void) ole_ReleaseStgMedium(STGMEDIUM *med)
{
	if (!med)
		return;
	IUnknown *punk = med->pUnkForRelease;
	switch (med->tymed) {
	case TYMED_HGLOBAL:
		if (!punk)
			global_free(med->hGlobal);
		break;
	case TYMED_FILE:
		if (med->lpszFileName) {
			if (!punk) {
				BOOL (WINAPI *del)(LPCWSTR) = k32("DeleteFileW");
				if (del)
					del(med->lpszFileName);
			}
			ole_taskmem_free(med->lpszFileName);
		}
		break;
	case TYMED_ISTREAM:
		if (med->pstm)
			med->pstm->lpVtbl->Release(med->pstm);
		break;
	case TYMED_ISTORAGE:
		if (med->pstg)
			med->pstg->lpVtbl->Release(med->pstg);
		break;
	case TYMED_GDI:
		if (!punk && med->hBitmap) {
			BOOL (WINAPI *del)(HGDIOBJ) = g32("DeleteObject");
			if (del)
				del((HGDIOBJ)med->hBitmap);
		}
		break;
	case TYMED_MFPICT:
		/* The METAFILEPICT's metafile goes with the process. */
		if (!punk)
			global_free(med->hMetaFilePict);
		break;
	case TYMED_ENHMF:
		if (!punk && med->hEnhMetaFile) {
			BOOL (WINAPI *del)(HANDLE) = g32("DeleteEnhMetaFile");
			if (del)
				del(med->hEnhMetaFile);
		}
		break;
	default:
		break;
	}
	med->tymed = TYMED_NULL;
	if (punk)
		punk->lpVtbl->Release(punk);
	med->pUnkForRelease = NULL;
}

/* Global memory formats are copied; GDI objects can't be, here. */
WINFN(HANDLE) ole_OleDuplicateData(HANDLE src, CLIPFORMAT cf, UINT flags)
{
	if (!src)
		return NULL;
	switch (cf) {
	case 2:		/* CF_BITMAP */
	case 3:		/* CF_METAFILEPICT */
	case 9:		/* CF_PALETTE */
	case 14:	/* CF_ENHMETAFILE */
		return NULL;
	default:
		return global_copy(src, flags);
	}
}

/* ---- FORMATETC copies ---- */

static DVTARGETDEVICE *ptd_dup(const DVTARGETDEVICE *ptd)
{
	if (!ptd)
		return NULL;
	DWORD n = ptd->tdSize < sizeof(DWORD) ? sizeof(DWORD) : ptd->tdSize;
	DVTARGETDEVICE *d = ole_taskmem_alloc(n);
	if (d)
		memcpy(d, ptd, n);
	return d;
}

static int fmt_copy(FORMATETC *dst, const FORMATETC *src)
{
	*dst = *src;
	dst->ptd = ptd_dup(src->ptd);
	return !src->ptd || dst->ptd;
}

/* ---- IEnumSTATDATA over a snapshot ---- */

struct ensd {
	IEnumSTATDATA iface;
	LONG refs;
	STATDATA *v;
	DWORD n, pos;
};

static const IEnumSTATDATAVtbl ensd_vtbl;

static void statdata_free(STATDATA *s)
{
	ole_taskmem_free(s->formatetc.ptd);
	if (s->pAdvSink)
		s->pAdvSink->lpVtbl->Release(s->pAdvSink);
}

static int statdata_copy(STATDATA *dst, const STATDATA *src)
{
	*dst = *src;
	if (!fmt_copy(&dst->formatetc, &src->formatetc))
		return 0;
	if (dst->pAdvSink)
		dst->pAdvSink->lpVtbl->AddRef(dst->pAdvSink);
	return 1;
}

static HRESULT ensd_new(const STATDATA *v, DWORD n, IEnumSTATDATA **out)
{
	struct ensd *e = calloc(1, sizeof(*e));
	if (!e || (n && !(e->v = calloc(n, sizeof(*e->v))))) {
		free(e);
		return E_OUTOFMEMORY;
	}
	e->iface.lpVtbl = &ensd_vtbl;
	e->refs = 1;
	for (DWORD i = 0; i < n; i++, e->n++)
		if (!statdata_copy(&e->v[i], &v[i])) {
			while (e->n)
				statdata_free(&e->v[--e->n]);
			free(e->v);
			free(e);
			return E_OUTOFMEMORY;
		}
	*out = &e->iface;
	return S_OK;
}

WINFN(HRESULT) ensd_QueryInterface(IEnumSTATDATA *self, REFIID riid,
				   void **ppv)
{
	if (!ppv)
		return E_POINTER;
	if (!guid_eq(riid, &IID_IUnknown) &&
	    !guid_eq(riid, &IID_IEnumSTATDATA)) {
		*ppv = NULL;
		return E_NOINTERFACE;
	}
	*ppv = self;
	__atomic_add_fetch(&((struct ensd *)self)->refs, 1, __ATOMIC_SEQ_CST);
	return S_OK;
}

WINFN(ULONG) ensd_AddRef(IEnumSTATDATA *self)
{
	return __atomic_add_fetch(&((struct ensd *)self)->refs, 1,
				  __ATOMIC_SEQ_CST);
}

WINFN(ULONG) ensd_Release(IEnumSTATDATA *self)
{
	struct ensd *e = (struct ensd *)self;
	ULONG r = __atomic_sub_fetch(&e->refs, 1, __ATOMIC_SEQ_CST);
	if (!r) {
		for (DWORD i = 0; i < e->n; i++)
			statdata_free(&e->v[i]);
		free(e->v);
		free(e);
	}
	return r;
}

WINFN(HRESULT) ensd_Next(IEnumSTATDATA *self, ULONG count, STATDATA *out,
			 ULONG *fetched)
{
	struct ensd *e = (struct ensd *)self;
	ULONG n = 0;

	if (!out || (count > 1 && !fetched))
		return E_POINTER;
	while (n < count && e->pos < e->n) {
		if (!statdata_copy(&out[n], &e->v[e->pos]))
			break;
		e->pos++;
		n++;
	}
	if (fetched)
		*fetched = n;
	return n == count ? S_OK : S_FALSE;
}

WINFN(HRESULT) ensd_Skip(IEnumSTATDATA *self, ULONG count)
{
	struct ensd *e = (struct ensd *)self;
	DWORD left = e->n - e->pos;
	e->pos += count < left ? count : left;
	return count <= left ? S_OK : S_FALSE;
}

WINFN(HRESULT) ensd_Reset(IEnumSTATDATA *self)
{
	((struct ensd *)self)->pos = 0;
	return S_OK;
}

WINFN(HRESULT) ensd_Clone(IEnumSTATDATA *self, IEnumSTATDATA **out)
{
	struct ensd *e = (struct ensd *)self;
	if (!out)
		return E_POINTER;
	HRESULT hr = ensd_new(e->v, e->n, out);
	if (SUCCEEDED(hr))
		((struct ensd *)*out)->pos = e->pos;
	return hr;
}

static const IEnumSTATDATAVtbl ensd_vtbl = {
	ensd_QueryInterface, ensd_AddRef, ensd_Release, ensd_Next, ensd_Skip,
	ensd_Reset, ensd_Clone,
};

/* ---- advise holders ---- */

/* The connections of a holder: slot i is connection i + 1. */
struct conns {
	STATDATA *v;
	unsigned *gen;	/* per slot: which registration holds it */
	BYTE *busy;	/* per slot: a one-shot callback is under way */
	DWORD n;
	unsigned next_gen;
};

static HRESULT conn_add(struct conns *c, const FORMATETC *fmt, DWORD advf,
			IAdviseSink *sink, DWORD *conn)
{
	DWORD i;
	for (i = 0; i < c->n && c->v[i].pAdvSink; i++)
		;
	if (i == c->n) {
		STATDATA *v = realloc(c->v, (c->n + 1) * sizeof(*v));
		if (!v)
			return E_OUTOFMEMORY;
		c->v = v;
		unsigned *g = realloc(c->gen, (c->n + 1) * sizeof(*g));
		if (!g)
			return E_OUTOFMEMORY;
		c->gen = g;
		BYTE *b = realloc(c->busy, c->n + 1);
		if (!b)
			return E_OUTOFMEMORY;
		c->busy = b;
		c->n++;
		memset(&c->v[i], 0, sizeof(c->v[i]));
	}
	c->gen[i] = ++c->next_gen;
	c->busy[i] = 0;
	STATDATA *s = &c->v[i];
	memset(s, 0, sizeof(*s));
	if (fmt && !fmt_copy(&s->formatetc, fmt))
		return E_OUTOFMEMORY;
	s->advf = advf;
	s->pAdvSink = sink;
	s->dwConnection = i + 1;
	sink->lpVtbl->AddRef(sink);
	*conn = i + 1;
	return S_OK;
}

static HRESULT conn_remove(struct conns *c, DWORD conn)
{
	if (!conn || conn > c->n || !c->v[conn - 1].pAdvSink)
		return OLE_E_NOCONNECTION;
	STATDATA *s = &c->v[conn - 1];
	IAdviseSink *sink = s->pAdvSink;
	ole_taskmem_free(s->formatetc.ptd);
	memset(s, 0, sizeof(*s));
	sink->lpVtbl->Release(sink);
	return S_OK;
}

static HRESULT conn_enum(struct conns *c, IEnumSTATDATA **out)
{
	STATDATA *live = calloc(c->n + 1, sizeof(*live));
	DWORD n = 0;
	if (!live)
		return E_OUTOFMEMORY;
	for (DWORD i = 0; i < c->n; i++)
		if (c->v[i].pAdvSink)
			live[n++] = c->v[i];
	HRESULT hr = ensd_new(live, n, out);
	free(live);
	return hr;
}

static void conn_free(struct conns *c)
{
	for (DWORD i = 0; i < c->n; i++)
		if (c->v[i].pAdvSink)
			conn_remove(c, i + 1);
	free(c->v);
	free(c->gen);
	free(c->busy);
}

struct holder {
	union {
		IOleAdviseHolder ole;
		IDataAdviseHolder data;
	};
	LONG refs;
	struct conns c;
};

static HRESULT holder_qi(struct holder *h, const IID *self_iid, REFIID riid,
			 void **ppv)
{
	if (!ppv)
		return E_POINTER;
	if (!guid_eq(riid, &IID_IUnknown) && !guid_eq(riid, self_iid)) {
		*ppv = NULL;
		return E_NOINTERFACE;
	}
	*ppv = h;
	__atomic_add_fetch(&h->refs, 1, __ATOMIC_SEQ_CST);
	return S_OK;
}

static ULONG holder_release(struct holder *h)
{
	ULONG r = __atomic_sub_fetch(&h->refs, 1, __ATOMIC_SEQ_CST);
	if (!r) {
		conn_free(&h->c);
		free(h);
	}
	return r;
}

/* Calls fn on each sink, holding a reference, as the list may change. */
static HRESULT each_sink(struct conns *c,
			 void (*fn)(IAdviseSink *, void *), void *ctx)
{
	DWORD n = c->n;
	IAdviseSink **v = calloc(n + 1, sizeof(*v));
	DWORD k = 0;
	if (!v)
		return E_OUTOFMEMORY;
	for (DWORD i = 0; i < n; i++)
		if ((v[k] = c->v[i].pAdvSink))
			v[k++]->lpVtbl->AddRef(c->v[i].pAdvSink);
	for (DWORD i = 0; i < k; i++) {
		fn(v[i], ctx);
		v[i]->lpVtbl->Release(v[i]);
	}
	free(v);
	return S_OK;
}

WINFN(HRESULT) oah_QueryInterface(IOleAdviseHolder *self, REFIID riid,
				  void **ppv)
{
	return holder_qi((struct holder *)self, &IID_IOleAdviseHolder, riid,
			 ppv);
}

WINFN(ULONG) oah_AddRef(IOleAdviseHolder *self)
{
	return __atomic_add_fetch(&((struct holder *)self)->refs, 1,
				  __ATOMIC_SEQ_CST);
}

WINFN(ULONG) oah_Release(IOleAdviseHolder *self)
{
	return holder_release((struct holder *)self);
}

WINFN(HRESULT) oah_Advise(IOleAdviseHolder *self, IAdviseSink *sink,
			  DWORD *conn)
{
	if (!conn)
		return E_INVALIDARG;
	*conn = 0;
	if (!sink)
		return E_INVALIDARG;
	return conn_add(&((struct holder *)self)->c, NULL, 0, sink, conn);
}

WINFN(HRESULT) oah_Unadvise(IOleAdviseHolder *self, DWORD conn)
{
	return conn_remove(&((struct holder *)self)->c, conn);
}

WINFN(HRESULT) oah_EnumAdvise(IOleAdviseHolder *self, IEnumSTATDATA **e)
{
	if (!e)
		return E_POINTER;
	return conn_enum(&((struct holder *)self)->c, e);
}

static void send_rename(IAdviseSink *s, void *mk)
{
	s->lpVtbl->OnRename(s, mk);
}

static void send_save(IAdviseSink *s, void *ctx)
{
	s->lpVtbl->OnSave(s);
}

static void send_close(IAdviseSink *s, void *ctx)
{
	s->lpVtbl->OnClose(s);
}

WINFN(HRESULT) oah_SendOnRename(IOleAdviseHolder *self, IMoniker *mk)
{
	if (!mk)
		return E_INVALIDARG;
	return each_sink(&((struct holder *)self)->c, send_rename, mk);
}

WINFN(HRESULT) oah_SendOnSave(IOleAdviseHolder *self)
{
	return each_sink(&((struct holder *)self)->c, send_save, NULL);
}

WINFN(HRESULT) oah_SendOnClose(IOleAdviseHolder *self)
{
	return each_sink(&((struct holder *)self)->c, send_close, NULL);
}

static const IOleAdviseHolderVtbl oah_vtbl = {
	oah_QueryInterface, oah_AddRef, oah_Release, oah_Advise, oah_Unadvise,
	oah_EnumAdvise, oah_SendOnRename, oah_SendOnSave, oah_SendOnClose,
};

WINFN(HRESULT) ole_CreateOleAdviseHolder(IOleAdviseHolder **out)
{
	if (!out)
		return E_INVALIDARG;
	struct holder *h = calloc(1, sizeof(*h));
	if (!h) {
		*out = NULL;
		return E_OUTOFMEMORY;
	}
	h->ole.lpVtbl = &oah_vtbl;
	h->refs = 1;
	*out = &h->ole;
	return S_OK;
}

WINFN(HRESULT) dah_QueryInterface(IDataAdviseHolder *self, REFIID riid,
				  void **ppv)
{
	return holder_qi((struct holder *)self, &IID_IDataAdviseHolder, riid,
			 ppv);
}

WINFN(ULONG) dah_AddRef(IDataAdviseHolder *self)
{
	return __atomic_add_fetch(&((struct holder *)self)->refs, 1,
				  __ATOMIC_SEQ_CST);
}

WINFN(ULONG) dah_Release(IDataAdviseHolder *self)
{
	return holder_release((struct holder *)self);
}

/* One data change to one sink: with the data, unless ADVF_NODATA (the
 * last change before a stop carries it for ADVF_DATAONSTOP). Returns
 * whether the sink was called. */
static int notify(IDataObject *data, STATDATA *s, int stopping)
{
	STGMEDIUM med;
	FORMATETC fmt = s->formatetc;
	int want = !(s->advf & ADVF_NODATA) ||
		   (stopping && (s->advf & ADVF_DATAONSTOP));

	memset(&med, 0, sizeof(med));
	if (want && data && FAILED(data->lpVtbl->GetData(data, &fmt, &med)))
		return 0;
	s->pAdvSink->lpVtbl->OnDataChange(s->pAdvSink, &fmt, &med);
	ole_ReleaseStgMedium(&med);
	return 1;
}

/* Notifies slot i. A one-shot registration is marked busy while its
 * callback runs, so a nested send skips it; once delivered it goes, but
 * only if the slot still holds that registration (the sink may have
 * unadvised and a new one taken the slot). An undelivered one stays. */
static HRESULT notify_slot(struct conns *c, DWORD i, IDataObject *data,
			   int stopping)
{
	STATDATA s;
	if (!c->v[i].pAdvSink || c->busy[i])
		return S_OK;
	if (!statdata_copy(&s, &c->v[i]))
		return E_OUTOFMEMORY;
	unsigned gen = c->gen[i];
	int once = !!(s.advf & ADVF_ONLYONCE);
	if (once)
		c->busy[i] = 1;
	int done = notify(data, &s, stopping);
	statdata_free(&s);
	if (i < c->n && c->gen[i] == gen && c->v[i].pAdvSink) {
		c->busy[i] = 0;
		if (once && done)
			conn_remove(c, i + 1);
	}
	return S_OK;
}

WINFN(HRESULT) dah_Advise(IDataAdviseHolder *self, IDataObject *data,
			  FORMATETC *fmt, DWORD advf, IAdviseSink *sink,
			  DWORD *conn)
{
	struct holder *h = (struct holder *)self;
	DWORD c;

	if (conn)
		*conn = 0;
	if (!fmt || !sink)
		return E_INVALIDARG;
	HRESULT hr = conn_add(&h->c, fmt, advf, sink, &c);
	if (FAILED(hr))
		return hr;
	if (conn)
		*conn = c;
	if ((advf & ADVF_PRIMEFIRST) && data)
		notify_slot(&h->c, c - 1, data, 0);
	return S_OK;
}

WINFN(HRESULT) dah_Unadvise(IDataAdviseHolder *self, DWORD conn)
{
	return conn_remove(&((struct holder *)self)->c, conn);
}

WINFN(HRESULT) dah_EnumAdvise(IDataAdviseHolder *self, IEnumSTATDATA **e)
{
	if (!e)
		return E_POINTER;
	return conn_enum(&((struct holder *)self)->c, e);
}

WINFN(HRESULT) dah_SendOnDataChange(IDataAdviseHolder *self,
				    IDataObject *data, DWORD reserved,
				    DWORD advf)
{
	struct holder *h = (struct holder *)self;

	if (!data)
		return E_INVALIDARG;
	int stopping = !!(advf & ADVF_DATAONSTOP);
	for (DWORD i = 0; i < h->c.n; i++) {
		HRESULT hr = notify_slot(&h->c, i, data, stopping);
		if (FAILED(hr))
			return hr;
	}
	return S_OK;
}

static const IDataAdviseHolderVtbl dah_vtbl = {
	dah_QueryInterface, dah_AddRef, dah_Release, dah_Advise, dah_Unadvise,
	dah_EnumAdvise, dah_SendOnDataChange,
};

WINFN(HRESULT) ole_CreateDataAdviseHolder(IDataAdviseHolder **out)
{
	if (!out)
		return E_INVALIDARG;
	struct holder *h = calloc(1, sizeof(*h));
	if (!h) {
		*out = NULL;
		return E_OUTOFMEMORY;
	}
	h->data.lpVtbl = &dah_vtbl;
	h->refs = 1;
	*out = &h->data;
	return S_OK;
}

/* ---- IEnumFORMATETC over a snapshot ---- */

struct enf {
	IEnumFORMATETC iface;
	LONG refs;
	FORMATETC *v;
	DWORD n, pos;
};

static const IEnumFORMATETCVtbl enf_vtbl;

static HRESULT enf_new(const FORMATETC *v, DWORD n, IEnumFORMATETC **out)
{
	struct enf *e = calloc(1, sizeof(*e));
	if (!e || (n && !(e->v = calloc(n, sizeof(*e->v))))) {
		free(e);
		return E_OUTOFMEMORY;
	}
	e->iface.lpVtbl = &enf_vtbl;
	e->refs = 1;
	for (DWORD i = 0; i < n; i++, e->n++)
		if (!fmt_copy(&e->v[i], &v[i])) {
			while (e->n)
				ole_taskmem_free(e->v[--e->n].ptd);
			free(e->v);
			free(e);
			return E_OUTOFMEMORY;
		}
	*out = &e->iface;
	return S_OK;
}

WINFN(HRESULT) enf_QueryInterface(IEnumFORMATETC *self, REFIID riid,
				  void **ppv)
{
	if (!ppv)
		return E_POINTER;
	if (!guid_eq(riid, &IID_IUnknown) &&
	    !guid_eq(riid, &IID_IEnumFORMATETC)) {
		*ppv = NULL;
		return E_NOINTERFACE;
	}
	*ppv = self;
	__atomic_add_fetch(&((struct enf *)self)->refs, 1, __ATOMIC_SEQ_CST);
	return S_OK;
}

WINFN(ULONG) enf_AddRef(IEnumFORMATETC *self)
{
	return __atomic_add_fetch(&((struct enf *)self)->refs, 1,
				  __ATOMIC_SEQ_CST);
}

WINFN(ULONG) enf_Release(IEnumFORMATETC *self)
{
	struct enf *e = (struct enf *)self;
	ULONG r = __atomic_sub_fetch(&e->refs, 1, __ATOMIC_SEQ_CST);
	if (!r) {
		for (DWORD i = 0; i < e->n; i++)
			ole_taskmem_free(e->v[i].ptd);
		free(e->v);
		free(e);
	}
	return r;
}

WINFN(HRESULT) enf_Next(IEnumFORMATETC *self, ULONG count, FORMATETC *out,
			ULONG *fetched)
{
	struct enf *e = (struct enf *)self;
	ULONG n = 0;

	if (!out || (count > 1 && !fetched))
		return E_POINTER;
	while (n < count && e->pos < e->n) {
		if (!fmt_copy(&out[n], &e->v[e->pos]))
			break;
		e->pos++;
		n++;
	}
	if (fetched)
		*fetched = n;
	return n == count ? S_OK : S_FALSE;
}

WINFN(HRESULT) enf_Skip(IEnumFORMATETC *self, ULONG count)
{
	struct enf *e = (struct enf *)self;
	DWORD left = e->n - e->pos;
	e->pos += count < left ? count : left;
	return count <= left ? S_OK : S_FALSE;
}

WINFN(HRESULT) enf_Reset(IEnumFORMATETC *self)
{
	((struct enf *)self)->pos = 0;
	return S_OK;
}

WINFN(HRESULT) enf_Clone(IEnumFORMATETC *self, IEnumFORMATETC **out)
{
	struct enf *e = (struct enf *)self;
	if (!out)
		return E_POINTER;
	HRESULT hr = enf_new(e->v, e->n, out);
	if (SUCCEEDED(hr))
		((struct enf *)*out)->pos = e->pos;
	return hr;
}

static const IEnumFORMATETCVtbl enf_vtbl = {
	enf_QueryInterface, enf_AddRef, enf_Release, enf_Next, enf_Skip,
	enf_Reset, enf_Clone,
};

/* ---- the OLE clipboard ---- */

static pthread_mutex_t clip_lock = PTHREAD_RECURSIVE_MUTEX_INITIALIZER_NP;
static IDataObject *clip_source;	/* what OleSetClipboard was given */
static DWORD clip_seq;			/* the clipboard's, when it was set */
static int clip_pending;		/* a format not yet rendered */
static const IDataObjectVtbl cd_vtbl;

/* The source, if the clipboard still holds what it put there. Once
 * something else has taken the clipboard, the source is let go. */
static IDataObject *current_source(void)
{
	DWORD (WINAPI *seq)(void) = u32("GetClipboardSequenceNumber");
	IDataObject *d = NULL, *lost = NULL;

	pthread_mutex_lock(&clip_lock);
	if (clip_source && seq && seq() == clip_seq) {
		d = clip_source;
		d->lpVtbl->AddRef(d);
	} else if (clip_source) {
		lost = clip_source;
		clip_source = NULL;
		clip_pending = 0;
	}
	pthread_mutex_unlock(&clip_lock);
	if (lost)
		lost->lpVtbl->Release(lost);
	return d;
}

/* Clipboard formats whose handle is a GDI object, not global memory. */
static int gdi_format(UINT f)
{
	/* CF_BITMAP, CF_METAFILEPICT, CF_PALETTE, CF_ENHMETAFILE and their
	 * CF_DSP* forms */
	return f == 2 || f == 3 || f == 9 || f == 14 || f == 0x82 ||
	       f == 0x83 || f == 0x8e;
}

static HRESULT clip_open(void)
{
	BOOL (WINAPI *open)(HWND) = u32("OpenClipboard");
	return open && open(NULL) ? S_OK : CLIPBRD_E_CANT_OPEN;
}

static void clip_close(void)
{
	BOOL (WINAPI *close)(void) = u32("CloseClipboard");
	if (close)
		close();
}

/* Puts the global memory formats of data on the user32 clipboard;
 * FALSE if one of them could not be put there. */
static int render(IDataObject *data)
{
	HANDLE (WINAPI *set)(UINT, HANDLE) = u32("SetClipboardData");
	IEnumFORMATETC *e;
	FORMATETC f;
	int all = 1;

	if (!set || FAILED(data->lpVtbl->EnumFormatEtc(data, DATADIR_GET, &e)))
		return 0;
	while (e->lpVtbl->Next(e, 1, &f, NULL) == S_OK) {
		STGMEDIUM med;
		memset(&med, 0, sizeof(med));
		if (!f.ptd && (f.tymed & TYMED_HGLOBAL) &&
		    (f.dwAspect == DVASPECT_CONTENT || !f.dwAspect)) {
			FORMATETC want = f;
			want.tymed = TYMED_HGLOBAL;
			if (SUCCEEDED(data->lpVtbl->GetData(data, &want, &med))) {
				HGLOBAL copy = med.tymed == TYMED_HGLOBAL ?
					global_copy(med.hGlobal, 0) : NULL;
				if (!copy && med.tymed == TYMED_HGLOBAL)
					all = 0;
				if (copy && !set(f.cfFormat, copy)) {
					global_free(copy);
					all = 0;
				}
				ole_ReleaseStgMedium(&med);
			} else {
				all = 0;
			}
		}
		ole_taskmem_free(f.ptd);
	}
	e->lpVtbl->Release(e);
	return all;
}

WINFN(HRESULT) ole_OleSetClipboard(IDataObject *data)
{
	BOOL (WINAPI *empty)(void) = u32("EmptyClipboard");
	DWORD (WINAPI *seq)(void) = u32("GetClipboardSequenceNumber");
	HRESULT hr;

	if (data && data->lpVtbl == &cd_vtbl) {
		/* OleGetClipboard's own object: what it shows is already
		 * there. Setting it would make it its own source. */
		IDataObject *src = current_source();
		if (!src)
			return S_OK;
		data = src;	/* the source behind it, set again */
		hr = ole_OleSetClipboard(data);
		src->lpVtbl->Release(src);
		return hr;
	}
	if (FAILED(hr = clip_open()))
		return hr;
	if (!empty || !empty()) {
		clip_close();
		return CLIPBRD_E_CANT_EMPTY;
	}
	pthread_mutex_lock(&clip_lock);
	IDataObject *old = clip_source;
	clip_source = data;
	clip_pending = 0;
	if (data) {
		data->lpVtbl->AddRef(data);
		clip_pending = !render(data);
	}
	clip_seq = seq ? seq() : 0;
	pthread_mutex_unlock(&clip_lock);
	clip_close();
	if (old)
		old->lpVtbl->Release(old);
	return S_OK;
}

WINFN(HRESULT) ole_OleIsCurrentClipboard(IDataObject *data)
{
	IDataObject *cur = current_source();
	if (cur)
		cur->lpVtbl->Release(cur);
	return data && cur == data ? S_OK : S_FALSE;
}

WINFN(HRESULT) ole_OleFlushClipboard(void)
{
	DWORD (WINAPI *seq)(void) = u32("GetClipboardSequenceNumber");
	IDataObject *src = current_source();
	HRESULT hr = S_OK;

	if (!src)
		return S_OK;
	/* Formats were rendered when set; one that failed then is tried
	 * again, and while one is missing the source is kept. */
	pthread_mutex_lock(&clip_lock);
	if (clip_pending && clip_source == src) {
		if (FAILED(hr = clip_open())) {
			pthread_mutex_unlock(&clip_lock);
			src->lpVtbl->Release(src);
			return hr;
		}
		clip_pending = !render(src);
		clip_seq = seq ? seq() : 0;
		clip_close();
		if (clip_pending)
			hr = CLIPBRD_E_CANT_SET;
	}
	IDataObject *old = NULL;
	if (SUCCEEDED(hr) && clip_source == src) {
		old = clip_source;
		clip_source = NULL;
	}
	pthread_mutex_unlock(&clip_lock);
	src->lpVtbl->Release(src);
	if (old)
		old->lpVtbl->Release(old);
	return hr;
}

/*
 * OleGetClipboard's data object: this process's source while it is on the
 * clipboard, else the user32 clipboard's global memory formats.
 */
struct clipobj {
	IDataObject iface;
	LONG refs;
};

WINFN(HRESULT) cd_QueryInterface(IDataObject *self, REFIID riid, void **ppv)
{
	if (!ppv)
		return E_POINTER;
	if (!guid_eq(riid, &IID_IUnknown) && !guid_eq(riid, &IID_IDataObject)) {
		*ppv = NULL;
		return E_NOINTERFACE;
	}
	*ppv = self;
	__atomic_add_fetch(&((struct clipobj *)self)->refs, 1,
			   __ATOMIC_SEQ_CST);
	return S_OK;
}

WINFN(ULONG) cd_AddRef(IDataObject *self)
{
	return __atomic_add_fetch(&((struct clipobj *)self)->refs, 1,
				  __ATOMIC_SEQ_CST);
}

WINFN(ULONG) cd_Release(IDataObject *self)
{
	struct clipobj *c = (struct clipobj *)self;
	ULONG r = __atomic_sub_fetch(&c->refs, 1, __ATOMIC_SEQ_CST);
	if (!r)
		free(c);
	return r;
}

WINFN(HRESULT) cd_GetData(IDataObject *self, FORMATETC *fmt, STGMEDIUM *med)
{
	HRESULT hr;

	if (!fmt || !med)
		return E_INVALIDARG;
	memset(med, 0, sizeof(*med));
	IDataObject *src = current_source();
	if (src) {
		hr = src->lpVtbl->GetData(src, fmt, med);
		src->lpVtbl->Release(src);
		return hr;
	}
	if (fmt->dwAspect && fmt->dwAspect != DVASPECT_CONTENT)
		return DV_E_FORMATETC;
	if (gdi_format(fmt->cfFormat))
		return DV_E_FORMATETC;	/* not global memory */
	if (!(fmt->tymed & TYMED_HGLOBAL))
		return DV_E_TYMED;
	HANDLE (WINAPI *get)(UINT) = u32("GetClipboardData");
	if (FAILED(hr = clip_open()))
		return hr;
	HANDLE h = get ? get(fmt->cfFormat) : NULL;
	HGLOBAL copy = h ? global_copy(h, 0) : NULL;
	clip_close();
	if (!h)
		return DV_E_FORMATETC;
	if (!copy)
		return E_OUTOFMEMORY;
	med->tymed = TYMED_HGLOBAL;
	med->hGlobal = copy;
	return S_OK;
}

WINFN(HRESULT) cd_GetDataHere(IDataObject *self, FORMATETC *fmt,
			      STGMEDIUM *med)
{
	IDataObject *src = current_source();
	HRESULT hr = E_NOTIMPL;
	if (src) {
		hr = src->lpVtbl->GetDataHere(src, fmt, med);
		src->lpVtbl->Release(src);
	}
	return hr;
}

WINFN(HRESULT) cd_QueryGetData(IDataObject *self, FORMATETC *fmt)
{
	HRESULT hr;

	if (!fmt)
		return E_INVALIDARG;
	IDataObject *src = current_source();
	if (src) {
		hr = src->lpVtbl->QueryGetData(src, fmt);
		src->lpVtbl->Release(src);
		return hr;
	}
	if (fmt->dwAspect && fmt->dwAspect != DVASPECT_CONTENT)
		return DV_E_FORMATETC;
	if (gdi_format(fmt->cfFormat))
		return DV_E_FORMATETC;
	if (!(fmt->tymed & TYMED_HGLOBAL))
		return DV_E_TYMED;
	BOOL (WINAPI *avail)(UINT) = u32("IsClipboardFormatAvailable");
	return avail && avail(fmt->cfFormat) ? S_OK : DV_E_FORMATETC;
}

WINFN(HRESULT) cd_GetCanonicalFormatEtc(IDataObject *self, FORMATETC *in,
					FORMATETC *out)
{
	if (!in || !out)
		return E_INVALIDARG;
	*out = *in;
	out->ptd = NULL;
	return DATA_S_SAMEFORMATETC;
}

WINFN(HRESULT) cd_SetData(IDataObject *self, FORMATETC *fmt, STGMEDIUM *med,
			  BOOL release)
{
	return E_NOTIMPL;
}

WINFN(HRESULT) cd_EnumFormatEtc(IDataObject *self, DWORD dir,
				IEnumFORMATETC **out)
{
	HRESULT hr;

	if (!out)
		return E_INVALIDARG;
	*out = NULL;
	if (dir != DATADIR_GET)
		return E_NOTIMPL;
	IDataObject *src = current_source();
	if (src) {
		hr = src->lpVtbl->EnumFormatEtc(src, dir, out);
		src->lpVtbl->Release(src);
		return hr;
	}
	UINT (WINAPI *next)(UINT) = u32("EnumClipboardFormats");
	FORMATETC *v = NULL;
	DWORD n = 0;
	if (FAILED(hr = clip_open()))
		return hr;
	for (UINT f = next ? next(0) : 0; f; f = next(f)) {
		if (gdi_format(f))
			continue;	/* only global memory is offered */
		FORMATETC *nv = realloc(v, (n + 1) * sizeof(*v));
		if (!nv)
			break;
		v = nv;
		v[n].cfFormat = (CLIPFORMAT)f;
		v[n].ptd = NULL;
		v[n].dwAspect = DVASPECT_CONTENT;
		v[n].lindex = -1;
		v[n].tymed = TYMED_HGLOBAL;
		n++;
	}
	clip_close();
	hr = enf_new(v, n, out);
	free(v);
	return hr;
}

WINFN(HRESULT) cd_DAdvise(IDataObject *self, FORMATETC *fmt, DWORD advf,
			  IAdviseSink *sink, DWORD *conn)
{
	return OLE_E_ADVISENOTSUPPORTED;
}

WINFN(HRESULT) cd_DUnadvise(IDataObject *self, DWORD conn)
{
	return OLE_E_ADVISENOTSUPPORTED;
}

WINFN(HRESULT) cd_EnumDAdvise(IDataObject *self, IEnumSTATDATA **e)
{
	return OLE_E_ADVISENOTSUPPORTED;
}

static const IDataObjectVtbl cd_vtbl = {
	cd_QueryInterface, cd_AddRef, cd_Release, cd_GetData, cd_GetDataHere,
	cd_QueryGetData, cd_GetCanonicalFormatEtc, cd_SetData,
	cd_EnumFormatEtc, cd_DAdvise, cd_DUnadvise, cd_EnumDAdvise,
};

WINFN(HRESULT) ole_OleGetClipboard(IDataObject **out)
{
	if (!out)
		return E_INVALIDARG;
	struct clipobj *c = calloc(1, sizeof(*c));
	if (!c) {
		*out = NULL;
		return E_OUTOFMEMORY;
	}
	c->iface.lpVtbl = &cd_vtbl;
	c->refs = 1;
	*out = &c->iface;
	return S_OK;
}

/* ---- running and locking objects ---- */

static IRunnableObject *runnable(IUnknown *u)
{
	IRunnableObject *r = NULL;
	if (u && FAILED(u->lpVtbl->QueryInterface(u, &IID_IRunnableObject,
						  (void **)&r)))
		r = NULL;
	return r;
}

WINFN(HRESULT) ole_OleRun(IUnknown *u)
{
	IRunnableObject *r = runnable(u);
	HRESULT hr = S_OK;
	if (r) {
		hr = r->lpVtbl->Run(r, NULL);
		r->lpVtbl->Release(r);
	}
	return hr;
}

WINFN(BOOL) ole_OleIsRunning(IUnknown *u)
{
	IRunnableObject *r = runnable(u);
	BOOL yes = TRUE;
	if (!u)
		return FALSE;
	if (r) {
		yes = r->lpVtbl->IsRunning(r);
		r->lpVtbl->Release(r);
	}
	return yes;
}

WINFN(HRESULT) ole_OleLockRunning(IUnknown *u, BOOL lock, BOOL last_closes)
{
	IRunnableObject *r = runnable(u);
	HRESULT hr = S_OK;
	if (!u)
		return E_INVALIDARG;
	if (r) {
		hr = r->lpVtbl->LockRunning(r, lock, last_closes);
		r->lpVtbl->Release(r);
	}
	return hr;
}

WINFN(HRESULT) ole_OleSetContainedObject(IUnknown *u, BOOL contained)
{
	IRunnableObject *r = runnable(u);
	HRESULT hr = S_OK;
	if (!u)
		return E_INVALIDARG;
	if (r) {
		hr = r->lpVtbl->SetContainedObject(r, contained);
		r->lpVtbl->Release(r);
	}
	return hr;
}

/* CoLockObjectExternal's strong locks: a reference and a connection. */
struct lock_entry {
	IUnknown *obj;
	DWORD count;
};

static pthread_mutex_t lock_lock = PTHREAD_RECURSIVE_MUTEX_INITIALIZER_NP;
static struct lock_entry *locks;
static size_t nlocks;

static void connection(IUnknown *u, int add, BOOL last_closes)
{
	IExternalConnection *c;
	if (FAILED(u->lpVtbl->QueryInterface(u, &IID_IExternalConnection,
					     (void **)&c)))
		return;
	if (add)
		c->lpVtbl->AddConnection(c, EXTCONN_STRONG, 0);
	else
		c->lpVtbl->ReleaseConnection(c, EXTCONN_STRONG, 0, last_closes);
	c->lpVtbl->Release(c);
}

WINFN(HRESULT) ole_CoLockObjectExternal(IUnknown *u, BOOL lock,
					BOOL last_unlock_releases)
{
	size_t i;

	if (!u)
		return E_INVALIDARG;
	pthread_mutex_lock(&lock_lock);
	for (i = 0; i < nlocks && locks[i].obj != u; i++)
		;
	if (lock) {
		if (i == nlocks) {
			struct lock_entry *v =
				realloc(locks, (nlocks + 1) * sizeof(*v));
			if (!v) {
				pthread_mutex_unlock(&lock_lock);
				return E_OUTOFMEMORY;
			}
			locks = v;
			locks[nlocks].obj = u;
			locks[nlocks].count = 0;
			nlocks++;
		}
		locks[i].count++;
		pthread_mutex_unlock(&lock_lock);
		u->lpVtbl->AddRef(u);
		connection(u, 1, FALSE);
		return S_OK;
	}
	if (i == nlocks) {
		pthread_mutex_unlock(&lock_lock);
		return S_OK;	/* nothing locked: nothing to do */
	}
	int last = !--locks[i].count;
	if (last)
		locks[i] = locks[--nlocks];
	/* Released with the table locked (the lock is recursive, so the
	 * object may lock or unlock again from ReleaseConnection), so no
	 * other thread can lock it between deciding "last" and saying so. */
	connection(u, 0, last && last_unlock_releases);
	pthread_mutex_unlock(&lock_lock);
	u->lpVtbl->Release(u);
	return S_OK;
}

WINFN(HRESULT) ole_CoDisconnectObject(IUnknown *u, DWORD reserved)
{
	/* No stubs or proxies exist in one process: nothing to cut off. */
	return u ? S_OK : E_INVALIDARG;
}

/* ---- message filters and security ---- */

/* Each thread (each STA) has its own filter, as on Windows. */
static __thread IMessageFilter *message_filter;
static int security_done;

void ole_message_filter_drop(void)
{
	IMessageFilter *f = message_filter;
	message_filter = NULL;
	if (f)
		f->lpVtbl->Release(f);
}

WINFN(HRESULT) ole_CoRegisterMessageFilter(IMessageFilter *filter,
					   IMessageFilter **old)
{
	if (filter)
		filter->lpVtbl->AddRef(filter);
	IMessageFilter *prev = message_filter;
	message_filter = filter;
	if (old)
		*old = prev;
	else if (prev)
		prev->lpVtbl->Release(prev);
	return S_OK;
}

WINFN(HRESULT) ole_CoInitializeSecurity(void *sd, LONG nauth, void *auth,
					void *reserved1, DWORD authn_level,
					DWORD imp_level, void *auth_list,
					DWORD caps, void *reserved3)
{
	return __atomic_exchange_n(&security_done, 1, __ATOMIC_SEQ_CST) ?
		       RPC_E_TOO_LATE : S_OK;
}

/* Objects here are never proxies, which is all a blanket applies to. */
WINFN(HRESULT) ole_CoSetProxyBlanket(IUnknown *proxy, DWORD authn,
				     DWORD authz, OLECHAR *principal,
				     DWORD level, DWORD imp, void *auth,
				     DWORD caps)
{
	return proxy ? E_NOINTERFACE : E_INVALIDARG;
}

WINFN(HRESULT) ole_CoQueryProxyBlanket(IUnknown *proxy, DWORD *authn,
				       DWORD *authz, OLECHAR **principal,
				       DWORD *level, DWORD *imp, void **auth,
				       DWORD *caps)
{
	return proxy ? E_NOINTERFACE : E_INVALIDARG;
}

WINFN(HRESULT) ole_CoCopyProxy(IUnknown *proxy, IUnknown **copy)
{
	if (copy)
		*copy = NULL;
	return proxy ? E_NOINTERFACE : E_INVALIDARG;
}

/* No call from another apartment is ever in progress. */
WINFN(HRESULT) ole_CoGetCallContext(REFIID riid, void **ctx)
{
	if (ctx)
		*ctx = NULL;
	return RPC_E_CALL_COMPLETE;
}

WINFN(HRESULT) ole_CoImpersonateClient(void)
{
	return RPC_E_CALL_COMPLETE;
}

WINFN(HRESULT) ole_CoRevertToSelf(void)
{
	return RPC_E_CALL_COMPLETE;
}

/* ---- drag and drop ---- */

struct drop_entry {
	HWND hwnd;
	IDropTarget *target;
};

static pthread_mutex_t drop_lock = PTHREAD_MUTEX_INITIALIZER;
static struct drop_entry *drops;
static size_t ndrops;

static int is_window(HWND h)
{
	BOOL (WINAPI *f)(HWND) = u32("IsWindow");
	return h && f && f(h);
}

WINFN(HRESULT) ole_RegisterDragDrop(HWND hwnd, IDropTarget *target)
{
	if (!target)
		return E_INVALIDARG;
	if (!is_window(hwnd))
		return DRAGDROP_E_INVALIDHWND;
	pthread_mutex_lock(&drop_lock);
	for (size_t i = 0; i < ndrops; i++)
		if (drops[i].hwnd == hwnd) {
			pthread_mutex_unlock(&drop_lock);
			return DRAGDROP_E_ALREADYREGISTERED;
		}
	struct drop_entry *v = realloc(drops, (ndrops + 1) * sizeof(*v));
	if (!v) {
		pthread_mutex_unlock(&drop_lock);
		return E_OUTOFMEMORY;
	}
	drops = v;
	drops[ndrops].hwnd = hwnd;
	drops[ndrops].target = target;
	ndrops++;
	pthread_mutex_unlock(&drop_lock);
	target->lpVtbl->AddRef(target);
	return S_OK;
}

WINFN(HRESULT) ole_RevokeDragDrop(HWND hwnd)
{
	IDropTarget *t = NULL;

	if (!is_window(hwnd))
		return DRAGDROP_E_INVALIDHWND;
	pthread_mutex_lock(&drop_lock);
	for (size_t i = 0; i < ndrops; i++)
		if (drops[i].hwnd == hwnd) {
			t = drops[i].target;
			drops[i] = drops[--ndrops];
			break;
		}
	pthread_mutex_unlock(&drop_lock);
	if (!t)
		return DRAGDROP_E_NOTREGISTERED;
	t->lpVtbl->Release(t);
	return S_OK;
}

/*
 * No mouse button is ever down, so the source is told so at once; it
 * normally answers with a drop, but with no target under the cursor
 * nothing receives it, which Windows reports as cancelled.
 */
WINFN(HRESULT) ole_DoDragDrop(IDataObject *data, IDropSource *source,
			      DWORD ok_effects, DWORD *effect)
{
	if (!data || !source || !effect)
		return E_INVALIDARG;
	*effect = DROPEFFECT_NONE;
	source->lpVtbl->GiveFeedback(source, DROPEFFECT_NONE);
	source->lpVtbl->QueryContinueDrag(source, FALSE, 0);
	return DRAGDROP_S_CANCEL;
}

/* ---- the registry's OLE keys ---- */

static const char *const class_roots[] = {
	"HKEY_CURRENT_USER\\Software\\Classes",
	"HKEY_LOCAL_MACHINE\\Software\\Classes",
};

/* Whether HKCR\sub exists; with reg_lock() not held. */
static int class_key_exists(const char *sub)
{
	int yes = 0;
	reg_lock();
	for (size_t i = 0; i < 2 && !yes; i++) {
		char path[512];
		snprintf(path, sizeof(path), "%s\\%s", class_roots[i], sub);
		yes = reg_open(path) != NULL;
	}
	reg_unlock();
	return yes;
}

static void clsid_key(REFCLSID clsid, char *out, size_t size)
{
	char g[OLE_GUID_CHARS];
	ole_guid_format(clsid, g);
	snprintf(out, size, "CLSID\\%s", g);
}

static LPOLESTR task_from_utf8(const char *s)
{
	WCHAR *w = utf8_to_utf16_dup(s);
	LPOLESTR t = NULL;
	if (w) {
		size_t n = wstrlen(w);
		if ((t = ole_taskmem_alloc((n + 1) * sizeof(WCHAR))))
			memcpy(t, w, (n + 1) * sizeof(WCHAR));
		free(w);
	}
	return t;
}

WINFN(HRESULT) ole_OleRegGetUserType(REFCLSID clsid, DWORD form,
				     LPOLESTR *out)
{
	char key[128], sub[160];
	char *v = NULL;

	if (!out)
		return E_INVALIDARG;
	*out = NULL;
	if (!clsid)
		return E_INVALIDARG;
	clsid_key(clsid, key, sizeof(key));
	if (!class_key_exists(key))
		return REGDB_E_CLASSNOTREG;
	if (form == USERCLASSTYPE_SHORT || form == USERCLASSTYPE_APPNAME) {
		snprintf(sub, sizeof(sub), "%s\\AuxUserType\\%lu", key,
			 (unsigned long)form);
		v = ole_classes_value(sub, "");
	}
	if (!v || !*v) {
		free(v);
		v = ole_classes_value(key, "");
	}
	if (!v || !*v) {
		free(v);
		return REGDB_E_READREGDB;
	}
	*out = task_from_utf8(v);
	free(v);
	return *out ? S_OK : E_OUTOFMEMORY;
}

WINFN(HRESULT) ole_OleRegGetMiscStatus(REFCLSID clsid, DWORD aspect,
				       DWORD *status)
{
	char key[128], sub[180];

	if (!status)
		return E_INVALIDARG;
	*status = 0;
	if (!clsid)
		return E_INVALIDARG;
	clsid_key(clsid, key, sizeof(key));
	if (!class_key_exists(key))
		return REGDB_E_CLASSNOTREG;
	snprintf(sub, sizeof(sub), "%s\\MiscStatus\\%lu", key,
		 (unsigned long)aspect);
	char *v = ole_classes_value(sub, "");
	if (!v) {
		snprintf(sub, sizeof(sub), "%s\\MiscStatus", key);
		v = ole_classes_value(sub, "");
	}
	if (v) {
		*status = (DWORD)strtoul(v, NULL, 10);
		free(v);
	}
	return S_OK;
}

/* ---- IEnumOLEVERB ---- */

struct env {
	IEnumOLEVERB iface;
	LONG refs;
	OLEVERB *v;	/* names malloc()ed */
	DWORD n, pos;
};

static const IEnumOLEVERBVtbl env_vtbl;

static void env_free(struct env *e)
{
	for (DWORD i = 0; i < e->n; i++)
		free(e->v[i].lpszVerbName);
	free(e->v);
	free(e);
}

WINFN(HRESULT) env_QueryInterface(IEnumOLEVERB *self, REFIID riid, void **ppv)
{
	if (!ppv)
		return E_POINTER;
	if (!guid_eq(riid, &IID_IUnknown) && !guid_eq(riid, &IID_IEnumOLEVERB)) {
		*ppv = NULL;
		return E_NOINTERFACE;
	}
	*ppv = self;
	__atomic_add_fetch(&((struct env *)self)->refs, 1, __ATOMIC_SEQ_CST);
	return S_OK;
}

WINFN(ULONG) env_AddRef(IEnumOLEVERB *self)
{
	return __atomic_add_fetch(&((struct env *)self)->refs, 1,
				  __ATOMIC_SEQ_CST);
}

WINFN(ULONG) env_Release(IEnumOLEVERB *self)
{
	struct env *e = (struct env *)self;
	ULONG r = __atomic_sub_fetch(&e->refs, 1, __ATOMIC_SEQ_CST);
	if (!r)
		env_free(e);
	return r;
}

/* The caller frees each verb's name with CoTaskMemFree. */
WINFN(HRESULT) env_Next(IEnumOLEVERB *self, ULONG count, OLEVERB *out,
			ULONG *fetched)
{
	struct env *e = (struct env *)self;
	ULONG n = 0;

	if (!out || (count > 1 && !fetched))
		return E_POINTER;
	while (n < count && e->pos < e->n) {
		out[n] = e->v[e->pos];
		size_t len = wstrlen(e->v[e->pos].lpszVerbName);
		out[n].lpszVerbName = ole_taskmem_alloc((len + 1) * sizeof(WCHAR));
		if (!out[n].lpszVerbName)
			break;
		memcpy(out[n].lpszVerbName, e->v[e->pos].lpszVerbName,
		       (len + 1) * sizeof(WCHAR));
		e->pos++;
		n++;
	}
	if (fetched)
		*fetched = n;
	return n == count ? S_OK : S_FALSE;
}

WINFN(HRESULT) env_Skip(IEnumOLEVERB *self, ULONG count)
{
	struct env *e = (struct env *)self;
	DWORD left = e->n - e->pos;
	e->pos += count < left ? count : left;
	return count <= left ? S_OK : S_FALSE;
}

WINFN(HRESULT) env_Reset(IEnumOLEVERB *self)
{
	((struct env *)self)->pos = 0;
	return S_OK;
}

static HRESULT env_new(const OLEVERB *v, DWORD n, IEnumOLEVERB **out);

WINFN(HRESULT) env_Clone(IEnumOLEVERB *self, IEnumOLEVERB **out)
{
	struct env *e = (struct env *)self;
	if (!out)
		return E_POINTER;
	HRESULT hr = env_new(e->v, e->n, out);
	if (SUCCEEDED(hr))
		((struct env *)*out)->pos = e->pos;
	return hr;
}

static const IEnumOLEVERBVtbl env_vtbl = {
	env_QueryInterface, env_AddRef, env_Release, env_Next, env_Skip,
	env_Reset, env_Clone,
};

static HRESULT env_new(const OLEVERB *v, DWORD n, IEnumOLEVERB **out)
{
	struct env *e = calloc(1, sizeof(*e));
	if (!e || (n && !(e->v = calloc(n, sizeof(*e->v))))) {
		free(e);
		return E_OUTOFMEMORY;
	}
	e->iface.lpVtbl = &env_vtbl;
	e->refs = 1;
	for (DWORD i = 0; i < n; i++, e->n++) {
		size_t len = wstrlen(v[i].lpszVerbName);
		e->v[i] = v[i];
		if (!(e->v[i].lpszVerbName = malloc((len + 1) * sizeof(WCHAR)))) {
			env_free(e);
			return E_OUTOFMEMORY;
		}
		memcpy(e->v[i].lpszVerbName, v[i].lpszVerbName,
		       (len + 1) * sizeof(WCHAR));
	}
	*out = &e->iface;
	return S_OK;
}

static int verb_cmp(const void *a, const void *b)
{
	LONG x = ((const OLEVERB *)a)->lVerb, y = ((const OLEVERB *)b)->lVerb;
	return x < y ? -1 : x > y;
}

/*
 * The Verb key's subkeys, named by number, each "Name,MenuFlags,Attribs";
 * the per-user classes' Verb key, if any, wins over the machine's.
 */
WINFN(HRESULT) ole_OleRegEnumVerbs(REFCLSID clsid, IEnumOLEVERB **out)
{
	char key[128];
	OLEVERB *v = NULL;
	DWORD n = 0;
	int found = 0;
	HRESULT hr = S_OK;

	if (!out)
		return E_INVALIDARG;
	*out = NULL;
	if (!clsid)
		return E_INVALIDARG;
	clsid_key(clsid, key, sizeof(key));
	if (!class_key_exists(key))
		return REGDB_E_CLASSNOTREG;
	reg_lock();
	for (size_t r = 0; r < 2 && !found; r++) {
		char path[512];
		snprintf(path, sizeof(path), "%s\\%s\\Verb", class_roots[r],
			 key);
		struct reg_key *k = reg_open(path);
		if (!k)
			continue;
		found = 1;
		for (size_t i = 0; i < k->nsub && SUCCEEDED(hr); i++) {
			struct reg_value *val = reg_get(k->sub[i], "");
			if (!val || val->type != REG_SZ)
				continue;
			size_t wn = val->size / sizeof(WCHAR);
			WCHAR *w = malloc((wn + 1) * sizeof(WCHAR));
			OLEVERB *nv = realloc(v, (n + 1) * sizeof(*v));
			if (!w || !nv) {
				free(w);
				if (nv)
					v = nv;
				hr = E_OUTOFMEMORY;
				break;
			}
			v = nv;
			memcpy(w, val->data, wn * sizeof(WCHAR));
			w[wn] = 0;
			while (wn && !w[wn - 1])
				wn--;
			/* Split "Name,flags,attribs" from the right. */
			DWORD num[2] = { 0, 0 };
			for (int f = 1; f >= 0; f--) {
				size_t c = wn;
				while (c && w[c - 1] != ',')
					c--;
				if (!c)
					break;
				char digits[16];
				size_t d = 0;
				for (size_t j = c; j < wn && d < 15; j++)
					if (w[j] >= '0' && w[j] <= '9')
						digits[d++] = (char)w[j];
				digits[d] = 0;
				num[f] = (DWORD)strtoul(digits, NULL, 10);
				wn = c - 1;
			}
			w[wn] = 0;
			v[n].lVerb = strtol(k->sub[i]->name, NULL, 10);
			v[n].lpszVerbName = w;
			v[n].fuFlags = num[0];
			v[n].grfAttribs = num[1];
			n++;
		}
	}
	reg_unlock();
	if (!found)
		hr = OLEOBJ_E_NOVERBS;
	if (SUCCEEDED(hr)) {
		qsort(v, n, sizeof(*v), verb_cmp);
		hr = env_new(v, n, out);
	}
	for (DWORD i = 0; i < n; i++)
		free(v[i].lpszVerbName);
	free(v);
	return hr;
}

/* TreatAs: the class to use in place of clsid, written under HKLM. */
void ole_treat_as(REFCLSID clsid, CLSID *out)
{
	*out = *clsid;
	for (int depth = 0; depth < 8; depth++) {
		char key[160];
		clsid_key(out, key, sizeof(key));
		strncat(key, "\\TreatAs", sizeof(key) - strlen(key) - 1);
		char *v = ole_classes_value(key, "");
		WCHAR *w = v ? utf8_to_utf16_dup(v) : NULL;
		CLSID next;
		int ok = w && !ole_guid_parse(w, &next);
		free(v);
		free(w);
		if (!ok || guid_eq(&next, out))
			return;
		*out = next;
	}
}

WINFN(HRESULT) ole_CoGetTreatAsClass(REFCLSID old, CLSID *out)
{
	if (!old || !out)
		return E_INVALIDARG;
	ole_treat_as(old, out);
	return guid_eq(old, out) ? S_FALSE : S_OK;
}

WINFN(HRESULT) ole_CoTreatAsClass(REFCLSID old, REFCLSID newc)
{
	static const CLSID null_clsid;
	char key[200], g[OLE_GUID_CHARS];
	LONG err = 0;

	if (!old || !newc)
		return E_INVALIDARG;
	clsid_key(old, key, sizeof(key));
	if (!class_key_exists(key))
		return REGDB_E_CLASSNOTREG;
	char path[300];
	snprintf(path, sizeof(path), "%s\\%s\\TreatAs", class_roots[1], key);
	reg_lock();
	if (guid_eq(newc, &null_clsid)) {
		/* No emulation at all. */
		struct reg_key *k = reg_open(path);
		if (k)
			err = reg_delete_tree(k, 0);
	} else if (guid_eq(newc, old)) {
		/* Back to AutoTreatAs, or to the class itself. */
		char auto_path[300];
		snprintf(auto_path, sizeof(auto_path), "%s\\%s\\AutoTreatAs",
			 class_roots[1], key);
		struct reg_key *a = reg_open(auto_path);
		struct reg_value *av = a ? reg_get(a, "") : NULL;
		struct reg_key *k = reg_open(path);
		if (av && av->type == REG_SZ) {
			k = k ? k : reg_create(path, 0, NULL, &err);
			if (k)
				err = reg_set(k, "", REG_SZ, av->data,
					      av->size);
			else if (!err)
				err = ERROR_CANTWRITE;
		} else if (k) {
			err = reg_delete_tree(k, 0);
		}
	} else {
		struct reg_key *k = reg_create(path, 0, NULL, &err);
		ole_guid_format(newc, g);
		WCHAR w[OLE_GUID_CHARS];
		for (int i = 0; i < OLE_GUID_CHARS; i++)
			w[i] = (BYTE)g[i];
		if (k)
			err = reg_set(k, "", REG_SZ, w, sizeof(w));
		else if (!err)
			err = ERROR_CANTWRITE;
	}
	/* A change that cannot be saved stays in this process's registry
	 * (later saves write it out) but is reported as not written. */
	if (!err)
		err = reg_save();
	reg_unlock();
	return err ? REGDB_E_WRITEREGDB : S_OK;
}

/* ---- menus and accelerators ---- */

struct menu_desc {
	HMENU menu;
	OLEMENUGROUPWIDTHS widths;
};

WINFN(HOLEMENU) ole_OleCreateMenuDescriptor(HMENU combined,
					    LPOLEMENUGROUPWIDTHS widths)
{
	HGLOBAL (WINAPI *alloc)(UINT, SIZE_T) = k32("GlobalAlloc");
	void *(WINAPI *lock)(HGLOBAL) = k32("GlobalLock");
	BOOL (WINAPI *unlock)(HGLOBAL) = k32("GlobalUnlock");

	if (!combined || !widths || !alloc || !lock || !unlock)
		return NULL;
	HGLOBAL h = alloc(GMEM_MOVEABLE, sizeof(struct menu_desc));
	struct menu_desc *d = h ? lock(h) : NULL;
	if (!d) {
		global_free(h);
		return NULL;
	}
	d->menu = combined;
	d->widths = *widths;
	unlock(h);
	return h;
}

WINFN(HRESULT) ole_OleDestroyMenuDescriptor(HOLEMENU h)
{
	global_free(h);
	return S_OK;
}

/* With no desktop, there is no menu loop to hook the frame's menus into. */
WINFN(HRESULT) ole_OleSetMenuDescriptor(HOLEMENU h, HWND frame,
					HWND active, void *frame_iface,
					void *active_iface)
{
	if (!is_window(frame) || (h && !is_window(active)))
		return E_INVALIDARG;
	return S_OK;
}

WINFN(BOOL) ole_IsAccelerator(HACCEL h, int count, LPMSG msg, WORD *cmd)
{
	int (WINAPI *copy)(HACCEL, LPACCEL, int) =
		u32("CopyAcceleratorTableW");

	if (!h || !msg || count <= 0 || !copy)
		return FALSE;
	int n = copy(h, NULL, 0);
	if (n <= 0)
		return FALSE;
	if (n > count)
		n = count;
	ACCEL *e = malloc((size_t)n * sizeof(*e));
	if (!e)
		return FALSE;
	n = copy(h, e, n);
	int c = user_accel_match(e, n, msg);
	free(e);
	if (c < 0)
		return FALSE;
	if (cmd)
		*cmd = (WORD)c;
	return TRUE;
}

/* ---- Co* helpers ---- */

WINFN(HRESULT) ole_CoWaitForMultipleHandles(DWORD flags, DWORD timeout,
					    ULONG count, HANDLE *handles,
					    DWORD *index)
{
	DWORD (WINAPI *wait)(DWORD, const HANDLE *, BOOL, DWORD, BOOL) =
		k32("WaitForMultipleObjectsEx");

	if (!index)
		return E_INVALIDARG;
	*index = 0;
	if (!count || !handles || !wait)
		return E_INVALIDARG;
	DWORD r = wait(count, handles, (flags & COWAIT_WAITALL) != 0, timeout,
		       (flags & COWAIT_ALERTABLE) != 0);
	if (r == 0x102)		/* WAIT_TIMEOUT */
		return RPC_S_CALLPENDING;
	if (r == 0xffffffffu)	/* WAIT_FAILED */
		return E_INVALIDARG;
	*index = r;		/* WAIT_ABANDONED_0 + i, WAIT_IO_COMPLETION */
	if (r < count)
		*index = r;
	else if (r >= 0x80 && r < 0x80 + count)
		*index = r - 0x80;
	return S_OK;
}

WINFN(HINSTANCE) ole_CoLoadLibrary(LPOLESTR name, BOOL autofree)
{
	HMODULE (WINAPI *load)(LPCWSTR, HANDLE, DWORD) = k32("LoadLibraryExW");
	if (!name || !load)
		return NULL;
	return load(name, NULL, 0x00000008 /* LOAD_WITH_ALTERED_SEARCH_PATH */);
}

WINFN(void) ole_CoFreeLibrary(HINSTANCE h)
{
	BOOL (WINAPI *free_lib)(HMODULE) = k32("FreeLibrary");
	if (h && free_lib)
		free_lib(h);
}

/* In-process servers stay loaded until exit, as CoFreeUnusedLibraries. */
WINFN(void) ole_CoFreeAllLibraries(void)
{
}

WINFN(HRESULT) ole_CoCreateInstanceEx(REFCLSID clsid, IUnknown *outer,
				      DWORD ctx, void *server, DWORD count,
				      MULTI_QI *results)
{
	HRESULT (WINAPI *create)(REFCLSID, IUnknown *, DWORD, REFIID, void **) =
		builtin_export(&builtin_ole32, "CoCreateInstance");
	IUnknown *u;
	HRESULT hr;
	DWORD got = 0;

	if (!count || !results)
		return E_INVALIDARG;
	for (DWORD i = 0; i < count; i++) {
		if (!results[i].pIID)
			return E_INVALIDARG;
		results[i].pItf = NULL;
		results[i].hr = E_NOINTERFACE;
	}
	if (!create)
		return E_UNEXPECTED;
	if (FAILED(hr = create(clsid, outer, ctx, &IID_IUnknown, (void **)&u)))
		return hr;
	for (DWORD i = 0; i < count; i++) {
		results[i].hr = u->lpVtbl->QueryInterface(
			u, results[i].pIID, (void **)&results[i].pItf);
		got += SUCCEEDED(results[i].hr);
	}
	u->lpVtbl->Release(u);
	return got == count ? S_OK : got ? CO_S_NOTALLINTERFACES :
					   E_NOINTERFACE;
}

#define E(name) { #name, 0, (void *)ole_##name }

const struct builtin_export ole_misc_exports[] = {
	E(CoCopyProxy),
	E(CoCreateInstanceEx),
	E(CoDisconnectObject),
	E(CoFreeAllLibraries),
	E(CoFreeLibrary),
	E(CoGetCallContext),
	E(CoGetTreatAsClass),
	E(CoImpersonateClient),
	E(CoInitializeSecurity),
	E(CoLoadLibrary),
	E(CoLockObjectExternal),
	E(CoQueryProxyBlanket),
	E(CoRegisterMessageFilter),
	E(CoRevertToSelf),
	E(CoSetProxyBlanket),
	E(CoTreatAsClass),
	E(CoWaitForMultipleHandles),
	E(CreateDataAdviseHolder),
	E(CreateOleAdviseHolder),
	E(DoDragDrop),
	E(IsAccelerator),
	E(OleCreateMenuDescriptor),
	E(OleDestroyMenuDescriptor),
	E(OleDuplicateData),
	E(OleFlushClipboard),
	E(OleGetClipboard),
	E(OleIsCurrentClipboard),
	E(OleIsRunning),
	E(OleLockRunning),
	E(OleRegEnumVerbs),
	E(OleRegGetMiscStatus),
	E(OleRegGetUserType),
	E(OleRun),
	E(OleSetClipboard),
	E(OleSetContainedObject),
	E(OleSetMenuDescriptor),
	E(RegisterDragDrop),
	E(ReleaseStgMedium),
	E(RevokeDragDrop),
	BUILTIN_END
};
