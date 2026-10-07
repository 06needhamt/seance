/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * msdmo.dll: registering DirectX Media Objects (DMORegister,
 * DMOUnregister), finding them again (DMOEnum, DMOGetTypes, DMOGetName),
 * writing and reading the registry as Windows does (after Wine's
 * dmoreg.c), and the DMO_MEDIA_TYPE helpers DMOs use on what they are
 * handed (MoInitMediaType and the rest). msdmo.h's capture mode records
 * what is registered, for DMO discovery.
 *
 * A DMO is HKCR\DirectShow\MediaObjects\{clsid}: its name as the default
 * value, InputTypes and OutputTypes as REG_BINARY arrays of
 * DMO_PARTIAL_MEDIATYPE, and a Keyed subkey if it is keyed; and, for each
 * category it is in, a key HKCR\DirectShow\MediaObjects\Categories
 * \{category}\{clsid}.
 */
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#include "common/log.h"
#include "runtime/builtin.h"
#include "win32/codepage.h"
#include "win32/internal.h"
#include "win32/msdmo.h"
#include "win32/ole.h"
#include "win32/reg.h"

/* HKEY_CLASSES_ROOT: written to the machine's classes, read from both. */
#define WRITE_ROOT "HKEY_LOCAL_MACHINE\\Software\\Classes"
static const char *const class_roots[] = {
	"HKEY_CURRENT_USER\\Software\\Classes",
	"HKEY_LOCAL_MACHINE\\Software\\Classes",
};
#define NROOTS (sizeof(class_roots) / sizeof(*class_roots))
#define OBJECTS "\\DirectShow\\MediaObjects"
#define CATEGORIES OBJECTS "\\Categories"

static const GUID guid_null;

static int guid_eq(const GUID *a, const GUID *b)
{
	return !memcmp(a, b, sizeof(GUID));
}

static WCHAR *wstrdup(const WCHAR *s)
{
	size_t n = (wstrlen(s) + 1) * sizeof(WCHAR);
	WCHAR *d = malloc(n);

	if (d)
		memcpy(d, s, n);
	return d;
}

static HRESULT hr_from_win32(LONG err)
{
	return err ? HRESULT_FROM_WIN32(err) : S_OK;
}

/* Registrations as data. */

void dmo_reg_clear(struct dmo_reg *d)
{
	free(d->name);
	free(d->in);
	free(d->out);
	memset(d, 0, sizeof(*d));
}

void dmo_regs_free(struct dmo_reg *d, size_t n)
{
	for (size_t i = 0; i < n; i++)
		dmo_reg_clear(&d[i]);
	free(d);
}

static int types_copy(DMO_PARTIAL_MEDIATYPE **out, size_t *n,
		      const DMO_PARTIAL_MEDIATYPE *t, size_t count)
{
	*out = NULL;
	*n = 0;
	if (!count)
		return 0;
	if (!t || !(*out = malloc(count * sizeof(*t))))
		return -1;
	memcpy(*out, t, count * sizeof(*t));
	*n = count;
	return 0;
}

/* d's copy of another registration; 0, or -1 if out of memory. */
static int reg_copy(struct dmo_reg *d, const struct dmo_reg *s)
{
	*d = *s;
	d->name = NULL;
	d->in = d->out = NULL;
	if ((s->name && !(d->name = wstrdup(s->name))) ||
	    types_copy(&d->in, &d->nin, s->in, s->nin) ||
	    types_copy(&d->out, &d->nout, s->out, s->nout)) {
		dmo_reg_clear(d);
		return -1;
	}
	return 0;
}

/* Capture mode. */

static pthread_mutex_t capture_lock = PTHREAD_MUTEX_INITIALIZER;
static int capturing;
static struct dmo_reg *captured;
static size_t ncaptured;

void msdmo_capture_begin(void)
{
	pthread_mutex_lock(&capture_lock);
	dmo_regs_free(captured, ncaptured);
	captured = NULL;
	ncaptured = 0;
	capturing = 1;
	pthread_mutex_unlock(&capture_lock);
}

size_t msdmo_capture_end(struct dmo_reg **out)
{
	pthread_mutex_lock(&capture_lock);
	size_t n = ncaptured;
	*out = captured;
	captured = NULL;
	ncaptured = 0;
	capturing = 0;
	pthread_mutex_unlock(&capture_lock);
	return n;
}

