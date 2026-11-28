/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * shell32: the objects a folder hands out for its items (GetUIObjectOf)
 * and for itself (CreateViewObject), and dropped files.
 *
 * - IDataObject: the items as CF_HDROP (when they are files) and "Shell
 *   IDList Array", plus whatever is set on it ("Preferred DropEffect").
 * - IDropTarget on a directory: files dropped on it are copied or moved
 *   into it, as the keys, the source's preferred effect and the drives
 *   decide, as Explorer does.
 * - IContextMenu: open, explore, cut, copy, paste, delete, rename and
 *   properties for items, and paste and properties for a folder's
 *   background. Cut and copy put a data object on the OLE clipboard and
 *   paste takes files from it; delete goes to the Recycle Bin unless Shift
 *   is down. With no GUI, rename and properties show nothing.
 * - IExtractIconA/W: shell_icon.c's icon locations.
 * - IShellView: a "SHELLDLL_DefView" window with a list view of the
 *   folder's items, a selection, and the objects of the selection.
 * - DragQueryFile, DragQueryPoint, DragFinish over an HDROP (DROPFILES
 *   in global memory), and DragAcceptFiles setting WS_EX_ACCEPTFILES.
 */
#include <pthread.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>

#include "win32/codepage.h"
#include "win32/commctrl.h"
#include "win32/internal.h"
#include "win32/k32.h"
#include "win32/path.h"
#include "win32/shell.h"

extern const struct builtin_dll builtin_user32, builtin_ole32,
	builtin_comctl32, builtin_shell32, builtin_kernel32;

#define InterlockedIncrement(p) __atomic_add_fetch((p), 1, __ATOMIC_SEQ_CST)
#define InterlockedDecrement(p) __atomic_sub_fetch((p), 1, __ATOMIC_SEQ_CST)

#define U32(name) builtin_export(&builtin_user32, name)
#define OLE(name) builtin_export(&builtin_ole32, name)
#define SH(name) builtin_export(&builtin_shell32, name)

#define GMEM_MOVEABLE 0x0002
#define GMEM_ZEROINIT 0x0040
#define DATADIR_GET 1
#define DATA_S_SAMEFORMATETC ((HRESULT)0x00040130)
#define GW_CHILD 5
#ifndef E_ABORT
#define E_ABORT ((HRESULT)0x80004004)
#endif
#define DV_E_DVASPECT ((HRESULT)0x8004006B)
#define MAKE_HRESULT(sev, fac, code) \
	((HRESULT)(((unsigned long)(sev) << 31) | \
		   ((unsigned long)(fac) << 16) | (unsigned long)(code)))
#define SEVERITY_SUCCESS 0

static int same_guid(REFIID a, const GUID *b)
{
	return a && !memcmp(a, b, sizeof(GUID));
}

static const char *base_of(const char *path)
{
	const char *b = strrchr(path, '\\');
	return b && b[1] ? b + 1 : path;
}

static UINT clip_format(const char *name)
{
	UINT (WINAPI *reg)(LPCSTR) = U32("RegisterClipboardFormatA");
	return reg(name);
}

static UINT cf_idlist(void) { return clip_format("Shell IDList Array"); }
static UINT cf_effect(void) { return clip_format("Preferred DropEffect"); }

static UINT il_size(LPCITEMIDLIST pidl)
{
	UINT (WINAPI *size)(LPCITEMIDLIST) = SH("ILGetSize");
	return pidl ? size(pidl) : 2;
}

static LPITEMIDLIST il_clone(LPCITEMIDLIST pidl)
{
	LPITEMIDLIST (WINAPI *clone)(LPCITEMIDLIST) = SH("ILClone");
	return clone(pidl);
}

static void il_free(LPITEMIDLIST pidl)
{
	void (WINAPI *f)(LPITEMIDLIST) = SH("ILFree");
	if (pidl)
		f(pidl);
}

/* The items a folder's objects work on. */
struct items {
	UINT n;
	LPITEMIDLIST parent;	/* the folder's own ID list */
	LPITEMIDLIST *child;
	char **path;		/* NULL entries for virtual items */
	char *dir;		/* the folder's directory, or NULL */
};

static void items_free(struct items *it)
{
	for (UINT i = 0; i < it->n; i++) {
		il_free(it->child[i]);
		free(it->path[i]);
	}
	free(it->child);
	free(it->path);
	il_free(it->parent);
	free(it->dir);
	memset(it, 0, sizeof(*it));
}

static int items_init(struct items *it, LPCITEMIDLIST parent, const char *dir,
		      UINT n, LPCITEMIDLIST *children)
{
	memset(it, 0, sizeof(*it));
	it->child = calloc(n ? n : 1, sizeof(*it->child));
	it->path = calloc(n ? n : 1, sizeof(*it->path));
	it->parent = parent ? il_clone(parent) : NULL;
	it->dir = dir ? strdup(dir) : NULL;
	if (!it->child || !it->path || (parent && !it->parent) ||
	    (dir && !it->dir)) {
		items_free(it);
		return -1;
	}
	for (UINT i = 0; i < n; i++) {
		it->n = i + 1;
		if (!(it->child[i] = il_clone(children[i]))) {
			items_free(it);
			return -1;
		}
		it->path[i] = shell_pidl_path(children[i]);
		if (it->path[i])
			for (char *c = it->path[i]; *c; c++)
				if (*c == '/')
					*c = '\\';
	}
	it->n = n;
	return 0;
}

static int all_files(const struct items *it)
{
	for (UINT i = 0; i < it->n; i++)
		if (!it->path[i])
			return 0;
	return it->n > 0;
}

/* HDROP and CIDA blocks. */

/* A wide DROPFILES with the paths, double-NUL-terminated. */
static HGLOBAL make_hdrop(char *const *paths, UINT n)
{
	size_t chars = 1;
	WCHAR **w = calloc(n ? n : 1, sizeof(*w));
	HGLOBAL h = NULL;

	if (!w)
		return NULL;
	for (UINT i = 0; i < n; i++) {
		if (!(w[i] = utf8_to_utf16_dup(paths[i])))
			goto done;
		chars += wstrlen(w[i]) + 1;
	}
	h = global_alloc(GMEM_MOVEABLE | GMEM_ZEROINIT,
			 sizeof(DROPFILES) + chars * sizeof(WCHAR));
	DROPFILES *df = h ? global_lock(h) : NULL;
	if (df) {
		df->pFiles = sizeof(DROPFILES);
		df->fWide = TRUE;
		WCHAR *out = (WCHAR *)(df + 1);
		for (UINT i = 0; i < n; i++) {
			size_t k = wstrlen(w[i]) + 1;
			memcpy(out, w[i], k * sizeof(WCHAR));
			out += k;
		}
		*out = 0;
		global_unlock(h);
	}
done:
	for (UINT i = 0; i < n; i++)
		free(w[i]);
	free(w);
	return h;
}

static HGLOBAL make_cida(const struct items *it)
{
	static const WORD empty = 0;
	size_t head = sizeof(UINT) * (it->n + 2), total = head;

	total += il_size(it->parent);
	for (UINT i = 0; i < it->n; i++)
		total += il_size(it->child[i]);
	HGLOBAL h = global_alloc(GMEM_MOVEABLE | GMEM_ZEROINIT, total);
	BYTE *p = h ? global_lock(h) : NULL;
	if (!p)
		return h;
	CIDA *c = (CIDA *)p;
	size_t at = head;
	c->cidl = it->n;
	for (UINT i = 0; i <= it->n; i++) {
		LPCITEMIDLIST pidl = i ? it->child[i - 1] : it->parent;
		UINT k = il_size(pidl);
		c->aoffset[i] = (UINT)at;
		memcpy(p + at, pidl ? (const void *)pidl : &empty, k);
		at += k;
	}
	global_unlock(h);
	return h;
}

static HGLOBAL dup_global(HGLOBAL h)
{
	SIZE_T n = global_size(h);
	HGLOBAL c = global_alloc(GMEM_MOVEABLE, n ? n : 1);
	void *from = global_lock(h), *to = c ? global_lock(c) : NULL;

	if (from && to)
		memcpy(to, from, n);
	if (to)
		global_unlock(c);
	if (from)
		global_unlock(h);
	if (c && (!from || !to)) {
		global_free(c);
		c = NULL;
	}
	return c;
}

