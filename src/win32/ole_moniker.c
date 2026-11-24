/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * Monikers, bind contexts and the running object table.
 *
 * The system monikers: file (a path), item (a delimiter and a name, bound
 * through the IOleItemContainer to its left), anti (cancels what is to its
 * left), generic composite (a flat list of the others), class (a CLSID,
 * bound to its class object) and pointer (an object already in hand).
 * MkParseDisplayName reads "path!item!item", "clsid:GUID:", "@ProgID" and
 * "ProgID:" names. There is one process, so the running object table is
 * this process's own, and a moniker it holds binds to its object directly.
 *
 * Saved monikers use Windows' stream formats, so OleSaveToStream output
 * reads back on either side: an item moniker as counted ANSI strings (with
 * UTF-16 after the NUL when ANSI loses characters), a file moniker as the
 * number of leading "..\" parts, the ANSI path, an 0xDEADFFFF marker and an
 * optional UTF-16 copy, an anti moniker as its count, a class moniker as
 * its CLSID, and a composite as its parts, each with its CLSID.
 */
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>

#include "common/log.h"
#include "runtime/builtin.h"
#include "win32/codepage.h"
#include "win32/internal.h"
#include "win32/ole.h"
#include "win32/path.h"

extern const struct builtin_dll builtin_ole32;

#define SYS_CLSID(n) { n, 0, 0, { 0xc0, 0, 0, 0, 0, 0, 0, 0x46 } }
static const CLSID clsid_file = SYS_CLSID(0x303);
static const CLSID clsid_item = SYS_CLSID(0x304);
static const CLSID clsid_anti = SYS_CLSID(0x305);
static const CLSID clsid_pointer = SYS_CLSID(0x306);
static const CLSID clsid_composite = SYS_CLSID(0x309);
static const CLSID clsid_class = SYS_CLSID(0x31a);

static int guid_eq(const GUID *a, const GUID *b)
{
	return !memcmp(a, b, sizeof(*a));
}

/* ---- wide strings ---- */

static size_t wlen(const WCHAR *s)
{
	size_t n = 0;
	while (s && s[n])
		n++;
	return n;
}

static WCHAR *wdupn(const WCHAR *s, size_t n)
{
	WCHAR *d = malloc((n + 1) * sizeof(WCHAR));
	if (d) {
		if (n)
			memcpy(d, s, n * sizeof(WCHAR));
		d[n] = 0;
	}
	return d;
}

static WCHAR *wdup(const WCHAR *s)
{
	return wdupn(s, wlen(s));
}

static WCHAR wup(WCHAR c)
{
	if ((c >= 'a' && c <= 'z') || (c >= 0xe0 && c <= 0xfe && c != 0xf7))
		return c - 0x20;
	return c;
}

static int wieqn(const WCHAR *a, const WCHAR *b, size_t n)
{
	for (size_t i = 0; i < n; i++)
		if (wup(a[i]) != wup(b[i]))
			return 0;
	return 1;
}

static int wieq(const WCHAR *a, const WCHAR *b)
{
	size_t n = wlen(a);
	return n == wlen(b) && wieqn(a, b, n);
}

static WCHAR *wascii(const char *s)
{
	size_t n = strlen(s);
	WCHAR *w = malloc((n + 1) * sizeof(WCHAR));
	for (size_t i = 0; w && i <= n; i++)
		w[i] = (BYTE)s[i];
	return w;
}

/* A display name for the caller: CoTaskMem, as Windows hands them out. */
static LPOLESTR task_wdup(const WCHAR *s)
{
	size_t n = wlen(s);
	LPOLESTR d = ole_taskmem_alloc((n + 1) * sizeof(WCHAR));
	if (d) {
		memcpy(d, s, n * sizeof(WCHAR));
		d[n] = 0;
	}
	return d;
}

static WCHAR *acp_to_w(const char *s, size_t n)
{
	int k = cp_to_utf16(CP_ACP, s, (int)n, NULL, 0, 0);
	WCHAR *w = malloc(((k > 0 ? (size_t)k : 0) + 1) * sizeof(WCHAR));
	if (!w)
		return NULL;
	if (k > 0)
		cp_to_utf16(CP_ACP, s, (int)n, w, k, 0);
	w[k > 0 ? k : 0] = 0;
	return w;
}

/* ANSI form of s (malloc()ed, *lossy if it doesn't read back as s). */
static char *w_to_acp(const WCHAR *s, int *lossy)
{
	int n = (int)wlen(s);
	int k = cp_from_utf16(CP_ACP, s, n, NULL, 0, '?', NULL);
	char *a = malloc((k > 0 ? (size_t)k : 0) + 1);
	int dflt = 0;
	if (!a)
		return NULL;
	if (k > 0)
		cp_from_utf16(CP_ACP, s, n, a, k, '?', &dflt);
	a[k > 0 ? k : 0] = 0;
	if (lossy) {
		WCHAR *back = acp_to_w(a, strlen(a));
		*lossy = dflt || !back || wlen(back) != (size_t)n ||
			 memcmp(back, s, n * sizeof(WCHAR));
		free(back);
	}
	return a;
}

/* The Unix form of a Windows path given in UTF-16; malloc()ed. */
static char *unix_path_w(const WCHAR *w)
{
	char *u = utf16_to_utf8_dup(w);
	char *p = u ? path_to_unix(u) : NULL;
	free(u);
	return p;
}

static void now(FILETIME *ft)
{
	struct timespec ts;
	clock_gettime(CLOCK_REALTIME, &ts);
	uint64_t t = (uint64_t)ts.tv_sec * 10000000u + ts.tv_nsec / 100 +
		     116444736000000000ull;
	ft->dwLowDateTime = (DWORD)t;
	ft->dwHighDateTime = (DWORD)(t >> 32);
}

static void unix_time(time_t s, FILETIME *ft)
{
	uint64_t t = (uint64_t)s * 10000000u + 116444736000000000ull;
	ft->dwLowDateTime = (DWORD)t;
	ft->dwHighDateTime = (DWORD)(t >> 32);
}

/* ---- calls into the rest of ole32 ---- */

static void *ole_fn(const char *name)
{
	return builtin_export(&builtin_ole32, name);
}

static HRESULT create_instance(REFCLSID clsid, DWORD ctx, REFIID riid,
			       void **out)
{
	HRESULT (WINAPI *f)(REFCLSID, IUnknown *, DWORD, REFIID, void **) =
		ole_fn("CoCreateInstance");
	return f ? f(clsid, NULL, ctx, riid, out) : E_UNEXPECTED;
}

static HRESULT class_object(REFCLSID clsid, DWORD ctx, REFIID riid,
			    void **out)
{
	HRESULT (WINAPI *f)(REFCLSID, DWORD, void *, REFIID, void **) =
		ole_fn("CoGetClassObject");
	return f ? f(clsid, ctx, NULL, riid, out) : E_UNEXPECTED;
}

static HRESULT save_to_stream(IMoniker *m, IStream *stm)
{
	HRESULT (WINAPI *f)(IPersistStream *, IStream *) =
		ole_fn("OleSaveToStream");
	return f ? f((IPersistStream *)m, stm) : E_UNEXPECTED;
}

static HRESULT load_from_stream(IStream *stm, IMoniker **out)
{
	HRESULT (WINAPI *f)(IStream *, REFIID, void **) =
		ole_fn("OleLoadFromStream");
	return f ? f(stm, &IID_IMoniker, (void **)out) : E_UNEXPECTED;
}

static HRESULT stm_write(IStream *s, const void *p, ULONG n)
{
	ULONG done = 0;
	HRESULT hr = s->lpVtbl->Write(s, p, n, &done);
	return FAILED(hr) ? hr : done == n ? S_OK : STG_E_MEDIUMFULL;
}

static HRESULT stm_read(IStream *s, void *p, ULONG n)
{
	ULONG done = 0;
	HRESULT hr = s->lpVtbl->Read(s, p, n, &done);
	return FAILED(hr) ? hr : done == n ? S_OK : STG_E_READFAULT;
}

/* ---- the monikers ---- */

/* An anti moniker's largest count: its "\\.." display name must fit. */
#define ANTI_MAX 0x100000u
/* How deep composite records may nest inside one another on Load. */
#define LOAD_DEPTH_MAX 32
static __thread unsigned load_depth;

enum kind { K_FILE, K_ITEM, K_ANTI, K_COMPOSITE, K_CLASS, K_POINTER };

struct mk {
	IMoniker iface;
	IROTData rotdata;
	LONG refs;
	enum kind kind;
	WCHAR *name;		/* file: the path; item: the item */
	WCHAR *delim;		/* item */
	DWORD count;		/* anti */
	IMoniker **parts;	/* composite: two or more, none a composite */
	DWORD nparts;
	CLSID clsid;		/* class */
	IUnknown *obj;		/* pointer */
};

static const IMonikerVtbl mk_vtbl;
static const IROTDataVtbl rotdata_vtbl;

static struct mk *ours(IMoniker *m)
{
	return m && m->lpVtbl == &mk_vtbl ? (struct mk *)m : NULL;
}

static struct mk *mk_new(enum kind kind)
{
	struct mk *m = calloc(1, sizeof(*m));
	if (m) {
		m->iface.lpVtbl = &mk_vtbl;
		m->rotdata.lpVtbl = &rotdata_vtbl;
		m->refs = 1;
		m->kind = kind;
	}
	return m;
}

static HRESULT file_new(const WCHAR *path, size_t n, IMoniker **out)
{
	struct mk *m = mk_new(K_FILE);
	if (!m || !(m->name = wdupn(path, n))) {
		free(m);
		return E_OUTOFMEMORY;
	}
	*out = &m->iface;
	return S_OK;
}

static HRESULT item_new(const WCHAR *delim, const WCHAR *item, IMoniker **out)
{
	struct mk *m = mk_new(K_ITEM);
	if (!m || !(m->delim = wdup(delim)) || !(m->name = wdup(item))) {
		if (m)
			free(m->delim);
		free(m);
		return E_OUTOFMEMORY;
	}
	*out = &m->iface;
	return S_OK;
}

static HRESULT anti_new(DWORD count, IMoniker **out)
{
	struct mk *m = mk_new(K_ANTI);
	if (!m)
		return E_OUTOFMEMORY;
	m->count = count;
	*out = &m->iface;
	return S_OK;
}

static HRESULT class_new(const CLSID *clsid, IMoniker **out)
{
	struct mk *m = mk_new(K_CLASS);
	if (!m)
		return E_OUTOFMEMORY;
	m->clsid = *clsid;
	*out = &m->iface;
	return S_OK;
}

static HRESULT pointer_new(IUnknown *obj, IMoniker **out)
{
	struct mk *m = mk_new(K_POINTER);
	if (!m)
		return E_OUTOFMEMORY;
	m->obj = obj;
	if (obj)
		obj->lpVtbl->AddRef(obj);
	*out = &m->iface;
	return S_OK;
}

/* A list of monikers, each holding a reference. */
struct list {
	IMoniker **v;
	DWORD n, cap;
};

static int list_add(struct list *l, IMoniker *m)
{
	if (l->n == l->cap) {
		DWORD cap = l->cap ? l->cap * 2 : 8;
		IMoniker **v = realloc(l->v, cap * sizeof(*v));
		if (!v)
			return 0;
		l->v = v;
		l->cap = cap;
	}
	m->lpVtbl->AddRef(m);
	l->v[l->n++] = m;
	return 1;
}

static void list_pop(struct list *l)
{
	IMoniker *m = l->v[--l->n];
	m->lpVtbl->Release(m);
}

static void list_free(struct list *l)
{
	while (l->n)
		list_pop(l);
	free(l->v);
	l->v = NULL;
	l->cap = 0;
}

/* The parts of m: a composite's, or m itself. */
static int parts_of(IMoniker *m, struct list *l)
{
	struct mk *o = ours(m);
	if (o && o->kind == K_COMPOSITE) {
		for (DWORD i = 0; i < o->nparts; i++)
			if (!list_add(l, o->parts[i]))
				return 0;
		return 1;
	}
	return list_add(l, m);
}

/* The moniker made of parts v[0..n): NULL, the one part, or a composite. */
static HRESULT from_parts(IMoniker *const *v, DWORD n, IMoniker **out)
{
	*out = NULL;
	if (!n)
		return S_OK;
	if (n == 1) {
		v[0]->lpVtbl->AddRef(v[0]);
		*out = v[0];
		return S_OK;
	}
	struct mk *m = mk_new(K_COMPOSITE);
	if (!m || !(m->parts = malloc(n * sizeof(*m->parts)))) {
		free(m);
		return E_OUTOFMEMORY;
	}
	for (DWORD i = 0; i < n; i++) {
		m->parts[i] = v[i];
		v[i]->lpVtbl->AddRef(v[i]);
	}
	m->nparts = n;
	*out = &m->iface;
	return S_OK;
}