/* Drops the captured registrations of clsid in category (any if NULL). */
static void capture_remove_locked(REFCLSID clsid, const GUID *category)
{
	for (size_t i = 0; i < ncaptured;) {
		if (guid_eq(&captured[i].clsid, clsid) &&
		    (guid_eq(category, &guid_null) ||
		     guid_eq(&captured[i].category, category))) {
			dmo_reg_clear(&captured[i]);
			memmove(&captured[i], &captured[i + 1],
				(ncaptured - i - 1) * sizeof(*captured));
			ncaptured--;
		} else {
			i++;
		}
	}
}

static void capture_remove(REFCLSID clsid, const GUID *category)
{
	pthread_mutex_lock(&capture_lock);
	capture_remove_locked(clsid, category);
	pthread_mutex_unlock(&capture_lock);
}

/* Records a registration (copying it), replacing an earlier one. */
static void capture_add(const struct dmo_reg *d)
{
	pthread_mutex_lock(&capture_lock);
	if (capturing) {
		capture_remove_locked(&d->clsid, &d->category);
		struct dmo_reg *c = realloc(captured,
					    (ncaptured + 1) * sizeof(*c));
		if (c) {
			captured = c;
			if (!reg_copy(&captured[ncaptured], d))
				ncaptured++;
		}
	}
	pthread_mutex_unlock(&capture_lock);
}

/* The registry. */

/* Deletes k and everything under it (the registry lock held). */
static LONG delete_tree(struct reg_key *k)
{
	while (k->nsub) {
		LONG err = delete_tree(k->sub[k->nsub - 1]);
		if (err)
			return err;
	}
	return reg_delete(k);
}

/* A GUID in a key's name. */
static int parse_guid(const char *s, GUID *out)
{
	WCHAR *w = utf8_to_utf16_dup(s);
	int r = w ? ole_guid_parse(w, out) : -1;

	free(w);
	return r;
}

static struct reg_key *subkey(struct reg_key *k, const char *name)
{
	for (size_t i = 0; k && i < k->nsub; i++)
		if (!strcasecmp(k->sub[i]->name, name))
			return k->sub[i];
	return NULL;
}

/* A REG_BINARY array of partial types; 0 if absent, -1 if malformed. */
static int read_types(struct reg_key *k, const char *name,
		      DMO_PARTIAL_MEDIATYPE **out, size_t *n)
{
	struct reg_value *v = reg_get(k, name);

	*out = NULL;
	*n = 0;
	if (!v)
		return 0;
	if (v->type != REG_BINARY || v->size % sizeof(DMO_PARTIAL_MEDIATYPE))
		return -1;
	return types_copy(out, n, (const DMO_PARTIAL_MEDIATYPE *)v->data,
			  v->size / sizeof(DMO_PARTIAL_MEDIATYPE));
}

/* The default value as a string, malloc()ed; NULL if there is none. */
static WCHAR *read_name(struct reg_key *k)
{
	struct reg_value *v = reg_get(k, "");

	if (!v || (v->type != REG_SZ && v->type != REG_EXPAND_SZ))
		return NULL;
	size_t n = v->size / sizeof(WCHAR);
	WCHAR *w = malloc((n + 1) * sizeof(WCHAR));
	if (!w)
		return NULL;
	memcpy(w, v->data, n * sizeof(WCHAR));
	w[n] = 0;
	return w;
}

struct reg_list {
	struct dmo_reg *v;
	size_t n, cap;
};

static struct dmo_reg *list_add(struct reg_list *l)
{
	if (l->n == l->cap) {
		size_t cap = l->cap ? l->cap * 2 : 16;
		struct dmo_reg *v = realloc(l->v, cap * sizeof(*v));
		if (!v)
			return NULL;
		l->v = v;
		l->cap = cap;
	}
	memset(&l->v[l->n], 0, sizeof(*l->v));
	return &l->v[l->n++];
}

