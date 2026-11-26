/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * oleaut32's reader for "SLTG" type libraries, the format before MSFT
 * (OLE 2.0's, still found in older controls' TYPELIB resources). The
 * library is rebuilt through oleaut_create.c's model and handed to the
 * MSFT reader as bytes, so everything else sees one kind of library.
 *
 * The layout, from the format's documentation of these files:
 * - A header ("SLTG", the number of blocks + 1, the first block's entry),
 *   then that many-1 block entries (length, index-string offset, next
 *   block's entry, 1-based), "\1CompObj\0dir\0", the blocks' 11-byte
 *   index strings and 9 bytes of padding. The blocks follow, in the order
 *   the entries chain them; the last is the library block.
 * - The library block: magic 0x51cc, its name's name-table offset,
 *   three counted strings (unknown, doc string, help file), help context,
 *   SYSKIND, LCID, a DWORD, library flags, version and GUID; 0x40 bytes;
 *   per type, its index string, another string, its name's offset, a
 *   counted run of bytes (its compressed doc string, not read), help
 *   context, GUID and kind; then the name table's offset from the block
 *   (0x218 bytes on from where it points, 0x20 more when that is 0x0200).
 *   Names are NUL-terminated there.
 * - A type's block: a 0x22-byte header (kind, flags, version, offsets of
 *   its reference table and its members), the members, then a 0x36-byte
 *   tail (counts, the first function's, variable's and base's offsets,
 *   the alias, size, alignment and vtable size).
 * - Types are WORDs: the VARTYPE in the low 6 bits, 0xe00 for a pointer
 *   to it, and VT_PTR, VT_SAFEARRAY, VT_CARRAY (an offset to its
 *   SAFEARRAY) and VT_USERDEFINED (then a reference table index * 4)
 *   leading to more; a parameter's direction in the top bits.
 * - A reference table entry names "*\R<lib>*#<index>": a type of this
 *   library (lib ffff) or of the one whose "*\G{guid}#maj.min#lcid#path#"
 *   is at that name-table offset.
 * Dispatch interfaces' base is IDispatch whatever their entry says.
 * Doc strings of types and functions (compressed) are not read.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "common/log.h"
#include "win32/codepage.h"
#include "win32/internal.h"
#include "win32/oleaut.h"

#define SLTG_MAGIC 0x47544c53
#define LIBBLK_MAGIC 0x51cc
#define TIHEADER_MAGIC 0x0501
#define REF_MAGIC 0xdf
#define IMPL_MAGIC 0x004a
#define FUNC_FLAGS_PRESENT 0x20
#define FUNC_MAGIC 0x4c
#define DISPATCH_FUNC_MAGIC 0xcb
#define STATIC_FUNC_MAGIC 0x8b
#define VAR_MAGIC 0x0a
#define VAR_WITH_FLAGS_MAGIC 0x2a
#define TIHEADER_SIZE 0x22
#define TAIL_SIZE 0x36
#define MAX_TD 64

static const GUID stdole_libid = { 0x00020430, 0, 0,
	{ 0xc0, 0, 0, 0, 0, 0, 0, 0x46 } };
/* stdole32's types 3, 4 and 5, which libraries refer to by index. */
static const GUID stdole_types[3] = {
	{ 0x00000000, 0, 0, { 0xc0, 0, 0, 0, 0, 0, 0, 0x46 } },
	{ 0x00020400, 0, 0, { 0xc0, 0, 0, 0, 0, 0, 0, 0x46 } },
	{ 0x00020404, 0, 0, { 0xc0, 0, 0, 0, 0, 0, 0, 0x46 } },
};

struct sltg {
	const BYTE *d;
	size_t len;
	int bad;
	size_t nt;		/* the name table */
	size_t lo, hi;		/* the block being read (hi 0: the file) */
	ICreateTypeLib2 *ctl;
	UINT ntypes;
	/* TYPEDESCs and ARRAYDESCs for the member being added */
	void *td[MAX_TD];
	int ntd;
};

/* Is [o, o + n) inside the block being read, and the file? */
static int in_block(struct sltg *s, size_t o, size_t n)
{
	size_t lo = s->hi ? s->lo : 0, hi = s->hi ? s->hi : s->len;

	return o >= lo && o <= hi && hi - o >= n;
}

static WORD w16(struct sltg *s, size_t o)
{
	if (!in_block(s, o, 2)) {
		if (!s->bad)
			TRACE("SLTG: a WORD past the end (%zu)", o);
		s->bad = 1;
		return 0;
	}
	return (WORD)(s->d[o] | s->d[o + 1] << 8);
}

static DWORD w32(struct sltg *s, size_t o)
{
	return w16(s, o) | (DWORD)w16(s, o + 2) << 16;
}

static BYTE b8(struct sltg *s, size_t o)
{
	if (!in_block(s, o, 1)) {
		s->bad = 1;
		return 0;
	}
	return s->d[o];
}

static void get_guid(struct sltg *s, size_t o, GUID *g)
{
	if (!in_block(s, o, sizeof(GUID))) {
		s->bad = 1;
		memset(g, 0, sizeof(*g));
		return;
	}
	g->Data1 = w32(s, o);
	g->Data2 = w16(s, o + 4);
	g->Data3 = w16(s, o + 6);
	memcpy(g->Data4, s->d + o + 8, 8);
}

/* The NUL-terminated string at o, or NULL. */
static const char *cstr(struct sltg *s, size_t o)
{
	if (o >= s->len || !memchr(s->d + o, 0, s->len - o)) {
		s->bad = 1;
		return NULL;
	}
	return (const char *)s->d + o;
}

static WCHAR *widen(const char *a, int n)
{
	int need = cp_to_utf16(0, a, n, NULL, 0, 0);
	WCHAR *w = need >= 0 ? malloc((need + 1) * sizeof(WCHAR)) : NULL;

	if (w) {
		cp_to_utf16(0, a, n, w, need, 0);
		w[need] = 0;
	}
	return w;
}

/* The name at off in the name table (malloc'd), or NULL. */
static WCHAR *name_at(struct sltg *s, WORD off)
{
	const char *a = off == 0xffff ? NULL : cstr(s, s->nt + off);

	return a ? widen(a, (int)strlen(a)) : NULL;
}

/* A counted string at *o (WORD length, 0xffff for none), stepped past. */
static WCHAR *counted(struct sltg *s, size_t *o)
{
	WORD n = w16(s, *o);

	*o += 2;
	if (n == 0xffff)
		return NULL;
	if (!in_block(s, *o, n)) {
		s->bad = 1;
		return NULL;
	}
	WCHAR *w = widen((const char *)s->d + *o, n);
	*o += n;
	return w;
}

static void *td_new(struct sltg *s, size_t size)
{
	void *p;

	if (s->ntd >= MAX_TD || !(p = calloc(1, size))) {
		s->bad = 1;
		return NULL;
	}
	s->td[s->ntd++] = p;
	return p;
}

static void td_clear(struct sltg *s)
{
	while (s->ntd)
		free(s->td[--s->ntd]);
}

/* ---- types ---- */
struct refs {
	HREFTYPE *href;		/* (HREFTYPE)-1: not resolved */
	UINT n;
};

/* The type at *o into td; *o steps past it. items: the members' start. */
static void do_type(struct sltg *s, size_t *o, size_t items,
		    const struct refs *refs, TYPEDESC *td)
{
	for (int depth = 0; depth < 16 && !s->bad; depth++) {
		WORD w = w16(s, *o);

		if ((w & 0xe00) == 0xe00) {
			td->vt = VT_PTR;
			if (!(td->lptdesc = td_new(s, sizeof(TYPEDESC))))
				return;
			td = td->lptdesc;
		}
		switch (w & 0x3f) {
		case VT_PTR:
		case VT_SAFEARRAY:
			td->vt = w & 0x3f;
			if (!(td->lptdesc = td_new(s, sizeof(TYPEDESC))))
				return;
			td = td->lptdesc;
			*o += (w & 0x3f) == VT_SAFEARRAY ? 4 : 2;
			continue;
		case VT_USERDEFINED: {
			WORD i = w16(s, *o + 2) / 4;
			*o += 4;
			if (i < refs->n && refs->href[i] != (HREFTYPE)-1) {
				td->vt = VT_USERDEFINED;
				td->hreftype = refs->href[i];
			} else {
				/* Another library's, not found: opaque. */
				td->vt = VT_UNKNOWN;
			}
			return;
		}
		case VT_CARRAY: {
			size_t sa = items + w16(s, *o + 2);
			WORD dims = w16(s, sa);
			if (!dims || dims > 32) {
				s->bad = 1;
				return;
			}
			ARRAYDESC *a = td_new(s, sizeof(ARRAYDESC) +
						 (dims - 1) *
						 sizeof(SAFEARRAYBOUND));
			if (!a)
				return;
			a->cDims = dims;
			/* A 32-bit SAFEARRAY: the bounds after 16 bytes. */
			for (WORD k = 0; k < dims; k++) {
				a->rgbounds[k].cElements = w32(s, sa + 16 +
							       8 * k);
				a->rgbounds[k].lLbound =
					(LONG)w32(s, sa + 20 + 8 * k);
			}
			td->vt = VT_CARRAY;
			td->lpadesc = a;
			td = &a->tdescElem;
			*o += 4;
			continue;
		}
		default:
			td->vt = w & 0x3f;
			*o += 2;
			return;
		}
	}
	s->bad = 1;
}

/* A parameter: its direction and flags, then its type. */
static void do_elem(struct sltg *s, size_t *o, size_t items,
		    const struct refs *refs, ELEMDESC *e)
{
	WORD w = w16(s, *o);
	USHORT f;

	if ((w & 0xc000) == 0xc000)
		f = PARAMFLAG_NONE;
	else if (w & 0x8000)
		f = PARAMFLAG_FIN | PARAMFLAG_FOUT;
	else if (w & 0x4000)
		f = PARAMFLAG_FOUT;
	else
		f = PARAMFLAG_FIN;
	if (w & 0x2000)
		f |= PARAMFLAG_FLCID;
	if (w & 0x80)
		f |= PARAMFLAG_FRETVAL;
	e->paramdesc.wParamFlags = f;
	do_type(s, o, items, refs, &e->tdesc);
}

static int hex_field(const char **p, char stop, unsigned *out)
{
	char *end;
	unsigned long v = strtoul(*p, &end, 16);

	if (end == *p || *end != stop)
		return -1;
	*out = (unsigned)v;
	*p = end + 1;
	return 0;
}

static int parse_guid(const char *p, GUID *g)
{
	unsigned v[11];
	int n = -1;	/* stays so unless %n is reached */

	if (sscanf(p, "{%8x-%4x-%4x-%2x%2x-%2x%2x%2x%2x%2x%2x}%n", &v[0],
		   &v[1], &v[2], &v[3], &v[4], &v[5], &v[6], &v[7], &v[8],
		   &v[9], &v[10], &n) != 11 || n != 38)
		return -1;
	g->Data1 = v[0];
	g->Data2 = (WORD)v[1];
	g->Data3 = (WORD)v[2];
	for (int i = 0; i < 8; i++)
		g->Data4[i] = (BYTE)v[3 + i];
	return 0;
}

/* "*\G{guid}#maj.min#lcid#path#" at lib_off: an import of type index. */
static HREFTYPE import_ref(struct sltg *s, unsigned lib_off, unsigned index)
{
	const char *p = cstr(s, s->nt + lib_off);
	GUID libid;
	unsigned maj, min, lcid;
	char *end;
	HREFTYPE href;

	if (!p || strncmp(p, "*\\G", 3) || parse_guid(p + 3, &libid))
		return (HREFTYPE)-1;
	p += 3 + 38;
	if (*p++ != '#')
		return (HREFTYPE)-1;
	maj = (unsigned)strtoul(p, &end, 10);
	if (*end != '.')
		return (HREFTYPE)-1;
	min = (unsigned)strtoul(end + 1, &end, 10);
	p = end;
	if (*p++ != '#' || hex_field(&p, '#', &lcid))
		return (HREFTYPE)-1;
	/* The file's base name, as an import names it. */
	const char *path_end = strchr(p, '#');
	size_t n = path_end ? (size_t)(path_end - p) : strlen(p);
	char file[260];
	const char *base = p;
	for (size_t i = 0; i < n; i++)
		if (p[i] == '\\' || p[i] == '/' || p[i] == ':')
			base = p + i + 1;
	n -= (size_t)(base - p);
	if (n >= sizeof(file))
		n = sizeof(file) - 1;
	memcpy(file, base, n);
	file[n] = 0;

	const GUID *guid = NULL;
	if (!memcmp(&libid, &stdole_libid, sizeof(GUID)) && index >= 3 &&
	    index <= 5)
		guid = &stdole_types[index - 3];
	if (oa_ctl_import(s->ctl, &libid, (WORD)maj, (WORD)min, lcid, file,
			  guid, index, &href) != S_OK)
		return (HREFTYPE)-1;
	return href;
}

/* The reference table at o: each entry's href. */
static int do_refs(struct sltg *s, size_t o, struct refs *r)
{
	r->href = NULL;
	r->n = 0;
	if (b8(s, o) != REF_MAGIC)
		return -1;
	DWORD bytes = w32(s, o + 0x44);
	if (s->bad || bytes / 8 > 0x10000)
		return -1;
	r->n = bytes / 8;
	if (r->n && !(r->href = calloc(r->n, sizeof(*r->href))))
		return -1;
	size_t p = o + 0x4f + bytes;
	for (UINT i = 0; i < r->n && !s->bad; i++) {
		WORD len = w16(s, p);
		char buf[64];
		unsigned lib, index;

		p += 2;
		r->href[i] = (HREFTYPE)-1;
		if (p > s->len || s->len - p < len || len >= sizeof(buf)) {
			s->bad = 1;
			break;
		}
		memcpy(buf, s->d + p, len);
		buf[len] = 0;
		p += len;
		const char *q = buf + 3;
		if (strncmp(buf, "*\\R", 3) || hex_field(&q, '*', &lib) ||
		    *q++ != '#' || hex_field(&q, 0, &index)) {
			TRACE("SLTG: reference \"%s\" not understood", buf);
			continue;
		}
		if (lib == 0xffff) {
			if (index < s->ntypes)
				r->href[i] = index * 0x64;
		} else {
			r->href[i] = import_ref(s, lib, index);
		}
	}
	return s->bad ? -1 : 0;
}

/* ---- members ---- */
static void do_vars(struct sltg *s, ICreateTypeInfo2 *cti, size_t items,
		    WORD first, UINT count, const struct refs *refs)
{
	size_t q = items + first;

	for (UINT i = 0; i < count && !s->bad; i++) {
		BYTE magic = b8(s, q), flags = b8(s, q + 1);
		WORD next = w16(s, q + 2), name = w16(s, q + 4);
		WORD offs = w16(s, q + 6), type = w16(s, q + 8);
		VARDESC vd;
		VARIANT value;
		size_t to;

		if (magic != VAR_MAGIC && magic != VAR_WITH_FLAGS_MAGIC) {
			s->bad = 1;
			break;
		}
		memset(&vd, 0, sizeof(vd));
		vd.memid = (MEMBERID)w32(s, q + 10);
		to = flags & 0x02 ? q + 8 : items + type;
		do_type(s, &to, items, refs, &vd.elemdescVar.tdesc);
		if (flags & 0x40) {
			vd.varkind = VAR_DISPATCH;
		} else if (flags & 0x10) {
			vd.varkind = VAR_CONST;
			memset(&value, 0, sizeof(value));
			value.vt = VT_INT;
			if (flags & 0x08) {
				value.intVal = offs;
			} else {
				VARTYPE vt = vd.elemdescVar.tdesc.vt;
				if (vt == VT_BSTR || vt == VT_LPSTR ||
				    vt == VT_LPWSTR) {
					size_t so = items + offs;
					WCHAR *w = counted(s, &so);
					value.vt = VT_BSTR;
					static const WCHAR none[1];
					value.bstrVal = oa_SysAllocString(
						w ? w : none);
					free(w);
				} else {
					value.intVal = (INT)w32(s, items + offs);
				}
			}
			vd.lpvarValue = &value;
		} else {
			vd.varkind = VAR_PERINSTANCE;
			vd.oInst = offs;
		}
		if (magic == VAR_WITH_FLAGS_MAGIC)
			vd.wVarFlags = w16(s, q + 18);
		if (flags & 0x80)
			vd.wVarFlags |= VARFLAG_FREADONLY;
		if (!s->bad) {
			HRESULT hr = cti->lpVtbl->AddVarDesc(cti, i, &vd);
			if (hr != S_OK) {
				TRACE("SLTG: variable %u: %#x", i, (unsigned)hr);
				s->bad = 1;
			}
		}
		if (vd.varkind == VAR_CONST && value.vt == VT_BSTR)
			oa_SysFreeString(value.bstrVal);
		td_clear(s);
		WCHAR *n = name_at(s, name), *doc = name_at(s, w16(s, q + 16));
		if (!s->bad && n)
			cti->lpVtbl->SetVarName(cti, i, n);
		if (!s->bad && doc)
			cti->lpVtbl->SetVarDocString(cti, i, doc);
		free(n);
		free(doc);
		if (next == 0xffff)
			break;
		q = items + next;
	}
}

static void do_funcs(struct sltg *s, ICreateTypeInfo2 *cti, size_t items,
		     WORD first, UINT count, const struct refs *refs)
{
	size_t q = items + first;

	for (UINT i = 0; i < count && !s->bad; i++) {
		BYTE magic = b8(s, q), inv = b8(s, q + 1);
		WORD next = w16(s, q + 2), name = w16(s, q + 4);
		BYTE nacc = b8(s, q + 0x10), rno = b8(s, q + 0x11);
		FUNCDESC fd;
		ELEMDESC params[32];
		WCHAR *names[33] = { 0 };
		size_t to, pa;

		memset(&fd, 0, sizeof(fd));
		switch (magic & ~FUNC_FLAGS_PRESENT) {
		case FUNC_MAGIC: fd.funckind = FUNC_PUREVIRTUAL; break;
		case DISPATCH_FUNC_MAGIC: fd.funckind = FUNC_DISPATCH; break;
		case STATIC_FUNC_MAGIC: fd.funckind = FUNC_STATIC; break;
		default:
			s->bad = 1;
			return;
		}
		fd.memid = (MEMBERID)w32(s, q + 6);
		fd.invkind = inv >> 4;
		fd.callconv = nacc & 7;
		fd.cParams = nacc >> 3;
		fd.cParamsOpt = (rno & 0x7e) >> 1;
		if (fd.funckind != FUNC_DISPATCH)
			fd.oVft = (SHORT)(w16(s, q + 0x14) & ~1);
		if (magic & FUNC_FLAGS_PRESENT)
			fd.wFuncFlags = w16(s, q + 0x16);
		to = rno & 0x80 ? q + 0x12 : items + w16(s, q + 0x12);
		do_elem(s, &to, items, refs, &fd.elemdescFunc);
		fd.elemdescFunc.paramdesc.wParamFlags = 0;
		memset(params, 0, sizeof(params));
		fd.lprgelemdescParam = params;
		pa = items + w16(s, q + 0x0e);
		for (SHORT k = 0; k < fd.cParams && !s->bad; k++) {
			WORD n = w16(s, pa);
			int inline_type;

			pa += 2;
			/* Bit 0: the type follows inline, else the next
			 * WORD is its offset. The rest is the name's
			 * offset (0xffff and 0xfffe: no name). */
			inline_type = n & 1;
			if (n != 0xffff && n != 0xfffe)
				names[k + 1] = name_at(s, (WORD)(n & ~1));
			if (inline_type) {
				do_elem(s, &pa, items, refs, &params[k]);
			} else {
				to = items + w16(s, pa);
				pa += 2;
				do_elem(s, &to, items, refs, &params[k]);
			}
			if (fd.cParams - k <= fd.cParamsOpt)
				params[k].paramdesc.wParamFlags |=
					PARAMFLAG_FOPT;
		}
		if (fd.cParams > 32)
			s->bad = 1;
		if (!s->bad) {
			HRESULT hr = cti->lpVtbl->AddFuncDesc(cti, i, &fd);
			if (hr != S_OK) {
				TRACE("SLTG: function %u: %#x", i,
				      (unsigned)hr);
				s->bad = 1;
			}
		}
		td_clear(s);
		names[0] = name_at(s, name);
		if (!s->bad && names[0]) {
			UINT want = (UINT)fd.cParams + 1;
			if (fd.invkind & (INVOKE_PROPERTYPUT |
					  INVOKE_PROPERTYPUTREF))
				want--;
			if (cti->lpVtbl->SetFuncAndParamNames(cti, i, names,
							      want) != S_OK)
				TRACE("SLTG: function %u's names", i);
		}
		/* +0x0c, the help string, is in the member block, as
		 * compressed documentation, which isn't read. */
		for (int k = 0; k <= fd.cParams && k < 33; k++)
			free(names[k]);
		if (next == 0xffff)
			break;
		q = items + next;
	}
}

static void do_impls(struct sltg *s, ICreateTypeInfo2 *cti, size_t items,
		     WORD first, UINT count, const struct refs *refs)
{
	size_t q = items + first;

	for (UINT i = 0; i < count && !s->bad; i++) {
		WORD next = w16(s, q + 2), ref = w16(s, q + 10);
		BYTE flags = b8(s, q + 6);

		if (w16(s, q) != IMPL_MAGIC) {
			s->bad = 1;
			break;
		}
		if (ref < refs->n && refs->href[ref] != (HREFTYPE)-1) {
			if (cti->lpVtbl->AddImplType(cti, i, refs->href[ref]) !=
			    S_OK) {
				s->bad = 1;	/* not a partial library */
				break;
			}
			if (flags)
				cti->lpVtbl->SetImplTypeFlags(cti, i, flags);
		}
		if (next == 0xffff)
			break;
		q = items + next;
	}
}

static void do_block(struct sltg *s, ICreateTypeInfo2 *cti, size_t o);

/* The type in the block at o, len bytes: what it says is read from the
 * block only (its names from the name table). */
static void do_type_block(struct sltg *s, ICreateTypeInfo2 *cti, size_t o,
			  size_t len)
{
	s->lo = o;
	s->hi = o + len;
	do_block(s, cti, o);
	s->lo = s->hi = 0;
}

static void do_block(struct sltg *s, ICreateTypeInfo2 *cti, size_t o)
{
	struct refs refs = { 0 };
	DWORD href_table = w32(s, o + 2), elem = w32(s, o + 10);
	BYTE kind = b8(s, o + 0x1d);
	size_t mh = o + elem;
	size_t items = mh + 9;
	size_t tail = items + w32(s, mh + 5);

	if (s->bad || tail > s->len || s->len - tail < TAIL_SIZE) {
		s->bad = 1;
		return;
	}
	if (href_table != 0xffffffff && do_refs(s, o + href_table, &refs)) {
		s->bad = 1;
		free(refs.href);
		return;
	}
	WORD nfuncs = w16(s, tail), nvars = w16(s, tail + 2);
	WORD nimpls = w16(s, tail + 4), funcs_off = w16(s, tail + 8);
	WORD vars_off = w16(s, tail + 0x0a), impls_off = w16(s, tail + 0x0c);
	WORD alias = w16(s, tail + 0x14), simple = w16(s, tail + 0x1c);
	WORD size = w16(s, tail + 0x20), align = w16(s, tail + 0x22);
	WORD vft = w16(s, tail + 0x28);

	switch (kind) {
	case TKIND_ALIAS: {
		TYPEDESC td;
		memset(&td, 0, sizeof(td));
		if (simple) {
			td.vt = alias;
		} else {
			size_t to = items + alias;
			do_type(s, &to, items, &refs, &td);
		}
		if (!s->bad &&
		    cti->lpVtbl->SetTypeDescAlias(cti, &td) != S_OK)
			s->bad = 1;
		td_clear(s);
		break;
	}
	case TKIND_DISPATCH: {
		/* Its base, whatever the entry says, is IDispatch. */
		HREFTYPE h;
		if (oa_ctl_import(s->ctl, &stdole_libid, 2, 0, 0,
				  "stdole2.tlb", &stdole_types[1], 4,
				  &h) != S_OK ||
		    cti->lpVtbl->AddImplType(cti, 0, h) != S_OK)
			s->bad = 1;
		break;
	}
	case TKIND_INTERFACE:
	case TKIND_COCLASS:
		if (impls_off != 0xffff)
			do_impls(s, cti, items, impls_off, nimpls, &refs);
		break;
	default:
		break;
	}
	if (vars_off != 0xffff && nvars)
		do_vars(s, cti, items, vars_off, nvars, &refs);
	if (funcs_off != 0xffff && nfuncs)
		do_funcs(s, cti, items, funcs_off, nfuncs, &refs);
	oa_cti_fix_layout(cti, size, align, vft);
	free(refs.href);
}

/* ---- the library ---- */
struct oti {
	size_t index_name;	/* offset of its index string */
	WORD index_len, name;
	DWORD help_ctx;
	GUID guid;
};

static HRESULT convert(struct sltg *s)
{
	WORD nblocks = w16(s, 4), first = w16(s, 10);
	HRESULT hr = S_OK;

	if (nblocks < 2 || !first || first >= nblocks)
		return TYPE_E_INVDATAREAD;
	UINT nent = nblocks - 1;
	size_t magic_at = 0x24 + 8 * (size_t)nent;
	size_t at = magic_at + 13 + 11 * (size_t)(nblocks - 2) + 9;
	if (at > s->len || memcmp(s->d + magic_at, "\1CompObj", 8))
		return TYPE_E_INVDATAREAD;

	/* The blocks in their chain's order; the last is the library's. */
	size_t *blk = calloc(nent, sizeof(*blk));
	size_t *blen = calloc(nent, sizeof(*blen));
	UINT *ent = calloc(nent, sizeof(*ent));
	/* the block of each type, found by its index string */
	size_t *tblk = calloc(nent, sizeof(*tblk));
	size_t *tlen = calloc(nent, sizeof(*tlen));
	struct oti *oti = NULL;
	ICreateTypeInfo2 **ctis = NULL;
	UINT n = 0;
	if (!blk || !blen || !ent || !tblk || !tlen) {
		hr = E_OUTOFMEMORY;
		goto out;
	}
	for (UINT e = first - 1; n < nent;) {
		blk[n] = at;
		blen[n] = w32(s, 0x24 + 8 * (size_t)e);
		ent[n++] = e;
		WORD next = w16(s, 0x24 + 8 * (size_t)e + 6);
		at += blen[n - 1];
		if (!next)
			break;
		if (next > nent) {
			s->bad = 1;
			break;
		}
		e = next - 1;
	}
	if (s->bad || n != nent || at > s->len) {
		hr = TYPE_E_INVDATAREAD;
		goto out;
	}
	s->ntypes = n - 1;

	size_t lib = blk[n - 1], p = lib + 4;
	if (w16(s, lib) != LIBBLK_MAGIC) {
		hr = TYPE_E_INVDATAREAD;
		goto out;
	}
	WORD lib_name = w16(s, p);
	p += 2;
	WCHAR *unused = counted(s, &p);
	free(unused);
	WCHAR *doc = counted(s, &p), *help = counted(s, &p);
	DWORD help_ctx = w32(s, p);
	LCID lcid = w16(s, p + 6);
	WORD flags = w16(s, p + 12), major = w16(s, p + 14);
	WORD minor = w16(s, p + 16);
	GUID libid;
	get_guid(s, p + 18, &libid);
	p += 18 + 16 + 0x40 + 2;	/* and a WORD */

	if (!(oti = calloc(s->ntypes ? s->ntypes : 1, sizeof(*oti)))) {
		free(doc);
		free(help);
		hr = E_OUTOFMEMORY;
		goto out;
	}
	for (UINT i = 0; i < s->ntypes && !s->bad; i++) {
		oti[i].index_len = w16(s, p);
		oti[i].index_name = p + 2;
		p += 2 + (oti[i].index_len == 0xffff ? 0 : oti[i].index_len);
		WORD other = w16(s, p);
		p += 2 + (other == 0xffff ? 0 : other);
		oti[i].name = w16(s, p + 2);
		p += 6 + w16(s, p + 4);
		oti[i].help_ctx = w32(s, p + 2);
		get_guid(s, p + 8, &oti[i].guid);
		p += 26;
	}
	s->nt = lib + w32(s, p);
	if (w16(s, s->nt) == 0x0200)
		s->nt += 0x20;
	s->nt += 0x218;
	if (s->bad || s->nt >= s->len) {
		free(doc);
		free(help);
		hr = TYPE_E_INVDATAREAD;
		goto out;
	}

	if ((hr = oa_ctl_new(SYS_WIN32, &s->ctl)) != S_OK) {
		free(doc);
		free(help);
		goto out;
	}
	ICreateTypeLib2 *ctl = s->ctl;
	WCHAR *name = name_at(s, lib_name);
	if (name)
		ctl->lpVtbl->SetName(ctl, name);
	free(name);
	if (doc)
		ctl->lpVtbl->SetDocString(ctl, doc);
	if (help)
		ctl->lpVtbl->SetHelpFileName(ctl, help);
	free(doc);
	free(help);
	ctl->lpVtbl->SetHelpContext(ctl, help_ctx);
	ctl->lpVtbl->SetGuid(ctl, &libid);
	ctl->lpVtbl->SetVersion(ctl, major, minor);
	ctl->lpVtbl->SetLcid(ctl, lcid);
	ctl->lpVtbl->SetLibFlags(ctl, flags);

	/* The types first, so that references between them resolve. */
	if (!(ctis = calloc(s->ntypes ? s->ntypes : 1, sizeof(*ctis)))) {
		hr = E_OUTOFMEMORY;
		goto out;
	}
	for (UINT i = 0; i < s->ntypes && hr == S_OK; i++) {
		size_t o = blk[i], olen = blen[i];
		/* Its block is the one with its index string. */
		for (UINT b = 0; b + 1 < n; b++) {
			size_t is = magic_at + w16(s, 0x24 + 8 *
						       (size_t)ent[b] + 4);
			const char *a = cstr(s, is);
			if (a && oti[i].index_len != 0xffff &&
			    strlen(a) == oti[i].index_len &&
			    oti[i].index_name + oti[i].index_len <= s->len &&
			    !memcmp(a, s->d + oti[i].index_name,
				    oti[i].index_len)) {
				o = blk[b];
				olen = blen[b];
				break;
			}
		}
		tblk[i] = o;
		tlen[i] = olen;
		if (w16(s, o) != TIHEADER_MAGIC) {
			hr = TYPE_E_INVDATAREAD;
			break;
		}
		BYTE f1 = b8(s, o + 0x1a), f2 = b8(s, o + 0x1b);
		TYPEKIND kind = b8(s, o + 0x1d);
		WCHAR *tn = name_at(s, oti[i].name);
		ICreateTypeInfo *cti = NULL;
		if (!tn || kind >= TKIND_MAX) {
			free(tn);
			hr = TYPE_E_INVDATAREAD;
			break;
		}
		hr = ctl->lpVtbl->CreateTypeInfo(ctl, tn, kind, &cti);
		free(tn);
		if (hr != S_OK)
			break;
		ctis[i] = (ICreateTypeInfo2 *)cti;
		cti->lpVtbl->SetGuid(cti, &oti[i].guid);
		cti->lpVtbl->SetTypeFlags(cti, (UINT)(f1 >> 3 | f2 << 5));
		cti->lpVtbl->SetVersion(cti, w16(s, o + 0x12),
					w16(s, o + 0x14));
		cti->lpVtbl->SetHelpContext(cti, oti[i].help_ctx);
	}
	for (UINT i = 0; i < s->ntypes && hr == S_OK; i++) {
		do_type_block(s, ctis[i], tblk[i], tlen[i]);
		if (s->bad)
			hr = TYPE_E_INVDATAREAD;
	}
out:
	if (ctis)
		for (UINT i = 0; i < s->ntypes; i++)
			if (ctis[i])
				ctis[i]->lpVtbl->Release(ctis[i]);
	free(ctis);
	free(oti);
	free(blk);
	free(blen);
	free(tblk);
	free(tlen);
	free(ent);
	return s->bad && hr == S_OK ? TYPE_E_INVDATAREAD : hr;
}

HRESULT oa_sltg_to_msft(const BYTE *d, size_t len, BYTE **out,
			size_t *out_len)
{
	struct sltg s = { .d = d, .len = len };
	HRESULT hr;

	*out = NULL;
	if (len < 0x24 || w32(&s, 0) != SLTG_MAGIC)
		return TYPE_E_CANTLOADLIBRARY;
	hr = convert(&s);
	td_clear(&s);
	if (hr == S_OK)
		hr = oa_ctl_serialize(s.ctl, out, out_len);
	if (s.ctl)
		s.ctl->lpVtbl->Release(s.ctl);
	return hr;
}