/*
 * CreateGenericComposite: the parts of left then right, where each pair
 * that meets is given the chance to combine (a file and a relative file,
 * anything and an anti moniker).
 */
static HRESULT compose(IMoniker *left, IMoniker *right, IMoniker **out)
{
	struct list l = { 0 }, r = { 0 };
	HRESULT hr = S_OK;

	*out = NULL;
	if (!left || !right) {
		IMoniker *m = left ? left : right;
		if (m)
			m->lpVtbl->AddRef(m);
		*out = m;
		return S_OK;
	}
	if (!parts_of(left, &l) || !parts_of(right, &r)) {
		hr = E_OUTOFMEMORY;
		goto out;
	}
	for (DWORD i = 0; i < r.n && SUCCEEDED(hr); i++) {
		IMoniker *cur = r.v[i];
		cur->lpVtbl->AddRef(cur);
		while (cur && l.n) {
			IMoniker *last = l.v[l.n - 1], *c = NULL;
			HRESULT h = last->lpVtbl->ComposeWith(last, cur, TRUE,
							      &c);
			if (h == MK_E_NEEDGENERIC || (SUCCEEDED(h) && c == last)) {
				if (c)
					c->lpVtbl->Release(c);
				break;
			}
			if (FAILED(h)) {
				hr = h;
				break;
			}
			list_pop(&l);
			cur->lpVtbl->Release(cur);
			cur = c;	/* NULL: the two cancelled */
		}
		if (cur && SUCCEEDED(hr) && !list_add(&l, cur))
			hr = E_OUTOFMEMORY;
		if (cur)
			cur->lpVtbl->Release(cur);
	}
	if (SUCCEEDED(hr))
		hr = from_parts(l.v, l.n, out);
out:
	list_free(&l);
	list_free(&r);
	return hr;
}

/* ---- file paths ---- */

static int is_sep(WCHAR c)
{
	return c == '\\' || c == '/';
}

static int is_absolute(const WCHAR *p)
{
	return is_sep(p[0]) || (p[0] && p[1] == ':');
}

/* Splits a path at its separators; the parts point into p. */
struct comp {
	const WCHAR *s;
	size_t n;
};

static size_t split(const WCHAR *p, struct comp **out)
{
	size_t n = 0, cap = 0;
	struct comp *v = NULL;

	for (size_t i = 0; p[i];) {
		while (is_sep(p[i]))
			i++;
		if (!p[i])
			break;
		size_t j = i;
		while (p[j] && !is_sep(p[j]))
			j++;
		if (n == cap) {
			cap = cap ? cap * 2 : 8;
			struct comp *nv = realloc(v, cap * sizeof(*v));
			if (!nv) {
				free(v);
				*out = NULL;
				return 0;
			}
			v = nv;
		}
		v[n].s = p + i;
		v[n].n = j - i;
		n++;
		i = j;
	}
	*out = v;
	return n;
}

static int is_dotdot(const struct comp *c)
{
	return c->n == 2 && c->s[0] == '.' && c->s[1] == '.';
}

/* base, then rel's parts, each ".." taking one off; malloc()ed. */
static WCHAR *join(const WCHAR *base, const WCHAR *rel)
{
	struct comp *r;
	size_t nr = split(rel, &r), up = 0;
	size_t bl = wlen(base);

	while (up < nr && is_dotdot(&r[up]))
		up++;
	/* Take up parts off base, keeping its separators. */
	size_t keep = bl, ups = up;
	while (ups && keep) {
		while (keep && is_sep(base[keep - 1]))
			keep--;
		while (keep && !is_sep(base[keep - 1]))
			keep--;
		ups--;
	}
	while (keep && is_sep(base[keep - 1]))
		keep--;
	/* A root keeps its separator: "C:\" and "\" are not "C:" and "". */
	if (keep == 2 && base[1] == ':' && is_sep(base[2]))
		keep = 3;
	else if (!keep && bl && is_sep(base[0]) && !(bl > 1 && is_sep(base[1])))
		keep = 1;
	size_t rest = wlen(rel), at = 0;
	if (up)		/* past the last ".." and its separator */
		at = (size_t)(r[up - 1].s + 2 - rel);
	while (rel[at] && is_sep(rel[at]))
		at++;
	rest -= at;
	free(r);

	WCHAR *out = malloc((keep + 3 * ups + rest + 2) * sizeof(WCHAR));
	size_t o = 0;
	if (!out)
		return NULL;
	for (size_t i = 0; i < ups; i++) {	/* more ".." than base has */
		out[o++] = '.';
		out[o++] = '.';
		out[o++] = '\\';
	}
	memcpy(out + o, base, keep * sizeof(WCHAR));
	o += keep;
	if (rest) {
		if (o && !is_sep(out[o - 1]))
			out[o++] = '\\';
		memcpy(out + o, rel + at, rest * sizeof(WCHAR));
		o += rest;
	}
	out[o] = 0;
	return out;
}

/* ---- the running object table ---- */

struct rot_entry {
	DWORD cookie;
	IUnknown *obj;
	IMoniker *name;
	FILETIME time;
};

static pthread_mutex_t rot_lock = PTHREAD_RECURSIVE_MUTEX_INITIALIZER_NP;
static struct rot_entry *rot;
static size_t rot_n, rot_cap;
static DWORD rot_next = 1;

/* The entry with this cookie, or NULL; call with rot_lock held. */
static struct rot_entry *rot_by_cookie(DWORD cookie)
{
	for (size_t i = 0; i < rot_n; i++)
		if (rot[i].cookie == cookie)
			return &rot[i];
	return NULL;
}

/* Call with rot_lock held. IsEqual may re-enter the table (the lock is
 * recursive) and revoke or register entries, so each candidate is held
 * by its cookie and a reference, and looked up again after the call. */
static struct rot_entry *rot_find(IMoniker *name)
{
	unsigned restarts = 0;
	for (size_t i = 0; i < rot_n; i++) {
		IMoniker *m = rot[i].name;
		DWORD cookie = rot[i].cookie;
		m->lpVtbl->AddRef(m);
		HRESULT eq = m->lpVtbl->IsEqual(m, name);
		m->lpVtbl->Release(m);
		struct rot_entry *e = rot_by_cookie(cookie);
		if (eq == S_OK && e && e->name == m)
			return e;
		if (!e || e != &rot[i]) {	/* the table moved: start again */
			if (++restarts > 64)
				return NULL;
			i = (size_t)-1;
		}
	}
	return NULL;
}

static HRESULT rot_object(IMoniker *name, IUnknown **obj)
{
	pthread_mutex_lock(&rot_lock);
	struct rot_entry *e = rot_find(name);
	if (e && obj) {
		*obj = e->obj;
		e->obj->lpVtbl->AddRef(e->obj);
	}
	pthread_mutex_unlock(&rot_lock);
	return e ? S_OK : MK_E_UNAVAILABLE;
}

static HRESULT rot_time(IMoniker *name, FILETIME *t)
{
	pthread_mutex_lock(&rot_lock);
	struct rot_entry *e = rot_find(name);
	if (e)
		*t = e->time;
	pthread_mutex_unlock(&rot_lock);
	return e ? S_OK : MK_E_UNAVAILABLE;
}

/* ---- IMoniker ---- */

static struct mk *impl(IMoniker *i)
{
	return (struct mk *)i;
}

WINFN(HRESULT) mk_QueryInterface(IMoniker *self, REFIID riid, void **ppv)
{
	struct mk *m = impl(self);

	if (!ppv)
		return E_POINTER;
	*ppv = NULL;
	if (guid_eq(riid, &IID_IUnknown) || guid_eq(riid, &IID_IMoniker) ||
	    guid_eq(riid, &IID_IPersist) || guid_eq(riid, &IID_IPersistStream))
		*ppv = self;
	else if (guid_eq(riid, &IID_IROTData) && m->kind != K_POINTER)
		*ppv = &m->rotdata;
	else
		return E_NOINTERFACE;
	__atomic_add_fetch(&m->refs, 1, __ATOMIC_SEQ_CST);
	return S_OK;
}

WINFN(ULONG) mk_AddRef(IMoniker *self)
{
	return __atomic_add_fetch(&impl(self)->refs, 1, __ATOMIC_SEQ_CST);
}

WINFN(ULONG) mk_Release(IMoniker *self)
{
	struct mk *m = impl(self);
	ULONG r = __atomic_sub_fetch(&m->refs, 1, __ATOMIC_SEQ_CST);

	if (!r) {
		for (DWORD i = 0; i < m->nparts; i++)
			m->parts[i]->lpVtbl->Release(m->parts[i]);
		free(m->parts);
		if (m->obj)
			m->obj->lpVtbl->Release(m->obj);
		free(m->name);
		free(m->delim);
		free(m);
	}
	return r;
}

WINFN(HRESULT) mk_GetClassID(IMoniker *self, CLSID *clsid)
{
	static const CLSID *const ids[] = {
		[K_FILE] = &clsid_file, [K_ITEM] = &clsid_item,
		[K_ANTI] = &clsid_anti, [K_COMPOSITE] = &clsid_composite,
		[K_CLASS] = &clsid_class, [K_POINTER] = &clsid_pointer,
	};
	if (!clsid)
		return E_POINTER;
	*clsid = *ids[impl(self)->kind];
	return S_OK;
}

WINFN(HRESULT) mk_IsDirty(IMoniker *self)
{
	return S_FALSE;
}

/* An ANSI string as Windows writes it: the length with its NUL, then the
 * bytes; UTF-16 follows the NUL inside the counted length if ANSI loses
 * characters. */
static HRESULT put_string(IStream *stm, const WCHAR *s)
{
	int lossy;
	char *a = w_to_acp(s, &lossy);
	HRESULT hr;

	if (!a)
		return E_OUTOFMEMORY;
	DWORD an = (DWORD)strlen(a) + 1, wn = lossy ? (DWORD)wlen(s) : 0;
	DWORD n = an + wn * sizeof(WCHAR);
	if (SUCCEEDED(hr = stm_write(stm, &n, 4)) &&
	    SUCCEEDED(hr = stm_write(stm, a, an)) && wn)
		hr = stm_write(stm, s, wn * sizeof(WCHAR));
	free(a);
	return hr;
}

static HRESULT get_string(IStream *stm, WCHAR **out)
{
	DWORD n;
	HRESULT hr = stm_read(stm, &n, 4);

	if (FAILED(hr))
		return hr;
	if (!n || n > 0x100000)
		return E_FAIL;
	char *buf = malloc(n);
	if (!buf)
		return E_OUTOFMEMORY;
	if (FAILED(hr = stm_read(stm, buf, n))) {
		free(buf);
		return hr;
	}
	size_t an = strnlen(buf, n);
	size_t after = an < n ? n - an - 1 : 0;
	if (after >= sizeof(WCHAR))
		*out = wdupn((const WCHAR *)(void *)(buf + an + 1),
			     after / sizeof(WCHAR));
	else
		*out = acp_to_w(buf, an);
	free(buf);
	return *out ? S_OK : E_OUTOFMEMORY;
}

/* A file path splits into its leading "..\" parts and the rest. */
static size_t leading_ups(const WCHAR *p, const WCHAR **rest)
{
	size_t up = 0;
	while (p[0] == '.' && p[1] == '.' && (is_sep(p[2]) || !p[2])) {
		up++;
		p += p[2] ? 3 : 2;
	}
	*rest = p;
	return up;
}

