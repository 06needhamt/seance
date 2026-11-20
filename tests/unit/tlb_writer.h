/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * A writer of MSFT type libraries, and the library test_typelib checks
 * (described there in IDL), for Win32 (ptr 4) or Win64 (ptr 8) layouts.
 * Shared by test_typelib and gen_tlb, which writes the library that
 * pe_typelib.dll carries as its TYPELIB resource.
 */
#ifndef VFW_TLB_WRITER_H
#define VFW_TLB_WRITER_H

#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "win32.h"

static const GUID libid = { 0x5eace001, 0x1111, 0x2222,
			    { 1, 2, 3, 4, 5, 6, 7, 8 } };
static const GUID iid_thing = { 0x5eace002, 0x1111, 0x2222,
				{ 1, 2, 3, 4, 5, 6, 7, 8 } };
static const GUID iid_plain = { 0x5eace003, 0x1111, 0x2222,
				{ 1, 2, 3, 4, 5, 6, 7, 8 } };
static const GUID iid_events = { 0x5eace004, 0x1111, 0x2222,
				 { 1, 2, 3, 4, 5, 6, 7, 8 } };
static const GUID guid_colors = { 0x5eace005, 0x1111, 0x2222,
				  { 1, 2, 3, 4, 5, 6, 7, 8 } };
static const GUID guid_point = { 0x5eace006, 0x1111, 0x2222,
				 { 1, 2, 3, 4, 5, 6, 7, 8 } };
static const GUID clsid_thing = { 0x5eace007, 0x1111, 0x2222,
				  { 1, 2, 3, 4, 5, 6, 7, 8 } };
static const GUID guid_cust = { 0x5eace008, 0x1111, 0x2222,
				{ 1, 2, 3, 4, 5, 6, 7, 8 } };
static const GUID stdole2 = { 0x00020430, 0, 0,
			      { 0xc0, 0, 0, 0, 0, 0, 0, 0x46 } };
static const GUID iid_idispatch = { 0x00020400, 0, 0,
				    { 0xc0, 0, 0, 0, 0, 0, 0, 0x46 } };
static const GUID iid_iunknown = { 0, 0, 0,
				   { 0xc0, 0, 0, 0, 0, 0, 0, 0x46 } };

/* ----------------------------------------------------------------------
 * Writing the library.
 */

struct buf {
	unsigned char *d;
	size_t len, cap;
};

static size_t put(struct buf *b, const void *p, size_t n)
{
	size_t at = b->len;

	if (b->len + n > b->cap) {
		b->cap = (b->len + n) * 2 + 64;
		b->d = realloc(b->d, b->cap);
	}
	memcpy(b->d + b->len, p, n);
	b->len += n;
	return at;
}

static size_t put32(struct buf *b, int32_t v)
{
	return put(b, &v, 4);
}

static size_t put16(struct buf *b, int16_t v)
{
	return put(b, &v, 2);
}

static void pad4(struct buf *b)
{
	while (b->len % 4)
		put(b, "", 1);
}

enum {
	SEG_TYPEINFO, SEG_IMPINFO, SEG_IMPFILES, SEG_REFTAB, SEG_GUIDHASH,
	SEG_GUID, SEG_NAMEHASH, SEG_NAME, SEG_STRING, SEG_TYPEDESC,
	SEG_ARRAYDESC, SEG_CUSTDATA, SEG_CDGUIDS, SEG_COUNT = 15
};

struct writer {
	int ptr;			/* 4 or 8 */
	struct buf seg[SEG_COUNT];
	struct buf members[16];
	int32_t ti[16][25];		/* each type info's words */
	int ntypes;
};

static int32_t w_guid(struct writer *w, const GUID *g, int32_t href)
{
	int32_t off = put(&w->seg[SEG_GUID], g, 16);

	put32(&w->seg[SEG_GUID], href);
	put32(&w->seg[SEG_GUID], -1);
	return off;
}

static int32_t w_name(struct writer *w, const char *s)
{
	struct buf *b = &w->seg[SEG_NAME];
	int32_t off = put32(b, -1);

	put32(b, -1);
	put32(b, (int32_t)strlen(s) | 0x1234 << 16);
	put(b, s, strlen(s));
	pad4(b);
	return off;
}

