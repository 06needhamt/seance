/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * oleaut32's type libraries: LoadTypeLib and the ITypeLib and ITypeInfo it
 * gives, over a library this test writes (there is no midl here), for
 * Win32 and for Win64; the built-in stdole2; and the reader against
 * truncated and mutated copies of the library, which must fail cleanly or
 * load into something every call can walk.
 *
 * The library, in IDL:
 *
 *   [uuid(LIBID), version(1.2), helpstring("Test library"),
 *    helpfile("test.hlp"), helpcontext(7), custom(CUST, "hello")]
 *   library TestLib {
 *     importlib("stdole2.tlb");
 *     [uuid(..), dual, oleautomation, custom(CUST, 42)]
 *     interface IThing : IDispatch {
 *       [id(1), helpstring("Adds two numbers")] HRESULT Add([in] long a,
 *           [in, optional, defaultvalue(2)] long b, [out, retval] long *r);
 *       [id(2), propget] HRESULT Name([out, retval] BSTR *v);
 *       [id(2), propput] HRESULT Name([in] BSTR v);
 *       [id(3)] HRESULT Check([in] GUID *g);
 *     };
 *     [uuid(..)] interface IPlain : IUnknown { HRESULT Ping([in] Point *p); };
 *     [uuid(..)] dispinterface IEvents { methods: [id(10)] void Changed(
 *         [in] long what); };
 *     [uuid(..)] enum Colors { Red = 1, Green = 2 };
 *     [uuid(..)] struct Point { long x; long y; unsigned char tag[4]; };
 *     typedef Point *PPoint;
 *     [dllname("kernel32.dll")] module Kernel {
 *       [entry("Beep")] HRESULT Beep(); [entry(42)] HRESULT Tick(); };
 *     [uuid(..)] coclass Thing { [default] interface IThing;
 *       [default, source] dispinterface IEvents; };
 *   };
 */
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#include "loader/modules.h"
#include "runtime/runtime.h"
#include "test.h"
#include "win32/codepage.h"
#include "win32/path.h"

static struct module *oa;

static void *get(const char *name)
{
	void *p = mod_get_proc(oa, name);

	if (!p) {
		fprintf(stderr, "oleaut32 has no %s\n", name);
		exit(1);
	}
	return p;
}

static HRESULT (WINAPI *pLoadTypeLib)(LPCOLESTR, ITypeLib **);
static HRESULT (WINAPI *pLoadTypeLibEx)(LPCOLESTR, REGKIND, ITypeLib **);
static void (WINAPI *pSysFreeString)(BSTR);
static HRESULT (WINAPI *pVariantClear)(VARIANT *);
static void (WINAPI *pClearCustData)(CUSTDATA *);

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
static const GUID guid_none;
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

/* The library, laid out for pointers of ptr bytes. */
static struct buf build_library(int ptr)
{
	struct writer w = { .ptr = ptr };
	struct buf out = { 0 };
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

	/* The header. */
	int32_t h[21];
	const char *hello = "hello";

	h[0] = 0x5446534d;
	h[1] = 0x00010002;
	h[2] = w_guid(&w, &libid, -2);
	h[3] = h[4] = 0x409;
	h[5] = ptr == 8 ? SYS_WIN64 : SYS_WIN32;
	h[6] = 1 | 2 << 16;
	h[7] = 0;
	h[8] = w.ntypes;
	h[9] = w_string(&w, "Test library");
	h[10] = 0;
	h[11] = 7;
	h[12] = h[13] = 0;
	h[14] = w_name(&w, "TestLib");
	h[15] = w_string(&w, "test.hlp");
	h[16] = w_cust(&w, &guid_cust,
		       w_value(&w, VT_BSTR, hello, strlen(hello)), -1);
	h[17] = h[18] = 0;
	h[19] = href_idispatch;
	h[20] = 3;
	put(&out, h, sizeof(h));
	for (int i = 0; i < w.ntypes; i++)
		put32(&out, i * 0x64);