static HRESULT save_file(struct mk *m, IStream *stm)
{
	const WCHAR *rest;
	WORD up = (WORD)leading_ups(m->name, &rest);
	int lossy;
	char *a = w_to_acp(rest, &lossy);
	HRESULT hr;

	if (!a)
		return E_OUTOFMEMORY;
	DWORD an = (DWORD)strlen(a) + 1;
	BYTE marker[24] = { 0xff, 0xff, 0xad, 0xde };
	/* endServer: the "\\server" length of a UNC path, else 0xFFFF. */
	if (is_sep(rest[0]) && is_sep(rest[1]) && rest[2]) {
		size_t e = 2;
		while (rest[e] && !is_sep(rest[e]))
			e++;
		if (e < 0xffff) {
			marker[0] = (BYTE)e;
			marker[1] = (BYTE)(e >> 8);
		}
	}
	DWORD wbytes = lossy ? (DWORD)(wlen(rest) * sizeof(WCHAR)) : 0;
	DWORD extra = wbytes ? wbytes + 6 : 0;
	WORD three = 3;
	if (SUCCEEDED(hr = stm_write(stm, &up, 2)) &&
	    SUCCEEDED(hr = stm_write(stm, &an, 4)) &&
	    SUCCEEDED(hr = stm_write(stm, a, an)) &&
	    SUCCEEDED(hr = stm_write(stm, marker, sizeof(marker))) &&
	    SUCCEEDED(hr = stm_write(stm, &extra, 4)) && extra &&
	    SUCCEEDED(hr = stm_write(stm, &wbytes, 4)) &&
	    SUCCEEDED(hr = stm_write(stm, &three, 2)))
		hr = stm_write(stm, rest, wbytes);
	free(a);
	return hr;
}

static HRESULT load_file(struct mk *m, IStream *stm)
{
	WORD up;
	DWORD an, extra, wbytes;
	BYTE marker[24];
	HRESULT hr;
	char *a = NULL;
	WCHAR *rest = NULL;

	if (FAILED(hr = stm_read(stm, &up, 2)) ||
	    FAILED(hr = stm_read(stm, &an, 4)))
		return hr;
	if (!an || an > 0x100000)
		return E_FAIL;
	if (!(a = malloc(an)))
		return E_OUTOFMEMORY;
	if (FAILED(hr = stm_read(stm, a, an)) ||
	    FAILED(hr = stm_read(stm, marker, sizeof(marker))) ||
	    FAILED(hr = stm_read(stm, &extra, 4)))
		goto out;
	/* endServer (marker[0..1]) is the UNC server length or 0xFFFF;
	 * the version that follows is always 0xDEAD. */
	if (marker[2] != 0xad || marker[3] != 0xde) {
		hr = E_FAIL;
		goto out;
	}
	if (extra) {
		WORD three;
		if (extra < 6 || extra > 0x200000 ||
		    FAILED(hr = stm_read(stm, &wbytes, 4)) ||
		    FAILED(hr = stm_read(stm, &three, 2)) ||
		    wbytes != extra - 6) {
			hr = FAILED(hr) ? hr : E_FAIL;
			goto out;
		}
		if (!(rest = malloc(wbytes + sizeof(WCHAR)))) {
			hr = E_OUTOFMEMORY;
			goto out;
		}
		if (FAILED(hr = stm_read(stm, rest, wbytes)))
			goto out;
		rest[wbytes / sizeof(WCHAR)] = 0;
	} else if (!(rest = acp_to_w(a, strnlen(a, an)))) {
		hr = E_OUTOFMEMORY;
		goto out;
	}
	size_t rn = wlen(rest);
	WCHAR *name = malloc((3 * (size_t)up + rn + 1) * sizeof(WCHAR));
	if (!name) {
		hr = E_OUTOFMEMORY;
		goto out;
	}
	size_t o = 0;
	for (WORD i = 0; i < up; i++) {
		name[o++] = '.';
		name[o++] = '.';
		name[o++] = '\\';
	}
	if (up && !rn)
		o--;		/* just "..\.." */
	memcpy(name + o, rest, rn * sizeof(WCHAR));
	name[o + rn] = 0;
	free(m->name);
	m->name = name;
out:
	free(a);
	free(rest);
	return hr;
}

WINFN(HRESULT) mk_Load(IMoniker *self, IStream *stm)
{
	struct mk *m = impl(self);
	HRESULT hr = S_OK;

	if (!stm)
		return E_POINTER;
	switch (m->kind) {
	case K_FILE:
		return load_file(m, stm);
	case K_ITEM: {
		WCHAR *d = NULL, *n = NULL;
		if (SUCCEEDED(hr = get_string(stm, &d)) &&
		    SUCCEEDED(hr = get_string(stm, &n))) {
			free(m->delim);
			free(m->name);
			m->delim = d;
			m->name = n;
		} else {
			free(d);
		}
		return hr;
	}
	case K_ANTI: {
		DWORD c;
		if (FAILED(hr = stm_read(stm, &c, 4)))
			return hr;
		if (!c || c > ANTI_MAX)
			return E_FAIL;
		m->count = c;
		return S_OK;
	}
	case K_CLASS: {
		DWORD extra;
		if (SUCCEEDED(hr = stm_read(stm, &m->clsid, sizeof(CLSID))) &&
		    SUCCEEDED(hr = stm_read(stm, &extra, 4)) && extra) {
			LARGE_INTEGER skip;
			skip.QuadPart = extra;
			hr = stm->lpVtbl->Seek(stm, skip, 1 /* CUR */, NULL);
		}
		return hr;
	}
	case K_COMPOSITE: {
		DWORD n;
		struct list l = { 0 };
		if (FAILED(hr = stm_read(stm, &n, 4)))
			return hr;
		if (n > 0x10000)
			return E_FAIL;
		if (load_depth >= LOAD_DEPTH_MAX)
			return E_FAIL;
		load_depth++;
		for (DWORD i = 0; i < n && SUCCEEDED(hr); i++) {
			IMoniker *p = NULL;
			if (SUCCEEDED(hr = load_from_stream(stm, &p))) {
				if (!parts_of(p, &l))
					hr = E_OUTOFMEMORY;
				p->lpVtbl->Release(p);
			}
		}
		load_depth--;
		if (SUCCEEDED(hr) && l.n < 2)
			hr = E_FAIL;
		if (SUCCEEDED(hr)) {
			for (DWORD i = 0; i < m->nparts; i++)
				m->parts[i]->lpVtbl->Release(m->parts[i]);
			free(m->parts);
			m->parts = l.v;	/* the references move */
			m->nparts = l.n;
			l.v = NULL;
			l.n = 0;
		}
		list_free(&l);
		return hr;
	}
	case K_POINTER:
		break;
	}
	return E_NOTIMPL;
}

WINFN(HRESULT) mk_Save(IMoniker *self, IStream *stm, BOOL clear)
{
	struct mk *m = impl(self);
	HRESULT hr;

	if (!stm)
		return E_POINTER;
	switch (m->kind) {
	case K_FILE:
		return save_file(m, stm);
	case K_ITEM:
		if (FAILED(hr = put_string(stm, m->delim)))
			return hr;
		return put_string(stm, m->name);
	case K_ANTI:
		return stm_write(stm, &m->count, 4);
	case K_CLASS: {
		DWORD zero = 0;
		if (FAILED(hr = stm_write(stm, &m->clsid, sizeof(CLSID))))
			return hr;
		return stm_write(stm, &zero, 4);
	}
	case K_COMPOSITE:
		if (FAILED(hr = stm_write(stm, &m->nparts, 4)))
			return hr;
		for (DWORD i = 0; i < m->nparts; i++)
			if (FAILED(hr = save_to_stream(m->parts[i], stm)))
				return hr;
		return S_OK;
	case K_POINTER:
		break;
	}
	return E_NOTIMPL;
}

WINFN(HRESULT) mk_GetSizeMax(IMoniker *self, ULARGE_INTEGER *size)
{
	struct mk *m = impl(self);
	ULONGLONG n = 0;

	if (!size)
		return E_POINTER;
	switch (m->kind) {
	case K_FILE:
		n = 2 + 4 + 2 * wlen(m->name) + 1 + 24 + 4 + 6 +
		    2 * wlen(m->name);
		break;
	case K_ITEM:
		n = 8 + 3 * (wlen(m->delim) + wlen(m->name)) + 2;
		break;
	case K_ANTI:
		n = 4;
		break;
	case K_CLASS:
		n = sizeof(CLSID) + 4;
		break;
	case K_COMPOSITE:
		n = 4;
		for (DWORD i = 0; i < m->nparts; i++) {
			ULARGE_INTEGER p;
			IMoniker *q = m->parts[i];
			if (FAILED(q->lpVtbl->GetSizeMax(q, &p)))
				return E_FAIL;
			n += sizeof(CLSID) + p.QuadPart;
		}
		break;
	case K_POINTER:
		return E_NOTIMPL;
	}
	size->QuadPart = n;
	return S_OK;
}

/* left∘self, or self if there is no left. */
static HRESULT full_name(IMoniker *self, IMoniker *left, IMoniker **out)
{
	if (!left) {
		self->lpVtbl->AddRef(self);
		*out = self;
		return S_OK;
	}
	return compose(left, self, out);
}

static DWORD bind_context(IBindCtx *bc)
{
	BIND_OPTS2 o;
	memset(&o, 0, sizeof(o));
	o.cbStruct = sizeof(o);
	if (bc && SUCCEEDED(bc->lpVtbl->GetBindOptions(bc, (BIND_OPTS *)&o)) &&
	    o.dwClassContext)
		return o.dwClassContext;
	return CLSCTX_SERVER;
}

static DWORD bind_mode(IBindCtx *bc)
{
	BIND_OPTS o;
	memset(&o, 0, sizeof(o));
	o.cbStruct = sizeof(o);
	if (bc && SUCCEEDED(bc->lpVtbl->GetBindOptions(bc, &o)))
		return o.grfMode;
	return STGM_READWRITE;
}

WINFN(HRESULT) ole_GetClassFile(LPCOLESTR name, CLSID *clsid);

/* A moniker to the left (a class moniker, say) supplies the class
 * factory; with none, the file's own class does. */
static HRESULT bind_file(struct mk *m, IBindCtx *bc, IMoniker *left,
			 REFIID riid, void **ppv)
{
	CLSID clsid;
	IPersistFile *pf;
	HRESULT hr;

	if (left) {
		IClassFactory *cf;
		hr = left->lpVtbl->BindToObject(left, bc, NULL,
						&IID_IClassFactory,
						(void **)&cf);
		if (FAILED(hr))
			return hr;
		hr = cf->lpVtbl->CreateInstance(cf, NULL, &IID_IPersistFile,
						(void **)&pf);
		cf->lpVtbl->Release(cf);
	} else if (SUCCEEDED(hr = ole_GetClassFile(m->name, &clsid))) {
		hr = create_instance(&clsid, bind_context(bc),
				     &IID_IPersistFile, (void **)&pf);
	}
	if (FAILED(hr))
		return hr;
	hr = pf->lpVtbl->Load(pf, m->name, bind_mode(bc));
	if (SUCCEEDED(hr))
		hr = pf->lpVtbl->QueryInterface(pf, riid, ppv);
	pf->lpVtbl->Release(pf);
	return hr;
}

/* The IOleItemContainer to the left of an item moniker. */
static HRESULT item_container(IBindCtx *bc, IMoniker *left,
			      IOleItemContainer **c)
{
	if (!left)
		return E_INVALIDARG;
	return left->lpVtbl->BindToObject(left, bc, NULL,
					  &IID_IOleItemContainer, (void **)c);
}

static DWORD bind_speed(IBindCtx *bc)
{
	BIND_OPTS o;
	memset(&o, 0, sizeof(o));
	o.cbStruct = sizeof(o);
	if (bc && SUCCEEDED(bc->lpVtbl->GetBindOptions(bc, &o)) &&
	    o.dwTickCountDeadline)
		return BINDSPEED_MODERATE;
	return BINDSPEED_INDEFINITE;
}

/* A composite's last part, and the moniker of all before it. */
static HRESULT split_last(struct mk *m, IMoniker *left, IMoniker **rest,
			  IMoniker **last)
{
	IMoniker *before;
	if (m->nparts < 2)	/* a composite not yet loaded */
		return E_UNEXPECTED;
	HRESULT hr = from_parts(m->parts, m->nparts - 1, &before);
	if (FAILED(hr))
		return hr;
	hr = compose(left, before, rest);
	if (before)
		before->lpVtbl->Release(before);
	*last = m->parts[m->nparts - 1];
	return hr;
}