/* The paths in an HDROP, as UTF-8; their count, or -1. */
static int hdrop_paths(HGLOBAL h, char ***out)
{
	DROPFILES *df = global_lock(h);
	SIZE_T size = global_size(h);
	int n = 0;

	*out = NULL;
	if (!df || size < sizeof(*df) || df->pFiles >= size) {
		if (df)
			global_unlock(h);
		return -1;
	}
	const BYTE *p = (const BYTE *)df + df->pFiles;
	const BYTE *end = (const BYTE *)df + size;
	char **list = NULL;
	for (;;) {
		char *s = NULL;
		if (df->fWide) {
			const WCHAR *w = (const WCHAR *)p;
			size_t k = 0;
			while ((const BYTE *)(w + k + 1) <= end && w[k])
				k++;
			if ((const BYTE *)(w + k + 1) > end || !k)
				break;
			WCHAR *copy = malloc((k + 1) * sizeof(WCHAR));
			if (copy) {
				memcpy(copy, w, k * sizeof(WCHAR));
				copy[k] = 0;
				s = utf16_to_utf8_dup(copy);
				free(copy);
			}
			p += (k + 1) * sizeof(WCHAR);
		} else {
			const char *a = (const char *)p;
			size_t k = 0;
			while ((const BYTE *)a + k < end && a[k])
				k++;
			if ((const BYTE *)a + k >= end || !k)
				break;
			char *copy = strndup(a, k);
			s = copy ? acp_to_utf8(copy) : NULL;
			free(copy);
			p += k + 1;
		}
		char **g = s ? realloc(list, (n + 1) * sizeof(*list)) : NULL;
		if (!g) {
			free(s);
			for (int i = 0; i < n; i++)
				free(list[i]);
			free(list);
			global_unlock(h);
			return -1;
		}
		list = g;
		list[n++] = s;
	}
	global_unlock(h);
	*out = list;
	return n;
}

static void paths_free(char **p, int n)
{
	for (int i = 0; i < n; i++)
		free(p[i]);
	free(p);
}

/* An HGLOBAL format of a data object, or NULL. */
static HGLOBAL get_global(IDataObject *d, UINT cf)
{
	void (WINAPI *release)(STGMEDIUM *) = OLE("ReleaseStgMedium");
	FORMATETC fe = { (CLIPFORMAT)cf, NULL, DVASPECT_CONTENT, -1,
			 TYMED_HGLOBAL };
	STGMEDIUM m;
	HGLOBAL r = NULL;

	memset(&m, 0, sizeof(m));
	if (d->lpVtbl->GetData(d, &fe, &m) != S_OK)
		return NULL;
	if (m.tymed == TYMED_HGLOBAL && m.hGlobal)
		r = dup_global(m.hGlobal);
	release(&m);
	return r;
}

static int data_paths(IDataObject *d, char ***out)
{
	HGLOBAL h = get_global(d, CF_HDROP);
	int n = h ? hdrop_paths(h, out) : -1;
	if (h)
		global_free(h);
	return n;
}

static DWORD data_effect(IDataObject *d)
{
	HGLOBAL h = get_global(d, cf_effect());
	DWORD e = 0;
	DWORD *p = h ? global_lock(h) : NULL;

	if (p && global_size(h) >= sizeof(DWORD))
		e = *p;
	if (p)
		global_unlock(h);
	if (h)
		global_free(h);
	return e;
}

/* Copies or moves files into dir with SHFileOperation. */
static HRESULT transfer(char *const *src, int n, const char *dir, int move)
{
	int (WINAPI *op)(SHFILEOPSTRUCTW *) = SH("SHFileOperationW");
	size_t chars = 1;
	int same = 1;

	for (int i = 0; i < n; i++) {
		chars += strlen(src[i]) + 1;
		const char *b = strrchr(src[i], '\\');
		if (!b || (size_t)(b - src[i]) != strlen(dir) ||
		    strncasecmp(src[i], dir, b - src[i]))
			same = 0;
	}
	if (!n)
		return S_FALSE;
	/* Moving a file to where it is does nothing. */
	if (move && same)
		return S_OK;
	char *from = malloc(chars), *to = malloc(strlen(dir) + 2);
	if (!from || !to) {
		free(from);
		free(to);
		return E_OUTOFMEMORY;
	}
	char *p = from;
	for (int i = 0; i < n; i++) {
		strcpy(p, src[i]);
		p += strlen(src[i]) + 1;
	}
	*p = 0;
	strcpy(to, dir);
	to[strlen(dir) + 1] = 0;
	/* Both lists as UTF-16, each double-NUL-terminated. */
	WCHAR *wfrom = malloc(chars * sizeof(WCHAR) * 2);
	WCHAR *wto = utf8_to_utf16_dup(dir);
	HRESULT hr = E_OUTOFMEMORY;
	if (wfrom && wto) {
		WCHAR *q = wfrom;
		for (char *s = from; *s; s += strlen(s) + 1) {
			WCHAR *w = utf8_to_utf16_dup(s);
			if (!w)
				goto out;
			size_t k = wstrlen(w) + 1;
			memcpy(q, w, k * sizeof(WCHAR));
			q += k;
			free(w);
		}
		*q = 0;
		size_t k = wstrlen(wto);
		WCHAR *t = realloc(wto, (k + 2) * sizeof(WCHAR));
		if (!t)
			goto out;
		wto = t;
		wto[k + 1] = 0;
		SHFILEOPSTRUCTW fo;
		memset(&fo, 0, sizeof(fo));
		fo.wFunc = move ? FO_MOVE : FO_COPY;
		fo.pFrom = wfrom;
		fo.pTo = wto;
		/* A copy into its own folder makes "Copy of" names. */
		fo.fFlags = FOF_NOCONFIRMATION | FOF_NOCONFIRMMKDIR |
			    FOF_NOERRORUI | (same ? FOF_RENAMEONCOLLISION : 0);
		int r = op(&fo);
		hr = r ? HRESULT_FROM_WIN32(r) :
		     fo.fAnyOperationsAborted ? E_ABORT : S_OK;
	}
out:
	free(wfrom);
	free(wto);
	free(from);
	free(to);
	return hr;
}

/* IEnumFORMATETC over a copy of a format list. */

struct fenum {
	IEnumFORMATETC iface;
	LONG ref;
	ULONG n, at;
	FORMATETC *f;
};

static const IEnumFORMATETCVtbl fenum_vtbl;

static IEnumFORMATETC *new_fenum(const FORMATETC *f, ULONG n, ULONG at)
{
	struct fenum *e = calloc(1, sizeof(*e));

	if (!e || !(e->f = malloc((n ? n : 1) * sizeof(*f)))) {
		free(e);
		return NULL;
	}
	memcpy(e->f, f, n * sizeof(*f));
	e->iface.lpVtbl = &fenum_vtbl;
	e->ref = 1;
	e->n = n;
	e->at = at;
	return &e->iface;
}

static HRESULT WINAPI fe_QueryInterface(IEnumFORMATETC *This, REFIID riid,
					void **ppv)
{
	if (!ppv)
		return E_POINTER;
	if (same_guid(riid, &IID_IUnknown) ||
	    same_guid(riid, &IID_IEnumFORMATETC)) {
		*ppv = This;
		InterlockedIncrement(&((struct fenum *)This)->ref);
		return S_OK;
	}
	*ppv = NULL;
	return E_NOINTERFACE;
}

static ULONG WINAPI fe_AddRef(IEnumFORMATETC *This)
{
	return (ULONG)InterlockedIncrement(&((struct fenum *)This)->ref);
}

static ULONG WINAPI fe_Release(IEnumFORMATETC *This)
{
	struct fenum *e = (struct fenum *)This;
	LONG r = InterlockedDecrement(&e->ref);
	if (!r) {
		free(e->f);
		free(e);
	}
	return (ULONG)r;
}

static HRESULT WINAPI fe_Next(IEnumFORMATETC *This, ULONG count,
			      FORMATETC *out, ULONG *fetched)
{
	struct fenum *e = (struct fenum *)This;
	ULONG k = 0;

	if (!out || (count > 1 && !fetched))
		return E_INVALIDARG;
	while (k < count && e->at < e->n)
		out[k++] = e->f[e->at++];
	if (fetched)
		*fetched = k;
	return k == count ? S_OK : S_FALSE;
}

static HRESULT WINAPI fe_Skip(IEnumFORMATETC *This, ULONG count)
{
	struct fenum *e = (struct fenum *)This;
	ULONG left = e->n - e->at;
	e->at += count < left ? count : left;
	return count <= left ? S_OK : S_FALSE;
}

static HRESULT WINAPI fe_Reset(IEnumFORMATETC *This)
{
	((struct fenum *)This)->at = 0;
	return S_OK;
}

static HRESULT WINAPI fe_Clone(IEnumFORMATETC *This, IEnumFORMATETC **out)
{
	struct fenum *e = (struct fenum *)This;
	if (!out)
		return E_INVALIDARG;
	*out = new_fenum(e->f, e->n, e->at);
	return *out ? S_OK : E_OUTOFMEMORY;
}