static int32_t w_string(struct writer *w, const char *s)
{
	struct buf *b = &w->seg[SEG_STRING];
	int32_t off = put16(b, (int16_t)strlen(s));

	put(b, s, strlen(s));
	pad4(b);
	return off;
}

/* A simple type, encoded in the word itself. */
static int32_t simple(VARTYPE vt)
{
	return (int32_t)(0x80000000u | (uint32_t)vt << 16 | vt);
}

static int32_t w_tdesc(struct writer *w, VARTYPE vt, int32_t v)
{
	struct buf *b = &w->seg[SEG_TYPEDESC];
	int32_t off = put16(b, (int16_t)(vt | 0x7000));

	put16(b, -1);
	put32(b, v);
	return off;
}

/* A value in the custom data segment: a VARTYPE, then the data. */
static int32_t w_value(struct writer *w, VARTYPE vt, const void *p,
		       size_t n)
{
	struct buf *b = &w->seg[SEG_CUSTDATA];
	int32_t off = put16(b, (int16_t)vt);

	if (vt == VT_BSTR)
		put32(b, (int32_t)n);
	put(b, p, n);
	pad4(b);
	return off;
}

static int32_t w_cust(struct writer *w, const GUID *g, int32_t value,
		      int32_t next)
{
	struct buf *b = &w->seg[SEG_CDGUIDS];
	int32_t off = put32(b, w_guid(w, g, -1));

	put32(b, value);
	put32(b, next);
	return off;
}

#define INLINE_I4(v) ((int32_t)(0x80000000u | VT_I4 << 26 | (v)))

static int w_type(struct writer *w, TYPEKIND kind, const char *name,
		  const GUID *g, int flags, int size, int align)
{
	int i = w->ntypes++;
	int32_t *t = w->ti[i];

	memset(t, 0xff, sizeof(w->ti[i]));
	t[0] = kind | align << 11 | i << 16;
	t[2] = t[3] = t[4] = t[5] = 0;
	t[6] = 0;
	t[7] = t[8] = t[9] = t[10] = 0;
	t[11] = g ? w_guid(w, g, i * 0x64) : -1;
	t[12] = flags;
	t[13] = w_name(w, name);
	t[14] = 0;
	t[16] = t[17] = 0;
	t[19] = 0;
	t[20] = size;
	t[22] = 0;
	t[23] = t[24] = 0;
	return i;
}

/* The ints after a function record's fixed part. */
struct opt {
	int n;
	int32_t v[8];
};

struct param {
	int32_t type;
	const char *name;
	int flags;
	int32_t def;			/* with PARAMFLAG_FHASDEFAULT */
};

struct member {
	int32_t memid, name;
	struct buf rec;
};

static struct member mem[16][8];
static int nmem[16];

static void w_func(struct writer *w, int t, int32_t memid, const char *name,
		   int funckind, int invkind, int32_t ret, int vft_slot,
		   const struct opt *o, int nparams, const struct param *p)
{
	struct member *m = &mem[t][nmem[t]++];
	struct buf *r = &m->rec;
	int defaults = 0;

	for (int i = 0; i < nparams; i++)
		if (p[i].flags & PARAMFLAG_FHASDEFAULT)
			defaults = 1;
	m->memid = memid;
	m->name = w_name(w, name);
	put32(r, 0);				/* the length, below */
	put32(r, ret);
	put32(r, 0);				/* FUNCFLAGs */
	put16(r, (int16_t)(vft_slot * w->ptr));
	put16(r, 0);
	put32(r, funckind | invkind << 3 | CC_STDCALL << 8 |
		 (defaults ? 0x1000 : 0) |
		 (o && o->n > 2 && o->v[2] >= 0 && o->v[2] < 0x10000 &&
		  !strcmp(name, "Tick") ? 0x2000 : 0));
	put16(r, (int16_t)nparams);
	put16(r, 0);
	for (int i = 0; o && i < o->n; i++)
		put32(r, o->v[i]);
	if (defaults)
		for (int i = 0; i < nparams; i++)
			put32(r, p[i].flags & PARAMFLAG_FHASDEFAULT ? p[i].def
								     : -1);
	for (int i = 0; i < nparams; i++) {
		put32(r, p[i].type);
		put32(r, p[i].name ? w_name(w, p[i].name) : -1);
		put32(r, p[i].flags);
	}
	*(int32_t *)r->d = (int32_t)r->len;
	w->ti[t][6] += 1;
}