/* The DMO keys of every root, one entry each, without categories. */
static void read_objects(struct reg_list *l)
{
	char path[256];

	for (size_t r = 0; r < NROOTS; r++) {
		snprintf(path, sizeof(path), "%s" OBJECTS, class_roots[r]);
		struct reg_key *k = reg_open(path);
		for (size_t i = 0; k && i < k->nsub; i++) {
			struct reg_key *o = k->sub[i];
			GUID clsid;
			int dup = 0;
			if (parse_guid(o->name, &clsid))
				continue;	/* Categories, and anything odd */
			for (size_t j = 0; j < l->n && !dup; j++)
				dup = guid_eq(&l->v[j].clsid, &clsid);
			if (dup)
				continue;
			struct dmo_reg *d = list_add(l);
			if (!d)
				return;
			d->clsid = clsid;
			d->name = read_name(o);
			if (subkey(o, "Keyed"))
				d->flags |= DMO_REGISTERF_IS_KEYED;
			if (read_types(o, "InputTypes", &d->in, &d->nin) ||
			    read_types(o, "OutputTypes", &d->out, &d->nout))
				TRACE("msdmo: %s has malformed types", o->name);
		}
	}
}

/* Whether clsid is listed under the category key c. */
static int listed(struct reg_key *c, const GUID *clsid)
{
	for (size_t i = 0; c && i < c->nsub; i++) {
		GUID g;
		if (!parse_guid(c->sub[i]->name, &g) && guid_eq(&g, clsid))
			return 1;
	}
	return 0;
}

/* The categories (from every root) listing clsid, at most max of them. */
static size_t categories_of(const GUID *clsid, GUID *out, size_t max)
{
	char path[256];
	size_t n = 0;

	for (size_t r = 0; r < NROOTS; r++) {
		snprintf(path, sizeof(path), "%s" CATEGORIES, class_roots[r]);
		struct reg_key *k = reg_open(path);
		for (size_t i = 0; k && i < k->nsub && n < max; i++) {
			GUID cat;
			int dup = 0;
			if (parse_guid(k->sub[i]->name, &cat) ||
			    !listed(k->sub[i], clsid))
				continue;
			for (size_t j = 0; j < n && !dup; j++)
				dup = guid_eq(&out[j], &cat);
			if (!dup)
				out[n++] = cat;
		}
	}
	return n;
}

/* msdmo_registered with the registry lock held. */
static size_t registered_locked(struct dmo_reg **out)
{
	struct reg_list objects = { 0 }, l = { 0 };

	read_objects(&objects);
	for (size_t i = 0; i < objects.n; i++) {
		GUID cats[32];
		size_t n = categories_of(&objects.v[i].clsid, cats, 32);
		for (size_t c = 0; c < (n ? n : 1); c++) {
			struct dmo_reg *d = list_add(&l);
			if (!d || reg_copy(d, &objects.v[i])) {
				if (d)
					l.n--;
				break;
			}
			d->category = n ? cats[c] : guid_null;
		}
	}
	dmo_regs_free(objects.v, objects.n);
	*out = l.v;
	return l.n;
}

size_t msdmo_registered(struct dmo_reg **out)
{
	reg_lock();
	size_t n = registered_locked(out);
	reg_unlock();
	return n;
}

/* Registering. */

static LONG write_types(struct reg_key *k, const char *name, DWORD n,
			const DMO_PARTIAL_MEDIATYPE *t)
{
	return reg_set(k, name, REG_BINARY, n ? (const void *)t : "",
		       n * (DWORD)sizeof(*t));
}