static const IEnumFORMATETCVtbl fenum_vtbl = {
	fe_QueryInterface, fe_AddRef, fe_Release, fe_Next, fe_Skip, fe_Reset,
	fe_Clone,
};

/* IDataObject. */

struct dobj {
	IDataObject iface;
	LONG ref;
	UINT n;
	FORMATETC *fe;
	HGLOBAL *h;
};

static const IDataObjectVtbl dobj_vtbl;

#define DOBJ(This) ((struct dobj *)(This))

/* Takes h; on failure frees it. */
static HRESULT dobj_put(struct dobj *d, UINT cf, HGLOBAL h)
{
	for (UINT i = 0; i < d->n; i++) {
		if (d->fe[i].cfFormat == cf) {
			global_free(d->h[i]);
			d->h[i] = h;
			return S_OK;
		}
	}
	FORMATETC *fe = realloc(d->fe, (d->n + 1) * sizeof(*fe));
	if (fe)
		d->fe = fe;
	HGLOBAL *hh = fe ? realloc(d->h, (d->n + 1) * sizeof(*hh)) : NULL;
	if (!hh) {
		global_free(h);
		return E_OUTOFMEMORY;
	}
	d->h = hh;
	d->fe[d->n] = (FORMATETC){ (CLIPFORMAT)cf, NULL, DVASPECT_CONTENT, -1,
				   TYMED_HGLOBAL };
	d->h[d->n++] = h;
	return S_OK;
}

static HRESULT new_dobj(const struct items *it, IDataObject **out)
{
	struct dobj *d = calloc(1, sizeof(*d));
	HRESULT hr = S_OK;

	*out = NULL;
	if (!d)
		return E_OUTOFMEMORY;
	d->iface.lpVtbl = &dobj_vtbl;
	d->ref = 1;
	if (all_files(it)) {
		HGLOBAL h = make_hdrop(it->path, it->n);
		hr = h ? dobj_put(d, CF_HDROP, h) : E_OUTOFMEMORY;
	}
	if (hr == S_OK) {
		HGLOBAL h = make_cida(it);
		hr = h ? dobj_put(d, cf_idlist(), h) : E_OUTOFMEMORY;
	}
	if (hr != S_OK) {
		d->iface.lpVtbl->Release(&d->iface);
		return hr;
	}
	*out = &d->iface;
	return S_OK;
}

static HRESULT WINAPI do_QueryInterface(IDataObject *This, REFIID riid,
					void **ppv)
{
	if (!ppv)
		return E_POINTER;
	if (same_guid(riid, &IID_IUnknown) ||
	    same_guid(riid, &IID_IDataObject)) {
		*ppv = This;
		InterlockedIncrement(&DOBJ(This)->ref);
		return S_OK;
	}
	*ppv = NULL;
	return E_NOINTERFACE;
}

static ULONG WINAPI do_AddRef(IDataObject *This)
{
	return (ULONG)InterlockedIncrement(&DOBJ(This)->ref);
}

static ULONG WINAPI do_Release(IDataObject *This)
{
	struct dobj *d = DOBJ(This);
	LONG r = InterlockedDecrement(&d->ref);
	if (!r) {
		for (UINT i = 0; i < d->n; i++)
			global_free(d->h[i]);
		free(d->fe);
		free(d->h);
		free(d);
	}
	return (ULONG)r;
}

static int dobj_find(struct dobj *d, const FORMATETC *f, HRESULT *why)
{
	if (!f) {
		*why = E_INVALIDARG;
		return -1;
	}
	for (UINT i = 0; i < d->n; i++) {
		if (d->fe[i].cfFormat != f->cfFormat)
			continue;
		if (!(f->tymed & TYMED_HGLOBAL)) {
			*why = DV_E_TYMED;
			return -1;
		}
		if (f->dwAspect != DVASPECT_CONTENT) {
			*why = DV_E_DVASPECT;
			return -1;
		}
		return (int)i;
	}
	*why = DV_E_FORMATETC;
	return -1;
}

static HRESULT WINAPI do_GetData(IDataObject *This, FORMATETC *f,
				 STGMEDIUM *m)
{
	struct dobj *d = DOBJ(This);
	HRESULT why;
	int i;

	if (!m)
		return E_INVALIDARG;
	if ((i = dobj_find(d, f, &why)) < 0)
		return why;
	HGLOBAL h = dup_global(d->h[i]);
	if (!h)
		return E_OUTOFMEMORY;
	m->tymed = TYMED_HGLOBAL;
	m->hGlobal = h;
	m->pUnkForRelease = NULL;
	return S_OK;
}

static HRESULT WINAPI do_GetDataHere(IDataObject *This, FORMATETC *f,
				     STGMEDIUM *m)
{
	return E_NOTIMPL;
}

static HRESULT WINAPI do_QueryGetData(IDataObject *This, FORMATETC *f)
{
	HRESULT why;
	return dobj_find(DOBJ(This), f, &why) < 0 ? why : S_OK;
}

static HRESULT WINAPI do_GetCanonicalFormatEtc(IDataObject *This,
					       FORMATETC *in, FORMATETC *out)
{
	if (!in || !out)
		return E_INVALIDARG;
	*out = *in;
	out->ptd = NULL;
	return DATA_S_SAMEFORMATETC;
}

static HRESULT WINAPI do_SetData(IDataObject *This, FORMATETC *f,
				 STGMEDIUM *m, BOOL release)
{
	void (WINAPI *rel)(STGMEDIUM *) = OLE("ReleaseStgMedium");

	if (!f || !m)
		return E_INVALIDARG;
	if (!(f->tymed & TYMED_HGLOBAL) || m->tymed != TYMED_HGLOBAL ||
	    !m->hGlobal)
		return DV_E_TYMED;
	HGLOBAL h = dup_global(m->hGlobal);
	if (!h)
		return E_OUTOFMEMORY;
	HRESULT hr = dobj_put(DOBJ(This), f->cfFormat, h);
	if (hr == S_OK && release)
		rel(m);
	return hr;
}

static HRESULT WINAPI do_EnumFormatEtc(IDataObject *This, DWORD dir,
				       IEnumFORMATETC **e)
{
	if (!e)
		return E_INVALIDARG;
	*e = NULL;
	if (dir != DATADIR_GET)
		return E_NOTIMPL;
	*e = new_fenum(DOBJ(This)->fe, DOBJ(This)->n, 0);
	return *e ? S_OK : E_OUTOFMEMORY;
}

static HRESULT WINAPI do_DAdvise(IDataObject *This, FORMATETC *f, DWORD advf,
				 IAdviseSink *sink, DWORD *conn)
{
	return OLE_E_ADVISENOTSUPPORTED;
}

static HRESULT WINAPI do_DUnadvise(IDataObject *This, DWORD conn)
{
	return OLE_E_ADVISENOTSUPPORTED;
}

static HRESULT WINAPI do_EnumDAdvise(IDataObject *This, IEnumSTATDATA **e)
{
	return OLE_E_ADVISENOTSUPPORTED;
}

static const IDataObjectVtbl dobj_vtbl = {
	do_QueryInterface, do_AddRef, do_Release, do_GetData, do_GetDataHere,
	do_QueryGetData, do_GetCanonicalFormatEtc, do_SetData,
	do_EnumFormatEtc, do_DAdvise, do_DUnadvise, do_EnumDAdvise,
};

/* IDropTarget on a directory. */

struct dtarget {
	IDropTarget iface;
	LONG ref;
	char *dir;
	IDataObject *data;	/* between DragEnter and DragLeave or Drop */
};

#define DT(This) ((struct dtarget *)(This))

/*
 * The effect of dropping data with keys down, out of those allowed:
 * Ctrl copies, Shift moves (both, or Alt, would link, which isn't done
 * here); else the source's preference; else a move on the same drive
 * and a copy to another.
 */
static DWORD choose_effect(struct dtarget *t, IDataObject *d, DWORD keys,
			   DWORD allowed)
{
	char **p = NULL;
	int n = d ? data_paths(d, &p) : -1;
	DWORD want;

	if (n <= 0) {
		paths_free(p, n > 0 ? n : 0);
		return DROPEFFECT_NONE;
	}
	if ((keys & MK_CONTROL) && (keys & MK_SHIFT))
		want = DROPEFFECT_LINK;
	else if (keys & MK_CONTROL)
		want = DROPEFFECT_COPY;
	else if (keys & MK_SHIFT)
		want = DROPEFFECT_MOVE;
	else if ((want = data_effect(d) & allowed &
			 (DROPEFFECT_COPY | DROPEFFECT_MOVE))) {
		/* a move if it allows one, as Explorer reads it */
		if (want & DROPEFFECT_MOVE)
			want = DROPEFFECT_MOVE;
	} else
		want = !strncasecmp(p[0], t->dir, 2) ? DROPEFFECT_MOVE :
						       DROPEFFECT_COPY;
	paths_free(p, n);
	if (want == DROPEFFECT_LINK)
		return DROPEFFECT_NONE;
	if (want & allowed)
		return want;
	if (allowed & DROPEFFECT_COPY)
		return DROPEFFECT_COPY;
	return allowed & DROPEFFECT_MOVE;
}

