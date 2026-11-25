/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * oleaut32's type library creation: CreateTypeLib and CreateTypeLib2,
 * with ICreateTypeLib2 and ICreateTypeInfo2, writing MSFT files that
 * oleaut_typelib.c reads.
 *
 * - The library is built in memory and written by SaveAllChanges: the
 *   header, the type infos' offsets, the segment directory, the segments
 *   (type infos, imports, GUIDs, names, strings, type and array
 *   descriptions, custom data) and each type's member records, in the
 *   layout the reader is written from. The name and GUID hash tables are
 *   left empty: nothing here looks names up through them.
 * - LayOut (also done when saving) gives an interface's functions their
 *   vtable offsets after its base's, a dispatch interface IDispatch's
 *   vtable, and records and unions their members' offsets and their size
 *   and alignment (natural alignment, at most SetAlignment's or 8).
 * - References to other libraries' types (AddRefTypeInfo) become imports,
 *   by the type's GUID; stdole2's import file is stdole2.tlb, another
 *   registered library's its registered file.
 * - The ITypeLib and ITypeInfo the objects give out through
 *   QueryInterface are snapshots: the library as it would be saved then.
 *   Each is a view of its creator, sharing its COM identity: it holds the
 *   creator, and IUnknown or ICreateTypeLib2/ICreateTypeInfo2 asked of it
 *   come from the creator. The creator keeps none of them.
 * - A dual interface (TYPEFLAG_FDUAL on a TKIND_INTERFACE) is stored in
 *   the dispatch form, as MIDL stores one: the reader shows it as the
 *   dispatch interface, and its interface half by GetRefTypeOfImplType(-1).
 * - Dispatch interfaces derive from IDispatch: when no type here refers
 *   to it already, an import of stdole2's IDispatch is written for them.
 * - The oa_ctl_* functions at the end let other readers (oleaut_sltg.c)
 *   build a library here: references to other libraries' types by GUID or
 *   index without loading them, layouts given rather than worked out, and
 *   the MSFT bytes without a file.
 */
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "common/log.h"
#include "win32/codepage.h"
#include "win32/internal.h"
#include "win32/oleaut.h"
#include "win32/path.h"

#define MSFT_MAGIC 0x5446534d
#define MSFT_HEADER_SIZE 0x54
#define MSFT_TYPEINFO_SIZE 0x64
#define MSFT_HELPDLL 0x100

enum {
	SEG_TYPEINFO, SEG_IMPINFO, SEG_IMPFILES, SEG_REFTAB, SEG_GUIDHASH,
	SEG_GUID, SEG_NAMEHASH, SEG_NAME, SEG_STRING, SEG_TYPEDESC,
	SEG_ARRAYDESC, SEG_CUSTDATA, SEG_CDGUIDS, SEG_RES0E, SEG_RES0F,
	SEG_COUNT
};

static const GUID null_guid;
static const GUID stdole2_libid = { 0x00020430, 0x0000, 0x0000,
				    { 0xc0, 0, 0, 0, 0, 0, 0, 0x46 } };
static const GUID iid_unknown = { 0, 0, 0,
				  { 0xc0, 0, 0, 0, 0, 0, 0, 0x46 } };
static const GUID iid_dispatch = { 0x00020400, 0, 0,
				   { 0xc0, 0, 0, 0, 0, 0, 0, 0x46 } };

static int guid_eq(const GUID *a, const GUID *b)
{
	return !memcmp(a, b, sizeof(GUID));
}

static WCHAR *wdup(const WCHAR *s)
{
	if (!s)
		return NULL;
	size_t n = wstrlen(s);
	WCHAR *w = malloc((n + 1) * sizeof(WCHAR));
	if (w)
		memcpy(w, s, (n + 1) * sizeof(WCHAR));
	return w;
}

static int set_str(WCHAR **dst, const WCHAR *s)
{
	WCHAR *w = NULL;
	if (s && !(w = wdup(s)))
		return -1;
	free(*dst);
	*dst = w;
	return 0;
}

/* ---- the model ---- */

struct c_cust {
	struct c_cust *next;
	GUID guid;
	VARIANT value;
};

struct c_param {
	WCHAR *name;
	TYPEDESC type;
	USHORT flags;
	VARIANT def;		/* with PARAMFLAG_FHASDEFAULT */
	struct c_cust *cust;
};

struct c_func {
	MEMBERID memid;
	FUNCKIND funckind;
	INVOKEKIND invkind;
	CALLCONV callconv;
	SHORT nparams, nopt, oVft;
	SHORT given_vft;	/* the caller's oVft, kept by LayOut; or 0 */
	WORD flags;
	TYPEDESC ret;
	struct c_param *params;
	WCHAR *name, *doc, *entry;
	int ordinal;		/* the entry's ordinal, or -1 */
	DWORD help_ctx, help_str_ctx;
	struct c_cust *cust;
};

struct c_var {
	MEMBERID memid;
	VARKIND varkind;
	WORD flags;
	TYPEDESC type;
	ULONG oInst;
	VARIANT value;		/* VAR_CONST */
	WCHAR *name, *doc;
	DWORD help_ctx, help_str_ctx;
	struct c_cust *cust;
};

struct c_impl {
	HREFTYPE href;
	INT flags;
	struct c_cust *cust;
};

struct ctl;

struct c_type {
	ICreateTypeInfo2 iface;
	struct ctl *lib;
	TYPEKIND kind;
	WCHAR *name, *doc, *dll;
	GUID guid;
	WORD flags, major, minor, align_set;
	DWORD help_ctx, help_str_ctx;
	struct c_cust *cust;
	TYPEDESC alias;
	int has_alias;
	struct c_func *funcs;
	UINT nfuncs;
	struct c_var *vars;
	UINT nvars;
	struct c_impl *impls;
	UINT nimpls;
	/* LayOut's results, or given (fixed) */
	ULONG size;
	WORD align, vft;
	int fixed;
	int laying;		/* to catch a record that contains itself */
	int deleted;		/* DeleteTypeInfo took it out of the library */
};

/* A type of another library that a type here refers to. */
struct c_ref {
	ITypeInfo *info;	/* held; NULL for one oa_ctl_import made, or
				 * for one from another creator (see attr) */
	int have_attr;		/* attr and depth are set */
	TYPEATTR attr;		/* its attributes when it was added */
	int depth;		/* its count of base interfaces */
	GUID libid, guid;
	LCID lcid;
	WORD major, minor;
	TYPEKIND kind;
	UINT index;		/* in its library */
	char *file;
};

struct ctl {
	ICreateTypeLib2 iface;
	LONG refs;
	char *path;		/* Windows path */
	SYSKIND sys;
	WCHAR *name, *doc, *help_file, *help_dll;
	GUID guid;
	LCID lcid;
	WORD major, minor, flags;
	DWORD help_ctx, help_str_ctx;
	struct c_cust *cust;
	struct c_type **types;
	UINT ntypes;
	struct c_type **dead;	/* deleted, kept for their interfaces */
	UINT ndead;
	struct c_ref *refs_out;
	UINT nrefs;
	BYTE *last;		/* the last view's image, or NULL */
	size_t last_len;
};

static const ICreateTypeLib2Vtbl ctl_vtbl;
static const ICreateTypeInfo2Vtbl cti_vtbl;

static struct ctl *ctl_of(ICreateTypeLib2 *i)
{
	return (struct ctl *)i;
}

static struct c_type *cti_of(ICreateTypeInfo2 *i)
{
	return (struct c_type *)i;
}

static int ptr_size(const struct ctl *lib)
{
	return lib->sys == SYS_WIN64 ? 8 : 4;
}

/* ---- deep copies ---- */

static void td_free(TYPEDESC *td)
{
	if (td->vt == VT_PTR || td->vt == VT_SAFEARRAY) {
		if (td->lptdesc) {
			td_free(td->lptdesc);
			free(td->lptdesc);
		}
	} else if (td->vt == VT_CARRAY && td->lpadesc) {
		td_free(&td->lpadesc->tdescElem);
		free(td->lpadesc);
	}
	td->vt = VT_EMPTY;
}

static HRESULT td_copy(TYPEDESC *dst, const TYPEDESC *src, int depth)
{
	memset(dst, 0, sizeof(*dst));
	if (!src || depth > 32)
		return E_INVALIDARG;
	dst->vt = src->vt;
	switch (src->vt) {
	case VT_PTR:
	case VT_SAFEARRAY:
		if (!src->lptdesc)
			return E_INVALIDARG;
		if (!(dst->lptdesc = malloc(sizeof(TYPEDESC))))
			return E_OUTOFMEMORY;
		return td_copy(dst->lptdesc, src->lptdesc, depth + 1);
	case VT_CARRAY: {
		const ARRAYDESC *a = src->lpadesc;
		if (!a || !a->cDims)
			return E_INVALIDARG;
		size_t sz = sizeof(ARRAYDESC) +
			    (a->cDims - 1) * sizeof(SAFEARRAYBOUND);
		if (!(dst->lpadesc = malloc(sz)))
			return E_OUTOFMEMORY;
		memcpy(dst->lpadesc, a, sz);
		dst->lpadesc->tdescElem.vt = VT_EMPTY;
		return td_copy(&dst->lpadesc->tdescElem, &a->tdescElem,
			       depth + 1);
	}
	case VT_USERDEFINED:
		dst->hreftype = src->hreftype;
		return S_OK;
	default:
		if ((src->vt & ~VT_TYPEMASK) || src->vt > VT_LPWSTR + 10)
			return E_INVALIDARG;
		return S_OK;
	}
}

static void cust_free(struct c_cust *c)
{
	while (c) {
		struct c_cust *n = c->next;
		oa_VariantClear(&c->value);
		free(c);
		c = n;
	}
}

/* Sets a custom value, replacing one with the same GUID. */
static HRESULT cust_set(struct c_cust **list, REFGUID guid, VARIANT *value)
{
	struct c_cust *c;
	HRESULT hr;

	if (!guid || !value)
		return E_INVALIDARG;
	switch (value->vt) {
	case VT_EMPTY: case VT_NULL: case VT_I1: case VT_UI1: case VT_I2:
	case VT_UI2: case VT_BOOL: case VT_I4: case VT_UI4: case VT_R4:
	case VT_INT: case VT_UINT: case VT_ERROR: case VT_HRESULT:
	case VT_I8: case VT_UI8: case VT_R8: case VT_CY: case VT_DATE:
	case VT_BSTR: case VT_DECIMAL:
		break;
	default:
		return DISP_E_BADVARTYPE;
	}
	/* The copy first: a failure leaves the old value. */
	VARIANT copy;
	oa_VariantInit(&copy);
	if ((hr = oa_VariantCopy(&copy, value)) != S_OK)
		return hr;
	for (c = *list; c; c = c->next)
		if (guid_eq(&c->guid, guid))
			break;
	if (!c) {
		if (!(c = calloc(1, sizeof(*c)))) {
			oa_VariantClear(&copy);
			return E_OUTOFMEMORY;
		}
		c->guid = *guid;
		struct c_cust **end = list;
		while (*end)
			end = &(*end)->next;
		*end = c;
	}
	oa_VariantClear(&c->value);
	c->value = copy;
	return S_OK;
}

static void func_free(struct c_func *f)
{
	td_free(&f->ret);
	for (SHORT i = 0; f->params && i < f->nparams; i++) {
		free(f->params[i].name);
		td_free(&f->params[i].type);
		oa_VariantClear(&f->params[i].def);
		cust_free(f->params[i].cust);
	}
	free(f->params);
	free(f->name);
	free(f->doc);
	free(f->entry);
	cust_free(f->cust);
}

static void var_free(struct c_var *v)
{
	td_free(&v->type);
	oa_VariantClear(&v->value);
	free(v->name);
	free(v->doc);
	cust_free(v->cust);
}

static void type_free(struct c_type *t)
{
	for (UINT i = 0; i < t->nfuncs; i++)
		func_free(&t->funcs[i]);
	for (UINT i = 0; i < t->nvars; i++)
		var_free(&t->vars[i]);
	for (UINT i = 0; i < t->nimpls; i++)
		cust_free(t->impls[i].cust);
	free(t->funcs);
	free(t->vars);
	free(t->impls);
	if (t->has_alias)
		td_free(&t->alias);
	free(t->name);
	free(t->doc);
	free(t->dll);
	cust_free(t->cust);
	free(t);
}

static void ctl_free(struct ctl *lib)
{
	for (UINT i = 0; i < lib->ntypes; i++)
		type_free(lib->types[i]);
	for (UINT i = 0; i < lib->ndead; i++)
		type_free(lib->dead[i]);
	free(lib->dead);
	for (UINT i = 0; i < lib->nrefs; i++) {
		if (lib->refs_out[i].info)
			lib->refs_out[i].info->lpVtbl->Release(
				lib->refs_out[i].info);
		free(lib->refs_out[i].file);
	}
	free(lib->refs_out);
	free(lib->types);
	free(lib->last);
	free(lib->path);
	free(lib->name);
	free(lib->doc);
	free(lib->help_file);
	free(lib->help_dll);
	cust_free(lib->cust);
	free(lib);
}

/* ---- layout ---- */

static HRESULT type_layout(struct c_type *t);

static struct c_type *internal_type(struct ctl *lib, HREFTYPE href)
{
	if (href & 3)
		return NULL;
	UINT i = href / MSFT_TYPEINFO_SIZE;
	return href % MSFT_TYPEINFO_SIZE || i >= lib->ntypes ? NULL :
	       lib->types[i];
}

/* An external reference is its 12-byte IMPINFO entry's offset, ORed with 1. */
static HREFTYPE ext_href(UINT i)
{
	return (HREFTYPE)(12 * i + 1);
}

static struct c_ref *external_ref(struct ctl *lib, HREFTYPE href)
{
	UINT i = (href - 1) / 12;
	return (href & 3) == 1 && (href - 1) % 12 == 0 && i < lib->nrefs ?
	       &lib->refs_out[i] : NULL;
}

/* An external type's attributes, sized for this library's pointers. */
static HRESULT ref_attr(struct ctl *lib, struct c_ref *r, ULONG *size,
			WORD *align, WORD *vft)
{
	TYPEATTR *a;
	HRESULT hr;
	int p = ptr_size(lib);

	if (r->have_attr)
		a = &r->attr;
	else if (!r->info)
		return TYPE_E_CANTLOADLIBRARY;
	else if (FAILED(hr = r->info->lpVtbl->GetTypeAttr(r->info, &a)))
		return hr;
	*vft = (WORD)(a->cbSizeVft / sizeof(void *) * p);
	*size = a->typekind == TKIND_INTERFACE ||
		a->typekind == TKIND_DISPATCH ||
		a->typekind == TKIND_COCLASS ? (ULONG)p : a->cbSizeInstance;
	*align = a->cbAlignment ? a->cbAlignment : 1;
	if (*align > p && a->typekind != TKIND_RECORD &&
	    a->typekind != TKIND_UNION)
		*align = (WORD)p;
	if (a != &r->attr)
		r->info->lpVtbl->ReleaseTypeAttr(r->info, a);
	return S_OK;
}

/* The count of base interfaces of an interface another library holds. */
static int info_depth(ITypeInfo *ti, int n)
{
	TYPEATTR *a;
	HREFTYPE href;
	ITypeInfo *b;
	int d = 0;

	if (n > 64 || FAILED(ti->lpVtbl->GetTypeAttr(ti, &a)))
		return 0;
	int bases = a->cImplTypes && a->typekind != TKIND_COCLASS;
	ti->lpVtbl->ReleaseTypeAttr(ti, a);
	if (bases &&
	    SUCCEEDED(ti->lpVtbl->GetRefTypeOfImplType(ti, 0, &href)) &&
	    SUCCEEDED(ti->lpVtbl->GetRefTypeInfo(ti, href, &b))) {
		d = 1 + info_depth(b, n + 1);
		b->lpVtbl->Release(b);
	}
	return d;
}

/* The count of base interfaces of a type here: a dual interface with
 * none given has IDispatch and IUnknown. */
static int type_depth(struct c_type *t, int n)
{
	struct c_type *b;
	struct c_ref *r;

	if (n > 64)
		return 0;
	if (!t->nimpls)
		return t->kind == TKIND_INTERFACE &&
		       (t->flags & TYPEFLAG_FDUAL) ? 2 : 0;
	if ((b = internal_type(t->lib, t->impls[0].href)))
		return 1 + type_depth(b, n + 1);
	if ((r = external_ref(t->lib, t->impls[0].href)))
		return 1 + (r->have_attr ? r->depth :
			    r->info ? info_depth(r->info, 0) : 0);
	return 0;
}

/* The size and alignment of a member of a type. */
static HRESULT td_size(struct ctl *lib, const TYPEDESC *td, ULONG *size,
		       WORD *align)
{
	int p = ptr_size(lib);
	HRESULT hr;

	switch (td->vt) {
	case VT_I1: case VT_UI1:
		*size = *align = 1;
		return S_OK;
	case VT_I2: case VT_UI2: case VT_BOOL:
		*size = *align = 2;
		return S_OK;
	case VT_I4: case VT_UI4: case VT_INT: case VT_UINT: case VT_R4:
	case VT_ERROR: case VT_HRESULT:
		*size = *align = 4;
		return S_OK;
	case VT_I8: case VT_UI8: case VT_R8: case VT_CY: case VT_DATE:
		*size = *align = 8;
		return S_OK;
	case VT_DECIMAL:
		*size = 16;
		*align = 8;
		return S_OK;
	case VT_VARIANT:
		*size = p == 8 ? 24 : 16;
		*align = 8;
		return S_OK;
	case VT_BSTR: case VT_PTR: case VT_SAFEARRAY: case VT_LPSTR:
	case VT_LPWSTR: case VT_DISPATCH: case VT_UNKNOWN:
		*size = *align = (WORD)p;
		return S_OK;
	case VT_CARRAY: {
		const ARRAYDESC *a = td->lpadesc;
		uint64_t n;
		if ((hr = td_size(lib, &a->tdescElem, size, align)) != S_OK)
			return hr;
		n = *size;
		for (USHORT i = 0; i < a->cDims; i++) {
			n *= a->rgbounds[i].cElements;
			if (n > 0xffffffffu)	/* past a ULONG */
				return TYPE_E_SIZETOOBIG;
		}
		*size = (ULONG)n;
		return S_OK;
	}
	case VT_USERDEFINED: {
		struct c_type *t = internal_type(lib, td->hreftype);
		struct c_ref *r;
		WORD vft;
		if (t) {
			if ((hr = type_layout(t)) != S_OK)
				return hr;
			*size = t->size;
			*align = t->align ? t->align : 1;
			return S_OK;
		}
		if ((r = external_ref(lib, td->hreftype)))
			return ref_attr(lib, r, size, align, &vft);
		return TYPE_E_ELEMENTNOTFOUND;
	}
	}
	return TYPE_E_UNSUPFORMAT;
}

static HRESULT type_layout(struct c_type *t)
{
	struct ctl *lib = t->lib;
	int p = ptr_size(lib);
	HRESULT hr = S_OK;

	if (t->fixed)
		return S_OK;
	if (t->laying)
		return TYPE_E_CIRCULARTYPE;
	t->laying = 1;
	switch (t->kind) {
	case TKIND_INTERFACE: {
		WORD base = 0;
		if (t->nimpls) {
			struct c_type *b = internal_type(lib, t->impls[0].href);
			struct c_ref *r = external_ref(lib, t->impls[0].href);
			ULONG sz;
			WORD al;
			if (b) {
				if ((hr = type_layout(b)) != S_OK)
					break;
				base = b->vft;
			} else if (r) {
				if ((hr = ref_attr(lib, r, &sz, &al, &base)) !=
				    S_OK)
					break;
			} else {
				hr = TYPE_E_ELEMENTNOTFOUND;
				break;
			}
		} else if (t->flags & TYPEFLAG_FDUAL)
			base = (WORD)(7 * p);	/* IDispatch, implied */
		/* The slots the caller gave first, each past the base's and
		 * none twice; then the others in order in the free slots
		 * after the base's. The table reaches past the last. */
		ULONG end = base;
		for (UINT i = 0; i < t->nfuncs; i++) {
			struct c_func *f = &t->funcs[i];
			if (!f->given_vft)
				continue;
			if (f->given_vft < base) {
				hr = E_INVALIDARG;
				goto out;
			}
			for (UINT k = 0; k < i; k++)
				if (t->funcs[k].given_vft == f->given_vft) {
					hr = TYPE_E_DUPLICATEID;
					goto out;
				}
			f->oVft = f->given_vft;
			if ((ULONG)f->oVft + p > end)
				end = (ULONG)f->oVft + p;
		}
		ULONG next = base;
		for (UINT i = 0; i < t->nfuncs; i++) {
			struct c_func *f = &t->funcs[i];
			if (f->given_vft)
				continue;
			for (UINT k = 0; k < t->nfuncs; k++)
				if (t->funcs[k].given_vft &&
				    (ULONG)t->funcs[k].given_vft == next) {
					next += p;
					k = (UINT)-1;
				}
			if (next + p > 0x7fff) {
				hr = TYPE_E_SIZETOOBIG;
				goto out;
			}
			f->oVft = (SHORT)next;
			next += p;
			if (next > end)
				end = next;
		}
		if (end > 0x7fff) {
			hr = TYPE_E_SIZETOOBIG;
			break;
		}
		t->vft = (WORD)end;
		t->size = (ULONG)p;
		t->align = (WORD)p;
		break;
	}
	case TKIND_DISPATCH:
		for (UINT i = 0; i < t->nfuncs; i++)
			t->funcs[i].oVft = 0;
		t->vft = (WORD)(7 * p);
		t->size = (ULONG)p;
		t->align = (WORD)p;
		break;
	case TKIND_RECORD:
	case TKIND_UNION: {
		uint64_t off = 0, size = 0;
		WORD max_align = 1;
		WORD cap = t->align_set ? t->align_set : 8;
		for (UINT i = 0; i < t->nvars; i++) {
			struct c_var *v = &t->vars[i];
			ULONG s;
			WORD a;
			if (v->varkind != VAR_PERINSTANCE)
				continue;
			if ((hr = td_size(lib, &v->type, &s, &a)) != S_OK)
				goto out;
			if (a > cap)
				a = cap;
			if (a > max_align)
				max_align = a;
			if (t->kind == TKIND_UNION) {
				v->oInst = 0;
				if (s > size)
					size = s;
			} else {
				off = (off + a - 1) / a * a;
				v->oInst = (ULONG)off;
				off += s;
				size = off;
			}
			if (off > 0xffffffffu) {	/* past a ULONG */
				hr = TYPE_E_SIZETOOBIG;
				goto out;
			}
		}
		size = (size + max_align - 1) / max_align * max_align;
		if (size > 0xffffffffu) {
			hr = TYPE_E_SIZETOOBIG;
			goto out;
		}
		t->align = max_align;
		t->size = (ULONG)size;
		break;
	}
	case TKIND_ENUM:
		t->size = 4;
		t->align = 4;
		break;
	case TKIND_ALIAS:
		if (!t->has_alias) {	/* not yet given: no size */
			t->size = 0;
			t->align = 1;
			break;
		}
		hr = td_size(lib, &t->alias, &t->size, &t->align);
		if (hr == TYPE_E_UNSUPFORMAT) {	/* void and the like */
			t->size = 0;
			t->align = 1;
			hr = S_OK;
		}
		break;
	case TKIND_COCLASS:
		t->size = (ULONG)p;
		t->align = (WORD)p;
		break;
	case TKIND_MODULE:
		t->size = 2;
		t->align = 1;
		break;
	default:
		break;
	}
out:
	t->laying = 0;
	return hr;
}

/* ---- writing ---- */

struct buf {
	BYTE *d;
	size_t n, cap;
	int bad;
};

static size_t buf_put(struct buf *b, const void *p, size_t n)
{
	size_t at = b->n;
	if (b->n + n > b->cap) {
		size_t cap = b->cap ? b->cap * 2 : 256;
		while (cap < b->n + n)
			cap *= 2;
		BYTE *d = realloc(b->d, cap);
		if (!d) {
			b->bad = 1;
			return at;
		}
		b->d = d;
		b->cap = cap;
	}
	if (p)
		memcpy(b->d + b->n, p, n);
	else
		memset(b->d + b->n, 0, n);
	b->n += n;
	return at;
}

static size_t put32(struct buf *b, INT v)
{
	BYTE x[4] = { (BYTE)v, (BYTE)(v >> 8), (BYTE)(v >> 16),
		      (BYTE)(v >> 24) };
	return buf_put(b, x, 4);
}

static size_t put16(struct buf *b, int v)
{
	BYTE x[2] = { (BYTE)v, (BYTE)(v >> 8) };
	return buf_put(b, x, 2);
}

static void set32(struct buf *b, size_t at, INT v)
{
	if (at + 4 <= b->n) {
		b->d[at] = (BYTE)v;
		b->d[at + 1] = (BYTE)(v >> 8);
		b->d[at + 2] = (BYTE)(v >> 16);
		b->d[at + 3] = (BYTE)(v >> 24);
	}
}

static void pad4(struct buf *b)
{
	static const BYTE pad[3] = { 0x57, 0x57, 0x57 };
	if (b->n % 4)
		buf_put(b, pad, 4 - b->n % 4);
}

struct writer {
	struct ctl *lib;
	struct buf seg[SEG_COUNT];
	HREFTYPE dispatch_href;
	UINT names, name_chars;
};

/* ANSI bytes of a name or string; *len its length. */
static char *ansi(const WCHAR *s, int *len)
{
	int n = cp_from_utf16(CP_ACP, s, -1, NULL, 0, '?', NULL);
	char *a = n > 0 ? malloc((size_t)n) : NULL;
	if (!a)
		return NULL;
	cp_from_utf16(CP_ACP, s, -1, a, n, '?', NULL);
	*len = n - 1;
	return a;
}

/* A name's entry, shared by equal names; -1 for none. */
static INT add_name(struct writer *w, const WCHAR *name, INT href)
{
	struct buf *b = &w->seg[SEG_NAME];
	int len;
	char *a;

	if (!name)
		return -1;
	if (!(a = ansi(name, &len))) {
		b->bad = 1;
		return -1;
	}
	if (len > 255)
		len = 255;
	for (size_t off = 0; off + 12 <= b->n;) {
		int l = b->d[off + 8];
		if (l == len && !memcmp(b->d + off + 12, a, (size_t)len)) {
			free(a);
			if (href != -1)
				set32(b, off, href);
			return (INT)off;
		}
		off += 12 + (size_t)((l + 3) & ~3);
	}
	size_t off = put32(b, href);
	put32(b, -1);
	put32(b, len | 0x3800);
	buf_put(b, a, (size_t)len);
	pad4(b);
	free(a);
	w->names++;
	w->name_chars += (UINT)len;
	return (INT)off;
}

static INT add_string(struct writer *w, const WCHAR *s)
{
	struct buf *b = &w->seg[SEG_STRING];
	int len;
	char *a;

	if (!s)
		return -1;
	if (!(a = ansi(s, &len))) {
		b->bad = 1;
		return -1;
	}
	if (len > 0xffff)
		len = 0xffff;
	for (size_t off = 0; off + 2 <= b->n;) {
		int l = b->d[off] | b->d[off + 1] << 8;
		if (l == len && !memcmp(b->d + off + 2, a, (size_t)len)) {
			free(a);
			return (INT)off;
		}
		off += (size_t)((2 + l + 3) & ~3);
	}
	size_t off = put16(b, len);
	buf_put(b, a, (size_t)len);
	pad4(b);
	free(a);
	return (INT)off;
}

static INT add_guid(struct writer *w, const GUID *g, INT href)
{
	struct buf *b = &w->seg[SEG_GUID];
	for (size_t off = 0; off + 24 <= b->n; off += 24)
		if (!memcmp(b->d + off, g, 16))
			return (INT)off;
	size_t off = buf_put(b, g, 16);
	put32(b, href);
	put32(b, -1);
	return (INT)off;
}

/* A value: inline when small, else in the custom data segment. */
static INT add_value(struct writer *w, const VARIANT *v)
{
	struct buf *b = &w->seg[SEG_CUSTDATA];
	size_t off, size = 0;

	switch (v->vt) {
	case VT_I2: case VT_I4: case VT_INT: case VT_UI2: case VT_UI4:
	case VT_UINT: case VT_BOOL: case VT_I1: case VT_UI1: case VT_ERROR:
	case VT_HRESULT: {
		LONG x = v->vt == VT_I2 || v->vt == VT_BOOL ? v->iVal :
			 v->vt == VT_UI2 ? v->uiVal :
			 v->vt == VT_I1 ? v->cVal :
			 v->vt == VT_UI1 ? v->bVal : v->lVal;
		if (x >= 0 && x <= 0x3ffffff)
			return (INT)(0x80000000u | (DWORD)v->vt << 26 |
				     (DWORD)x);
		break;
	}
	case VT_EMPTY: case VT_NULL:
		return (INT)(0x80000000u | (DWORD)v->vt << 26);
	}
	off = put16(b, v->vt);
	switch (v->vt) {
	case VT_I1: case VT_UI1:
		size = 1;
		break;
	case VT_I2: case VT_UI2: case VT_BOOL:
		size = 2;
		break;
	case VT_I4: case VT_UI4: case VT_R4: case VT_INT: case VT_UINT:
	case VT_ERROR: case VT_HRESULT:
		size = 4;
		break;
	case VT_I8: case VT_UI8: case VT_R8: case VT_CY: case VT_DATE:
		size = 8;
		break;
	case VT_DECIMAL:
		buf_put(b, &v->decVal, 16);
		break;
	case VT_BSTR: {
		int len = 0;
		char *a = NULL;
		if (v->bstrVal) {
			WCHAR *z = calloc(oa_SysStringLen(v->bstrVal) + 1,
					  sizeof(WCHAR));
			if (z) {
				memcpy(z, v->bstrVal, oa_SysStringLen(v->bstrVal) *
							     sizeof(WCHAR));
				a = ansi(z, &len);
				free(z);
			}
			if (!a) {
				b->bad = 1;
				break;
			}
		}
		put32(b, a ? len : -1);
		if (a)
			buf_put(b, a, (size_t)len);
		free(a);
		break;
	}
	}
	if (size)
		buf_put(b, &v->llVal, size);
	pad4(b);
	return (INT)off;
}

static INT add_cust(struct writer *w, const struct c_cust *c)
{
	if (!c)
		return -1;
	INT next = add_cust(w, c->next);
	INT g = add_guid(w, &c->guid, -1);
	INT v = add_value(w, &c->value);
	size_t off = put32(&w->seg[SEG_CDGUIDS], g);
	put32(&w->seg[SEG_CDGUIDS], v);
	put32(&w->seg[SEG_CDGUIDS], next);
	return (INT)off;
}

static INT add_tdesc(struct writer *w, const TYPEDESC *td)
{
	struct buf *b = &w->seg[SEG_TYPEDESC];
	INT operand;

	switch (td->vt) {
	case VT_PTR:
	case VT_SAFEARRAY:
		operand = add_tdesc(w, td->lptdesc);
		break;
	case VT_USERDEFINED:
		operand = (INT)td->hreftype;
		break;
	case VT_CARRAY: {
		const ARRAYDESC *a = td->lpadesc;
		struct buf *ab = &w->seg[SEG_ARRAYDESC];
		INT elem = add_tdesc(w, &a->tdescElem);
		size_t off = put32(ab, elem);
		ULONG total = 1;
		for (USHORT i = 0; i < a->cDims; i++)
			total *= a->rgbounds[i].cElements;
		put16(ab, a->cDims);
		put16(ab, 0);
		for (USHORT i = 0; i < a->cDims; i++) {
			put32(ab, (INT)a->rgbounds[i].cElements);
			put32(ab, a->rgbounds[i].lLbound);
		}
		operand = (INT)off;
		(void)total;
		break;
	}
	default:
		return (INT)(0x80000000u | (DWORD)td->vt << 16 | td->vt);
	}
	BYTE e[8] = { (BYTE)td->vt, (BYTE)(td->vt >> 8), 0, 0,
		      (BYTE)operand, (BYTE)(operand >> 8),
		      (BYTE)(operand >> 16), (BYTE)(operand >> 24) };
	for (size_t off = 0; off + 8 <= b->n; off += 8)
		if (!memcmp(b->d + off, e, 8))
			return (INT)off;
	return (INT)buf_put(b, e, 8);
}

/* The import file entry for a reference's library. */
static INT add_impfile(struct writer *w, const struct c_ref *r)
{
	struct buf *b = &w->seg[SEG_IMPFILES];
	INT g = add_guid(w, &r->libid, -2);
	size_t len = strlen(r->file);

	for (size_t off = 0; off + 14 <= b->n;) {
		INT eg = b->d[off] | b->d[off + 1] << 8 | b->d[off + 2] << 16 |
			 b->d[off + 3] << 24;
		INT el = b->d[off + 4] | b->d[off + 5] << 8 |
			 b->d[off + 6] << 16 | b->d[off + 7] << 24;
		INT ev = b->d[off + 8] | b->d[off + 9] << 8 |
			 b->d[off + 10] << 16 | b->d[off + 11] << 24;
		int l = (b->d[off + 12] | b->d[off + 13] << 8) >> 2;
		/* the same library: its GUID, locale and version */
		if (eg == g && el == (INT)r->lcid &&
		    ev == (INT)(r->major | r->minor << 16))
			return (INT)off;
		off += (14 + (size_t)l + 3) & ~(size_t)3;
	}
	size_t off = put32(b, g);
	put32(b, (INT)r->lcid);
	put32(b, r->major | r->minor << 16);
	put16(b, (int)(len << 2));
	buf_put(b, r->file, len);
	pad4(b);
	return (INT)off;
}

/* The member records of a type, appended to members; their offset. */
static size_t write_members(struct writer *w, struct c_type *t,
			    struct buf *mem)
{
	UINT n = t->nfuncs + t->nvars;
	size_t start = put32(mem, 0), info0 = mem->n;
	INT *offs = calloc(n ? n : 1, sizeof(INT));

	if (!offs) {
		mem->bad = 1;
		return start;
	}
	for (UINT k = 0; k < t->nfuncs; k++) {
		struct c_func *f = &t->funcs[k];
		int defaults = 0, any_cust = f->cust != NULL;
		for (SHORT i = 0; i < f->nparams; i++) {
			defaults |= (f->params[i].flags & PARAMFLAG_FHASDEFAULT)
				    != 0;
			any_cust |= f->params[i].cust != NULL;
		}
		INT nopt = 7 + f->nparams;
		INT reclen = 24 + 4 * nopt + (defaults ? 4 * f->nparams : 0) +
			     12 * f->nparams;
		INT fkccic = (f->funckind & 7) | (f->invkind & 0xf) << 3 |
			     (f->callconv & 0xf) << 8 |
			     (any_cust ? 0x80 : 0) | (defaults ? 0x1000 : 0) |
			     (f->ordinal >= 0 ? 0x2000 : 0);
		offs[k] = (INT)(mem->n - info0);
		put32(mem, reclen);
		put32(mem, add_tdesc(w, &f->ret));
		put32(mem, f->flags);
		put16(mem, f->funckind == FUNC_DISPATCH ? 0 : f->oVft);
		put16(mem, 0);
		put32(mem, fkccic);
		put16(mem, f->nparams);
		put16(mem, f->nopt);
		put32(mem, (INT)f->help_ctx);
		put32(mem, add_string(w, f->doc));
		put32(mem, f->ordinal >= 0 ? f->ordinal : add_string(w, f->entry));
		put32(mem, 0);
		put32(mem, -1);
		put32(mem, (INT)f->help_str_ctx);
		put32(mem, add_cust(w, f->cust));
		for (SHORT i = 0; i < f->nparams; i++)
			put32(mem, add_cust(w, f->params[i].cust));
		if (defaults)
			for (SHORT i = 0; i < f->nparams; i++)
				put32(mem, f->params[i].flags &
					   PARAMFLAG_FHASDEFAULT ?
					   add_value(w, &f->params[i].def) : -1);
		for (SHORT i = 0; i < f->nparams; i++) {
			struct c_param *pa = &f->params[i];
			put32(mem, add_tdesc(w, &pa->type));
			put32(mem, add_name(w, pa->name, -1));
			put32(mem, pa->flags |
				   (pa->cust ? PARAMFLAG_FHASCUSTDATA : 0));
		}
	}
	for (UINT k = 0; k < t->nvars; k++) {
		struct c_var *v = &t->vars[k];
		ULONG s = 0;
		WORD a;
		offs[t->nfuncs + k] = (INT)(mem->n - info0);
		put32(mem, 40);
		put32(mem, add_tdesc(w, &v->type));
		put32(mem, v->flags);
		put16(mem, v->varkind);
		if (v->varkind == VAR_PERINSTANCE &&
		    td_size(w->lib, &v->type, &s, &a) != S_OK)
			s = 0;
		put16(mem, (int)s);
		put32(mem, v->varkind == VAR_CONST ? add_value(w, &v->value) :
			   (INT)v->oInst);
		put32(mem, (INT)v->help_ctx);
		put32(mem, add_string(w, v->doc));
		put32(mem, -1);
		put32(mem, add_cust(w, v->cust));
		put32(mem, (INT)v->help_str_ctx);
	}
	set32(mem, start, (INT)(mem->n - info0));
	for (UINT k = 0; k < t->nfuncs; k++)
		put32(mem, t->funcs[k].memid);
	for (UINT k = 0; k < t->nvars; k++)
		put32(mem, t->vars[k].memid);
	for (UINT k = 0; k < t->nfuncs; k++)
		put32(mem, add_name(w, t->funcs[k].name, -1));
	for (UINT k = 0; k < t->nvars; k++)
		put32(mem, add_name(w, t->vars[k].name, -1));
	for (UINT k = 0; k < n; k++)
		put32(mem, offs[k]);
	free(offs);
	return start;
}

/* The library as an MSFT image. */
static HRESULT serialize(struct ctl *lib, BYTE **out, size_t *out_len)
{
	struct writer w = { .lib = lib, .dispatch_href = (HREFTYPE)-1 };
	struct buf mem = { 0 }, file = { 0 };
	size_t *member_at = NULL;
	HRESULT hr;

	for (UINT i = 0; i < lib->ntypes; i++)
		if ((hr = type_layout(lib->types[i])) != S_OK)
			return hr;
	member_at = calloc(lib->ntypes + 1, sizeof(*member_at));
	if (!member_at)
		return E_OUTOFMEMORY;

	/* Imports first: their offsets are the hrefs already handed out. */
	for (UINT i = 0; i < lib->nrefs; i++) {
		struct c_ref *r = &lib->refs_out[i];
		INT file_off = add_impfile(&w, r);
		struct buf *b = &w.seg[SEG_IMPINFO];
		int by_guid = !guid_eq(&r->guid, &null_guid);
		put32(b, (INT)((DWORD)r->kind << 24 | (by_guid ? 0x10000 : 0) |
			       1));
		put32(b, file_off);
		put32(b, by_guid ? add_guid(&w, &r->guid, -2) : (INT)r->index);
		if (guid_eq(&r->guid, &iid_dispatch))
			w.dispatch_href = ext_href(i);
	}
	int dispatches = 0;
	for (UINT i = 0; i < lib->ntypes; i++)
		dispatches |= lib->types[i]->kind == TKIND_DISPATCH ||
			      (lib->types[i]->kind == TKIND_INTERFACE &&
			       (lib->types[i]->flags & TYPEFLAG_FDUAL));
	if (dispatches && w.dispatch_href == (HREFTYPE)-1) {
		/* IDispatch, from stdole2, as the dispatch types' base. */
		struct c_ref d = { 0 };
		d.libid = stdole2_libid;
		d.guid = iid_dispatch;
		d.major = 2;
		d.kind = TKIND_INTERFACE;
		d.file = (char *)"stdole2.tlb";
		INT file_off = add_impfile(&w, &d);
		struct buf *b = &w.seg[SEG_IMPINFO];
		put32(b, (INT)((DWORD)TKIND_INTERFACE << 24 | 0x10000 | 1));
		put32(b, file_off);
		put32(b, add_guid(&w, &iid_dispatch, -2));
		w.dispatch_href = ext_href(lib->nrefs);
	}

	/* The type infos. */
	struct buf *ti = &w.seg[SEG_TYPEINFO];
	for (UINT i = 0; i < lib->ntypes; i++) {
		struct c_type *t = lib->types[i];
		INT href = (INT)(i * MSFT_TYPEINFO_SIZE);
		INT data1 = -1;

		member_at[i] = write_members(&w, t, &mem);
		if (t->kind == TKIND_ALIAS && t->has_alias)
			data1 = add_tdesc(&w, &t->alias);
		else if (t->kind == TKIND_MODULE)
			data1 = add_string(&w, t->dll);
		else if (t->kind == TKIND_COCLASS && t->nimpls) {
			struct buf *rt = &w.seg[SEG_REFTAB];
			size_t first = rt->n;
			for (UINT k = 0; k < t->nimpls; k++) {
				put32(rt, (INT)t->impls[k].href);
				put32(rt, t->impls[k].flags);
				put32(rt, add_cust(&w, t->impls[k].cust));
				put32(rt, k + 1 < t->nimpls ?
					  (INT)(rt->n + 4) : -1);
			}
			data1 = (INT)first;
		} else if (t->nimpls)
			data1 = (INT)t->impls[0].href;
		/* A dual interface with no base given derives from
		 * IDispatch, as its layout took it to. */
		int implied = !t->nimpls && t->kind == TKIND_INTERFACE &&
			      (t->flags & TYPEFLAG_FDUAL);
		if (implied)
			data1 = (INT)w.dispatch_href;

		TYPEKIND stored = t->kind == TKIND_INTERFACE &&
				  (t->flags & TYPEFLAG_FDUAL) ?
				  TKIND_DISPATCH : t->kind;
		put32(ti, stored | (t->align & 0x1f) << 11);
		put32(ti, 0);		/* T_MEMBERS, set below */
		put32(ti, 0);
		put32(ti, 0);
		put32(ti, 0);
		put32(ti, 0);
		put32(ti, (INT)(t->nfuncs | t->nvars << 16));
		put32(ti, 0);
		put32(ti, 0);
		put32(ti, 0);
		put32(ti, 0);
		put32(ti, guid_eq(&t->guid, &null_guid) ? -1 :
			  add_guid(&w, &t->guid, href));
		put32(ti, t->flags);
		put32(ti, add_name(&w, t->name, href));
		put32(ti, t->major | t->minor << 16);
		put32(ti, add_string(&w, t->doc));
		put32(ti, (INT)t->help_str_ctx);
		put32(ti, (INT)t->help_ctx);
		put32(ti, add_cust(&w, t->cust));
		put16(ti, t->kind == TKIND_COCLASS ? (int)t->nimpls :
			  t->nimpls || implied ? 1 : 0);
		put16(ti, t->vft);
		put32(ti, (INT)t->size);
		put32(ti, data1);
		put32(ti, 0);
		put32(ti, 0);
		put32(ti, -1);
	}

	/* The library's own. */
	INT h[21];
	int helpdll = lib->help_dll != NULL;
	h[0] = MSFT_MAGIC;
	h[1] = 0x00010002;
	h[2] = guid_eq(&lib->guid, &null_guid) ? -1 :
	       add_guid(&w, &lib->guid, -2);
	h[3] = (INT)lib->lcid;
	h[4] = (INT)lib->lcid;
	h[5] = lib->sys | (helpdll ? MSFT_HELPDLL : 0);
	h[6] = lib->major | lib->minor << 16;
	h[7] = lib->flags;
	h[8] = (INT)lib->ntypes;
	h[9] = add_string(&w, lib->doc);
	h[10] = (INT)lib->help_str_ctx;
	h[11] = (INT)lib->help_ctx;
	h[14] = add_name(&w, lib->name, -1);
	h[15] = add_string(&w, lib->help_file);
	h[16] = add_cust(&w, lib->cust);
	h[17] = 0x20;
	h[18] = 0x80;
	h[19] = (INT)w.dispatch_href;
	h[20] = (INT)(w.seg[SEG_IMPINFO].n / 12);
	INT helpdll_off = helpdll ? add_string(&w, lib->help_dll) : -1;
	h[12] = (INT)w.names;
	h[13] = (INT)w.name_chars;

	/* Empty hash tables. */
	for (int i = 0; i < 0x80 / 4; i++)
		put32(&w.seg[SEG_GUIDHASH], -1);
	for (int i = 0; i < 0x200 / 4; i++)
		put32(&w.seg[SEG_NAMEHASH], -1);

	for (int i = 0; i < SEG_COUNT; i++)
		if (w.seg[i].bad) {
			hr = E_OUTOFMEMORY;
			goto out;
		}
	if (mem.bad) {
		hr = E_OUTOFMEMORY;
		goto out;
	}

	/* Header, type info offsets, segment directory, segments, members. */
	for (int i = 0; i < 21; i++)
		put32(&file, h[i]);
	if (helpdll)
		put32(&file, helpdll_off);
	for (UINT i = 0; i < lib->ntypes; i++)
		put32(&file, (INT)(i * MSFT_TYPEINFO_SIZE));
	size_t dir = buf_put(&file, NULL, 16 * SEG_COUNT);
	if (file.bad) {		/* no directory to fill in */
		hr = E_OUTOFMEMORY;
		goto out;
	}
	for (int i = 0; i < SEG_COUNT; i++) {
		INT off = -1, len = 0;
		if (w.seg[i].n) {
			off = (INT)file.n;
			len = (INT)w.seg[i].n;
			buf_put(&file, w.seg[i].d, w.seg[i].n);
		}
		set32(&file, dir + 16 * i, off);
		set32(&file, dir + 16 * i + 4, len);
		set32(&file, dir + 16 * i + 8, -1);
		set32(&file, dir + 16 * i + 12, 0x0f);
	}
	size_t members = buf_put(&file, mem.d, mem.n);
	if (file.bad) {
		hr = E_OUTOFMEMORY;
		goto out;
	}
	size_t ti_at = (size_t)(file.d[dir] | file.d[dir + 1] << 8 |
				file.d[dir + 2] << 16 |
				(size_t)file.d[dir + 3] << 24);
	for (UINT i = 0; lib->ntypes && i < lib->ntypes; i++)
		set32(&file, ti_at + i * MSFT_TYPEINFO_SIZE + 4,
		      (INT)(members + member_at[i]));
	if (file.bad) {
		hr = E_OUTOFMEMORY;
		goto out;
	}
	*out = file.d;
	*out_len = file.n;
	file.d = NULL;
	hr = S_OK;
out:
	for (int i = 0; i < SEG_COUNT; i++)
		free(w.seg[i].d);
	free(mem.d);
	free(file.d);
	free(member_at);
	return hr;
}

WINFN(HRESULT) ctl_QueryInterface(ICreateTypeLib2 *self, REFIID riid,
				  void **ppv);
WINFN(HRESULT) cti_QueryInterface(ICreateTypeInfo2 *self, REFIID riid,
				  void **ppv);

/* What a snapshot asks its creator: the library's or a type's interfaces. */
static HRESULT outer_qi(IUnknown *outer, int index, REFIID riid, void **ppv)
{
	struct ctl *lib = (struct ctl *)outer;

	if (index < 0)
		return ctl_QueryInterface(&lib->iface, riid, ppv);
	if ((UINT)index >= lib->ntypes) {	/* deleted since */
		*ppv = NULL;
		return E_NOINTERFACE;
	}
	return cti_QueryInterface(&lib->types[index]->iface, riid, ppv);
}

/* The library as it stands, loaded as a type library: a view of lib. */
static HRESULT snapshot(struct ctl *lib, ITypeLib **out)
{
	BYTE *d;
	size_t len;
	HRESULT hr = serialize(lib, &d, &len);

	if (hr == S_OK) {
		free(lib->last);
		lib->last = d;
		lib->last_len = len;
	} else if (!lib->last) {
		return hr;
	}
	/* A draft that won't lay out still answers QueryInterface, as
	 * it did: with the last view that did. LayOut and saving refuse it. */
	return oa_typelib_view(lib->last, lib->last_len,
			       (IUnknown *)&lib->iface, outer_qi, out);
}

/* ---- ICreateTypeLib2 ---- */

WINFN(HRESULT) ctl_QueryInterface(ICreateTypeLib2 *self, REFIID riid,
				  void **ppv)
{
	struct ctl *lib = ctl_of(self);

	if (!ppv)
		return E_POINTER;
	*ppv = NULL;
	if (guid_eq(riid, &iid_unknown) ||
	    guid_eq(riid, &IID_ICreateTypeLib) ||
	    guid_eq(riid, &IID_ICreateTypeLib2)) {
		__atomic_add_fetch(&lib->refs, 1, __ATOMIC_SEQ_CST);
		*ppv = self;
		return S_OK;
	}
	if (guid_eq(riid, &IID_ITypeLib) || guid_eq(riid, &IID_ITypeLib2))
		return snapshot(lib, (ITypeLib **)ppv);
	return E_NOINTERFACE;
}

WINFN(ULONG) ctl_AddRef(ICreateTypeLib2 *self)
{
	return (ULONG)__atomic_add_fetch(&ctl_of(self)->refs, 1,
					 __ATOMIC_SEQ_CST);
}

WINFN(ULONG) ctl_Release(ICreateTypeLib2 *self)
{
	struct ctl *lib = ctl_of(self);
	LONG r = __atomic_sub_fetch(&lib->refs, 1, __ATOMIC_SEQ_CST);

	if (!r)
		ctl_free(lib);
	return (ULONG)r;
}

static int name_eq(const WCHAR *a, const WCHAR *b)
{
	for (;; a++, b++) {
		WCHAR x = *a, y = *b;
		if (x >= 'a' && x <= 'z')
			x -= 32;
		if (y >= 'a' && y <= 'z')
			y -= 32;
		if (x != y)
			return 0;
		if (!x)
			return 1;
	}
}

WINFN(HRESULT) ctl_CreateTypeInfo(ICreateTypeLib2 *self, LPOLESTR name,
				  TYPEKIND kind, ICreateTypeInfo **out)
{
	struct ctl *lib = ctl_of(self);
	struct c_type *t;

	if (!out)
		return E_INVALIDARG;
	*out = NULL;
	if (!name || !*name || (unsigned)kind >= TKIND_MAX)
		return E_INVALIDARG;
	for (UINT i = 0; i < lib->ntypes; i++)
		if (name_eq(lib->types[i]->name, name))
			return TYPE_E_NAMECONFLICT;
	if (!(t = calloc(1, sizeof(*t))))
		return E_OUTOFMEMORY;
	struct c_type **n = realloc(lib->types,
				    (lib->ntypes + 1) * sizeof(*n));
	if (!n || !(t->name = wdup(name))) {
		free(t);
		if (n)
			lib->types = n;
		return E_OUTOFMEMORY;
	}
	lib->types = n;
	t->iface.lpVtbl = &cti_vtbl;
	t->lib = lib;
	t->kind = kind;
	n[lib->ntypes++] = t;
	ctl_AddRef(self);
	*out = &t->iface;
	return S_OK;
}

WINFN(HRESULT) ctl_SetName(ICreateTypeLib2 *self, LPOLESTR name)
{
	return name && set_str(&ctl_of(self)->name, name) ? E_OUTOFMEMORY :
	       name ? S_OK : E_INVALIDARG;
}

WINFN(HRESULT) ctl_SetVersion(ICreateTypeLib2 *self, WORD major, WORD minor)
{
	ctl_of(self)->major = major;
	ctl_of(self)->minor = minor;
	return S_OK;
}

WINFN(HRESULT) ctl_SetGuid(ICreateTypeLib2 *self, REFGUID guid)
{
	if (!guid)
		return E_INVALIDARG;
	ctl_of(self)->guid = *guid;
	return S_OK;
}

WINFN(HRESULT) ctl_SetDocString(ICreateTypeLib2 *self, LPOLESTR doc)
{
	return doc ? set_str(&ctl_of(self)->doc, doc) ? E_OUTOFMEMORY : S_OK :
		     E_INVALIDARG;
}

WINFN(HRESULT) ctl_SetHelpFileName(ICreateTypeLib2 *self, LPOLESTR file)
{
	return file ? set_str(&ctl_of(self)->help_file, file) ? E_OUTOFMEMORY
							      : S_OK :
		      E_INVALIDARG;
}

WINFN(HRESULT) ctl_SetHelpContext(ICreateTypeLib2 *self, DWORD ctx)
{
	ctl_of(self)->help_ctx = ctx;
	return S_OK;
}

WINFN(HRESULT) ctl_SetLcid(ICreateTypeLib2 *self, LCID lcid)
{
	ctl_of(self)->lcid = lcid;
	return S_OK;
}

WINFN(HRESULT) ctl_SetLibFlags(ICreateTypeLib2 *self, UINT flags)
{
	if (flags & ~0xfu)
		return E_INVALIDARG;
	ctl_of(self)->flags = (WORD)flags;
	return S_OK;
}

WINFN(HRESULT) ctl_SaveAllChanges(ICreateTypeLib2 *self)
{
	struct ctl *lib = ctl_of(self);
	BYTE *d;
	size_t len;
	HRESULT hr = serialize(lib, &d, &len);

	if (hr != S_OK)
		return hr;
	/* Through a temporary file beside it: a failed save leaves the
	 * library that was there. */
	char *unix_path = lib->path ? path_to_unix(lib->path) : NULL;
	char *tmp = unix_path ? malloc(strlen(unix_path) + 16) : NULL;
	int fd = -1;
	if (tmp) {
		sprintf(tmp, "%s.XXXXXX", unix_path);
		fd = mkstemp(tmp);
	}
	FILE *f = fd >= 0 ? fdopen(fd, "wb") : NULL;
	if (!f) {
		TRACE("SaveAllChanges: can't write %s",
		      lib->path ? lib->path : "(no file)");
		hr = STG_E_ACCESSDENIED;
		if (fd >= 0) {
			close(fd);
			unlink(tmp);
		}
	} else {
		/* Private while written; then the old one's permissions. */
		if (fwrite(d, 1, len, f) != len || fflush(f))
			hr = STG_E_MEDIUMFULL;
		if (hr == S_OK && path_replace_mode(fd, unix_path))
			hr = STG_E_ACCESSDENIED;
		if (fclose(f) && hr == S_OK)
			hr = STG_E_MEDIUMFULL;
		if (hr == S_OK && rename(tmp, unix_path))
			hr = STG_E_ACCESSDENIED;
		if (hr != S_OK)
			unlink(tmp);
	}
	free(tmp);
	free(unix_path);
	free(d);
	return hr;
}

/*
 * Calls fn on every internal reference in a type desc; fn returns nonzero
 * to stop, which this passes on.
 */
static int td_each_href(TYPEDESC *td, int (*fn)(HREFTYPE *, void *),
			void *arg)
{
	for (int depth = 0; td && depth < 64; depth++) {
		switch (td->vt) {
		case VT_PTR:
		case VT_SAFEARRAY:
			td = td->lptdesc;
			continue;
		case VT_CARRAY:
			td = td->lpadesc ? &td->lpadesc->tdescElem : NULL;
			continue;
		case VT_USERDEFINED:
			return !(td->hreftype & 3) && fn(&td->hreftype, arg);
		default:
			return 0;
		}
	}
	return 0;
}

/* Every internal reference a type makes: its bases and its members'. */
static int type_each_href(struct c_type *t, int (*fn)(HREFTYPE *, void *),
			  void *arg)
{
	for (UINT i = 0; i < t->nimpls; i++)
		if (!(t->impls[i].href & 3) && fn(&t->impls[i].href, arg))
			return 1;
	if (t->has_alias && td_each_href(&t->alias, fn, arg))
		return 1;
	for (UINT i = 0; i < t->nvars; i++)
		if (td_each_href(&t->vars[i].type, fn, arg))
			return 1;
	for (UINT i = 0; i < t->nfuncs; i++) {
		struct c_func *f = &t->funcs[i];
		if (td_each_href(&f->ret, fn, arg))
			return 1;
		for (SHORT k = 0; k < f->nparams; k++)
			if (td_each_href(&f->params[k].type, fn, arg))
				return 1;
	}
	return 0;
}

static int href_is(HREFTYPE *h, void *arg)
{
	return *h == *(HREFTYPE *)arg;
}

/* The types after a deleted one move down a slot. */
static int href_shift(HREFTYPE *h, void *arg)
{
	if (*h > *(HREFTYPE *)arg)
		*h -= MSFT_TYPEINFO_SIZE;
	return 0;
}

WINFN(HRESULT) ctl_DeleteTypeInfo(ICreateTypeLib2 *self, LPOLESTR name)
{
	struct ctl *lib = ctl_of(self);
	UINT i;

	if (!name)
		return E_INVALIDARG;
	for (i = 0; i < lib->ntypes; i++)
		if (lib->types[i]->name && name_eq(lib->types[i]->name, name))
			break;
	if (i == lib->ntypes)
		return TYPE_E_ELEMENTNOTFOUND;

	/*
	 * Other types refer to it by its slot: one still in use can't go
	 * without breaking them, so that is refused.
	 */
	HREFTYPE h = (HREFTYPE)i * MSFT_TYPEINFO_SIZE;
	for (UINT k = 0; k < lib->ntypes; k++)
		if (k != i && type_each_href(lib->types[k], href_is, &h))
			return TYPE_E_INVALIDSTATE;

	struct c_type **dead = realloc(lib->dead,
				       (lib->ndead + 1) * sizeof(*dead));
	if (!dead)
		return E_OUTOFMEMORY;
	lib->dead = dead;
	struct c_type *t = lib->types[i];
	/*
	 * Its ICreateTypeInfo may still be held: it stays, out of the
	 * library, until the library goes.
	 */
	t->deleted = 1;
	dead[lib->ndead++] = t;
	memmove(&lib->types[i], &lib->types[i + 1],
		(lib->ntypes - i - 1) * sizeof(*lib->types));
	lib->ntypes--;
	for (UINT k = 0; k < lib->ntypes; k++)
		type_each_href(lib->types[k], href_shift, &h);
	return S_OK;
}

WINFN(HRESULT) ctl_SetCustData(ICreateTypeLib2 *self, REFGUID guid,
			       VARIANT *value)
{
	return cust_set(&ctl_of(self)->cust, guid, value);
}

WINFN(HRESULT) ctl_SetHelpStringContext(ICreateTypeLib2 *self, ULONG ctx)
{
	ctl_of(self)->help_str_ctx = ctx;
	return S_OK;
}

WINFN(HRESULT) ctl_SetHelpStringDll(ICreateTypeLib2 *self, LPOLESTR dll)
{
	return dll ? set_str(&ctl_of(self)->help_dll, dll) ? E_OUTOFMEMORY
							   : S_OK :
		     E_INVALIDARG;
}

static const ICreateTypeLib2Vtbl ctl_vtbl = {
	ctl_QueryInterface, ctl_AddRef, ctl_Release, ctl_CreateTypeInfo,
	ctl_SetName, ctl_SetVersion, ctl_SetGuid, ctl_SetDocString,
	ctl_SetHelpFileName, ctl_SetHelpContext, ctl_SetLcid,
	ctl_SetLibFlags, ctl_SaveAllChanges, ctl_DeleteTypeInfo,
	ctl_SetCustData, ctl_SetHelpStringContext, ctl_SetHelpStringDll,
};

/* ---- ICreateTypeInfo2 ---- */

static UINT type_index(struct c_type *t)
{
	for (UINT i = 0; i < t->lib->ntypes; i++)
		if (t->lib->types[i] == t)
			return i;
	return 0;
}

WINFN(HRESULT) cti_QueryInterface(ICreateTypeInfo2 *self, REFIID riid,
				  void **ppv)
{
	struct c_type *t = cti_of(self);

	if (!ppv)
		return E_POINTER;
	*ppv = NULL;
	if (guid_eq(riid, &iid_unknown) ||
	    guid_eq(riid, &IID_ICreateTypeInfo) ||
	    guid_eq(riid, &IID_ICreateTypeInfo2)) {
		ctl_AddRef(&t->lib->iface);
		*ppv = self;
		return S_OK;
	}
	if (guid_eq(riid, &IID_ITypeInfo) || guid_eq(riid, &IID_ITypeInfo2)) {
		ITypeLib *tl;
		if (t->deleted)
			return TYPE_E_ELEMENTNOTFOUND;
		HRESULT hr = snapshot(t->lib, &tl);
		if (hr != S_OK)
			return hr;
		hr = tl->lpVtbl->GetTypeInfo(tl, type_index(t),
					     (ITypeInfo **)ppv);
		tl->lpVtbl->Release(tl);
		return hr;
	}
	return E_NOINTERFACE;
}

WINFN(ULONG) cti_AddRef(ICreateTypeInfo2 *self)
{
	return ctl_AddRef(&cti_of(self)->lib->iface);
}

WINFN(ULONG) cti_Release(ICreateTypeInfo2 *self)
{
	return ctl_Release(&cti_of(self)->lib->iface);
}

WINFN(HRESULT) cti_SetGuid(ICreateTypeInfo2 *self, REFGUID guid)
{
	if (!guid)
		return E_INVALIDARG;
	cti_of(self)->guid = *guid;
	return S_OK;
}

WINFN(HRESULT) cti_SetTypeFlags(ICreateTypeInfo2 *self, UINT flags)
{
	if (flags & ~0xffffu)
		return E_INVALIDARG;
	cti_of(self)->flags = (WORD)flags;
	return S_OK;
}

WINFN(HRESULT) cti_SetDocString(ICreateTypeInfo2 *self, LPOLESTR doc)
{
	return doc ? set_str(&cti_of(self)->doc, doc) ? E_OUTOFMEMORY : S_OK :
		     E_INVALIDARG;
}

WINFN(HRESULT) cti_SetHelpContext(ICreateTypeInfo2 *self, DWORD ctx)
{
	cti_of(self)->help_ctx = ctx;
	return S_OK;
}

WINFN(HRESULT) cti_SetVersion(ICreateTypeInfo2 *self, WORD major, WORD minor)
{
	cti_of(self)->major = major;
	cti_of(self)->minor = minor;
	return S_OK;
}

/* Is lib this one, or a snapshot of it? */
static int is_ours(struct ctl *lib, ITypeLib *other)
{
	TLIBATTR *a;
	int ours = 0;

	ICreateTypeLib2 *maker = NULL;
	if (SUCCEEDED(other->lpVtbl->QueryInterface(other, &IID_ICreateTypeLib2,
						    (void **)&maker))) {
		/* A creator's view: ours only if this creator's. */
		ours = maker == &lib->iface;
		maker->lpVtbl->Release(maker);
		return ours;
	}
	if (SUCCEEDED(other->lpVtbl->GetLibAttr(other, &a))) {
		ours = !guid_eq(&lib->guid, &null_guid) &&
		       guid_eq(&a->guid, &lib->guid) &&
		       a->wMajorVerNum == lib->major &&
		       a->wMinorVerNum == lib->minor && a->lcid == lib->lcid;
		other->lpVtbl->ReleaseTLibAttr(other, a);
	}
	return ours;
}

/* The file an imported library is found as. */
static char *import_file(ITypeLib *other, const TLIBATTR *a)
{
	BSTR path = NULL, name = NULL;
	char *file = NULL;

	if (guid_eq(&a->guid, &stdole2_libid))
		return strdup("stdole2.tlb");
	if (SUCCEEDED(oa_QueryPathOfRegTypeLib(&a->guid, a->wMajorVerNum,
					       a->wMinorVerNum, a->lcid,
					       &path)) && path) {
		char *u = utf16_to_utf8_dup(path);
		oa_SysFreeString(path);
		if (u) {
			const char *base = u;
			for (const char *p = u; *p; p++)
				if (*p == '\\' || *p == '/')
					base = p + 1;
			file = strdup(base);
			free(u);
			return file;
		}
	}
	if (SUCCEEDED(other->lpVtbl->GetDocumentation(other, -1, &name, NULL,
						      NULL, NULL)) && name) {
		char *u = utf16_to_utf8_dup(name);
		oa_SysFreeString(name);
		if (u) {
			size_t n = strlen(u);
			file = malloc(n + 5);
			if (file)
				snprintf(file, n + 5, "%s.tlb", u);
			free(u);
		}
	}
	return file ? file : strdup("");
}

WINFN(HRESULT) cti_AddRefTypeInfo(ICreateTypeInfo2 *self, ITypeInfo *info,
				  HREFTYPE *href)
{
	struct c_type *t = cti_of(self);
	struct ctl *lib = t->lib;
	ITypeLib *other = NULL;
	UINT index = 0;
	HRESULT hr;

	if (!info || !href)
		return E_INVALIDARG;
	if (FAILED(hr = info->lpVtbl->GetContainingTypeLib(info, &other,
							    &index)))
		return hr;
	if (is_ours(lib, other)) {
		other->lpVtbl->Release(other);
		if (index >= lib->ntypes)
			return TYPE_E_ELEMENTNOTFOUND;
		*href = index * MSFT_TYPEINFO_SIZE;
		return S_OK;
	}

	TYPEATTR *ta;
	TLIBATTR *la;
	if (FAILED(hr = info->lpVtbl->GetTypeAttr(info, &ta))) {
		other->lpVtbl->Release(other);
		return hr;
	}
	GUID guid = ta->guid;
	TYPEKIND kind = ta->typekind;
	TYPEATTR attr = *ta;
	memset(&attr.tdescAlias, 0, sizeof(attr.tdescAlias));
	info->lpVtbl->ReleaseTypeAttr(info, ta);
	if (FAILED(hr = other->lpVtbl->GetLibAttr(other, &la))) {
		other->lpVtbl->Release(other);
		return hr;
	}
	for (UINT i = 0; i < lib->nrefs; i++) {
		struct c_ref *r = &lib->refs_out[i];
		if (guid_eq(&r->libid, &la->guid) &&
		    r->major == la->wMajorVerNum &&
		    r->minor == la->wMinorVerNum && r->lcid == la->lcid &&
		    (guid_eq(&guid, &null_guid) ? r->index == index :
		     guid_eq(&r->guid, &guid))) {
			other->lpVtbl->ReleaseTLibAttr(other, la);
			other->lpVtbl->Release(other);
			*href = ext_href(i);
			return S_OK;
		}
	}
	struct c_ref *n = realloc(lib->refs_out,
				  (lib->nrefs + 1) * sizeof(*n));
	if (!n) {
		other->lpVtbl->ReleaseTLibAttr(other, la);
		other->lpVtbl->Release(other);
		return E_OUTOFMEMORY;
	}
	lib->refs_out = n;
	struct c_ref *r = &n[lib->nrefs];
	memset(r, 0, sizeof(*r));
	/* Another creator's type: its attributes as they are now, which is
	 * all its view gives, without holding the view; it holds that
	 * creator, and two creators referring to each other would keep
	 * each other alive. Any other library's type is held. */
	ICreateTypeLib2 *maker = NULL;
	int creator = SUCCEEDED(other->lpVtbl->QueryInterface(
		other, &IID_ICreateTypeLib2, (void **)&maker));
	if (creator)
		maker->lpVtbl->Release(maker);
	r->info = creator ? NULL : info;
	r->attr = attr;
	r->have_attr = 1;
	r->depth = info_depth(info, 0);
	r->libid = la->guid;
	r->guid = guid;
	r->lcid = la->lcid;
	r->major = la->wMajorVerNum;
	r->minor = la->wMinorVerNum;
	r->kind = kind;
	r->index = index;
	r->file = import_file(other, la);
	other->lpVtbl->ReleaseTLibAttr(other, la);
	other->lpVtbl->Release(other);
	if (!r->file)
		return E_OUTOFMEMORY;
	if (r->info)
		info->lpVtbl->AddRef(info);
	*href = ext_href(lib->nrefs);
	lib->nrefs++;
	return S_OK;
}

static int href_ok(struct ctl *lib, HREFTYPE href)
{
	return internal_type(lib, href) || external_ref(lib, href);
}

static HRESULT check_tdesc(struct ctl *lib, const TYPEDESC *td)
{
	switch (td->vt) {
	case VT_PTR:
	case VT_SAFEARRAY:
		return check_tdesc(lib, td->lptdesc);
	case VT_CARRAY:
		return check_tdesc(lib, &td->lpadesc->tdescElem);
	case VT_USERDEFINED:
		return href_ok(lib, td->hreftype) ? S_OK :
		       TYPE_E_ELEMENTNOTFOUND;
	}
	return S_OK;
}

static HRESULT param_default(struct c_param *pa, const PARAMDESC *pd)
{
	if (!(pa->flags & PARAMFLAG_FHASDEFAULT))
		return S_OK;
	if (!pd->pparamdescex)
		return E_INVALIDARG;
	return oa_VariantCopy(&pa->def, &pd->pparamdescex->varDefaultValue);
}

WINFN(HRESULT) cti_AddFuncDesc(ICreateTypeInfo2 *self, UINT index,
			       FUNCDESC *desc)
{
	struct c_type *t = cti_of(self);
	struct c_func f;
	HRESULT hr;

	if (!desc)
		return E_INVALIDARG;
	if (index > t->nfuncs)
		return TYPE_E_ELEMENTNOTFOUND;
	if (t->kind != TKIND_INTERFACE && t->kind != TKIND_DISPATCH &&
	    t->kind != TKIND_MODULE)
		return TYPE_E_BADMODULEKIND;
	/* cParamsOpt -1: the last parameter takes the rest (vararg). */
	if (desc->cParams < 0 ||
	    (desc->cParamsOpt < 0 &&
	     !(desc->cParamsOpt == -1 && desc->cParams > 0)) ||
	    desc->cParamsOpt > desc->cParams ||
	    (desc->cParams && !desc->lprgelemdescParam))
		return E_INVALIDARG;
	/* oVft 0 or -1: worked out by the layout; ignored for a module. */
	SHORT given = desc->oVft == -1 ? 0 : desc->oVft;
	if (t->kind != TKIND_MODULE &&
	    (given < 0 || given % ptr_size(t->lib)))
		return E_INVALIDARG;
	if ((t->kind == TKIND_DISPATCH) != (desc->funckind == FUNC_DISPATCH))
		return TYPE_E_BADMODULEKIND;
	if (t->kind == TKIND_MODULE && desc->funckind != FUNC_STATIC)
		return TYPE_E_BADMODULEKIND;
	if (!desc->invkind || (desc->invkind & ~0xf))
		return E_INVALIDARG;

	memset(&f, 0, sizeof(f));
	f.memid = desc->memid;
	if (f.memid == MEMBERID_NIL && t->kind != TKIND_DISPATCH) {
		/* As MIDL numbers them: 0x60000000, the count of base
		 * interfaces in bits 16-23, then the index; so none is an
		 * inherited one's. The first free one from there. */
		f.memid = (MEMBERID)(0x60000000 |
				     (type_depth(t, 0) & 0xff) << 16) +
			  (MEMBERID)t->nfuncs;
		for (UINT i = 0; i < t->nfuncs; i++)
			if (t->funcs[i].memid == f.memid) {
				f.memid++;
				i = (UINT)-1;
			}
	}
	for (UINT i = 0; i < t->nfuncs; i++)
		if (t->funcs[i].memid == f.memid &&
		    t->funcs[i].invkind == desc->invkind)
			return TYPE_E_DUPLICATEID;
	f.funckind = desc->funckind;
	f.invkind = desc->invkind;
	f.callconv = desc->callconv;
	f.nparams = desc->cParams;
	f.nopt = desc->cParamsOpt;
	f.oVft = given;		/* kept only by a fixed layout */
	f.flags = desc->wFuncFlags;
	if (t->kind == TKIND_INTERFACE)
		f.given_vft = given;
	f.ordinal = -1;
	if ((hr = td_copy(&f.ret, &desc->elemdescFunc.tdesc, 0)) != S_OK ||
	    (hr = check_tdesc(t->lib, &f.ret)) != S_OK)
		goto fail;
	if (f.nparams && !(f.params = calloc((size_t)f.nparams,
					     sizeof(*f.params)))) {
		hr = E_OUTOFMEMORY;
		goto fail;
	}
	for (SHORT i = 0; i < f.nparams; i++) {
		const ELEMDESC *e = &desc->lprgelemdescParam[i];
		f.params[i].flags = e->paramdesc.wParamFlags &
				    ~PARAMFLAG_FHASCUSTDATA;
		if ((hr = td_copy(&f.params[i].type, &e->tdesc, 0)) != S_OK ||
		    (hr = check_tdesc(t->lib, &f.params[i].type)) != S_OK ||
		    (hr = param_default(&f.params[i], &e->paramdesc)) != S_OK)
			goto fail;
	}
	struct c_func *n = realloc(t->funcs, (t->nfuncs + 1) * sizeof(*n));
	if (!n) {
		hr = E_OUTOFMEMORY;
		goto fail;
	}
	t->funcs = n;
	memmove(&n[index + 1], &n[index], (t->nfuncs - index) * sizeof(*n));
	n[index] = f;
	t->nfuncs++;
	return S_OK;
fail:
	func_free(&f);
	return hr;
}

WINFN(HRESULT) cti_AddImplType(ICreateTypeInfo2 *self, UINT index,
			       HREFTYPE href)
{
	struct c_type *t = cti_of(self);

	if (index > t->nimpls)
		return TYPE_E_ELEMENTNOTFOUND;
	if (!href_ok(t->lib, href))
		return TYPE_E_ELEMENTNOTFOUND;
	if (t->kind == TKIND_INTERFACE || t->kind == TKIND_DISPATCH) {
		if (index || t->nimpls)
			return TYPE_E_BADMODULEKIND;
	} else if (t->kind != TKIND_COCLASS) {
		return TYPE_E_BADMODULEKIND;
	}
	struct c_impl *n = realloc(t->impls, (t->nimpls + 1) * sizeof(*n));
	if (!n)
		return E_OUTOFMEMORY;
	t->impls = n;
	memmove(&n[index + 1], &n[index], (t->nimpls - index) * sizeof(*n));
	n[index].href = href;
	n[index].flags = 0;
	n[index].cust = NULL;
	t->nimpls++;
	return S_OK;
}

WINFN(HRESULT) cti_SetImplTypeFlags(ICreateTypeInfo2 *self, UINT index,
				    INT flags)
{
	struct c_type *t = cti_of(self);

	if (t->kind != TKIND_COCLASS)
		return TYPE_E_BADMODULEKIND;
	if (index >= t->nimpls)
		return TYPE_E_ELEMENTNOTFOUND;
	t->impls[index].flags = flags;
	return S_OK;
}

WINFN(HRESULT) cti_SetAlignment(ICreateTypeInfo2 *self, WORD align)
{
	if (align && (align & (align - 1)))
		return E_INVALIDARG;
	cti_of(self)->align_set = align;
	return S_OK;
}

WINFN(HRESULT) cti_SetSchema(ICreateTypeInfo2 *self, LPOLESTR schema)
{
	return S_OK;	/* reserved: kept nowhere, as on Windows */
}

WINFN(HRESULT) cti_AddVarDesc(ICreateTypeInfo2 *self, UINT index,
			      VARDESC *desc)
{
	struct c_type *t = cti_of(self);
	struct c_var v;
	HRESULT hr;

	if (!desc)
		return E_INVALIDARG;
	if (index > t->nvars)
		return TYPE_E_ELEMENTNOTFOUND;
	if (t->kind == TKIND_ENUM && desc->varkind != VAR_CONST)
		return TYPE_E_BADMODULEKIND;
	if ((t->kind == TKIND_DISPATCH) != (desc->varkind == VAR_DISPATCH))
		return TYPE_E_BADMODULEKIND;
	if (t->kind == TKIND_INTERFACE || t->kind == TKIND_COCLASS ||
	    t->kind == TKIND_ALIAS)
		return TYPE_E_BADMODULEKIND;
	if (desc->varkind == VAR_CONST && !desc->lpvarValue)
		return E_INVALIDARG;
	memset(&v, 0, sizeof(v));
	v.memid = desc->memid;
	if (v.memid == MEMBERID_NIL) {
		v.memid = (MEMBERID)(0x40000000 + t->nvars);
		for (UINT i = 0; i < t->nvars; i++)
			if (t->vars[i].memid == v.memid) {
				v.memid++;
				i = (UINT)-1;
			}
	}
	for (UINT i = 0; i < t->nvars; i++)
		if (t->vars[i].memid == v.memid)
			return TYPE_E_DUPLICATEID;
	v.varkind = desc->varkind;
	v.flags = desc->wVarFlags;
	if ((hr = td_copy(&v.type, &desc->elemdescVar.tdesc, 0)) != S_OK ||
	    (hr = check_tdesc(t->lib, &v.type)) != S_OK)
		goto fail;
	if (v.varkind == VAR_CONST &&
	    (hr = oa_VariantCopy(&v.value, desc->lpvarValue)) != S_OK)
		goto fail;
	if (v.varkind != VAR_CONST)
		v.oInst = desc->oInst;
	struct c_var *n = realloc(t->vars, (t->nvars + 1) * sizeof(*n));
	if (!n) {
		hr = E_OUTOFMEMORY;
		goto fail;
	}
	t->vars = n;
	memmove(&n[index + 1], &n[index], (t->nvars - index) * sizeof(*n));
	n[index] = v;
	t->nvars++;
	return S_OK;
fail:
	var_free(&v);
	return hr;
}

WINFN(HRESULT) cti_SetFuncAndParamNames(ICreateTypeInfo2 *self, UINT index,
					LPOLESTR *names, UINT count)
{
	struct c_type *t = cti_of(self);
	struct c_func *f;

	if (!names || !count)
		return E_INVALIDARG;
	if (index >= t->nfuncs)
		return TYPE_E_ELEMENTNOTFOUND;
	f = &t->funcs[index];
	/* A property put's last parameter has no name. */
	UINT want = (UINT)f->nparams + 1;
	if (f->invkind & (INVOKE_PROPERTYPUT | INVOKE_PROPERTYPUTREF))
		want--;
	if (count != want || !names[0])
		return TYPE_E_ELEMENTNOTFOUND;
	/* Only accessors of one property share a name. */
	for (UINT i = 0; i < t->nfuncs; i++)
		if (i != index && t->funcs[i].name &&
		    name_eq(t->funcs[i].name, names[0]) &&
		    (t->funcs[i].memid != f->memid ||
		     t->funcs[i].invkind == f->invkind))
			return TYPE_E_AMBIGUOUSNAME;
	if (set_str(&f->name, names[0]))
		return E_OUTOFMEMORY;
	for (UINT i = 1; i < count; i++)
		if (set_str(&f->params[i - 1].name, names[i]))
			return E_OUTOFMEMORY;
	return S_OK;
}

WINFN(HRESULT) cti_SetVarName(ICreateTypeInfo2 *self, UINT index,
			      LPOLESTR name)
{
	struct c_type *t = cti_of(self);

	if (!name)
		return E_INVALIDARG;
	if (index >= t->nvars)
		return TYPE_E_ELEMENTNOTFOUND;
	return set_str(&t->vars[index].name, name) ? E_OUTOFMEMORY : S_OK;
}

WINFN(HRESULT) cti_SetTypeDescAlias(ICreateTypeInfo2 *self, TYPEDESC *alias)
{
	struct c_type *t = cti_of(self);
	TYPEDESC td;
	HRESULT hr;

	if (!alias)
		return E_INVALIDARG;
	if (t->kind != TKIND_ALIAS)
		return TYPE_E_WRONGTYPEKIND;
	if ((hr = td_copy(&td, alias, 0)) != S_OK ||
	    (hr = check_tdesc(t->lib, &td)) != S_OK) {
		td_free(&td);
		return hr;
	}
	if (t->has_alias)
		td_free(&t->alias);
	t->alias = td;
	t->has_alias = 1;
	return S_OK;
}

WINFN(HRESULT) cti_DefineFuncAsDllEntry(ICreateTypeInfo2 *self, UINT index,
					LPOLESTR dll, LPOLESTR proc)
{
	struct c_type *t = cti_of(self);
	struct c_func *f;

	if (t->kind != TKIND_MODULE)
		return TYPE_E_BADMODULEKIND;
	if (index >= t->nfuncs)
		return TYPE_E_ELEMENTNOTFOUND;
	if (!dll || !proc)
		return E_INVALIDARG;
	if (t->dll && !name_eq(t->dll, dll))
		return E_INVALIDARG;	/* one DLL per module */
	if (!t->dll && set_str(&t->dll, dll))
		return E_OUTOFMEMORY;
	f = &t->funcs[index];
	if ((ULONG_PTR)proc <= 0xffff) {	/* an ordinal */
		free(f->entry);
		f->entry = NULL;
		f->ordinal = (int)(ULONG_PTR)proc;
		return S_OK;
	}
	f->ordinal = -1;
	return set_str(&f->entry, proc) ? E_OUTOFMEMORY : S_OK;
}

WINFN(HRESULT) cti_SetFuncDocString(ICreateTypeInfo2 *self, UINT index,
				    LPOLESTR doc)
{
	struct c_type *t = cti_of(self);

	if (!doc)
		return E_INVALIDARG;
	if (index >= t->nfuncs)
		return TYPE_E_ELEMENTNOTFOUND;
	return set_str(&t->funcs[index].doc, doc) ? E_OUTOFMEMORY : S_OK;
}

WINFN(HRESULT) cti_SetVarDocString(ICreateTypeInfo2 *self, UINT index,
				   LPOLESTR doc)
{
	struct c_type *t = cti_of(self);

	if (!doc)
		return E_INVALIDARG;
	if (index >= t->nvars)
		return TYPE_E_ELEMENTNOTFOUND;
	return set_str(&t->vars[index].doc, doc) ? E_OUTOFMEMORY : S_OK;
}

WINFN(HRESULT) cti_SetFuncHelpContext(ICreateTypeInfo2 *self, UINT index,
				      DWORD ctx)
{
	struct c_type *t = cti_of(self);

	if (index >= t->nfuncs)
		return TYPE_E_ELEMENTNOTFOUND;
	t->funcs[index].help_ctx = ctx;
	return S_OK;
}

WINFN(HRESULT) cti_SetVarHelpContext(ICreateTypeInfo2 *self, UINT index,
				     DWORD ctx)
{
	struct c_type *t = cti_of(self);

	if (index >= t->nvars)
		return TYPE_E_ELEMENTNOTFOUND;
	t->vars[index].help_ctx = ctx;
	return S_OK;
}

WINFN(HRESULT) cti_SetMops(ICreateTypeInfo2 *self, UINT index, BSTR mops)
{
	return S_OK;	/* reserved */
}

WINFN(HRESULT) cti_SetTypeIdldesc(ICreateTypeInfo2 *self, IDLDESC *desc)
{
	return desc ? S_OK : E_INVALIDARG;
}

WINFN(HRESULT) cti_LayOut(ICreateTypeInfo2 *self)
{
	struct c_type *t = cti_of(self);

	if (t->kind == TKIND_ALIAS && !t->has_alias)
		return TYPE_E_INVALIDSTATE;
	return type_layout(t);
}

WINFN(HRESULT) cti_DeleteFuncDesc(ICreateTypeInfo2 *self, UINT index)
{
	struct c_type *t = cti_of(self);

	if (index >= t->nfuncs)
		return TYPE_E_ELEMENTNOTFOUND;
	func_free(&t->funcs[index]);
	memmove(&t->funcs[index], &t->funcs[index + 1],
		(t->nfuncs - index - 1) * sizeof(*t->funcs));
	t->nfuncs--;
	return S_OK;
}

WINFN(HRESULT) cti_DeleteFuncDescByMemId(ICreateTypeInfo2 *self,
					 MEMBERID memid, INVOKEKIND kind)
{
	struct c_type *t = cti_of(self);

	for (UINT i = 0; i < t->nfuncs; i++)
		if (t->funcs[i].memid == memid && t->funcs[i].invkind == kind)
			return cti_DeleteFuncDesc(self, i);
	return TYPE_E_ELEMENTNOTFOUND;
}

WINFN(HRESULT) cti_DeleteVarDesc(ICreateTypeInfo2 *self, UINT index)
{
	struct c_type *t = cti_of(self);

	if (index >= t->nvars)
		return TYPE_E_ELEMENTNOTFOUND;
	var_free(&t->vars[index]);
	memmove(&t->vars[index], &t->vars[index + 1],
		(t->nvars - index - 1) * sizeof(*t->vars));
	t->nvars--;
	return S_OK;
}

WINFN(HRESULT) cti_DeleteVarDescByMemId(ICreateTypeInfo2 *self,
					MEMBERID memid)
{
	struct c_type *t = cti_of(self);

	for (UINT i = 0; i < t->nvars; i++)
		if (t->vars[i].memid == memid)
			return cti_DeleteVarDesc(self, i);
	return TYPE_E_ELEMENTNOTFOUND;
}

WINFN(HRESULT) cti_DeleteImplType(ICreateTypeInfo2 *self, UINT index)
{
	struct c_type *t = cti_of(self);

	if (index >= t->nimpls)
		return TYPE_E_ELEMENTNOTFOUND;
	cust_free(t->impls[index].cust);
	memmove(&t->impls[index], &t->impls[index + 1],
		(t->nimpls - index - 1) * sizeof(*t->impls));
	t->nimpls--;
	return S_OK;
}

WINFN(HRESULT) cti_SetCustData(ICreateTypeInfo2 *self, REFGUID guid,
			       VARIANT *value)
{
	return cust_set(&cti_of(self)->cust, guid, value);
}

WINFN(HRESULT) cti_SetFuncCustData(ICreateTypeInfo2 *self, UINT index,
				   REFGUID guid, VARIANT *value)
{
	struct c_type *t = cti_of(self);

	if (index >= t->nfuncs)
		return TYPE_E_ELEMENTNOTFOUND;
	return cust_set(&t->funcs[index].cust, guid, value);
}

WINFN(HRESULT) cti_SetParamCustData(ICreateTypeInfo2 *self, UINT func,
				    UINT param, REFGUID guid, VARIANT *value)
{
	struct c_type *t = cti_of(self);

	if (func >= t->nfuncs || param >= (UINT)t->funcs[func].nparams)
		return TYPE_E_ELEMENTNOTFOUND;
	return cust_set(&t->funcs[func].params[param].cust, guid, value);
}

WINFN(HRESULT) cti_SetVarCustData(ICreateTypeInfo2 *self, UINT index,
				  REFGUID guid, VARIANT *value)
{
	struct c_type *t = cti_of(self);

	if (index >= t->nvars)
		return TYPE_E_ELEMENTNOTFOUND;
	return cust_set(&t->vars[index].cust, guid, value);
}

WINFN(HRESULT) cti_SetImplTypeCustData(ICreateTypeInfo2 *self, UINT index,
				       REFGUID guid, VARIANT *value)
{
	struct c_type *t = cti_of(self);

	if (index >= t->nimpls)
		return TYPE_E_ELEMENTNOTFOUND;
	return cust_set(&t->impls[index].cust, guid, value);
}

WINFN(HRESULT) cti_SetHelpStringContext(ICreateTypeInfo2 *self, ULONG ctx)
{
	cti_of(self)->help_str_ctx = ctx;
	return S_OK;
}

WINFN(HRESULT) cti_SetFuncHelpStringContext(ICreateTypeInfo2 *self,
					    UINT index, ULONG ctx)
{
	struct c_type *t = cti_of(self);

	if (index >= t->nfuncs)
		return TYPE_E_ELEMENTNOTFOUND;
	t->funcs[index].help_str_ctx = ctx;
	return S_OK;
}

WINFN(HRESULT) cti_SetVarHelpStringContext(ICreateTypeInfo2 *self,
					   UINT index, ULONG ctx)
{
	struct c_type *t = cti_of(self);

	if (index >= t->nvars)
		return TYPE_E_ELEMENTNOTFOUND;
	t->vars[index].help_str_ctx = ctx;
	return S_OK;
}

WINFN(HRESULT) cti_Invalidate(ICreateTypeInfo2 *self)
{
	/*
	 * "Reserved": nothing is cached here to throw away, since LayOut
	 * and SaveAllChanges work out the layout afresh each time.
	 */
	return S_OK;
}

WINFN(HRESULT) cti_SetName(ICreateTypeInfo2 *self, LPOLESTR name)
{
	struct c_type *t = cti_of(self);

	if (!name || !*name)
		return E_INVALIDARG;
	for (UINT i = 0; i < t->lib->ntypes; i++)
		if (t->lib->types[i] != t &&
		    name_eq(t->lib->types[i]->name, name))
			return TYPE_E_NAMECONFLICT;
	return set_str(&t->name, name) ? E_OUTOFMEMORY : S_OK;
}

static const ICreateTypeInfo2Vtbl cti_vtbl = {
	cti_QueryInterface, cti_AddRef, cti_Release, cti_SetGuid,
	cti_SetTypeFlags, cti_SetDocString, cti_SetHelpContext,
	cti_SetVersion, cti_AddRefTypeInfo, cti_AddFuncDesc,
	cti_AddImplType, cti_SetImplTypeFlags, cti_SetAlignment,
	cti_SetSchema, cti_AddVarDesc, cti_SetFuncAndParamNames,
	cti_SetVarName, cti_SetTypeDescAlias, cti_DefineFuncAsDllEntry,
	cti_SetFuncDocString, cti_SetVarDocString, cti_SetFuncHelpContext,
	cti_SetVarHelpContext, cti_SetMops, cti_SetTypeIdldesc, cti_LayOut,
	cti_DeleteFuncDesc, cti_DeleteFuncDescByMemId, cti_DeleteVarDesc,
	cti_DeleteVarDescByMemId, cti_DeleteImplType, cti_SetCustData,
	cti_SetFuncCustData, cti_SetParamCustData, cti_SetVarCustData,
	cti_SetImplTypeCustData, cti_SetHelpStringContext,
	cti_SetFuncHelpStringContext, cti_SetVarHelpStringContext,
	cti_Invalidate, cti_SetName,
};

/* ---- the entry points ---- */

WINFN(HRESULT) oa_CreateTypeLib2(SYSKIND sys, LPCOLESTR file,
				 ICreateTypeLib2 **out)
{
	struct ctl *lib;

	if (!out)
		return E_INVALIDARG;
	*out = NULL;
	if (!file || (unsigned)sys > SYS_WIN64)
		return E_INVALIDARG;
	if (!(lib = calloc(1, sizeof(*lib))) ||
	    !(lib->path = utf16_to_utf8_dup(file))) {
		free(lib);
		return E_OUTOFMEMORY;
	}
	lib->iface.lpVtbl = &ctl_vtbl;
	lib->refs = 1;
	lib->sys = sys;
	*out = &lib->iface;
	return S_OK;
}

WINFN(HRESULT) oa_CreateTypeLib(SYSKIND sys, LPCOLESTR file,
				ICreateTypeLib **out)
{
	return oa_CreateTypeLib2(sys, file, out);
}

/* ---- for other readers (oleaut_sltg.c) ---- */

/* A library with no file: oa_ctl_serialize gives its bytes. */
HRESULT oa_ctl_new(SYSKIND sys, ICreateTypeLib2 **out)
{
	struct ctl *lib = calloc(1, sizeof(*lib));

	*out = NULL;
	if (!lib)
		return E_OUTOFMEMORY;
	lib->iface.lpVtbl = &ctl_vtbl;
	lib->refs = 1;
	lib->sys = sys;
	*out = &lib->iface;
	return S_OK;
}

/*
 * A reference to type index (or guid, if not NULL) of another library,
 * named by its GUID, version, LCID and file, without loading it. The
 * href is the same for the same type.
 */
HRESULT oa_ctl_import(ICreateTypeLib2 *self, const GUID *libid, WORD major,
		      WORD minor, LCID lcid, const char *file,
		      const GUID *guid, UINT index, HREFTYPE *out)
{
	struct ctl *lib = ctl_of(self);
	GUID g = guid ? *guid : null_guid;

	for (UINT i = 0; i < lib->nrefs; i++) {
		struct c_ref *r = &lib->refs_out[i];
		if (guid_eq(&r->libid, libid) && guid_eq(&r->guid, &g) &&
		    (guid || r->index == index)) {
			*out = ext_href(i);
			return S_OK;
		}
	}
	struct c_ref *n = realloc(lib->refs_out,
				  (lib->nrefs + 1) * sizeof(*n));
	if (!n)
		return E_OUTOFMEMORY;
	lib->refs_out = n;
	struct c_ref *r = &n[lib->nrefs];
	memset(r, 0, sizeof(*r));
	r->libid = *libid;
	r->guid = g;
	r->lcid = lcid;
	r->major = major;
	r->minor = minor;
	r->kind = TKIND_INTERFACE;
	r->index = index;
	if (!(r->file = strdup(file ? file : "")))
		return E_OUTOFMEMORY;
	*out = ext_href(lib->nrefs++);
	return S_OK;
}

/*
 * The type's layout as given, not worked out: its size, alignment and
 * vtable size here, its functions' vtable offsets and its variables'
 * offsets as AddFuncDesc and AddVarDesc were given them.
 */
void oa_cti_fix_layout(ICreateTypeInfo2 *self, ULONG size, WORD align,
		       WORD vft)
{
	struct c_type *t = cti_of(self);

	t->fixed = 1;
	t->size = size;
	t->align = align ? align : 1;
	t->vft = vft;
}

/* The library as an MSFT file, in *out (malloc'd). */
HRESULT oa_ctl_serialize(ICreateTypeLib2 *self, BYTE **out, size_t *len)
{
	return serialize(ctl_of(self), out, len);
}

#define E(name, ord) { #name, ord, (void *)oa_##name }

const struct builtin_export oa_create_exports[] = {
	E(CreateTypeLib, 160),
	E(CreateTypeLib2, 180),
	BUILTIN_END
};