WINFN(HRESULT) dmo_DMORegister(LPCWSTR name, REFCLSID clsid,
			       REFGUID category, DWORD flags, DWORD nin,
			       const DMO_PARTIAL_MEDIATYPE *in, DWORD nout,
			       const DMO_PARTIAL_MEDIATYPE *out)
{
	char g[OLE_GUID_CHARS], cat[OLE_GUID_CHARS], path[256];
	LONG err;

	if (!clsid || !category || (nin && !in) || (nout && !out))
		return E_POINTER;
	ole_guid_format(clsid, g);
	ole_guid_format(category, cat);

	reg_lock();
	snprintf(path, sizeof(path), WRITE_ROOT OBJECTS "\\%s", g);
	struct reg_key *k = reg_create(path, 0, NULL, &err);
	if (k && name)
		err = reg_set(k, "", REG_SZ, name,
			      (DWORD)((wstrlen(name) + 1) * sizeof(WCHAR)));
	if (k && !err && !(err = write_types(k, "InputTypes", nin, in)) &&
	    !(err = write_types(k, "OutputTypes", nout, out))) {
		struct reg_key *keyed = subkey(k, "Keyed");
		if (flags & DMO_REGISTERF_IS_KEYED) {
			snprintf(path, sizeof(path),
				 WRITE_ROOT OBJECTS "\\%s\\Keyed", g);
			if (!reg_create(path, 0, NULL, &err))
				k = NULL;
		} else if (keyed) {
			err = delete_tree(keyed);
		}
	}
	if (k && !err) {
		snprintf(path, sizeof(path), WRITE_ROOT CATEGORIES "\\%s\\%s",
			 cat, g);
		if (reg_create(path, 0, NULL, &err))
			err = reg_save();
	}
	reg_unlock();

	char *n = name ? utf16_to_utf8_dup(name) : NULL;
	TRACE("DMORegister(%s \"%s\", category %s, flags 0x%lx, %lu in, %lu "
	      "out) = %ld", g, n ? n : "", cat, (unsigned long)flags,
	      (unsigned long)nin, (unsigned long)nout, (long)err);
	free(n);
	if (err)
		return hr_from_win32(err);

	struct dmo_reg d = {
		.clsid = *clsid, .category = *category,
		.name = (WCHAR *)name, .flags = flags & DMO_REGISTERF_IS_KEYED,
		.nin = nin, .nout = nout,
		.in = (DMO_PARTIAL_MEDIATYPE *)in,
		.out = (DMO_PARTIAL_MEDIATYPE *)out,
	};
	capture_add(&d);
	return S_OK;
}

/*
 * Takes the DMO out of category, or out of every category if that is
 * GUID_NULL, and removes its key. S_OK if anything was removed, else
 * S_FALSE.
 */
WINFN(HRESULT) dmo_DMOUnregister(REFCLSID clsid, REFGUID category)
{
	char g[OLE_GUID_CHARS], path[256];
	LONG err = 0;
	int removed = 0;

	if (!clsid || !category)
		return E_POINTER;
	ole_guid_format(clsid, g);

	reg_lock();
	for (size_t r = 0; r < NROOTS && !err; r++) {
		snprintf(path, sizeof(path), "%s" CATEGORIES, class_roots[r]);
		struct reg_key *cats = reg_open(path);
		for (size_t i = cats ? cats->nsub : 0; i-- > 0 && !err;) {
			GUID cat;
			struct reg_key *c = cats->sub[i];
			if (parse_guid(c->name, &cat) ||
			    (!guid_eq(category, &guid_null) &&
			     !guid_eq(category, &cat)))
				continue;
			for (size_t j = c->nsub; j-- > 0 && !err;) {
				GUID o;
				if (!parse_guid(c->sub[j]->name, &o) &&
				    guid_eq(&o, clsid)) {
					err = delete_tree(c->sub[j]);
					removed = 1;
				}
			}
		}
		snprintf(path, sizeof(path), "%s" OBJECTS "\\%s",
			 class_roots[r], g);
		struct reg_key *k = err ? NULL : reg_open(path);
		if (k) {
			err = delete_tree(k);
			removed = 1;
		}
	}
	if (removed && !err)
		err = reg_save();
	reg_unlock();
	capture_remove(clsid, category);

	char cat[OLE_GUID_CHARS];
	ole_guid_format(category, cat);
	TRACE("DMOUnregister(%s, %s) = %ld%s", g, cat, (long)err,
	      removed ? "" : " (not registered)");
	if (err)
		return hr_from_win32(err);
	return removed ? S_OK : S_FALSE;
}

/* The registration of clsid (category ignored), or -1 if there is none. */
static int find(REFCLSID clsid, struct dmo_reg *out)
{
	struct dmo_reg *all = NULL;
	size_t n = msdmo_registered(&all);
	int found = -1;

	for (size_t i = 0; i < n && found; i++)
		if (guid_eq(&all[i].clsid, clsid))
			found = reg_copy(out, &all[i]) ? -2 : 0;
	dmo_regs_free(all, n);
	return found;
}