static void w_var(struct writer *w, int t, int32_t memid, const char *name,
		  int32_t type, int kind, int32_t value)
{
	struct member *m = &mem[t][nmem[t]++];
	struct buf *r = &m->rec;

	m->memid = memid;
	m->name = w_name(w, name);
	put32(r, 20);
	put32(r, type);
	put32(r, 0);
	put16(r, (int16_t)kind);
	put16(r, 0);
	put32(r, value);
	w->ti[t][6] += 1 << 16;
}

/* The header, directories and members after the segments: the file. */
static struct buf finish(struct writer *w, const GUID *lib_guid,
			 const char *doc, const char *name,
			 int32_t dispatch_href)
{
	struct buf out = { 0 };

	/* The header. */
	int32_t h[21];
	const char *hello = "hello";

	h[0] = 0x5446534d;
	h[1] = 0x00010002;
	h[2] = w_guid(w, lib_guid, -2);
	h[3] = h[4] = 0x409;
	h[5] = w->ptr == 8 ? SYS_WIN64 : SYS_WIN32;
	h[6] = 1 | 2 << 16;
	h[7] = 0;
	h[8] = w->ntypes;
	h[9] = w_string(w, doc);
	h[10] = 0;
	h[11] = 7;
	h[12] = h[13] = 0;
	h[14] = w_name(w, name);
	h[15] = w_string(w, "test.hlp");
	h[16] = w_cust(w, &guid_cust,
		       w_value(w, VT_BSTR, hello, strlen(hello)), -1);
	h[17] = h[18] = 0;
	h[19] = dispatch_href;
	h[20] = 3;
	put(&out, h, sizeof(h));
	for (int i = 0; i < w->ntypes; i++)
		put32(&out, i * 0x64);

	/* The segment directory, then the segments, then the members. */
	size_t dir = out.len;
	for (int i = 0; i < w->ntypes; i++)
		put(&w->seg[SEG_TYPEINFO], w->ti[i], 0x64);
	for (int i = 0; i < SEG_COUNT * 4; i++)
		put32(&out, 0);
	for (int i = 0; i < SEG_COUNT; i++) {
		int32_t *e = (int32_t *)(out.d + dir) + 4 * i;

		e[0] = w->seg[i].len ? (int32_t)out.len : -1;
		e[1] = (int32_t)w->seg[i].len;
		e[2] = -1;
		e[3] = 0xf;
		if (w->seg[i].len)
			put(&out, w->seg[i].d, w->seg[i].len);
	}
	size_t ti_seg = ((int32_t *)(out.d + dir))[0];
	for (int i = 0; i < w->ntypes; i++) {
		int n = nmem[i];
		int32_t len = 0;

		if (!n)
			continue;
		((int32_t *)(out.d + ti_seg + i * 0x64))[1] = (int32_t)out.len;
		for (int j = 0; j < n; j++)
			len += (int32_t)mem[i][j].rec.len;
		put32(&out, len);
		for (int j = 0; j < n; j++)
			put(&out, mem[i][j].rec.d, mem[i][j].rec.len);
		for (int j = 0; j < n; j++)
			put32(&out, mem[i][j].memid);
		for (int j = 0; j < n; j++)
			put32(&out, mem[i][j].name);
		for (int j = 0, off = 0; j < n; j++) {
			put32(&out, off);
			off += (int)mem[i][j].rec.len;
		}
	}
	for (int i = 0; i < SEG_COUNT; i++)
		free(w->seg[i].d);
	for (int i = 0; i < 16; i++)
		for (int j = 0; j < 8; j++)
			free(mem[i][j].rec.d);
	return out;
}

/* The library, laid out for pointers of ptr bytes. */
static struct buf build_library(int ptr)
{
	struct writer w = { .ptr = ptr };
	const char *stdole = "stdole2.tlb";
	int32_t td_i4ptr, td_bstrptr, td_guid, td_guidptr, td_point, td_ppoint,
		td_tag, ad;

	memset(mem, 0, sizeof(mem));
	memset(nmem, 0, sizeof(nmem));