WINFN(HRESULT) mk_BindToObject(IMoniker *self, IBindCtx *bc, IMoniker *left,
			       REFIID riid, void **ppv)
{
	struct mk *m = impl(self);
	IMoniker *full = NULL;
	IUnknown *obj;
	HRESULT hr;

	if (!ppv)
		return E_POINTER;
	*ppv = NULL;
	if (!riid)
		return E_INVALIDARG;
	switch (m->kind) {
	case K_POINTER:
		if (!m->obj)
			return E_UNEXPECTED;
		return m->obj->lpVtbl->QueryInterface(m->obj, riid, ppv);
	case K_CLASS:
		if (left)
			return E_NOTIMPL;
		return class_object(&m->clsid, bind_context(bc), riid, ppv);
	case K_ANTI:
		return E_NOTIMPL;
	default:
		break;
	}

	/* Running already: the object itself. */
	if (FAILED(hr = full_name(self, left, &full)))
		return hr;
	if (full && rot_object(full, &obj) == S_OK) {
		hr = obj->lpVtbl->QueryInterface(obj, riid, ppv);
		obj->lpVtbl->Release(obj);
		full->lpVtbl->Release(full);
		return hr;
	}
	if (full)
		full->lpVtbl->Release(full);

	switch (m->kind) {
	case K_FILE:
		hr = bind_file(m, bc, left, riid, ppv);
		break;
	case K_ITEM: {
		IOleItemContainer *c;
		if (FAILED(hr = item_container(bc, left, &c)))
			return hr;
		hr = c->lpVtbl->GetObject(c, m->name, bind_speed(bc), bc, riid,
					  ppv);
		c->lpVtbl->Release(c);
		break;
	}
	case K_COMPOSITE: {
		IMoniker *rest, *last;
		if (FAILED(hr = split_last(m, left, &rest, &last)))
			return hr;
		hr = last->lpVtbl->BindToObject(last, bc, rest, riid, ppv);
		if (rest)
			rest->lpVtbl->Release(rest);
		break;
	}
	default:
		hr = E_NOTIMPL;
	}
	if (SUCCEEDED(hr) && *ppv && bc)
		bc->lpVtbl->RegisterObjectBound(bc, (IUnknown *)*ppv);
	return hr;
}

WINFN(HRESULT) mk_BindToStorage(IMoniker *self, IBindCtx *bc, IMoniker *left,
				REFIID riid, void **ppv)
{
	struct mk *m = impl(self);
	HRESULT hr;

	if (!ppv)
		return E_POINTER;
	*ppv = NULL;
	switch (m->kind) {
	case K_FILE:
		if (guid_eq(riid, &IID_IStorage)) {
			HRESULT (WINAPI *open)(const WCHAR *, IStorage *, DWORD,
					       void *, DWORD, IStorage **) =
				ole_fn("StgOpenStorage");
			if (!open)
				return E_UNEXPECTED;
			return open(m->name, NULL, bind_mode(bc), NULL, 0,
				    (IStorage **)ppv);
		}
		if (guid_eq(riid, &IID_IStream) ||
		    guid_eq(riid, &IID_ILockBytes))
			return MK_E_NOSTORAGE;
		return E_NOINTERFACE;	/* storage only, never the object */
	case K_ITEM: {
		IOleItemContainer *c;
		if (FAILED(hr = item_container(bc, left, &c)))
			return hr;
		hr = c->lpVtbl->GetObjectStorage(c, m->name, bc, riid, ppv);
		c->lpVtbl->Release(c);
		return hr;
	}
	case K_COMPOSITE: {
		IMoniker *rest, *last;
		if (FAILED(hr = split_last(m, left, &rest, &last)))
			return hr;
		hr = last->lpVtbl->BindToStorage(last, bc, rest, riid, ppv);
		if (rest)
			rest->lpVtbl->Release(rest);
		return hr;
	}
	case K_POINTER:
		return mk_BindToObject(self, bc, left, riid, ppv);
	default:
		return E_NOTIMPL;
	}
}

WINFN(HRESULT) mk_Reduce(IMoniker *self, IBindCtx *bc, DWORD how_far,
			 IMoniker **left_reduced, IMoniker **reduced)
{
	if (!reduced)
		return E_POINTER;
	mk_AddRef(self);
	*reduced = self;
	return MK_S_REDUCED_TO_SELF;
}

WINFN(HRESULT) mk_ComposeWith(IMoniker *self, IMoniker *right, BOOL only,
			      IMoniker **out)
{
	struct mk *m = impl(self), *r = ours(right);

	if (!out)
		return E_POINTER;
	*out = NULL;
	if (!right) {
		mk_AddRef(self);
		*out = self;
		return S_OK;
	}
	if (m->kind == K_COMPOSITE || (r && r->kind == K_COMPOSITE))
		return compose(self, right, out);
	if (r && r->kind == K_ANTI) {
		if (m->kind == K_ANTI)
			return m->count + (uint64_t)r->count > ANTI_MAX ?
				E_FAIL : anti_new(m->count + r->count, out);
		return r->count > 1 ? anti_new(r->count - 1, out) : S_OK;
	}
	if (m->kind == K_FILE && r && r->kind == K_FILE) {
		if (is_absolute(r->name))
			return MK_E_SYNTAX;
		WCHAR *p = join(m->name, r->name);
		if (!p)
			return E_OUTOFMEMORY;
		HRESULT hr = file_new(p, wlen(p), out);
		free(p);
		return hr;
	}
	if (only)
		return MK_E_NEEDGENERIC;
	return compose(self, right, out);
}

/* ---- IEnumMoniker over a snapshot ---- */

struct enm {
	IEnumMoniker iface;
	LONG refs;
	struct list l;
	DWORD pos;
};

static const IEnumMonikerVtbl enm_vtbl;

static HRESULT enm_new(IMoniker *const *v, DWORD n, int reverse,
		       IEnumMoniker **out)
{
	struct enm *e = calloc(1, sizeof(*e));
	if (!e)
		return E_OUTOFMEMORY;
	e->iface.lpVtbl = &enm_vtbl;
	e->refs = 1;
	for (DWORD i = 0; i < n; i++)
		if (!list_add(&e->l, v[reverse ? n - 1 - i : i])) {
			list_free(&e->l);
			free(e);
			return E_OUTOFMEMORY;
		}
	*out = &e->iface;
	return S_OK;
}

WINFN(HRESULT) enm_QueryInterface(IEnumMoniker *self, REFIID riid,
				  void **ppv)
{
	if (!ppv)
		return E_POINTER;
	if (!guid_eq(riid, &IID_IUnknown) && !guid_eq(riid, &IID_IEnumMoniker)) {
		*ppv = NULL;
		return E_NOINTERFACE;
	}
	*ppv = self;
	__atomic_add_fetch(&((struct enm *)self)->refs, 1, __ATOMIC_SEQ_CST);
	return S_OK;
}

WINFN(ULONG) enm_AddRef(IEnumMoniker *self)
{
	return __atomic_add_fetch(&((struct enm *)self)->refs, 1,
				  __ATOMIC_SEQ_CST);
}

WINFN(ULONG) enm_Release(IEnumMoniker *self)
{
	struct enm *e = (struct enm *)self;
	ULONG r = __atomic_sub_fetch(&e->refs, 1, __ATOMIC_SEQ_CST);
	if (!r) {
		list_free(&e->l);
		free(e);
	}
	return r;
}

WINFN(HRESULT) enm_Next(IEnumMoniker *self, ULONG count, IMoniker **out,
			ULONG *fetched)
{
	struct enm *e = (struct enm *)self;
	ULONG n = 0;

	if (!out || (count > 1 && !fetched))
		return E_POINTER;
	while (n < count && e->pos < e->l.n) {
		out[n] = e->l.v[e->pos++];
		out[n]->lpVtbl->AddRef(out[n]);
		n++;
	}
	if (fetched)
		*fetched = n;
	return n == count ? S_OK : S_FALSE;
}

WINFN(HRESULT) enm_Skip(IEnumMoniker *self, ULONG count)
{
	struct enm *e = (struct enm *)self;
	DWORD left = e->l.n - e->pos;
	e->pos += count < left ? count : left;
	return count <= left ? S_OK : S_FALSE;
}

WINFN(HRESULT) enm_Reset(IEnumMoniker *self)
{
	((struct enm *)self)->pos = 0;
	return S_OK;
}

WINFN(HRESULT) enm_Clone(IEnumMoniker *self, IEnumMoniker **out)
{
	struct enm *e = (struct enm *)self;
	if (!out)
		return E_POINTER;
	HRESULT hr = enm_new(e->l.v, e->l.n, 0, out);
	if (SUCCEEDED(hr))
		((struct enm *)*out)->pos = e->pos;
	return hr;
}

static const IEnumMonikerVtbl enm_vtbl = {
	enm_QueryInterface, enm_AddRef, enm_Release, enm_Next, enm_Skip,
	enm_Reset, enm_Clone,
};

WINFN(HRESULT) mk_Enum(IMoniker *self, BOOL forward, IEnumMoniker **e)
{
	struct mk *m = impl(self);

	if (!e)
		return E_POINTER;
	*e = NULL;
	if (m->kind != K_COMPOSITE)
		return S_OK;
	return enm_new(m->parts, m->nparts, !forward, e);
}

WINFN(HRESULT) mk_IsEqual(IMoniker *self, IMoniker *other)
{
	struct mk *m = impl(self), *o = ours(other);

	if (!other)
		return E_INVALIDARG;
	if (!o || o->kind != m->kind)
		return S_FALSE;
	switch (m->kind) {
	case K_FILE:
	case K_ITEM:	/* Windows compares the item alone, not its delimiter */
		return wieq(m->name, o->name) ? S_OK : S_FALSE;
	case K_ANTI:
		return m->count == o->count ? S_OK : S_FALSE;
	case K_CLASS:
		return guid_eq(&m->clsid, &o->clsid) ? S_OK : S_FALSE;
	case K_POINTER:
		return m->obj == o->obj ? S_OK : S_FALSE;
	case K_COMPOSITE:
		if (m->nparts != o->nparts)
			return S_FALSE;
		for (DWORD i = 0; i < m->nparts; i++) {
			IMoniker *p = m->parts[i];
			if (p->lpVtbl->IsEqual(p, o->parts[i]) != S_OK)
				return S_FALSE;
		}
		return S_OK;
	}
	return S_FALSE;
}

static DWORD hash_name(const WCHAR *s)
{
	DWORD h = 0;
	for (; s && *s; s++)
		h = h * 31 + wup(*s);
	return h;
}

WINFN(HRESULT) mk_Hash(IMoniker *self, DWORD *hash)
{
	struct mk *m = impl(self);
	DWORD h = 0;

	if (!hash)
		return E_POINTER;
	switch (m->kind) {
	case K_FILE:
	case K_ITEM:
		h = hash_name(m->name);
		break;
	case K_ANTI:
		h = 0x80000000u | m->count;
		break;
	case K_CLASS:
		h = m->clsid.Data1;
		break;
	case K_POINTER:
		h = (DWORD)(uintptr_t)m->obj;
		break;
	case K_COMPOSITE:
		for (DWORD i = 0; i < m->nparts; i++) {
			DWORD p = 0;
			IMoniker *q = m->parts[i];
			q->lpVtbl->Hash(q, &p);
			h ^= p;
		}
		break;
	}
	*hash = h;
	return S_OK;
}

WINFN(HRESULT) mk_IsRunning(IMoniker *self, IBindCtx *bc, IMoniker *left,
			    IMoniker *newly)
{
	struct mk *m = impl(self);
	IMoniker *full;
	HRESULT hr;

	switch (m->kind) {
	case K_POINTER:
		return S_OK;
	case K_CLASS:
	case K_ANTI:
		return E_NOTIMPL;
	default:
		break;
	}
	if (FAILED(hr = full_name(self, left, &full)))
		return hr;
	hr = S_FALSE;
	if (full && newly && full->lpVtbl->IsEqual(full, newly) == S_OK)
		hr = S_OK;
	else if (full && rot_object(full, NULL) == S_OK)
		hr = S_OK;
	if (full)
		full->lpVtbl->Release(full);
	if (hr == S_FALSE && m->kind == K_COMPOSITE) {
		/* The last part, with everything before it as its left. */
		IMoniker *rest, *last;
		if (FAILED(hr = split_last(m, left, &rest, &last)))
			return hr;
		hr = last->lpVtbl->IsRunning(last, bc, rest, newly);
		if (rest)
			rest->lpVtbl->Release(rest);
		return hr;
	}
	if (hr == S_FALSE && m->kind == K_ITEM && left &&
	    left->lpVtbl->IsRunning(left, bc, NULL, NULL) == S_OK) {
		IOleItemContainer *c;
		if (SUCCEEDED(item_container(bc, left, &c))) {
			hr = c->lpVtbl->IsRunning(c, m->name);
			c->lpVtbl->Release(c);
		}
	}
	return hr;
}