WINFN(HRESULT) dmo_DMOGetTypes(REFCLSID clsid, ULONG max_in, ULONG *nin,
			       DMO_PARTIAL_MEDIATYPE *in, ULONG max_out,
			       ULONG *nout, DMO_PARTIAL_MEDIATYPE *out)
{
	struct dmo_reg d;

	if (!clsid || !nin || !nout || (max_in && !in) || (max_out && !out))
		return E_POINTER;
	*nin = *nout = 0;
	int r = find(clsid, &d);
	if (r)
		return r == -1 ? HRESULT_FROM_WIN32(ERROR_FILE_NOT_FOUND) :
				 E_OUTOFMEMORY;
	*nin = d.nin < max_in ? (ULONG)d.nin : max_in;
	*nout = d.nout < max_out ? (ULONG)d.nout : max_out;
	if (*nin)
		memcpy(in, d.in, *nin * sizeof(*in));
	if (*nout)
		memcpy(out, d.out, *nout * sizeof(*out));
	dmo_reg_clear(&d);
	return S_OK;
}

/* The name of a DMO, truncated to 79 characters, as Windows stores 80. */
WINFN(HRESULT) dmo_DMOGetName(REFCLSID clsid, WCHAR name[80])
{
	struct dmo_reg d;

	if (!clsid || !name)
		return E_POINTER;
	int r = find(clsid, &d);
	if (r)
		return r == -1 ? HRESULT_FROM_WIN32(ERROR_FILE_NOT_FOUND) :
				 E_OUTOFMEMORY;
	size_t n = d.name ? wstrlen(d.name) : 0;
	if (n > 79)
		n = 79;
	if (n)
		memcpy(name, d.name, n * sizeof(WCHAR));
	name[n] = 0;
	dmo_reg_clear(&d);
	return S_OK;
}

/* Enumerating. */

/* Whether a registered type answers a wanted one, GUID_NULL matching any. */
static int type_matches(const DMO_PARTIAL_MEDIATYPE *have,
			const DMO_PARTIAL_MEDIATYPE *want)
{
	return (guid_eq(&want->type, &guid_null) ||
		guid_eq(&have->type, &guid_null) ||
		guid_eq(&have->type, &want->type)) &&
	       (guid_eq(&want->subtype, &guid_null) ||
		guid_eq(&have->subtype, &guid_null) ||
		guid_eq(&have->subtype, &want->subtype));
}

/* With no wanted types, anything; else one of them must be registered. */
static int types_match(const DMO_PARTIAL_MEDIATYPE *have, size_t nhave,
		       const DMO_PARTIAL_MEDIATYPE *want, DWORD nwant)
{
	if (!nwant)
		return 1;
	for (size_t i = 0; i < nhave; i++)
		for (DWORD j = 0; j < nwant; j++)
			if (type_matches(&have[i], &want[j]))
				return 1;
	return 0;
}

struct item {
	CLSID clsid;
	WCHAR *name;
};

struct enum_dmo {
	IEnumDMO iface;
	LONG refs;
	struct item *items;
	size_t n, pos;
};

static struct enum_dmo *from_enum(IEnumDMO *e)
{
	return (struct enum_dmo *)e;
}

static void items_free(struct item *items, size_t n)
{
	for (size_t i = 0; i < n; i++)
		free(items[i].name);
	free(items);
}

static const IEnumDMOVtbl enum_vtbl;

/* An enumerator over items (taking them), at pos. */
static HRESULT enum_create(struct item *items, size_t n, size_t pos,
			   IEnumDMO **out)
{
	struct enum_dmo *e = calloc(1, sizeof(*e));

	if (!e) {
		items_free(items, n);
		return E_OUTOFMEMORY;
	}
	e->iface.lpVtbl = &enum_vtbl;
	e->refs = 1;
	e->items = items;
	e->n = n;
	e->pos = pos;
	*out = &e->iface;
	return S_OK;
}

WINFN(HRESULT) enum_QueryInterface(IEnumDMO *self, REFIID riid, void **ppv)
{
	if (!ppv)
		return E_POINTER;
	if (riid && (guid_eq(riid, &IID_IUnknown) ||
		     guid_eq(riid, &IID_IEnumDMO))) {
		*ppv = self;
		self->lpVtbl->AddRef(self);
		return S_OK;
	}
	*ppv = NULL;
	return E_NOINTERFACE;
}

WINFN(ULONG) enum_AddRef(IEnumDMO *self)
{
	return (ULONG)__atomic_add_fetch(&from_enum(self)->refs, 1,
					 __ATOMIC_SEQ_CST);
}

WINFN(ULONG) enum_Release(IEnumDMO *self)
{
	struct enum_dmo *e = from_enum(self);
	LONG refs = __atomic_sub_fetch(&e->refs, 1, __ATOMIC_SEQ_CST);

	if (!refs) {
		items_free(e->items, e->n);
		free(e);
	}
	return (ULONG)refs;
}

