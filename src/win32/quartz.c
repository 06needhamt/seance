/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * quartz.dll, the part DirectShow filters use from inside a process that
 * isn't running a filter graph: the filter mappers their DllRegisterServer
 * registers them with (IFilterMapper2, and the legacy IFilterMapper),
 * writing the registry as Windows does (after Wine's filtermapper.c), the
 * system device enumerator, which finds no devices, and AMGetErrorText.
 * quartz.h's capture mode records what is registered for the DirectShow
 * host.
 */
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "common/log.h"
#include "win32/codepage.h"
#include "win32/internal.h"
#include "win32/k32.h"
#include "win32/ole.h"
#include "win32/quartz.h"
#include "win32/reg.h"

/* HKEY_CLASSES_ROOT, where filters are registered. */
#define CLASSES "HKEY_LOCAL_MACHINE\\Software\\Classes\\"

static const GUID guid_null;

static int guid_eq(const GUID *a, const GUID *b)
{
	return !memcmp(a, b, sizeof(GUID));
}

/* Objects and LockServer locks, for DllCanUnloadNow. */
static LONG objects, server_locks;

static void object_added(void)
{
	__atomic_add_fetch(&objects, 1, __ATOMIC_SEQ_CST);
}

static void object_gone(void)
{
	__atomic_sub_fetch(&objects, 1, __ATOMIC_SEQ_CST);
}

/* Pin names compare as Windows compares registry key names. */
static int wide_eq_nocase(const WCHAR *a, const WCHAR *b)
{
	while (*a && k32_tolower_w(*a) == k32_tolower_w(*b))
		a++, b++;
	return k32_tolower_w(*a) == k32_tolower_w(*b);
}

static WCHAR *wstrdup(const WCHAR *s)
{
	size_t n = (wstrlen(s) + 1) * sizeof(WCHAR);
	WCHAR *d = malloc(n);

	if (d)
		memcpy(d, s, n);
	return d;
}

/* Registrations as data. */

static void pin_clear(struct qz_pin *p)
{
	free(p->name);
	free(p->types);
	free(p->mediums);
	memset(p, 0, sizeof(*p));
}

void qz_filter_clear(struct qz_filter *f)
{
	for (size_t i = 0; i < f->npins; i++)
		pin_clear(&f->pins[i]);
	free(f->pins);
	free(f->name);
	memset(f, 0, sizeof(*f));
}

void qz_filters_free(struct qz_filter *f, size_t n)
{
	for (size_t i = 0; i < n; i++)
		qz_filter_clear(&f[i]);
	free(f);
}

static int types_copy(struct qz_pin *p, const REGPINTYPES *t, UINT n)
{
	if (!n)
		return 0;
	if (!t || !(p->types = calloc(n, sizeof(*p->types))))
		return -1;
	for (UINT i = 0; i < n; i++) {
		p->types[i].major = t[i].clsMajorType ? *t[i].clsMajorType :
							guid_null;
		p->types[i].minor = t[i].clsMinorType ? *t[i].clsMinorType :
							guid_null;
	}
	p->ntypes = n;
	return 0;
}

static int pin_from_rf2(struct qz_pin *p, const REGFILTERPINS2 *r)
{
	p->flags = r->dwFlags;
	p->instances = r->cInstances;
	if (r->clsPinCategory) {
		p->has_category = 1;
		p->category = *r->clsPinCategory;
	}
	if (types_copy(p, r->lpMediaType, r->nMediaTypes))
		return -1;
	if (r->nMediums) {
		if (!r->lpMedium ||
		    !(p->mediums = calloc(r->nMediums, sizeof(*p->mediums))))
			return -1;
		memcpy(p->mediums, r->lpMedium,
		       r->nMediums * sizeof(*p->mediums));
		p->nmediums = r->nMediums;
	}
	return 0;
}

/* A version 1 pin, as Wine converts it: its name is not kept. */
static int pin_from_rf1(struct qz_pin *p, const REGFILTERPINS *r)
{
	p->flags = (r->bZero ? REG_PINFLAG_B_ZERO : 0) |
		   (r->bRendered ? REG_PINFLAG_B_RENDERER : 0) |
		   (r->bMany ? REG_PINFLAG_B_MANY : 0) |
		   (r->bOutput ? REG_PINFLAG_B_OUTPUT : 0);
	return types_copy(p, r->lpMediaType, r->nMediaTypes);
}

static HRESULT filter_from_rf2(struct qz_filter *f, REFCLSID clsid,
			       LPCWSTR name, const CLSID *category,
			       const REGFILTER2 *rf2)
{
	memset(f, 0, sizeof(*f));
	f->clsid = *clsid;
	f->category = category ? *category : CLSID_LegacyAmFilterCategory;
	f->merit = rf2->dwMerit;
	if (!(f->name = wstrdup(name)))
		return E_OUTOFMEMORY;

	ULONG n = rf2->dwVersion == 1 ? rf2->cPins : rf2->cPins2;
	if (n && !(rf2->dwVersion == 1 ? (const void *)rf2->rgPins :
					 (const void *)rf2->rgPins2))
		return E_POINTER;
	if (n && !(f->pins = calloc(n, sizeof(*f->pins))))
		return E_OUTOFMEMORY;
	for (ULONG i = 0; i < n; i++, f->npins++) {
		int bad = rf2->dwVersion == 1 ?
			  pin_from_rf1(&f->pins[i], &rf2->rgPins[i]) :
			  pin_from_rf2(&f->pins[i], &rf2->rgPins2[i]);
		if (bad) {
			f->npins++;
			return E_OUTOFMEMORY;
		}
	}
	return S_OK;
}

/* FilterData. */

struct header {
	DWORD version;
	DWORD merit;
	DWORD pins;
	DWORD unused;
};

struct pin_header {
	BYTE signature[4];	/* "0pi3", the first character the pin's number */
	DWORD flags;
	DWORD instances;
	DWORD types;
	DWORD mediums;
	DWORD has_category;	/* then the category's offset */
};

struct type_header {
	BYTE signature[4];	/* "0ty3", numbered the same way */
	DWORD unused;
	DWORD major;		/* offsets of the GUIDs */
	DWORD minor;
};

struct blob {
	BYTE *p;
	size_t len, cap;
	int failed;
};

static size_t blob_add(struct blob *b, const void *data, size_t n)
{
	size_t at = b->len;

	if (b->len + n > b->cap) {
		size_t cap = b->cap ? b->cap * 2 : 256;
		while (cap < b->len + n)
			cap *= 2;
		BYTE *p = realloc(b->p, cap);
		if (!p) {
			b->failed = 1;
			return 0;
		}
		b->p = p;
		b->cap = cap;
	}
	memcpy(b->p + b->len, data, n);
	b->len += n;
	return at;
}

/* data's offset in b, added if it isn't there (as Wine looks for it). */
static DWORD blob_intern(struct blob *b, const void *data, size_t n)
{
	for (size_t i = 0; i + n <= b->len; i++)
		if (!memcmp(b->p + i, data, n))
			return (DWORD)i;
	return (DWORD)blob_add(b, data, n);
}

BYTE *qz_filterdata_write(DWORD merit, const struct qz_pin *pins,
			  size_t npins, DWORD *size)
{
	struct header h = { 2, merit, (DWORD)npins, 0 };
	struct blob main = { 0 }, table = { 0 };

	/* The table of GUIDs and mediums follows the rest. */
	size_t base = sizeof(h);
	for (size_t i = 0; i < npins; i++)
		base += sizeof(struct pin_header) +
			(pins[i].has_category ? sizeof(DWORD) : 0) +
			pins[i].ntypes * sizeof(struct type_header) +
			pins[i].nmediums * sizeof(DWORD);

	blob_add(&main, &h, sizeof(h));
	for (size_t i = 0; i < npins; i++) {
		const struct qz_pin *p = &pins[i];
		struct pin_header ph = {
			{ (BYTE)('0' + i), 'p', 'i', '3' }, p->flags,
			p->instances, (DWORD)p->ntypes, (DWORD)p->nmediums,
			p->has_category ? 1 : 0,
		};
		blob_add(&main, &ph, sizeof(ph));
		if (p->has_category) {
			DWORD at = (DWORD)base +
				   blob_intern(&table, &p->category, sizeof(GUID));
			blob_add(&main, &at, sizeof(at));
		}
		for (size_t j = 0; j < p->ntypes; j++) {
			struct type_header th = {
				{ (BYTE)('0' + j), 't', 'y', '3' }, 0,
				(DWORD)base + blob_intern(&table,
					&p->types[j].major, sizeof(GUID)),
				(DWORD)base + blob_intern(&table,
					&p->types[j].minor, sizeof(GUID)),
			};
			blob_add(&main, &th, sizeof(th));
		}
		for (size_t j = 0; j < p->nmediums; j++) {
			DWORD at = (DWORD)base +
				   blob_intern(&table, &p->mediums[j],
					       sizeof(REGPINMEDIUM));
			blob_add(&main, &at, sizeof(at));
		}
	}
	if (table.len)
		blob_add(&main, table.p, table.len);
	free(table.p);
	if (main.failed || table.failed || main.len != base + table.len) {
		free(main.p);
		return NULL;
	}
	*size = (DWORD)main.len;
	return main.p;
}

/* A GUID-sized (or n-byte) item at offset at, or NULL if out of range. */
static const BYTE *item_at(const BYTE *data, DWORD size, DWORD at, DWORD n)
{
	return at <= size && n <= size - at ? data + at : NULL;
}

int qz_filterdata_parse(const BYTE *data, DWORD size, struct qz_filter *f)
{
	struct header h;
	size_t pos = sizeof(h);

	memset(f, 0, sizeof(*f));
	if (!data || size < sizeof(h))
		return -1;
	memcpy(&h, data, sizeof(h));
	if (h.version != 2 || h.pins > (size - pos) / sizeof(struct pin_header))
		return -1;
	f->merit = h.merit;
	if (h.pins && !(f->pins = calloc(h.pins, sizeof(*f->pins))))
		return -1;

	for (DWORD i = 0; i < h.pins; i++) {
		struct qz_pin *p = &f->pins[f->npins++];
		struct pin_header ph;
		const BYTE *g;
		DWORD at;

		if (size - pos < sizeof(ph))
			goto bad;
		memcpy(&ph, data + pos, sizeof(ph));
		pos += sizeof(ph);
		if (memcmp(ph.signature + 1, "pi3", 3))
			goto bad;
		p->flags = ph.flags;
		p->instances = ph.instances;
		if (ph.has_category) {
			if (size - pos < sizeof(at))
				goto bad;
			memcpy(&at, data + pos, sizeof(at));
			pos += sizeof(at);
			if (!(g = item_at(data, size, at, sizeof(GUID))))
				goto bad;
			p->has_category = 1;
			memcpy(&p->category, g, sizeof(GUID));
		}

		if (ph.types > (size - pos) / sizeof(struct type_header))
			goto bad;
		if (ph.types &&
		    !(p->types = calloc(ph.types, sizeof(*p->types))))
			goto bad;
		for (DWORD j = 0; j < ph.types; j++) {
			struct type_header th;
			const BYTE *major, *minor;
			memcpy(&th, data + pos, sizeof(th));
			pos += sizeof(th);
			if (memcmp(th.signature + 1, "ty3", 3) ||
			    !(major = item_at(data, size, th.major,
					      sizeof(GUID))) ||
			    !(minor = item_at(data, size, th.minor,
					      sizeof(GUID))))
				goto bad;
			memcpy(&p->types[j].major, major, sizeof(GUID));
			memcpy(&p->types[j].minor, minor, sizeof(GUID));
			p->ntypes++;
		}

		if (ph.mediums > (size - pos) / sizeof(DWORD))
			goto bad;
		if (ph.mediums &&
		    !(p->mediums = calloc(ph.mediums, sizeof(*p->mediums))))
			goto bad;
		for (DWORD j = 0; j < ph.mediums; j++) {
			memcpy(&at, data + pos, sizeof(at));
			pos += sizeof(at);
			if (!(g = item_at(data, size, at, sizeof(REGPINMEDIUM))))
				goto bad;
			memcpy(&p->mediums[j], g, sizeof(REGPINMEDIUM));
			p->nmediums++;
		}
	}
	return 0;
bad:
	qz_filter_clear(f);
	return -1;
}

/* Capture mode. */

static pthread_mutex_t capture_lock = PTHREAD_MUTEX_INITIALIZER;
static int capturing;
static struct qz_filter *captured;
static size_t ncaptured;

void quartz_capture_begin(void)
{
	pthread_mutex_lock(&capture_lock);
	qz_filters_free(captured, ncaptured);
	captured = NULL;
	ncaptured = 0;
	capturing = 1;
	pthread_mutex_unlock(&capture_lock);
}

size_t quartz_capture_end(struct qz_filter **out)
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

/* Records f (taking what it holds) if capturing; otherwise clears it. */
static void capture_add(struct qz_filter *f)
{
	pthread_mutex_lock(&capture_lock);
	if (capturing) {
		struct qz_filter *c = realloc(captured,
					      (ncaptured + 1) * sizeof(*c));
		if (c) {
			captured = c;
			captured[ncaptured++] = *f;
			memset(f, 0, sizeof(*f));
		}
	}
	pthread_mutex_unlock(&capture_lock);
	qz_filter_clear(f);
}

/*
 * The captured legacy registration of clsid (the capture lock held), or
 * NULL.
 */
static struct qz_filter *capture_find_legacy(REFCLSID clsid)
{
	for (size_t i = ncaptured; i-- > 0;)
		if (guid_eq(&captured[i].clsid, clsid) &&
		    guid_eq(&captured[i].category,
			    &CLSID_LegacyAmFilterCategory))
			return &captured[i];
	return NULL;
}

/* Drops the captured registrations of clsid in category. */
static void capture_remove(REFCLSID clsid, const CLSID *category)
{
	pthread_mutex_lock(&capture_lock);
	for (size_t i = 0; i < ncaptured;) {
		if (guid_eq(&captured[i].clsid, clsid) &&
		    guid_eq(&captured[i].category, category)) {
			qz_filter_clear(&captured[i]);
			memmove(&captured[i], &captured[i + 1],
				(ncaptured - i - 1) * sizeof(*captured));
			ncaptured--;
		} else {
			i++;
		}
	}
	pthread_mutex_unlock(&capture_lock);
}

/* The registry. */

static HRESULT hr_from_win32(LONG err)
{
	return err ? HRESULT_FROM_WIN32(err) : S_OK;
}

static LONG set_sz(struct reg_key *k, const char *name, const WCHAR *s)
{
	return reg_set(k, name, REG_SZ, s,
		       (DWORD)((wstrlen(s) + 1) * sizeof(WCHAR)));
}

static LONG set_sz8(struct reg_key *k, const char *name, const char *s)
{
	WCHAR *w = utf8_to_utf16_dup(s);
	LONG err = w ? set_sz(k, name, w) : ERROR_NOT_ENOUGH_MEMORY;

	free(w);
	return err;
}

static LONG set_dword(struct reg_key *k, const char *name, DWORD v)
{
	return reg_set(k, name, REG_DWORD, &v, sizeof(v));
}

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

static HRESULT delete_path(const char *path)
{
	reg_lock();
	struct reg_key *k = reg_open(path);
	LONG err = k ? delete_tree(k) : ERROR_FILE_NOT_FOUND;
	if (!err)
		err = reg_save();
	reg_unlock();
	return hr_from_win32(err);
}

static void guid_string(const GUID *g, char out[OLE_GUID_CHARS])
{
	ole_guid_format(g, out);
}

/* The filter mappers: one object, with IFilterMapper2 and IFilterMapper. */

struct mapper {
	IFilterMapper2 fm2;
	IFilterMapper fm1;
	LONG refs;
};

static struct mapper *from_fm2(IFilterMapper2 *i)
{
	return (struct mapper *)i;
}

static struct mapper *from_fm1(IFilterMapper *i)
{
	return (struct mapper *)((BYTE *)i - offsetof(struct mapper, fm1));
}

static HRESULT mapper_qi(struct mapper *m, REFIID riid, void **ppv)
{
	if (!ppv)
		return E_POINTER;
	*ppv = NULL;
	if (!riid)
		return E_INVALIDARG;
	if (guid_eq(riid, &IID_IUnknown) || guid_eq(riid, &IID_IFilterMapper2))
		*ppv = &m->fm2;
	else if (guid_eq(riid, &IID_IFilterMapper))
		*ppv = &m->fm1;
	else
		return E_NOINTERFACE;
	__atomic_add_fetch(&m->refs, 1, __ATOMIC_SEQ_CST);
	return S_OK;
}

static ULONG mapper_release(struct mapper *m)
{
	ULONG r = __atomic_sub_fetch(&m->refs, 1, __ATOMIC_SEQ_CST);

	if (!r) {
		free(m);
		object_gone();
	}
	return r;
}

WINFN(HRESULT) fm2_QueryInterface(IFilterMapper2 *self, REFIID riid,
				  void **ppv)
{
	return mapper_qi(from_fm2(self), riid, ppv);
}

WINFN(ULONG) fm2_AddRef(IFilterMapper2 *self)
{
	return __atomic_add_fetch(&from_fm2(self)->refs, 1, __ATOMIC_SEQ_CST);
}

WINFN(ULONG) fm2_Release(IFilterMapper2 *self)
{
	return mapper_release(from_fm2(self));
}

WINFN(HRESULT) fm2_CreateCategory(IFilterMapper2 *self, REFCLSID category,
				  DWORD merit, LPCWSTR description)
{
	char g[OLE_GUID_CHARS], cat[OLE_GUID_CHARS], path[256];
	LONG err;

	if (!category || !description)
		return E_POINTER;
	guid_string(&CLSID_ActiveMovieCategories, cat);
	guid_string(category, g);
	snprintf(path, sizeof(path), CLASSES "CLSID\\%s\\Instance\\%s", cat, g);

	reg_lock();
	struct reg_key *k = reg_create(path, 0, NULL, &err);
	if (k && !(err = set_sz(k, "FriendlyName", description)) &&
	    !(err = set_sz8(k, "CLSID", g)) &&
	    !(err = set_dword(k, "Merit", merit)))
		err = reg_save();
	reg_unlock();
	TRACE("IFilterMapper2::CreateCategory(%s, 0x%lx) = %ld", g,
	      (unsigned long)merit, (long)err);
	return hr_from_win32(err);
}

/* "CLSID\{category}\Instance\{instance}", the instance name in UTF-8. */
static int instance_path(char *path, size_t size, const CLSID *category,
			 LPCOLESTR instance, REFCLSID filter)
{
	char cat[OLE_GUID_CHARS], g[OLE_GUID_CHARS];
	char *inst = NULL;

	guid_string(category ? category : &CLSID_LegacyAmFilterCategory, cat);
	if (instance && !(inst = utf16_to_utf8_dup(instance)))
		return -1;
	if (!inst)
		guid_string(filter, g);
	snprintf(path, size, CLASSES "CLSID\\%s\\Instance\\%s", cat,
		 inst ? inst : g);
	free(inst);
	return 0;
}

WINFN(HRESULT) fm2_UnregisterFilter(IFilterMapper2 *self,
				    const CLSID *category, LPCOLESTR instance,
				    REFCLSID filter)
{
	char path[512];

	if (!filter)
		return E_POINTER;
	if (instance_path(path, sizeof(path), category, instance, filter))
		return E_OUTOFMEMORY;
	HRESULT hr = delete_path(path);
	capture_remove(filter, category ? category :
			       &CLSID_LegacyAmFilterCategory);
	TRACE("IFilterMapper2::UnregisterFilter(%s) = 0x%08lx", path,
	      (unsigned long)hr);
	return hr;
}

/*
 * Writes a version 2 FilterData whatever the version given, as Windows
 * does; a moniker for the new registration isn't made (*moniker is NULL).
 */
WINFN(HRESULT) fm2_RegisterFilter(IFilterMapper2 *self, REFCLSID filter,
				  LPCWSTR name, IMoniker **moniker,
				  const CLSID *category, LPCOLESTR instance,
				  const REGFILTER2 *rf2)
{
	char path[512], g[OLE_GUID_CHARS];
	struct qz_filter f;
	DWORD size = 0;
	LONG err;

	if (moniker)
		*moniker = NULL;
	if (!filter || !name || !rf2)
		return E_POINTER;
	if (rf2->dwVersion != 1 && rf2->dwVersion != 2)
		return E_INVALIDARG;
	HRESULT hr = filter_from_rf2(&f, filter, name, category, rf2);
	BYTE *data = SUCCEEDED(hr) ?
		     qz_filterdata_write(f.merit, f.pins, f.npins, &size) : NULL;
	if (FAILED(hr) || !data ||
	    instance_path(path, sizeof(path), category, instance, filter)) {
		free(data);
		qz_filter_clear(&f);
		return FAILED(hr) ? hr : E_OUTOFMEMORY;
	}
	guid_string(filter, g);

	reg_lock();
	struct reg_key *k = reg_create(path, 0, NULL, &err);
	if (k && !(err = set_sz(k, "FriendlyName", name)) &&
	    !(err = set_sz8(k, "CLSID", g)) &&
	    !(err = reg_set(k, "FilterData", REG_BINARY, data, size)))
		err = reg_save();
	reg_unlock();
	free(data);

	char *n = utf16_to_utf8_dup(name);
	TRACE("IFilterMapper2::RegisterFilter(%s \"%s\", merit 0x%lx, %lu "
	      "pin(s)) at %s = %ld", g, n ? n : "", (unsigned long)f.merit,
	      (unsigned long)f.npins, path, (long)err);
	free(n);
	if (err) {
		qz_filter_clear(&f);
		return hr_from_win32(err);
	}
	/* Registering again replaces the registration. */
	capture_remove(filter, &f.category);
	capture_add(&f);
	return S_OK;
}

WINFN(HRESULT) fm2_EnumMatchingFilters(IFilterMapper2 *self,
				       IEnumMoniker **e, DWORD flags,
				       BOOL exact_match, DWORD merit,
				       BOOL input_needed, DWORD n_input_types,
				       const GUID *input_types,
				       const REGPINMEDIUM *medium_in,
				       const CLSID *pin_category_in,
				       BOOL render, BOOL output_needed,
				       DWORD n_output_types,
				       const GUID *output_types,
				       const REGPINMEDIUM *medium_out,
				       const CLSID *pin_category_out)
{
	if (e)
		*e = NULL;
	vfw_warn("quartz: IFilterMapper2::EnumMatchingFilters is not "
		 "implemented (there is no filter graph)");
	return E_NOTIMPL;
}

static const IFilterMapper2Vtbl fm2_vtbl = {
	fm2_QueryInterface, fm2_AddRef, fm2_Release, fm2_CreateCategory,
	fm2_UnregisterFilter, fm2_RegisterFilter, fm2_EnumMatchingFilters,
};

/*
 * The legacy mapper, which keeps a filter under HKCR\Filter\{clsid} and
 * its pins under HKCR\CLSID\{clsid}\Pins.
 */

WINFN(HRESULT) fm1_QueryInterface(IFilterMapper *self, REFIID riid, void **ppv)
{
	return mapper_qi(from_fm1(self), riid, ppv);
}

WINFN(ULONG) fm1_AddRef(IFilterMapper *self)
{
	return __atomic_add_fetch(&from_fm1(self)->refs, 1, __ATOMIC_SEQ_CST);
}

WINFN(ULONG) fm1_Release(IFilterMapper *self)
{
	return mapper_release(from_fm1(self));
}

WINFN(HRESULT) fm1_RegisterFilter(IFilterMapper *self, CLSID clsid,
				  LPCWSTR name, DWORD merit)
{
	char g[OLE_GUID_CHARS], path[256];
	LONG err;

	if (!name)
		return E_POINTER;
	guid_string(&clsid, g);

	reg_lock();
	snprintf(path, sizeof(path), CLASSES "Filter\\%s", g);
	struct reg_key *k = reg_create(path, 0, NULL, &err);
	if (k && !(err = set_sz(k, "", name))) {
		snprintf(path, sizeof(path), CLASSES "CLSID\\%s", g);
		if ((k = reg_create(path, 0, NULL, &err)) &&
		    !(err = set_dword(k, "Merit", merit)))
			err = reg_save();
	}
	reg_unlock();
	TRACE("IFilterMapper::RegisterFilter(%s, merit 0x%lx) = %ld", g,
	      (unsigned long)merit, (long)err);
	if (err)
		return hr_from_win32(err);

	struct qz_filter f = {
		.clsid = clsid, .category = CLSID_LegacyAmFilterCategory,
		.merit = merit, .name = wstrdup(name),
	};
	capture_remove(&clsid, &CLSID_LegacyAmFilterCategory);
	capture_add(&f);
	return S_OK;
}

WINFN(HRESULT) fm1_RegisterFilterInstance(IFilterMapper *self, CLSID clsid,
					  LPCWSTR name, CLSID *mrid)
{
	return E_NOTIMPL;
}

/* "CLSID\{filter}\Pins\name", or -1 if the name doesn't convert. */
static int pin_path(char *path, size_t size, const CLSID *filter,
		    LPCWSTR name)
{
	char g[OLE_GUID_CHARS];
	char *n = utf16_to_utf8_dup(name);

	if (!n)
		return -1;
	guid_string(filter, g);
	snprintf(path, size, CLASSES "CLSID\\%s\\Pins\\%s", g, n);
	free(n);
	return 0;
}

WINFN(HRESULT) fm1_RegisterPin(IFilterMapper *self, CLSID filter,
			       LPCWSTR name, BOOL rendered, BOOL output,
			       BOOL zero, BOOL many, CLSID connects_to_filter,
			       LPCWSTR connects_to_pin)
{
	char path[512];
	LONG err;

	if (!name)
		return E_POINTER;
	if (pin_path(path, sizeof(path), &filter, name))
		return E_OUTOFMEMORY;

	reg_lock();
	struct reg_key *k = reg_create(path, 0, NULL, &err);
	if (k && !(err = set_dword(k, "AllowedMany", many)) &&
	    !(err = set_dword(k, "AllowedZero", zero)) &&
	    !(err = set_dword(k, "Direction", output)) &&
	    !(err = set_dword(k, "IsRendered", rendered))) {
		strncat(path, "\\Types", sizeof(path) - strlen(path) - 1);
		if (reg_create(path, 0, NULL, &err))
			err = reg_save();
	}
	reg_unlock();
	if (err)
		return hr_from_win32(err);

	pthread_mutex_lock(&capture_lock);
	struct qz_filter *f = capturing ? capture_find_legacy(&filter) : NULL;
	struct qz_pin *pins = f ? realloc(f->pins, (f->npins + 1) *
					  sizeof(*pins)) : NULL;
	if (pins) {
		f->pins = pins;
		struct qz_pin *p = &pins[f->npins];
		memset(p, 0, sizeof(*p));
		if ((p->name = wstrdup(name))) {
			p->flags = (zero ? REG_PINFLAG_B_ZERO : 0) |
				   (rendered ? REG_PINFLAG_B_RENDERER : 0) |
				   (many ? REG_PINFLAG_B_MANY : 0) |
				   (output ? REG_PINFLAG_B_OUTPUT : 0);
			f->npins++;
		}
	}
	pthread_mutex_unlock(&capture_lock);
	return S_OK;
}

WINFN(HRESULT) fm1_RegisterPinType(IFilterMapper *self, CLSID filter,
				   LPCWSTR pin, CLSID major, CLSID sub)
{
	char path[512], g1[OLE_GUID_CHARS], g2[OLE_GUID_CHARS];
	LONG err;

	if (!pin)
		return E_POINTER;
	if (pin_path(path, sizeof(path), &filter, pin))
		return E_OUTOFMEMORY;
	guid_string(&major, g1);
	guid_string(&sub, g2);
	size_t len = strlen(path);
	snprintf(path + len, sizeof(path) - len, "\\Types\\%s\\%s", g1, g2);

	reg_lock();
	if (reg_create(path, 0, NULL, &err))
		err = reg_save();
	reg_unlock();
	if (err)
		return hr_from_win32(err);

	pthread_mutex_lock(&capture_lock);
	struct qz_filter *f = capturing ? capture_find_legacy(&filter) : NULL;
	for (size_t i = 0; f && i < f->npins; i++) {
		struct qz_pin *p = &f->pins[i];
		if (!p->name || !wide_eq_nocase(p->name, pin))
			continue;
		struct qz_type *t = realloc(p->types,
					    (p->ntypes + 1) * sizeof(*t));
		if (t) {
			p->types = t;
			t[p->ntypes].major = major;
			t[p->ntypes].minor = sub;
			p->ntypes++;
		}
		break;
	}
	pthread_mutex_unlock(&capture_lock);
	return S_OK;
}

WINFN(HRESULT) fm1_UnregisterFilter(IFilterMapper *self, CLSID filter)
{
	char g[OLE_GUID_CHARS], path[256];

	guid_string(&filter, g);
	snprintf(path, sizeof(path), CLASSES "Filter\\%s", g);
	HRESULT hr = delete_path(path);
	if (SUCCEEDED(hr)) {
		reg_lock();
		snprintf(path, sizeof(path), CLASSES "CLSID\\%s", g);
		struct reg_key *k = reg_open(path);
		if (k) {
			reg_unset(k, "Merit");
			strncat(path, "\\Pins", sizeof(path) - strlen(path) - 1);
			struct reg_key *pins = reg_open(path);
			if (pins)
				delete_tree(pins);
			reg_save();
		}
		reg_unlock();
	}
	capture_remove(&filter, &CLSID_LegacyAmFilterCategory);
	return hr;
}

WINFN(HRESULT) fm1_UnregisterFilterInstance(IFilterMapper *self, CLSID mrid)
{
	return E_NOTIMPL;
}

WINFN(HRESULT) fm1_UnregisterPin(IFilterMapper *self, CLSID filter,
				 LPCWSTR name)
{
	char path[512];

	if (!name)
		return E_INVALIDARG;
	if (pin_path(path, sizeof(path), &filter, name))
		return E_OUTOFMEMORY;
	return delete_path(path);
}

WINFN(HRESULT) fm1_EnumMatchingFilters(IFilterMapper *self,
				       IEnumRegFilters **e, DWORD merit,
				       BOOL input_needed, CLSID in_major,
				       CLSID in_sub, BOOL render,
				       BOOL output_needed, CLSID out_major,
				       CLSID out_sub)
{
	if (e)
		*e = NULL;
	vfw_warn("quartz: IFilterMapper::EnumMatchingFilters is not "
		 "implemented (there is no filter graph)");
	return E_NOTIMPL;
}

static const IFilterMapperVtbl fm1_vtbl = {
	fm1_QueryInterface, fm1_AddRef, fm1_Release, fm1_RegisterFilter,
	fm1_RegisterFilterInstance, fm1_RegisterPin, fm1_RegisterPinType,
	fm1_UnregisterFilter, fm1_UnregisterFilterInstance, fm1_UnregisterPin,
	fm1_EnumMatchingFilters,
};

static HRESULT mapper_create(REFIID riid, void **ppv)
{
	struct mapper *m = calloc(1, sizeof(*m));

	if (!m)
		return E_OUTOFMEMORY;
	m->fm2.lpVtbl = &fm2_vtbl;
	m->fm1.lpVtbl = &fm1_vtbl;
	m->refs = 1;
	object_added();
	HRESULT hr = mapper_qi(m, riid, ppv);
	mapper_release(m);
	return hr;
}

/* The system device enumerator: every category is empty. */

struct devenum {
	ICreateDevEnum iface;
	LONG refs;
};

WINFN(HRESULT) de_QueryInterface(ICreateDevEnum *self, REFIID riid, void **ppv)
{
	if (!ppv)
		return E_POINTER;
	*ppv = NULL;
	if (!riid)
		return E_INVALIDARG;
	if (!guid_eq(riid, &IID_IUnknown) && !guid_eq(riid, &IID_ICreateDevEnum))
		return E_NOINTERFACE;
	self->lpVtbl->AddRef(self);
	*ppv = self;
	return S_OK;
}

WINFN(ULONG) de_AddRef(ICreateDevEnum *self)
{
	return __atomic_add_fetch(&((struct devenum *)self)->refs, 1,
				  __ATOMIC_SEQ_CST);
}

WINFN(ULONG) de_Release(ICreateDevEnum *self)
{
	ULONG r = __atomic_sub_fetch(&((struct devenum *)self)->refs, 1,
				     __ATOMIC_SEQ_CST);
	if (!r) {
		free(self);
		object_gone();
	}
	return r;
}

/* S_FALSE and no enumerator, as Windows answers for an empty category. */
WINFN(HRESULT) de_CreateClassEnumerator(ICreateDevEnum *self,
					REFCLSID category, IEnumMoniker **e,
					DWORD flags)
{
	if (!e)
		return E_POINTER;
	*e = NULL;
	if (!category)
		return E_POINTER;
	char g[OLE_GUID_CHARS];
	guid_string(category, g);
	TRACE("ICreateDevEnum::CreateClassEnumerator(%s): no devices", g);
	return S_FALSE;
}

static const ICreateDevEnumVtbl devenum_vtbl = {
	de_QueryInterface, de_AddRef, de_Release, de_CreateClassEnumerator,
};

static HRESULT devenum_create(REFIID riid, void **ppv)
{
	struct devenum *d = calloc(1, sizeof(*d));

	if (!d)
		return E_OUTOFMEMORY;
	d->iface.lpVtbl = &devenum_vtbl;
	d->refs = 1;
	object_added();
	HRESULT hr = de_QueryInterface(&d->iface, riid, ppv);
	de_Release(&d->iface);
	return hr;
}

/* Class factories, one static object per class. */

struct factory {
	IClassFactory iface;
	const CLSID *clsid;
	HRESULT (*create)(REFIID riid, void **ppv);
};

WINFN(HRESULT) cf_QueryInterface(IClassFactory *self, REFIID riid, void **ppv)
{
	if (!ppv)
		return E_POINTER;
	if (riid && (guid_eq(riid, &IID_IUnknown) ||
		     guid_eq(riid, &IID_IClassFactory))) {
		*ppv = self;
		return S_OK;
	}
	*ppv = NULL;
	return E_NOINTERFACE;
}

WINFN(ULONG) cf_AddRef(IClassFactory *self) { return 2; }
WINFN(ULONG) cf_Release(IClassFactory *self) { return 1; }

WINFN(HRESULT) cf_CreateInstance(IClassFactory *self, IUnknown *outer,
				 REFIID riid, void **ppv)
{
	if (!ppv)
		return E_POINTER;
	*ppv = NULL;
	if (outer)
		return CLASS_E_NOAGGREGATION;
	return ((struct factory *)self)->create(riid, ppv);
}

WINFN(HRESULT) cf_LockServer(IClassFactory *self, BOOL lock)
{
	__atomic_add_fetch(&server_locks, lock ? 1 : -1, __ATOMIC_SEQ_CST);
	return S_OK;
}

static const IClassFactoryVtbl factory_vtbl = {
	cf_QueryInterface, cf_AddRef, cf_Release, cf_CreateInstance,
	cf_LockServer,
};

static struct factory factories[] = {
	{ { &factory_vtbl }, &CLSID_FilterMapper2, mapper_create },
	{ { &factory_vtbl }, &CLSID_FilterMapper, mapper_create },
	{ { &factory_vtbl }, &CLSID_SystemDeviceEnum, devenum_create },
};

WINFN(HRESULT) qz_DllGetClassObject(REFCLSID clsid, REFIID riid, LPVOID *ppv)
{
	if (!ppv)
		return E_POINTER;
	*ppv = NULL;
	if (!clsid || !riid)
		return E_INVALIDARG;
	for (size_t i = 0; i < sizeof(factories) / sizeof(*factories); i++)
		if (guid_eq(factories[i].clsid, clsid))
			return cf_QueryInterface(&factories[i].iface, riid, ppv);
	return CLASS_E_CLASSNOTAVAILABLE;
}

WINFN(HRESULT) qz_DllCanUnloadNow(void)
{
	return __atomic_load_n(&objects, __ATOMIC_SEQ_CST) ||
	       __atomic_load_n(&server_locks, __ATOMIC_SEQ_CST) ? S_FALSE : S_OK;
}

/* The built-in classes need no registration. */
WINFN(HRESULT) qz_DllRegisterServer(void) { return S_OK; }
WINFN(HRESULT) qz_DllUnregisterServer(void) { return S_OK; }

/* Error text. */

#include "quartz_errors.h"

/*
 * The text for hr: a description where there is one, then the code's
 * name in brackets, or "Error 0x...". It is copied only if it fits with
 * its NUL; returns its length, or 0.
 */
static const char *error_text(HRESULT hr, char *buf, size_t size)
{
	for (size_t i = 0; i < sizeof(qz_errors) / sizeof(*qz_errors); i++) {
		if (qz_errors[i].hr != hr)
			continue;
		if (qz_errors[i].text)
			snprintf(buf, size, "%s (%s)", qz_errors[i].text,
				 qz_errors[i].name);
		else
			snprintf(buf, size, "%s", qz_errors[i].name);
		return buf;
	}
	snprintf(buf, size, "Error 0x%08lX.", (unsigned long)hr);
	return buf;
}

WINFN(DWORD) qz_AMGetErrorTextA(HRESULT hr, LPSTR buf, DWORD max)
{
	char text[256];

	if (!buf)
		return 0;
	size_t n = strlen(error_text(hr, text, sizeof(text)));
	if (n >= max)
		return 0;
	memcpy(buf, text, n + 1);
	return (DWORD)n;
}

WINFN(DWORD) qz_AMGetErrorTextW(HRESULT hr, LPWSTR buf, DWORD max)
{
	char text[256];

	if (!buf)
		return 0;
	WCHAR *w = utf8_to_utf16_dup(error_text(hr, text, sizeof(text)));
	size_t n = w ? wstrlen(w) : 0;
	if (!w || n >= max) {
		free(w);
		return 0;
	}
	memcpy(buf, w, (n + 1) * sizeof(WCHAR));
	free(w);
	return (DWORD)n;
}

#define E(name) { #name, 0, (void *)qz_##name }

static const struct builtin_export exports[] = {
	E(AMGetErrorTextA),
	E(AMGetErrorTextW),
	E(DllCanUnloadNow),
	E(DllGetClassObject),
	E(DllRegisterServer),
	E(DllUnregisterServer),
	BUILTIN_END
};

static const struct builtin_export *const tables[] = { exports, NULL };

const struct builtin_dll builtin_quartz = { "quartz.dll", NULL, tables, 0 };