static HRESULT WINAPI dt_QueryInterface(IDropTarget *This, REFIID riid,
					void **ppv)
{
	if (!ppv)
		return E_POINTER;
	if (same_guid(riid, &IID_IUnknown) ||
	    same_guid(riid, &IID_IDropTarget)) {
		*ppv = This;
		InterlockedIncrement(&DT(This)->ref);
		return S_OK;
	}
	*ppv = NULL;
	return E_NOINTERFACE;
}

static ULONG WINAPI dt_AddRef(IDropTarget *This)
{
	return (ULONG)InterlockedIncrement(&DT(This)->ref);
}

static ULONG WINAPI dt_Release(IDropTarget *This)
{
	struct dtarget *t = DT(This);
	LONG r = InterlockedDecrement(&t->ref);
	if (!r) {
		if (t->data)
			t->data->lpVtbl->Release(t->data);
		free(t->dir);
		free(t);
	}
	return (ULONG)r;
}

static HRESULT WINAPI dt_DragEnter(IDropTarget *This, IDataObject *data,
				   DWORD keys, POINTL pt, DWORD *effect)
{
	struct dtarget *t = DT(This);

	if (!effect)
		return E_INVALIDARG;
	if (t->data)
		t->data->lpVtbl->Release(t->data);
	t->data = data;
	if (data)
		data->lpVtbl->AddRef(data);
	*effect = choose_effect(t, data, keys, *effect);
	return S_OK;
}

static HRESULT WINAPI dt_DragOver(IDropTarget *This, DWORD keys, POINTL pt,
				  DWORD *effect)
{
	if (!effect)
		return E_INVALIDARG;
	*effect = choose_effect(DT(This), DT(This)->data, keys, *effect);
	return S_OK;
}

static HRESULT WINAPI dt_DragLeave(IDropTarget *This)
{
	struct dtarget *t = DT(This);
	if (t->data)
		t->data->lpVtbl->Release(t->data);
	t->data = NULL;
	return S_OK;
}

static HRESULT WINAPI dt_Drop(IDropTarget *This, IDataObject *data,
			      DWORD keys, POINTL pt, DWORD *effect)
{
	struct dtarget *t = DT(This);
	char **p = NULL;
	HRESULT hr = S_OK;

	if (!effect)
		return E_INVALIDARG;
	DWORD e = choose_effect(t, data, keys, *effect);
	int n = e && data ? data_paths(data, &p) : 0;
	if (n > 0)
		hr = transfer(p, n, t->dir, e == DROPEFFECT_MOVE);
	paths_free(p, n > 0 ? n : 0);
	*effect = SUCCEEDED(hr) ? e : DROPEFFECT_NONE;
	dt_DragLeave(This);
	return SUCCEEDED(hr) ? S_OK : hr;
}

static const IDropTargetVtbl dtarget_vtbl = {
	dt_QueryInterface, dt_AddRef, dt_Release, dt_DragEnter, dt_DragOver,
	dt_DragLeave, dt_Drop,
};

static HRESULT new_dtarget(const char *dir, void **ppv)
{
	struct dtarget *t = calloc(1, sizeof(*t));

	if (!t || !(t->dir = strdup(dir))) {
		free(t);
		return E_OUTOFMEMORY;
	}
	t->iface.lpVtbl = &dtarget_vtbl;
	t->ref = 1;
	*ppv = &t->iface;
	return S_OK;
}

/* IContextMenu. */

enum { V_OPEN, V_EXPLORE, V_CUT, V_COPY, V_PASTE, V_DELETE, V_RENAME,
       V_PROPERTIES, V_COUNT };

static const struct verb {
	const char *name, *label, *help;
} verbs[V_COUNT] = {
	{ "open", "&Open", "Opens the selected items." },
	{ "explore", "&Explore", "Opens the folder in a browsing view." },
	{ "cut", "Cu&t", "Moves the selected items to the Clipboard." },
	{ "copy", "&Copy", "Copies the selected items to the Clipboard." },
	{ "paste", "&Paste", "Inserts the items you have copied or cut." },
	{ "delete", "&Delete", "Deletes the selected items." },
	{ "rename", "Rena&me", "Renames the selected item." },
	{ "properties", "P&roperties",
	  "Displays the properties of the selected items." },
};

struct cmenu {
	IContextMenu iface;
	LONG ref;
	struct items it;
	int background;		/* the folder's own menu */
};

#define CM(This) ((struct cmenu *)(This))

static int is_folder(const char *win)
{
	char *u = win ? path_to_unix(win) : NULL;
	struct stat st;
	int r = u && !stat(u, &st) && S_ISDIR(st.st_mode);
	free(u);
	return r;
}

/* Whether a verb applies to this menu. */
static int verb_ok(struct cmenu *m, int v)
{
	if (m->background)
		return v == V_PASTE || v == V_PROPERTIES ? m->it.dir != NULL : 0;
	int files = all_files(&m->it);
	switch (v) {
	case V_OPEN:
	case V_PROPERTIES:
		return m->it.n > 0;
	case V_EXPLORE:
		return m->it.n == 1 && is_folder(m->it.path[0]);
	case V_PASTE:
		return m->it.n == 1 && is_folder(m->it.path[0]);
	case V_RENAME:
		return m->it.n == 1 && files;
	default:
		return files;
	}
}

static HRESULT WINAPI cm_QueryInterface(IContextMenu *This, REFIID riid,
					void **ppv)
{
	if (!ppv)
		return E_POINTER;
	if (same_guid(riid, &IID_IUnknown) ||
	    same_guid(riid, &IID_IContextMenu)) {
		*ppv = This;
		InterlockedIncrement(&CM(This)->ref);
		return S_OK;
	}
	*ppv = NULL;
	return E_NOINTERFACE;
}

static ULONG WINAPI cm_AddRef(IContextMenu *This)
{
	return (ULONG)InterlockedIncrement(&CM(This)->ref);
}

static ULONG WINAPI cm_Release(IContextMenu *This)
{
	struct cmenu *m = CM(This);
	LONG r = InterlockedDecrement(&m->ref);
	if (!r) {
		items_free(&m->it);
		free(m);
	}
	return (ULONG)r;
}

/* The items go in at index, with ids first + the verb; the result says
 * how many ids were used. */
static HRESULT WINAPI cm_QueryContextMenu(IContextMenu *This, HMENU menu,
					  UINT index, UINT first, UINT last,
					  UINT flags)
{
	struct cmenu *m = CM(This);
	BOOL (WINAPI *insert)(HMENU, UINT, BOOL, const MENUITEMINFOW *) =
		U32("InsertMenuItemW");
	UINT used = 0;

	if (!menu)
		return E_INVALIDARG;
	for (int v = 0; v < V_COUNT; v++) {
		if (!verb_ok(m, v) || (flags & CMF_DEFAULTONLY && v != V_OPEN) ||
		    (v == V_RENAME && !(flags & CMF_CANRENAME)) ||
		    (v == V_EXPLORE && !(flags & CMF_EXPLORE)) ||
		    first + v > last)
			continue;
		WCHAR *label = utf8_to_utf16_dup(verbs[v].label);
		MENUITEMINFOW mi;
		memset(&mi, 0, sizeof(mi));
		mi.cbSize = sizeof(mi);
		mi.fMask = MIIM_ID | MIIM_STRING | MIIM_STATE | MIIM_FTYPE;
		mi.fType = MFT_STRING;
		mi.fState = v == V_OPEN && !(flags & CMF_NODEFAULT) ?
			    MFS_DEFAULT : 0;
		mi.wID = first + v;
		mi.dwTypeData = label;
		if (label && insert(menu, index++, TRUE, &mi))
			used = v + 1;
		free(label);
	}
	return MAKE_HRESULT(SEVERITY_SUCCESS, 0, used);
}

static void no_gui(const char *what, const char *name, const char *answer)
{
	fprintf(stderr, "%s [%s]\n  (no GUI: %s)\n", what, name, answer);
}

static HRESULT clip_items(struct cmenu *m, DWORD effect)
{
	HRESULT (WINAPI *set_clip)(IDataObject *) = OLE("OleSetClipboard");
	IDataObject *d;
	HRESULT hr = new_dobj(&m->it, &d);

	if (hr != S_OK)
		return hr;
	HGLOBAL h = global_alloc(GMEM_MOVEABLE, sizeof(DWORD));
	DWORD *p = h ? global_lock(h) : NULL;
	if (p) {
		*p = effect;
		global_unlock(h);
		hr = dobj_put(DOBJ(d), cf_effect(), h);
	} else {
		if (h)
			global_free(h);
		hr = E_OUTOFMEMORY;
	}
	if (hr == S_OK)
		hr = set_clip(d);
	d->lpVtbl->Release(d);
	return hr;
}