/* Each name is the caller's, to free with CoTaskMemFree. */
WINFN(HRESULT) enum_Next(IEnumDMO *self, DWORD count, CLSID *clsids,
			 WCHAR **names, DWORD *fetched)
{
	struct enum_dmo *e = from_enum(self);
	DWORD got = 0;

	if (!clsids || (!fetched && count != 1))
		return E_POINTER;
	for (; got < count && e->pos < e->n; got++, e->pos++) {
		const struct item *it = &e->items[e->pos];
		clsids[got] = it->clsid;
		if (!names)
			continue;
		static const WCHAR none[1];
		const WCHAR *src = it->name ? it->name : none;
		size_t size = (wstrlen(src) + 1) * sizeof(WCHAR);
		if (!(names[got] = ole_taskmem_alloc(size))) {
			while (got-- > 0)
				ole_taskmem_free(names[got]);
			return E_OUTOFMEMORY;
		}
		memcpy(names[got], src, size);
	}
	if (fetched)
		*fetched = got;
	return got == count ? S_OK : S_FALSE;
}

WINFN(HRESULT) enum_Skip(IEnumDMO *self, DWORD count)
{
	struct enum_dmo *e = from_enum(self);
	size_t left = e->n - e->pos;

	e->pos += count < left ? count : left;
	return count <= left ? S_OK : S_FALSE;
}

WINFN(HRESULT) enum_Reset(IEnumDMO *self)
{
	from_enum(self)->pos = 0;
	return S_OK;
}

WINFN(HRESULT) enum_Clone(IEnumDMO *self, IEnumDMO **copy)
{
	struct enum_dmo *e = from_enum(self);

	if (!copy)
		return E_POINTER;
	*copy = NULL;
	struct item *items = e->n ? calloc(e->n, sizeof(*items)) : NULL;
	if (e->n && !items)
		return E_OUTOFMEMORY;
	for (size_t i = 0; i < e->n; i++) {
		items[i].clsid = e->items[i].clsid;
		if (e->items[i].name &&
		    !(items[i].name = wstrdup(e->items[i].name))) {
			items_free(items, i);
			return E_OUTOFMEMORY;
		}
	}
	return enum_create(items, e->n, e->pos, copy);
}

static const IEnumDMOVtbl enum_vtbl = {
	enum_QueryInterface, enum_AddRef, enum_Release, enum_Next, enum_Skip,
	enum_Reset, enum_Clone,
};

/*
 * The DMOs in category (every DMO for GUID_NULL) that take one of the
 * input types and give one of the output types (any, for none given);
 * keyed DMOs only with DMO_ENUMF_INCLUDE_KEYED. Each DMO is listed once.
 */
WINFN(HRESULT) dmo_DMOEnum(REFGUID category, DWORD flags, DWORD nin,
			   const DMO_PARTIAL_MEDIATYPE *in, DWORD nout,
			   const DMO_PARTIAL_MEDIATYPE *out, IEnumDMO **result)
{
	struct dmo_reg *all = NULL;

	if (!result)
		return E_POINTER;
	*result = NULL;
	if (!category || (nin && !in) || (nout && !out))
		return E_POINTER;
	size_t n = msdmo_registered(&all), count = 0;
	struct item *items = n ? calloc(n, sizeof(*items)) : NULL;
	if (n && !items) {
		dmo_regs_free(all, n);
		return E_OUTOFMEMORY;
	}
	for (size_t i = 0; i < n; i++) {
		const struct dmo_reg *d = &all[i];
		int dup = 0;
		if ((!guid_eq(category, &guid_null) &&
		     !guid_eq(category, &d->category)) ||
		    ((d->flags & DMO_REGISTERF_IS_KEYED) &&
		     !(flags & DMO_ENUMF_INCLUDE_KEYED)) ||
		    !types_match(d->in, d->nin, in, nin) ||
		    !types_match(d->out, d->nout, out, nout))
			continue;
		for (size_t j = 0; j < count && !dup; j++)
			dup = guid_eq(&items[j].clsid, &d->clsid);
		if (dup)
			continue;
		items[count].clsid = d->clsid;
		items[count].name = d->name;	/* taken from all */
		all[i].name = NULL;
		count++;
	}
	dmo_regs_free(all, n);

	char cat[OLE_GUID_CHARS];
	ole_guid_format(category, cat);
	TRACE("DMOEnum(%s, flags 0x%lx, %lu in, %lu out): %zu DMO(s)", cat,
	      (unsigned long)flags, (unsigned long)nin, (unsigned long)nout,
	      count);
	return enum_create(items, count, 0, result);
}