WINFN(HRESULT) mk_GetTimeOfLastChange(IMoniker *self, IBindCtx *bc,
				      IMoniker *left, FILETIME *t)
{
	struct mk *m = impl(self);
	IMoniker *full;
	HRESULT hr;

	if (!t)
		return E_POINTER;
	if (m->kind == K_POINTER || m->kind == K_CLASS || m->kind == K_ANTI)
		return E_NOTIMPL;
	if (FAILED(hr = full_name(self, left, &full)))
		return hr;
	hr = full ? rot_time(full, t) : MK_E_UNAVAILABLE;
	if (full)
		full->lpVtbl->Release(full);
	if (hr == S_OK)
		return hr;
	if (m->kind == K_FILE) {
		char *u = unix_path_w(m->name);
		struct stat st;
		hr = u && !stat(u, &st) ? S_OK : MK_E_NOOBJECT;
		if (hr == S_OK)
			unix_time(st.st_mtime, t);
		free(u);
	}
	return hr;
}

WINFN(HRESULT) mk_Inverse(IMoniker *self, IMoniker **out)
{
	struct mk *m = impl(self);

	if (!out)
		return E_POINTER;
	*out = NULL;
	switch (m->kind) {
	case K_ANTI:
		return MK_E_NOINVERSE;
	case K_COMPOSITE: {
		IMoniker *acc = NULL;
		HRESULT hr = S_OK;
		for (DWORD i = m->nparts; i-- && SUCCEEDED(hr);) {
			IMoniker *p = m->parts[i], *inv, *next;
			if (FAILED(hr = p->lpVtbl->Inverse(p, &inv)))
				break;
			hr = compose(acc, inv, &next);
			inv->lpVtbl->Release(inv);
			if (acc)
				acc->lpVtbl->Release(acc);
			acc = next;
		}
		if (FAILED(hr)) {
			if (acc)
				acc->lpVtbl->Release(acc);
			return hr;
		}
		*out = acc;
		return S_OK;
	}
	default:
		return anti_new(1, out);
	}
}

/* The file moniker of the first n parts of path. */
static HRESULT file_prefix(const WCHAR *path, const struct comp *c, size_t n,
			   IMoniker **out)
{
	size_t len = (size_t)(c[n - 1].s + c[n - 1].n - path);
	return file_new(path, len, out);
}

WINFN(HRESULT) mk_CommonPrefixWith(IMoniker *self, IMoniker *other,
				   IMoniker **out)
{
	struct mk *m = impl(self), *o = ours(other);
	HRESULT hr;

	if (!out || !other)
		return E_INVALIDARG;
	*out = NULL;
	if (mk_IsEqual(self, other) == S_OK) {
		mk_AddRef(self);
		*out = self;
		return MK_S_US;
	}
	if (m->kind == K_FILE && o && o->kind == K_FILE) {
		struct comp *a, *b;
		size_t na = split(m->name, &a), nb = split(o->name, &b), k = 0;
		while (k < na && k < nb && a[k].n == b[k].n &&
		       wieqn(a[k].s, b[k].s, a[k].n))
			k++;
		if (!k)
			hr = MK_E_NOPREFIX;
		else if (k == na) {
			mk_AddRef(self);
			*out = self;
			hr = k == nb ? MK_S_US : MK_S_ME;
		} else if (k == nb) {
			other->lpVtbl->AddRef(other);
			*out = other;
			hr = MK_S_HIM;
		} else {
			hr = file_prefix(m->name, a, k, out);
		}
		free(a);
		free(b);
		return hr;
	}
	if (m->kind != K_COMPOSITE && !(o && o->kind == K_COMPOSITE))
		return MK_E_NOPREFIX;

	struct list a = { 0 }, b = { 0 };
	if (!parts_of(self, &a) || !parts_of(other, &b)) {
		hr = E_OUTOFMEMORY;
		goto out;
	}
	DWORD k = 0;
	while (k < a.n && k < b.n &&
	       a.v[k]->lpVtbl->IsEqual(a.v[k], b.v[k]) == S_OK)
		k++;
	if (!k) {
		hr = MK_E_NOPREFIX;
	} else if (k == a.n) {
		mk_AddRef(self);
		*out = self;
		hr = k == b.n ? MK_S_US : MK_S_ME;
	} else if (k == b.n) {
		other->lpVtbl->AddRef(other);
		*out = other;
		hr = MK_S_HIM;
	} else {
		hr = from_parts(a.v, k, out);
	}
out:
	list_free(&a);
	list_free(&b);
	return hr;
}

WINFN(HRESULT) mk_RelativePathTo(IMoniker *self, IMoniker *other,
				 IMoniker **out)
{
	struct mk *m = impl(self), *o = ours(other);
	HRESULT hr;

	if (!out || !other)
		return E_INVALIDARG;
	*out = NULL;
	if (m->kind == K_FILE && o && o->kind == K_FILE) {
		struct comp *a, *b;
		size_t na = split(m->name, &a), nb = split(o->name, &b), k = 0;
		while (k < na && k < nb && a[k].n == b[k].n &&
		       wieqn(a[k].s, b[k].s, a[k].n))
			k++;
		if (!k) {
			free(a);
			free(b);
			other->lpVtbl->AddRef(other);
			*out = other;
			return MK_S_HIM;
		}
		size_t up = na - k, rest = 0;
		for (size_t i = k; i < nb; i++)
			rest += b[i].n + 1;
		WCHAR *p = malloc((3 * up + rest + 1) * sizeof(WCHAR));
		size_t at = 0;
		if (p) {
			for (size_t i = 0; i < up; i++) {
				p[at++] = '.';
				p[at++] = '.';
				p[at++] = '\\';
			}
			for (size_t i = k; i < nb; i++) {
				memcpy(p + at, b[i].s, b[i].n * sizeof(WCHAR));
				at += b[i].n;
				p[at++] = '\\';
			}
			if (at)
				at--;	/* the last separator */
			p[at] = 0;
		}
		free(a);
		free(b);
		if (!p)
			return E_OUTOFMEMORY;
		hr = file_new(p, at, out);
		free(p);
		return hr;
	}
	if (m->kind != K_COMPOSITE && !(o && o->kind == K_COMPOSITE)) {
		other->lpVtbl->AddRef(other);
		*out = other;
		return MK_S_HIM;
	}

	struct list a = { 0 }, b = { 0 };
	IMoniker *tail = NULL, *inv = NULL, *head = NULL;
	if (!parts_of(self, &a) || !parts_of(other, &b)) {
		hr = E_OUTOFMEMORY;
		goto out;
	}
	DWORD k = 0;
	while (k < a.n && k < b.n &&
	       a.v[k]->lpVtbl->IsEqual(a.v[k], b.v[k]) == S_OK)
		k++;
	if (!k) {
		other->lpVtbl->AddRef(other);
		*out = other;
		hr = MK_S_HIM;
		goto out;
	}
	if (FAILED(hr = from_parts(a.v + k, a.n - k, &tail)) ||
	    (tail && FAILED(hr = tail->lpVtbl->Inverse(tail, &inv))) ||
	    FAILED(hr = from_parts(b.v + k, b.n - k, &head)))
		goto out;
	hr = compose(inv, head, out);
out:
	if (tail)
		tail->lpVtbl->Release(tail);
	if (inv)
		inv->lpVtbl->Release(inv);
	if (head)
		head->lpVtbl->Release(head);
	list_free(&a);
	list_free(&b);
	return hr;
}

/* The display name, malloc()ed. */
static HRESULT display_name(IMoniker *mon, IBindCtx *bc, WCHAR **out)
{
	struct mk *m = ours(mon);

	*out = NULL;
	if (!m) {
		LPOLESTR s = NULL;
		HRESULT hr = mon->lpVtbl->GetDisplayName(mon, bc, NULL, &s);
		if (SUCCEEDED(hr)) {
			*out = wdup(s);
			ole_taskmem_free(s);
			if (!*out)
				hr = E_OUTOFMEMORY;
		}
		return hr;
	}
	switch (m->kind) {
	case K_FILE:
		*out = wdup(m->name);
		break;
	case K_ITEM: {
		size_t a = wlen(m->delim), b = wlen(m->name);
		if ((*out = malloc((a + b + 1) * sizeof(WCHAR)))) {
			memcpy(*out, m->delim, a * sizeof(WCHAR));
			memcpy(*out + a, m->name, (b + 1) * sizeof(WCHAR));
		}
		break;
	}
	case K_ANTI:
		if (m->count > ANTI_MAX)
			return E_FAIL;
		if ((*out = malloc((3 * (size_t)m->count + 1) *
				   sizeof(WCHAR)))) {
			for (DWORD i = 0; i < m->count; i++) {
				(*out)[3 * i] = '\\';
				(*out)[3 * i + 1] = '.';
				(*out)[3 * i + 2] = '.';
			}
			(*out)[3 * m->count] = 0;
		}
		break;
	case K_CLASS: {
		char g[OLE_GUID_CHARS], s[64];
		ole_guid_format(&m->clsid, g);
		g[37] = 0;
		snprintf(s, sizeof(s), "clsid:%s:", g + 1);
		*out = wascii(s);
		break;
	}
	case K_COMPOSITE: {
		WCHAR *acc = wdup((const WCHAR[]){ 0 });
		for (DWORD i = 0; acc && i < m->nparts; i++) {
			WCHAR *p;
			HRESULT hr = display_name(m->parts[i], bc, &p);
			if (FAILED(hr)) {
				free(acc);
				return hr;
			}
			size_t a = wlen(acc), b = wlen(p);
			WCHAR *n = realloc(acc, (a + b + 1) * sizeof(WCHAR));
			if (n)
				memcpy(n + a, p, (b + 1) * sizeof(WCHAR));
			else
				free(acc);
			acc = n;
			free(p);
		}
		*out = acc;
		break;
	}
	case K_POINTER:
		return E_NOTIMPL;
	}
	return *out ? S_OK : E_OUTOFMEMORY;
}

WINFN(HRESULT) mk_GetDisplayName(IMoniker *self, IBindCtx *bc,
				 IMoniker *left, LPOLESTR *name)
{
	WCHAR *s;
	HRESULT hr;

	if (!name)
		return E_POINTER;
	*name = NULL;
	if (FAILED(hr = display_name(self, bc, &s)))
		return hr;
	*name = task_wdup(s);
	free(s);
	return *name ? S_OK : E_OUTOFMEMORY;
}

WINFN(HRESULT) mk_ParseDisplayName(IMoniker *self, IBindCtx *bc,
				   IMoniker *left, LPOLESTR name, ULONG *eaten,
				   IMoniker **out)
{
	IParseDisplayName *p;
	IMoniker *full;
	HRESULT hr;

	if (!eaten || !out || !name)
		return E_POINTER;
	*out = NULL;
	*eaten = 0;
	if (FAILED(hr = full_name(self, left, &full)))
		return hr;
	hr = full->lpVtbl->BindToObject(full, bc, NULL, &IID_IParseDisplayName,
					(void **)&p);
	full->lpVtbl->Release(full);
	if (FAILED(hr))
		return hr;
	hr = p->lpVtbl->ParseDisplayName(p, bc, name, eaten, out);
	p->lpVtbl->Release(p);
	return hr;
}

WINFN(HRESULT) mk_IsSystemMoniker(IMoniker *self, DWORD *mksys)
{
	static const DWORD sys[] = {
		[K_FILE] = MKSYS_FILEMONIKER, [K_ITEM] = MKSYS_ITEMMONIKER,
		[K_ANTI] = MKSYS_ANTIMONIKER,
		[K_COMPOSITE] = MKSYS_GENERICCOMPOSITE,
		[K_CLASS] = MKSYS_CLASSMONIKER,
		[K_POINTER] = MKSYS_POINTERMONIKER,
	};
	if (!mksys)
		return E_POINTER;
	*mksys = sys[impl(self)->kind];
	return S_OK;
}

static const IMonikerVtbl mk_vtbl = {
	mk_QueryInterface, mk_AddRef, mk_Release, mk_GetClassID, mk_IsDirty,
	mk_Load, mk_Save, mk_GetSizeMax, mk_BindToObject, mk_BindToStorage,
	mk_Reduce, mk_ComposeWith, mk_Enum, mk_IsEqual, mk_Hash, mk_IsRunning,
	mk_GetTimeOfLastChange, mk_Inverse, mk_CommonPrefixWith,
	mk_RelativePathTo, mk_GetDisplayName, mk_ParseDisplayName,
	mk_IsSystemMoniker,
};

/* ---- IROTData: the class and the upper-case display name ---- */

static struct mk *from_rotdata(IROTData *i)
{
	return (struct mk *)((char *)i - offsetof(struct mk, rotdata));
}

WINFN(HRESULT) rd_QueryInterface(IROTData *self, REFIID riid, void **ppv)
{
	return mk_QueryInterface(&from_rotdata(self)->iface, riid, ppv);
}