static HRESULT paste_into(const char *dir)
{
	HRESULT (WINAPI *get_clip)(IDataObject **) = OLE("OleGetClipboard");
	HRESULT (WINAPI *set_clip)(IDataObject *) = OLE("OleSetClipboard");
	IDataObject *d = NULL;
	char **p = NULL;
	HRESULT hr = get_clip(&d);

	if (hr != S_OK || !d)
		return FAILED(hr) ? hr : E_FAIL;
	int n = data_paths(d, &p);
	DWORD e = data_effect(d);
	int move = (e & DROPEFFECT_MOVE) && !(e & DROPEFFECT_COPY);
	hr = n > 0 ? transfer(p, n, dir, move) : DV_E_FORMATETC;
	paths_free(p, n > 0 ? n : 0);
	d->lpVtbl->Release(d);
	/* What was cut is gone once it is pasted. */
	if (SUCCEEDED(hr) && move)
		set_clip(NULL);
	return hr;
}

static HRESULT delete_items(struct cmenu *m, int to_bin)
{
	int (WINAPI *op)(SHFILEOPSTRUCTW *) = SH("SHFileOperationW");
	size_t chars = 1;
	HRESULT hr = E_OUTOFMEMORY;

	for (UINT i = 0; i < m->it.n; i++)
		chars += strlen(m->it.path[i]) + 1;
	WCHAR *from = malloc(chars * sizeof(WCHAR) * 2), *q = from;
	if (!from)
		return E_OUTOFMEMORY;
	for (UINT i = 0; i < m->it.n; i++) {
		WCHAR *w = utf8_to_utf16_dup(m->it.path[i]);
		if (!w)
			goto out;
		size_t k = wstrlen(w) + 1;
		memcpy(q, w, k * sizeof(WCHAR));
		q += k;
		free(w);
	}
	*q = 0;
	SHFILEOPSTRUCTW fo;
	memset(&fo, 0, sizeof(fo));
	fo.wFunc = FO_DELETE;
	fo.pFrom = from;
	fo.fFlags = FOF_NOCONFIRMATION | FOF_NOERRORUI |
		    (to_bin ? FOF_ALLOWUNDO : 0);
	int r = op(&fo);
	hr = r ? HRESULT_FROM_WIN32(r) : S_OK;
out:
	free(from);
	return hr;
}

static HRESULT open_items(struct cmenu *m, int show)
{
	BOOL (WINAPI *exec)(SHELLEXECUTEINFOW *) = SH("ShellExecuteExW");
	DWORD (WINAPI *last)(void) =
		builtin_export(&builtin_kernel32, "GetLastError");

	for (UINT i = 0; i < m->it.n; i++) {
		/* A folder opens in a view nothing shows. */
		if (!m->it.path[i] || is_folder(m->it.path[i]))
			continue;
		WCHAR *w = utf8_to_utf16_dup(m->it.path[i]);
		SHELLEXECUTEINFOW sei;
		memset(&sei, 0, sizeof(sei));
		sei.cbSize = sizeof(sei);
		sei.lpFile = w;
		sei.nShow = show;
		BOOL ok = w && exec(&sei);
		free(w);
		if (!ok)
			return w ? HRESULT_FROM_WIN32(last()) : E_OUTOFMEMORY;
	}
	return S_OK;
}

static int verb_of(LPCMINVOKECOMMANDINFO info)
{
	char *name = NULL;

	if ((info->fMask & CMIC_MASK_UNICODE) &&
	    info->cbSize >= sizeof(CMINVOKECOMMANDINFOEX)) {
		LPCWSTR w = ((const CMINVOKECOMMANDINFOEX *)info)->lpVerbW;
		if (w && !((ULONG_PTR)w >> 16))
			return (int)(ULONG_PTR)w;
		name = w ? utf16_to_utf8_dup(w) : NULL;
	}
	if (!name) {
		if (!((ULONG_PTR)info->lpVerb >> 16))
			return (int)(ULONG_PTR)info->lpVerb;
		name = acp_to_utf8(info->lpVerb);
	}
	int v = -1;
	for (int i = 0; name && i < V_COUNT; i++)
		if (!strcasecmp(name, verbs[i].name))
			v = i;
	free(name);
	return v;
}

static HRESULT WINAPI cm_InvokeCommand(IContextMenu *This,
				       LPCMINVOKECOMMANDINFO info)
{
	struct cmenu *m = CM(This);

	if (!info || info->cbSize < sizeof(CMINVOKECOMMANDINFO))
		return E_INVALIDARG;
	int v = verb_of(info);
	if (v < 0 || v >= V_COUNT || !verb_ok(m, v))
		return E_INVALIDARG;
	const char *name = m->background ? (m->it.dir ? m->it.dir : "") :
			   m->it.path[0] ? base_of(m->it.path[0]) : "";
	switch (v) {
	case V_OPEN:
		return open_items(m, info->nShow);
	case V_EXPLORE:
		return S_OK;
	case V_CUT:
		return clip_items(m, DROPEFFECT_MOVE);
	case V_COPY:
		return clip_items(m, DROPEFFECT_COPY | DROPEFFECT_LINK);
	case V_PASTE:
		return paste_into(m->background ? m->it.dir : m->it.path[0]);
	case V_DELETE:
		return delete_items(m, !(info->fMask & CMIC_MASK_SHIFT_DOWN));
	case V_RENAME:
		no_gui("Rename", name, "cancelled");
		return HRESULT_FROM_WIN32(ERROR_CANCELLED);
	default:
		no_gui("Properties", name, "nothing shown");
		return S_OK;
	}
}

static HRESULT WINAPI cm_GetCommandString(IContextMenu *This, UINT_PTR cmd,
					  UINT type, UINT *reserved,
					  LPSTR out, UINT cch)
{
	struct cmenu *m = CM(This);

	if (cmd >= V_COUNT || !verb_ok(m, (int)cmd))
		return (type & ~GCS_UNICODE) == GCS_VALIDATEA ? S_FALSE :
		       E_INVALIDARG;
	const char *s;
	switch (type & ~GCS_UNICODE) {
	case GCS_VERBA:
		s = verbs[cmd].name;
		break;
	case GCS_HELPTEXTA:
		s = verbs[cmd].help;
		break;
	case GCS_VALIDATEA:
		return S_OK;
	default:
		return E_INVALIDARG;
	}
	if (!out || !cch)
		return E_INVALIDARG;
	if (type & GCS_UNICODE) {
		if (cp_to_utf16(CP_UTF8, s, -1, (WCHAR *)out, (int)cch, 0) < 0)
			((WCHAR *)out)[0] = 0;
		((WCHAR *)out)[cch - 1] = 0;
	} else {
		snprintf(out, cch, "%s", s);
	}
	return S_OK;
}

static const IContextMenuVtbl cmenu_vtbl = {
	cm_QueryInterface, cm_AddRef, cm_Release, cm_QueryContextMenu,
	cm_InvokeCommand, cm_GetCommandString,
};

static HRESULT new_cmenu(LPCITEMIDLIST parent, const char *dir, UINT n,
			 LPCITEMIDLIST *children, int background, void **ppv)
{
	struct cmenu *m = calloc(1, sizeof(*m));

	if (!m)
		return E_OUTOFMEMORY;
	if (items_init(&m->it, parent, dir, n, children)) {
		free(m);
		return E_OUTOFMEMORY;
	}
	m->iface.lpVtbl = &cmenu_vtbl;
	m->ref = 1;
	m->background = background;
	*ppv = &m->iface;
	return S_OK;
}

/* IExtractIconA/W for one item. */

struct xicon {
	IExtractIconA a;
	IExtractIconW w;
	LONG ref;
	char *path;
	int csidl;
};

#define XA(This) ((struct xicon *)((char *)(This) - offsetof(struct xicon, a)))
#define XW(This) ((struct xicon *)((char *)(This) - offsetof(struct xicon, w)))

static HRESULT xi_query(struct xicon *x, REFIID riid, void **ppv)
{
	if (!ppv)
		return E_POINTER;
	if (same_guid(riid, &IID_IUnknown) ||
	    same_guid(riid, &IID_IExtractIconA))
		*ppv = &x->a;
	else if (same_guid(riid, &IID_IExtractIconW))
		*ppv = &x->w;
	else {
		*ppv = NULL;
		return E_NOINTERFACE;
	}
	InterlockedIncrement(&x->ref);
	return S_OK;
}

