/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * oleaut32's type libraries: LoadTypeLib(Ex) and the ITypeLib2 and
 * ITypeInfo2 objects over them.
 *
 * - Libraries come from a .tlb file, or from a PE file's TYPELIB resource
 *   (1, or N given as "path\N"), in the current "MSFT" format, or the
 *   older "SLTG" one, which oleaut_sltg.c turns into MSFT first.
 * - The file is read into one model (struct typelib, struct tl_type and
 *   their functions and variables), which everything else reads. The
 *   reader is written from the format's layout: a header, offsets of the
 *   type infos, and a directory of segments (type infos, imports, the
 *   GUID, name and string tables, type descriptions, array descriptions
 *   and custom data). Every offset is checked, and a bad file fails with
 *   TYPE_E_INVDATAREAD.
 * - stdole2 is built in (stdole2_lib()), with the types the libraries in
 *   use refer to: IUnknown, IDispatch, IEnumVARIANT, GUID, OLE_COLOR,
 *   OLE_HANDLE, and Font and Picture with their aliases IFontDisp and
 *   IPictureDisp. A reference into another library is resolved when it is
 *   first followed: to stdole2, a library already loaded, one registered
 *   (LoadRegTypeLib), or one of that file name searched for as
 *   LoadLibrary would.
 * - A dual interface is stored as one TKIND_DISPATCH type info. That is
 *   the view a library hands out: its functions are all the interface's,
 *   inherited ones included, as IDispatch calls them. GetRefTypeInfo of
 *   the href GetRefTypeOfImplType(-1) gives (-2) is the TKIND_INTERFACE
 *   view, with only its own functions, as the vtable has them.
 * - A type info's references are its library's: AddRef and Release on a
 *   type info count on the library, which is freed (and leaves the cache
 *   of loaded libraries) when the last goes.
 * - ITypeInfo::Invoke is oa_typeinfo_invoke (oleaut_dispatch.c).
 *   CreateDispTypeInfo builds a library of two types: the interface it
 *   describes, and a coclass whose default interface it is.
 * - Not here yet: ITypeComp, CreateInstance. AddressOfMember finds a
 *   module function's DLL entry.
 *   Registration is in oleaut_typereg.c. A dual interface's functions
 *   inherited from a dual interface in a third library (rather than
 *   stdole2's IDispatch) keep that library's hrefs, which this one's
 *   GetRefTypeInfo doesn't know; no library in use has one.
 */
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#include "common/log.h"
#include "loader/modules.h"
#include "loader/pe.h"
#include "win32/codepage.h"
#include "win32/internal.h"
#include "win32/k32.h"
#include "win32/oleaut.h"
#include "win32/path.h"

#define PTR_SIZE ((int)sizeof(void *))

static const GUID stdole2_libid = { 0x00020430, 0x0000, 0x0000,
				    { 0xc0, 0, 0, 0, 0, 0, 0, 0x46 } };

static int guid_eq(const GUID *a, const GUID *b)
{
	return !memcmp(a, b, sizeof(GUID));
}

/* Case-insensitive (ASCII) comparison of NUL-terminated names. */
static int name_eq(const WCHAR *a, const WCHAR *b)
{
	if (!a || !b)
		return 0;
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

/* ----------------------------------------------------------------------
 * The model.
 */

struct tl_cust {
	struct tl_cust *next;
	GUID guid;
	VARIANT value;
};

struct tl_func {
	FUNCDESC desc;
	WCHAR *name;
	WCHAR **param_names;		/* desc.cParams of them, or NULL each */
	WCHAR *doc;
	DWORD help_context, help_string_context;
	WCHAR *entry;			/* a module's function: its name, */
	int ordinal;			/* or its ordinal (-1: by name) */
	struct tl_cust *cust;
	struct tl_cust **param_cust;	/* desc.cParams of them */
};

struct tl_var {
	VARDESC desc;
	WCHAR *name;
	WCHAR *doc;
	DWORD help_context, help_string_context;
	struct tl_cust *cust;
};

struct tl_impl {
	HREFTYPE href;
	INT flags;
	struct tl_cust *cust;
};

struct tl_type {
	ITypeInfo2 iface;
	struct typelib *lib;
	UINT index;
	TYPEATTR attr;			/* as stored */
	WCHAR *name;
	WCHAR *doc;
	DWORD help_context, help_string_context;
	WCHAR *dll_name;		/* TKIND_MODULE */
	struct tl_func *funcs;		/* attr.cFuncs */
	struct tl_var *vars;		/* attr.cVars */
	struct tl_impl *impls;		/* attr.cImplTypes */
	struct tl_cust *cust;
	/*
	 * A dual interface's TKIND_INTERFACE view (made on first use, in the
	 * library's memory), and in that view, the type it is a view of.
	 */
	struct tl_type *iface_view;
	struct tl_type *dispatch_view;
};

/* A library a type in this one refers to (its import file table). */
struct tl_import {
	GUID libid;
	WORD major, minor;
	LCID lcid;
	char *file;			/* in the library's arena */
	int off;			/* its offset in IMPFILES, plus 1 */
	int exact;			/* that minor version, not a later one */
};

/*
 * A type reference of another library, shown by a view in this one (a dual
 * interface's inherited functions): the library and the href there.
 */
struct tl_foreign {
	struct tl_import lib;
	HREFTYPE href;
};

struct arena_block {
	struct arena_block *next;
	size_t used, size;
	max_align_t data[];
};

struct typelib {
	ITypeLib2 iface;
	LONG refs;
	struct typelib *next;		/* in the cache of loaded libraries */
	char *key;			/* the file and resource, for the cache */
	int builtin;			/* stdole2: lives for ever */
	TLIBATTR attr;
	WCHAR *name;
	WCHAR *doc;
	WCHAR *help_file;
	WCHAR *help_dll;
	DWORD help_context, help_string_context;
	UINT ntypes;
	struct tl_type *types;
	struct tl_cust *cust;
	HREFTYPE dispatch_href;		/* IDispatch, wherever it is */
	int ptr_size;			/* the pointer size it was written for */

	/* The file, kept for references into other libraries. */
	BYTE *data;
	size_t len;
	struct tl_import *imports;
	size_t nimports;
	/* hrefs of other libraries' types that this one's views show */
	struct tl_foreign *foreign;
	size_t nforeign;
	pthread_mutex_t lock;		/* imports[], foreign[] */

	struct arena_block *arena;
	BSTR *bstrs;			/* to free with the library */
	size_t nbstrs, bstrs_cap;

	IUnknown *outer;		/* a view's maker (held), or NULL */
	oa_outer_qi outer_qi;
};

static const ITypeLib2Vtbl typelib_vtbl;
static const ITypeInfo2Vtbl typeinfo_vtbl;

static struct typelib *lib_of(ITypeLib2 *iface)
{
	return (struct typelib *)iface;
}

static struct tl_type *type_of(ITypeInfo2 *iface)
{
	return (struct tl_type *)iface;
}

/* Zeroed memory that lives as long as the library. */
static void *ar_alloc(struct typelib *lib, size_t size)
{
	struct arena_block *b = lib->arena;

	size = (size + 15) & ~(size_t)15;
	if (!b || b->size - b->used < size) {
		size_t want = size > 16384 ? size : 16384;

		b = win_calloc(1, sizeof(*b) + want);
		if (!b)
			return NULL;
		b->size = want;
		b->next = lib->arena;
		lib->arena = b;
	}
	void *p = (BYTE *)b->data + b->used;
	b->used += size;
	return p;
}

/* A string in the library's code page, as UTF-16. */
static WCHAR *ar_ansi(struct typelib *lib, const BYTE *s, int len)
{
	WCHAR *w = ar_alloc(lib, ((size_t)len + 1) * sizeof(WCHAR));
	int n;

	if (!w)
		return NULL;
	n = len ? cp_to_utf16(CP_ACP, (const char *)s, len, w, len, 0) : 0;
	w[n > 0 ? n : 0] = 0;
	return w;
}

static WCHAR *ar_utf8(struct typelib *lib, const char *s)
{
	WCHAR *w;
	size_t n = strlen(s);

	w = ar_alloc(lib, (n + 1) * sizeof(WCHAR));
	if (w)
		for (size_t i = 0; i <= n; i++)
			w[i] = (BYTE)s[i];
	return w;
}

/* A BSTR freed with the library (for VT_BSTR values). */
static BSTR lib_bstr(struct typelib *lib, const WCHAR *s, UINT len)
{
	BSTR b;

	if (lib->nbstrs == lib->bstrs_cap) {
		size_t cap = lib->bstrs_cap ? lib->bstrs_cap * 2 : 16;
		BSTR *n = realloc(lib->bstrs, cap * sizeof(*n));

		if (!n)
			return NULL;
		lib->bstrs = n;
		lib->bstrs_cap = cap;
	}
	b = oa_SysAllocStringLen(s, len);
	if (b)
		lib->bstrs[lib->nbstrs++] = b;
	return b;
}

static void lib_free(struct typelib *lib)
{
	for (size_t i = 0; i < lib->nbstrs; i++)
		oa_SysFreeString(lib->bstrs[i]);
	while (lib->arena) {
		struct arena_block *b = lib->arena;

		lib->arena = b->next;
		win_free(b);
	}
	pthread_mutex_destroy(&lib->lock);
	free(lib->bstrs);
	free(lib->imports);
	free(lib->foreign);
	free(lib->data);
	free(lib->key);
	IUnknown *outer = lib->outer;
	free(lib);
	if (outer)
		outer->lpVtbl->Release(outer);
}

static struct typelib *lib_new(void)
{
	struct typelib *lib = calloc(1, sizeof(*lib));

	if (!lib)
		return NULL;
	lib->iface.lpVtbl = &typelib_vtbl;
	lib->refs = 1;
	lib->ptr_size = PTR_SIZE;
	lib->dispatch_href = (HREFTYPE)-1;
	pthread_mutex_init(&lib->lock, NULL);
	return lib;
}

static void type_init(struct typelib *lib, struct tl_type *t, UINT index)
{
	t->iface.lpVtbl = &typeinfo_vtbl;
	t->lib = lib;
	t->index = index;
	t->attr.memidConstructor = MEMBERID_NIL;
	t->attr.memidDestructor = MEMBERID_NIL;
	t->attr.lcid = lib->attr.lcid;
}

/* ----------------------------------------------------------------------
 * The MSFT format.
 */

#define MSFT_MAGIC 0x5446534d		/* "MSFT" */
#define SLTG_MAGIC 0x47544c53		/* "SLTG" */
#define MSFT_HEADER_SIZE 0x54
#define MSFT_TYPEINFO_SIZE 0x64
#define MSFT_HELPDLL 0x100		/* in the header's flags word */

enum {
	SEG_TYPEINFO, SEG_IMPINFO, SEG_IMPFILES, SEG_REFTAB, SEG_GUIDHASH,
	SEG_GUID, SEG_NAMEHASH, SEG_NAME, SEG_STRING, SEG_TYPEDESC,
	SEG_ARRAYDESC, SEG_CUSTDATA, SEG_CDGUIDS, SEG_RES0E, SEG_RES0F,
	SEG_COUNT
};

/* The header's words, in order. */
enum {
	H_MAGIC1, H_MAGIC2, H_POSGUID, H_LCID, H_LCID2, H_VARFLAGS,
	H_VERSION, H_FLAGS, H_NTYPES, H_HELPSTRING, H_HELPSTRINGCONTEXT,
	H_HELPCONTEXT, H_NAMECOUNT, H_NAMECHARS, H_NAME, H_HELPFILE,
	H_CUSTDATA, H_RES44, H_RES48, H_DISPATCH, H_NIMPINFOS, H_WORDS
};

/* A type info's words (the two 16-bit counts at T_IMPLVFT). */
enum {
	T_KIND, T_MEMBERS, T_RES2, T_RES3, T_RES4, T_RES5, T_COUNTS,
	T_RES7, T_RES8, T_RES9, T_RESA, T_GUID, T_FLAGS, T_NAME, T_VERSION,
	T_DOC, T_HELPSTRINGCONTEXT, T_HELPCONTEXT, T_CUSTDATA, T_IMPLVFT,
	T_SIZE, T_DATATYPE1, T_DATATYPE2, T_RES18, T_RES19
};

struct msft {
	struct typelib *lib;
	const BYTE *d;
	size_t len;
	INT h[H_WORDS];
	struct {
		INT off, len;
	} seg[SEG_COUNT];
	TYPEDESC **tdesc_cache;		/* by type description offset / 8 */
	BYTE *tdesc_busy;
	size_t ntdesc;
	int depth;			/* of msft_tdesc calls */
};

static INT rd32(const BYTE *p)
{
	return (INT)((DWORD)p[0] | (DWORD)p[1] << 8 | (DWORD)p[2] << 16 |
		     (DWORD)p[3] << 24);
}

static SHORT rd16(const BYTE *p)
{
	return (SHORT)(p[0] | p[1] << 8);
}

/* n bytes at off in the file, or NULL. */
static const BYTE *at(const struct msft *m, INT off, size_t n)
{
	if (off < 0 || (size_t)off > m->len || m->len - (size_t)off < n)
		return NULL;
	return m->d + off;
}

/* n bytes at off in a segment, or NULL. */
static const BYTE *seg_at(const struct msft *m, int seg, INT off, size_t n)
{
	if (off < 0 || m->seg[seg].off < 0 || off > m->seg[seg].len ||
	    (size_t)(m->seg[seg].len - off) < n)
		return NULL;
	return at(m, m->seg[seg].off + off, n);
}

static int msft_guid(const struct msft *m, INT off, GUID *out)
{
	const BYTE *p = seg_at(m, SEG_GUID, off, 16);

	if (!p)
		return -1;
	memcpy(out, p, sizeof(GUID));
	return 0;
}

/* A name from the name table; NULL (and no error) for offset -1. */
static int msft_name(const struct msft *m, INT off, WCHAR **out)
{
	const BYTE *p;

	*out = NULL;
	if (off == -1)
		return 0;
	if (!(p = seg_at(m, SEG_NAME, off, 12)))
		return -1;
	int len = rd32(p + 8) & 0xff;
	if (!(p = seg_at(m, SEG_NAME, off + 12, len)))
		return -1;
	return (*out = ar_ansi(m->lib, p, len)) ? 0 : -1;
}

static int msft_string(const struct msft *m, INT off, WCHAR **out)
{
	const BYTE *p;

	*out = NULL;
	if (off == -1)
		return 0;
	if (!(p = seg_at(m, SEG_STRING, off, 2)))
		return -1;
	int len = (USHORT)rd16(p);
	if (!(p = seg_at(m, SEG_STRING, off + 2, len)))
		return -1;
	return (*out = ar_ansi(m->lib, p, len)) ? 0 : -1;
}

/*
 * A value: packed into the offset itself when it is negative (the type in
 * bits 26-30, the value in the 26 below), else a VARTYPE then the data in
 * the custom data segment.
 */
static int msft_value(const struct msft *m, INT off, VARIANT *v)
{
	const BYTE *p;
	size_t size;

	memset(v, 0, sizeof(*v));
	if (off < 0) {
		/* Only a scalar fits; anything else would point nowhere. */
		switch ((off & 0x7c000000) >> 26) {
		case VT_EMPTY: case VT_NULL: case VT_I2: case VT_I4:
		case VT_R4: case VT_R8: case VT_CY: case VT_DATE:
		case VT_ERROR: case VT_BOOL: case VT_I1: case VT_UI1:
		case VT_UI2: case VT_UI4: case VT_I8: case VT_UI8:
		case VT_INT: case VT_UINT: case VT_HRESULT:
			break;
		default:
			return -1;
		}
		v->vt = (off & 0x7c000000) >> 26;
		v->lVal = off & 0x03ffffff;
		return 0;
	}
	if (!(p = seg_at(m, SEG_CUSTDATA, off, 2)))
		return -1;
	v->vt = (USHORT)rd16(p);
	switch (v->vt) {
	case VT_EMPTY:
	case VT_NULL:
		return 0;
	case VT_I1:
	case VT_UI1:
		size = 1;
		break;
	case VT_I2:
	case VT_UI2:
	case VT_BOOL:
		size = 2;
		break;
	case VT_I4:
	case VT_UI4:
	case VT_R4:
	case VT_INT:
	case VT_UINT:
	case VT_ERROR:
	case VT_HRESULT:
		size = 4;
		break;
	case VT_I8:
	case VT_UI8:
	case VT_R8:
	case VT_CY:
	case VT_DATE:
		size = 8;
		break;
	case VT_DECIMAL:
		if (!(p = seg_at(m, SEG_CUSTDATA, off + 2, 16)))
			return -1;
		memcpy(&v->decVal, p, 16);
		v->vt = VT_DECIMAL;
		return 0;
	case VT_BSTR: {
		INT n;

		if (!(p = seg_at(m, SEG_CUSTDATA, off + 2, 4)))
			return -1;
		n = rd32(p);
		if (n == -1)
			return 0;
		if (n < 0 || !(p = seg_at(m, SEG_CUSTDATA, off + 6, n)))
			return -1;
		WCHAR *w = ar_ansi(m->lib, p, n);
		if (!w || !(v->bstrVal = lib_bstr(m->lib, w, wstrlen(w))))
			return -1;
		return 0;
	}
	default:
		TRACE("type library: a value of type %u", v->vt);
		v->vt = VT_EMPTY;
		return 0;
	}
	if (!(p = seg_at(m, SEG_CUSTDATA, off + 2, size)))
		return -1;
	memcpy(&v->llVal, p, size);
	return 0;
}

/* A chain of custom data: GUID, value and next, in the CDGUIDS segment. */
static int msft_cust(const struct msft *m, INT off, struct tl_cust **out)
{
	*out = NULL;
	for (int n = 0; off != -1; n++) {
		const BYTE *p = seg_at(m, SEG_CDGUIDS, off, 12);
		struct tl_cust *c;

		if (!p || n > 100000)
			return -1;
		if (!(c = ar_alloc(m->lib, sizeof(*c))) ||
		    msft_guid(m, rd32(p), &c->guid) ||
		    msft_value(m, rd32(p + 4), &c->value))
			return -1;
		*out = c;
		out = &c->next;
		off = rd32(p + 8);
	}
	return 0;
}

static int msft_tdesc(struct msft *m, INT x, TYPEDESC *td);
static HRESULT tl_load(const char *win, struct typelib **out);

/*
 * A type encoded in the reference itself can't be one that needs another
 * description (what it points to, its elements, the type it names).
 */
static int inline_vt_ok(VARTYPE vt)
{
	return vt != VT_PTR && vt != VT_SAFEARRAY && vt != VT_CARRAY &&
	       vt != VT_USERDEFINED;
}

/* The description at offset x, shared: each is decoded once. */
static TYPEDESC *msft_tdesc_at(struct msft *m, INT x)
{
	size_t i = (size_t)x / 8;

	if (x < 0) {
		TYPEDESC *td;

		if (!inline_vt_ok(x & VT_TYPEMASK) ||
		    !(td = ar_alloc(m->lib, sizeof(*td))))
			return NULL;
		td->vt = x & VT_TYPEMASK;
		return td;
	}
	if (x % 8 || i >= m->ntdesc || m->tdesc_busy[i])
		return NULL;		/* misaligned, or a loop */
	if (!m->tdesc_cache[i]) {
		TYPEDESC *td = ar_alloc(m->lib, sizeof(*td));

		if (!td)
			return NULL;
		m->tdesc_busy[i] = 1;
		int bad = msft_tdesc(m, x, td);
		m->tdesc_busy[i] = 0;
		if (bad)
			return NULL;
		m->tdesc_cache[i] = td;
	}
	return m->tdesc_cache[i];
}

/*
 * A type: a VARTYPE in its low bits if negative, else the offset of an
 * entry in the type description segment: the VARTYPE, a word we don't
 * use, then a 32-bit operand (encoded as a type itself for VT_PTR and
 * VT_SAFEARRAY, an href for VT_USERDEFINED, an array description's
 * offset for VT_CARRAY).
 */
#define MSFT_TDESC_DEPTH 64

static int msft_tdesc_body(struct msft *m, INT x, TYPEDESC *td);

static int msft_tdesc(struct msft *m, INT x, TYPEDESC *td)
{
	int r;

	if (x < 0) {
		td->vt = x & VT_TYPEMASK;
		return inline_vt_ok(td->vt) ? 0 : -1;
	}
	/* Nested descriptions (arrays of arrays aren't cached): a limit. */
	if (m->depth >= MSFT_TDESC_DEPTH)
		return -1;
	m->depth++;
	r = msft_tdesc_body(m, x, td);
	m->depth--;
	return r;
}

static int msft_tdesc_body(struct msft *m, INT x, TYPEDESC *td)
{
	const BYTE *p;
	INT v;

	if (!(p = seg_at(m, SEG_TYPEDESC, x, 8)))
		return -1;
	td->vt = (USHORT)rd16(p) & VT_TYPEMASK;
	v = rd32(p + 4);
	switch (td->vt) {
	case VT_PTR:
	case VT_SAFEARRAY:
		return (td->lptdesc = msft_tdesc_at(m, v)) ? 0 : -1;
	case VT_USERDEFINED:
		td->hreftype = v;
		return 0;
	case VT_CARRAY: {
		USHORT dims;
		ARRAYDESC *a;

		if (!(p = seg_at(m, SEG_ARRAYDESC, v & 0x7fffffff, 8)))
			return -1;
		dims = (USHORT)rd16(p + 4);
		if (!dims || !(p = seg_at(m, SEG_ARRAYDESC, v & 0x7fffffff,
					  8 + 8 * (size_t)dims)))
			return -1;
		a = ar_alloc(m->lib, sizeof(*a) +
				     (dims - 1) * sizeof(SAFEARRAYBOUND));
		if (!a || msft_tdesc(m, rd32(p), &a->tdescElem))
			return -1;
		a->cDims = dims;
		for (USHORT i = 0; i < dims; i++) {
			a->rgbounds[i].cElements = rd32(p + 8 + 8 * i);
			a->rgbounds[i].lLbound = rd32(p + 12 + 8 * i);
		}
		td->lpadesc = a;
		return 0;
	}
	default:
		return 0;
	}
}

/* The functions and variables of a type, at memoffset in the file. */
static int msft_members(struct msft *m, struct tl_type *t, INT memoffset)
{
	struct typelib *lib = m->lib;
	UINT nf = t->attr.cFuncs, nv = t->attr.cVars, n = nf + nv;
	const BYTE *p;
	INT infolen, rec, end;

	if (!n)
		return 0;
	if (!(p = at(m, memoffset, 4)))
		return -1;
	infolen = rd32(p);
	if (infolen < 0 || !at(m, memoffset, 4 + (size_t)infolen + 8 * n))
		return -1;
	rec = memoffset + 4;
	end = rec + infolen;
	t->funcs = nf ? ar_alloc(lib, nf * sizeof(*t->funcs)) : NULL;
	t->vars = nv ? ar_alloc(lib, nv * sizeof(*t->vars)) : NULL;
	if ((nf && !t->funcs) || (nv && !t->vars))
		return -1;
	for (UINT k = 0; k < n; k++) {
		MEMBERID memid = rd32(m->d + end + 4 * k);
		INT name = rd32(m->d + end + 4 * (n + k));
		INT reclen;
		WCHAR *wname;

		if (!(p = at(m, rec, 4)))
			return -1;
		reclen = rd32(p) & 0xffff;
		if (reclen < 20 || rec + reclen > end ||
		    !(p = at(m, rec, reclen)) || msft_name(m, name, &wname))
			return -1;
		if (k < nf) {
			struct tl_func *f = &t->funcs[k];
			FUNCDESC *d = &f->desc;
			INT fkccic, nopt;
			SHORT nargs;
			const BYTE *params, *defaults = NULL;

			if (reclen < 24)
				return -1;
			fkccic = rd32(p + 16);
			nargs = rd16(p + 20);
			if (nargs < 0)
				return -1;
			INT tail = 12 * nargs + (fkccic & 0x1000 ? 4 * nargs : 0);
			if (reclen - 24 < tail)
				return -1;
			params = p + reclen - 12 * nargs;
			if (fkccic & 0x1000)
				defaults = params - 4 * nargs;
			nopt = (reclen - 24 - tail) / 4;
			f->name = wname;
			/*
			 * A property's other accessors may leave the name out:
			 * it is the one before's, with the same memid.
			 */
			if (!wname && k && t->funcs[k - 1].desc.memid == memid)
				f->name = t->funcs[k - 1].name;
			d->memid = memid;
			d->funckind = fkccic & 7;
			d->invkind = (fkccic >> 3) & 0xf;
			d->callconv = (fkccic >> 8) & 0xf;
			d->cParams = nargs;
			d->cParamsOpt = rd16(p + 22);
			/* In this host's pointers, which must fit a SHORT. */
			long vft = (long)(rd16(p + 12) & ~1) * PTR_SIZE /
				   lib->ptr_size;
			if (vft > 0x7fff)
				return -1;
			d->oVft = (SHORT)vft;
			d->wFuncFlags = (WORD)rd32(p + 8);
			if (msft_tdesc(m, rd32(p + 4), &d->elemdescFunc.tdesc))
				return -1;
			f->ordinal = -1;
			if (nopt > 0)
				f->help_context = rd32(p + 24);
			if (nopt > 1 && msft_string(m, rd32(p + 28), &f->doc))
				return -1;
			if (nopt > 2) {
				INT e = rd32(p + 32);

				if (fkccic & 0x2000)
					f->ordinal = e & 0xffff;
				else if (msft_string(m, e, &f->entry))
					return -1;
			}
			if (nopt > 5)
				f->help_string_context = rd32(p + 44);
			if (nopt > 6 && (fkccic & 0x80) &&
			    msft_cust(m, rd32(p + 48), &f->cust))
				return -1;
			if (nargs) {
				d->lprgelemdescParam =
					ar_alloc(lib, nargs * sizeof(ELEMDESC));
				f->param_names =
					ar_alloc(lib, nargs * sizeof(WCHAR *));
				f->param_cust = ar_alloc(
					lib, nargs * sizeof(struct tl_cust *));
				if (!d->lprgelemdescParam || !f->param_names ||
				    !f->param_cust)
					return -1;
			}
			for (SHORT j = 0; j < nargs; j++) {
				ELEMDESC *e = &d->lprgelemdescParam[j];
				const BYTE *q = params + 12 * j;
				USHORT flags = (USHORT)rd32(q + 8);

				if (msft_tdesc(m, rd32(q), &e->tdesc) ||
				    msft_name(m, rd32(q + 4),
					      &f->param_names[j]))
					return -1;
				e->paramdesc.wParamFlags = flags;
				if ((flags & PARAMFLAG_FHASDEFAULT) && defaults) {
					PARAMDESCEX *x = ar_alloc(lib, sizeof(*x));

					if (!x || msft_value(m,
							     rd32(defaults + 4 * j),
							     &x->varDefaultValue))
						return -1;
					x->cBytes = sizeof(*x);
					e->paramdesc.pparamdescex = x;
				}
				if (nopt > 7 + j && (fkccic & 0x80) &&
				    msft_cust(m, rd32(p + 52 + 4 * j),
					      &f->param_cust[j]))
					return -1;
			}
		} else {
			struct tl_var *v = &t->vars[k - nf];
			VARDESC *d = &v->desc;
			INT nopt = (reclen - 20) / 4, value = rd32(p + 16);

			v->name = wname;
			d->memid = memid;
			d->wVarFlags = (WORD)rd32(p + 8);
			d->varkind = rd16(p + 12);
			if (msft_tdesc(m, rd32(p + 4), &d->elemdescVar.tdesc))
				return -1;
			if (d->varkind == VAR_CONST) {
				VARIANT *val = ar_alloc(lib, sizeof(*val));

				if (!val || msft_value(m, value, val))
					return -1;
				d->lpvarValue = val;
			} else {
				d->oInst = value;
			}
			if (nopt > 0)
				v->help_context = rd32(p + 20);
			if (nopt > 1 && msft_string(m, rd32(p + 24), &v->doc))
				return -1;
			if (nopt > 3 && msft_cust(m, rd32(p + 32), &v->cust))
				return -1;
			if (nopt > 4)
				v->help_string_context = rd32(p + 36);
		}
		rec += reclen;
	}
	return 0;
}

static int msft_type(struct msft *m, struct tl_type *t, INT off)
{
	struct typelib *lib = m->lib;
	const BYTE *p = at(m, off, MSFT_TYPEINFO_SIZE);
	INT w[T_RES19 + 1];
	TYPEATTR *a = &t->attr;

	if (!p)
		return -1;
	for (int i = 0; i < T_IMPLVFT; i++)
		w[i] = rd32(p + 4 * i);
	for (int i = T_SIZE; i <= T_RES19; i++)
		w[i] = rd32(p + 4 * (i - 1) + 4);
	a->typekind = w[T_KIND] & 0xf;
	if (a->typekind >= TKIND_MAX)
		return -1;
	a->cbAlignment = (w[T_KIND] >> 11) & 0x1f;
	a->cFuncs = w[T_COUNTS] & 0xffff;
	a->cVars = (DWORD)w[T_COUNTS] >> 16;
	a->cImplTypes = rd16(p + 4 * T_IMPLVFT);
	a->cbSizeVft = (WORD)((USHORT)rd16(p + 4 * T_IMPLVFT + 2) * PTR_SIZE /
			      lib->ptr_size);
	a->cbSizeInstance = w[T_SIZE];
	if (a->typekind == TKIND_INTERFACE || a->typekind == TKIND_DISPATCH ||
	    a->typekind == TKIND_COCLASS)
		a->cbSizeInstance = PTR_SIZE;
	a->wTypeFlags = (WORD)w[T_FLAGS];
	a->wMajorVerNum = w[T_VERSION] & 0xffff;
	a->wMinorVerNum = (DWORD)w[T_VERSION] >> 16;
	if (w[T_GUID] != -1 && msft_guid(m, w[T_GUID], &a->guid))
		return -1;
	t->help_context = w[T_HELPCONTEXT];
	t->help_string_context = w[T_HELPSTRINGCONTEXT];
	if (msft_name(m, w[T_NAME], &t->name) ||
	    msft_string(m, w[T_DOC], &t->doc) ||
	    msft_cust(m, w[T_CUSTDATA], &t->cust))
		return -1;
	if (a->typekind == TKIND_ALIAS &&
	    msft_tdesc(m, w[T_DATATYPE1], &a->tdescAlias))
		return -1;
	if (a->typekind == TKIND_MODULE &&
	    msft_string(m, w[T_DATATYPE1], &t->dll_name))
		return -1;
	if (a->cImplTypes) {
		t->impls = ar_alloc(lib, a->cImplTypes * sizeof(*t->impls));
		if (!t->impls)
			return -1;
		if (a->typekind == TKIND_COCLASS) {
			INT r = w[T_DATATYPE1];

			for (UINT i = 0; i < a->cImplTypes; i++) {
				const BYTE *q = seg_at(m, SEG_REFTAB, r, 16);

				if (!q)
					return -1;
				t->impls[i].href = rd32(q);
				t->impls[i].flags = rd32(q + 4);
				if (msft_cust(m, rd32(q + 8),
					      &t->impls[i].cust))
					return -1;
				r = rd32(q + 12);
			}
		} else {
			a->cImplTypes = 1;
			t->impls[0].href = w[T_DATATYPE1];
		}
	}
	return msft_members(m, t, w[T_MEMBERS]);
}

static HRESULT msft_parse(struct typelib *lib, const BYTE *d, size_t len)
{
	struct msft m = { .lib = lib, .d = d, .len = len };
	const BYTE *p;
	INT off, tioffs;
	HRESULT hr = TYPE_E_INVDATAREAD;

	if (!(p = at(&m, 0, MSFT_HEADER_SIZE)))
		return hr;
	for (int i = 0; i < H_WORDS; i++)
		m.h[i] = rd32(p + 4 * i);
	off = MSFT_HEADER_SIZE;
	if (m.h[H_VARFLAGS] & MSFT_HELPDLL)
		off += 4;
	if (m.h[H_NTYPES] < 0 || m.h[H_NTYPES] > 0xffff)
		return hr;
	tioffs = off;
	off += 4 * m.h[H_NTYPES];
	if (!(p = at(&m, off, 16 * SEG_COUNT)))
		return hr;
	for (int i = 0; i < SEG_COUNT; i++) {
		m.seg[i].off = rd32(p + 16 * i);
		m.seg[i].len = rd32(p + 16 * i + 4);
		if (m.seg[i].off != -1 &&
		    (m.seg[i].len < 0 || !at(&m, m.seg[i].off, m.seg[i].len)))
			return hr;
		if (m.seg[i].off == -1)
			m.seg[i].len = 0;
	}

	lib->attr.lcid = m.h[H_LCID];
	lib->attr.syskind = m.h[H_VARFLAGS] & 0xf;
	lib->ptr_size = lib->attr.syskind == SYS_WIN64 ? 8 : 4;
	lib->attr.wMajorVerNum = m.h[H_VERSION] & 0xffff;
	lib->attr.wMinorVerNum = (DWORD)m.h[H_VERSION] >> 16;
	lib->attr.wLibFlags = (WORD)m.h[H_FLAGS];
	lib->help_context = m.h[H_HELPCONTEXT];
	lib->help_string_context = m.h[H_HELPSTRINGCONTEXT];
	lib->dispatch_href = m.h[H_DISPATCH];
	if ((m.h[H_POSGUID] != -1 &&
	     msft_guid(&m, m.h[H_POSGUID], &lib->attr.guid)) ||
	    msft_name(&m, m.h[H_NAME], &lib->name) ||
	    msft_string(&m, m.h[H_HELPSTRING], &lib->doc) ||
	    msft_string(&m, m.h[H_HELPFILE], &lib->help_file) ||
	    msft_cust(&m, m.h[H_CUSTDATA], &lib->cust))
		return hr;
	if ((m.h[H_VARFLAGS] & MSFT_HELPDLL) &&
	    msft_string(&m, rd32(d + MSFT_HEADER_SIZE), &lib->help_dll))
		return hr;

	m.ntdesc = (size_t)m.seg[SEG_TYPEDESC].len / 8;
	m.tdesc_cache = calloc(m.ntdesc + 1, sizeof(*m.tdesc_cache));
	m.tdesc_busy = calloc(m.ntdesc + 1, 1);
	lib->ntypes = m.h[H_NTYPES];
	lib->types = ar_alloc(lib, (lib->ntypes + 1) * sizeof(*lib->types));
	if (!m.tdesc_cache || !m.tdesc_busy || !lib->types) {
		hr = E_OUTOFMEMORY;
		goto out;
	}
	for (UINT i = 0; i < lib->ntypes; i++) {
		struct tl_type *t = &lib->types[i];

		type_init(lib, t, i);
		if (msft_type(&m, t, m.seg[SEG_TYPEINFO].off +
					     (INT)i * MSFT_TYPEINFO_SIZE)) {
			TRACE("type library: type info %u is bad", i);
			goto out;
		}
		(void)tioffs;
	}
	if (!(lib->data = malloc(len ? len : 1))) {
		hr = E_OUTOFMEMORY;
		goto out;
	}
	memcpy(lib->data, d, len);
	lib->len = len;
	hr = S_OK;
out:
	free(m.tdesc_cache);
	free(m.tdesc_busy);
	return hr;
}

/* ----------------------------------------------------------------------
 * The built-in stdole2.
 */

/* A type in a table below: a VARTYPE, behind ptr VT_PTRs. */
struct s_type {
	VARTYPE vt;
	BYTE ptr;
	SHORT ref;			/* VT_USERDEFINED: a type's index */
};

struct s_param {
	const char *name;
	struct s_type type;
	USHORT flags;
};

struct s_func {
	const char *name;
	MEMBERID memid;
	struct s_type ret;
	BYTE nparams;
	struct s_param params[8];
};

#define IN PARAMFLAG_FIN
#define OUT PARAMFLAG_FOUT
#define T(vt, ptr) { vt, ptr, -1 }
#define U(ptr, index) { VT_USERDEFINED, ptr, index }

enum {
	STD_GUID, STD_IUNKNOWN, STD_IDISPATCH, STD_IENUMVARIANT,
	STD_OLE_COLOR, STD_OLE_HANDLE, STD_FONT, STD_IFONTDISP, STD_PICTURE,
	STD_IPICTUREDISP, STD_COUNT
};

static const struct s_func iunknown_funcs[] = {
	{ "QueryInterface", 0x60000000, T(VT_HRESULT, 0), 2,
	  { { "riid", U(1, STD_GUID), IN },
	    { "ppvObj", T(VT_VOID, 2), OUT } } },
	{ "AddRef", 0x60000001, T(VT_UI4, 0), 0, { { NULL } } },
	{ "Release", 0x60000002, T(VT_UI4, 0), 0, { { NULL } } },
};

static const struct s_func idispatch_funcs[] = {
	{ "GetTypeInfoCount", 0x60010000, T(VT_HRESULT, 0), 1,
	  { { "pctinfo", T(VT_UINT, 1), OUT } } },
	{ "GetTypeInfo", 0x60010001, T(VT_HRESULT, 0), 3,
	  { { "itinfo", T(VT_UINT, 0), IN }, { "lcid", T(VT_UI4, 0), IN },
	    { "pptinfo", T(VT_VOID, 2), OUT } } },
	{ "GetIDsOfNames", 0x60010002, T(VT_HRESULT, 0), 5,
	  { { "riid", U(1, STD_GUID), IN }, { "rgszNames", T(VT_I1, 2), IN },
	    { "cNames", T(VT_UINT, 0), IN }, { "lcid", T(VT_UI4, 0), IN },
	    { "rgdispid", T(VT_I4, 1), OUT } } },
	{ "Invoke", 0x60010003, T(VT_HRESULT, 0), 8,
	  { { "dispidMember", T(VT_I4, 0), IN },
	    { "riid", U(1, STD_GUID), IN }, { "lcid", T(VT_UI4, 0), IN },
	    { "wFlags", T(VT_UI2, 0), IN },
	    { "pdispparams", T(VT_VOID, 1), IN },
	    { "pvarResult", T(VT_VARIANT, 1), OUT },
	    { "pexcepinfo", T(VT_VOID, 1), OUT },
	    { "puArgErr", T(VT_UINT, 1), OUT } } },
};

static const struct s_func ienumvariant_funcs[] = {
	{ "Next", 0x60010000, T(VT_HRESULT, 0), 3,
	  { { "celt", T(VT_UI4, 0), IN }, { "rgvar", T(VT_VARIANT, 1), IN },
	    { "pceltFetched", T(VT_UI4, 1), OUT } } },
	{ "Skip", 0x60010001, T(VT_HRESULT, 0), 1,
	  { { "celt", T(VT_UI4, 0), IN } } },
	{ "Reset", 0x60010002, T(VT_HRESULT, 0), 0, { { NULL } } },
	{ "Clone", 0x60010003, T(VT_HRESULT, 0), 1,
	  { { "ppenum", U(2, STD_IENUMVARIANT), OUT } } },
};

#undef T
#undef U
#undef IN
#undef OUT

/*
 * stdole2's types have hrefs of their own (with bit 1 set, which no file
 * uses), so they mean the same from any library: a dual interface's view
 * shows IDispatch's functions, whose parameters refer to stdole2's GUID.
 */
#define STD_HREF_BASE 0x7f000002u
/* Below them, other libraries' hrefs a view shows (struct tl_foreign). */
#define FOREIGN_HREF_BASE 0x7d000002u
#define FOREIGN_MAX ((STD_HREF_BASE - FOREIGN_HREF_BASE) / 4)

static HREFTYPE std_href(int index)
{
	return (HREFTYPE)(STD_HREF_BASE + 4 * (unsigned)index);
}

static int s_tdesc(struct typelib *lib, const struct s_type *s,
		   TYPEDESC *td)
{
	for (int i = 0; i < s->ptr; i++) {
		td->vt = VT_PTR;
		if (!(td->lptdesc = ar_alloc(lib, sizeof(TYPEDESC))))
			return -1;
		td = td->lptdesc;
	}
	td->vt = s->vt;
	if (s->vt == VT_USERDEFINED)
		td->hreftype = std_href(s->ref);
	return 0;
}

static int s_funcs(struct tl_type *t, const struct s_func *s, int n,
		   int vft_base)
{
	struct typelib *lib = t->lib;

	t->attr.cFuncs = n;
	t->attr.cbSizeVft = (vft_base + n) * PTR_SIZE;
	if (!(t->funcs = ar_alloc(lib, n * sizeof(*t->funcs))))
		return -1;
	for (int i = 0; i < n; i++) {
		struct tl_func *f = &t->funcs[i];
		FUNCDESC *d = &f->desc;

		f->name = ar_utf8(lib, s[i].name);
		f->ordinal = -1;
		d->memid = s[i].memid;
		d->funckind = FUNC_PUREVIRTUAL;
		d->invkind = INVOKE_FUNC;
		d->callconv = CC_STDCALL;
		d->cParams = s[i].nparams;
		d->oVft = (vft_base + i) * PTR_SIZE;
		d->wFuncFlags = FUNCFLAG_FRESTRICTED;
		if (s_tdesc(lib, &s[i].ret, &d->elemdescFunc.tdesc))
			return -1;
		if (!s[i].nparams)
			continue;
		d->lprgelemdescParam =
			ar_alloc(lib, s[i].nparams * sizeof(ELEMDESC));
		f->param_names = ar_alloc(lib, s[i].nparams * sizeof(WCHAR *));
		f->param_cust =
			ar_alloc(lib, s[i].nparams * sizeof(struct tl_cust *));
		if (!d->lprgelemdescParam || !f->param_names || !f->param_cust)
			return -1;
		for (int j = 0; j < s[i].nparams; j++) {
			const struct s_param *p = &s[i].params[j];

			f->param_names[j] = ar_utf8(lib, p->name);
			d->lprgelemdescParam[j].paramdesc.wParamFlags =
				p->flags;
			if (s_tdesc(lib, &p->type,
				    &d->lprgelemdescParam[j].tdesc))
				return -1;
		}
	}
	return 0;
}

static int s_interface(struct typelib *lib, int index, const char *name,
		       const GUID *iid, WORD flags, int base,
		       const struct s_func *funcs, int n, int vft_base)
{
	struct tl_type *t = &lib->types[index];

	t->name = ar_utf8(lib, name);
	t->attr.guid = *iid;
	t->attr.typekind = TKIND_INTERFACE;
	t->attr.cbSizeInstance = PTR_SIZE;
	t->attr.cbAlignment = PTR_SIZE;
	t->attr.wTypeFlags = flags;
	if (base >= 0) {
		t->attr.cImplTypes = 1;
		if (!(t->impls = ar_alloc(lib, sizeof(*t->impls))))
			return -1;
		t->impls[0].href = std_href(base);
	}
	return s_funcs(t, funcs, n, vft_base);
}

static int s_alias(struct typelib *lib, int index, const char *name,
		   const GUID *guid, VARTYPE vt)
{
	struct tl_type *t = &lib->types[index];

	t->name = ar_utf8(lib, name);
	t->attr.guid = *guid;
	t->attr.typekind = TKIND_ALIAS;
	t->attr.cbSizeInstance = 4;
	t->attr.cbAlignment = 4;
	t->attr.tdescAlias.vt = vt;
	return 0;
}

/*
 * A dispinterface with no members of its own listed (Font and Picture:
 * what refers to them uses them as IDispatch), and its alias.
 */
static int s_dispatch(struct typelib *lib, int index, const char *name,
		      const GUID *iid, int alias, const char *alias_name)
{
	struct tl_type *t = &lib->types[index], *a = &lib->types[alias];

	t->name = ar_utf8(lib, name);
	t->attr.guid = *iid;
	t->attr.typekind = TKIND_DISPATCH;
	t->attr.cbSizeInstance = PTR_SIZE;
	t->attr.cbAlignment = PTR_SIZE;
	t->attr.cbSizeVft = 7 * PTR_SIZE;
	t->attr.cImplTypes = 1;
	if (!(t->impls = ar_alloc(lib, sizeof(*t->impls))))
		return -1;
	t->impls[0].href = std_href(STD_IDISPATCH);
	a->name = ar_utf8(lib, alias_name);
	a->attr.typekind = TKIND_ALIAS;
	a->attr.cbSizeInstance = PTR_SIZE;
	a->attr.cbAlignment = PTR_SIZE;
	a->attr.tdescAlias.vt = VT_USERDEFINED;
	a->attr.tdescAlias.hreftype = std_href(index);
	return t->name && a->name ? 0 : -1;
}

static int s_guid_record(struct typelib *lib)
{
	static const struct {
		const char *name;
		VARTYPE vt;
		ULONG offset;
	} fields[] = {
		{ "Data1", VT_UI4, 0 }, { "Data2", VT_UI2, 4 },
		{ "Data3", VT_UI2, 6 }, { "Data4", VT_CARRAY, 8 },
	};
	struct tl_type *t = &lib->types[STD_GUID];

	t->name = ar_utf8(lib, "GUID");
	t->attr.typekind = TKIND_RECORD;
	t->attr.cbSizeInstance = 16;
	t->attr.cbAlignment = 4;
	t->attr.cVars = 4;
	if (!(t->vars = ar_alloc(lib, 4 * sizeof(*t->vars))))
		return -1;
	for (int i = 0; i < 4; i++) {
		struct tl_var *v = &t->vars[i];

		v->name = ar_utf8(lib, fields[i].name);
		v->desc.memid = 0x40000000 + i;
		v->desc.varkind = VAR_PERINSTANCE;
		v->desc.oInst = fields[i].offset;
		v->desc.elemdescVar.tdesc.vt = fields[i].vt;
		if (fields[i].vt == VT_CARRAY) {
			ARRAYDESC *a = ar_alloc(lib, sizeof(*a));

			if (!a)
				return -1;
			a->tdescElem.vt = VT_UI1;
			a->cDims = 1;
			a->rgbounds[0].cElements = 8;
			v->desc.elemdescVar.tdesc.lpadesc = a;
		}
	}
	return 0;
}

static struct typelib *stdole2;
static pthread_once_t stdole2_once = PTHREAD_ONCE_INIT;

static void stdole2_build(void)
{
	static const GUID iid_ienumvariant = {
		0x00020404, 0, 0, { 0xc0, 0, 0, 0, 0, 0, 0, 0x46 } };
	static const GUID ole_color = {
		0x66504301, 0xbe0f, 0x101a,
		{ 0x8b, 0xbb, 0x00, 0xaa, 0x00, 0x30, 0x0c, 0xab } };
	static const GUID ole_handle = {
		0x66504313, 0xbe0f, 0x101a,
		{ 0x8b, 0xbb, 0x00, 0xaa, 0x00, 0x30, 0x0c, 0xab } };
	static const GUID font = {
		0xbef6e003, 0xa874, 0x101a,
		{ 0x8b, 0xba, 0x00, 0xaa, 0x00, 0x30, 0x0c, 0xab } };
	static const GUID picture = {
		0x7bf80981, 0xbf32, 0x101a,
		{ 0x8b, 0xbb, 0x00, 0xaa, 0x00, 0x30, 0x0c, 0xab } };
	struct typelib *lib = lib_new();

	if (!lib)
		return;
	lib->builtin = 1;
	lib->attr.guid = stdole2_libid;
	lib->attr.syskind = PTR_SIZE == 8 ? SYS_WIN64 : SYS_WIN32;
	lib->attr.wMajorVerNum = 2;
	lib->name = ar_utf8(lib, "stdole");
	lib->doc = ar_utf8(lib, "OLE Automation");
	lib->ntypes = STD_COUNT;
	lib->dispatch_href = std_href(STD_IDISPATCH);
	lib->types = ar_alloc(lib, STD_COUNT * sizeof(*lib->types));
	if (!lib->types || !lib->name || !lib->doc)
		goto fail;
	for (int i = 0; i < STD_COUNT; i++)
		type_init(lib, &lib->types[i], i);
	if (s_guid_record(lib) ||
	    s_interface(lib, STD_IUNKNOWN, "IUnknown", &oa_IID_IUnknown,
			TYPEFLAG_FHIDDEN, -1, iunknown_funcs, 3, 0) ||
	    s_interface(lib, STD_IDISPATCH, "IDispatch", &oa_IID_IDispatch,
			TYPEFLAG_FRESTRICTED, STD_IUNKNOWN, idispatch_funcs,
			4, 3) ||
	    s_interface(lib, STD_IENUMVARIANT, "IEnumVARIANT",
			&iid_ienumvariant, TYPEFLAG_FHIDDEN, STD_IUNKNOWN,
			ienumvariant_funcs, 4, 3) ||
	    s_alias(lib, STD_OLE_COLOR, "OLE_COLOR", &ole_color, VT_UI4) ||
	    s_alias(lib, STD_OLE_HANDLE, "OLE_HANDLE", &ole_handle, VT_INT) ||
	    s_dispatch(lib, STD_FONT, "Font", &font, STD_IFONTDISP,
		       "IFontDisp") ||
	    s_dispatch(lib, STD_PICTURE, "Picture", &picture,
		       STD_IPICTUREDISP, "IPictureDisp"))
		goto fail;
	stdole2 = lib;
	return;
fail:
	lib_free(lib);
}

static struct typelib *stdole2_lib(void)
{
	pthread_once(&stdole2_once, stdole2_build);
	return stdole2;
}

/*
 * stdole2's types by index, for references that give one: their places in
 * Windows' stdole2.tlb, for the ones libraries in use refer to that way
 * (GUID; IFontDisp, from ATL's ambient Font; IPictureDisp, from LEAD's).
 */
static int stdole2_index(int index)
{
	switch (index) {
	case 0:
		return STD_GUID;
	case 32:
		return STD_IFONTDISP;
	case 36:
		return STD_IPICTUREDISP;
	default:
		return -1;
	}
}

/* ----------------------------------------------------------------------
 * Loading, and the cache of loaded libraries.
 */

static pthread_mutex_t cache_lock = PTHREAD_MUTEX_INITIALIZER;
static struct typelib *cache;

static ULONG lib_addref(struct typelib *lib)
{
	return __atomic_add_fetch(&lib->refs, 1, __ATOMIC_SEQ_CST);
}

static ULONG lib_release(struct typelib *lib)
{
	ULONG r;

	if (lib->builtin)
		return 1;
	pthread_mutex_lock(&cache_lock);
	r = __atomic_sub_fetch(&lib->refs, 1, __ATOMIC_SEQ_CST);
	if (!r) {
		for (struct typelib **p = &cache; *p; p = &(*p)->next)
			if (*p == lib) {
				*p = lib->next;
				break;
			}
	}
	pthread_mutex_unlock(&cache_lock);
	if (!r)
		lib_free(lib);
	return r;
}

static int read_file(const char *path, BYTE **out, size_t *len)
{
	FILE *f = fopen(path, "rb");
	struct stat st;
	BYTE *d;

	if (!f)
		return -1;
	if (fstat(fileno(f), &st) || st.st_size <= 0 ||
	    st.st_size > 512 * 1024 * 1024 ||
	    !(d = malloc((size_t)st.st_size))) {
		fclose(f);
		return -1;
	}
	if (fread(d, 1, (size_t)st.st_size, f) != (size_t)st.st_size) {
		free(d);
		fclose(f);
		return -1;
	}
	fclose(f);
	*out = d;
	*len = (size_t)st.st_size;
	return 0;
}

/* Parses a library from its bytes (a .tlb file's, or a resource's). */
static HRESULT lib_from_bytes(const BYTE *d, size_t len, const char *what,
			      struct typelib **out)
{
	struct typelib *lib;
	HRESULT hr;

	if (len >= 4 && rd32(d) == SLTG_MAGIC) {
		/* Read as the MSFT library it converts to. */
		BYTE *m;
		size_t mlen;

		if (FAILED(hr = oa_sltg_to_msft(d, len, &m, &mlen))) {
			TRACE("LoadTypeLib(%s): a bad SLTG library (%#x)", what,
			      (unsigned)hr);
			return hr;
		}
		hr = lib_from_bytes(m, mlen, what, out);
		free(m);
		return hr;
	}
	if (len < 4 || rd32(d) != MSFT_MAGIC)
		return TYPE_E_CANTLOADLIBRARY;
	if (!(lib = lib_new()))
		return E_OUTOFMEMORY;
	if (FAILED(hr = msft_parse(lib, d, len))) {
		TRACE("LoadTypeLib(%s): a bad type library (%#x)", what,
		      (unsigned)hr);
		lib_free(lib);
		return hr;
	}
	*out = lib;
	return S_OK;
}

/* A library from bytes in memory, outside the cache (ICreateTypeLib's). */
HRESULT oa_typelib_from_bytes(const void *d, size_t len, ITypeLib **out)
{
	struct typelib *lib;
	HRESULT hr = lib_from_bytes(d, len, "memory", &lib);

	if (hr == S_OK)
		*out = (ITypeLib *)&lib->iface;
	return hr;
}

HRESULT oa_typelib_view(const void *d, size_t len, IUnknown *outer,
			oa_outer_qi qi, ITypeLib **out)
{
	struct typelib *lib;
	HRESULT hr = lib_from_bytes(d, len, "memory", &lib);

	if (hr != S_OK)
		return hr;
	outer->lpVtbl->AddRef(outer);
	lib->outer = outer;
	lib->outer_qi = qi;
	*out = (ITypeLib *)&lib->iface;
	return S_OK;
}

/*
 * The library in the file at path (a Unix path): the file itself, or its
 * TYPELIB resource res.
 */
static HRESULT lib_from_file(const char *path, int res, struct typelib **out)
{
	BYTE *d;
	size_t len;
	HRESULT hr;

	if (read_file(path, &d, &len))
		return TYPE_E_CANTLOADLIBRARY;
	if (len >= 2 && d[0] == 'M' && d[1] == 'Z') {
		struct pe_image *img = pe_parse(d, len);
		struct pe_resource r;
		const void *p;

		free(d);
		if (!img)
			return TYPE_E_CANTLOADLIBRARY;
		if (pe_find_resource(img, (pe_resid) "TYPELIB", res,
				     PE_RES_ANY_LANG, &r) ||
		    !(p = pe_ptr(img, r.data_rva, r.size))) {
			TRACE("LoadTypeLib(%s): no TYPELIB resource %d", path,
			      res);
			pe_free(img);
			return TYPE_E_CANTLOADLIBRARY;
		}
		hr = lib_from_bytes(p, r.size, path, out);
		pe_free(img);
		return hr;
	}
	hr = lib_from_bytes(d, len, path, out);
	free(d);
	return hr;
}

/* "name" or "name.tlb" for stdole2 itself. */
static int is_stdole2_name(const char *base)
{
	return !strcasecmp(base, "stdole2.tlb") ||
	       !strcasecmp(base, "stdole32.tlb") ||
	       !strcasecmp(base, "stdole2") || !strcasecmp(base, "stdole32");
}

/*
 * Finds the file a LoadTypeLib argument names: a path, perhaps with a
 * resource number after a backslash, or a bare name searched for as
 * LoadLibrary would. 0 with the Unix path in out and the resource in *res.
 */
static int find_typelib_file(const char *win, char *out, size_t size,
			     int *res)
{
	char buf[1024];
	const char *base;
	char *unix_path;
	struct stat st;

	*res = 1;
	snprintf(buf, sizeof(buf), "%s", win);
	for (int pass = 0; pass < 2; pass++) {
		base = strrchr(buf, '\\');
		base = base ? base + 1 : buf;
		if (!strchr(buf, '\\') && !strchr(buf, '/')) {
			if (!mod_search(buf, NULL, out, size))
				return 0;
		} else if ((unix_path = path_to_unix(buf))) {
			int ok = !stat(unix_path, &st) && S_ISREG(st.st_mode);

			if (ok)
				snprintf(out, size, "%s", unix_path);
			free(unix_path);
			if (ok)
				return 0;
		}
		/* "file.dll\2": the second TYPELIB resource. */
		char *slash = strrchr(buf, '\\');
		char *end;
		long n;

		if (pass || !slash || !slash[1])
			return -1;
		n = strtol(slash + 1, &end, 10);
		if (*end || n <= 0 || n > 0xffff || slash == buf)
			return -1;
		*slash = 0;
		*res = (int)n;
	}
	return -1;
}

static HRESULT tl_load(const char *win, struct typelib **out)
{
	char path[1024], key[1100];
	const char *base;
	int res;
	HRESULT hr;
	struct typelib *lib;

	*out = NULL;
	base = strrchr(win, '\\');
	base = base ? base + 1 : win;
	if (find_typelib_file(win, path, sizeof(path), &res)) {
		if (is_stdole2_name(base) && (lib = stdole2_lib())) {
			*out = lib;
			return S_OK;
		}
		TRACE("LoadTypeLib(%s): no such file", win);
		return TYPE_E_CANTLOADLIBRARY;
	}
	/* An absolute path: the cache's key, and the path registered. */
	{
		char *abs = realpath(path, NULL);

		if (abs) {
			snprintf(path, sizeof(path), "%s", abs);
			free(abs);
		}
	}
	snprintf(key, sizeof(key), "%s\\%d", path, res);
	pthread_mutex_lock(&cache_lock);
	for (lib = cache; lib; lib = lib->next)
		if (!strcmp(lib->key, key)) {
			lib_addref(lib);
			break;
		}
	pthread_mutex_unlock(&cache_lock);
	if (lib) {
		*out = lib;
		return S_OK;
	}
	if (FAILED(hr = lib_from_file(path, res, &lib)))
		return hr;
	if (!(lib->key = strdup(key))) {
		lib_free(lib);
		return E_OUTOFMEMORY;
	}
	pthread_mutex_lock(&cache_lock);
	/* Another thread may have loaded it meanwhile: use that one. */
	for (struct typelib *l = cache; l; l = l->next)
		if (!strcmp(l->key, key)) {
			lib_addref(l);
			pthread_mutex_unlock(&cache_lock);
			lib_free(lib);
			*out = l;
			return S_OK;
		}
	lib->next = cache;
	cache = lib;
	pthread_mutex_unlock(&cache_lock);
	TRACE("LoadTypeLib(%s): %u types", win, lib->ntypes);
	*out = lib;
	return S_OK;
}

static ITypeLib *tl_iface(struct typelib *lib)
{
	return (ITypeLib *)&lib->iface;
}

/*
 * A loaded library with this GUID and major version, and at least this
 * minor version (the newest), with a reference; stdole2 for stdole.
 */
/*
 * How well a library's locale serves one asked for, as LoadRegTypeLib
 * falls back: 3 the same, 2 its language's neutral one, 1 the neutral
 * one (or any, for an import of no locale), 0 not at all.
 */
static int lcid_rank(LCID want, LCID have)
{
	WORD w = (WORD)want, h = (WORD)have;

	if (w == h)
		return 3;
	if (h == (w & 0x3ff))		/* PRIMARYLANGID, SUBLANG_NEUTRAL */
		return 2;
	/* LANG_NEUTRAL; an import that names no locale takes any. */
	return h == 0 || w == 0 ? 1 : 0;
}

static struct typelib *find_loaded(const GUID *libid, WORD major,
				   WORD minor, int exact, LCID lcid)
{
	struct typelib *best = NULL, *std = stdole2_lib();
	int best_rank = 0;

	/* stdole32 (1.0) has the same GUID: the built-in one stands in,
	 * whatever the locale asked for. */
	if (std && guid_eq(libid, &std->attr.guid))
		return std;
	pthread_mutex_lock(&cache_lock);
	for (struct typelib *l = cache; l; l = l->next) {
		int rank = lcid_rank(lcid, l->attr.lcid);
		if (guid_eq(&l->attr.guid, libid) &&
		    l->attr.wMajorVerNum == major &&
		    l->attr.wMinorVerNum >= minor &&
		    (!exact || l->attr.wMinorVerNum == minor) && rank &&
		    (!best || l->attr.wMinorVerNum > best->attr.wMinorVerNum ||
		     (l->attr.wMinorVerNum == best->attr.wMinorVerNum &&
		      rank > best_rank))) {
			best = l;
			best_rank = rank;
		}
	}
	if (best)
		lib_addref(best);
	pthread_mutex_unlock(&cache_lock);
	return best;
}

/* ----------------------------------------------------------------------
 * References between types.
 */

#define HREF_DUAL ((HREFTYPE)-2)

/*
 * The import file entry at off in the IMPFILES segment, parsed (once) and
 * copied to *out.  Only what the library is is kept: each use looks the
 * library up again, so libraries that import each other don't keep each
 * other alive, and one that couldn't be found is tried again next time.
 */
static int lib_import(struct typelib *lib, INT off, struct tl_import *out)
{
	struct msft m = { .lib = lib, .d = lib->data, .len = lib->len };
	const BYTE *p;
	struct tl_import *imp = NULL;

	/* The segment directory again: it follows the type info offsets. */
	INT h = MSFT_HEADER_SIZE + (rd32(lib->data + 4 * H_VARFLAGS) &
				    MSFT_HELPDLL ? 4 : 0) +
		4 * (INT)lib->ntypes;
	for (int i = 0; i < SEG_COUNT; i++) {
		m.seg[i].off = rd32(lib->data + h + 16 * i);
		m.seg[i].len = m.seg[i].off == -1 ? 0
						  : rd32(lib->data + h + 16 * i + 4);
	}
	if (!(p = seg_at(&m, SEG_IMPFILES, off, 14)))
		return -1;
	pthread_mutex_lock(&lib->lock);
	for (size_t i = 0; i < lib->nimports; i++)
		if (lib->imports[i].off == off + 1) {
			imp = &lib->imports[i];
			goto out;
		}
	struct tl_import *n = realloc(lib->imports,
				      (lib->nimports + 1) * sizeof(*n));
	if (!n)
		goto out;
	lib->imports = n;
	imp = &n[lib->nimports++];
	memset(imp, 0, sizeof(*imp));
	imp->off = off + 1;
	msft_guid(&m, rd32(p), &imp->libid);
	imp->lcid = rd32(p + 4);
	imp->major = rd32(p + 8) & 0xffff;
	imp->minor = (DWORD)rd32(p + 8) >> 16;
	int namelen = ((USHORT)rd16(p + 12)) >> 2;
	const BYTE *name = seg_at(&m, SEG_IMPFILES, off + 14, namelen);
	if (name && (imp->file = ar_alloc(lib, namelen + 1)))
		memcpy(imp->file, name, namelen);
out:;
	/* Decided under the lock: imp means nothing once it is released. */
	int r = imp ? 0 : -1;

	if (imp)
		*out = *imp;
	pthread_mutex_unlock(&lib->lock);
	return r;
}

/* Is lib the library imp names (the same GUID, major, and minor or later)? */
static int import_matches(const struct tl_import *imp,
			  const struct typelib *lib)
{
	if (!guid_eq(&lib->attr.guid, &imp->libid))
		return 0;
	return lib->builtin ||
	       (lib->attr.wMajorVerNum == imp->major &&
		(imp->exact ? lib->attr.wMinorVerNum == imp->minor
			    : lib->attr.wMinorVerNum >= imp->minor) &&
		lcid_rank(imp->lcid, lib->attr.lcid));
}

/* The library an import names, with a reference, or NULL. */
static struct typelib *import_load(struct typelib *lib,
				   const struct tl_import *imp)
{
	struct typelib *l = find_loaded(&imp->libid, imp->major, imp->minor,
					imp->exact, imp->lcid);
	ITypeLib *tl = NULL;

	if (l)
		return l;
	if (SUCCEEDED(oa_LoadRegTypeLib(&imp->libid, imp->major, imp->minor,
					imp->lcid, &tl)) && tl) {
		if (tl->lpVtbl == &typelib_vtbl &&
		    import_matches(imp, (struct typelib *)tl))
			return (struct typelib *)tl;
		tl->lpVtbl->Release((ITypeLib2 *)tl);
	}
	if (imp->file && tl_load(imp->file, &l) == S_OK) {
		if (import_matches(imp, l))
			return l;
		TRACE("type library %s: %s isn't the library imported",
		      lib->key ? lib->key : "", imp->file);
		lib_release(l);
		return NULL;
	}
	TRACE("type library %s: can't find %s", lib->key ? lib->key : "",
	      imp->file ? imp->file : "?");
	return NULL;
}

static HRESULT get_type(struct tl_type *t, ITypeInfo **out);

/* The type an href names, from type t; *out holds a reference. */
static HRESULT resolve_href(struct tl_type *t, HREFTYPE href,
			    struct tl_type **out)
{
	struct typelib *lib = t->lib;

	*out = NULL;
	if (href == HREF_DUAL) {
		struct tl_type *d = t->dispatch_view ? t->dispatch_view : t;

		if (d->attr.typekind != TKIND_DISPATCH ||
		    !(d->attr.wTypeFlags & TYPEFLAG_FDUAL))
			return TYPE_E_ELEMENTNOTFOUND;
		if (t->dispatch_view) {
			*out = d;
		} else {
			pthread_mutex_lock(&lib->lock);
			if (!d->iface_view) {
				struct tl_type *v = ar_alloc(lib, sizeof(*v));

				if (v) {
					*v = *d;
					v->attr.typekind = TKIND_INTERFACE;
					v->dispatch_view = d;
					v->iface_view = NULL;
					d->iface_view = v;
				}
			}
			pthread_mutex_unlock(&lib->lock);
			if (!(*out = d->iface_view))
				return E_OUTOFMEMORY;
		}
		lib_addref(lib);
		return S_OK;
	}
	if (href >= STD_HREF_BASE && href < STD_HREF_BASE + 4 * STD_COUNT &&
	    (href & 3) == 2) {
		struct typelib *std = stdole2_lib();

		if (!std)
			return TYPE_E_ELEMENTNOTFOUND;
		*out = &std->types[(href - STD_HREF_BASE) / 4];
		return S_OK;		/* stdole2 isn't counted */
	}
	if (href >= FOREIGN_HREF_BASE && href < STD_HREF_BASE &&
	    (href & 3) == 2) {
		size_t i = (href - FOREIGN_HREF_BASE) / 4;
		struct tl_foreign f;
		struct typelib *other;
		HRESULT hr;

		int known;

		pthread_mutex_lock(&lib->lock);
		if ((known = i < lib->nforeign))
			f = lib->foreign[i];
		pthread_mutex_unlock(&lib->lock);
		if (!known)
			return TYPE_E_ELEMENTNOTFOUND;
		if (!(other = import_load(lib, &f.lib)))
			return TYPE_E_CANTLOADLIBRARY;
		hr = other->ntypes ? resolve_href(&other->types[0], f.href, out)
				   : TYPE_E_ELEMENTNOTFOUND;
		lib_release(other);
		return hr;
	}
	if (!(href & 3)) {
		UINT i = href / MSFT_TYPEINFO_SIZE;

		if (href % MSFT_TYPEINFO_SIZE || i >= lib->ntypes)
			return TYPE_E_ELEMENTNOTFOUND;
		*out = &lib->types[i];
		lib_addref(lib);
		return S_OK;
	}
	if (!lib->data || !(href & 1))
		return TYPE_E_ELEMENTNOTFOUND;

	/* An import: flags, its file, and a GUID or an index. */
	struct msft m = { .lib = lib, .d = lib->data, .len = lib->len };
	INT h = MSFT_HEADER_SIZE +
		(rd32(lib->data + 4 * H_VARFLAGS) & MSFT_HELPDLL ? 4 : 0) +
		4 * (INT)lib->ntypes;
	const BYTE *p;
	struct tl_import imp;

	for (int i = 0; i < SEG_COUNT; i++) {
		m.seg[i].off = rd32(lib->data + h + 16 * i);
		m.seg[i].len = m.seg[i].off == -1 ? 0
						  : rd32(lib->data + h + 16 * i + 4);
	}
	if (!(p = seg_at(&m, SEG_IMPINFO, href & ~3u, 12)))
		return TYPE_E_ELEMENTNOTFOUND;
	INT flags = rd32(p), what = rd32(p + 8);
	struct typelib *other;
	HRESULT hr = TYPE_E_ELEMENTNOTFOUND;

	if (lib_import(lib, rd32(p + 4), &imp))
		return TYPE_E_ELEMENTNOTFOUND;
	if (!(other = import_load(lib, &imp)))
		return TYPE_E_CANTLOADLIBRARY;
	/* other's reference becomes the reference *out holds. */
	if (flags & 0x10000) {
		GUID g;

		if (!msft_guid(&m, what, &g))
			for (UINT i = 0; i < other->ntypes; i++)
				if (guid_eq(&other->types[i].attr.guid, &g)) {
					*out = &other->types[i];
					return S_OK;
				}
		TRACE("type library: no type in %s with the GUID asked for",
		      imp.file ? imp.file : "?");
		goto fail;
	}
	if (other->builtin) {
		int i = stdole2_index(what);

		if (i < 0) {
			TRACE("type library: stdole2's type #%d isn't known",
			      what);
			goto fail;
		}
		what = i;
	}
	if (what >= 0 && (UINT)what < other->ntypes) {
		*out = &other->types[what];
		return S_OK;
	}
fail:
	lib_release(other);
	return hr;
}

static HRESULT get_type(struct tl_type *t, ITypeInfo **out)
{
	lib_addref(t->lib);
	*out = (ITypeInfo *)&t->iface;
	return S_OK;
}

/* ----------------------------------------------------------------------
 * Views: what a type info shows, which for a dispatch interface is not
 * quite what is stored.
 */

static int is_dispatch(const struct tl_type *t)
{
	return t->attr.typekind == TKIND_DISPATCH && !t->dispatch_view;
}

static int is_dual_dispatch(const struct tl_type *t)
{
	return is_dispatch(t) && (t->attr.wTypeFlags & TYPEFLAG_FDUAL);
}

/* The base of an interface (its first implemented type), or NULL. */
static struct tl_type *base_of(struct tl_type *t)
{
	struct tl_type *b;

	if (!t->attr.cImplTypes || t->impls[0].href == (HREFTYPE)-1 ||
	    FAILED(resolve_href(t, t->impls[0].href, &b)))
		return NULL;
	if (b->attr.typekind == TKIND_DISPATCH &&
	    (b->attr.wTypeFlags & TYPEFLAG_FDUAL) && !b->dispatch_view) {
		struct tl_type *v;

		/* A dual base: its vtable is the interface view's. */
		if (SUCCEEDED(resolve_href(b, HREF_DUAL, &v))) {
			lib_release(b->lib);
			b = v;
		}
	}
	return b;
}

/*
 * The i-th function of a dual dispatch interface, inherited ones first:
 * walks the bases. *owner gets the type that holds it, with a reference.
 */
static struct tl_func *dual_func(struct tl_type *t, UINT i,
				 struct tl_type **owner, int depth)
{
	struct tl_type *b = base_of(t);
	UINT inherited = 0;

	if (b) {
		inherited = b->attr.cbSizeVft / PTR_SIZE;
		if (b->attr.typekind != TKIND_INTERFACE)
			inherited = 0;
		if (i < inherited) {
			struct tl_func *f =
				depth < 32 ? dual_func(b, i, owner, depth + 1)
					   : NULL;

			lib_release(b->lib);
			return f;
		}
		lib_release(b->lib);
	}
	i -= inherited;
	if (i >= t->attr.cFuncs)
		return NULL;
	lib_addref(t->lib);
	*owner = t;
	return &t->funcs[i];
}

static UINT view_func_count(struct tl_type *t)
{
	if (is_dual_dispatch(t))
		return t->attr.cbSizeVft / PTR_SIZE;
	return t->attr.cFuncs;
}

/* The function shown at index i; *owner holds a reference. */
static struct tl_func *view_func(struct tl_type *t, UINT i,
				 struct tl_type **owner)
{
	if (is_dual_dispatch(t))
		return dual_func(t, i, owner, 0);
	if (i >= t->attr.cFuncs)
		return NULL;
	lib_addref(t->lib);
	*owner = t;
	return &t->funcs[i];
}

/* ----------------------------------------------------------------------
 * ITypeInfo2.
 */

WINFN(HRESULT) ti_QueryInterface(ITypeInfo2 *self, REFIID riid, void **ppv)
{
	struct typelib *lib = type_of(self)->lib;

	if (!ppv)
		return E_POINTER;
	if (lib->outer && riid && !guid_eq(riid, &IID_ITypeInfo) &&
	    !guid_eq(riid, &IID_ITypeInfo2))
		return lib->outer_qi(lib->outer, (int)type_of(self)->index,
				     riid, ppv);
	if (riid && (guid_eq(riid, &oa_IID_IUnknown) ||
		     guid_eq(riid, &IID_ITypeInfo) ||
		     guid_eq(riid, &IID_ITypeInfo2))) {
		lib_addref(type_of(self)->lib);
		*ppv = self;
		return S_OK;
	}
	*ppv = NULL;
	return E_NOINTERFACE;
}

WINFN(ULONG) ti_AddRef(ITypeInfo2 *self)
{
	return lib_addref(type_of(self)->lib);
}

WINFN(ULONG) ti_Release(ITypeInfo2 *self)
{
	return lib_release(type_of(self)->lib);
}

static void *out_alloc(size_t size)
{
	return heap_alloc(heap_process(), HEAP_ZERO_MEMORY, size);
}

static void out_free(void *p)
{
	if (p)
		heap_free(heap_process(), 0, p);
}

WINFN(HRESULT) ti_GetTypeAttr(ITypeInfo2 *self, TYPEATTR **out)
{
	struct tl_type *t = type_of(self);
	TYPEATTR *a;

	if (!out)
		return E_INVALIDARG;
	if (!(a = out_alloc(sizeof(*a))))
		return E_OUTOFMEMORY;
	*a = t->attr;
	if (is_dispatch(t)) {
		a->cFuncs = view_func_count(t);
		a->cbSizeVft = 7 * PTR_SIZE;
		a->cImplTypes = 1;
		a->wTypeFlags &= ~TYPEFLAG_FOLEAUTOMATION;
	}
	*out = a;
	return S_OK;
}

WINFN(HRESULT) ti_GetTypeComp(ITypeInfo2 *self, ITypeComp **out)
{
	if (out)
		*out = NULL;
	TRACE("ITypeInfo::GetTypeComp: not implemented");
	return E_NOTIMPL;
}

static WCHAR *lib_win_path(const struct typelib *lib);

/*
 * href, a reference in other's types, as one in lib's: other's own
 * references are made into entries of lib's foreign list.
 */
static HREFTYPE foreign_href(struct typelib *lib, struct typelib *other,
			     HREFTYPE href, const char *file)
{
	HREFTYPE r = (HREFTYPE)-1;
	size_t i;

	if (other == lib || other->builtin ||
	    (href >= STD_HREF_BASE && href < STD_HREF_BASE + 4 * STD_COUNT))
		return href;
	pthread_mutex_lock(&lib->lock);
	for (i = 0; i < lib->nforeign; i++) {
		const struct tl_foreign *f = &lib->foreign[i];

		if (f->href == href && guid_eq(&f->lib.libid, &other->attr.guid) &&
		    f->lib.major == other->attr.wMajorVerNum &&
		    f->lib.minor == other->attr.wMinorVerNum)
			goto found;
	}
	if (i < FOREIGN_MAX) {
		struct tl_foreign *n = realloc(lib->foreign,
					       (i + 1) * sizeof(*n));
		char *copy = file ? ar_alloc(lib, strlen(file) + 1) : NULL;

		if (!n)
			goto out;
		lib->foreign = n;
		memset(&n[i], 0, sizeof(n[i]));
		n[i].lib.libid = other->attr.guid;
		n[i].lib.major = other->attr.wMajorVerNum;
		n[i].lib.minor = other->attr.wMinorVerNum;
		n[i].lib.lcid = other->attr.lcid;
		/* The href is that version's: a later one may number differently. */
		n[i].lib.exact = 1;
		if (copy)
			n[i].lib.file = strcpy(copy, file);
		n[i].href = href;
		lib->nforeign++;
		goto found;
	}
	goto out;
found:
	r = FOREIGN_HREF_BASE + 4 * (HREFTYPE)i;
out:
	pthread_mutex_unlock(&lib->lock);
	return r;
}

#define ALIGN8(n) (((n) + 7) & ~(size_t)7)

/* The room the descriptions td points to take. */
static size_t tdesc_extra(const TYPEDESC *td)
{
	switch (td->vt) {
	case VT_PTR:
	case VT_SAFEARRAY:
		return ALIGN8(sizeof(TYPEDESC)) + tdesc_extra(td->lptdesc);
	case VT_CARRAY:
		return ALIGN8(sizeof(ARRAYDESC) + (td->lpadesc->cDims - 1) *
						     sizeof(SAFEARRAYBOUND)) +
		       tdesc_extra(&td->lpadesc->tdescElem);
	default:
		return 0;
	}
}

struct relocate {
	BYTE *next;
	struct typelib *lib, *other;
	const char *file;
};

static void *reloc_take(struct relocate *r, size_t n)
{
	void *p = r->next;

	r->next += ALIGN8(n);
	return p;
}

/* *td, which points into other's memory, with its own copy. */
static void tdesc_relocate(TYPEDESC *td, struct relocate *r)
{
	switch (td->vt) {
	case VT_PTR:
	case VT_SAFEARRAY: {
		TYPEDESC *c = reloc_take(r, sizeof(*c));

		*c = *td->lptdesc;
		tdesc_relocate(c, r);
		td->lptdesc = c;
		break;
	}
	case VT_CARRAY: {
		size_t n = sizeof(ARRAYDESC) + (td->lpadesc->cDims - 1) *
						     sizeof(SAFEARRAYBOUND);
		ARRAYDESC *a = reloc_take(r, n);

		memcpy(a, td->lpadesc, n);
		tdesc_relocate(&a->tdescElem, r);
		td->lpadesc = a;
		break;
	}
	case VT_USERDEFINED:
		td->hreftype = foreign_href(r->lib, r->other, td->hreftype,
					    r->file);
		break;
	default:
		break;
	}
}

static size_t paramdesc_extra(const PARAMDESC *p)
{
	size_t n;

	if (!(p->wParamFlags & PARAMFLAG_FHASDEFAULT) || !p->pparamdescex)
		return 0;
	n = ALIGN8(sizeof(PARAMDESCEX));
	if (p->pparamdescex->varDefaultValue.vt == VT_BSTR &&
	    p->pparamdescex->varDefaultValue.bstrVal)
		n += ALIGN8(4 + oa_SysStringByteLen(
				    p->pparamdescex->varDefaultValue.bstrVal) +
			    2);
	return n;
}

static void paramdesc_relocate(PARAMDESC *p, struct relocate *r)
{
	PARAMDESCEX *x;
	BSTR b;

	if (!(p->wParamFlags & PARAMFLAG_FHASDEFAULT) || !p->pparamdescex) {
		p->pparamdescex = NULL;
		return;
	}
	x = reloc_take(r, sizeof(*x));
	*x = *p->pparamdescex;
	b = x->varDefaultValue.bstrVal;
	if (x->varDefaultValue.vt == VT_BSTR && b) {
		UINT len = oa_SysStringByteLen(b);
		BYTE *q = reloc_take(r, 4 + len + 2);

		memcpy(q, &len, 4);
		memcpy(q + 4, b, len);
		memset(q + 4 + len, 0, 2);
		x->varDefaultValue.bstrVal = (BSTR)(q + 4);
	}
	p->pparamdescex = x;
}

/*
 * d with everything it points to in its own block, for a function that
 * belongs to another library than the view showing it: that library may
 * go once the description is returned, and its type references mean
 * nothing in this one.
 */
static FUNCDESC *funcdesc_relocate(FUNCDESC *d, struct typelib *lib,
				   struct typelib *other)
{
	size_t n = ALIGN8(sizeof(*d) + d->cParams * sizeof(ELEMDESC));
	WCHAR *w = lib_win_path(other);
	char *file = w ? utf16_to_utf8_dup(w) : NULL;
	struct relocate r = { .lib = lib, .other = other, .file = file };
	FUNCDESC *c;

	free(w);
	n += tdesc_extra(&d->elemdescFunc.tdesc) +
	     paramdesc_extra(&d->elemdescFunc.paramdesc);
	for (SHORT i = 0; i < d->cParams; i++)
		n += tdesc_extra(&d->lprgelemdescParam[i].tdesc) +
		     paramdesc_extra(&d->lprgelemdescParam[i].paramdesc);
	if ((c = out_alloc(n))) {
		*c = *d;
		r.next = (BYTE *)c + ALIGN8(sizeof(*c) +
					    d->cParams * sizeof(ELEMDESC));
		c->lprgelemdescParam = d->cParams ? (ELEMDESC *)(c + 1) : NULL;
		if (d->cParams)
			memcpy(c->lprgelemdescParam, d->lprgelemdescParam,
			       d->cParams * sizeof(ELEMDESC));
		tdesc_relocate(&c->elemdescFunc.tdesc, &r);
		paramdesc_relocate(&c->elemdescFunc.paramdesc, &r);
		for (SHORT i = 0; i < c->cParams; i++) {
			tdesc_relocate(&c->lprgelemdescParam[i].tdesc, &r);
			paramdesc_relocate(&c->lprgelemdescParam[i].paramdesc,
					   &r);
		}
	}
	free(file);
	out_free(d);
	return c;
}

/* A copy of a function's description for the caller, as a view shows it. */
static FUNCDESC *copy_funcdesc(const struct tl_func *f, int dispatch)
{
	const FUNCDESC *s = &f->desc;
	FUNCDESC *d = out_alloc(sizeof(*d) + s->cParams * sizeof(ELEMDESC));

	if (!d)
		return NULL;
	*d = *s;
	d->lprgscode = NULL;
	d->cScodes = 0;
	d->lprgelemdescParam = s->cParams ? (ELEMDESC *)(d + 1) : NULL;
	if (s->cParams)
		memcpy(d->lprgelemdescParam, s->lprgelemdescParam,
		       s->cParams * sizeof(ELEMDESC));
	if (dispatch && d->funckind != FUNC_DISPATCH) {
		d->funckind = FUNC_DISPATCH;
		/* IDispatch returns an [out, retval] parameter as the result. */
		if (d->elemdescFunc.tdesc.vt == VT_HRESULT) {
			ELEMDESC *last = d->cParams ? &d->lprgelemdescParam[d->cParams - 1]
						    : NULL;

			d->elemdescFunc.tdesc.vt = VT_VOID;
			if (last &&
			    (last->paramdesc.wParamFlags & PARAMFLAG_FRETVAL) &&
			    last->tdesc.vt == VT_PTR) {
				d->elemdescFunc.tdesc = *last->tdesc.lptdesc;
				d->elemdescFunc.paramdesc.wParamFlags = 0;
				d->elemdescFunc.paramdesc.pparamdescex = NULL;
				d->cParams--;
			}
		}
		if (!d->cParams)
			d->lprgelemdescParam = NULL;
	}
	return d;
}

WINFN(HRESULT) ti_GetFuncDesc(ITypeInfo2 *self, UINT index, FUNCDESC **out)
{
	struct tl_type *t = type_of(self), *owner;
	struct tl_func *f;

	if (!out)
		return E_INVALIDARG;
	*out = NULL;
	if (!(f = view_func(t, index, &owner)))
		return TYPE_E_ELEMENTNOTFOUND;
	*out = copy_funcdesc(f, is_dual_dispatch(t));
	if (*out && owner->lib != t->lib && !owner->lib->builtin)
		*out = funcdesc_relocate(*out, t->lib, owner->lib);
	lib_release(owner->lib);
	return *out ? S_OK : E_OUTOFMEMORY;
}

WINFN(HRESULT) ti_GetVarDesc(ITypeInfo2 *self, UINT index, VARDESC **out)
{
	struct tl_type *t = type_of(self);
	VARDESC *d;

	if (!out)
		return E_INVALIDARG;
	*out = NULL;
	if (index >= t->attr.cVars)
		return TYPE_E_ELEMENTNOTFOUND;
	if (!(d = out_alloc(sizeof(*d))))
		return E_OUTOFMEMORY;
	*d = t->vars[index].desc;
	*out = d;
	return S_OK;
}

/*
 * The function or variable with memid among those the view shows (a
 * function of the invoke kind, if kind isn't 0), searching the bases of
 * an interface too. *owner holds a reference; the index is in its own
 * list.
 */
static int find_member(struct tl_type *t, MEMBERID memid, INVOKEKIND kind,
		       struct tl_type **owner, struct tl_func **func,
		       struct tl_var **var, int depth)
{
	struct tl_type *b;

	*func = NULL;
	*var = NULL;
	for (UINT i = 0; i < t->attr.cFuncs; i++)
		if (t->funcs[i].desc.memid == memid &&
		    (!kind || t->funcs[i].desc.invkind == kind)) {
			*func = &t->funcs[i];
			goto found;
		}
	for (UINT i = 0; i < t->attr.cVars; i++)
		if (t->vars[i].desc.memid == memid) {
			*var = &t->vars[i];
			goto found;
		}
	if (depth > 32 ||
	    (t->attr.typekind == TKIND_DISPATCH && !is_dual_dispatch(t)) ||
	    !(b = base_of(t)))
		return 0;
	int r = find_member(b, memid, kind, owner, func, var, depth + 1);
	lib_release(b->lib);
	return r;
found:
	lib_addref(t->lib);
	*owner = t;
	return 1;
}

WINFN(HRESULT) ti_GetNames(ITypeInfo2 *self, MEMBERID memid, BSTR *names,
			   UINT max, UINT *count)
{
	struct tl_type *t = type_of(self), *owner;
	struct tl_func *f;
	struct tl_var *v;
	UINT n = 0;

	if (!names || !count)
		return E_INVALIDARG;
	*count = 0;
	if (!find_member(t, memid, 0, &owner, &f, &v, 0))
		return TYPE_E_ELEMENTNOTFOUND;
	if (max && (n = 1))
		names[0] = oa_SysAllocString(f ? f->name : v->name);
	if (f) {
		/* A property put's value has no name, and isn't counted. */
		int params = f->desc.cParams;

		if (f->desc.invkind & (INVOKE_PROPERTYPUT |
				       INVOKE_PROPERTYPUTREF))
			params--;
		for (int i = 0; i < params && n < max; i++)
			names[n++] = oa_SysAllocString(f->param_names[i]);
	}
	lib_release(owner->lib);
	*count = n;
	return S_OK;
}

WINFN(HRESULT) ti_GetRefTypeOfImplType(ITypeInfo2 *self, UINT index,
				       HREFTYPE *out)
{
	struct tl_type *t = type_of(self);

	if (!out)
		return E_INVALIDARG;
	if (index == (UINT)-1) {
		/* Either half of a dual interface: the other half. */
		if (!is_dual_dispatch(t) && !t->dispatch_view)
			return TYPE_E_ELEMENTNOTFOUND;
		*out = HREF_DUAL;
		return S_OK;
	}
	if (is_dispatch(t) && index == 0) {
		/* Every dispatch interface derives from IDispatch. */
		*out = t->lib->dispatch_href;
		if (*out != (HREFTYPE)-1)
			return S_OK;
	}
	if (index >= t->attr.cImplTypes)
		return TYPE_E_ELEMENTNOTFOUND;
	*out = t->impls[index].href;
	return S_OK;
}

WINFN(HRESULT) ti_GetImplTypeFlags(ITypeInfo2 *self, UINT index, INT *out)
{
	struct tl_type *t = type_of(self);

	if (!out)
		return E_INVALIDARG;
	if (index >= t->attr.cImplTypes) {
		*out = 0;
		return TYPE_E_ELEMENTNOTFOUND;
	}
	*out = t->impls[index].flags;
	return S_OK;
}

/* A function or variable by name, as find_member() gives it. */
static int find_name(struct tl_type *t, const WCHAR *name,
		     struct tl_type **owner, struct tl_func **func,
		     struct tl_var **var, int depth)
{
	struct tl_type *b;

	*func = NULL;
	*var = NULL;
	for (UINT i = 0; i < t->attr.cFuncs; i++)
		if (name_eq(t->funcs[i].name, name)) {
			*func = &t->funcs[i];
			goto found;
		}
	for (UINT i = 0; i < t->attr.cVars; i++)
		if (name_eq(t->vars[i].name, name)) {
			*var = &t->vars[i];
			goto found;
		}
	if (depth > 32 ||
	    (t->attr.typekind == TKIND_DISPATCH && !is_dual_dispatch(t)) ||
	    !(b = base_of(t)))
		return 0;
	int r = find_name(b, name, owner, func, var, depth + 1);
	lib_release(b->lib);
	return r;
found:
	lib_addref(t->lib);
	*owner = t;
	return 1;
}

WINFN(HRESULT) ti_GetIDsOfNames(ITypeInfo2 *self, LPOLESTR *names,
				UINT count, MEMBERID *ids)
{
	struct tl_type *t = type_of(self), *owner;
	struct tl_func *f;
	struct tl_var *v;
	HRESULT hr = S_OK;

	if (!names || !ids || !count)
		return E_INVALIDARG;
	for (UINT i = 0; i < count; i++)
		ids[i] = MEMBERID_NIL;
	if (!find_name(t, names[0], &owner, &f, &v, 0))
		return DISP_E_UNKNOWNNAME;
	ids[0] = f ? f->desc.memid : v->desc.memid;
	for (UINT i = 1; i < count; i++) {
		int found = 0;

		for (int j = 0; f && j < f->desc.cParams; j++)
			if (name_eq(f->param_names[j], names[i])) {
				ids[i] = j;
				found = 1;
				break;
			}
		if (!found)
			hr = DISP_E_UNKNOWNNAME;
	}
	lib_release(owner->lib);
	return hr;
}

WINFN(HRESULT) ti_Invoke(ITypeInfo2 *self, PVOID instance, MEMBERID memid,
			 WORD flags, DISPPARAMS *params, VARIANT *result,
			 EXCEPINFO *excep, UINT *arg_err)
{
	return oa_typeinfo_invoke((ITypeInfo *)self, instance, memid, flags,
				  params, result, excep, arg_err);
}

static BSTR bstr_or_null(const WCHAR *s)
{
	return s ? oa_SysAllocString(s) : NULL;
}

WINFN(HRESULT) ti_GetDocumentation(ITypeInfo2 *self, MEMBERID memid,
				   BSTR *name, BSTR *doc, DWORD *help_context,
				   BSTR *help_file)
{
	struct tl_type *t = type_of(self), *owner;
	struct tl_func *f;
	struct tl_var *v;

	if (help_file)
		*help_file = bstr_or_null(t->lib->help_file);
	if (memid == MEMBERID_NIL) {
		if (name)
			*name = bstr_or_null(t->name);
		if (doc)
			*doc = bstr_or_null(t->doc);
		if (help_context)
			*help_context = t->help_context;
		return S_OK;
	}
	if (!find_member(t, memid, 0, &owner, &f, &v, 0)) {
		if (help_file) {
			oa_SysFreeString(*help_file);
			*help_file = NULL;
		}
		return TYPE_E_ELEMENTNOTFOUND;
	}
	if (name)
		*name = bstr_or_null(f ? f->name : v->name);
	if (doc)
		*doc = bstr_or_null(f ? f->doc : v->doc);
	if (help_context)
		*help_context = f ? f->help_context : v->help_context;
	lib_release(owner->lib);
	return S_OK;
}

WINFN(HRESULT) ti_GetDllEntry(ITypeInfo2 *self, MEMBERID memid,
			      INVOKEKIND kind, BSTR *dll, BSTR *name,
			      WORD *ordinal)
{
	struct tl_type *t = type_of(self);

	if (dll)
		*dll = NULL;
	if (name)
		*name = NULL;
	if (ordinal)
		*ordinal = 0;
	if (t->attr.typekind != TKIND_MODULE)
		return TYPE_E_BADMODULEKIND;
	for (UINT i = 0; i < t->attr.cFuncs; i++) {
		struct tl_func *f = &t->funcs[i];

		if (f->desc.memid != memid || f->desc.invkind != kind)
			continue;
		if (dll)
			*dll = bstr_or_null(t->dll_name);
		if (f->ordinal >= 0) {
			if (ordinal)
				*ordinal = (WORD)f->ordinal;
		} else if (name) {
			*name = bstr_or_null(f->entry);
		}
		return S_OK;
	}
	return TYPE_E_ELEMENTNOTFOUND;
}

WINFN(HRESULT) ti_GetRefTypeInfo(ITypeInfo2 *self, HREFTYPE href,
				 ITypeInfo **out)
{
	struct tl_type *t = type_of(self), *r;
	HRESULT hr;

	if (!out)
		return E_INVALIDARG;
	*out = NULL;
	if (FAILED(hr = resolve_href(t, href, &r)))
		return hr;
	*out = (ITypeInfo *)&r->iface;
	return S_OK;
}

/*
 * A module's function's address: its DLL loaded (and kept loaded, as
 * Windows keeps it while the type library is), and its entry looked up by
 * name or ordinal.
 */
WINFN(HRESULT) ti_AddressOfMember(ITypeInfo2 *self, MEMBERID memid,
				  INVOKEKIND kind, PVOID *addr)
{
	BSTR dll = NULL, name = NULL;
	WORD ordinal = 0;
	HRESULT hr;

	if (!addr)
		return E_INVALIDARG;
	*addr = NULL;
	if (FAILED(hr = ti_GetDllEntry(self, memid, kind, &dll, &name,
				       &ordinal)))
		return hr;
	char *d = dll ? utf16_to_utf8_dup(dll) : NULL;
	char *n = name ? utf16_to_utf8_dup(name) : NULL;
	oa_SysFreeString(dll);
	oa_SysFreeString(name);
	HMODULE h = d && *d ? k32_load_library(d, NULL) : NULL;
	struct module *m = h ? mod_from_base(h) : NULL;
	if (m)
		*addr = n ? mod_get_proc(m, n) : mod_get_ordinal(m, ordinal);
	TRACE("ITypeInfo::AddressOfMember(%#x): %s!%s = %p", (unsigned)memid,
	      d ? d : "?", n ? n : "(ordinal)", *addr);
	free(d);
	free(n);
	if (!m)
		return TYPE_E_CANTLOADLIBRARY;
	return *addr ? S_OK : TYPE_E_DLLFUNCTIONNOTFOUND;
}

WINFN(HRESULT) ti_CreateInstance(ITypeInfo2 *self, IUnknown *outer,
				 REFIID riid, PVOID *obj)
{
	if (obj)
		*obj = NULL;
	TRACE("ITypeInfo::CreateInstance: not implemented");
	return E_NOTIMPL;
}

WINFN(HRESULT) ti_GetMops(ITypeInfo2 *self, MEMBERID memid, BSTR *mops)
{
	if (!mops)
		return E_INVALIDARG;
	*mops = NULL;
	return S_OK;
}

WINFN(HRESULT) ti_GetContainingTypeLib(ITypeInfo2 *self, ITypeLib **lib,
				       UINT *index)
{
	struct tl_type *t = type_of(self);

	if (lib) {
		lib_addref(t->lib);
		*lib = (ITypeLib *)&t->lib->iface;
	}
	if (index)
		*index = t->index;
	return S_OK;
}

WINFN(void) ti_ReleaseTypeAttr(ITypeInfo2 *self, TYPEATTR *attr)
{
	out_free(attr);
}

WINFN(void) ti_ReleaseFuncDesc(ITypeInfo2 *self, FUNCDESC *desc)
{
	out_free(desc);
}

WINFN(void) ti_ReleaseVarDesc(ITypeInfo2 *self, VARDESC *desc)
{
	out_free(desc);
}

WINFN(HRESULT) ti_GetTypeKind(ITypeInfo2 *self, TYPEKIND *kind)
{
	struct tl_type *t = type_of(self);

	if (!kind)
		return E_INVALIDARG;
	*kind = t->attr.typekind;
	return S_OK;
}

WINFN(HRESULT) ti_GetTypeFlags(ITypeInfo2 *self, ULONG *flags)
{
	if (!flags)
		return E_INVALIDARG;
	*flags = type_of(self)->attr.wTypeFlags;
	return S_OK;
}

WINFN(HRESULT) ti_GetFuncIndexOfMemId(ITypeInfo2 *self, MEMBERID memid,
				      INVOKEKIND kind, UINT *index)
{
	struct tl_type *t = type_of(self), *owner;
	UINT n = view_func_count(t);

	if (!index)
		return E_INVALIDARG;
	for (UINT i = 0; i < n; i++) {
		struct tl_func *f = view_func(t, i, &owner);
		int hit;

		if (!f)
			continue;
		hit = f->desc.memid == memid && f->desc.invkind == kind;
		lib_release(owner->lib);
		if (hit) {
			*index = i;
			return S_OK;
		}
	}
	return TYPE_E_ELEMENTNOTFOUND;
}

WINFN(HRESULT) ti_GetVarIndexOfMemId(ITypeInfo2 *self, MEMBERID memid,
				     UINT *index)
{
	struct tl_type *t = type_of(self);

	if (!index)
		return E_INVALIDARG;
	for (UINT i = 0; i < t->attr.cVars; i++)
		if (t->vars[i].desc.memid == memid) {
			*index = i;
			return S_OK;
		}
	return TYPE_E_ELEMENTNOTFOUND;
}

/* Custom data: a value by GUID (VT_EMPTY if none), or all of it. */
static HRESULT cust_get(const struct tl_cust *c, REFGUID guid, VARIANT *v)
{
	if (!guid || !v)
		return E_INVALIDARG;
	oa_VariantInit(v);
	for (; c; c = c->next)
		if (guid_eq(&c->guid, guid))
			return oa_VariantCopy(v, &c->value);
	return S_OK;
}

static HRESULT cust_all(const struct tl_cust *c, CUSTDATA *out)
{
	DWORD n = 0;

	if (!out)
		return E_INVALIDARG;
	out->cCustData = 0;
	out->prgCustData = NULL;
	for (const struct tl_cust *i = c; i; i = i->next)
		n++;
	if (!n)
		return S_OK;
	if (!(out->prgCustData = out_alloc(n * sizeof(CUSTDATAITEM))))
		return E_OUTOFMEMORY;
	for (; c; c = c->next) {
		CUSTDATAITEM *it = &out->prgCustData[out->cCustData++];

		it->guid = c->guid;
		oa_VariantInit(&it->varValue);
		oa_VariantCopy(&it->varValue, &c->value);
	}
	return S_OK;
}

WINFN(HRESULT) ti_GetCustData(ITypeInfo2 *self, REFGUID guid, VARIANT *v)
{
	return cust_get(type_of(self)->cust, guid, v);
}

WINFN(HRESULT) ti_GetFuncCustData(ITypeInfo2 *self, UINT index,
				  REFGUID guid, VARIANT *v)
{
	struct tl_type *t = type_of(self), *owner;
	struct tl_func *f = view_func(t, index, &owner);
	HRESULT hr;

	if (!f)
		return TYPE_E_ELEMENTNOTFOUND;
	hr = cust_get(f->cust, guid, v);
	lib_release(owner->lib);
	return hr;
}

WINFN(HRESULT) ti_GetParamCustData(ITypeInfo2 *self, UINT func, UINT param,
				   REFGUID guid, VARIANT *v)
{
	struct tl_type *t = type_of(self), *owner;
	struct tl_func *f = view_func(t, func, &owner);
	HRESULT hr = TYPE_E_ELEMENTNOTFOUND;

	if (!f)
		return hr;
	if (param < (UINT)f->desc.cParams)
		hr = cust_get(f->param_cust[param], guid, v);
	lib_release(owner->lib);
	return hr;
}

WINFN(HRESULT) ti_GetVarCustData(ITypeInfo2 *self, UINT index, REFGUID guid,
				 VARIANT *v)
{
	struct tl_type *t = type_of(self);

	if (index >= t->attr.cVars)
		return TYPE_E_ELEMENTNOTFOUND;
	return cust_get(t->vars[index].cust, guid, v);
}

WINFN(HRESULT) ti_GetImplTypeCustData(ITypeInfo2 *self, UINT index,
				      REFGUID guid, VARIANT *v)
{
	struct tl_type *t = type_of(self);

	if (index >= t->attr.cImplTypes)
		return TYPE_E_ELEMENTNOTFOUND;
	return cust_get(t->impls[index].cust, guid, v);
}

WINFN(HRESULT) ti_GetDocumentation2(ITypeInfo2 *self, MEMBERID memid,
				    LCID lcid, BSTR *help_string,
				    DWORD *help_context, BSTR *help_dll)
{
	struct tl_type *t = type_of(self), *owner;
	struct tl_func *f;
	struct tl_var *v;

	if (help_dll)
		*help_dll = bstr_or_null(t->lib->help_dll);
	if (memid == MEMBERID_NIL) {
		if (help_string)
			*help_string = bstr_or_null(t->doc);
		if (help_context)
			*help_context = t->help_string_context;
		return S_OK;
	}
	if (!find_member(t, memid, 0, &owner, &f, &v, 0)) {
		if (help_dll) {
			oa_SysFreeString(*help_dll);
			*help_dll = NULL;
		}
		return TYPE_E_ELEMENTNOTFOUND;
	}
	if (help_string)
		*help_string = bstr_or_null(f ? f->doc : v->doc);
	if (help_context)
		*help_context = f ? f->help_string_context
				  : v->help_string_context;
	lib_release(owner->lib);
	return S_OK;
}

WINFN(HRESULT) ti_GetAllCustData(ITypeInfo2 *self, CUSTDATA *data)
{
	return cust_all(type_of(self)->cust, data);
}

WINFN(HRESULT) ti_GetAllFuncCustData(ITypeInfo2 *self, UINT index,
				     CUSTDATA *data)
{
	struct tl_type *t = type_of(self), *owner;
	struct tl_func *f = view_func(t, index, &owner);
	HRESULT hr;

	if (!f)
		return TYPE_E_ELEMENTNOTFOUND;
	hr = cust_all(f->cust, data);
	lib_release(owner->lib);
	return hr;
}

WINFN(HRESULT) ti_GetAllParamCustData(ITypeInfo2 *self, UINT func,
				      UINT param, CUSTDATA *data)
{
	struct tl_type *t = type_of(self), *owner;
	struct tl_func *f = view_func(t, func, &owner);
	HRESULT hr = TYPE_E_ELEMENTNOTFOUND;

	if (!f)
		return hr;
	if (param < (UINT)f->desc.cParams)
		hr = cust_all(f->param_cust[param], data);
	lib_release(owner->lib);
	return hr;
}

WINFN(HRESULT) ti_GetAllVarCustData(ITypeInfo2 *self, UINT index,
				    CUSTDATA *data)
{
	struct tl_type *t = type_of(self);

	if (index >= t->attr.cVars)
		return TYPE_E_ELEMENTNOTFOUND;
	return cust_all(t->vars[index].cust, data);
}

WINFN(HRESULT) ti_GetAllImplTypeCustData(ITypeInfo2 *self, UINT index,
					 CUSTDATA *data)
{
	struct tl_type *t = type_of(self);

	if (index >= t->attr.cImplTypes)
		return TYPE_E_ELEMENTNOTFOUND;
	return cust_all(t->impls[index].cust, data);
}

static const ITypeInfo2Vtbl typeinfo_vtbl = {
	ti_QueryInterface, ti_AddRef, ti_Release, ti_GetTypeAttr,
	ti_GetTypeComp, ti_GetFuncDesc, ti_GetVarDesc, ti_GetNames,
	ti_GetRefTypeOfImplType, ti_GetImplTypeFlags, ti_GetIDsOfNames,
	ti_Invoke, ti_GetDocumentation, ti_GetDllEntry, ti_GetRefTypeInfo,
	ti_AddressOfMember, ti_CreateInstance, ti_GetMops,
	ti_GetContainingTypeLib, ti_ReleaseTypeAttr, ti_ReleaseFuncDesc,
	ti_ReleaseVarDesc, ti_GetTypeKind, ti_GetTypeFlags,
	ti_GetFuncIndexOfMemId, ti_GetVarIndexOfMemId, ti_GetCustData,
	ti_GetFuncCustData, ti_GetParamCustData, ti_GetVarCustData,
	ti_GetImplTypeCustData, ti_GetDocumentation2, ti_GetAllCustData,
	ti_GetAllFuncCustData, ti_GetAllParamCustData, ti_GetAllVarCustData,
	ti_GetAllImplTypeCustData,
};

/* ----------------------------------------------------------------------
 * ITypeLib2.
 */

WINFN(HRESULT) tl_QueryInterface(ITypeLib2 *self, REFIID riid, void **ppv)
{
	struct typelib *lib = lib_of(self);

	if (!ppv)
		return E_POINTER;
	if (lib->outer && riid && !guid_eq(riid, &IID_ITypeLib) &&
	    !guid_eq(riid, &IID_ITypeLib2))
		return lib->outer_qi(lib->outer, -1, riid, ppv);
	if (riid && (guid_eq(riid, &oa_IID_IUnknown) ||
		     guid_eq(riid, &IID_ITypeLib) ||
		     guid_eq(riid, &IID_ITypeLib2))) {
		lib_addref(lib_of(self));
		*ppv = self;
		return S_OK;
	}
	*ppv = NULL;
	return E_NOINTERFACE;
}

WINFN(ULONG) tl_AddRef(ITypeLib2 *self)
{
	return lib_addref(lib_of(self));
}

WINFN(ULONG) tl_Release(ITypeLib2 *self)
{
	return lib_release(lib_of(self));
}

WINFN(UINT) tl_GetTypeInfoCount(ITypeLib2 *self)
{
	return lib_of(self)->ntypes;
}

WINFN(HRESULT) tl_GetTypeInfo(ITypeLib2 *self, UINT index, ITypeInfo **out)
{
	struct typelib *lib = lib_of(self);

	if (!out)
		return E_INVALIDARG;
	*out = NULL;
	if (index >= lib->ntypes)
		return TYPE_E_ELEMENTNOTFOUND;
	return get_type(&lib->types[index], out);
}

WINFN(HRESULT) tl_GetTypeInfoType(ITypeLib2 *self, UINT index,
				  TYPEKIND *kind)
{
	struct typelib *lib = lib_of(self);

	if (!kind)
		return E_INVALIDARG;
	if (index >= lib->ntypes)
		return TYPE_E_ELEMENTNOTFOUND;
	*kind = lib->types[index].attr.typekind;
	return S_OK;
}

WINFN(HRESULT) tl_GetTypeInfoOfGuid(ITypeLib2 *self, REFGUID guid,
				    ITypeInfo **out)
{
	struct typelib *lib = lib_of(self);

	if (!guid || !out)
		return E_INVALIDARG;
	*out = NULL;
	if (guid_eq(guid, &oa_IID_NULL))
		return TYPE_E_ELEMENTNOTFOUND;	/* the types with none */
	for (UINT i = 0; i < lib->ntypes; i++)
		if (guid_eq(&lib->types[i].attr.guid, guid))
			return get_type(&lib->types[i], out);
	return TYPE_E_ELEMENTNOTFOUND;
}

WINFN(HRESULT) tl_GetLibAttr(ITypeLib2 *self, TLIBATTR **out)
{
	TLIBATTR *a;

	if (!out)
		return E_INVALIDARG;
	if (!(a = out_alloc(sizeof(*a))))
		return E_OUTOFMEMORY;
	*a = lib_of(self)->attr;
	*out = a;
	return S_OK;
}

WINFN(HRESULT) tl_GetTypeComp(ITypeLib2 *self, ITypeComp **out)
{
	if (out)
		*out = NULL;
	TRACE("ITypeLib::GetTypeComp: not implemented");
	return E_NOTIMPL;
}

WINFN(HRESULT) tl_GetDocumentation(ITypeLib2 *self, INT index, BSTR *name,
				   BSTR *doc, DWORD *help_context,
				   BSTR *help_file)
{
	struct typelib *lib = lib_of(self);

	if (index == -1) {
		if (name)
			*name = bstr_or_null(lib->name);
		if (doc)
			*doc = bstr_or_null(lib->doc);
		if (help_context)
			*help_context = lib->help_context;
		if (help_file)
			*help_file = bstr_or_null(lib->help_file);
		return S_OK;
	}
	if (index < 0 || (UINT)index >= lib->ntypes)
		return TYPE_E_ELEMENTNOTFOUND;
	return ti_GetDocumentation(&lib->types[index].iface, MEMBERID_NIL,
				   name, doc, help_context, help_file);
}

/* Each name in the library: types, their members and their parameters. */
static int lib_find_name(struct typelib *lib, const WCHAR *name,
			 const WCHAR **match, ITypeInfo **infos,
			 MEMBERID *memids, USHORT max)
{
	USHORT n = 0;

	for (UINT i = 0; i < lib->ntypes && (!infos || n < max); i++) {
		struct tl_type *t = &lib->types[i];
		const WCHAR *hit = NULL;
		MEMBERID id = MEMBERID_NIL;

		if (name_eq(t->name, name))
			hit = t->name;
		for (UINT j = 0; !hit && j < t->attr.cFuncs; j++) {
			if (name_eq(t->funcs[j].name, name)) {
				hit = t->funcs[j].name;
				id = t->funcs[j].desc.memid;
			}
			/* IsName knows parameters; FindName has no id for them. */
			for (int k = 0;
			     !infos && !hit && k < t->funcs[j].desc.cParams; k++)
				if (name_eq(t->funcs[j].param_names[k], name))
					hit = t->funcs[j].param_names[k];
		}
		for (UINT j = 0; !hit && j < t->attr.cVars; j++)
			if (name_eq(t->vars[j].name, name)) {
				hit = t->vars[j].name;
				id = t->vars[j].desc.memid;
			}
		if (!hit)
			continue;
		if (match)
			*match = hit;
		if (infos) {
			get_type(t, &infos[n]);
			if (memids)
				memids[n] = id;
		}
		n++;
		if (!infos)
			break;
	}
	return n;
}

WINFN(HRESULT) tl_IsName(ITypeLib2 *self, LPOLESTR name, ULONG hash,
			 BOOL *found)
{
	const WCHAR *match = NULL;

	if (!name || !found)
		return E_INVALIDARG;
	*found = lib_find_name(lib_of(self), name, &match, NULL, NULL, 0) > 0;
	/* The name as the library spells it. */
	if (*found && match && wstrlen(match) == wstrlen(name))
		memcpy(name, match, wstrlen(match) * sizeof(WCHAR));
	return S_OK;
}

WINFN(HRESULT) tl_FindName(ITypeLib2 *self, LPOLESTR name, ULONG hash,
			   ITypeInfo **infos, MEMBERID *memids,
			   USHORT *found)
{
	if (!name || !infos || !memids || !found || !*found)
		return E_INVALIDARG;
	*found = lib_find_name(lib_of(self), name, NULL, infos, memids,
			       *found);
	return S_OK;
}

WINFN(void) tl_ReleaseTLibAttr(ITypeLib2 *self, TLIBATTR *attr)
{
	out_free(attr);
}

WINFN(HRESULT) tl_GetCustData(ITypeLib2 *self, REFGUID guid, VARIANT *v)
{
	return cust_get(lib_of(self)->cust, guid, v);
}

WINFN(HRESULT) tl_GetLibStatistics(ITypeLib2 *self, ULONG *names,
				   ULONG *chars)
{
	struct typelib *lib = lib_of(self);
	ULONG n = 0, c = 0;

	for (UINT i = 0; i < lib->ntypes; i++) {
		struct tl_type *t = &lib->types[i];

		if (t->name) {
			n++;
			c += wstrlen(t->name);
		}
		for (UINT j = 0; j < t->attr.cFuncs; j++)
			if (t->funcs[j].name) {
				n++;
				c += wstrlen(t->funcs[j].name);
			}
		for (UINT j = 0; j < t->attr.cVars; j++)
			if (t->vars[j].name) {
				n++;
				c += wstrlen(t->vars[j].name);
			}
	}
	if (names)
		*names = n;
	if (chars)
		*chars = c;
	return S_OK;
}

WINFN(HRESULT) tl_GetDocumentation2(ITypeLib2 *self, INT index, LCID lcid,
				    BSTR *help_string, DWORD *help_context,
				    BSTR *help_dll)
{
	struct typelib *lib = lib_of(self);

	if (index == -1) {
		if (help_string)
			*help_string = bstr_or_null(lib->doc);
		if (help_context)
			*help_context = lib->help_string_context;
		if (help_dll)
			*help_dll = bstr_or_null(lib->help_dll);
		return S_OK;
	}
	if (index < 0 || (UINT)index >= lib->ntypes)
		return TYPE_E_ELEMENTNOTFOUND;
	return ti_GetDocumentation2(&lib->types[index].iface, MEMBERID_NIL,
				    lcid, help_string, help_context,
				    help_dll);
}

WINFN(HRESULT) tl_GetAllCustData(ITypeLib2 *self, CUSTDATA *data)
{
	return cust_all(lib_of(self)->cust, data);
}

static const ITypeLib2Vtbl typelib_vtbl = {
	tl_QueryInterface, tl_AddRef, tl_Release, tl_GetTypeInfoCount,
	tl_GetTypeInfo, tl_GetTypeInfoType, tl_GetTypeInfoOfGuid,
	tl_GetLibAttr, tl_GetTypeComp, tl_GetDocumentation, tl_IsName,
	tl_FindName, tl_ReleaseTLibAttr, tl_GetCustData,
	tl_GetLibStatistics, tl_GetDocumentation2, tl_GetAllCustData,
};

/* ----------------------------------------------------------------------
 * The exports.
 */

/* The Windows path of a library's file, "\\N" after it for resource N. */
static WCHAR *lib_win_path(const struct typelib *lib)
{
	char *unix_path, *slash, *win, buf[1100];
	WCHAR *w = NULL;
	long res;

	if (!lib->key || !(unix_path = strdup(lib->key)))
		return NULL;
	slash = strrchr(unix_path, '\\');
	res = slash ? strtol(slash + 1, NULL, 10) : 1;
	if (slash)
		*slash = 0;
	if ((win = path_to_windows(unix_path))) {
		if (res != 1)
			snprintf(buf, sizeof(buf), "%s\\%ld", win, res);
		else
			snprintf(buf, sizeof(buf), "%s", win);
		w = utf8_to_utf16_dup(buf);
		free(win);
	}
	free(unix_path);
	return w;
}

/* A path with a drive letter or a UNC prefix: not one to search for. */
static int is_full_path(LPCOLESTR f)
{
	return f[0] == '\\' ||
	       (((f[0] | 0x20) >= 'a' && (f[0] | 0x20) <= 'z') && f[1] == ':');
}

HRESULT STUB_WINAPI oa_LoadTypeLibEx(LPCOLESTR file, REGKIND kind,
				     ITypeLib **out)
{
	char *f;
	struct typelib *lib;
	HRESULT hr;

	if (!file || !out)
		return E_INVALIDARG;
	*out = NULL;
	if (!(f = utf16_to_utf8_dup(file)))
		return E_OUTOFMEMORY;
	hr = tl_load(f, &lib);
	free(f);
	if (FAILED(hr))
		return hr;
	/*
	 * REGKIND_REGISTER: as RegisterTypeLib, with the file found.
	 * REGKIND_DEFAULT (LoadTypeLib) registers too when it had to search
	 * for the file: a name without a drive or a UNC prefix.
	 */
	if ((kind == REGKIND_REGISTER ||
	     (kind == REGKIND_DEFAULT && !is_full_path(file))) &&
	    !lib->builtin) {
		WCHAR *path = lib_win_path(lib);

		hr = path ? oa_register_typelib(tl_iface(lib), path, NULL, 0)
			  : E_OUTOFMEMORY;
		free(path);
		if (FAILED(hr)) {
			lib_release(lib);
			return hr;
		}
	}
	*out = tl_iface(lib);
	return S_OK;
}

WINFN(HRESULT) oa_LoadTypeLib(LPCOLESTR file, ITypeLib **out)
{
	return oa_LoadTypeLibEx(file, REGKIND_DEFAULT, out);
}

/*
 * stdole2 is always there; other libraries load from the path the
 * registry gives (oleaut_typereg.c).
 */
HRESULT STUB_WINAPI oa_LoadRegTypeLib(REFGUID guid, WORD major, WORD minor,
				      LCID lcid, ITypeLib **out)
{
	struct typelib *std = stdole2_lib();
	BSTR path = NULL;
	HRESULT hr;

	if (!guid || !out)
		return E_INVALIDARG;
	*out = NULL;
	if (std && guid_eq(guid, &std->attr.guid) &&
	    major == std->attr.wMajorVerNum) {
		*out = tl_iface(std);
		return S_OK;
	}
	if (FAILED(hr = oa_QueryPathOfRegTypeLib(guid, major, minor, lcid,
						 &path)))
		return hr;
	hr = oa_LoadTypeLibEx(path, REGKIND_NONE, out);
	oa_SysFreeString(path);
	return hr;
}

WINFN(void) oa_ClearCustData(CUSTDATA *data)
{
	if (!data)
		return;
	for (DWORD i = 0; i < data->cCustData; i++)
		oa_VariantClear(&data->prgCustData[i].varValue);
	out_free(data->prgCustData);
	data->prgCustData = NULL;
	data->cCustData = 0;
}

/*
 * CreateDispTypeInfo: a library of the interface data describes (type 0,
 * its methods virtual, at iMeth in the vtable) and a coclass (type 1)
 * implementing it, which is the type info given out.
 */
static int disp_tdesc(struct typelib *lib, VARTYPE vt, TYPEDESC *td)
{
	if (vt & VT_BYREF) {
		TYPEDESC *in = ar_alloc(lib, sizeof(*in));

		if (!in)
			return -1;
		in->vt = vt & ~VT_BYREF;
		td->vt = VT_PTR;
		td->lptdesc = in;
		return 0;
	}
	td->vt = vt;
	return 0;
}

WINFN(HRESULT) oa_CreateDispTypeInfo(INTERFACEDATA *data, LCID lcid,
				     ITypeInfo **out)
{
	struct typelib *lib;
	struct tl_type *it, *co;
	UINT vft = 0;

	if (!out)
		return E_INVALIDARG;
	*out = NULL;
	if (!data || (data->cMembers && !data->pmethdata) ||
	    data->cMembers > 0xffff)
		return E_INVALIDARG;
	if (!(lib = lib_new()))
		return E_OUTOFMEMORY;
	lib->attr.lcid = lcid;
	lib->attr.syskind = PTR_SIZE == 8 ? SYS_WIN64 : SYS_WIN32;
	if (!(lib->types = ar_alloc(lib, 2 * sizeof(*lib->types))))
		goto oom;
	lib->ntypes = 2;
	it = &lib->types[0];
	co = &lib->types[1];
	type_init(lib, it, 0);
	type_init(lib, co, 1);
	it->attr.typekind = TKIND_INTERFACE;
	it->attr.cbAlignment = PTR_SIZE;
	it->attr.cFuncs = data->cMembers;
	if (data->cMembers &&
	    !(it->funcs = ar_alloc(lib, data->cMembers * sizeof(*it->funcs))))
		goto oom;
	for (UINT i = 0; i < data->cMembers; i++) {
		const METHODDATA *m = &data->pmethdata[i];
		struct tl_func *f = &it->funcs[i];
		FUNCDESC *d = &f->desc;

		if (m->cArgs > 0x7fff || (m->cArgs && !m->ppdata) ||
		    m->iMeth >= 0x7fff / PTR_SIZE)
			goto bad;
		f->ordinal = -1;
		if (m->szName && !(f->name = ar_alloc(lib, (wstrlen(m->szName) + 1) *
							 sizeof(WCHAR))))
			goto oom;
		if (m->szName)
			memcpy(f->name, m->szName,
			       (wstrlen(m->szName) + 1) * sizeof(WCHAR));
		d->memid = m->dispid;
		d->funckind = FUNC_VIRTUAL;
		d->invkind = m->wFlags & 0xf ? m->wFlags & 0xf : INVOKE_FUNC;
		d->callconv = m->cc;
		d->cParams = (SHORT)m->cArgs;
		d->oVft = (SHORT)(m->iMeth * PTR_SIZE);
		if (m->iMeth + 1 > vft)
			vft = m->iMeth + 1;
		if (disp_tdesc(lib, m->vtReturn, &d->elemdescFunc.tdesc))
			goto oom;
		if (!m->cArgs)
			continue;
		d->lprgelemdescParam = ar_alloc(lib, m->cArgs * sizeof(ELEMDESC));
		f->param_names = ar_alloc(lib, m->cArgs * sizeof(WCHAR *));
		f->param_cust = ar_alloc(lib, m->cArgs *
					      sizeof(struct tl_cust *));
		if (!d->lprgelemdescParam || !f->param_names || !f->param_cust)
			goto oom;
		for (UINT j = 0; j < m->cArgs; j++) {
			const PARAMDATA *pd = &m->ppdata[j];
			ELEMDESC *e = &d->lprgelemdescParam[j];

			if (pd->szName) {
				size_t len = wstrlen(pd->szName) + 1;

				if (!(f->param_names[j] =
					      ar_alloc(lib, len * sizeof(WCHAR))))
					goto oom;
				memcpy(f->param_names[j], pd->szName,
				       len * sizeof(WCHAR));
			}
			e->paramdesc.wParamFlags = PARAMFLAG_FIN;
			if (disp_tdesc(lib, pd->vt, &e->tdesc))
				goto oom;
		}
	}
	it->attr.cbSizeVft = (WORD)(vft * PTR_SIZE);
	co->attr.typekind = TKIND_COCLASS;
	co->attr.cbAlignment = 4;
	co->attr.cImplTypes = 1;
	if (!(co->impls = ar_alloc(lib, sizeof(*co->impls))))
		goto oom;
	co->impls[0].href = 0;		/* type 0 */
	co->impls[0].flags = IMPLTYPEFLAG_FDEFAULT;
	*out = (ITypeInfo *)&co->iface;
	return S_OK;
bad:
	lib_free(lib);
	return E_INVALIDARG;
oom:
	lib_free(lib);
	return E_OUTOFMEMORY;
}

#define E(name, ord) { #name, ord, (void *)oa_##name }

const struct builtin_export oa_typelib_exports[] = {
	E(CreateDispTypeInfo, 31),
	E(LoadTypeLib, 161),
	E(LoadRegTypeLib, 162),
	E(ClearCustData, 171),
	E(LoadTypeLibEx, 183),
	BUILTIN_END
};