WINFN(ULONG) rd_AddRef(IROTData *self)
{
	return mk_AddRef(&from_rotdata(self)->iface);
}

WINFN(ULONG) rd_Release(IROTData *self)
{
	return mk_Release(&from_rotdata(self)->iface);
}

WINFN(HRESULT) rd_GetComparisonData(IROTData *self, BYTE *data, ULONG max,
				    ULONG *size)
{
	struct mk *m = from_rotdata(self);
	WCHAR *s;
	CLSID clsid;
	HRESULT hr;

	if (!data || !size)
		return E_POINTER;
	if (FAILED(hr = display_name(&m->iface, NULL, &s)))
		return hr;
	mk_GetClassID(&m->iface, &clsid);
	size_t n = wlen(s);
	ULONG need = (ULONG)(sizeof(CLSID) + (n + 1) * sizeof(WCHAR));
	*size = need;
	if (need > max) {
		free(s);
		return E_OUTOFMEMORY;
	}
	for (size_t i = 0; i < n; i++)
		s[i] = wup(s[i]);
	memcpy(data, &clsid, sizeof(clsid));
	memcpy(data + sizeof(clsid), s, (n + 1) * sizeof(WCHAR));
	free(s);
	return S_OK;
}

static const IROTDataVtbl rotdata_vtbl = {
	rd_QueryInterface, rd_AddRef, rd_Release, rd_GetComparisonData,
};

/* ---- class objects, for OleLoadFromStream ---- */

struct factory {
	IClassFactory iface;
	enum kind kind;
};

WINFN(HRESULT) cf_QueryInterface(IClassFactory *self, REFIID riid, void **ppv)
{
	if (!ppv)
		return E_POINTER;
	if (!guid_eq(riid, &IID_IUnknown) &&
	    !guid_eq(riid, &IID_IClassFactory)) {
		*ppv = NULL;
		return E_NOINTERFACE;
	}
	*ppv = self;
	return S_OK;
}

WINFN(ULONG) cf_AddRef(IClassFactory *self)
{
	return 2;
}

WINFN(ULONG) cf_Release(IClassFactory *self)
{
	return 1;
}

WINFN(HRESULT) cf_CreateInstance(IClassFactory *self, IUnknown *outer,
				 REFIID riid, void **ppv)
{
	struct factory *f = (struct factory *)self;
	IMoniker *m = NULL;
	HRESULT hr;

	if (!ppv)
		return E_POINTER;
	*ppv = NULL;
	if (outer)
		return CLASS_E_NOAGGREGATION;
	static const WCHAR empty[1];
	switch (f->kind) {
	case K_FILE:
		hr = file_new(empty, 0, &m);
		break;
	case K_ITEM:
		hr = item_new(empty, empty, &m);
		break;
	case K_ANTI:
		hr = anti_new(1, &m);
		break;
	case K_CLASS:
		hr = class_new(&(CLSID){ 0 }, &m);
		break;
	case K_COMPOSITE: {
		struct mk *c = mk_new(K_COMPOSITE);
		hr = c ? S_OK : E_OUTOFMEMORY;
		if (c)
			m = &c->iface;
		break;
	}
	default:
		hr = pointer_new(NULL, &m);
		break;
	}
	if (FAILED(hr))
		return hr;
	hr = mk_QueryInterface(m, riid, ppv);
	mk_Release(m);
	return hr;
}

WINFN(HRESULT) cf_LockServer(IClassFactory *self, BOOL lock)
{
	return S_OK;
}

static const IClassFactoryVtbl cf_vtbl = {
	cf_QueryInterface, cf_AddRef, cf_Release, cf_CreateInstance,
	cf_LockServer,
};

static struct factory factories[] = {
	{ { &cf_vtbl }, K_FILE }, { { &cf_vtbl }, K_ITEM },
	{ { &cf_vtbl }, K_ANTI }, { { &cf_vtbl }, K_COMPOSITE },
	{ { &cf_vtbl }, K_CLASS }, { { &cf_vtbl }, K_POINTER },
};

HRESULT ole_moniker_class_object(REFCLSID clsid, REFIID riid, void **ppv)
{
	static const CLSID *const ids[] = {
		&clsid_file, &clsid_item, &clsid_anti, &clsid_composite,
		&clsid_class, &clsid_pointer,
	};
	for (size_t i = 0; i < sizeof(ids) / sizeof(*ids); i++)
		if (guid_eq(clsid, ids[i]))
			return cf_QueryInterface(&factories[i].iface, riid,
						 ppv);
	return REGDB_E_CLASSNOTREG;
}

/* ---- IEnumString over the bind context's keys ---- */

struct ens {
	IEnumString iface;
	LONG refs;
	WCHAR **v;
	DWORD n, pos;
};

static const IEnumStringVtbl ens_vtbl;

static HRESULT ens_new(WCHAR *const *keys, DWORD n, IEnumString **out)
{
	struct ens *e = calloc(1, sizeof(*e));
	if (!e || (n && !(e->v = calloc(n, sizeof(*e->v))))) {
		free(e);
		return E_OUTOFMEMORY;
	}
	e->iface.lpVtbl = &ens_vtbl;
	e->refs = 1;
	for (DWORD i = 0; i < n; i++) {
		if (!(e->v[i] = wdup(keys[i]))) {
			for (DWORD j = 0; j < i; j++)
				free(e->v[j]);
			free(e->v);
			free(e);
			return E_OUTOFMEMORY;
		}
	}
	e->n = n;
	*out = &e->iface;
	return S_OK;
}

WINFN(HRESULT) ens_QueryInterface(IEnumString *self, REFIID riid, void **ppv)
{
	if (!ppv)
		return E_POINTER;
	if (!guid_eq(riid, &IID_IUnknown) && !guid_eq(riid, &IID_IEnumString)) {
		*ppv = NULL;
		return E_NOINTERFACE;
	}
	*ppv = self;
	__atomic_add_fetch(&((struct ens *)self)->refs, 1, __ATOMIC_SEQ_CST);
	return S_OK;
}

WINFN(ULONG) ens_AddRef(IEnumString *self)
{
	return __atomic_add_fetch(&((struct ens *)self)->refs, 1,
				  __ATOMIC_SEQ_CST);
}

WINFN(ULONG) ens_Release(IEnumString *self)
{
	struct ens *e = (struct ens *)self;
	ULONG r = __atomic_sub_fetch(&e->refs, 1, __ATOMIC_SEQ_CST);
	if (!r) {
		for (DWORD i = 0; i < e->n; i++)
			free(e->v[i]);
		free(e->v);
		free(e);
	}
	return r;
}

WINFN(HRESULT) ens_Next(IEnumString *self, ULONG count, LPOLESTR *out,
			ULONG *fetched)
{
	struct ens *e = (struct ens *)self;
	ULONG n = 0;

	if (!out || (count > 1 && !fetched))
		return E_POINTER;
	while (n < count && e->pos < e->n) {
		if (!(out[n] = task_wdup(e->v[e->pos]))) {
			while (n)
				ole_taskmem_free(out[--n]);
			if (fetched)
				*fetched = 0;
			return E_OUTOFMEMORY;
		}
		e->pos++;
		n++;
	}
	if (fetched)
		*fetched = n;
	return n == count ? S_OK : S_FALSE;
}

WINFN(HRESULT) ens_Skip(IEnumString *self, ULONG count)
{
	struct ens *e = (struct ens *)self;
	DWORD left = e->n - e->pos;
	e->pos += count < left ? count : left;
	return count <= left ? S_OK : S_FALSE;
}

WINFN(HRESULT) ens_Reset(IEnumString *self)
{
	((struct ens *)self)->pos = 0;
	return S_OK;
}

WINFN(HRESULT) ens_Clone(IEnumString *self, IEnumString **out)
{
	struct ens *e = (struct ens *)self;
	if (!out)
		return E_POINTER;
	HRESULT hr = ens_new(e->v, e->n, out);
	if (SUCCEEDED(hr))
		((struct ens *)*out)->pos = e->pos;
	return hr;
}

static const IEnumStringVtbl ens_vtbl = {
	ens_QueryInterface, ens_AddRef, ens_Release, ens_Next, ens_Skip,
	ens_Reset, ens_Clone,
};

/* ---- IBindCtx ---- */

struct param {
	WCHAR *key;
	IUnknown *obj;
};

struct bc {
	IBindCtx iface;
	LONG refs;
	BIND_OPTS2 opts;
	IUnknown **bound;
	DWORD nbound, capbound;
	struct param *params;
	DWORD nparams, capparams;
};

static struct bc *bc_impl(IBindCtx *i)
{
	return (struct bc *)i;
}

WINFN(HRESULT) bc_QueryInterface(IBindCtx *self, REFIID riid, void **ppv)
{
	if (!ppv)
		return E_POINTER;
	if (!guid_eq(riid, &IID_IUnknown) && !guid_eq(riid, &IID_IBindCtx)) {
		*ppv = NULL;
		return E_NOINTERFACE;
	}
	*ppv = self;
	__atomic_add_fetch(&bc_impl(self)->refs, 1, __ATOMIC_SEQ_CST);
	return S_OK;
}

WINFN(ULONG) bc_AddRef(IBindCtx *self)
{
	return __atomic_add_fetch(&bc_impl(self)->refs, 1, __ATOMIC_SEQ_CST);
}

WINFN(HRESULT) bc_ReleaseBoundObjects(IBindCtx *self)
{
	struct bc *b = bc_impl(self);
	while (b->nbound) {
		IUnknown *o = b->bound[--b->nbound];
		o->lpVtbl->Release(o);
	}
	return S_OK;
}

WINFN(ULONG) bc_Release(IBindCtx *self)
{
	struct bc *b = bc_impl(self);
	ULONG r = __atomic_sub_fetch(&b->refs, 1, __ATOMIC_SEQ_CST);

	if (!r) {
		bc_ReleaseBoundObjects(self);
		free(b->bound);
		for (DWORD i = 0; i < b->nparams; i++) {
			free(b->params[i].key);
			b->params[i].obj->lpVtbl->Release(b->params[i].obj);
		}
		free(b->params);
		free(b);
	}
	return r;
}

WINFN(HRESULT) bc_RegisterObjectBound(IBindCtx *self, IUnknown *obj)
{
	struct bc *b = bc_impl(self);

	if (!obj)
		return E_INVALIDARG;
	if (b->nbound == b->capbound) {
		DWORD cap = b->capbound ? b->capbound * 2 : 4;
		IUnknown **v = realloc(b->bound, cap * sizeof(*v));
		if (!v)
			return E_OUTOFMEMORY;
		b->bound = v;
		b->capbound = cap;
	}
	obj->lpVtbl->AddRef(obj);
	b->bound[b->nbound++] = obj;
	return S_OK;
}

WINFN(HRESULT) bc_RevokeObjectBound(IBindCtx *self, IUnknown *obj)
{
	struct bc *b = bc_impl(self);

	if (!obj)
		return E_INVALIDARG;
	for (DWORD i = b->nbound; i--;)
		if (b->bound[i] == obj) {
			memmove(b->bound + i, b->bound + i + 1,
				(b->nbound - i - 1) * sizeof(*b->bound));
			b->nbound--;
			obj->lpVtbl->Release(obj);
			return S_OK;
		}
	return MK_E_NOTBOUND;
}

WINFN(HRESULT) bc_SetBindOptions(IBindCtx *self, BIND_OPTS *opts)
{
	struct bc *b = bc_impl(self);

	if (!opts)
		return E_INVALIDARG;
	if (opts->cbStruct < sizeof(BIND_OPTS))
		return E_INVALIDARG;
	DWORD n = opts->cbStruct < sizeof(b->opts) ? opts->cbStruct :
						     sizeof(b->opts);
	memcpy(&b->opts, opts, n);
	b->opts.cbStruct = sizeof(b->opts);
	return S_OK;
}

WINFN(HRESULT) bc_GetBindOptions(IBindCtx *self, BIND_OPTS *opts)
{
	struct bc *b = bc_impl(self);

	if (!opts)
		return E_POINTER;
	DWORD want = opts->cbStruct;
	if (want < sizeof(BIND_OPTS))
		return E_INVALIDARG;
	memcpy(opts, &b->opts, want < sizeof(b->opts) ? want : sizeof(b->opts));
	opts->cbStruct = want;
	return S_OK;
}

WINFN(HRESULT) ole_GetRunningObjectTable(DWORD reserved,
					 IRunningObjectTable **out);

WINFN(HRESULT) bc_GetRunningObjectTable(IBindCtx *self,
					IRunningObjectTable **out)
{
	if (!out)
		return E_POINTER;
	return ole_GetRunningObjectTable(0, out);
}