static ULONG xi_release(struct xicon *x)
{
	LONG r = InterlockedDecrement(&x->ref);
	if (!r) {
		free(x->path);
		free(x);
	}
	return (ULONG)r;
}

static HRESULT xi_location(struct xicon *x, UINT flags, void *buf, UINT cch,
			   int *index, UINT *out, int wide)
{
	int at = 0;
	char *loc = x->path ? shell_icon_location(x->path, 0, 0,
						  flags & GIL_OPENICON, &at) :
		    shell_icon_csidl(x->csidl, &at);

	if (!buf || !cch || !index) {
		free(loc);
		return E_INVALIDARG;
	}
	if (!loc)
		return E_OUTOFMEMORY;
	if (wide) {
		if (cp_to_utf16(CP_UTF8, loc, -1, buf, (int)cch, 0) < 0)
			((WCHAR *)buf)[0] = 0;
		((WCHAR *)buf)[cch - 1] = 0;
	} else {
		char *a = utf8_to_acp_dup(loc);
		snprintf(buf, cch, "%s", a ? a : "");
		free(a);
	}
	*index = at;
	if (out)
		*out = x->path && !strcasecmp(loc, x->path) ? GIL_PERINSTANCE :
		       GIL_PERCLASS;
	free(loc);
	return S_OK;
}

static HRESULT WINAPI xa_QueryInterface(IExtractIconA *This, REFIID r,
					void **p)
{
	return xi_query(XA(This), r, p);
}
static ULONG WINAPI xa_AddRef(IExtractIconA *This)
{
	return (ULONG)InterlockedIncrement(&XA(This)->ref);
}
static ULONG WINAPI xa_Release(IExtractIconA *This)
{
	return xi_release(XA(This));
}
static HRESULT WINAPI xa_GetIconLocation(IExtractIconA *This, UINT flags,
					 LPSTR file, UINT cch, int *index,
					 UINT *out)
{
	return xi_location(XA(This), flags, file, cch, index, out, 0);
}
static HRESULT WINAPI xa_Extract(IExtractIconA *This, LPCSTR file, UINT index,
				 HICON *large, HICON *small, UINT size)
{
	char *u = file ? acp_to_utf8(file) : NULL;
	HRESULT hr = file ? shell_extract_icon(u, (int)index, large, small) :
		     E_INVALIDARG;
	free(u);
	return hr;
}

static HRESULT WINAPI xw_QueryInterface(IExtractIconW *This, REFIID r,
					void **p)
{
	return xi_query(XW(This), r, p);
}
static ULONG WINAPI xw_AddRef(IExtractIconW *This)
{
	return (ULONG)InterlockedIncrement(&XW(This)->ref);
}
static ULONG WINAPI xw_Release(IExtractIconW *This)
{
	return xi_release(XW(This));
}
static HRESULT WINAPI xw_GetIconLocation(IExtractIconW *This, UINT flags,
					 LPWSTR file, UINT cch, int *index,
					 UINT *out)
{
	return xi_location(XW(This), flags, file, cch, index, out, 1);
}
static HRESULT WINAPI xw_Extract(IExtractIconW *This, LPCWSTR file,
				 UINT index, HICON *large, HICON *small,
				 UINT size)
{
	char *u = file ? utf16_to_utf8_dup(file) : NULL;
	HRESULT hr = file ? shell_extract_icon(u, (int)index, large, small) :
		     E_INVALIDARG;
	free(u);
	return hr;
}

static const IExtractIconAVtbl xa_vtbl = {
	xa_QueryInterface, xa_AddRef, xa_Release, xa_GetIconLocation,
	xa_Extract,
};
static const IExtractIconWVtbl xw_vtbl = {
	xw_QueryInterface, xw_AddRef, xw_Release, xw_GetIconLocation,
	xw_Extract,
};

static HRESULT new_xicon(LPCITEMIDLIST child, REFIID riid, void **ppv)
{
	struct xicon *x = calloc(1, sizeof(*x));

	if (!x)
		return E_OUTOFMEMORY;
	x->a.lpVtbl = &xa_vtbl;
	x->w.lpVtbl = &xw_vtbl;
	x->ref = 1;
	x->csidl = shell_pidl_csidl(child);
	if (x->csidl < 0 && !(x->path = shell_pidl_path(child))) {
		free(x);
		return E_INVALIDARG;
	}
	if (x->path)
		for (char *c = x->path; *c; c++)
			if (*c == '/')
				*c = '\\';
	HRESULT hr = xi_query(x, riid, ppv);
	xi_release(x);
	return hr;
}

/* What GetUIObjectOf gives for items of a folder. */
HRESULT shell_ui_object(LPCITEMIDLIST parent, const char *dir, UINT n,
			LPCITEMIDLIST *children, REFIID riid, void **ppv)
{
	if (!ppv)
		return E_POINTER;
	*ppv = NULL;
	if (!n || !children)
		return E_INVALIDARG;
	if (same_guid(riid, &IID_IDataObject)) {
		struct items it;
		if (items_init(&it, parent, dir, n, children))
			return E_OUTOFMEMORY;
		HRESULT hr = new_dobj(&it, (IDataObject **)ppv);
		items_free(&it);
		return hr;
	}
	if (same_guid(riid, &IID_IContextMenu))
		return new_cmenu(parent, dir, n, children, 0, ppv);
	if (same_guid(riid, &IID_IExtractIconA) ||
	    same_guid(riid, &IID_IExtractIconW))
		return n == 1 ? new_xicon(children[0], riid, ppv) :
		       E_INVALIDARG;
	if (same_guid(riid, &IID_IDropTarget)) {
		char *p = n == 1 ? shell_pidl_path(children[0]) : NULL;
		HRESULT hr = E_NOINTERFACE;
		if (p) {
			for (char *c = p; *c; c++)
				if (*c == '/')
					*c = '\\';
			if (is_folder(p))
				hr = new_dtarget(p, ppv);
		}
		free(p);
		return hr;
	}
	return E_NOINTERFACE;
}

/* IShellView. */

struct view {
	IShellView iface;
	LONG ref;
	IShellFolder *sf;
	LPITEMIDLIST parent;
	char *dir;
	HWND owner, hwnd, list;
	FOLDERSETTINGS fs;
	UINT state;
	UINT n;
	LPITEMIDLIST *item;
	BYTE *sel;
	int focus;
};

#define VIEW(This) ((struct view *)(This))

static void view_clear(struct view *v)
{
	for (UINT i = 0; i < v->n; i++)
		il_free(v->item[i]);
	free(v->item);
	free(v->sel);
	v->item = NULL;
	v->sel = NULL;
	v->n = 0;
	v->focus = -1;
}

static int il_equal(LPCITEMIDLIST a, LPCITEMIDLIST b)
{
	BOOL (WINAPI *eq)(LPCITEMIDLIST, LPCITEMIDLIST) = SH("ILIsEqual");
	return eq(a, b);
}

static char *display_name(struct view *v, LPCITEMIDLIST pidl)
{
	void (WINAPI *task_free)(void *) = OLE("CoTaskMemFree");
	STRRET s;
	char *r = NULL;

	memset(&s, 0, sizeof(s));
	if (v->sf->lpVtbl->GetDisplayNameOf(v->sf, pidl, SHGDN_INFOLDER, &s) ==
	    S_OK) {
		if (s.uType == STRRET_WSTR && s.u.pOleStr)
			r = utf16_to_utf8_dup(s.u.pOleStr);
		if (s.uType == STRRET_WSTR)
			task_free(s.u.pOleStr);
	}
	return r ? r : strdup("");
}

/* The list view shows the items, with their selection. */
static void view_show(struct view *v)
{
	LRESULT (WINAPI *send)(HWND, UINT, WPARAM, LPARAM) =
		U32("SendMessageW");

	if (!v->list)
		return;
	send(v->list, LVM_DELETEALLITEMS, 0, 0);
	for (UINT i = 0; i < v->n; i++) {
		char *name = display_name(v, v->item[i]);
		WCHAR *w = utf8_to_utf16_dup(name);
		LVITEMW li;
		memset(&li, 0, sizeof(li));
		li.mask = LVIF_TEXT | LVIF_STATE;
		li.iItem = (int)i;
		li.pszText = w;
		li.stateMask = LVIS_SELECTED | LVIS_FOCUSED;
		li.state = (v->sel[i] ? LVIS_SELECTED : 0) |
			   ((int)i == v->focus ? LVIS_FOCUSED : 0);
		send(v->list, LVM_INSERTITEMW, 0, (LPARAM)&li);
		free(w);
		free(name);
	}
}