	/* Imports: IDispatch and IUnknown by GUID, GUID by index. */
	int32_t impfile = put32(&w.seg[SEG_IMPFILES],
				w_guid(&w, &stdole2, -1));
	put32(&w.seg[SEG_IMPFILES], 0);
	put32(&w.seg[SEG_IMPFILES], 2);
	put16(&w.seg[SEG_IMPFILES], (int16_t)(strlen(stdole) << 2));
	put(&w.seg[SEG_IMPFILES], stdole, strlen(stdole));
	pad4(&w.seg[SEG_IMPFILES]);
	const int32_t href_idispatch = 1, href_iunknown = 13, href_guid = 25;
	put32(&w.seg[SEG_IMPINFO], 0x10000 | TKIND_DISPATCH << 24);
	put32(&w.seg[SEG_IMPINFO], impfile);
	put32(&w.seg[SEG_IMPINFO], w_guid(&w, &iid_idispatch, href_idispatch));
	put32(&w.seg[SEG_IMPINFO], 0x10000 | TKIND_INTERFACE << 24);
	put32(&w.seg[SEG_IMPINFO], impfile);
	put32(&w.seg[SEG_IMPINFO], w_guid(&w, &iid_iunknown, href_iunknown));
	put32(&w.seg[SEG_IMPINFO], TKIND_RECORD << 24);
	put32(&w.seg[SEG_IMPINFO], impfile);
	put32(&w.seg[SEG_IMPINFO], 0);

	/* Type descriptions. */
	td_i4ptr = w_tdesc(&w, VT_PTR, simple(VT_I4));
	td_bstrptr = w_tdesc(&w, VT_PTR, simple(VT_BSTR));
	td_guid = w_tdesc(&w, VT_USERDEFINED, href_guid);
	td_guidptr = w_tdesc(&w, VT_PTR, td_guid);
	td_point = w_tdesc(&w, VT_USERDEFINED, 4 * 0x64);
	td_ppoint = w_tdesc(&w, VT_PTR, td_point);
	ad = put32(&w.seg[SEG_ARRAYDESC], simple(VT_UI1));
	put16(&w.seg[SEG_ARRAYDESC], 1);
	put16(&w.seg[SEG_ARRAYDESC], 0);
	put32(&w.seg[SEG_ARRAYDESC], 4);
	put32(&w.seg[SEG_ARRAYDESC], 0);
	td_tag = w_tdesc(&w, VT_CARRAY, ad);

	/* 0: IThing, dual. */
	int t = w_type(&w, TKIND_DISPATCH, "IThing", &iid_thing,
		       TYPEFLAG_FDUAL | TYPEFLAG_FDISPATCHABLE |
			       TYPEFLAG_FOLEAUTOMATION, ptr, ptr);
	int32_t v42 = 42;
	w.ti[t][18] = w_cust(&w, &guid_cust,
			     w_value(&w, VT_I4, &v42, 4), -1);
	w.ti[t][19] = 1 | (11 * ptr) << 16;
	w.ti[t][21] = href_idispatch;
	w.ti[t][14] = 1;
	{
		struct opt o = { 2, { 0, w_string(&w, "Adds two numbers") } };
		struct param p[] = {
			{ simple(VT_I4), "a", PARAMFLAG_FIN, 0 },
			{ simple(VT_I4), "b",
			  PARAMFLAG_FIN | PARAMFLAG_FOPT |
				  PARAMFLAG_FHASDEFAULT, INLINE_I4(2) },
			{ td_i4ptr, "r", PARAMFLAG_FOUT | PARAMFLAG_FRETVAL, 0 },
		};
		struct param get[] = {
			{ td_bstrptr, "v", PARAMFLAG_FOUT | PARAMFLAG_FRETVAL, 0 },
		};
		struct param set[] = { { simple(VT_BSTR), "v", PARAMFLAG_FIN, 0 } };
		struct param chk[] = { { td_guidptr, "g", PARAMFLAG_FIN, 0 } };

		w_func(&w, t, 1, "Add", FUNC_PUREVIRTUAL, INVOKE_FUNC,
		       simple(VT_HRESULT), 7, &o, 3, p);
		w_func(&w, t, 2, "Name", FUNC_PUREVIRTUAL, INVOKE_PROPERTYGET,
		       simple(VT_HRESULT), 8, NULL, 1, get);
		w_func(&w, t, 2, "Name", FUNC_PUREVIRTUAL, INVOKE_PROPERTYPUT,
		       simple(VT_HRESULT), 9, NULL, 1, set);
		w_func(&w, t, 3, "Check", FUNC_PUREVIRTUAL, INVOKE_FUNC,
		       simple(VT_HRESULT), 10, NULL, 1, chk);
	}