static struct param *find_param(struct bc *b, const WCHAR *key)
{
	for (DWORD i = 0; i < b->nparams; i++)
		if (wlen(b->params[i].key) == wlen(key) &&
		    !memcmp(b->params[i].key, key, wlen(key) * sizeof(WCHAR)))
			return &b->params[i];
	return NULL;
}

WINFN(HRESULT) bc_RegisterObjectParam(IBindCtx *self, LPOLESTR key,
				      IUnknown *obj)
{
	struct bc *b = bc_impl(self);

	if (!key || !obj)
		return E_INVALIDARG;
	struct param *p = find_param(b, key);
	if (p) {	/* publish the new object, then let the old go */
		IUnknown *old = p->obj;
		obj->lpVtbl->AddRef(obj);
		p->obj = obj;
		old->lpVtbl->Release(old);
		return S_OK;
	}
	if (b->nparams == b->capparams) {
		DWORD cap = b->capparams ? b->capparams * 2 : 4;
		struct param *v = realloc(b->params, cap * sizeof(*v));
		if (!v)
			return E_OUTOFMEMORY;
		b->params = v;
		b->capparams = cap;
	}
	WCHAR *k = wdup(key);
	if (!k)
		return E_OUTOFMEMORY;
	obj->lpVtbl->AddRef(obj);
	b->params[b->nparams].key = k;
	b->params[b->nparams].obj = obj;
	b->nparams++;
	return S_OK;
}

WINFN(HRESULT) bc_GetObjectParam(IBindCtx *self, LPOLESTR key,
				 IUnknown **obj)
{
	if (!obj)
		return E_POINTER;
	*obj = NULL;
	if (!key)
		return E_INVALIDARG;
	struct param *p = find_param(bc_impl(self), key);
	if (!p)
		return E_FAIL;
	p->obj->lpVtbl->AddRef(p->obj);
	*obj = p->obj;
	return S_OK;
}

WINFN(HRESULT) bc_EnumObjectParam(IBindCtx *self, IEnumString **out)
{
	struct bc *b = bc_impl(self);
	WCHAR **keys;
	HRESULT hr;

	if (!out)
		return E_POINTER;
	*out = NULL;
	if (!(keys = calloc(b->nparams + 1, sizeof(*keys))))
		return E_OUTOFMEMORY;
	for (DWORD i = 0; i < b->nparams; i++)
		keys[i] = b->params[i].key;
	hr = ens_new(keys, b->nparams, out);
	free(keys);
	return hr;
}

WINFN(HRESULT) bc_RevokeObjectParam(IBindCtx *self, LPOLESTR key)
{
	struct bc *b = bc_impl(self);

	if (!key)
		return E_INVALIDARG;
	struct param *p = find_param(b, key);
	if (!p)
		return S_FALSE;
	/* Detach first: the last Release may call back into the context. */
	WCHAR *k = p->key;
	IUnknown *obj = p->obj;
	DWORD i = (DWORD)(p - b->params);
	memmove(p, p + 1, (b->nparams - i - 1) * sizeof(*p));
	b->nparams--;
	free(k);
	obj->lpVtbl->Release(obj);
	return S_OK;
}

static const IBindCtxVtbl bc_vtbl = {
	bc_QueryInterface, bc_AddRef, bc_Release, bc_RegisterObjectBound,
	bc_RevokeObjectBound, bc_ReleaseBoundObjects, bc_SetBindOptions,
	bc_GetBindOptions, bc_GetRunningObjectTable, bc_RegisterObjectParam,
	bc_GetObjectParam, bc_EnumObjectParam, bc_RevokeObjectParam,
};

WINFN(HRESULT) ole_CreateBindCtx(DWORD reserved, IBindCtx **out)
{
	if (!out)
		return E_INVALIDARG;
	*out = NULL;
	if (reserved)
		return E_INVALIDARG;
	struct bc *b = calloc(1, sizeof(*b));
	if (!b)
		return E_OUTOFMEMORY;
	b->iface.lpVtbl = &bc_vtbl;
	b->refs = 1;
	b->opts.cbStruct = sizeof(b->opts);
	b->opts.grfMode = STGM_READWRITE;
	b->opts.dwClassContext = CLSCTX_SERVER;
	b->opts.locale = 0x0409;
	*out = &b->iface;
	return S_OK;
}

/* ---- IRunningObjectTable ---- */

WINFN(HRESULT) rot_QueryInterface(IRunningObjectTable *self, REFIID riid,
				  void **ppv)
{
	if (!ppv)
		return E_POINTER;
	if (!guid_eq(riid, &IID_IUnknown) &&
	    !guid_eq(riid, &IID_IRunningObjectTable)) {
		*ppv = NULL;
		return E_NOINTERFACE;
	}
	*ppv = self;
	return S_OK;
}

WINFN(ULONG) rot_AddRef(IRunningObjectTable *self)
{
	return 2;
}

WINFN(ULONG) rot_Release(IRunningObjectTable *self)
{
	return 1;
}

WINFN(HRESULT) rot_Register(IRunningObjectTable *self, DWORD flags,
			    IUnknown *obj, IMoniker *name, DWORD *cookie)
{
	HRESULT hr = S_OK;

	if (cookie)
		*cookie = 0;
	if (!obj || !name || !cookie)
		return E_INVALIDARG;
	if (flags & ~(ROTFLAGS_REGISTRATIONKEEPSALIVE |
		      ROTFLAGS_ALLOWANYCLIENT))
		return E_INVALIDARG;
	pthread_mutex_lock(&rot_lock);
	if (rot_find(name))
		hr = MK_S_MONIKERALREADYREGISTERED;
	if (rot_n == rot_cap) {
		size_t cap = rot_cap ? rot_cap * 2 : 8;
		struct rot_entry *v = realloc(rot, cap * sizeof(*v));
		if (!v) {
			pthread_mutex_unlock(&rot_lock);
			return E_OUTOFMEMORY;
		}
		rot = v;
		rot_cap = cap;
	}
	/* The table holds the object, as Windows' does for its process. */
	obj->lpVtbl->AddRef(obj);
	name->lpVtbl->AddRef(name);
	struct rot_entry *e = &rot[rot_n++];
	e->cookie = rot_next++;
	if (!rot_next)
		rot_next = 1;
	e->obj = obj;
	e->name = name;
	now(&e->time);
	*cookie = e->cookie;
	pthread_mutex_unlock(&rot_lock);
	return hr;
}

WINFN(HRESULT) rot_Revoke(IRunningObjectTable *self, DWORD cookie)
{
	IUnknown *obj = NULL;
	IMoniker *name = NULL;

	pthread_mutex_lock(&rot_lock);
	for (size_t i = 0; i < rot_n; i++)
		if (rot[i].cookie == cookie) {
			obj = rot[i].obj;
			name = rot[i].name;
			memmove(rot + i, rot + i + 1,
				(rot_n - i - 1) * sizeof(*rot));
			rot_n--;
			break;
		}
	pthread_mutex_unlock(&rot_lock);
	if (!obj)
		return E_INVALIDARG;
	/* Outside the lock: the last release may run anything. */
	obj->lpVtbl->Release(obj);
	name->lpVtbl->Release(name);
	return S_OK;
}

WINFN(HRESULT) rot_IsRunning(IRunningObjectTable *self, IMoniker *name)
{
	if (!name)
		return E_INVALIDARG;
	return rot_object(name, NULL) == S_OK ? S_OK : S_FALSE;
}

WINFN(HRESULT) rot_GetObject(IRunningObjectTable *self, IMoniker *name,
			     IUnknown **obj)
{
	if (!obj)
		return E_POINTER;
	*obj = NULL;
	if (!name)
		return E_INVALIDARG;
	HRESULT hr = rot_object(name, obj);
	return hr == MK_E_UNAVAILABLE ? S_FALSE : hr;	/* a miss */
}

WINFN(HRESULT) rot_NoteChangeTime(IRunningObjectTable *self, DWORD cookie,
				  FILETIME *t)
{
	HRESULT hr = E_INVALIDARG;

	if (!t)
		return E_INVALIDARG;
	pthread_mutex_lock(&rot_lock);
	for (size_t i = 0; i < rot_n; i++)
		if (rot[i].cookie == cookie) {
			rot[i].time = *t;
			hr = S_OK;
			break;
		}
	pthread_mutex_unlock(&rot_lock);
	return hr;
}

WINFN(HRESULT) rot_GetTimeOfLastChange(IRunningObjectTable *self,
				       IMoniker *name, FILETIME *t)
{
	if (!name || !t)
		return E_INVALIDARG;
	return rot_time(name, t);
}

WINFN(HRESULT) rot_EnumRunning(IRunningObjectTable *self, IEnumMoniker **out)
{
	IMoniker **v;
	HRESULT hr;

	if (!out)
		return E_POINTER;
	*out = NULL;
	pthread_mutex_lock(&rot_lock);
	v = malloc((rot_n + 1) * sizeof(*v));
	for (size_t i = 0; v && i < rot_n; i++)
		v[i] = rot[i].name;
	hr = v ? enm_new(v, (DWORD)rot_n, 0, out) : E_OUTOFMEMORY;
	pthread_mutex_unlock(&rot_lock);
	free(v);
	return hr;
}

static const IRunningObjectTableVtbl rot_vtbl = {
	rot_QueryInterface, rot_AddRef, rot_Release, rot_Register, rot_Revoke,
	rot_IsRunning, rot_GetObject, rot_NoteChangeTime,
	rot_GetTimeOfLastChange, rot_EnumRunning,
};

static IRunningObjectTable the_rot = { &rot_vtbl };

WINFN(HRESULT) ole_GetRunningObjectTable(DWORD reserved,
					 IRunningObjectTable **out)
{
	if (!out)
		return E_INVALIDARG;
	*out = NULL;
	if (reserved)
		return E_UNEXPECTED;
	*out = &the_rot;
	return S_OK;
}

/* ---- the creation functions ---- */

WINFN(HRESULT) ole_CreateFileMoniker(LPCOLESTR path, IMoniker **out)
{
	if (!out)
		return E_INVALIDARG;
	*out = NULL;
	if (!path)
		return MK_E_SYNTAX;
	return file_new(path, wlen(path), out);
}

WINFN(HRESULT) ole_CreateItemMoniker(LPCOLESTR delim, LPCOLESTR item,
				     IMoniker **out)
{
	static const WCHAR empty[1];
	if (!out)
		return E_INVALIDARG;
	*out = NULL;
	return item_new(delim ? delim : empty, item ? item : empty, out);
}

WINFN(HRESULT) ole_CreateAntiMoniker(IMoniker **out)
{
	if (!out)
		return E_INVALIDARG;
	return anti_new(1, out);
}

WINFN(HRESULT) ole_CreateClassMoniker(REFCLSID clsid, IMoniker **out)
{
	if (!out || !clsid)
		return E_INVALIDARG;
	return class_new(clsid, out);
}

WINFN(HRESULT) ole_CreatePointerMoniker(IUnknown *obj, IMoniker **out)
{
	if (!out)
		return E_INVALIDARG;
	return pointer_new(obj, out);
}

WINFN(HRESULT) ole_CreateGenericComposite(IMoniker *first, IMoniker *rest,
					  IMoniker **out)
{
	if (!out)
		return E_POINTER;
	*out = NULL;
	/* compose() itself; a custom ComposeWith(FALSE) may call us. */
	return compose(first, rest, out);
}

WINFN(HRESULT) ole_MonikerCommonPrefixWith(IMoniker *a, IMoniker *b,
					   IMoniker **out)
{
	if (!a || !b || !out)
		return E_INVALIDARG;
	return a->lpVtbl->CommonPrefixWith(a, b, out);
}

WINFN(HRESULT) ole_MonikerRelativePathTo(IMoniker *src, IMoniker *dst,
					 IMoniker **out, BOOL reserved)
{
	if (!src || !dst || !out)
		return E_INVALIDARG;
	return src->lpVtbl->RelativePathTo(src, dst, out);
}

WINFN(HRESULT) ole_BindMoniker(IMoniker *mk, DWORD opts, REFIID riid,
			       void **ppv)
{
	IBindCtx *bc;
	HRESULT hr;

	if (!ppv)
		return E_INVALIDARG;
	*ppv = NULL;
	if (!mk || opts)
		return E_INVALIDARG;
	if (FAILED(hr = ole_CreateBindCtx(0, &bc)))
		return hr;
	hr = mk->lpVtbl->BindToObject(mk, bc, NULL, riid, ppv);
	bc->lpVtbl->Release(bc);
	return hr;
}