static void view_mark(struct view *v, UINT i)
{
	LRESULT (WINAPI *send)(HWND, UINT, WPARAM, LPARAM) =
		U32("SendMessageW");
	LVITEMW li;

	if (!v->list)
		return;
	memset(&li, 0, sizeof(li));
	li.stateMask = LVIS_SELECTED | LVIS_FOCUSED;
	li.state = (v->sel[i] ? LVIS_SELECTED : 0) |
		   ((int)i == v->focus ? LVIS_FOCUSED : 0);
	send(v->list, LVM_SETITEMSTATE, i, (LPARAM)&li);
}

static HRESULT WINAPI v_Refresh(IShellView *This)
{
	struct view *v = VIEW(This);
	IEnumIDList *e = NULL;
	LPITEMIDLIST *old = v->item;
	BYTE *old_sel = v->sel;
	UINT old_n = v->n;
	HRESULT hr = v->sf->lpVtbl->EnumObjects(v->sf, v->owner,
			SHCONTF_FOLDERS | SHCONTF_NONFOLDERS, &e);

	if (FAILED(hr))
		return hr;
	v->item = NULL;
	v->sel = NULL;
	v->n = 0;
	UINT cap = 0;
	LPITEMIDLIST pidl;
	ULONG got;
	while (e && e->lpVtbl->Next(e, 1, &pidl, &got) == S_OK && got) {
		if (v->n == cap) {
			cap = cap ? cap * 2 : 16;
			LPITEMIDLIST *g = realloc(v->item, cap * sizeof(*g));
			BYTE *s = g ? realloc(v->sel, cap) : NULL;
			if (g)
				v->item = g;
			if (!s) {
				il_free(pidl);
				hr = E_OUTOFMEMORY;
				break;
			}
			v->sel = s;
		}
		v->sel[v->n] = 0;
		/* A refresh keeps the selection of what is still there. */
		for (UINT k = 0; k < old_n; k++)
			if (old_sel[k] && il_equal(old[k], pidl))
				v->sel[v->n] = 1;
		v->item[v->n++] = pidl;
	}
	if (e)
		e->lpVtbl->Release(e);
	for (UINT k = 0; k < old_n; k++)
		il_free(old[k]);
	free(old);
	free(old_sel);
	v->focus = -1;
	view_show(v);
	return SUCCEEDED(hr) ? S_OK : hr;
}

static HRESULT WINAPI v_QueryInterface(IShellView *This, REFIID riid,
				       void **ppv)
{
	if (!ppv)
		return E_POINTER;
	if (same_guid(riid, &IID_IUnknown) || same_guid(riid, &IID_IShellView) ||
	    same_guid(riid, &IID_IOleWindow)) {
		*ppv = This;
		InterlockedIncrement(&VIEW(This)->ref);
		return S_OK;
	}
	*ppv = NULL;
	return E_NOINTERFACE;
}

static ULONG WINAPI v_AddRef(IShellView *This)
{
	return (ULONG)InterlockedIncrement(&VIEW(This)->ref);
}

static HRESULT WINAPI v_DestroyViewWindow(IShellView *This)
{
	struct view *v = VIEW(This);
	BOOL (WINAPI *destroy)(HWND) = U32("DestroyWindow");

	if (v->hwnd)
		destroy(v->hwnd);
	v->hwnd = v->list = NULL;
	return S_OK;
}

static ULONG WINAPI v_Release(IShellView *This)
{
	struct view *v = VIEW(This);
	LONG r = InterlockedDecrement(&v->ref);
	if (!r) {
		v_DestroyViewWindow(This);
		view_clear(v);
		v->sf->lpVtbl->Release(v->sf);
		il_free(v->parent);
		free(v->dir);
		free(v);
	}
	return (ULONG)r;
}

static HRESULT WINAPI v_GetWindow(IShellView *This, HWND *hwnd)
{
	if (!hwnd)
		return E_INVALIDARG;
	*hwnd = VIEW(This)->hwnd;
	return *hwnd ? S_OK : E_FAIL;
}

static HRESULT WINAPI v_ContextSensitiveHelp(IShellView *This, BOOL enter)
{
	return E_NOTIMPL;
}

static HRESULT WINAPI v_TranslateAccelerator(IShellView *This, MSG *msg)
{
	return S_FALSE;
}

static HRESULT WINAPI v_EnableModeless(IShellView *This, BOOL enable)
{
	return S_OK;
}

static HRESULT WINAPI v_UIActivate(IShellView *This, UINT state)
{
	VIEW(This)->state = state;
	return S_OK;
}

/* The view window keeps its list view filling it. */
WINFN(LRESULT) defview_proc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
{
	LRESULT (WINAPI *def)(HWND, UINT, WPARAM, LPARAM) =
		U32("DefWindowProcW");
	HWND (WINAPI *child)(HWND, UINT) = U32("GetWindow");
	BOOL (WINAPI *move)(HWND, int, int, int, int, BOOL) = U32("MoveWindow");

	if (msg == WM_SIZE) {
		HWND list = child(hwnd, GW_CHILD);
		if (list)
			move(list, 0, 0, LOWORD(lp), HIWORD(lp), TRUE);
		return 0;
	}
	return def(hwnd, msg, wp, lp);
}

static pthread_once_t defview_once = PTHREAD_ONCE_INIT;

static void defview_register(void)
{
	ATOM (WINAPI *reg)(const WNDCLASSW *) = U32("RegisterClassW");
	void (WINAPI *init)(void) =
		builtin_export(&builtin_comctl32, "InitCommonControls");
	static const WCHAR name[] = { 'S', 'H', 'E', 'L', 'L', 'D', 'L', 'L',
				      '_', 'D', 'e', 'f', 'V', 'i', 'e', 'w',
				      0 };
	WNDCLASSW wc;

	init();
	memset(&wc, 0, sizeof(wc));
	wc.style = CS_GLOBALCLASS;
	wc.lpfnWndProc = defview_proc;
	wc.lpszClassName = name;
	reg(&wc);
}

/* An IShellBrowser is an IOleWindow first: its window holds the view. */
typedef struct {
	HRESULT (WINAPI *QueryInterface)(void *This, REFIID riid, void **ppv);
	ULONG (WINAPI *AddRef)(void *This);
	ULONG (WINAPI *Release)(void *This);
	HRESULT (WINAPI *GetWindow)(void *This, HWND *hwnd);
} ole_window_vtbl;

static HRESULT WINAPI v_CreateViewWindow(IShellView *This, IShellView *prev,
					 LPCFOLDERSETTINGS fs,
					 IShellBrowser *browser, RECT *rc,
					 HWND *out)
{
	struct view *v = VIEW(This);
	HWND (WINAPI *create)(DWORD, LPCWSTR, LPCWSTR, DWORD, int, int, int,
			      int, HWND, HMENU, HINSTANCE, LPVOID) =
		U32("CreateWindowExW");
	static const WCHAR defview[] = { 'S', 'H', 'E', 'L', 'L', 'D', 'L',
					 'L', '_', 'D', 'e', 'f', 'V', 'i',
					 'e', 'w', 0 };
	static const WCHAR listview[] = { 'S', 'y', 's', 'L', 'i', 's', 't',
					  'V', 'i', 'e', 'w', '3', '2', 0 };
	static const DWORD modes[] = { LVS_ICON, LVS_ICON, LVS_SMALLICON,
				       LVS_LIST, LVS_REPORT };

	if (!out || !fs)
		return E_INVALIDARG;
	*out = NULL;
	if (v->hwnd)
		return E_UNEXPECTED;
	HWND parent = v->owner;
	if (browser) {
		const ole_window_vtbl *vt = *(const ole_window_vtbl **)browser;
		HWND b = NULL;
		if (vt->GetWindow(browser, &b) == S_OK && b)
			parent = b;
	}
	pthread_once(&defview_once, defview_register);
	v->fs = *fs;
	int x = rc ? rc->left : 0, y = rc ? rc->top : 0;
	int w = rc ? rc->right - rc->left : 0, h = rc ? rc->bottom - rc->top : 0;
	v->hwnd = create(0, defview, NULL, (parent ? WS_CHILD : WS_POPUP) |
			 WS_VISIBLE, x, y, w, h, parent, NULL, NULL, NULL);
	if (!v->hwnd)
		return E_FAIL;
	DWORD mode = fs->ViewMode < 5 ? modes[fs->ViewMode] : LVS_ICON;
	v->list = create(0, listview, NULL, WS_CHILD | WS_VISIBLE |
			 LVS_SHOWSELALWAYS | mode, 0, 0, w, h, v->hwnd, NULL,
			 NULL, NULL);
	if (!v->list) {
		v_DestroyViewWindow(This);
		return E_FAIL;
	}
	HRESULT hr = v->item ? (view_show(v), S_OK) : v_Refresh(This);
	if (FAILED(hr)) {
		v_DestroyViewWindow(This);
		return hr;
	}
	*out = v->hwnd;
	return S_OK;
}