	/* 1: IPlain. */
	t = w_type(&w, TKIND_INTERFACE, "IPlain", &iid_plain, 0, ptr, ptr);
	w.ti[t][19] = 1 | (4 * ptr) << 16;
	w.ti[t][21] = href_iunknown;
	{
		struct param p[] = { { td_ppoint, "p", PARAMFLAG_FIN, 0 } };

		w_func(&w, t, 0x60010000, "Ping", FUNC_PUREVIRTUAL,
		       INVOKE_FUNC, simple(VT_HRESULT), 3, NULL, 1, p);
	}

	/* 2: IEvents. */
	t = w_type(&w, TKIND_DISPATCH, "IEvents", &iid_events, 0, ptr, ptr);
	w.ti[t][19] = 1 | (7 * ptr) << 16;
	w.ti[t][21] = href_idispatch;
	{
		struct param p[] = { { simple(VT_I4), "what", PARAMFLAG_FIN, 0 } };

		w_func(&w, t, 10, "Changed", FUNC_DISPATCH, INVOKE_FUNC,
		       simple(VT_VOID), 0, NULL, 1, p);
	}

	/* 3: Colors. */
	t = w_type(&w, TKIND_ENUM, "Colors", &guid_colors, 0, 4, 4);
	w_var(&w, t, 0x40000000, "Red", simple(VT_I4), VAR_CONST,
	      INLINE_I4(1));
	w_var(&w, t, 0x40000001, "Green", simple(VT_I4), VAR_CONST,
	      INLINE_I4(2));

	/* 4: Point. */
	t = w_type(&w, TKIND_RECORD, "Point", &guid_point, 0, 12, 4);
	w_var(&w, t, 0x40000000, "x", simple(VT_I4), VAR_PERINSTANCE, 0);
	w_var(&w, t, 0x40000001, "y", simple(VT_I4), VAR_PERINSTANCE, 4);
	w_var(&w, t, 0x40000002, "tag", td_tag, VAR_PERINSTANCE, 8);

	/* 5: PPoint. */
	t = w_type(&w, TKIND_ALIAS, "PPoint", NULL, 0, ptr, ptr);
	w.ti[t][21] = td_ppoint;

	/* 6: Kernel. */
	t = w_type(&w, TKIND_MODULE, "Kernel", NULL, 0, 4, 4);
	w.ti[t][21] = w_string(&w, "kernel32.dll");
	{
		struct opt beep = { 3, { 0, -1, w_string(&w, "Beep") } };
		struct opt tick = { 3, { 0, -1, 42 } };

		w_func(&w, t, 0x60000000, "Beep", FUNC_STATIC, INVOKE_FUNC,
		       simple(VT_HRESULT), 0, &beep, 0, NULL);
		w_func(&w, t, 0x60000001, "Tick", FUNC_STATIC, INVOKE_FUNC,
		       simple(VT_HRESULT), 0, &tick, 0, NULL);
	}

	/* 7: Thing. */
	t = w_type(&w, TKIND_COCLASS, "Thing", &clsid_thing,
		   TYPEFLAG_FCANCREATE, ptr, ptr);
	w.ti[t][19] = 2;
	w.ti[t][21] = put32(&w.seg[SEG_REFTAB], 0);
	put32(&w.seg[SEG_REFTAB], IMPLTYPEFLAG_FDEFAULT);
	put32(&w.seg[SEG_REFTAB], -1);
	put32(&w.seg[SEG_REFTAB], 16);
	put32(&w.seg[SEG_REFTAB], 2 * 0x64);
	put32(&w.seg[SEG_REFTAB], IMPLTYPEFLAG_FDEFAULT |
				   IMPLTYPEFLAG_FSOURCE);
	put32(&w.seg[SEG_REFTAB], -1);
	put32(&w.seg[SEG_REFTAB], -1);

	return finish(&w, &libid, "Test library", "TestLib", href_idispatch);
}

#endif