/* "XXXXXXXX-XXXX-XXXX-XXXX-XXXXXXXXXXXX", braced or not; chars read. */
static size_t parse_bare_guid(const WCHAR *s, GUID *g)
{
	WCHAR buf[OLE_GUID_CHARS];
	size_t n = s[0] == '{' ? 38 : 36;

	for (size_t i = 0; i < n; i++)
		if (!s[i])
			return 0;
	if (s[0] == '{') {
		memcpy(buf, s, 38 * sizeof(WCHAR));
	} else {
		buf[0] = '{';
		memcpy(buf + 1, s, 36 * sizeof(WCHAR));
		buf[37] = '}';
	}
	buf[38] = 0;
	return ole_guid_parse(buf, g) ? 0 : n;
}

static int starts_with_i(const WCHAR *s, const char *p)
{
	for (; *p; p++, s++)
		if (wup(*s) != wup((BYTE)*p))
			return 0;
	return 1;
}

/* The class's IParseDisplayName parses the whole of name. */
static HRESULT parse_by_class(IBindCtx *bc, const CLSID *clsid,
			      LPOLESTR name, ULONG *eaten, IMoniker **out)
{
	IParseDisplayName *p;
	HRESULT hr = class_object(clsid, CLSCTX_ALL, &IID_IParseDisplayName,
				  (void **)&p);
	if (FAILED(hr))
		return hr;
	hr = p->lpVtbl->ParseDisplayName(p, bc, name, eaten, out);
	p->lpVtbl->Release(p);
	return hr;
}

static HRESULT progid_clsid(const WCHAR *s, size_t n, CLSID *clsid)
{
	HRESULT (WINAPI *f)(LPCOLESTR, LPCLSID) = ole_fn("CLSIDFromProgID");
	WCHAR *p = wdupn(s, n);
	HRESULT hr = p && f ? f(p, clsid) : E_OUTOFMEMORY;
	free(p);
	return hr;
}

/* Whether a file moniker of path names something: running, or on disk. */
static int file_exists(const WCHAR *path, size_t n)
{
	IMoniker *m;
	int yes = 0;

	if (FAILED(file_new(path, n, &m)))
		return 0;
	yes = rot_object(m, NULL) == S_OK;
	if (!yes) {
		char *u = unix_path_w(((struct mk *)m)->name);
		struct stat st;
		yes = u && !stat(u, &st);
		free(u);
	}
	mk_Release(m);
	return yes;
}

static int item_boundary(const WCHAR *name, size_t end, size_t n)
{
	if (end == n)
		return 1;
	WCHAR c = name[end];
	if ((c >= '0' && c <= '9') || (c >= 'A' && c <= 'Z') ||
	    (c >= 'a' && c <= 'z') || c >= 0x80)
		return 0;
	return !strchr(".-_ \\/:~$'()[]{}+,;=", (char)c);
}

WINFN(HRESULT) ole_MkParseDisplayName(IBindCtx *bc, LPCOLESTR name,
				      ULONG *eaten, IMoniker **out)
{
	IMoniker *acc = NULL;
	ULONG at = 0;
	HRESULT hr;
	CLSID clsid;

	if (!out)
		return E_POINTER;
	*out = NULL;
	if (!bc || !name || !eaten)
		return E_INVALIDARG;
	*eaten = 0;
	size_t n = wlen(name);
	if (!n)
		return MK_E_SYNTAX;

	if (name[0] == '@') {
		size_t k = 1;
		while (k < n && name[k] != '!' && name[k] != '\\' &&
		       name[k] != '/' && name[k] != ':')
			k++;
		if (FAILED(progid_clsid(name + 1, k - 1, &clsid)))
			return MK_E_SYNTAX;
		return parse_by_class(bc, &clsid, (LPOLESTR)name, eaten, out);
	}
	if (starts_with_i(name, "clsid:")) {
		size_t g = parse_bare_guid(name + 6, &clsid);
		if (!g)
			return MK_E_SYNTAX;
		at = (ULONG)(6 + g);
		if (name[at] == ':')
			at++;
		if (FAILED(hr = class_new(&clsid, &acc)))
			return hr;
	} else {
		/* "ProgID:rest", where ProgID is a class's. */
		size_t colon = 0;
		while (colon < n && name[colon] != ':' && name[colon] != '\\' &&
		       name[colon] != '/' && name[colon] != '!')
			colon++;
		if (colon > 1 && colon < n && name[colon] == ':' &&
		    SUCCEEDED(progid_clsid(name, colon, &clsid)))
			return parse_by_class(bc, &clsid, (LPOLESTR)name,
					      eaten, out);
		/* The longest prefix that is a running or real file, ending
		 * at the end or before a character that can start an item
		 * ("!", "|", ...: not part of an ordinary path). */
		size_t end = n;
		while (end && !(item_boundary(name, end, n) &&
				file_exists(name, end)))
			end--;
		if (!end)
			return MK_E_CANTOPENFILE;
		if (FAILED(hr = file_new(name, end, &acc)))
			return hr;
		at = (ULONG)end;
	}

	while (at < n) {
		IMoniker *next = NULL, *c;
		ULONG e = 0;
		hr = acc->lpVtbl->ParseDisplayName(acc, bc, NULL,
						   (LPOLESTR)name + at, &e,
						   &next);
		if (FAILED(hr) || !e || !next) {
			if (next)
				next->lpVtbl->Release(next);
			acc->lpVtbl->Release(acc);
			*eaten = at;
			return FAILED(hr) ? hr : MK_E_SYNTAX;
		}
		hr = compose(acc, next, &c);
		next->lpVtbl->Release(next);
		acc->lpVtbl->Release(acc);
		if (SUCCEEDED(hr) && !c)	/* it cancelled to nothing */
			hr = MK_E_SYNTAX;
		if (FAILED(hr)) {
			*eaten = at;
			return hr;
		}
		acc = c;
		at += e;
	}
	*eaten = at;
	*out = acc;
	return S_OK;
}

WINFN(HRESULT) ole_CoGetObject(LPCWSTR name, BIND_OPTS *opts, REFIID riid,
			       void **ppv)
{
	IBindCtx *bc;
	IMoniker *mk;
	ULONG eaten;
	HRESULT hr;

	if (!ppv)
		return E_POINTER;
	*ppv = NULL;
	if (FAILED(hr = ole_CreateBindCtx(0, &bc)))
		return hr;
	if (opts && FAILED(hr = bc->lpVtbl->SetBindOptions(bc, opts)))
		goto out;
	if (FAILED(hr = ole_MkParseDisplayName(bc, name, &eaten, &mk)))
		goto out;
	hr = mk->lpVtbl->BindToObject(mk, bc, NULL, riid, ppv);
	mk->lpVtbl->Release(mk);
out:
	bc->lpVtbl->Release(bc);
	return hr;
}

WINFN(HRESULT) ole_GetClassFile(LPCOLESTR name, CLSID *clsid)
{
	HRESULT hr;

	if (!name || !clsid)
		return E_INVALIDARG;
	memset(clsid, 0, sizeof(*clsid));
	char *u = unix_path_w(name);
	struct stat st;
	int there = u && !stat(u, &st);
	free(u);
	if (!there)
		return MK_E_CANTOPENFILE;

	/* A compound file names its class. */
	HRESULT (WINAPI *is_stg)(const WCHAR *) = ole_fn("StgIsStorageFile");
	HRESULT (WINAPI *open)(const WCHAR *, IStorage *, DWORD, void *, DWORD,
			       IStorage **) = ole_fn("StgOpenStorage");
	if (is_stg && open && is_stg(name) == S_OK) {
		IStorage *stg;
		hr = open(name, NULL, STGM_READ | STGM_SHARE_DENY_WRITE, NULL,
			  0, &stg);
		if (SUCCEEDED(hr)) {
			STATSTG s;
			memset(&s, 0, sizeof(s));
			hr = stg->lpVtbl->Stat(stg, &s, 1 /* STATFLAG_NONAME */);
			stg->lpVtbl->Release(stg);
			if (SUCCEEDED(hr)) {
				*clsid = s.clsid;
				return S_OK;
			}
		}
	}

	/* Else the class registered for its extension. */
	size_t n = wlen(name), dot = n;
	while (dot && name[dot - 1] != '.' && !is_sep(name[dot - 1]))
		dot--;
	if (!dot || name[dot - 1] != '.')
		return MK_E_INVALIDEXTENSION;
	char *ext = utf16_to_utf8_dup(name + dot - 1);
	char *progid = ext ? ole_classes_value(ext, "") : NULL;
	free(ext);
	if (!progid)
		return MK_E_INVALIDEXTENSION;
	char sub[300];
	snprintf(sub, sizeof(sub), "%s\\CLSID", progid);
	free(progid);
	char *g = ole_classes_value(sub, "");
	WCHAR *w = g ? utf8_to_utf16_dup(g) : NULL;
	free(g);
	hr = w && !ole_guid_parse(w, clsid) ? S_OK : MK_E_INVALIDEXTENSION;
	free(w);
	return hr;
}

static HRESULT fill_results(IUnknown *obj, DWORD count, MULTI_QI *results)
{
	DWORD got = 0;
	for (DWORD i = 0; i < count; i++) {
		results[i].pItf = NULL;
		results[i].hr = obj->lpVtbl->QueryInterface(
			obj, results[i].pIID, (void **)&results[i].pItf);
		got += SUCCEEDED(results[i].hr);
	}
	return got == count ? S_OK : got ? CO_S_NOTALLINTERFACES :
					   E_NOINTERFACE;
}

WINFN(HRESULT) ole_CoGetInstanceFromFile(void *server, CLSID *clsid,
					 IUnknown *outer, DWORD ctx,
					 DWORD mode, OLECHAR *name,
					 DWORD count, MULTI_QI *results)
{
	CLSID c;
	IPersistFile *pf;
	HRESULT hr;

	if (!count || !results || !name)
		return E_INVALIDARG;
	for (DWORD i = 0; i < count; i++) {
		results[i].pItf = NULL;
		results[i].hr = E_NOINTERFACE;
	}
	if (outer)
		return CLASS_E_NOAGGREGATION;
	if (clsid)
		c = *clsid;
	else if (FAILED(hr = ole_GetClassFile(name, &c)))
		return hr;
	if (FAILED(hr = create_instance(&c, ctx, &IID_IPersistFile,
					(void **)&pf)))
		return hr;
	hr = pf->lpVtbl->Load(pf, name, mode);
	if (SUCCEEDED(hr))
		hr = fill_results((IUnknown *)pf, count, results);
	pf->lpVtbl->Release(pf);
	return hr;
}

WINFN(HRESULT) ole_CoGetInstanceFromIStorage(void *server, CLSID *clsid,
					     IUnknown *outer, DWORD ctx,
					     IStorage *stg, DWORD count,
					     MULTI_QI *results)
{
	CLSID c;
	IPersistStorage *ps;
	HRESULT hr;

	if (!count || !results || !stg)
		return E_INVALIDARG;
	for (DWORD i = 0; i < count; i++) {
		results[i].pItf = NULL;
		results[i].hr = E_NOINTERFACE;
	}
	if (outer)
		return CLASS_E_NOAGGREGATION;
	if (clsid) {
		c = *clsid;
	} else {
		STATSTG s;
		memset(&s, 0, sizeof(s));
		if (FAILED(hr = stg->lpVtbl->Stat(stg, &s, 1)))
			return hr;
		c = s.clsid;
	}
	if (FAILED(hr = create_instance(&c, ctx, &IID_IPersistStorage,
					(void **)&ps)))
		return hr;
	hr = ps->lpVtbl->Load(ps, stg);
	if (SUCCEEDED(hr))
		hr = fill_results((IUnknown *)ps, count, results);
	ps->lpVtbl->Release(ps);
	return hr;
}

#define E(name) { #name, 0, (void *)ole_##name }

const struct builtin_export ole_moniker_exports[] = {
	E(BindMoniker),
	E(CoGetInstanceFromFile),
	E(CoGetInstanceFromIStorage),
	E(CoGetObject),
	E(CreateAntiMoniker),
	E(CreateBindCtx),
	E(CreateClassMoniker),
	E(CreateFileMoniker),
	E(CreateGenericComposite),
	E(CreateItemMoniker),
	E(CreatePointerMoniker),
	E(GetClassFile),
	E(GetRunningObjectTable),
	E(MkParseDisplayName),
	E(MonikerCommonPrefixWith),
	E(MonikerRelativePathTo),
	BUILTIN_END
};