static HRESULT WINAPI v_GetCurrentInfo(IShellView *This, LPFOLDERSETTINGS fs)
{
	if (!fs)
		return E_INVALIDARG;
	*fs = VIEW(This)->fs;
	return S_OK;
}

static HRESULT WINAPI v_AddPropertySheetPages(IShellView *This, DWORD res,
					      LPFNADDPROPSHEETPAGE fn,
					      LPARAM lp)
{
	return S_OK;	/* it has none */
}

static HRESULT WINAPI v_SaveViewState(IShellView *This)
{
	return S_OK;
}

static HRESULT WINAPI v_SelectItem(IShellView *This, LPCITEMIDLIST pidl,
				   UINT flags)
{
	struct view *v = VIEW(This);
	int at = -1;

	if (!v->item) {
		HRESULT hr = v_Refresh(This);
		if (FAILED(hr))
			return hr;
	}
	for (UINT i = 0; pidl && i < v->n && at < 0; i++)
		if (il_equal(v->item[i], pidl))
			at = (int)i;
	if (pidl && at < 0)
		return E_INVALIDARG;
	if (flags & SVSI_DESELECTOTHERS)
		for (UINT i = 0; i < v->n; i++)
			if ((int)i != at && v->sel[i]) {
				v->sel[i] = 0;
				view_mark(v, i);
			}
	if (at < 0)
		return S_OK;
	if (flags & SVSI_SELECT)
		v->sel[at] = 1;
	else if (!(flags & SVSI_DESELECTOTHERS) && !(flags & SVSI_FOCUSED))
		v->sel[at] = 0;
	if (flags & SVSI_FOCUSED)
		v->focus = at;
	view_mark(v, (UINT)at);
	if ((flags & SVSI_EDIT) == SVSI_EDIT) {
		char *name = display_name(v, v->item[at]);
		no_gui("Rename", name, "cancelled");
		free(name);
	}
	return S_OK;
}

static HRESULT WINAPI v_GetItemObject(IShellView *This, UINT what, REFIID riid,
				      void **ppv)
{
	struct view *v = VIEW(This);

	if (!ppv)
		return E_POINTER;
	*ppv = NULL;
	switch (what & SVGIO_TYPE_MASK) {
	case SVGIO_BACKGROUND:
		if (same_guid(riid, &IID_IContextMenu))
			return new_cmenu(v->parent, v->dir, 0, NULL, 1, ppv);
		if (same_guid(riid, &IID_IDropTarget) && v->dir)
			return new_dtarget(v->dir, ppv);
		return E_NOINTERFACE;
	case SVGIO_SELECTION:
	case SVGIO_ALLVIEW: {
		if (!v->item) {
			HRESULT hr = v_Refresh(This);
			if (FAILED(hr))
				return hr;
		}
		int all = (what & SVGIO_TYPE_MASK) == SVGIO_ALLVIEW;
		LPCITEMIDLIST *list = malloc((v->n ? v->n : 1) * sizeof(*list));
		UINT k = 0;
		if (!list)
			return E_OUTOFMEMORY;
		for (UINT i = 0; i < v->n; i++)
			if (all || v->sel[i])
				list[k++] = v->item[i];
		HRESULT hr = k ? v->sf->lpVtbl->GetUIObjectOf(v->sf, v->owner,
				 k, list, riid, NULL, ppv) : E_FAIL;
		free(list);
		return hr;
	}
	default:
		return E_INVALIDARG;
	}
}

static const IShellViewVtbl view_vtbl = {
	v_QueryInterface, v_AddRef, v_Release, v_GetWindow,
	v_ContextSensitiveHelp, v_TranslateAccelerator, v_EnableModeless,
	v_UIActivate, v_Refresh, v_CreateViewWindow, v_DestroyViewWindow,
	v_GetCurrentInfo, v_AddPropertySheetPages, v_SaveViewState,
	v_SelectItem, v_GetItemObject,
};

/* What CreateViewObject gives for a folder. */
HRESULT shell_view_object(IShellFolder *sf, LPCITEMIDLIST parent,
			  const char *dir, HWND owner, REFIID riid, void **ppv)
{
	if (!ppv)
		return E_POINTER;
	*ppv = NULL;
	if (same_guid(riid, &IID_IDropTarget))
		return dir ? new_dtarget(dir, ppv) : E_NOINTERFACE;
	if (same_guid(riid, &IID_IContextMenu))
		return new_cmenu(parent, dir, 0, NULL, 1, ppv);
	if (!same_guid(riid, &IID_IShellView))
		return E_NOINTERFACE;
	struct view *v = calloc(1, sizeof(*v));
	if (!v)
		return E_OUTOFMEMORY;
	v->parent = parent ? il_clone(parent) : NULL;
	v->dir = dir ? strdup(dir) : NULL;
	if ((parent && !v->parent) || (dir && !v->dir)) {
		il_free(v->parent);
		free(v->dir);
		free(v);
		return E_OUTOFMEMORY;
	}
	v->iface.lpVtbl = &view_vtbl;
	v->ref = 1;
	v->sf = sf;
	sf->lpVtbl->AddRef(sf);
	v->owner = owner;
	v->focus = -1;
	v->fs.ViewMode = FVM_ICON;
	*ppv = &v->iface;
	return S_OK;
}

/* Dropped files. */

/* The i-th file as UTF-8; with i == 0xffffffff, the count in *count. */
static char *drop_file(HDROP drop, UINT i, UINT *count)
{
	char **p = NULL;
	int n = drop ? hdrop_paths((HGLOBAL)drop, &p) : -1;
	char *r = NULL;

	*count = n > 0 ? (UINT)n : 0;
	if (n > 0 && i < (UINT)n) {
		r = p[i];
		p[i] = NULL;
	}
	paths_free(p, n > 0 ? n : 0);
	return r;
}

WINFN(UINT) sh_DragQueryFileA(HDROP drop, UINT i, LPSTR buf, UINT n)
{
	UINT count;
	char *f = drop_file(drop, i, &count);

	if (i == 0xffffffff)
		return count;
	if (!f) {
		if (buf && n)
			buf[0] = 0;
		return 0;
	}
	char *a = utf8_to_acp_dup(f);
	free(f);
	UINT len = a ? (UINT)strlen(a) : 0;
	if (buf && n) {
		UINT k = len < n ? len : n - 1;
		memcpy(buf, a ? a : "", k);
		buf[k] = 0;
		len = k;
	}
	free(a);
	return len;
}

WINFN(UINT) sh_DragQueryFileW(HDROP drop, UINT i, LPWSTR buf, UINT n)
{
	UINT count;
	char *f = drop_file(drop, i, &count);

	if (i == 0xffffffff)
		return count;
	if (!f) {
		if (buf && n)
			buf[0] = 0;
		return 0;
	}
	WCHAR *w = utf8_to_utf16_dup(f);
	free(f);
	UINT len = w ? (UINT)wstrlen(w) : 0;
	if (buf && n) {
		UINT k = len < n ? len : n - 1;
		if (w)
			memcpy(buf, w, k * sizeof(WCHAR));
		buf[k] = 0;
		len = k;
	}
	free(w);
	return len;
}

/* The drop point, client relative; FALSE when it was outside it. */
WINFN(BOOL) sh_DragQueryPoint(HDROP drop, POINT *pt)
{
	DROPFILES *df = drop ? global_lock((HGLOBAL)drop) : NULL;
	BOOL r = FALSE;

	if (!df)
		return FALSE;
	if (global_size((HGLOBAL)drop) >= sizeof(*df)) {
		if (pt)
			*pt = df->pt;
		r = !df->fNC;
	}
	global_unlock((HGLOBAL)drop);
	return r;
}

WINFN(void) sh_DragFinish(HDROP drop)
{
	if (drop)
		global_free((HGLOBAL)drop);
}

/* Whether a window takes WM_DROPFILES: WS_EX_ACCEPTFILES. */
WINFN(void) sh_DragAcceptFiles(HWND hwnd, BOOL accept)
{
	LONG (WINAPI *get)(HWND, int) = U32("GetWindowLongW");
	LONG (WINAPI *set)(HWND, int, LONG) = U32("SetWindowLongW");

	if (!hwnd)
		return;
	LONG ex = get(hwnd, GWL_EXSTYLE);
	set(hwnd, GWL_EXSTYLE, accept ? ex | WS_EX_ACCEPTFILES :
					ex & ~WS_EX_ACCEPTFILES);
}

#define E(name) { #name, 0, (void *)sh_##name }

const struct builtin_export shell_ui_exports[] = {
	E(DragAcceptFiles), E(DragFinish), E(DragQueryFileA),
	E(DragQueryFileW), E(DragQueryPoint),
	BUILTIN_END
};