	/* The segment directory, then the segments, then the members. */
	size_t dir = out.len;
	for (int i = 0; i < w.ntypes; i++)
		put(&w.seg[SEG_TYPEINFO], w.ti[i], 0x64);
	for (int i = 0; i < SEG_COUNT * 4; i++)
		put32(&out, 0);
	for (int i = 0; i < SEG_COUNT; i++) {
		int32_t *e = (int32_t *)(out.d + dir) + 4 * i;

		e[0] = w.seg[i].len ? (int32_t)out.len : -1;
		e[1] = (int32_t)w.seg[i].len;
		e[2] = -1;
		e[3] = 0xf;
		if (w.seg[i].len)
			put(&out, w.seg[i].d, w.seg[i].len);
	}
	size_t ti_seg = ((int32_t *)(out.d + dir))[0];
	for (int i = 0; i < w.ntypes; i++) {
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
		free(w.seg[i].d);
	for (int i = 0; i < 16; i++)
		for (int j = 0; j < 8; j++)
			free(mem[i][j].rec.d);
	return out;
}

/* ----------------------------------------------------------------------
 * Checking it.
 */

static char dir[] = "/tmp/test_typelib.XXXXXX";

/* Writes the bytes to a file and returns its Windows path, as UTF-16. */
static WCHAR *write_lib(const char *name, const void *d, size_t len)
{
	char path[256];
	FILE *f;

	snprintf(path, sizeof(path), "%s/%s", dir, name);
	f = fopen(path, "wb");
	if (!f)
		return NULL;
	fwrite(d, 1, len, f);
	fclose(f);
	char *win = path_to_windows(path);
	WCHAR *w = win ? utf8_to_utf16_dup(win) : NULL;
	free(win);
	return w;
}

static char *utf8(BSTR b)
{
	static char buf[4][256];
	static int next;
	char *s = buf[next++ % 4];
	size_t i = 0;

	if (!b)
		return NULL;
	for (; b[i] && i < 255; i++)
		s[i] = (char)b[i];
	s[i] = 0;
	pSysFreeString(b);
	return s;
}

static WCHAR *wide(const char *s)
{
	static WCHAR buf[4][64];
	static int next;
	WCHAR *w = buf[next++ % 4];
	size_t i = 0;

	for (; s[i] && i < 63; i++)
		w[i] = (unsigned char)s[i];
	w[i] = 0;
	return w;
}

/* The name of a type info (from GetDocumentation). */
static const char *type_name(ITypeInfo *ti)
{
	BSTR name = NULL;

	if (!ti || FAILED(ti->lpVtbl->GetDocumentation(ti, MEMBERID_NIL, &name,
						       NULL, NULL, NULL)))
		return "(none)";
	return utf8(name);
}

/* The name of the type an href names, from ti. */
static const char *ref_name(ITypeInfo *ti, HREFTYPE href)
{
	ITypeInfo *r = NULL;
	const char *n;

	if (FAILED(ti->lpVtbl->GetRefTypeInfo(ti, href, &r)))
		return "(unresolved)";
	n = type_name(r);
	r->lpVtbl->Release(r);
	return n;
}

static ITypeInfo *type_at(ITypeLib *lib, UINT i)
{
	ITypeInfo *ti = NULL;

	CHECK_EQ(lib->lpVtbl->GetTypeInfo(lib, i, &ti), S_OK);
	return ti;
}

static void check_library(ITypeLib *lib, int ptr)
{
	TLIBATTR *la;
	BSTR name, doc, file;
	DWORD ctx;
	VARIANT v;

	CHECK_EQ(lib->lpVtbl->GetTypeInfoCount(lib), 8);
	CHECK_EQ(lib->lpVtbl->GetLibAttr(lib, &la), S_OK);
	CHECK(!memcmp(&la->guid, &libid, sizeof(GUID)));
	CHECK_EQ(la->lcid, 0x409);
	CHECK_EQ(la->syskind, ptr == 8 ? SYS_WIN64 : SYS_WIN32);
	CHECK_EQ(la->wMajorVerNum, 1);
	CHECK_EQ(la->wMinorVerNum, 2);
	lib->lpVtbl->ReleaseTLibAttr(lib, la);
	CHECK_EQ(lib->lpVtbl->GetDocumentation(lib, -1, &name, &doc, &ctx,
					       &file), S_OK);
	CHECK_STR(utf8(name), "TestLib");
	CHECK_STR(utf8(doc), "Test library");
	CHECK_STR(utf8(file), "test.hlp");
	CHECK_EQ(ctx, 7);
	CHECK_EQ(lib->lpVtbl->GetDocumentation(lib, 4, &name, NULL, NULL,
					       NULL), S_OK);
	CHECK_STR(utf8(name), "Point");
	CHECK_EQ(lib->lpVtbl->GetDocumentation(lib, 8, &name, NULL, NULL,
					       NULL), TYPE_E_ELEMENTNOTFOUND);

	/* Custom data. */
	CHECK_EQ(lib->lpVtbl->GetCustData(lib, &guid_cust, &v), S_OK);
	CHECK_EQ(v.vt, VT_BSTR);
	CHECK(v.vt == VT_BSTR && !memcmp(v.bstrVal, wide("hello"), 12));
	pVariantClear(&v);
	CUSTDATA cd;
	CHECK_EQ(lib->lpVtbl->GetAllCustData(lib, &cd), S_OK);
	CHECK_EQ(cd.cCustData, 1);
	CHECK(cd.cCustData == 1 &&
	      !memcmp(&cd.prgCustData[0].guid, &guid_cust, sizeof(GUID)));
	pClearCustData(&cd);
	CHECK_EQ(cd.cCustData, 0);
	CHECK(!cd.prgCustData);

	/* Kinds, and by GUID. */
	static const TYPEKIND kinds[] = {
		TKIND_DISPATCH, TKIND_INTERFACE, TKIND_DISPATCH, TKIND_ENUM,
		TKIND_RECORD, TKIND_ALIAS, TKIND_MODULE, TKIND_COCLASS,
	};
	for (UINT i = 0; i < 8; i++) {
		TYPEKIND k;

		CHECK_EQ(lib->lpVtbl->GetTypeInfoType(lib, i, &k), S_OK);
		CHECK_EQ(k, kinds[i]);
	}
	ITypeInfo *ti = NULL;
	CHECK_EQ(lib->lpVtbl->GetTypeInfoOfGuid(lib, &clsid_thing, &ti),
		 S_OK);
	CHECK_STR(type_name(ti), "Thing");
	UINT index = 99;
	ITypeLib *back = NULL;
	CHECK_EQ(ti->lpVtbl->GetContainingTypeLib(ti, &back, &index), S_OK);
	CHECK(back == lib);
	CHECK_EQ(index, 7);
	back->lpVtbl->Release(back);
	ti->lpVtbl->Release(ti);
	CHECK_EQ(lib->lpVtbl->GetTypeInfoOfGuid(lib, &guid_none, &ti),
		 TYPE_E_ELEMENTNOTFOUND);

	/* Names. */
	WCHAR *n = wide("pOiNt");
	BOOL found = FALSE;
	CHECK_EQ(lib->lpVtbl->IsName(lib, n, 0, &found), S_OK);
	CHECK(found);
	CHECK(!memcmp(n, wide("Point"), 12));
	CHECK_EQ(lib->lpVtbl->IsName(lib, wide("Nothing"), 0, &found), S_OK);
	CHECK(!found);
	ITypeInfo *infos[4];
	MEMBERID ids[4];
	USHORT count = 4;
	CHECK_EQ(lib->lpVtbl->FindName(lib, wide("add"), 0, infos, ids,
				       &count), S_OK);
	CHECK_EQ(count, 1);
	if (count == 1) {
		CHECK_STR(type_name(infos[0]), "IThing");
		CHECK_EQ(ids[0], 1);
		infos[0]->lpVtbl->Release(infos[0]);
	}
}

static void check_dual(ITypeLib *lib, int ptr)
{
	ITypeInfo *ti = type_at(lib, 0), *iface = NULL;
	TYPEATTR *a;
	FUNCDESC *f;
	HREFTYPE h;
	VARIANT v;

	/* The dispatch view: IDispatch's functions, then its own. */
	CHECK_EQ(ti->lpVtbl->GetTypeAttr(ti, &a), S_OK);
	CHECK_EQ(a->typekind, TKIND_DISPATCH);
	CHECK_EQ(a->cFuncs, 11);
	CHECK_EQ(a->cbSizeVft, 7 * sizeof(void *));
	CHECK_EQ(a->cImplTypes, 1);
	CHECK_EQ(a->wTypeFlags, TYPEFLAG_FDUAL | TYPEFLAG_FDISPATCHABLE);
	CHECK(!memcmp(&a->guid, &iid_thing, sizeof(GUID)));
	ti->lpVtbl->ReleaseTypeAttr(ti, a);
	CHECK_EQ(ti->lpVtbl->GetRefTypeOfImplType(ti, 0, &h), S_OK);
	CHECK_STR(ref_name(ti, h), "IDispatch");

	CHECK_EQ(ti->lpVtbl->GetFuncDesc(ti, 0, &f), S_OK);
	CHECK_EQ(f->memid, 0x60000000);
	CHECK_EQ(f->funckind, FUNC_DISPATCH);
	CHECK_EQ(f->cParams, 2);
	/* stdole2's GUID, though this is another library's view. */
	CHECK_EQ(f->lprgelemdescParam[0].tdesc.vt, VT_PTR);
	CHECK_STR(ref_name(ti, f->lprgelemdescParam[0].tdesc.lptdesc->hreftype),
		  "GUID");
	ti->lpVtbl->ReleaseFuncDesc(ti, f);

	CHECK_EQ(ti->lpVtbl->GetFuncDesc(ti, 7, &f), S_OK);
	CHECK_EQ(f->memid, 1);
	CHECK_EQ(f->funckind, FUNC_DISPATCH);
	CHECK_EQ(f->invkind, INVOKE_FUNC);
	CHECK_EQ(f->callconv, CC_STDCALL);
	CHECK_EQ(f->cParams, 2);	/* the retval is the result */
	CHECK_EQ(f->elemdescFunc.tdesc.vt, VT_I4);
	USHORT pf = f->lprgelemdescParam[1].paramdesc.wParamFlags;
	CHECK_EQ(pf, PARAMFLAG_FIN | PARAMFLAG_FOPT | PARAMFLAG_FHASDEFAULT);
	PARAMDESCEX *x = f->lprgelemdescParam[1].paramdesc.pparamdescex;
	CHECK(x && x->varDefaultValue.vt == VT_I4 &&
	      x->varDefaultValue.lVal == 2);
	ti->lpVtbl->ReleaseFuncDesc(ti, f);
	CHECK_EQ(ti->lpVtbl->GetFuncDesc(ti, 9, &f), S_OK);
	CHECK_EQ(f->invkind, INVOKE_PROPERTYPUT);
	CHECK_EQ(f->elemdescFunc.tdesc.vt, VT_VOID);
	CHECK_EQ(f->cParams, 1);
	ti->lpVtbl->ReleaseFuncDesc(ti, f);
	CHECK_EQ(ti->lpVtbl->GetFuncDesc(ti, 11, &f), TYPE_E_ELEMENTNOTFOUND);

	UINT index;
	CHECK_EQ(ti->lpVtbl->GetFuncIndexOfMemId(ti, 2, INVOKE_PROPERTYPUT,
						 &index), S_OK);
	CHECK_EQ(index, 9);
	CHECK_EQ(ti->lpVtbl->GetFuncIndexOfMemId(ti, 0x60010003, INVOKE_FUNC,
						 &index), S_OK);
	CHECK_EQ(index, 6);

	/* Names and IDs. */
	BSTR names[4];
	UINT n = 0;
	CHECK_EQ(ti->lpVtbl->GetNames(ti, 1, names, 4, &n), S_OK);
	CHECK_EQ(n, 4);
	if (n == 4) {
		CHECK_STR(utf8(names[0]), "Add");
		CHECK_STR(utf8(names[1]), "a");
		CHECK_STR(utf8(names[2]), "b");
		CHECK_STR(utf8(names[3]), "r");
	}
	CHECK_EQ(ti->lpVtbl->GetNames(ti, 0x60010002, names, 1, &n), S_OK);
	CHECK_EQ(n, 1);
	if (n)
		CHECK_STR(utf8(names[0]), "GetIDsOfNames");
	LPOLESTR want[] = { wide("ADD"), wide("B"), wide("zz") };
	MEMBERID ids[3];
	CHECK_EQ(ti->lpVtbl->GetIDsOfNames(ti, want, 2, ids), S_OK);
	CHECK_EQ(ids[0], 1);
	CHECK_EQ(ids[1], 1);
	CHECK_EQ(ti->lpVtbl->GetIDsOfNames(ti, want, 3, ids),
		 DISP_E_UNKNOWNNAME);
	CHECK_EQ(ids[2], MEMBERID_NIL);
	want[0] = wide("Missing");
	CHECK_EQ(ti->lpVtbl->GetIDsOfNames(ti, want, 1, ids),
		 DISP_E_UNKNOWNNAME);

	BSTR doc;
	CHECK_EQ(ti->lpVtbl->GetDocumentation(ti, 1, NULL, &doc, NULL, NULL),
		 S_OK);
	CHECK_STR(utf8(doc), "Adds two numbers");
	CHECK_EQ(ti->lpVtbl->GetDocumentation(ti, 77, NULL, &doc, NULL, NULL),
		 TYPE_E_ELEMENTNOTFOUND);
	CHECK_EQ(ti->lpVtbl->GetCustData(ti, &guid_cust, &v), S_OK);
	CHECK(v.vt == VT_I4 && v.lVal == 42);

	/* The interface view: its own functions, as the vtable has them. */
	CHECK_EQ(ti->lpVtbl->GetRefTypeOfImplType(ti, (UINT)-1, &h), S_OK);
	CHECK_EQ(ti->lpVtbl->GetRefTypeInfo(ti, h, &iface), S_OK);
	if (!iface) {
		ti->lpVtbl->Release(ti);
		return;
	}
	CHECK_STR(type_name(iface), "IThing");
	TYPEKIND k;
	CHECK_EQ(iface->lpVtbl->GetTypeKind(iface, &k), S_OK);
	CHECK_EQ(k, TKIND_INTERFACE);
	CHECK_EQ(iface->lpVtbl->GetTypeAttr(iface, &a), S_OK);
	CHECK_EQ(a->typekind, TKIND_INTERFACE);
	CHECK_EQ(a->cFuncs, 4);
	CHECK_EQ(a->cbSizeVft, 11 * sizeof(void *));
	iface->lpVtbl->ReleaseTypeAttr(iface, a);
	CHECK_EQ(iface->lpVtbl->GetRefTypeOfImplType(iface, 0, &h), S_OK);
	CHECK_STR(ref_name(iface, h), "IDispatch");
	CHECK_EQ(iface->lpVtbl->GetFuncDesc(iface, 0, &f), S_OK);
	CHECK_EQ(f->funckind, FUNC_PUREVIRTUAL);
	CHECK_EQ(f->oVft, 7 * sizeof(void *));
	CHECK_EQ(f->cParams, 3);
	CHECK_EQ(f->elemdescFunc.tdesc.vt, VT_HRESULT);
	CHECK_EQ(f->lprgelemdescParam[2].paramdesc.wParamFlags,
		 PARAMFLAG_FOUT | PARAMFLAG_FRETVAL);
	iface->lpVtbl->ReleaseFuncDesc(iface, f);
	CHECK_EQ(iface->lpVtbl->GetFuncDesc(iface, 3, &f), S_OK);
	CHECK_EQ(f->memid, 3);
	CHECK_EQ(f->oVft, 10 * sizeof(void *));
	CHECK_EQ(f->lprgelemdescParam[0].tdesc.vt, VT_PTR);
	CHECK_STR(ref_name(iface,
			   f->lprgelemdescParam[0].tdesc.lptdesc->hreftype),
		  "GUID");
	iface->lpVtbl->ReleaseFuncDesc(iface, f);
	/* And back. */
	ITypeInfo *disp = NULL;
	CHECK_EQ(iface->lpVtbl->GetRefTypeOfImplType(iface, (UINT)-1, &h),
		 S_OK);
	CHECK_EQ(iface->lpVtbl->GetRefTypeInfo(iface, h, &disp), S_OK);
	CHECK(disp == ti);
	if (disp)
		disp->lpVtbl->Release(disp);
	iface->lpVtbl->Release(iface);
	ti->lpVtbl->Release(ti);
	(void)ptr;
}

static void check_others(ITypeLib *lib)
{
	ITypeInfo *ti;
	TYPEATTR *a;
	FUNCDESC *f;
	VARDESC *v;
	HREFTYPE h;
	INT flags;

	/* IPlain: IUnknown's, by GUID, and a Point* parameter. */
	ti = type_at(lib, 1);
	CHECK_EQ(ti->lpVtbl->GetTypeAttr(ti, &a), S_OK);
	CHECK_EQ(a->typekind, TKIND_INTERFACE);
	CHECK_EQ(a->cFuncs, 1);
	CHECK_EQ(a->cbSizeVft, 4 * sizeof(void *));
	ti->lpVtbl->ReleaseTypeAttr(ti, a);
	CHECK_EQ(ti->lpVtbl->GetRefTypeOfImplType(ti, 0, &h), S_OK);
	CHECK_STR(ref_name(ti, h), "IUnknown");
	CHECK_EQ(ti->lpVtbl->GetRefTypeOfImplType(ti, (UINT)-1, &h),
		 TYPE_E_ELEMENTNOTFOUND);
	CHECK_EQ(ti->lpVtbl->GetFuncDesc(ti, 0, &f), S_OK);
	CHECK_EQ(f->oVft, 3 * sizeof(void *));
	CHECK_STR(ref_name(ti, f->lprgelemdescParam[0].tdesc.lptdesc->hreftype),
		  "Point");
	ti->lpVtbl->ReleaseFuncDesc(ti, f);
	ti->lpVtbl->Release(ti);

	/* IEvents: a plain dispinterface shows only its own functions. */
	ti = type_at(lib, 2);
	CHECK_EQ(ti->lpVtbl->GetTypeAttr(ti, &a), S_OK);
	CHECK_EQ(a->cFuncs, 1);
	CHECK_EQ(a->cbSizeVft, 7 * sizeof(void *));
	ti->lpVtbl->ReleaseTypeAttr(ti, a);
	CHECK_EQ(ti->lpVtbl->GetFuncDesc(ti, 0, &f), S_OK);
	CHECK_EQ(f->memid, 10);
	CHECK_EQ(f->funckind, FUNC_DISPATCH);
	CHECK_EQ(f->elemdescFunc.tdesc.vt, VT_VOID);
	ti->lpVtbl->ReleaseFuncDesc(ti, f);
	CHECK_EQ(ti->lpVtbl->GetRefTypeOfImplType(ti, 0, &h), S_OK);
	CHECK_STR(ref_name(ti, h), "IDispatch");
	ti->lpVtbl->Release(ti);

	/* Colors. */
	ti = type_at(lib, 3);
	CHECK_EQ(ti->lpVtbl->GetVarDesc(ti, 1, &v), S_OK);
	CHECK_EQ(v->varkind, VAR_CONST);
	CHECK_EQ(v->memid, 0x40000001);
	CHECK(v->lpvarValue && v->lpvarValue->vt == VT_I4 &&
	      v->lpvarValue->lVal == 2);
	ti->lpVtbl->ReleaseVarDesc(ti, v);
	BSTR name;
	UINT n;
	CHECK_EQ(ti->lpVtbl->GetNames(ti, 0x40000001, &name, 1, &n), S_OK);
	CHECK_STR(utf8(name), "Green");
	UINT index;
	CHECK_EQ(ti->lpVtbl->GetVarIndexOfMemId(ti, 0x40000001, &index), S_OK);
	CHECK_EQ(index, 1);
	CHECK_EQ(ti->lpVtbl->GetVarDesc(ti, 2, &v), TYPE_E_ELEMENTNOTFOUND);
	ti->lpVtbl->Release(ti);

	/* Point. */
	ti = type_at(lib, 4);
	CHECK_EQ(ti->lpVtbl->GetTypeAttr(ti, &a), S_OK);
	CHECK_EQ(a->cbSizeInstance, 12);
	CHECK_EQ(a->cVars, 3);
	CHECK_EQ(a->cbAlignment, 4);
	ti->lpVtbl->ReleaseTypeAttr(ti, a);
	CHECK_EQ(ti->lpVtbl->GetVarDesc(ti, 2, &v), S_OK);
	CHECK_EQ(v->varkind, VAR_PERINSTANCE);
	CHECK_EQ(v->oInst, 8);
	CHECK_EQ(v->elemdescVar.tdesc.vt, VT_CARRAY);
	ARRAYDESC *ad = v->elemdescVar.tdesc.lpadesc;
	CHECK(ad && ad->cDims == 1 && ad->rgbounds[0].cElements == 4 &&
	      ad->tdescElem.vt == VT_UI1);
	ti->lpVtbl->ReleaseVarDesc(ti, v);
	ti->lpVtbl->Release(ti);

	/* PPoint. */
	ti = type_at(lib, 5);
	CHECK_EQ(ti->lpVtbl->GetTypeAttr(ti, &a), S_OK);
	CHECK_EQ(a->tdescAlias.vt, VT_PTR);
	CHECK_STR(ref_name(ti, a->tdescAlias.lptdesc->hreftype), "Point");
	ti->lpVtbl->ReleaseTypeAttr(ti, a);
	ti->lpVtbl->Release(ti);

	/* Kernel. */
	ti = type_at(lib, 6);
	BSTR dll = NULL, entry = NULL;
	WORD ord = 99;
	CHECK_EQ(ti->lpVtbl->GetDllEntry(ti, 0x60000000, INVOKE_FUNC, &dll,
					 &entry, &ord), S_OK);
	CHECK_STR(utf8(dll), "kernel32.dll");
	CHECK_STR(utf8(entry), "Beep");
	CHECK_EQ(ord, 0);
	CHECK_EQ(ti->lpVtbl->GetDllEntry(ti, 0x60000001, INVOKE_FUNC, &dll,
					 &entry, &ord), S_OK);
	pSysFreeString(dll);
	CHECK(!entry);
	CHECK_EQ(ord, 42);
	CHECK_EQ(ti->lpVtbl->GetDllEntry(ti, 0x60000002, INVOKE_FUNC, &dll,
					 &entry, &ord), TYPE_E_ELEMENTNOTFOUND);
	ti->lpVtbl->Release(ti);

	/* Thing. */
	ti = type_at(lib, 7);
	CHECK_EQ(ti->lpVtbl->GetTypeAttr(ti, &a), S_OK);
	CHECK_EQ(a->cImplTypes, 2);
	CHECK_EQ(a->wTypeFlags, TYPEFLAG_FCANCREATE);
	ti->lpVtbl->ReleaseTypeAttr(ti, a);
	CHECK_EQ(ti->lpVtbl->GetRefTypeOfImplType(ti, 0, &h), S_OK);
	CHECK_STR(ref_name(ti, h), "IThing");
	CHECK_EQ(ti->lpVtbl->GetImplTypeFlags(ti, 0, &flags), S_OK);
	CHECK_EQ(flags, IMPLTYPEFLAG_FDEFAULT);
	CHECK_EQ(ti->lpVtbl->GetRefTypeOfImplType(ti, 1, &h), S_OK);
	CHECK_STR(ref_name(ti, h), "IEvents");
	CHECK_EQ(ti->lpVtbl->GetImplTypeFlags(ti, 1, &flags), S_OK);
	CHECK_EQ(flags, IMPLTYPEFLAG_FDEFAULT | IMPLTYPEFLAG_FSOURCE);
	CHECK_EQ(ti->lpVtbl->GetImplTypeFlags(ti, 2, &flags),
		 TYPE_E_ELEMENTNOTFOUND);
	ti->lpVtbl->Release(ti);
}

static void test_library(int ptr)
{
	struct buf b = build_library(ptr);
	WCHAR *path = write_lib(ptr == 8 ? "test64.tlb" : "test32.tlb", b.d,
				b.len);
	ITypeLib *lib = NULL, *again = NULL;

	free(b.d);
	CHECK(path);
	if (!path)
		return;
	CHECK_EQ(pLoadTypeLib(path, &lib), S_OK);
	if (!lib) {
		free(path);
		return;
	}
	check_library(lib, ptr);
	check_dual(lib, ptr);
	check_others(lib);

	/* Loaded again: the same library. */
	CHECK_EQ(pLoadTypeLibEx(path, REGKIND_NONE, &again), S_OK);
	CHECK(again == lib);
	if (again)
		again->lpVtbl->Release(again);
	CHECK_EQ(lib->lpVtbl->Release(lib), 0);
	free(path);
}

static void test_stdole2(void)
{
	ITypeLib *lib = NULL;
	ITypeInfo *ti = NULL;
	TYPEATTR *a;
	FUNCDESC *f;
	TLIBATTR *la;

	CHECK_EQ(pLoadTypeLib(wide("stdole2.tlb"), &lib), S_OK);
	if (!lib)
		return;
	CHECK_EQ(lib->lpVtbl->GetLibAttr(lib, &la), S_OK);
	CHECK(!memcmp(&la->guid, &stdole2, sizeof(GUID)));
	CHECK_EQ(la->wMajorVerNum, 2);
	lib->lpVtbl->ReleaseTLibAttr(lib, la);
	CHECK_EQ(lib->lpVtbl->GetTypeInfoOfGuid(lib, &iid_idispatch, &ti),
		 S_OK);
	if (ti) {
		HREFTYPE h;

		CHECK_EQ(ti->lpVtbl->GetTypeAttr(ti, &a), S_OK);
		CHECK_EQ(a->typekind, TKIND_INTERFACE);
		CHECK_EQ(a->cFuncs, 4);
		CHECK_EQ(a->cbSizeVft, 7 * sizeof(void *));
		ti->lpVtbl->ReleaseTypeAttr(ti, a);
		CHECK_EQ(ti->lpVtbl->GetFuncDesc(ti, 3, &f), S_OK);
		CHECK_EQ(f->memid, 0x60010003);
		CHECK_EQ(f->cParams, 8);
		CHECK_EQ(f->oVft, 6 * sizeof(void *));
		ti->lpVtbl->ReleaseFuncDesc(ti, f);
		CHECK_EQ(ti->lpVtbl->GetRefTypeOfImplType(ti, 0, &h), S_OK);
		CHECK_STR(ref_name(ti, h), "IUnknown");
		ti->lpVtbl->Release(ti);
	}
	USHORT count = 1;
	ITypeInfo *infos[1];
	MEMBERID ids[1];
	CHECK_EQ(lib->lpVtbl->FindName(lib, wide("IPictureDisp"), 0, infos,
				       ids, &count), S_OK);
	CHECK_EQ(count, 1);
	if (count == 1) {
		CHECK_EQ(infos[0]->lpVtbl->GetTypeAttr(infos[0], &a), S_OK);
		CHECK_EQ(a->typekind, TKIND_ALIAS);
		CHECK_STR(ref_name(infos[0], a->tdescAlias.hreftype),
			  "Picture");
		infos[0]->lpVtbl->ReleaseTypeAttr(infos[0], a);
		infos[0]->lpVtbl->Release(infos[0]);
	}
	lib->lpVtbl->Release(lib);
}

static void test_errors(void)
{
	ITypeLib *lib = (ITypeLib *)1;
	static const unsigned char sltg[] = "SLTG\x01\x00\x00\x00";
	WCHAR *p;

	CHECK_EQ(pLoadTypeLib(wide("C:\\nowhere\\missing.tlb"), &lib),
		 TYPE_E_CANTLOADLIBRARY);
	CHECK(!lib);
	CHECK_EQ(pLoadTypeLib(NULL, &lib), E_INVALIDARG);
	p = write_lib("sltg.tlb", sltg, sizeof(sltg));
	CHECK_EQ(pLoadTypeLib(p, &lib), TYPE_E_UNSUPFORMAT);
	free(p);
	p = write_lib("junk.tlb", "junk", 4);
	CHECK_EQ(pLoadTypeLib(p, &lib), TYPE_E_CANTLOADLIBRARY);
	free(p);
}

/* ----------------------------------------------------------------------
 * Bad libraries.
 */

/* Everything a caller can read from a library, references followed. */
static void walk_tdesc(ITypeInfo *ti, const TYPEDESC *td, int depth)
{
	ITypeInfo *r;

	if (depth > 8)
		return;
	if (td->vt == VT_PTR || td->vt == VT_SAFEARRAY)
		walk_tdesc(ti, td->lptdesc, depth + 1);
	else if (td->vt == VT_CARRAY)
		walk_tdesc(ti, &td->lpadesc->tdescElem, depth + 1);
	else if (td->vt == VT_USERDEFINED &&
		 SUCCEEDED(ti->lpVtbl->GetRefTypeInfo(ti, td->hreftype, &r))) {
		TYPEATTR *a;

		if (SUCCEEDED(r->lpVtbl->GetTypeAttr(r, &a))) {
			if (a->typekind == TKIND_ALIAS)
				walk_tdesc(r, &a->tdescAlias, depth + 1);
			r->lpVtbl->ReleaseTypeAttr(r, a);
		}
		r->lpVtbl->Release(r);
	}
}

static void walk_type(ITypeInfo *ti, int depth)
{
	TYPEATTR *a;
	BSTR names[8], s;
	UINT n;

	if (depth > 2 || FAILED(ti->lpVtbl->GetTypeAttr(ti, &a)))
		return;
	if (SUCCEEDED(ti->lpVtbl->GetDocumentation(ti, MEMBERID_NIL, &s, NULL,
						   NULL, NULL)))
		pSysFreeString(s);
	if (a->typekind == TKIND_ALIAS)
		walk_tdesc(ti, &a->tdescAlias, 0);
	for (UINT i = 0; i < a->cFuncs && i < 300; i++) {
		FUNCDESC *f;

		if (FAILED(ti->lpVtbl->GetFuncDesc(ti, i, &f)))
			continue;
		walk_tdesc(ti, &f->elemdescFunc.tdesc, 0);
		for (SHORT j = 0; j < f->cParams; j++)
			walk_tdesc(ti, &f->lprgelemdescParam[j].tdesc, 0);
		if (SUCCEEDED(ti->lpVtbl->GetNames(ti, f->memid, names, 8,
						   &n)))
			for (UINT j = 0; j < n; j++)
				pSysFreeString(names[j]);
		ti->lpVtbl->ReleaseFuncDesc(ti, f);
	}
	for (UINT i = 0; i < a->cVars && i < 300; i++) {
		VARDESC *v;

		if (FAILED(ti->lpVtbl->GetVarDesc(ti, i, &v)))
			continue;
		walk_tdesc(ti, &v->elemdescVar.tdesc, 0);
		ti->lpVtbl->ReleaseVarDesc(ti, v);
	}
	for (int i = -1; i < (int)a->cImplTypes && i < 8; i++) {
		HREFTYPE h;
		ITypeInfo *r;

		if (SUCCEEDED(ti->lpVtbl->GetRefTypeOfImplType(ti, (UINT)i,
							       &h)) &&
		    SUCCEEDED(ti->lpVtbl->GetRefTypeInfo(ti, h, &r))) {
			walk_type(r, depth + 1);
			r->lpVtbl->Release(r);
		}
	}
	LPOLESTR name = wide("Add");
	MEMBERID id;
	ti->lpVtbl->GetIDsOfNames(ti, &name, 1, &id);
	ti->lpVtbl->ReleaseTypeAttr(ti, a);
}

static void walk_library(const void *d, size_t len)
{
	WCHAR *p = write_lib("bad.tlb", d, len);
	ITypeLib *lib = NULL;

	if (p && SUCCEEDED(pLoadTypeLib(p, &lib))) {
		UINT n = lib->lpVtbl->GetTypeInfoCount(lib);

		for (UINT i = 0; i < n && i < 64; i++) {
			ITypeInfo *ti;

			if (SUCCEEDED(lib->lpVtbl->GetTypeInfo(lib, i, &ti))) {
				walk_type(ti, 0);
				ti->lpVtbl->Release(ti);
			}
		}
		CHECK_EQ(lib->lpVtbl->Release(lib), 0);
	}
	free(p);
}

static void test_bad_libraries(void)
{
	struct buf b = build_library(sizeof(void *));
	unsigned char *copy = malloc(b.len);
	uint32_t seed = 12345;

	/* Every truncation. */
	for (size_t len = 0; len < b.len; len += 1 + len / 64)
		walk_library(b.d, len);

	/* Bytes changed at random, a few at a time. */
	for (int round = 0; round < 1500; round++) {
		memcpy(copy, b.d, b.len);
		for (int k = 0; k < 1 + round % 4; k++) {
			seed = seed * 1103515245 + 12345;
			size_t at = (seed >> 8) % b.len;

			seed = seed * 1103515245 + 12345;
			switch (seed >> 29) {
			case 0:
				copy[at] = 0xff;
				break;
			case 1:
				copy[at] = 0;
				break;
			case 2:
				copy[at] ^= 0x80;
				break;
			default:
				copy[at] = (unsigned char)(seed >> 16);
			}
		}
		walk_library(copy, b.len);
	}
	CHECK(1);
	free(copy);
	free(b.d);
}

int main(void)
{
	char cleanup[64];

	CHECK_EQ(runtime_enter(), 0);
	oa = mod_load("oleaut32.dll", NULL);
	CHECK(oa != NULL);
	if (!oa || !mkdtemp(dir))
		return test_summary("test_typelib");
	pLoadTypeLib = get("LoadTypeLib");
	pLoadTypeLibEx = get("LoadTypeLibEx");
	pSysFreeString = get("SysFreeString");
	pVariantClear = get("VariantClear");
	pClearCustData = get("ClearCustData");

	test_library(4);
	test_library(8);
	test_stdole2();
	test_errors();
	test_bad_libraries();

	snprintf(cleanup, sizeof(cleanup), "rm -rf -- '%s'", dir);
	if (system(cleanup))
		fprintf(stderr, "couldn't remove %s\n", dir);
	mod_free(oa);
	return test_summary("test_typelib");
}