/* DMO_MEDIA_TYPE helpers: the format block is CoTaskMem memory. */

WINFN(HRESULT) dmo_MoInitMediaType(DMO_MEDIA_TYPE *mt, DWORD format_size)
{
	if (!mt)
		return E_POINTER;
	memset(mt, 0, sizeof(*mt));
	if (format_size) {
		if (!(mt->pbFormat = ole_taskmem_alloc(format_size)))
			return E_OUTOFMEMORY;
		mt->cbFormat = format_size;
	}
	return S_OK;
}

WINFN(HRESULT) dmo_MoFreeMediaType(DMO_MEDIA_TYPE *mt)
{
	if (!mt)
		return E_POINTER;
	if (mt->pUnk) {
		mt->pUnk->lpVtbl->Release(mt->pUnk);
		mt->pUnk = NULL;
	}
	ole_taskmem_free(mt->pbFormat);
	mt->pbFormat = NULL;
	mt->cbFormat = 0;
	return S_OK;
}

WINFN(HRESULT) dmo_MoCopyMediaType(DMO_MEDIA_TYPE *dst,
				   const DMO_MEDIA_TYPE *src)
{
	if (!dst || !src)
		return E_POINTER;
	*dst = *src;
	dst->pbFormat = NULL;
	if (src->cbFormat) {
		if (!src->pbFormat) {
			dst->cbFormat = 0;
			dst->pUnk = NULL;
			return E_POINTER;
		}
		if (!(dst->pbFormat = ole_taskmem_alloc(src->cbFormat))) {
			dst->cbFormat = 0;
			dst->pUnk = NULL;
			return E_OUTOFMEMORY;
		}
		memcpy(dst->pbFormat, src->pbFormat, src->cbFormat);
	}
	if (dst->pUnk)
		dst->pUnk->lpVtbl->AddRef(dst->pUnk);
	return S_OK;
}

WINFN(HRESULT) dmo_MoCreateMediaType(DMO_MEDIA_TYPE **out, DWORD format_size)
{
	if (!out)
		return E_POINTER;
	DMO_MEDIA_TYPE *mt = ole_taskmem_alloc(sizeof(*mt));
	if (!mt) {
		*out = NULL;
		return E_OUTOFMEMORY;
	}
	HRESULT hr = dmo_MoInitMediaType(mt, format_size);
	if (FAILED(hr)) {
		ole_taskmem_free(mt);
		mt = NULL;
	}
	*out = mt;
	return hr;
}

WINFN(HRESULT) dmo_MoDeleteMediaType(DMO_MEDIA_TYPE *mt)
{
	if (!mt)
		return E_POINTER;
	dmo_MoFreeMediaType(mt);
	ole_taskmem_free(mt);
	return S_OK;
}

WINFN(HRESULT) dmo_MoDuplicateMediaType(DMO_MEDIA_TYPE **out,
					const DMO_MEDIA_TYPE *src)
{
	if (!out || !src)
		return E_POINTER;
	HRESULT hr = dmo_MoCreateMediaType(out, 0);
	if (FAILED(hr))
		return hr;
	if (FAILED(hr = dmo_MoCopyMediaType(*out, src))) {
		ole_taskmem_free(*out);
		*out = NULL;
	}
	return hr;
}

#define E(name) { #name, 0, (void *)dmo_##name }

static const struct builtin_export exports[] = {
	E(DMOEnum),
	E(DMOGetName),
	E(DMOGetTypes),
	E(DMORegister),
	E(DMOUnregister),
	E(MoCopyMediaType),
	E(MoCreateMediaType),
	E(MoDeleteMediaType),
	E(MoDuplicateMediaType),
	E(MoFreeMediaType),
	E(MoInitMediaType),
	BUILTIN_END
};

static const struct builtin_export *const tables[] = { exports, NULL };

const struct builtin_dll builtin_msdmo = { "msdmo.dll", NULL, tables, 0 };
