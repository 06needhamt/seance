/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * comctl32 through its export table: dynamic arrays, the Alloc and Str_
 * helpers, the controls' state, window subclassing, image lists, the
 * headless property sheets and task dialogs, the rest, and its ordinals.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "loader/modules.h"
#include "runtime/runtime.h"
#include "test.h"
#include "win32/codepage.h"
#include "win32/commctrl.h"
#include "win32/objbase.h"
#include "win32/winreg.h"

static struct module *cc, *u32, *gdi;

static void *get_from(struct module *m, const char *name)
{
	void *p = mod_get_proc(m, name);
	if (!p) {
		fprintf(stderr, "%s has no %s\n", m->name, name);
		exit(1);
	}
	return p;
}

#define FN(ret, name, ...) \
	ret (WINAPI *name)(__VA_ARGS__) = \
		(ret (WINAPI *)(__VA_ARGS__))get_from(cc, #name)
#define USER(ret, name, ...) \
	ret (WINAPI *name)(__VA_ARGS__) = \
		(ret (WINAPI *)(__VA_ARGS__))get_from(u32, #name)
#define GDI(ret, name, ...) \
	ret (WINAPI *name)(__VA_ARGS__) = \
		(ret (WINAPI *)(__VA_ARGS__))get_from(gdi, #name)
/* Get/SetWindowLongPtrW, whatever their export names. */
#define USER_WLP(get_or_set, ...) \
	LONG_PTR (WINAPI *get_or_set##WindowLongPtrW)(__VA_ARGS__) = \
		(LONG_PTR (WINAPI *)(__VA_ARGS__))get_from(u32, \
			WLP_NAME(get_or_set, W))

static WCHAR *w(const char *s)
{
	static WCHAR buf[8][128];
	static int next;
	WCHAR *o = buf[next++ % 8];
	size_t i = 0;

	for (; s[i] && i < 127; i++)
		o[i] = (BYTE)s[i];
	o[i] = 0;
	return o;
}

static const char *a(const WCHAR *s)
{
	static char buf[4][128];
	static int next;
	char *o = buf[next++ % 4];
	size_t i = 0;

	for (; s && s[i] && i < 127; i++)
		o[i] = s[i] < 128 ? (char)s[i] : '?';
	o[i] = '\0';
	return o;
}

/* Dynamic arrays. */

static int WINAPI cmp_int(void *x, void *y, LPARAM lp)
{
	/* Compares the high digit only, so equal keys show stability. */
	int dx = (int)(INT_PTR)x / 10, dy = (int)(INT_PTR)y / 10;
	return (dx > dy) - (dx < dy) + (int)lp;
}

static int enum_seen[8], enum_n;

static int WINAPI count_until(void *p, void *data)
{
	if (enum_n < 8)
		enum_seen[enum_n++] = (int)(INT_PTR)p;
	return (int)(INT_PTR)p != (int)(INT_PTR)data;
}

static void test_dpa(void)
{
	FN(HDPA, DPA_Create, INT);
	FN(HDPA, DPA_CreateEx, INT, HANDLE);
	FN(BOOL, DPA_Destroy, HDPA);
	FN(INT, DPA_InsertPtr, HDPA, INT, LPVOID);
	FN(BOOL, DPA_SetPtr, HDPA, INT, LPVOID);
	FN(LPVOID, DPA_GetPtr, HDPA, INT_PTR);
	FN(INT, DPA_GetPtrIndex, HDPA, LPCVOID);
	FN(LPVOID, DPA_DeletePtr, HDPA, INT);
	FN(BOOL, DPA_DeleteAllPtrs, HDPA);
	FN(BOOL, DPA_Sort, HDPA, PFNDACOMPARE, LPARAM);
	FN(INT, DPA_Search, HDPA, LPVOID, INT, PFNDACOMPARE, LPARAM, UINT);
	FN(HDPA, DPA_Clone, const HDPA, HDPA);
	FN(void, DPA_EnumCallback, HDPA, PFNDAENUMCALLBACK, LPVOID);
	FN(void, DPA_DestroyCallback, HDPA, PFNDAENUMCALLBACK, LPVOID);
	FN(BOOL, DPA_Grow, HDPA, INT);

	HDPA d = DPA_Create(2);
	CHECK(d != NULL);
	if (!d)
		return;
	/* commctrl.h's macros read the count and the array directly. */
	CHECK_EQ(*(int *)d, 0);
	CHECK_EQ(DPA_InsertPtr(d, 0, (void *)30), 0);
	CHECK_EQ(DPA_InsertPtr(d, 99, (void *)10), 1);	/* past the end: appended */
	CHECK_EQ(DPA_InsertPtr(d, 1, (void *)20), 1);
	CHECK_EQ(DPA_InsertPtr(d, -1, (void *)5), -1);
	CHECK_EQ(*(int *)d, 3);
	void **ptrs = *(void ***)((BYTE *)d + sizeof(void *));
	CHECK(ptrs[0] == (void *)30 && ptrs[1] == (void *)20 &&
	      ptrs[2] == (void *)10);
	CHECK(DPA_GetPtr(d, 1) == (void *)20);
	CHECK(DPA_GetPtr(d, 3) == NULL && DPA_GetPtr(d, -1) == NULL);
	CHECK_EQ(DPA_GetPtrIndex(d, (void *)10), 2);
	CHECK_EQ(DPA_GetPtrIndex(d, (void *)11), -1);

	/* Setting past the end fills with NULLs. */
	CHECK(DPA_SetPtr(d, 5, (void *)60));
	CHECK_EQ(*(int *)d, 6);
	CHECK(DPA_GetPtr(d, 4) == NULL && DPA_GetPtr(d, 5) == (void *)60);
	CHECK(!DPA_SetPtr(d, -1, NULL));
	CHECK(DPA_DeletePtr(d, 4) == NULL && *(int *)d == 5);
	CHECK(DPA_DeletePtr(d, 3) == NULL && *(int *)d == 4);
	CHECK(DPA_DeletePtr(d, 9) == NULL && *(int *)d == 4);

	/* A stable sort: 31 and 35 keep their order. */
	CHECK(DPA_SetPtr(d, 3, (void *)35));
	CHECK(DPA_InsertPtr(d, 0, (void *)31) == 0);
	CHECK(DPA_Sort(d, cmp_int, 0));
	ptrs = *(void ***)((BYTE *)d + sizeof(void *));
	CHECK(ptrs[0] == (void *)10 && ptrs[1] == (void *)20 &&
	      ptrs[2] == (void *)31 && ptrs[3] == (void *)30 &&
	      ptrs[4] == (void *)35);

	/* Sorted searches find a match, or where one would go. */
	CHECK_EQ(DPA_Search(d, (void *)20, 0, cmp_int, 0, DPAS_SORTED), 1);
	CHECK_EQ(DPA_Search(d, (void *)55, 0, cmp_int, 0, DPAS_SORTED), -1);
	CHECK_EQ(DPA_Search(d, (void *)55, 0, cmp_int, 0,
			    DPAS_SORTED | DPAS_INSERTAFTER), 5);
	CHECK_EQ(DPA_Search(d, (void *)1, 0, cmp_int, 0,
			    DPAS_SORTED | DPAS_INSERTBEFORE), 0);
	CHECK_EQ(DPA_Search(d, (void *)33, 3, cmp_int, 0, 0), 3);
	CHECK_EQ(DPA_Search(d, (void *)10, 1, cmp_int, 0, 0), -1);

	/* Clones, into a new array or over an old one. */
	HDPA c = DPA_Clone(d, NULL);
	CHECK(c && *(int *)c == 5 && DPA_GetPtr(c, 4) == (void *)35);
	HDPA e = DPA_CreateEx(0, NULL);
	CHECK(DPA_InsertPtr(e, 0, (void *)1) == 0);
	CHECK(DPA_Clone(d, e) == e && *(int *)e == 5 &&
	      DPA_GetPtr(e, 0) == (void *)10);
	CHECK(DPA_Grow(e, 100));
	for (int i = 0; i < 40; i++)
		DPA_InsertPtr(e, DA_LAST, (void *)(INT_PTR)i);
	CHECK_EQ(*(int *)e, 45);
	CHECK(DPA_DeleteAllPtrs(e) && *(int *)e == 0);
	CHECK(DPA_Destroy(e));

	/* Enumeration stops when the callback says 0. */
	enum_n = 0;
	DPA_EnumCallback(c, count_until, (void *)31);
	CHECK_EQ(enum_n, 3);
	CHECK(enum_seen[2] == 31);
	enum_n = 0;
	DPA_DestroyCallback(c, count_until, (void *)-1);
	CHECK_EQ(enum_n, 5);
	CHECK(DPA_Destroy(d));
	CHECK(!DPA_Destroy(NULL));
	CHECK(DPA_GetPtr(NULL, 0) == NULL);
}

/* DPA_Merge and the stream form. */

static int merged, inserted, deleted;

static int WINAPI cmp_exact(void *x, void *y, LPARAM lp)
{
	INT_PTR a = (INT_PTR)x, b = (INT_PTR)y;
	return (a > b) - (a < b);
}

static PVOID WINAPI merge_cb(UINT msg, PVOID d, PVOID s, LPARAM lp)
{
	switch (msg) {
	case DPAMM_MERGE:
		merged++;
		return (PVOID)((INT_PTR)d + (INT_PTR)lp);	/* marks it */
	case DPAMM_INSERT:
		inserted++;
		return d;
	case DPAMM_DELETE:
		deleted++;
		return NULL;
	}
	return NULL;
}

static HRESULT WINAPI save_item(DPASTREAMINFO *info, IStream *s, void *data)
{
	INT_PTR v = (INT_PTR)info->pvItem;
	DWORD x = (DWORD)v;
	return s->lpVtbl->Write(s, &x, sizeof(x), NULL);
}

static HRESULT WINAPI load_item(DPASTREAMINFO *info, IStream *s, void *data)
{
	DWORD x = 0;
	ULONG got = 0;
	HRESULT hr = s->lpVtbl->Read(s, &x, sizeof(x), &got);
	if (FAILED(hr) || got != sizeof(x))
		return E_FAIL;
	if (data && x == *(DWORD *)data)
		return S_FALSE;		/* stop here */
	info->pvItem = (void *)(INT_PTR)x;
	return S_OK;
}

static void test_dpa_merge_stream(void)
{
	FN(HDPA, DPA_Create, INT);
	FN(BOOL, DPA_Destroy, HDPA);
	FN(INT, DPA_InsertPtr, HDPA, INT, LPVOID);
	FN(LPVOID, DPA_GetPtr, HDPA, INT_PTR);
	FN(BOOL, DPA_Merge, HDPA, HDPA, DWORD, PFNDACOMPARE, PFNDPAMERGE,
	   LPARAM);
	FN(HRESULT, DPA_SaveStream, HDPA, PFNDPASTREAM, IStream *, void *);
	FN(HRESULT, DPA_LoadStream, HDPA *, PFNDPASTREAM, IStream *, void *);
	CHECK(DPA_Merge == mod_get_ordinal(cc, 11));
	CHECK(DPA_SaveStream == mod_get_ordinal(cc, 10));
	CHECK(DPA_LoadStream == mod_get_ordinal(cc, 9));

	/* Union: in both merged, only in src inserted; unsorted input is
	 * sorted first. */
	HDPA d = DPA_Create(4), s = DPA_Create(4);
	static const int dv[] = { 30, 10, 50 }, sv[] = { 40, 10, 50, 60 };
	for (int i = 0; i < 3; i++)
		DPA_InsertPtr(d, DA_LAST, (void *)(INT_PTR)dv[i]);
	for (int i = 0; i < 4; i++)
		DPA_InsertPtr(s, DA_LAST, (void *)(INT_PTR)sv[i]);
	merged = inserted = deleted = 0;
	CHECK(DPA_Merge(d, s, DPAM_UNION, cmp_exact, merge_cb, 1));
	CHECK(merged == 2 && inserted == 2 && deleted == 0);
	static const int want_u[] = { 11, 30, 40, 51, 60 };
	CHECK_EQ(*(int *)d, 5);
	for (int i = 0; i < 5; i++)
		CHECK(DPA_GetPtr(d, i) == (void *)(INT_PTR)want_u[i]);
	DPA_Destroy(d);

	/* Intersect, both sorted already: only what is in both stays. */
	d = DPA_Create(4);
	static const int dv2[] = { 10, 40, 60, 70 };
	for (int i = 0; i < 4; i++)
		DPA_InsertPtr(d, DA_LAST, (void *)(INT_PTR)dv2[i]);
	DPA_Destroy(s);
	s = DPA_Create(4);
	for (int i = 0; i < 4; i++)
		DPA_InsertPtr(s, DA_LAST, (void *)(INT_PTR)(10 + i * 20));
	merged = inserted = deleted = 0;	/* s: 10 30 50 70 */
	CHECK(DPA_Merge(d, s, DPAM_SORTED | DPAM_INTERSECT, cmp_exact,
			merge_cb, 0));
	CHECK(merged == 2 && inserted == 0 && deleted == 2);
	CHECK(*(int *)d == 2 && DPA_GetPtr(d, 0) == (void *)10 &&
	      DPA_GetPtr(d, 1) == (void *)70);
	CHECK(!DPA_Merge(NULL, s, 0, cmp_exact, merge_cb, 0));

	/* Saved and loaded back, through a stream on global memory. */
	struct module *ole = mod_load("ole32.dll", NULL);
	CHECK(ole != NULL);
	if (ole) {
		HRESULT (WINAPI *CreateStreamOnHGlobal)(HGLOBAL, BOOL,
							IStream **) =
			(HRESULT (WINAPI *)(HGLOBAL, BOOL, IStream **))
			get_from(ole, "CreateStreamOnHGlobal");
		IStream *st = NULL;
		CHECK_EQ(CreateStreamOnHGlobal(NULL, TRUE, &st), S_OK);
		if (st) {
			DWORD junk = 0xabcd;
			st->lpVtbl->Write(st, &junk, 4, NULL); /* not at 0 */
			CHECK_EQ(DPA_SaveStream(s, save_item, st, NULL), S_OK);
			ULARGE_INTEGER at;
			LARGE_INTEGER z = { .QuadPart = 0 };
			st->lpVtbl->Seek(st, z, STREAM_SEEK_CUR, &at);
			CHECK_EQ(at.QuadPart, 4 + 12 + 16);
			LARGE_INTEGER four = { .QuadPart = 4 };
			st->lpVtbl->Seek(st, four, STREAM_SEEK_SET, NULL);
			DWORD head[3];
			st->lpVtbl->Read(st, head, sizeof(head), NULL);
			CHECK(head[0] == 28 && head[1] == 1 && head[2] == 4);
			st->lpVtbl->Seek(st, four, STREAM_SEEK_SET, NULL);
			HDPA l = NULL;
			CHECK_EQ(DPA_LoadStream(&l, load_item, st, NULL), S_OK);
			CHECK(l && *(int *)l == 4 &&
			      DPA_GetPtr(l, 3) == (void *)70);
			DPA_Destroy(l);
			/* A callback that stops: what loaded is kept. */
			st->lpVtbl->Seek(st, four, STREAM_SEEK_SET, NULL);
			DWORD stop = 50;
			l = NULL;
			CHECK_EQ(DPA_LoadStream(&l, load_item, st, &stop),
				 S_FALSE);
			CHECK(l && *(int *)l == 2);
			DPA_Destroy(l);
			/* Not a saved array. */
			st->lpVtbl->Seek(st, z, STREAM_SEEK_SET, NULL);
			l = (HDPA)1;
			CHECK_EQ(DPA_LoadStream(&l, load_item, st, NULL),
				 E_FAIL);
			CHECK(l == NULL);
			st->lpVtbl->Release(st);
		}
		mod_free(ole);
	}
	DPA_Destroy(d);
	DPA_Destroy(s);
}

struct item {
	int a, b;
};

static void test_dsa(void)
{
	FN(HDSA, DSA_Create, INT, INT);
	FN(BOOL, DSA_Destroy, HDSA);
	FN(INT, DSA_InsertItem, HDSA, INT, LPVOID);
	FN(BOOL, DSA_SetItem, HDSA, INT, LPVOID);
	FN(BOOL, DSA_GetItem, HDSA, INT, LPVOID);
	FN(LPVOID, DSA_GetItemPtr, HDSA, INT);
	FN(BOOL, DSA_DeleteItem, HDSA, INT);
	FN(BOOL, DSA_DeleteAllItems, HDSA);
	FN(void, DSA_EnumCallback, HDSA, PFNDAENUMCALLBACK, LPVOID);

	HDSA d = DSA_Create(sizeof(struct item), 2);
	CHECK(d != NULL);
	if (!d)
		return;
	struct item x = { 1, 2 }, y = { 3, 4 }, out;
	CHECK_EQ(DSA_InsertItem(d, 0, &x), 0);
	CHECK_EQ(DSA_InsertItem(d, 0, &y), 0);
	CHECK_EQ(DSA_InsertItem(d, DA_LAST, &y), 2);
	CHECK_EQ(*(int *)d, 3);
	CHECK(DSA_GetItem(d, 1, &out) && out.a == 1 && out.b == 2);
	struct item *p = DSA_GetItemPtr(d, 0);
	CHECK(p && p->a == 3);
	CHECK(!DSA_GetItem(d, 3, &out) && !DSA_GetItemPtr(d, -1));

	/* Setting past the end fills with zeroed items. */
	CHECK(DSA_SetItem(d, 5, &x));
	CHECK_EQ(*(int *)d, 6);
	p = DSA_GetItemPtr(d, 4);
	CHECK(p && p->a == 0 && p->b == 0);
	CHECK(DSA_DeleteItem(d, 0));
	CHECK(DSA_GetItem(d, 0, &out) && out.a == 1);
	CHECK(!DSA_DeleteItem(d, 5));
	CHECK_EQ(*(int *)d, 5);

	enum_n = 0;
	DSA_EnumCallback(d, count_until, NULL);
	CHECK_EQ(enum_n, 5);
	CHECK(DSA_DeleteAllItems(d) && *(int *)d == 0);
	CHECK(DSA_Destroy(d));
	CHECK(!DSA_Create(0, 1));
}

static void test_alloc_str(void)
{
	FN(LPVOID, Alloc, DWORD);
	FN(LPVOID, ReAlloc, LPVOID, DWORD);
	FN(BOOL, Free, LPVOID);
	FN(DWORD, GetSize, LPVOID);
	FN(INT, Str_GetPtrA, LPCSTR, LPSTR, INT);
	FN(BOOL, Str_SetPtrA, LPSTR *, LPCSTR);
	FN(INT, Str_GetPtrW, LPCWSTR, LPWSTR, INT);
	FN(BOOL, Str_SetPtrW, LPWSTR *, LPCWSTR);

	BYTE *m = Alloc(16);
	CHECK(m && GetSize(m) == 16 && !m[15]);
	m[0] = 7;
	m = ReAlloc(m, 64);
	CHECK(m && m[0] == 7 && !m[63] && GetSize(m) == 64);
	CHECK(Free(m));
	m = ReAlloc(NULL, 8);
	CHECK(m && GetSize(m) == 8);
	CHECK(Free(m) && Free(NULL));

	char *s = NULL, buf[8];
	CHECK(Str_SetPtrA(&s, "hello world"));
	CHECK_STR(s, "hello world");
	CHECK_EQ(Str_GetPtrA(s, NULL, 0), 12);
	CHECK_EQ(Str_GetPtrA(s, buf, sizeof(buf)), 7);
	CHECK_STR(buf, "hello w");
	CHECK_EQ(Str_GetPtrA(NULL, buf, sizeof(buf)), 0);
	CHECK_STR(buf, "");
	CHECK(Str_SetPtrA(&s, NULL) && s == NULL);

	WCHAR *ws = NULL, wb[4];
	CHECK(Str_SetPtrW(&ws, w("wide")));
	CHECK_STR(a(ws), "wide");
	CHECK_EQ(Str_GetPtrW(ws, wb, 4), 3);
	CHECK_STR(a(wb), "wid");
	CHECK(Str_SetPtrW(&ws, NULL) && !ws);
}

/* Controls. */

static HWND parent;

static HWND make(const char *cls, DWORD style, int cx, int cy)
{
	USER(HWND, CreateWindowExW, DWORD, LPCWSTR, LPCWSTR, DWORD, int, int,
	     int, int, HWND, HMENU, HINSTANCE, LPVOID);
	return CreateWindowExW(0, w(cls), NULL, WS_CHILD | style, 0, 0, cx, cy,
			       parent, NULL, NULL, NULL);
}

static void test_classes(void)
{
	FN(BOOL, InitCommonControlsEx, const INITCOMMONCONTROLSEX *);
	FN(void, InitCommonControls, void);
	FN(BOOL, RegisterClassNameW, LPCWSTR);
	USER(BOOL, GetClassInfoW, HINSTANCE, LPCWSTR, WNDCLASSW *);
	WNDCLASSW wc;

	INITCOMMONCONTROLSEX icc = { sizeof(icc), ICC_WIN95_CLASSES };
	CHECK(InitCommonControlsEx(&icc));
	icc.dwSize = 4;
	CHECK(!InitCommonControlsEx(&icc));
	InitCommonControls();
	CHECK(GetClassInfoW(NULL, w("msctls_trackbar32"), &wc));
	CHECK(GetClassInfoW(NULL, w("SysListView32"), &wc));
	CHECK(RegisterClassNameW(w("SYSTREEVIEW32")));
	CHECK(!RegisterClassNameW(w("Button")));
}

static void test_trackbar(void)
{
	USER(LRESULT, SendMessageW, HWND, UINT, WPARAM, LPARAM);
	USER(BOOL, DestroyWindow, HWND);
	HWND t = make("msctls_trackbar32", TBS_AUTOTICKS | TBS_ENABLESELRANGE,
		      200, 30);
	CHECK(t != NULL);
	if (!t)
		return;
#define TB(m, wp, lp) SendMessageW(t, m, (WPARAM)(wp), (LPARAM)(lp))
	CHECK_EQ(TB(TBM_GETRANGEMIN, 0, 0), 0);
	CHECK_EQ(TB(TBM_GETRANGEMAX, 0, 0), 100);
	CHECK_EQ(TB(TBM_GETPAGESIZE, 0, 0), 20);
	CHECK_EQ(TB(TBM_GETLINESIZE, 0, 0), 1);
	TB(TBM_SETPOS, TRUE, 150);
	CHECK_EQ(TB(TBM_GETPOS, 0, 0), 100);	/* clamped */
	TB(TBM_SETRANGE, TRUE, MAKELONG(-10, 10));
	CHECK_EQ(TB(TBM_GETRANGEMIN, 0, 0), -10);
	CHECK_EQ(TB(TBM_GETPOS, 0, 0), 10);
	TB(TBM_SETRANGEMAX, TRUE, 5);
	CHECK_EQ(TB(TBM_GETPOS, 0, 0), 5);
	TB(TBM_SETPOS, TRUE, -3);
	CHECK_EQ(TB(TBM_GETPOS, 0, 0), -3);
	CHECK_EQ(TB(TBM_SETPAGESIZE, 0, 4), 20);
	CHECK_EQ(TB(TBM_GETPAGESIZE, 0, 0), 4);
	CHECK_EQ(TB(TBM_SETLINESIZE, 0, 2), 1);

	/* Auto tics every freq steps, then manual ones, sorted. */
	TB(TBM_SETTICFREQ, 5, 0);
	CHECK_EQ(TB(TBM_GETNUMTICS, 0, 0), 4);	/* -5, 0 and both ends */
	CHECK_EQ(TB(TBM_GETTIC, 0, 0), -5);
	CHECK_EQ(TB(TBM_GETTIC, 1, 0), 0);
	CHECK(TB(TBM_SETTIC, 0, 3));
	CHECK(!TB(TBM_SETTIC, 0, 50));
	CHECK_EQ(TB(TBM_GETTIC, 2, 0), 3);
	CHECK(TB(TBM_SETTIC, 0, -8));
	DWORD *tics = (DWORD *)TB(TBM_GETPTICS, 0, 0);
	CHECK(tics && tics[0] == (DWORD)-8 && tics[1] == (DWORD)-5 &&
	      tics[3] == 3);
	CHECK_EQ(TB(TBM_GETTIC, 4, 0), -1);
	CHECK_EQ(TB(TBM_GETNUMTICS, 0, 0), 6);
	TB(TBM_CLEARTICS, TRUE, 0);
	CHECK_EQ(TB(TBM_GETNUMTICS, 0, 0), 2);

	TB(TBM_SETSEL, TRUE, MAKELONG(-20, 4));
	CHECK_EQ(TB(TBM_GETSELSTART, 0, 0), -10);	/* clamped */
	CHECK_EQ(TB(TBM_GETSELEND, 0, 0), 4);
	TB(TBM_CLEARSEL, TRUE, 0);
	CHECK_EQ(TB(TBM_GETSELEND, 0, 0), 0);

	HWND buddy = make("STATIC", 0, 10, 10);
	CHECK(TB(TBM_SETBUDDY, TRUE, buddy) == 0);
	CHECK((HWND)TB(TBM_GETBUDDY, TRUE, 0) == buddy);
	CHECK(TB(TBM_GETBUDDY, FALSE, 0) == 0);
	RECT rc;
	TB(TBM_GETCHANNELRECT, 0, &rc);
	CHECK(rc.left == 8 && rc.right == 192);
	TB(TBM_GETTHUMBRECT, 0, &rc);
	CHECK(rc.left < rc.right && rc.top < rc.bottom);
	CHECK(DestroyWindow(t));
	CHECK(DestroyWindow(buddy));

	/* Without TBS_ENABLESELRANGE there is no selection. */
	t = make("msctls_trackbar32", 0, 100, 30);
	TB(TBM_SETSEL, TRUE, MAKELONG(10, 20));
	CHECK_EQ(TB(TBM_GETSELEND, 0, 0), 0);
	CHECK_EQ(TB(TBM_GETNUMTICS, 0, 0), 2);
	CHECK(DestroyWindow(t));
#undef TB
}

static void test_progress(void)
{
	USER(LRESULT, SendMessageW, HWND, UINT, WPARAM, LPARAM);
	USER(BOOL, DestroyWindow, HWND);
	HWND p = make("msctls_progress32", 0, 100, 20);
	CHECK(p != NULL);
	if (!p)
		return;
#define PB(m, wp, lp) SendMessageW(p, m, (WPARAM)(wp), (LPARAM)(lp))
	PBRANGE r;
	CHECK_EQ(PB(PBM_GETRANGE, TRUE, &r), 0);
	CHECK(r.iLow == 0 && r.iHigh == 100);
	CHECK_EQ(PB(PBM_SETRANGE, 0, MAKELPARAM(10, 50)), MAKELONG(0, 100));
	CHECK_EQ(PB(PBM_GETPOS, 0, 0), 10);	/* moved into range */
	CHECK_EQ(PB(PBM_SETPOS, 60, 0), 10);
	CHECK_EQ(PB(PBM_GETPOS, 0, 0), 50);
	CHECK_EQ(PB(PBM_DELTAPOS, -15, 0), 50);
	CHECK_EQ(PB(PBM_GETPOS, 0, 0), 35);
	CHECK_EQ(PB(PBM_SETSTEP, 20, 0), 10);
	CHECK_EQ(PB(PBM_STEPIT, 0, 0), 35);
	CHECK_EQ(PB(PBM_GETPOS, 0, 0), 15);	/* 55 wraps round to 15 */
	CHECK_EQ(PB(PBM_SETRANGE32, -100, 100000), MAKELONG(10, 50));
	CHECK_EQ(PB(PBM_GETRANGE, FALSE, NULL), 100000);
	CHECK_EQ(PB(PBM_GETSTATE, 0, 0), PBST_NORMAL);
	CHECK_EQ(PB(PBM_SETSTATE, PBST_ERROR, 0), PBST_NORMAL);
	CHECK_EQ(PB(PBM_GETSTATE, 0, 0), PBST_ERROR);
	CHECK_EQ((COLORREF)PB(PBM_SETBARCOLOR, 0, RGB(1, 2, 3)), CLR_DEFAULT);
	CHECK_EQ(PB(PBM_GETBARCOLOR, 0, 0), RGB(1, 2, 3));
	CHECK(DestroyWindow(p));
#undef PB
}

static void test_updown(void)
{
	FN(HWND, CreateUpDownControl, DWORD, INT, INT, INT, INT, HWND, INT,
	   HINSTANCE, HWND, INT, INT, INT);
	USER(LRESULT, SendMessageW, HWND, UINT, WPARAM, LPARAM);
	USER(BOOL, DestroyWindow, HWND);
	USER(BOOL, SetWindowTextW, HWND, LPCWSTR);
	USER(int, GetWindowTextW, HWND, LPWSTR, int);
	WCHAR text[32];

	/* The default range runs backwards. */
	HWND u = make("msctls_updown32", 0, 16, 20);
	CHECK(u != NULL);
	if (!u)
		return;
#define UD(m, wp, lp) SendMessageW(u, m, (WPARAM)(wp), (LPARAM)(lp))
	CHECK_EQ(UD(UDM_GETRANGE, 0, 0), MAKELONG(0, 100));
	CHECK_EQ(UD(UDM_GETBASE, 0, 0), 10);
	UDACCEL acc[4];
	CHECK_EQ(UD(UDM_GETACCEL, 4, acc), 3);
	CHECK(acc[1].nSec == 2 && acc[1].nInc == 5);
	CHECK(DestroyWindow(u));

	HWND edit = make("EDIT", 0, 50, 20);
	u = CreateUpDownControl(WS_CHILD | UDS_SETBUDDYINT, 0, 0, 16, 20,
				parent, 7, NULL, edit, 5000, -5000, 1234);
	CHECK(u != NULL);
	CHECK((HWND)UD(UDM_GETBUDDY, 0, 0) == edit);
	CHECK_EQ(UD(UDM_GETRANGE, 0, 0), MAKELONG(5000, -5000));
	GetWindowTextW(edit, text, 32);
	CHECK_STR(a(text), "1,234");
	CHECK_EQ(UD(UDM_SETPOS, 0, MAKELONG(-2500, 0)), 1234);
	GetWindowTextW(edit, text, 32);
	CHECK_STR(a(text), "-2,500");
	CHECK_EQ(UD(UDM_SETPOS, 0, MAKELONG(9000, 0)), -2500);
	CHECK_EQ(UD(UDM_GETPOS, 0, 0), MAKELONG(5000, 0));	/* clamped */

	/* The buddy's text is read back; bad text is an error. */
	SetWindowTextW(edit, w("4,321"));
	CHECK_EQ(UD(UDM_GETPOS, 0, 0), MAKELONG(4321, 0));
	SetWindowTextW(edit, w("lots"));
	CHECK_EQ(UD(UDM_GETPOS, 0, 0), MAKELONG(4321, 1));
	BOOL err = FALSE;
	SetWindowTextW(edit, w("99999"));
	CHECK_EQ(UD(UDM_GETPOS32, 0, &err), 4321);
	CHECK(err);
	CHECK_EQ(UD(UDM_SETBASE, 16, 0), 10);
	GetWindowTextW(edit, text, 32);
	CHECK_STR(a(text), "0x10E1");
	CHECK_EQ(UD(UDM_SETBASE, 8, 0), 0);
	CHECK_EQ(UD(UDM_GETBASE, 0, 0), 16);

	UD(UDM_SETRANGE32, -100000, 100000);
	INT lo = 0, hi = 0;
	UD(UDM_GETRANGE32, &lo, &hi);
	CHECK(lo == -100000 && hi == 100000);
	CHECK_EQ(UD(UDM_SETPOS32, 0, 70000), 4321);
	CHECK_EQ(UD(UDM_GETPOS32, 0, &err), 70000);
	CHECK(!err);
	UDACCEL one = { 0, 7 };
	CHECK(UD(UDM_SETACCEL, 1, &one));
	CHECK_EQ(UD(UDM_GETACCEL, 4, acc), 1);
	CHECK_EQ(acc[0].nInc, 7);
	CHECK(DestroyWindow(u));
	CHECK(DestroyWindow(edit));
#undef UD
}

static void test_status(void)
{
	FN(HWND, CreateStatusWindowA, LONG, LPCSTR, HWND, UINT);
	USER(LRESULT, SendMessageW, HWND, UINT, WPARAM, LPARAM);
	USER(LRESULT, SendMessageA, HWND, UINT, WPARAM, LPARAM);
	USER(BOOL, DestroyWindow, HWND);
	USER(BOOL, GetClientRect, HWND, LPRECT);
	USER(BOOL, SetWindowTextA, HWND, LPCSTR);
	RECT rc, pc;
	WCHAR wbuf[32];
	char buf[32];

	HWND s = CreateStatusWindowA(WS_CHILD | WS_VISIBLE, "Ready", parent, 9);
	CHECK(s != NULL);
	if (!s)
		return;
	/* Along the bottom of the parent. */
	GetClientRect(parent, &pc);
	GetClientRect(s, &rc);
	CHECK(rc.right == pc.right && rc.bottom == 20);
	CHECK_EQ(SendMessageW(s, SB_GETPARTS, 0, 0), 1);
	CHECK_EQ(SendMessageW(s, SB_GETTEXTLENGTHW, 0, 0), 5);
	CHECK_EQ(SendMessageW(s, SB_GETTEXTW, 0, (LPARAM)wbuf), 5);
	CHECK_STR(a(wbuf), "Ready");

	INT parts[3] = { 100, 200, -1 }, got[3];
	CHECK(SendMessageW(s, SB_SETPARTS, 3, (LPARAM)parts));
	CHECK(!SendMessageW(s, SB_SETPARTS, 0, (LPARAM)parts));
	CHECK_EQ(SendMessageW(s, SB_GETPARTS, 3, (LPARAM)got), 3);
	CHECK(got[1] == 200 && got[2] == -1);
	CHECK(SendMessageA(s, SB_SETTEXTA, 2 | SBT_POPOUT, (LPARAM)"caf\xe9"));
	CHECK_EQ(SendMessageW(s, SB_GETTEXTLENGTHW, 2, 0),
		 MAKELONG(4, SBT_POPOUT));
	CHECK_EQ(SendMessageW(s, SB_GETTEXTW, 2, (LPARAM)wbuf), MAKELONG(4, SBT_POPOUT));
	CHECK_EQ(wbuf[3], 0xe9);
	CHECK_EQ(SendMessageA(s, SB_GETTEXTA, 2, (LPARAM)buf), MAKELONG(4, SBT_POPOUT));
	CHECK_STR(buf, "caf\xe9");
	CHECK(!SendMessageW(s, SB_SETTEXTW, 3, (LPARAM)w("no such part")));
	CHECK(SendMessageW(s, SB_GETRECT, 1, (LPARAM)&rc));
	CHECK(rc.left == 102 && rc.right == 200);
	CHECK(SendMessageW(s, SB_GETRECT, 2, (LPARAM)&rc));
	CHECK(rc.right == pc.right);
	CHECK(!SendMessageW(s, SB_GETRECT, 3, (LPARAM)&rc));

	/* WM_SETTEXT is part 0's text. */
	SetWindowTextA(s, "Busy");
	CHECK_EQ(SendMessageA(s, SB_GETTEXTA, 0, (LPARAM)buf), 4);
	CHECK_STR(buf, "Busy");
	CHECK(!SendMessageW(s, SB_ISSIMPLE, 0, 0));
	SendMessageW(s, SB_SIMPLE, TRUE, 0);
	CHECK(SendMessageW(s, SB_ISSIMPLE, 0, 0));
	CHECK(SendMessageW(s, SB_SETTEXTW, SB_SIMPLEID, (LPARAM)w("simple")));
	CHECK_EQ(SendMessageW(s, SB_GETTEXTW, SB_SIMPLEID, (LPARAM)wbuf), 6);
	INT borders[3];
	CHECK(SendMessageW(s, SB_GETBORDERS, 0, (LPARAM)borders) && borders[1] == 2);
	CHECK(DestroyWindow(s));
}

/* Subclassing. */

static char trail[64];

static void note(char c)
{
	size_t n = strlen(trail);
	if (n + 1 < sizeof(trail)) {
		trail[n] = c;
		trail[n + 1] = '\0';
	}
}

static LRESULT WINAPI base_proc(HWND h, UINT m, WPARAM wp, LPARAM lp)
{
	USER(LRESULT, DefWindowProcW, HWND, UINT, WPARAM, LPARAM);
	if (m == WM_USER + 1) {
		note('o');
		return 100 + (LRESULT)wp;
	}
	return DefWindowProcW(h, m, wp, lp);
}

static LRESULT WINAPI sub_proc(HWND h, UINT m, WPARAM wp, LPARAM lp,
			       UINT_PTR id, DWORD_PTR ref)
{
	FN(LRESULT, DefSubclassProc, HWND, UINT, WPARAM, LPARAM);
	if (m == WM_USER + 1) {
		note((char)ref);
		return DefSubclassProc(h, m, wp + id, lp);
	}
	if (m == WM_NCDESTROY)
		note('d');
	return DefSubclassProc(h, m, wp, lp);
}

static void test_subclass(void)
{
	FN(BOOL, SetWindowSubclass, HWND, SUBCLASSPROC, UINT_PTR, DWORD_PTR);
	FN(BOOL, GetWindowSubclass, HWND, SUBCLASSPROC, UINT_PTR, DWORD_PTR *);
	FN(BOOL, RemoveWindowSubclass, HWND, SUBCLASSPROC, UINT_PTR);
	USER(ATOM, RegisterClassW, const WNDCLASSW *);
	USER(HWND, CreateWindowExW, DWORD, LPCWSTR, LPCWSTR, DWORD, int, int,
	     int, int, HWND, HMENU, HINSTANCE, LPVOID);
	USER(LRESULT, SendMessageW, HWND, UINT, WPARAM, LPARAM);
	USER_WLP(Get, HWND, int);
	USER(BOOL, IsWindowUnicode, HWND);
	USER(BOOL, DestroyWindow, HWND);
	WNDCLASSW wc;

	memset(&wc, 0, sizeof(wc));
	wc.lpfnWndProc = base_proc;
	wc.lpszClassName = w("SubclassTest");
	CHECK(RegisterClassW(&wc));
	HWND h = CreateWindowExW(0, w("SubclassTest"), NULL, 0, 0, 0, 10, 10,
				 NULL, NULL, NULL, NULL);
	CHECK(h != NULL);
	if (!h)
		return;

	/* The newest runs first; each passes on; then the original. */
	CHECK(SetWindowSubclass(h, sub_proc, 1, 'a'));
	CHECK(SetWindowSubclass(h, sub_proc, 10, 'b'));
	CHECK(GetWindowLongPtrW(h, GWLP_WNDPROC) != (LONG_PTR)base_proc);
	trail[0] = '\0';
	CHECK_EQ(SendMessageW(h, WM_USER + 1, 0, 0), 111);
	CHECK_STR(trail, "bao");
	/* The same pair again only changes its data. */
	CHECK(SetWindowSubclass(h, sub_proc, 1, 'c'));
	DWORD_PTR ref = 0;
	CHECK(GetWindowSubclass(h, sub_proc, 1, &ref) && ref == 'c');
	CHECK(!GetWindowSubclass(h, sub_proc, 2, &ref) && ref == 0);
	trail[0] = '\0';
	SendMessageW(h, WM_USER + 1, 0, 0);
	CHECK_STR(trail, "bco");

	/* Removing the last gives the window its own procedure back. */
	CHECK(RemoveWindowSubclass(h, sub_proc, 10));
	CHECK(!RemoveWindowSubclass(h, sub_proc, 10));
	trail[0] = '\0';
	CHECK_EQ(SendMessageW(h, WM_USER + 1, 5, 0), 106);
	CHECK_STR(trail, "co");
	CHECK(RemoveWindowSubclass(h, sub_proc, 1));
	CHECK(GetWindowLongPtrW(h, GWLP_WNDPROC) == (LONG_PTR)base_proc);
	CHECK(IsWindowUnicode(h));	/* still a Unicode window */

	/* Left in place, they see WM_NCDESTROY and are then dropped. */
	CHECK(SetWindowSubclass(h, sub_proc, 1, 'a'));
	trail[0] = '\0';
	CHECK(DestroyWindow(h));
	CHECK_STR(trail, "d");
	CHECK(!SetWindowSubclass(h, NULL, 1, 0));
}

/* Image lists. */

/* A 32-bit top-down DIB section, its bits, and a DC holding it. */
static HBITMAP dib(int cx, int cy, DWORD **bits, HDC *dc)
{
	GDI(HBITMAP, CreateDIBSection, HDC, const BITMAPINFO *, UINT, void **,
	    HANDLE, DWORD);
	GDI(HDC, CreateCompatibleDC, HDC);
	GDI(HGDIOBJ, SelectObject, HDC, HGDIOBJ);
	BITMAPINFO bi;

	memset(&bi, 0, sizeof(bi));
	bi.bmiHeader.biSize = sizeof(bi.bmiHeader);
	bi.bmiHeader.biWidth = cx;
	bi.bmiHeader.biHeight = -cy;
	bi.bmiHeader.biPlanes = 1;
	bi.bmiHeader.biBitCount = 32;
	HBITMAP b = CreateDIBSection(NULL, &bi, DIB_RGB_COLORS, (void **)bits,
				     NULL, 0);
	if (dc) {
		*dc = CreateCompatibleDC(NULL);
		SelectObject(*dc, b);
	}
	return b;
}

static void test_imagelist(void)
{
	FN(HIMAGELIST, ImageList_Create, INT, INT, UINT, INT, INT);
	FN(BOOL, ImageList_Destroy, HIMAGELIST);
	FN(INT, ImageList_GetImageCount, HIMAGELIST);
	FN(BOOL, ImageList_SetImageCount, HIMAGELIST, UINT);
	FN(INT, ImageList_Add, HIMAGELIST, HBITMAP, HBITMAP);
	FN(INT, ImageList_AddMasked, HIMAGELIST, HBITMAP, COLORREF);
	FN(INT, ImageList_AddIcon, HIMAGELIST, HICON);
	FN(BOOL, ImageList_Remove, HIMAGELIST, INT);
	FN(BOOL, ImageList_Draw, HIMAGELIST, INT, HDC, INT, INT, UINT);
	FN(BOOL, ImageList_DrawEx, HIMAGELIST, INT, HDC, INT, INT, INT, INT,
	   COLORREF, COLORREF, UINT);
	FN(BOOL, ImageList_DrawIndirect, IMAGELISTDRAWPARAMS *);
	FN(BOOL, ImageList_GetImageInfo, HIMAGELIST, INT, IMAGEINFO *);
	FN(BOOL, ImageList_GetIconSize, HIMAGELIST, INT *, INT *);
	FN(BOOL, ImageList_SetIconSize, HIMAGELIST, INT, INT);
	FN(COLORREF, ImageList_SetBkColor, HIMAGELIST, COLORREF);
	FN(HIMAGELIST, ImageList_Duplicate, HIMAGELIST);
	FN(BOOL, ImageList_Copy, HIMAGELIST, INT, HIMAGELIST, INT, UINT);
	FN(HIMAGELIST, ImageList_Merge, HIMAGELIST, INT, HIMAGELIST, INT, INT,
	   INT);
	FN(BOOL, ImageList_Replace, HIMAGELIST, INT, HBITMAP, HBITMAP);
	FN(BOOL, ImageList_SetOverlayImage, HIMAGELIST, INT, INT);
	FN(HICON, ImageList_GetIcon, HIMAGELIST, INT, UINT);
	GDI(BOOL, DeleteObject, HGDIOBJ);
	GDI(BOOL, DeleteDC, HDC);
	USER(HICON, LoadIconW, HINSTANCE, LPCWSTR);

	HIMAGELIST il = ImageList_Create(4, 2, ILC_COLOR32 | ILC_MASK, 1, 1);
	CHECK(il != NULL);
	if (!il)
		return;
	CHECK(!ImageList_Create(0, 2, ILC_COLOR32, 1, 1));

	/* Two images from one bitmap: red with a magenta corner, then blue. */
	DWORD *src;
	HBITMAP b = dib(8, 2, &src, NULL);
	for (int i = 0; i < 16; i++)
		src[i] = (i % 8) < 4 ? 0xff0000 : 0x0000ff;
	src[0] = 0xff00ff;
	CHECK_EQ(ImageList_AddMasked(il, b, RGB(255, 0, 255)), 0);
	CHECK_EQ(ImageList_GetImageCount(il), 2);
	IMAGEINFO info;
	CHECK(ImageList_GetImageInfo(il, 1, &info));
	CHECK(info.rcImage.left == 4 && info.rcImage.right == 8 &&
	      info.rcImage.bottom == 2 && info.hbmImage && info.hbmMask);
	CHECK(!ImageList_GetImageInfo(il, 2, &info));

	/* Drawn transparently, the corner keeps what was underneath. */
	DWORD *dst;
	HDC dc;
	HBITMAP db = dib(8, 4, &dst, &dc);
	for (int i = 0; i < 32; i++)
		dst[i] = 0x00ff00;
	CHECK(ImageList_Draw(il, 0, dc, 1, 1, ILD_TRANSPARENT));
	CHECK_EQ(dst[8 + 1], 0x00ff00);			/* the corner */
	CHECK_EQ(dst[8 + 2], 0xff0000);
	CHECK_EQ(dst[16 + 4], 0xff0000);
	CHECK_EQ(dst[16 + 5], 0x00ff00);		/* outside */
	/* On a background colour, the corner gets it. */
	CHECK(ImageList_DrawEx(il, 0, dc, 0, 0, 0, 0, RGB(0, 0, 0x80),
			       CLR_DEFAULT, ILD_NORMAL));
	CHECK_EQ(dst[0], 0x000080);
	CHECK_EQ(dst[1], 0xff0000);
	/* ILD_MASK draws the mask: white where transparent. */
	CHECK(ImageList_Draw(il, 0, dc, 4, 2, ILD_MASK));
	CHECK_EQ(dst[16 + 4], 0xffffff);
	CHECK_EQ(dst[16 + 5], 0x000000);
	/* A part of it, from IMAGELISTDRAWPARAMS. */
	IMAGELISTDRAWPARAMS p;
	memset(&p, 0, sizeof(p));
	p.cbSize = sizeof(p);
	p.himl = il;
	p.i = 1;
	p.hdcDst = dc;
	p.x = 6;
	p.cx = 1;
	p.cy = 1;
	p.xBitmap = 3;
	p.rgbBk = CLR_NONE;
	dst[5] = dst[6] = dst[7] = 0x123456;
	CHECK(ImageList_DrawIndirect(&p));
	CHECK(dst[5] == 0x123456 && dst[6] == 0x0000ff && dst[7] == 0x123456);
	p.cbSize = 40;
	CHECK(!ImageList_DrawIndirect(&p));

	/* Overlays go on top, transparently. */
	CHECK(ImageList_SetOverlayImage(il, 1, 1));
	CHECK(!ImageList_SetOverlayImage(il, 1, 16));
	for (int i = 0; i < 32; i++)
		dst[i] = 0x00ff00;
	CHECK(ImageList_Draw(il, 0, dc, 0, 0,
			     ILD_TRANSPARENT | (1 << 8)));	/* INDEXTOOVERLAYMASK(1) */
	CHECK_EQ(dst[0], 0x0000ff);

	/* Growing, removing and replacing. */
	CHECK_EQ(ImageList_Add(il, b, NULL), 2);	/* opaque this time */
	CHECK_EQ(ImageList_GetImageCount(il), 4);
	CHECK(ImageList_Remove(il, 0));
	CHECK_EQ(ImageList_GetImageCount(il), 3);
	for (int i = 0; i < 32; i++)
		dst[i] = 0x00ff00;
	CHECK(ImageList_Draw(il, 1, dc, 0, 0, ILD_TRANSPARENT));
	CHECK_EQ(dst[0], 0xff00ff);	/* the corner, now opaque */
	CHECK(ImageList_Draw(il, 0, dc, 0, 2, ILD_TRANSPARENT));
	CHECK_EQ(dst[16], 0x0000ff);
	CHECK(ImageList_Replace(il, 0, b, NULL));
	CHECK(ImageList_Draw(il, 0, dc, 0, 2, ILD_NORMAL));
	CHECK_EQ(dst[17], 0xff0000);
	CHECK(!ImageList_Replace(il, 3, b, NULL));

	/* Swapping, duplicating and merging. */
	CHECK(ImageList_Copy(il, 0, il, 2, ILCF_SWAP));
	CHECK(ImageList_Draw(il, 2, dc, 0, 0, ILD_NORMAL));
	CHECK_EQ(dst[1], 0xff0000);
	CHECK(ImageList_Draw(il, 0, dc, 0, 0, ILD_NORMAL));
	CHECK_EQ(dst[1], 0x0000ff);
	HIMAGELIST dup = ImageList_Duplicate(il);
	CHECK(dup && ImageList_GetImageCount(dup) == 3);
	HIMAGELIST m = ImageList_Merge(il, 1, dup, 2, 2, 0);
	INT cx = 0, cy = 0;
	CHECK(m && ImageList_GetIconSize(m, &cx, &cy) && cx == 6 && cy == 2);
	CHECK(ImageList_Destroy(m));

	/* Written to a stream and read back: the same images and mask. */
	struct module *ole = mod_load("ole32.dll", NULL);
	CHECK(ole != NULL);
	if (ole) {
		FN(BOOL, ImageList_Write, HIMAGELIST, IStream *);
		FN(HRESULT, ImageList_WriteEx, HIMAGELIST, DWORD, IStream *);
		FN(HIMAGELIST, ImageList_Read, IStream *);
		HRESULT (WINAPI *CreateStreamOnHGlobal)(HGLOBAL, BOOL,
							IStream **) =
			(HRESULT (WINAPI *)(HGLOBAL, BOOL, IStream **))
			get_from(ole, "CreateStreamOnHGlobal");
		IStream *st = NULL;
		CHECK_EQ(CreateStreamOnHGlobal(NULL, TRUE, &st), S_OK);
		LARGE_INTEGER z = { .QuadPart = 0 };
		if (st) {
			ImageList_SetBkColor(il, RGB(9, 8, 7));
			CHECK(ImageList_Write(il, st));
			CHECK_EQ(ImageList_WriteEx(il, 1, st), S_OK);
			CHECK_EQ(ImageList_WriteEx(il, 2, st), E_INVALIDARG);
			st->lpVtbl->Seek(st, z, STREAM_SEEK_SET, NULL);
			WORD head[7];
			st->lpVtbl->Read(st, head, sizeof(head), NULL);
			CHECK(head[0] == 0x4c49 && head[1] == 0x101 &&
			      head[2] == 3 && head[5] == 4 && head[6] == 2);
			st->lpVtbl->Seek(st, z, STREAM_SEEK_SET, NULL);
			for (int k = 0; k < 2; k++) {	/* Write, WriteEx */
				HIMAGELIST r = ImageList_Read(st);
				CHECK(r != NULL);
				if (!r)
					break;
				CHECK_EQ(ImageList_GetImageCount(r), 3);
				CHECK(ImageList_GetIconSize(r, &cx, &cy) &&
				      cx == 4 && cy == 2);
				CHECK_EQ(ImageList_SetBkColor(r, CLR_NONE),
					 RGB(9, 8, 7));
				for (int i = 0; i < 3; i++) {
					DWORD want[16];
					for (int j = 0; j < 32; j++)
						dst[j] = 0x00ff00;
					ImageList_Draw(il, i, dc, 0, 0,
						       ILD_TRANSPARENT);
					memcpy(want, dst, sizeof(want));
					for (int j = 0; j < 32; j++)
						dst[j] = 0x00ff00;
					ImageList_Draw(r, i, dc, 0, 0,
						       ILD_TRANSPARENT);
					CHECK(!memcmp(want, dst, sizeof(want)));
				}
				ImageList_Destroy(r);
			}
			/* Not an image list. */
			st->lpVtbl->Seek(st, z, STREAM_SEEK_SET, NULL);
			DWORD junk = 0x12345678;
			st->lpVtbl->Write(st, &junk, 4, NULL);
			st->lpVtbl->Seek(st, z, STREAM_SEEK_SET, NULL);
			CHECK(ImageList_Read(st) == NULL);
			CHECK(!ImageList_Write(NULL, st));
			st->lpVtbl->Release(st);
			ImageList_SetBkColor(il, CLR_NONE);
		}
	}

	/* Icons: blank and transparent. A new size drops every image. */
	HICON icon = LoadIconW(NULL, (LPCWSTR)(ULONG_PTR)32512);
	CHECK_EQ(ImageList_AddIcon(il, icon), 3);
	for (int i = 0; i < 32; i++)
		dst[i] = 0x00ff00;
	CHECK(ImageList_Draw(il, 3, dc, 0, 0, ILD_TRANSPARENT));
	CHECK_EQ(dst[0], 0x00ff00);
	CHECK(ImageList_GetIcon(il, 3, 0) != NULL);
	CHECK(ImageList_SetImageCount(il, 6));
	CHECK_EQ(ImageList_GetImageCount(il), 6);
	CHECK_EQ(ImageList_SetBkColor(il, RGB(1, 1, 1)), CLR_NONE);
	CHECK(ImageList_SetIconSize(il, 16, 16));
	CHECK_EQ(ImageList_GetImageCount(il), 0);
	CHECK(ImageList_GetIconSize(il, &cx, &cy) && cx == 16 && cy == 16);
	CHECK(ImageList_Remove(dup, -1) && ImageList_GetImageCount(dup) == 0);

	CHECK(ImageList_Destroy(dup));
	CHECK(ImageList_Destroy(il));
	CHECK(!ImageList_Destroy(NULL));
	DeleteDC(dc);
	DeleteObject(db);
	DeleteObject(b);
}

/* Property sheets and task dialogs. */

struct my_page {
	PROPSHEETPAGEA psp;
	int cookie;
};

static int addrefs, releases, creates, last_cookie;

static UINT WINAPI page_cb(HWND h, UINT msg, struct _PROPSHEETPAGEA *p)
{
	last_cookie = ((struct my_page *)p)->cookie;
	if (msg == PSPCB_ADDREF)
		addrefs++;
	else if (msg == PSPCB_RELEASE)
		releases++;
	else if (msg == PSPCB_CREATE)
		creates++;
	return 1;
}

static void test_sheets(void)
{
	FN(HPROPSHEETPAGE, CreatePropertySheetPageA, LPCPROPSHEETPAGEA);
	FN(BOOL, DestroyPropertySheetPage, HPROPSHEETPAGE);
	FN(INT_PTR, PropertySheetA, LPCPROPSHEETHEADERA);
	FN(INT_PTR, PropertySheetW, LPCPROPSHEETHEADERW);
	UINT refs = 0;

	struct my_page pages[2];
	memset(pages, 0, sizeof(pages));
	for (int i = 0; i < 2; i++) {
		pages[i].psp.dwSize = sizeof(pages[i]);
		pages[i].psp.dwFlags = PSP_USECALLBACK | PSP_USEREFPARENT;
		pages[i].psp.pfnCallback = page_cb;
		pages[i].psp.pcRefParent = &refs;
		pages[i].cookie = 40 + i;
	}

	/* A page is a copy, with the caller's data after the structure. */
	HPROPSHEETPAGE h = CreatePropertySheetPageA(&pages[0].psp);
	CHECK(h != NULL);
	CHECK(addrefs == 1 && refs == 1 && last_cookie == 40);
	pages[0].cookie = 99;
	CHECK(DestroyPropertySheetPage(h));
	CHECK(releases == 1 && refs == 0 && last_cookie == 40);
	pages[0].cookie = 40;
	PROPSHEETPAGEA tiny = { .dwSize = 12 };
	CHECK(CreatePropertySheetPageA(&tiny) == NULL);

	/* PropertySheet answers Cancel and destroys every page. */
	PROPSHEETHEADERA psh;
	memset(&psh, 0, sizeof(psh));
	psh.dwSize = sizeof(psh);
	psh.dwFlags = PSH_PROPSHEETPAGE | PSH_NOAPPLYNOW;
	psh.pszCaption = "Codec settings";
	psh.nPages = 2;
	psh.ppsp = &pages[0].psp;
	addrefs = releases = 0;
	CHECK_EQ(PropertySheetA(&psh), 0);
	CHECK(addrefs == 2 && releases == 2 && refs == 0 && !creates);
	CHECK_EQ(last_cookie, 41);

	HPROPSHEETPAGE hp[2] = { CreatePropertySheetPageA(&pages[0].psp),
				 CreatePropertySheetPageA(&pages[1].psp) };
	CHECK(refs == 2);
	psh.dwFlags = 0;
	psh.phpage = hp;
	CHECK_EQ(PropertySheetA(&psh), 0);
	CHECK(refs == 0 && releases == 4);
	psh.dwSize = 8;
	CHECK_EQ(PropertySheetA(&psh), -1);
	PROPSHEETHEADERW pw;
	memset(&pw, 0, sizeof(pw));
	pw.dwSize = sizeof(pw);
	pw.pszCaption = w("Wide");
	CHECK_EQ(PropertySheetW(&pw), 0);
}

static void test_task_dialog(void)
{
	FN(HRESULT, TaskDialog, HWND, HINSTANCE, LPCWSTR, LPCWSTR, LPCWSTR, int,
	   LPCWSTR, int *);
	FN(HRESULT, TaskDialogIndirect, const TASKDIALOGCONFIG *, int *, int *,
	   BOOL *);
	int button = 0, radio = -1;
	BOOL checked = FALSE;

	CHECK_EQ(TaskDialog(NULL, NULL, w("Title"), w("Main"), w("Content"),
			    TDCBF_NO_BUTTON | TDCBF_YES_BUTTON, NULL, &button),
		 S_OK);
	CHECK_EQ(button, IDYES);	/* the first shown */
	CHECK_EQ(TaskDialog(NULL, NULL, NULL, NULL, NULL, 0, NULL, &button),
		 S_OK);
	CHECK_EQ(button, IDOK);

	TASKDIALOG_BUTTON custom[2] = { { 1001, NULL }, { 1002, NULL } };
	TASKDIALOG_BUTTON radios[2] = { { 7, NULL }, { 8, NULL } };
	custom[0].pszButtonText = w("Encode");
	custom[1].pszButtonText = w("Skip");
	TASKDIALOGCONFIG c;
	memset(&c, 0, sizeof(c));
	c.cbSize = sizeof(c);
	c.dwFlags = TDF_VERIFICATION_FLAG_CHECKED;
	c.dwCommonButtons = TDCBF_CANCEL_BUTTON;
	c.pButtons = custom;
	c.cButtons = 2;
	c.pRadioButtons = radios;
	c.cRadioButtons = 2;
	c.nDefaultRadioButton = 8;
	CHECK_EQ(TaskDialogIndirect(&c, &button, &radio, &checked), S_OK);
	CHECK(button == 1001 && radio == 8 && checked);
	c.nDefaultButton = IDCANCEL;
	c.nDefaultRadioButton = 3;	/* not one of them: the first */
	c.dwFlags = 0;
	CHECK_EQ(TaskDialogIndirect(&c, &button, &radio, &checked), S_OK);
	CHECK(button == IDCANCEL && radio == 7 && !checked);
	c.dwFlags = TDF_NO_DEFAULT_RADIO_BUTTON;
	c.nDefaultButton = 1002;
	CHECK_EQ(TaskDialogIndirect(&c, &button, &radio, NULL), S_OK);
	CHECK(button == 1002 && radio == 0);
	c.cbSize = 20;
	CHECK_EQ(TaskDialogIndirect(&c, &button, NULL, NULL), E_INVALIDARG);
}

/* The rest. */

static UINT notified;

static LRESULT WINAPI parent_proc(HWND h, UINT m, WPARAM wp, LPARAM lp)
{
	USER(LRESULT, DefWindowProcW, HWND, UINT, WPARAM, LPARAM);
	if (m == WM_NOTIFY) {
		notified = ((NMHDR *)lp)->code;
		return 77;
	}
	return DefWindowProcW(h, m, wp, lp);
}

static void test_misc(void)
{
	FN(HRESULT, DllGetVersion, DLLVERSIONINFO *);
	FN(BOOL, _TrackMouseEvent, TRACKMOUSEEVENT *);
	FN(void, GetEffectiveClientRect, HWND, LPRECT, const INT *);
	FN(BOOL, ShowHideMenuCtl, HWND, UINT_PTR, INT *);
	FN(HWND, CreateStatusWindowW, LONG, LPCWSTR, HWND, UINT);
	FN(HWND, CreateToolbarEx, HWND, DWORD, UINT, INT, HINSTANCE, UINT_PTR,
	   LPCTBBUTTON, INT, INT, INT, INT, INT, UINT);
	FN(LRESULT, SendNotify, HWND, HWND, UINT, NMHDR *);
	FN(HRESULT, LoadIconMetric, HINSTANCE, LPCWSTR, int, HICON *);
	FN(void, InitMUILanguage, LANGID);
	FN(LANGID, GetMUILanguage, void);
	FN(INT, LBItemFromPt, HWND, POINT, BOOL);
	USER(LONG, GetWindowLongW, HWND, int);
	USER(BOOL, GetClientRect, HWND, LPRECT);
	USER(BOOL, DestroyWindow, HWND);
	USER_WLP(Set, HWND, int, LONG_PTR);

	DLLVERSIONINFO v;
	memset(&v, 0, sizeof(v));
	v.cbSize = sizeof(v);
	CHECK_EQ(DllGetVersion(&v), S_OK);
	CHECK(v.dwMajorVersion == 5 && v.dwMinorVersion == 82);
	DLLVERSIONINFO2 v2;
	memset(&v2, 0, sizeof(v2));
	v2.info1.cbSize = sizeof(v2);
	CHECK_EQ(DllGetVersion(&v2.info1), S_OK);
	CHECK(v2.ullVersion >> 48 == 5);
	v.cbSize = 8;
	CHECK_EQ(DllGetVersion(&v), E_INVALIDARG);

	TRACKMOUSEEVENT t = { sizeof(t), TME_QUERY, parent, 5 };
	CHECK(_TrackMouseEvent(&t) && t.dwFlags == 0);
	t.cbSize = 4;
	CHECK(!_TrackMouseEvent(&t));

	/* The client area less a visible status bar along its bottom. */
	HWND s = CreateStatusWindowW(WS_CHILD | WS_VISIBLE, w("x"), parent, 50);
	HWND tb = CreateToolbarEx(parent, 0, 51, 0, NULL, 0, NULL, 0, 16, 16,
				  16, 16, sizeof(TBBUTTON));
	CHECK(s && tb);
	RECT pc, rc;
	GetClientRect(parent, &pc);
	INT info[] = { 0, 0, 1000, 50, 1001, 51, 0 };
	GetEffectiveClientRect(parent, &rc, info);
	CHECK(rc.top == 0 && rc.bottom == pc.bottom - 20);
	CHECK(ShowHideMenuCtl(parent, 1000, info));
	CHECK(!(GetWindowLongW(s, GWL_STYLE) & WS_VISIBLE));
	GetEffectiveClientRect(parent, &rc, info);
	CHECK_EQ(rc.bottom, pc.bottom);
	CHECK(!ShowHideMenuCtl(parent, 999, info));

	/* SendNotify: WM_NOTIFY to the parent, from the control. */
	SetWindowLongPtrW(parent, GWLP_WNDPROC, (LONG_PTR)parent_proc);
	NMHDR hdr;
	CHECK_EQ(SendNotify(NULL, s, NM_CLICK, &hdr), 77);
	CHECK(notified == NM_CLICK && hdr.hwndFrom == s && hdr.idFrom == 50);
	CHECK(DestroyWindow(s) && DestroyWindow(tb));

	HICON icon = NULL;
	CHECK_EQ(LoadIconMetric(NULL, (LPCWSTR)(ULONG_PTR)32512, LIM_SMALL,
				&icon), S_OK);
	CHECK(icon != NULL);
	CHECK_EQ(LoadIconMetric(NULL, (LPCWSTR)(ULONG_PTR)32512, 5, &icon),
		 E_INVALIDARG);
	InitMUILanguage(0x0409);
	CHECK_EQ(GetMUILanguage(), 0x0409);
	POINT pt = { 0, 0 };
	CHECK_EQ(LBItemFromPt(NULL, pt, FALSE), -1);
}


static int CALLBACK by_param_desc(LPARAM a1, LPARAM a2, LPARAM sort)
{
	return a1 < a2 ? 1 : a1 > a2 ? -1 : 0;
}

static int CALLBACK tv_by_param_desc(LPARAM a1, LPARAM a2, LPARAM sort)
{
	return a1 < a2 ? 1 : a1 > a2 ? -1 : 0;
}

/* Header, list view and tree view: a parent that records notifications,
 * refuses changes when asked to, and answers GETDISPINFO. */

static UINT codes[64];
static int ncodes;
static BOOL refuse;		/* ITEMCHANGING, SELCHANGING, ITEMEXPANDING */
static NMLISTVIEW last_lv;
static NMTREEVIEWW last_tv;
static int deleted_tv;
static WPARAM last_command;
static WORD commands[16];
static NMSELCHANGE last_sel;
static COMBOBOXEXITEMW last_cbe;
static HWND link_window;
static int last_link = -9;
static SYSTEMTIME last_daystate_start;
static int ncommands;
/* Called first for every WM_NOTIFY the parent gets; a nonzero result is
 * the answer. For the tests of handlers that change the control. */
static LRESULT (*notify_hook)(const NMHDR *hdr);

static int seen(UINT code)
{
	int n = 0;

	for (int i = 0; i < ncodes; i++)
		n += codes[i] == code;
	return n;
}

static LRESULT WINAPI record_proc(HWND h, UINT m, WPARAM wp, LPARAM lp)
{
	USER(LRESULT, DefWindowProcW, HWND, UINT, WPARAM, LPARAM);
	if (m == WM_COMMAND) {
		last_command = wp;
		if (ncommands < 16)
			commands[ncommands++] = HIWORD(wp);
		return 0;
	}
	if (m != WM_NOTIFY)
		return DefWindowProcW(h, m, wp, lp);
	NMHDR *hdr = (NMHDR *)lp;
	if (ncodes < 64)
		codes[ncodes++] = hdr->code;
	if (notify_hook) {
		LRESULT r = notify_hook(hdr);
		if (r)
			return r;
	}
	switch (hdr->code) {
	case LVN_ITEMCHANGING:
		last_lv = *(NMLISTVIEW *)lp;
		return refuse;
	case LVN_ITEMCHANGED:
	case LVN_INSERTITEM:
	case LVN_DELETEITEM:
		last_lv = *(NMLISTVIEW *)lp;
		return 0;
	case LVN_GETDISPINFOW: {
		NMLVDISPINFOW *d = (NMLVDISPINFOW *)lp;
		if (d->item.mask & LVIF_TEXT) {
			static WCHAR t[16];
			t[0] = 'c';
			t[1] = '0' + (WCHAR)d->item.iItem;
			t[2] = '0' + (WCHAR)d->item.iSubItem;
			t[3] = 0;
			d->item.pszText = t;	/* (its own string) */
		}
		if (d->item.mask & LVIF_IMAGE)
			d->item.iImage = 7;
		return 0;
	}
	case TVN_SELCHANGINGW:
	case TVN_ITEMEXPANDINGW:
		last_tv = *(NMTREEVIEWW *)lp;
		return refuse;
	case TVN_SELCHANGEDW:
	case TVN_ITEMEXPANDEDW:
		last_tv = *(NMTREEVIEWW *)lp;
		return 0;
	case TVN_DELETEITEMW:
		deleted_tv++;
		return 0;
	case TVN_GETDISPINFOW: {
		NMTVDISPINFOW *d = (NMTVDISPINFOW *)lp;
		if (d->item.mask & TVIF_TEXT) {
			WCHAR *t = d->item.pszText;
			if (t && d->item.cchTextMax >= 4) {
				t[0] = 'c';
				t[1] = 'b';
				t[2] = (WCHAR)('0' + d->item.lParam);
				t[3] = 0;
			}
		}
		if (d->item.mask & TVIF_CHILDREN)
			d->item.cChildren = 1;
		return 0;
	}
	case HDN_ITEMCHANGINGW:
	case TCN_SELCHANGING:
	case RBN_BEGINDRAG:
		return refuse;
	case CBEN_GETDISPINFOW: {
		NMCOMBOBOXEXW *c = (NMCOMBOBOXEXW *)lp;
		if (c->ceItem.pszText && c->ceItem.cchTextMax > 3) {
			c->ceItem.pszText[0] = 'c';
			c->ceItem.pszText[1] = 'b';
			c->ceItem.pszText[2] = (WCHAR)('0' + c->ceItem.lParam);
			c->ceItem.pszText[3] = 0;
		}
		return 0;
	}
	case CBEN_INSERTITEM:
	case CBEN_DELETEITEM:
		last_cbe = ((NMCOMBOBOXEXW *)lp)->ceItem;
		return 0;
	case NM_CLICK:
	case NM_RETURN:
		if (hdr->hwndFrom && link_window == hdr->hwndFrom)
			last_link = ((NMLINK *)lp)->item.iLink;
		return 0;
	case MCN_SELCHANGE:
	case MCN_SELECT:
		last_sel = *(NMSELCHANGE *)lp;
		return 0;
	case MCN_GETDAYSTATE: {
		NMDAYSTATE *d = (NMDAYSTATE *)lp;
		for (int i = 0; i < d->cDayState; i++)
			d->prgDayState[i] = 1u << i;	/* bold day i+1 */
		last_daystate_start = d->stStart;
		return 0;
	}
	case IPN_FIELDCHANGED: {
		NMIPADDRESS *ip = (NMIPADDRESS *)lp;
		if (ip->iValue == 13)
			ip->iValue = 31;	/* the parent can change it */
		return 0;
	}
	case PGN_CALCSIZE: {
		NMPGCALCSIZE *c = (NMPGCALCSIZE *)lp;
		if (c->dwFlag == PGF_CALCWIDTH)
			c->iWidth = 500;
		else
			c->iHeight = 400;
		return 0;
	}
	case TTN_GETDISPINFOW: {
		NMTTDISPINFOW *d = (NMTTDISPINFOW *)lp;
		d->szText[0] = 't';
		d->szText[1] = (WCHAR)('0' + d->lParam);
		d->szText[2] = 0;
		if (d->lParam == 9)
			d->uFlags |= TTF_DI_SETITEM;
		return 0;
	}
	}
	return 0;
}

static void test_header(void)
{
	USER(LRESULT, SendMessageW, HWND, UINT, WPARAM, LPARAM);
	USER(LRESULT, SendMessageA, HWND, UINT, WPARAM, LPARAM);
	USER(BOOL, DestroyWindow, HWND);
	HWND h = make("SysHeader32", HDS_BUTTONS, 300, 20);
	HDITEMW it;
	WCHAR buf[32];

	CHECK(h != NULL);
	if (!h)
		return;
	CHECK_EQ(SendMessageW(h, HDM_GETITEMCOUNT, 0, 0), 0);
	static const char *const names[3] = { "Name", "Size", "Type" };
	for (int i = 0; i < 3; i++) {
		memset(&it, 0, sizeof(it));
		it.mask = HDI_TEXT | HDI_WIDTH | HDI_FORMAT | HDI_LPARAM;
		it.pszText = w(names[i]);
		it.cxy = 50 + 10 * i;
		it.fmt = HDF_STRING | HDF_LEFT;
		it.lParam = 100 + i;
		CHECK_EQ(SendMessageW(h, HDM_INSERTITEMW, 10, (LPARAM)&it), i);
	}
	/* An ANSI one in front. */
	HDITEMA ia;
	memset(&ia, 0, sizeof(ia));
	ia.mask = HDI_TEXT | HDI_WIDTH;
	ia.pszText = (LPSTR)"First";
	ia.cxy = 40;
	CHECK_EQ(SendMessageA(h, HDM_INSERTITEMA, 0, (LPARAM)&ia), 0);
	CHECK_EQ(SendMessageW(h, HDM_GETITEMCOUNT, 0, 0), 4);

	memset(&it, 0, sizeof(it));
	it.mask = HDI_TEXT | HDI_WIDTH | HDI_LPARAM | HDI_ORDER;
	it.pszText = buf;
	it.cchTextMax = 32;
	CHECK(SendMessageW(h, HDM_GETITEMW, 2, (LPARAM)&it));
	CHECK(!strcmp(a(buf), "Size") && it.cxy == 60 && it.lParam == 101 &&
	      it.iOrder == 2);
	char abuf[32];
	ia.mask = HDI_TEXT;
	ia.pszText = abuf;
	ia.cchTextMax = 3;
	CHECK(SendMessageA(h, HDM_GETITEMA, 0, (LPARAM)&ia));
	CHECK(!strcmp(abuf, "Fi"));

	RECT rc;
	CHECK(SendMessageW(h, HDM_GETITEMRECT, 2, (LPARAM)&rc));
	CHECK(rc.left == 90 && rc.right == 150);

	/* Order: Type first. */
	int order[4] = { 3, 0, 1, 2 }, got[4];
	CHECK(SendMessageW(h, HDM_SETORDERARRAY, 4, (LPARAM)order));
	CHECK(SendMessageW(h, HDM_GETORDERARRAY, 4, (LPARAM)got));
	CHECK(!memcmp(order, got, sizeof(got)));
	CHECK_EQ(SendMessageW(h, HDM_ORDERTOINDEX, 0, 0), 3);
	int bad[4] = { 0, 0, 1, 2 };
	CHECK(!SendMessageW(h, HDM_SETORDERARRAY, 4, (LPARAM)bad));
	CHECK(SendMessageW(h, HDM_GETITEMRECT, 3, (LPARAM)&rc));
	CHECK(rc.left == 0 && rc.right == 70);
	CHECK(SendMessageW(h, HDM_GETITEMRECT, 0, (LPARAM)&rc));
	CHECK(rc.left == 70 && rc.right == 110);

	HDHITTESTINFO ht;
	ht.pt.x = 80;
	ht.pt.y = 5;
	CHECK_EQ(SendMessageW(h, HDM_HITTEST, 0, (LPARAM)&ht), 0);
	CHECK(ht.flags == HHT_ONHEADER && ht.iItem == 0);
	ht.pt.x = 109;
	SendMessageW(h, HDM_HITTEST, 0, (LPARAM)&ht);
	CHECK(ht.flags == HHT_ONDIVIDER && ht.iItem == 0);
	ht.pt.x = 290;
	CHECK_EQ(SendMessageW(h, HDM_HITTEST, 0, (LPARAM)&ht), -1);
	CHECK_EQ(ht.flags, HHT_NOWHERE);

	/* Setting tells the parent, which can refuse. */
	ncodes = 0;
	memset(&it, 0, sizeof(it));
	it.mask = HDI_WIDTH;
	it.cxy = 99;
	CHECK(SendMessageW(h, HDM_SETITEMW, 1, (LPARAM)&it));
	CHECK(seen(HDN_ITEMCHANGINGW) == 1 && seen(HDN_ITEMCHANGEDW) == 1);
	refuse = TRUE;
	it.cxy = 5;
	CHECK(!SendMessageW(h, HDM_SETITEMW, 1, (LPARAM)&it));
	refuse = FALSE;
	it.mask = HDI_WIDTH;
	SendMessageW(h, HDM_GETITEMW, 1, (LPARAM)&it);
	CHECK_EQ(it.cxy, 99);

	/* Deleting keeps the order of the rest. */
	CHECK(SendMessageW(h, HDM_DELETEITEM, 0, 0));
	CHECK_EQ(SendMessageW(h, HDM_GETITEMCOUNT, 0, 0), 3);
	CHECK(SendMessageW(h, HDM_GETORDERARRAY, 3, (LPARAM)got));
	CHECK(got[0] == 2 && got[1] == 0 && got[2] == 1);
	CHECK(!SendMessageW(h, HDM_DELETEITEM, 7, 0));

	RECT lr = { 0, 0, 300, 200 };
	WINDOWPOS wpos;
	HDLAYOUT lay = { &lr, &wpos };
	CHECK(SendMessageW(h, HDM_LAYOUT, 0, (LPARAM)&lay));
	CHECK(wpos.cy > 0 && lr.top == wpos.cy && wpos.cx == 300);
	CHECK_EQ(SendMessageW(h, HDM_GETBITMAPMARGIN, 0, 0), 6);
	CHECK(DestroyWindow(h));
}

static void test_listview(void)
{
	USER(LRESULT, SendMessageW, HWND, UINT, WPARAM, LPARAM);
	USER(LRESULT, SendMessageA, HWND, UINT, WPARAM, LPARAM);
	USER(BOOL, DestroyWindow, HWND);
	HWND h = make("SysListView32", LVS_REPORT, 300, 200);
	LVITEMW it;
	WCHAR buf[32];

	CHECK(h != NULL);
	if (!h)
		return;
	HWND hdr = (HWND)SendMessageW(h, LVM_GETHEADER, 0, 0);
	CHECK(hdr != NULL);
	LVCOLUMNW col;
	memset(&col, 0, sizeof(col));
	col.mask = LVCF_TEXT | LVCF_WIDTH;
	col.pszText = w("Name");
	col.cx = 100;
	CHECK_EQ(SendMessageW(h, LVM_INSERTCOLUMNW, 0, (LPARAM)&col), 0);
	col.pszText = w("Size");
	col.cx = 60;
	CHECK_EQ(SendMessageW(h, LVM_INSERTCOLUMNW, 1, (LPARAM)&col), 1);
	CHECK_EQ(SendMessageW(hdr, HDM_GETITEMCOUNT, 0, 0), 2);

	ncodes = 0;
	static const char *const names[3] = { "beta", "alpha", "gamma" };
	for (int i = 0; i < 3; i++) {
		memset(&it, 0, sizeof(it));
		it.mask = LVIF_TEXT | LVIF_PARAM;
		it.iItem = i;
		it.pszText = w(names[i]);
		it.lParam = 10 + i;
		CHECK_EQ(SendMessageW(h, LVM_INSERTITEMW, 0, (LPARAM)&it), i);
	}
	CHECK_EQ(seen(LVN_INSERTITEM), 3);
	CHECK_EQ(last_lv.lParam, 12);
	CHECK_EQ(SendMessageW(h, LVM_GETITEMCOUNT, 0, 0), 3);

	/* Subitem text, A and W. */
	memset(&it, 0, sizeof(it));
	it.iSubItem = 1;
	it.pszText = w("12 KB");
	CHECK(SendMessageW(h, LVM_SETITEMTEXTW, 0, (LPARAM)&it));
	LVITEMA ia;
	char abuf[32];
	memset(&ia, 0, sizeof(ia));
	ia.iSubItem = 1;
	ia.pszText = abuf;
	ia.cchTextMax = 32;
	CHECK_EQ(SendMessageA(h, LVM_GETITEMTEXTA, 0, (LPARAM)&ia), 5);
	CHECK(!strcmp(abuf, "12 KB"));
	ia.iSubItem = 0;
	ia.cchTextMax = 3;
	CHECK_EQ(SendMessageA(h, LVM_GETITEMTEXTA, 2, (LPARAM)&ia), 2);
	CHECK(!strcmp(abuf, "ga"));

	/* States: ITEMCHANGING can refuse; single selection. */
	ncodes = 0;
	memset(&it, 0, sizeof(it));
	it.state = LVIS_SELECTED | LVIS_FOCUSED;
	it.stateMask = LVIS_SELECTED | LVIS_FOCUSED;
	CHECK(SendMessageW(h, LVM_SETITEMSTATE, 1, (LPARAM)&it));
	CHECK(seen(LVN_ITEMCHANGING) == 1 && seen(LVN_ITEMCHANGED) == 1);
	CHECK(last_lv.iItem == 1 && last_lv.uNewState == it.state &&
	      last_lv.uOldState == 0 && last_lv.uChanged == LVIF_STATE);
	CHECK_EQ(SendMessageW(h, LVM_GETITEMSTATE, 1, LVIS_SELECTED),
		 LVIS_SELECTED);
	it.state = it.stateMask = LVIS_SELECTED;
	SendMessageW(h, LVM_SETITEMSTATE, 2, (LPARAM)&it);
	CHECK_EQ(SendMessageW(h, LVM_GETSELECTEDCOUNT, 0, 0), 2);
	refuse = TRUE;
	it.state = 0;
	CHECK(!SendMessageW(h, LVM_SETITEMSTATE, 2, (LPARAM)&it));
	refuse = FALSE;
	CHECK_EQ(SendMessageW(h, LVM_GETSELECTEDCOUNT, 0, 0), 2);
	CHECK_EQ(SendMessageW(h, LVM_GETNEXTITEM, -1, LVNI_SELECTED), 1);
	CHECK_EQ(SendMessageW(h, LVM_GETNEXTITEM, 1, LVNI_SELECTED), 2);
	CHECK_EQ(SendMessageW(h, LVM_GETNEXTITEM, 2, LVNI_SELECTED), -1);
	CHECK_EQ(SendMessageW(h, LVM_GETNEXTITEM, -1, LVNI_FOCUSED), 1);
	CHECK_EQ(SendMessageW(h, LVM_GETNEXTITEM, 2, LVNI_ABOVE), 1);
	/* Clearing them all. */
	it.state = 0;
	it.stateMask = LVIS_SELECTED;
	CHECK(SendMessageW(h, LVM_SETITEMSTATE, -1, (LPARAM)&it));
	CHECK_EQ(SendMessageW(h, LVM_GETSELECTEDCOUNT, 0, 0), 0);

	/* Finding. */
	LVFINDINFOW f;
	memset(&f, 0, sizeof(f));
	f.flags = LVFI_STRING;
	f.psz = w("ALPHA");
	CHECK_EQ(SendMessageW(h, LVM_FINDITEMW, -1, (LPARAM)&f), 1);
	f.flags = LVFI_PARTIAL | LVFI_STRING;
	f.psz = w("g");
	CHECK_EQ(SendMessageW(h, LVM_FINDITEMW, -1, (LPARAM)&f), 2);
	f.flags = LVFI_PARAM;
	f.lParam = 10;
	CHECK_EQ(SendMessageW(h, LVM_FINDITEMW, 0, (LPARAM)&f), -1);
	f.flags = LVFI_PARAM | LVFI_WRAP;
	CHECK_EQ(SendMessageW(h, LVM_FINDITEMW, 0, (LPARAM)&f), 0);

	/* Sorting by lParam, descending. */
	CHECK(SendMessageW(h, LVM_SORTITEMS, 0, (LPARAM)by_param_desc));
	memset(&it, 0, sizeof(it));
	it.mask = LVIF_PARAM | LVIF_TEXT;
	it.pszText = buf;
	it.cchTextMax = 32;
	SendMessageW(h, LVM_GETITEMW, 0, (LPARAM)&it);
	CHECK(it.lParam == 12 && !strcmp(a(buf), "gamma"));
	it.iItem = 2;
	SendMessageW(h, LVM_GETITEMW, 0, (LPARAM)&it);
	CHECK_EQ(it.lParam, 10);

	/* A callback item asks the parent. */
	memset(&it, 0, sizeof(it));
	it.mask = LVIF_TEXT | LVIF_IMAGE;
	it.iItem = 3;
	it.pszText = LPSTR_TEXTCALLBACKW;
	it.iImage = I_IMAGECALLBACK;
	CHECK_EQ(SendMessageW(h, LVM_INSERTITEMW, 0, (LPARAM)&it), 3);
	it.pszText = buf;
	it.cchTextMax = 32;
	it.iImage = 0;
	CHECK(SendMessageW(h, LVM_GETITEMW, 0, (LPARAM)&it));
	CHECK(!strcmp(a(buf), "c30") && it.iImage == 7);
	CHECK_EQ(SendMessageA(h, LVM_GETITEMTEXTA, 3, (LPARAM)&ia), 2);

	/* Geometry: report rows under the header. */
	RECT r0, r1;
	r0.left = r1.left = LVIR_BOUNDS;
	CHECK(SendMessageW(h, LVM_GETITEMRECT, 0, (LPARAM)&r0));
	CHECK(SendMessageW(h, LVM_GETITEMRECT, 1, (LPARAM)&r1));
	CHECK(r0.top > 0 && r1.top == r0.bottom && r0.right == 160);
	LVHITTESTINFO ht;
	memset(&ht, 0, sizeof(ht));
	ht.pt.x = 120;
	ht.pt.y = r1.top + 1;
	CHECK_EQ(SendMessageW(h, LVM_SUBITEMHITTEST, 0, (LPARAM)&ht), 1);
	CHECK(ht.iSubItem == 1 && (ht.flags & LVHT_ONITEM));
	ht.pt.y = 190;
	CHECK_EQ(SendMessageW(h, LVM_HITTEST, 0, (LPARAM)&ht), -1);
	CHECK_EQ(SendMessageW(h, LVM_GETCOLUMNWIDTH, 1, 0), 60);
	CHECK(SendMessageW(h, LVM_SETCOLUMNWIDTH, 1, 80));
	HDITEMW hi;
	memset(&hi, 0, sizeof(hi));
	hi.mask = HDI_WIDTH;
	SendMessageW(hdr, HDM_GETITEMW, 1, (LPARAM)&hi);
	CHECK_EQ(hi.cxy, 80);

	/* Views and styles. */
	CHECK_EQ(SendMessageW(h, LVM_GETVIEW, 0, 0), LV_VIEW_DETAILS);
	CHECK_EQ(SendMessageW(h, LVM_SETVIEW, LV_VIEW_LIST, 0), 1);
	CHECK_EQ(SendMessageW(h, LVM_GETVIEW, 0, 0), LV_VIEW_LIST);
	CHECK_EQ(SendMessageW(h, LVM_SETEXTENDEDLISTVIEWSTYLE,
			      LVS_EX_FULLROWSELECT, LVS_EX_FULLROWSELECT |
			      LVS_EX_GRIDLINES), 0);
	CHECK_EQ(SendMessageW(h, LVM_GETEXTENDEDLISTVIEWSTYLE, 0, 0),
		 LVS_EX_FULLROWSELECT);
	CHECK_EQ(SendMessageW(h, LVM_SETVIEW, LV_VIEW_ICON, 0), 1);
	POINT pt = { 33, 44 };
	CHECK(SendMessageW(h, LVM_SETITEMPOSITION32, 1, (LPARAM)&pt));
	pt.x = pt.y = 0;
	CHECK(SendMessageW(h, LVM_GETITEMPOSITION, 1, (LPARAM)&pt));
	CHECK(pt.x == 33 && pt.y == 44);
	SendMessageW(h, LVM_SETVIEW, LV_VIEW_DETAILS, 0);

	/* Deleting tells the parent; DELETEALLITEMS can say not to. */
	ncodes = 0;
	CHECK(SendMessageW(h, LVM_DELETEITEM, 0, 0));
	CHECK(seen(LVN_DELETEITEM) == 1 && last_lv.lParam == 12);
	CHECK(!SendMessageW(h, LVM_DELETEITEM, 9, 0));
	ncodes = 0;
	CHECK(SendMessageW(h, LVM_DELETEALLITEMS, 0, 0));
	CHECK(seen(LVN_DELETEALLITEMS) == 1 && seen(LVN_DELETEITEM) == 3);
	CHECK_EQ(SendMessageW(h, LVM_GETITEMCOUNT, 0, 0), 0);
	CHECK(SendMessageW(h, LVM_DELETECOLUMN, 0, 0));
	CHECK_EQ(SendMessageW(hdr, HDM_GETITEMCOUNT, 0, 0), 1);
	CHECK(DestroyWindow(h));

	/* An owner-data list view keeps only its count and states. */
	h = make("SysListView32", LVS_REPORT | LVS_OWNERDATA, 300, 200);
	CHECK(SendMessageW(h, LVM_SETITEMCOUNT, 1000, 0));
	CHECK_EQ(SendMessageW(h, LVM_GETITEMCOUNT, 0, 0), 1000);
	memset(&it, 0, sizeof(it));
	it.state = it.stateMask = LVIS_SELECTED;
	CHECK(SendMessageW(h, LVM_SETITEMSTATE, 500, (LPARAM)&it));
	CHECK_EQ(SendMessageW(h, LVM_GETNEXTITEM, -1, LVNI_SELECTED), 500);
	memset(&it, 0, sizeof(it));
	it.mask = LVIF_TEXT;
	it.iItem = 5;
	it.iSubItem = 2;
	it.pszText = buf;
	it.cchTextMax = 32;
	CHECK(SendMessageW(h, LVM_GETITEMW, 0, (LPARAM)&it));
	CHECK(!strcmp(a(buf), "c52"));
	CHECK(DestroyWindow(h));
}

static void test_treeview(void)
{
	USER(LRESULT, SendMessageW, HWND, UINT, WPARAM, LPARAM);
	USER(LRESULT, SendMessageA, HWND, UINT, WPARAM, LPARAM);
	USER(BOOL, DestroyWindow, HWND);
	HWND h = make("SysTreeView32", TVS_HASBUTTONS | TVS_HASLINES, 200,
		      160);
	TVINSERTSTRUCTW ins;
	WCHAR buf[32];

	CHECK(h != NULL);
	if (!h)
		return;
#define ADD(par, after, txt, param) \
	(memset(&ins, 0, sizeof(ins)), ins.hParent = (par), \
	 ins.hInsertAfter = (after), \
	 ins.item.mask = TVIF_TEXT | TVIF_PARAM, ins.item.pszText = w(txt), \
	 ins.item.lParam = (param), \
	 (HTREEITEM)SendMessageW(h, TVM_INSERTITEMW, 0, (LPARAM)&ins))
	HTREEITEM fruit = ADD(TVI_ROOT, TVI_LAST, "fruit", 1);
	HTREEITEM veg = ADD(TVI_ROOT, TVI_LAST, "veg", 2);
	HTREEITEM pear = ADD(fruit, TVI_LAST, "pear", 3);
	HTREEITEM apple = ADD(fruit, TVI_SORT, "apple", 4);
	HTREEITEM fig = ADD(fruit, TVI_SORT, "fig", 5);
	HTREEITEM top = ADD(TVI_ROOT, TVI_FIRST, "top", 6);
	CHECK(fruit && veg && pear && apple && fig && top);
	CHECK_EQ(SendMessageW(h, TVM_GETCOUNT, 0, 0), 6);

	/* Order and navigation. */
	CHECK((HTREEITEM)SendMessageW(h, TVM_GETNEXTITEM, TVGN_ROOT, 0) == top);
	CHECK((HTREEITEM)SendMessageW(h, TVM_GETNEXTITEM, TVGN_NEXT,
				      (LPARAM)top) == fruit);
	CHECK((HTREEITEM)SendMessageW(h, TVM_GETNEXTITEM, TVGN_CHILD,
				      (LPARAM)fruit) == apple);
	CHECK((HTREEITEM)SendMessageW(h, TVM_GETNEXTITEM, TVGN_NEXT,
				      (LPARAM)apple) == fig);
	CHECK((HTREEITEM)SendMessageW(h, TVM_GETNEXTITEM, TVGN_NEXT,
				      (LPARAM)fig) == pear);
	CHECK((HTREEITEM)SendMessageW(h, TVM_GETNEXTITEM, TVGN_PARENT,
				      (LPARAM)fig) == fruit);
	CHECK(!SendMessageW(h, TVM_GETNEXTITEM, TVGN_PARENT, (LPARAM)fruit));
	/* Children are hidden until fruit is expanded. */
	CHECK((HTREEITEM)SendMessageW(h, TVM_GETNEXTITEM, TVGN_NEXTVISIBLE,
				      (LPARAM)fruit) == veg);

	TVITEMW it;
	memset(&it, 0, sizeof(it));
	it.mask = TVIF_TEXT | TVIF_PARAM | TVIF_CHILDREN;
	it.hItem = fruit;
	it.pszText = buf;
	it.cchTextMax = 32;
	CHECK(SendMessageW(h, TVM_GETITEMW, 0, (LPARAM)&it));
	CHECK(!strcmp(a(buf), "fruit") && it.lParam == 1 && it.cChildren == 1);
	TVITEMA ia;
	char abuf[8];
	memset(&ia, 0, sizeof(ia));
	ia.mask = TVIF_TEXT;
	ia.hItem = apple;
	ia.pszText = abuf;
	ia.cchTextMax = 4;
	CHECK(SendMessageA(h, TVM_GETITEMA, 0, (LPARAM)&ia));
	CHECK(!strcmp(abuf, "app"));
	it.hItem = (HTREEITEM)(ULONG_PTR)0x1234;
	CHECK(!SendMessageW(h, TVM_GETITEMW, 0, (LPARAM)&it));

	/* Expanding: refused, then told once. */
	ncodes = 0;
	refuse = TRUE;
	CHECK(!SendMessageW(h, TVM_EXPAND, TVE_EXPAND, (LPARAM)fruit));
	refuse = FALSE;
	CHECK(SendMessageW(h, TVM_EXPAND, TVE_EXPAND, (LPARAM)fruit));
	CHECK(seen(TVN_ITEMEXPANDINGW) == 2 && seen(TVN_ITEMEXPANDEDW) == 1);
	CHECK(last_tv.action == TVE_EXPAND && last_tv.itemNew.hItem == fruit &&
	      last_tv.itemNew.lParam == 1);
	CHECK_EQ(SendMessageW(h, TVM_GETITEMSTATE, (WPARAM)fruit,
			      TVIS_EXPANDED | TVIS_EXPANDEDONCE),
		 TVIS_EXPANDED | TVIS_EXPANDEDONCE);
	CHECK((HTREEITEM)SendMessageW(h, TVM_GETNEXTITEM, TVGN_NEXTVISIBLE,
				      (LPARAM)fruit) == apple);
	CHECK((HTREEITEM)SendMessageW(h, TVM_GETNEXTITEM, TVGN_PREVIOUSVISIBLE,
				      (LPARAM)veg) == pear);
	CHECK(!SendMessageW(h, TVM_EXPAND, TVE_EXPAND, (LPARAM)veg));
	ncodes = 0;
	CHECK(SendMessageW(h, TVM_EXPAND, TVE_COLLAPSE, (LPARAM)fruit));
	CHECK(SendMessageW(h, TVM_EXPAND, TVE_TOGGLE, (LPARAM)fruit));
	CHECK_EQ(ncodes, 0);	/* (expanded once) */

	/* The caret: SELCHANGING can refuse. */
	ncodes = 0;
	CHECK(SendMessageW(h, TVM_SELECTITEM, TVGN_CARET, (LPARAM)fig));
	CHECK(seen(TVN_SELCHANGINGW) == 1 && seen(TVN_SELCHANGEDW) == 1);
	CHECK(last_tv.itemNew.hItem == fig && !last_tv.itemOld.hItem);
	CHECK((HTREEITEM)SendMessageW(h, TVM_GETNEXTITEM, TVGN_CARET, 0) == fig);
	CHECK(SendMessageW(h, TVM_GETITEMSTATE, (WPARAM)fig, TVIS_SELECTED));
	refuse = TRUE;
	CHECK(!SendMessageW(h, TVM_SELECTITEM, TVGN_CARET, (LPARAM)veg));
	refuse = FALSE;
	CHECK((HTREEITEM)SendMessageW(h, TVM_GETNEXTITEM, TVGN_CARET, 0) == fig);
	/* Collapsing fruit moves it there. */
	CHECK(SendMessageW(h, TVM_EXPAND, TVE_COLLAPSE, (LPARAM)fruit));
	CHECK((HTREEITEM)SendMessageW(h, TVM_GETNEXTITEM, TVGN_CARET, 0) ==
	      fruit);

	/* Rectangles: rows from the top, the text indented. */
	SendMessageW(h, TVM_EXPAND, TVE_EXPAND, (LPARAM)fruit);
	RECT rc;
	memcpy(&rc, &fig, sizeof(fig));
	CHECK(SendMessageW(h, TVM_GETITEMRECT, TRUE, (LPARAM)&rc));
	CHECK(rc.top == 3 * 16 && rc.left == 19);
	memcpy(&rc, &veg, sizeof(veg));
	CHECK(SendMessageW(h, TVM_GETITEMRECT, FALSE, (LPARAM)&rc));
	CHECK(rc.top == 5 * 16 && rc.left == 0 && rc.right == 200);
	TVHITTESTINFO ht;
	ht.pt.x = 30;
	ht.pt.y = 3 * 16 + 4;
	CHECK((HTREEITEM)SendMessageW(h, TVM_HITTEST, 0, (LPARAM)&ht) == fig);
	CHECK(ht.flags & TVHT_ONITEMLABEL);
	CHECK_EQ(SendMessageW(h, TVM_GETVISIBLECOUNT, 0, 0), 10);
	CHECK_EQ(SendMessageW(h, TVM_SETITEMHEIGHT, 21, 0), 16);
	CHECK_EQ(SendMessageW(h, TVM_GETITEMHEIGHT, 0, 0), 20);

	/* Sorting by lParam, descending. */
	TVSORTCB sc = { fruit, tv_by_param_desc, 0 };
	CHECK(SendMessageW(h, TVM_SORTCHILDRENCB, FALSE, (LPARAM)&sc));
	CHECK((HTREEITEM)SendMessageW(h, TVM_GETNEXTITEM, TVGN_CHILD,
				      (LPARAM)fruit) == fig);
	CHECK(SendMessageW(h, TVM_SORTCHILDREN, FALSE, (LPARAM)fruit));
	CHECK((HTREEITEM)SendMessageW(h, TVM_GETNEXTITEM, TVGN_CHILD,
				      (LPARAM)fruit) == apple);

	/* A callback item. */
	memset(&ins, 0, sizeof(ins));
	ins.hParent = veg;
	ins.hInsertAfter = TVI_LAST;
	ins.item.mask = TVIF_TEXT | TVIF_PARAM | TVIF_CHILDREN;
	ins.item.pszText = LPSTR_TEXTCALLBACKW;
	ins.item.lParam = 7;
	ins.item.cChildren = I_CHILDRENCALLBACK;
	HTREEITEM cb = (HTREEITEM)SendMessageW(h, TVM_INSERTITEMW, 0,
					       (LPARAM)&ins);
	memset(&it, 0, sizeof(it));
	it.mask = TVIF_TEXT | TVIF_CHILDREN;
	it.hItem = cb;
	it.pszText = buf;
	it.cchTextMax = 32;
	CHECK(SendMessageW(h, TVM_GETITEMW, 0, (LPARAM)&it));
	CHECK(!strcmp(a(buf), "cb7") && it.cChildren == 1);

	/* Deleting: each item, children too, tells the parent. */
	deleted_tv = 0;
	CHECK(SendMessageW(h, TVM_DELETEITEM, 0, (LPARAM)fruit));
	CHECK_EQ(deleted_tv, 4);
	CHECK_EQ(SendMessageW(h, TVM_GETCOUNT, 0, 0), 3);
	CHECK(!SendMessageW(h, TVM_GETNEXTITEM, TVGN_CARET, 0));
	CHECK(!SendMessageW(h, TVM_DELETEITEM, 0, (LPARAM)fruit));
	CHECK(SendMessageW(h, TVM_DELETEITEM, 0, (LPARAM)TVI_ROOT));
	CHECK_EQ(deleted_tv, 7);
	CHECK_EQ(SendMessageW(h, TVM_GETCOUNT, 0, 0), 0);
	CHECK(DestroyWindow(h));
#undef ADD
}


static void test_toolbar(void)
{
	USER(LRESULT, SendMessageW, HWND, UINT, WPARAM, LPARAM);
	USER(LRESULT, SendMessageA, HWND, UINT, WPARAM, LPARAM);
	USER(BOOL, DestroyWindow, HWND);
	USER(BOOL, GetWindowRect, HWND, LPRECT);
	USER(BOOL, GetClientRect, HWND, LPRECT);
	FN(HWND, CreateToolbarEx, HWND, DWORD, UINT, INT, HINSTANCE, UINT_PTR,
	   LPCTBBUTTON, INT, INT, INT, INT, INT, UINT);
	HWND h = make("ToolbarWindow32", CCS_NORESIZE, 200, 30);
	WCHAR buf[32];
	char abuf[32];
	RECT rc;

	CHECK(h != NULL);
	if (!h)
		return;
	SendMessageW(h, TB_BUTTONSTRUCTSIZE, sizeof(TBBUTTON), 0);
	CHECK_EQ(SendMessageW(h, TB_GETBUTTONSIZE, 0, 0), MAKELONG(24, 22));
	CHECK_EQ(SendMessageW(h, TB_GETPADDING, 0, 0), MAKELONG(7, 6));

	/* Strings: a double-NUL run, then the standard bitmap. */
	CHECK_EQ(SendMessageW(h, TB_ADDSTRINGW, 0,
			      (LPARAM)u"Open\0Save\0"), 0);
	CHECK_EQ(SendMessageA(h, TB_ADDSTRINGA, 0, (LPARAM)"Cut\0"), 2);
	CHECK_EQ(SendMessageW(h, TB_GETSTRINGW, MAKEWPARAM(32, 1),
			      (LPARAM)buf), 4);
	CHECK(!strcmp(a(buf), "Save"));
	CHECK_EQ(SendMessageW(h, TB_GETSTRINGW, MAKEWPARAM(32, 9),
			      (LPARAM)buf), -1);
	TBADDBITMAP ab = { HINST_COMMCTRL, IDB_STD_SMALL_COLOR };
	CHECK_EQ(SendMessageW(h, TB_ADDBITMAP, 0, (LPARAM)&ab), 0);
	TBADDBITMAP mine = { NULL, 0x1234 };
	CHECK_EQ(SendMessageW(h, TB_ADDBITMAP, 4, (LPARAM)&mine), 15);
	CHECK_EQ(SendMessageW(h, TB_ADDBITMAP, 4, (LPARAM)&mine), 15);

	TBBUTTON b[6];
	memset(b, 0, sizeof(b));
	for (int i = 0; i < 6; i++) {
		b[i].iBitmap = i;
		b[i].idCommand = 100 + i;
		b[i].fsState = TBSTATE_ENABLED;
		b[i].iString = -1;
	}
	b[0].iString = 0;
	b[1].iString = 1;
	b[2].fsStyle = BTNS_SEP;
	b[2].iBitmap = 0;
	b[3].fsStyle = BTNS_CHECKGROUP;
	b[4].fsStyle = BTNS_CHECKGROUP;
	b[5].fsStyle = BTNS_CHECK | BTNS_DROPDOWN;
	b[5].iString = (INT_PTR)u"Own";
	CHECK(SendMessageW(h, TB_ADDBUTTONSW, 6, (LPARAM)b));
	CHECK_EQ(SendMessageW(h, TB_BUTTONCOUNT, 0, 0), 6);
	CHECK_EQ(SendMessageW(h, TB_COMMANDTOINDEX, 104, 0), 4);
	CHECK_EQ(SendMessageW(h, TB_COMMANDTOINDEX, 999, 0), -1);

	/* Geometry: 24-wide buttons, an 8-wide separator. */
	CHECK(SendMessageW(h, TB_GETITEMRECT, 1, (LPARAM)&rc));
	CHECK(rc.left == 24 && rc.right == 48 && rc.top == 0 &&
	      rc.bottom == 22);
	CHECK(SendMessageW(h, TB_GETRECT, 103, (LPARAM)&rc));
	CHECK(rc.left == 56 && rc.right == 80);
	POINT pt = { 50, 5 };
	CHECK_EQ(SendMessageW(h, TB_HITTEST, 0, (LPARAM)&pt), -2);
	pt.x = 60;
	CHECK_EQ(SendMessageW(h, TB_HITTEST, 0, (LPARAM)&pt), 3);
	pt.x = 190;
	CHECK_EQ(SendMessageW(h, TB_HITTEST, 0, (LPARAM)&pt), -1);
	SIZE sz;
	CHECK(SendMessageW(h, TB_GETMAXSIZE, 0, (LPARAM)&sz));
	CHECK(sz.cx == 5 * 24 + 8 && sz.cy == 22);

	/* Text: from the pool or the button's own. */
	CHECK_EQ(SendMessageW(h, TB_GETBUTTONTEXTW, 101, (LPARAM)buf), 4);
	CHECK(!strcmp(a(buf), "Save"));
	CHECK_EQ(SendMessageA(h, TB_GETBUTTONTEXTA, 105, 0), 3);
	CHECK_EQ(SendMessageA(h, TB_GETBUTTONTEXTA, 105, (LPARAM)abuf), 3);
	CHECK(!strcmp(abuf, "Own"));
	CHECK_EQ(SendMessageW(h, TB_GETBUTTONTEXTW, 999, (LPARAM)buf), -1);

	/* States. */
	CHECK(SendMessageW(h, TB_ISBUTTONENABLED, 100, 0));
	CHECK(SendMessageW(h, TB_ENABLEBUTTON, 100, FALSE));
	CHECK(!SendMessageW(h, TB_ISBUTTONENABLED, 100, 0));
	CHECK_EQ(SendMessageW(h, TB_GETSTATE, 100, 0), 0);
	CHECK_EQ(SendMessageW(h, TB_GETSTATE, 999, 0), -1);
	CHECK(SendMessageW(h, TB_SETSTATE, 100, TBSTATE_ENABLED |
						TBSTATE_INDETERMINATE));
	CHECK(SendMessageW(h, TB_ISBUTTONINDETERMINATE, 100, 0));
	CHECK(SendMessageW(h, TB_CHECKBUTTON, 103, TRUE));
	CHECK(SendMessageW(h, TB_CHECKBUTTON, 104, TRUE));
	CHECK(!SendMessageW(h, TB_ISBUTTONCHECKED, 103, 0));
	CHECK(SendMessageW(h, TB_ISBUTTONCHECKED, 104, 0));
	CHECK(SendMessageW(h, TB_MARKBUTTON, 101, TRUE));
	CHECK(SendMessageW(h, TB_ISBUTTONHIGHLIGHTED, 101, 0));

	/* Hiding moves the later buttons left. */
	CHECK(SendMessageW(h, TB_HIDEBUTTON, 101, TRUE));
	CHECK(SendMessageW(h, TB_ISBUTTONHIDDEN, 101, 0));
	CHECK(!SendMessageW(h, TB_GETITEMRECT, 1, (LPARAM)&rc));
	CHECK(SendMessageW(h, TB_GETRECT, 103, (LPARAM)&rc) && rc.left == 32);
	CHECK(SendMessageW(h, TB_HIDEBUTTON, 101, FALSE));

	/* Clicks: a check-group button stays checked, a check button
	 * toggles, a disabled one does nothing. */
	ncodes = 0;
	last_command = 0;
	SendMessageW(h, WM_LBUTTONDOWN, 0, MAKELPARAM(60, 5));
	CHECK(SendMessageW(h, TB_ISBUTTONPRESSED, 103, 0));
	SendMessageW(h, WM_LBUTTONUP, 0, MAKELPARAM(60, 5));
	CHECK(!SendMessageW(h, TB_ISBUTTONPRESSED, 103, 0));
	CHECK_EQ(last_command, MAKEWPARAM(103, 0));
	CHECK(SendMessageW(h, TB_ISBUTTONCHECKED, 103, 0) &&
	      !SendMessageW(h, TB_ISBUTTONCHECKED, 104, 0));
	last_command = 0;
	SendMessageW(h, WM_LBUTTONDOWN, 0, MAKELPARAM(60, 5));
	SendMessageW(h, WM_LBUTTONUP, 0, MAKELPARAM(150, 5));
	CHECK_EQ(last_command, 0);	/* released elsewhere */
	SendMessageW(h, TB_ENABLEBUTTON, 103, FALSE);
	SendMessageW(h, WM_LBUTTONDOWN, 0, MAKELPARAM(60, 5));
	SendMessageW(h, WM_LBUTTONUP, 0, MAKELPARAM(60, 5));
	CHECK_EQ(last_command, 0);
	SendMessageW(h, TB_ENABLEBUTTON, 103, TRUE);
	/* The drop-down one notifies instead. */
	SendMessageW(h, WM_LBUTTONDOWN, 0, MAKELPARAM(110, 5));
	CHECK_EQ(seen(TBN_DROPDOWN), 1);
	CHECK_EQ(last_command, 0);

	/* The hot item, with TBN_HOTITEMCHANGE. */
	CHECK_EQ(SendMessageW(h, TB_GETHOTITEM, 0, 0), -1);
	CHECK_EQ(SendMessageW(h, TB_SETHOTITEM, 1, 0), -1);
	CHECK_EQ(SendMessageW(h, TB_GETHOTITEM, 0, 0), 1);
	CHECK_EQ(seen(TBN_HOTITEMCHANGE), 1);

	/* TBBUTTONINFO by command and by index. */
	TBBUTTONINFOW bi;
	memset(&bi, 0, sizeof(bi));
	bi.cbSize = sizeof(bi);
	bi.dwMask = TBIF_TEXT | TBIF_COMMAND | TBIF_IMAGE | TBIF_SIZE;
	bi.pszText = buf;
	bi.cchText = 32;
	CHECK_EQ(SendMessageW(h, TB_GETBUTTONINFOW, 100, (LPARAM)&bi), 0);
	CHECK(!strcmp(a(buf), "Open") && bi.idCommand == 100 &&
	      bi.iImage == 0 && bi.cx == 24);
	bi.dwMask = TBIF_TEXT | TBIF_SIZE | TBIF_BYINDEX;
	bi.pszText = (LPWSTR)u"New";
	bi.cx = 40;
	CHECK(SendMessageW(h, TB_SETBUTTONINFOW, 0, (LPARAM)&bi));
	CHECK(SendMessageW(h, TB_GETITEMRECT, 1, (LPARAM)&rc) &&
	      rc.left == 40);
	CHECK_EQ(SendMessageW(h, TB_GETBUTTONTEXTW, 100, (LPARAM)buf), 3);
	bi.cbSize = 8;
	CHECK_EQ(SendMessageW(h, TB_GETBUTTONINFOW, 100, (LPARAM)&bi), -1);

	/* Insert, move, delete (with TBN_DELETINGBUTTON). */
	TBBUTTON nb;
	memset(&nb, 0, sizeof(nb));
	nb.idCommand = 200;
	nb.fsState = TBSTATE_ENABLED;
	nb.iString = -1;
	CHECK(SendMessageW(h, TB_INSERTBUTTONW, 1, (LPARAM)&nb));
	CHECK_EQ(SendMessageW(h, TB_COMMANDTOINDEX, 200, 0), 1);
	CHECK(SendMessageW(h, TB_MOVEBUTTON, 1, 6));
	CHECK_EQ(SendMessageW(h, TB_COMMANDTOINDEX, 200, 0), 6);
	TBBUTTON got;
	CHECK(SendMessageW(h, TB_GETBUTTON, 6, (LPARAM)&got) &&
	      got.idCommand == 200 && got.iString == -1);
	CHECK(SendMessageW(h, TB_SETCMDID, 6, 201));
	ncodes = 0;
	CHECK(SendMessageW(h, TB_DELETEBUTTON, 6, 0));
	CHECK_EQ(seen(TBN_DELETINGBUTTON), 1);
	CHECK_EQ(SendMessageW(h, TB_BUTTONCOUNT, 0, 0), 6);
	CHECK(!SendMessageW(h, TB_DELETEBUTTON, 6, 0));

	/* Rows and wrapping. */
	CHECK_EQ(SendMessageW(h, TB_GETROWS, 0, 0), 1);
	SendMessageW(h, TB_SETROWS, MAKEWPARAM(2, TRUE), (LPARAM)&rc);
	CHECK_EQ(SendMessageW(h, TB_GETROWS, 0, 0), 2);
	CHECK_EQ(rc.bottom, 44);
	SendMessageW(h, TB_SETROWS, MAKEWPARAM(1, TRUE), 0);
	CHECK_EQ(SendMessageW(h, TB_GETROWS, 0, 0), 1);
	SendMessageW(h, TB_SETSTYLE, 0, WS_CHILD | CCS_NORESIZE |
					TBSTYLE_WRAPABLE);
	CHECK(SendMessageW(h, TB_GETSTYLE, 0, 0) & TBSTYLE_WRAPABLE);
	SendMessageW(h, TB_SETBUTTONSIZE, 0, MAKELPARAM(60, 30));
	/* 40 + 60 + 8 + 60 fit in 200; the fifth button starts row 2. */
	CHECK_EQ(SendMessageW(h, TB_GETROWS, 0, 0), 2);
	CHECK(SendMessageW(h, TB_GETITEMRECT, 4, (LPARAM)&rc) &&
	      rc.left == 0 && rc.top == 30);

	/* Sizes, styles, the insert mark, image lists. */
	CHECK(SendMessageW(h, TB_SETBITMAPSIZE, 0, MAKELPARAM(32, 32)));
	CHECK_EQ(SendMessageW(h, TB_GETBUTTONSIZE, 0, 0), MAKELONG(60, 39));
	CHECK_EQ(SendMessageW(h, TB_SETEXTENDEDSTYLE, 0,
			      TBSTYLE_EX_DRAWDDARROWS), 0);
	CHECK_EQ(SendMessageW(h, TB_GETEXTENDEDSTYLE, 0, 0),
		 TBSTYLE_EX_DRAWDDARROWS);
	TBINSERTMARK mk = { 2, TBIMHT_AFTER }, mk2;
	SendMessageW(h, TB_SETINSERTMARK, 0, (LPARAM)&mk);
	SendMessageW(h, TB_GETINSERTMARK, 0, (LPARAM)&mk2);
	CHECK(mk2.iButton == 2 && mk2.dwFlags == TBIMHT_AFTER);
	pt.x = 30;
	pt.y = 5;
	CHECK(SendMessageW(h, TB_INSERTMARKHITTEST, (WPARAM)&pt,
			   (LPARAM)&mk2));
	CHECK(mk2.iButton == 0 && mk2.dwFlags == TBIMHT_AFTER);
	CHECK_EQ(SendMessageW(h, TB_SETIMAGELIST, 0, (LPARAM)0x1234), 0);
	CHECK_EQ(SendMessageW(h, TB_GETIMAGELIST, 0, 0), 0x1234);
	CHECK_EQ(SendMessageW(h, TB_SETPARENT, (WPARAM)h, 0), (LRESULT)parent);
	CHECK(DestroyWindow(h));

	/* CreateToolbarEx: the buttons and bitmaps, across the parent. */
	ncodes = 0;
	HWND tb = CreateToolbarEx(parent, WS_CHILD, 52, 15, HINST_COMMCTRL,
				  IDB_STD_SMALL_COLOR, b, 3, 0, 0, 16, 15,
				  sizeof(TBBUTTON));
	CHECK(tb != NULL);
	CHECK_EQ(SendMessageW(tb, TB_BUTTONCOUNT, 0, 0), 3);
	RECT pc;
	GetClientRect(parent, &pc);
	GetWindowRect(tb, &rc);
	CHECK(rc.right - rc.left == pc.right && rc.bottom - rc.top == 22);
	CHECK(DestroyWindow(tb));
}


static HWND CreateWindowExW_(HWND owner)
{
	USER(HWND, CreateWindowExW, DWORD, LPCWSTR, LPCWSTR, DWORD, int, int,
	     int, int, HWND, HMENU, HINSTANCE, LPVOID);
	return CreateWindowExW(0, w("STATIC"), NULL, WS_CHILD, 0, 0, 10, 10,
			       owner, NULL, NULL, NULL);
}

static void set_rect(RECT *r, int l, int t, int rt, int b)
{
	r->left = l;
	r->top = t;
	r->right = rt;
	r->bottom = b;
}

static void test_tooltip(void)
{
	USER(LRESULT, SendMessageW, HWND, UINT, WPARAM, LPARAM);
	USER(LRESULT, SendMessageA, HWND, UINT, WPARAM, LPARAM);
	USER(BOOL, DestroyWindow, HWND);
	HWND h = make("tooltips_class32", 0, 10, 10);
	WCHAR buf[96];
	char abuf[96];

	CHECK(h != NULL);
	if (!h)
		return;
	TTTOOLINFOW ti;
	memset(&ti, 0, sizeof(ti));
	ti.cbSize = sizeof(ti);
	ti.hwnd = parent;
	ti.uId = 1;
	set_rect(&ti.rect, 0, 0, 50, 20);
	ti.lpszText = (LPWSTR)u"First tip";
	CHECK(SendMessageW(h, TTM_ADDTOOLW, 0, (LPARAM)&ti));
	TTTOOLINFOA ta;
	memset(&ta, 0, sizeof(ta));
	ta.cbSize = offsetof(TTTOOLINFOA, lParam);	/* the oldest size */
	ta.hwnd = parent;
	ta.uId = 2;
	set_rect(&ta.rect, 50, 0, 100, 20);
	ta.lpszText = (LPSTR)"Second";
	CHECK(SendMessageA(h, TTM_ADDTOOLA, 0, (LPARAM)&ta));
	ti.uId = 3;
	set_rect(&ti.rect, 100, 0, 150, 20);
	ti.lpszText = LPSTR_TEXTCALLBACKW;
	ti.lParam = 4;
	CHECK(SendMessageW(h, TTM_ADDTOOLW, 0, (LPARAM)&ti));
	ti.uId = 4;
	set_rect(&ti.rect, 150, 0, 200, 20);
	ti.lParam = 9;
	CHECK(SendMessageW(h, TTM_ADDTOOLW, 0, (LPARAM)&ti));
	ti.cbSize = 8;
	CHECK(!SendMessageW(h, TTM_ADDTOOLW, 0, (LPARAM)&ti));
	ti.cbSize = sizeof(ti);
	CHECK_EQ(SendMessageW(h, TTM_GETTOOLCOUNT, 0, 0), 4);

	/* Text: a string, A from W, a callback, one the owner keeps. */
	ti.uId = 1;
	ti.lpszText = buf;
	SendMessageW(h, TTM_GETTEXTW, 96, (LPARAM)&ti);
	CHECK(!strcmp(a(buf), "First tip"));
	ta.uId = 1;
	ta.lpszText = abuf;
	SendMessageA(h, TTM_GETTEXTA, 96, (LPARAM)&ta);
	CHECK(!strcmp(abuf, "First tip"));
	ti.uId = 3;
	ti.lpszText = buf;
	ncodes = 0;
	SendMessageW(h, TTM_GETTEXTW, 96, (LPARAM)&ti);
	CHECK(!strcmp(a(buf), "t4") && seen(TTN_GETDISPINFOW) == 1);
	ti.uId = 4;
	SendMessageW(h, TTM_GETTEXTW, 96, (LPARAM)&ti);
	SendMessageW(h, TTM_GETTEXTW, 96, (LPARAM)&ti);
	CHECK(!strcmp(a(buf), "t9") && seen(TTN_GETDISPINFOW) == 2);

	/* Tool info round trip, and enumeration. */
	memset(&ti, 0, sizeof(ti));
	ti.cbSize = sizeof(ti);
	ti.hwnd = parent;
	ti.uId = 2;
	ti.lpszText = buf;
	CHECK(SendMessageW(h, TTM_GETTOOLINFOW, 0, (LPARAM)&ti));
	CHECK(!strcmp(a(buf), "Second") && ti.rect.left == 50);
	ti.uId = 3;
	CHECK(SendMessageW(h, TTM_GETTOOLINFOW, 0, (LPARAM)&ti));
	CHECK(ti.lpszText == LPSTR_TEXTCALLBACKW && ti.lParam == 4);
	ti.uId = 77;
	CHECK(!SendMessageW(h, TTM_GETTOOLINFOW, 0, (LPARAM)&ti));
	ti.lpszText = buf;
	CHECK(SendMessageW(h, TTM_ENUMTOOLSW, 1, (LPARAM)&ti));
	CHECK(ti.uId == 2 && !strcmp(a(buf), "Second"));
	CHECK(!SendMessageW(h, TTM_ENUMTOOLSW, 4, (LPARAM)&ti));
	ti.uId = 1;
	ti.lpszText = (LPWSTR)u"Changed";
	SendMessageW(h, TTM_UPDATETIPTEXTW, 0, (LPARAM)&ti);
	set_rect(&ti.rect, 0, 20, 50, 40);
	SendMessageW(h, TTM_NEWTOOLRECTW, 0, (LPARAM)&ti);
	ti.lpszText = buf;
	CHECK(SendMessageW(h, TTM_GETTOOLINFOW, 0, (LPARAM)&ti));
	CHECK(!strcmp(a(buf), "Changed") && ti.rect.top == 20);

	/* Hit test. */
	TTHITTESTINFOW hi;
	memset(&hi, 0, sizeof(hi));
	hi.hwnd = parent;
	hi.pt.x = 60;
	hi.pt.y = 5;
	hi.ti.cbSize = sizeof(hi.ti);
	hi.ti.lpszText = buf;
	CHECK(SendMessageW(h, TTM_HITTESTW, 0, (LPARAM)&hi));
	CHECK(hi.ti.uId == 2);
	hi.pt.x = 10;
	CHECK(!SendMessageW(h, TTM_HITTESTW, 0, (LPARAM)&hi));

	/* Relayed mouse moves show and hide the tip. */
	MSG m;
	memset(&m, 0, sizeof(m));
	m.hwnd = parent;
	m.message = WM_MOUSEMOVE;
	m.lParam = MAKELPARAM(60, 5);
	ncodes = 0;
	SendMessageW(h, TTM_RELAYEVENT, 0, (LPARAM)&m);
	CHECK_EQ(seen(TTN_SHOW), 1);
	memset(&ti, 0, sizeof(ti));
	ti.cbSize = sizeof(ti);
	CHECK(SendMessageW(h, TTM_GETCURRENTTOOLW, 0, (LPARAM)&ti) &&
	      ti.uId == 2);
	m.lParam = MAKELPARAM(300, 5);
	SendMessageW(h, TTM_RELAYEVENT, 0, (LPARAM)&m);
	CHECK_EQ(seen(TTN_POP), 1);
	CHECK(!SendMessageW(h, TTM_GETCURRENTTOOLW, 0, 0));
	SendMessageW(h, TTM_ACTIVATE, FALSE, 0);
	m.lParam = MAKELPARAM(60, 5);
	SendMessageW(h, TTM_RELAYEVENT, 0, (LPARAM)&m);
	CHECK_EQ(seen(TTN_SHOW), 1);
	SendMessageW(h, TTM_ACTIVATE, TRUE, 0);

	/* Settings. */
	CHECK_EQ(SendMessageW(h, TTM_GETDELAYTIME, TTDT_INITIAL, 0), 500);
	CHECK_EQ(SendMessageW(h, TTM_GETDELAYTIME, TTDT_AUTOPOP, 0), 5000);
	SendMessageW(h, TTM_SETDELAYTIME, TTDT_INITIAL, 200);
	CHECK_EQ(SendMessageW(h, TTM_GETDELAYTIME, TTDT_RESHOW, 0), 40);
	SendMessageW(h, TTM_SETDELAYTIME, TTDT_AUTOMATIC, 1000);
	CHECK_EQ(SendMessageW(h, TTM_GETDELAYTIME, TTDT_AUTOPOP, 0), 10000);
	SendMessageW(h, TTM_SETDELAYTIME, TTDT_AUTOMATIC, -1);
	CHECK_EQ(SendMessageW(h, TTM_GETDELAYTIME, TTDT_INITIAL, 0), 500);
	CHECK_EQ(SendMessageW(h, TTM_SETMAXTIPWIDTH, 0, 120), -1);
	CHECK_EQ(SendMessageW(h, TTM_GETMAXTIPWIDTH, 0, 0), 120);
	SendMessageW(h, TTM_SETTIPBKCOLOR, RGB(1, 2, 3), 0);
	CHECK_EQ(SendMessageW(h, TTM_GETTIPBKCOLOR, 0, 0), RGB(1, 2, 3));
	RECT mg = { 1, 2, 3, 4 }, mg2;
	SendMessageW(h, TTM_SETMARGIN, 0, (LPARAM)&mg);
	SendMessageW(h, TTM_GETMARGIN, 0, (LPARAM)&mg2);
	CHECK(!memcmp(&mg, &mg2, sizeof(mg)));
	RECT r = { 10, 10, 20, 20 };
	CHECK(SendMessageW(h, TTM_ADJUSTRECT, TRUE, (LPARAM)&r));
	CHECK(SendMessageW(h, TTM_ADJUSTRECT, FALSE, (LPARAM)&r));
	CHECK(r.left == 10 && r.top == 10 && r.right == 20 && r.bottom == 20);
	CHECK(SendMessageA(h, TTM_SETTITLEA, TTI_INFO, (LPARAM)"Title"));
	TTGETTITLE gt = { sizeof(gt), 0, 32, buf };
	SendMessageW(h, TTM_GETTITLE, 0, (LPARAM)&gt);
	CHECK(!strcmp(a(buf), "Title") && gt.uTitleBitmap == TTI_INFO);
	memset(&ti, 0, sizeof(ti));
	ti.cbSize = sizeof(ti);
	ti.hwnd = parent;
	ti.uId = 2;
	LRESULT bs = SendMessageW(h, TTM_GETBUBBLESIZE, 0, (LPARAM)&ti);
	CHECK(LOWORD(bs) > 0 && HIWORD(bs) > 16);

	/* Tracking needs TTF_TRACK. */
	ncodes = 0;
	SendMessageW(h, TTM_TRACKACTIVATE, TRUE, (LPARAM)&ti);
	CHECK_EQ(seen(TTN_SHOW), 0);
	ti.uId = 5;
	ti.uFlags = TTF_TRACK | TTF_ABSOLUTE;
	ti.lpszText = (LPWSTR)u"Tracked";
	CHECK(SendMessageW(h, TTM_ADDTOOLW, 0, (LPARAM)&ti));
	SendMessageW(h, TTM_TRACKACTIVATE, TRUE, (LPARAM)&ti);
	CHECK_EQ(seen(TTN_SHOW), 1);
	SendMessageW(h, TTM_TRACKPOSITION, 0, MAKELPARAM(30, 40));
	SendMessageW(h, TTM_TRACKACTIVATE, FALSE, 0);
	CHECK_EQ(seen(TTN_POP), 1);

	/* TTF_SUBCLASS: the tool window's own mouse moves are relayed. */
	HWND child = make("STATIC", 0, 40, 40);
	memset(&ti, 0, sizeof(ti));
	ti.cbSize = sizeof(ti);
	ti.hwnd = parent;
	ti.uId = (UINT_PTR)child;
	ti.uFlags = TTF_IDISHWND | TTF_SUBCLASS;
	ti.lpszText = (LPWSTR)u"Child";
	CHECK(SendMessageW(h, TTM_ADDTOOLW, 0, (LPARAM)&ti));
	ncodes = 0;
	SendMessageW(child, WM_MOUSEMOVE, 0, MAKELPARAM(5, 5));
	CHECK_EQ(seen(TTN_SHOW), 1);
	SendMessageW(child, WM_LBUTTONDOWN, 0, MAKELPARAM(5, 5));
	CHECK_EQ(seen(TTN_POP), 1);
	SendMessageW(h, TTM_DELTOOLW, 0, (LPARAM)&ti);
	ncodes = 0;
	SendMessageW(child, WM_MOUSEMOVE, 0, MAKELPARAM(5, 5));
	CHECK_EQ(seen(TTN_SHOW), 0);
	CHECK(DestroyWindow(child));

	/* Deleting. */
	ti.uId = 2;
	ti.uFlags = 0;
	SendMessageW(h, TTM_DELTOOLW, 0, (LPARAM)&ti);
	CHECK_EQ(SendMessageW(h, TTM_GETTOOLCOUNT, 0, 0), 4);
	CHECK(!SendMessageW(h, TTM_GETTOOLINFOW, 0, (LPARAM)&ti));
	CHECK(DestroyWindow(h));
}


static void test_tab(void)
{
	USER(LRESULT, SendMessageW, HWND, UINT, WPARAM, LPARAM);
	USER(LRESULT, SendMessageA, HWND, UINT, WPARAM, LPARAM);
	USER(BOOL, DestroyWindow, HWND);
	HWND h = make("SysTabControl32", TCS_FIXEDWIDTH, 200, 100);
	WCHAR buf[32];
	RECT rc;

	CHECK(h != NULL);
	if (!h)
		return;
	CHECK_EQ(SendMessageW(h, TCM_GETCURSEL, 0, 0), -1);
	TCITEMW it;
	memset(&it, 0, sizeof(it));
	it.mask = TCIF_TEXT | TCIF_PARAM;
	static const char *const names[3] = { "One", "Two", "Three" };
	for (int i = 0; i < 3; i++) {
		it.pszText = w(names[i]);
		it.lParam = 10 + i;
		CHECK_EQ(SendMessageW(h, TCM_INSERTITEMW, 99, (LPARAM)&it), i);
	}
	TCITEMA ia;
	memset(&ia, 0, sizeof(ia));
	ia.mask = TCIF_TEXT | TCIF_IMAGE;
	ia.pszText = (LPSTR)"Zero";
	ia.iImage = 2;
	CHECK_EQ(SendMessageA(h, TCM_INSERTITEMA, 0, (LPARAM)&ia), 0);
	CHECK_EQ(SendMessageW(h, TCM_GETITEMCOUNT, 0, 0), 4);
	/* The first insert selected item 0, which is now item 1. */
	CHECK_EQ(SendMessageW(h, TCM_GETCURSEL, 0, 0), 1);
	CHECK(!SendMessageW(h, TCM_SETITEMEXTRA, 8, 0));

	memset(&it, 0, sizeof(it));
	it.mask = TCIF_TEXT | TCIF_PARAM | TCIF_IMAGE;
	it.pszText = buf;
	it.cchTextMax = 32;
	CHECK(SendMessageW(h, TCM_GETITEMW, 2, (LPARAM)&it));
	CHECK(!strcmp(a(buf), "Two") && it.lParam == 11 && it.iImage == -1);
	CHECK(SendMessageW(h, TCM_GETITEMW, 0, (LPARAM)&it));
	CHECK(!strcmp(a(buf), "Zero") && it.iImage == 2);
	CHECK(!SendMessageW(h, TCM_GETITEMW, 4, (LPARAM)&it));

	/* Fixed width tabs, side by side. */
	CHECK(SendMessageW(h, TCM_GETITEMRECT, 1, (LPARAM)&rc));
	CHECK(rc.left == 96 && rc.right == 192 && rc.bottom == 20);
	CHECK_EQ(SendMessageW(h, TCM_GETROWCOUNT, 0, 0), 1);
	CHECK_EQ(SendMessageW(h, TCM_SETITEMSIZE, 0, MAKELPARAM(40, 24)),
		 MAKELONG(96, 20));
	CHECK(SendMessageW(h, TCM_GETITEMRECT, 3, (LPARAM)&rc));
	CHECK(rc.left == 120 && rc.right == 160 && rc.bottom == 24);
	TCHITTESTINFO hi = { { 50, 5 }, 0 };
	CHECK_EQ(SendMessageW(h, TCM_HITTEST, 0, (LPARAM)&hi), 1);
	CHECK_EQ(hi.flags, TCHT_ONITEMLABEL);
	hi.pt.y = 50;
	CHECK_EQ(SendMessageW(h, TCM_HITTEST, 0, (LPARAM)&hi), -1);
	CHECK_EQ(hi.flags, TCHT_NOWHERE);
	RECT disp = { 0, 0, 200, 100 };
	SendMessageW(h, TCM_ADJUSTRECT, FALSE, (LPARAM)&disp);
	CHECK(disp.left == 2 && disp.top == 26 && disp.right == 198 &&
	      disp.bottom == 98);
	SendMessageW(h, TCM_ADJUSTRECT, TRUE, (LPARAM)&disp);
	CHECK(disp.left == 0 && disp.top == 0 && disp.bottom == 100);

	/* SETCURSEL doesn't notify; a click does, and can be refused. */
	ncodes = 0;
	CHECK_EQ(SendMessageW(h, TCM_SETCURSEL, 2, 0), 1);
	CHECK_EQ(SendMessageW(h, TCM_SETCURSEL, 9, 0), -1);
	CHECK_EQ(SendMessageW(h, TCM_GETCURSEL, 0, 0), 2);
	CHECK_EQ(ncodes, 0);
	SendMessageW(h, WM_LBUTTONDOWN, 0, MAKELPARAM(130, 5));
	CHECK_EQ(SendMessageW(h, TCM_GETCURSEL, 0, 0), 3);
	CHECK(seen(TCN_SELCHANGING) == 1 && seen(TCN_SELCHANGE) == 1);
	refuse = TRUE;
	SendMessageW(h, WM_LBUTTONDOWN, 0, MAKELPARAM(10, 5));
	refuse = FALSE;
	CHECK_EQ(SendMessageW(h, TCM_GETCURSEL, 0, 0), 3);
	CHECK_EQ(seen(TCN_SELCHANGE), 1);
	SendMessageW(h, TCM_SETCURFOCUS, 0, 0);
	CHECK(SendMessageW(h, TCM_GETCURSEL, 0, 0) == 0 &&
	      SendMessageW(h, TCM_GETCURFOCUS, 0, 0) == 0);

	/* States, images, deleting. */
	CHECK(SendMessageW(h, TCM_HIGHLIGHTITEM, 2, TRUE));
	memset(&it, 0, sizeof(it));
	it.mask = TCIF_STATE;
	it.dwStateMask = TCIS_HIGHLIGHTED | TCIS_BUTTONPRESSED;
	CHECK(SendMessageW(h, TCM_GETITEMW, 2, (LPARAM)&it));
	CHECK_EQ(it.dwState, TCIS_HIGHLIGHTED);
	/* Without an image list there is nothing to remove. */
	SendMessageW(h, TCM_REMOVEIMAGE, 1, 0);
	it.mask = TCIF_IMAGE;
	CHECK(SendMessageW(h, TCM_GETITEMW, 0, (LPARAM)&it) && it.iImage == 2);
	{
		FN(HIMAGELIST, ImageList_Create, INT, INT, UINT, INT, INT);
		FN(BOOL, ImageList_Destroy, HIMAGELIST);
		FN(INT, ImageList_GetImageCount, HIMAGELIST);
		FN(BOOL, ImageList_SetImageCount, HIMAGELIST, UINT);
		HIMAGELIST il = ImageList_Create(16, 16, ILC_COLOR32, 3, 1);
		CHECK(il && ImageList_SetImageCount(il, 3));
		SendMessageW(h, TCM_SETIMAGELIST, 0, (LPARAM)il);
		SendMessageW(h, TCM_REMOVEIMAGE, 1, 0);
		CHECK_EQ(ImageList_GetImageCount(il), 2);
		CHECK(SendMessageW(h, TCM_GETITEMW, 0, (LPARAM)&it) &&
		      it.iImage == 1);
		SendMessageW(h, TCM_SETIMAGELIST, 0, 0);
		ImageList_Destroy(il);
	}
	SendMessageW(h, TCM_SETCURSEL, 2, 0);
	CHECK(SendMessageW(h, TCM_DELETEITEM, 1, 0));
	CHECK_EQ(SendMessageW(h, TCM_GETCURSEL, 0, 0), 1);
	CHECK(SendMessageW(h, TCM_DELETEITEM, 1, 0));
	CHECK_EQ(SendMessageW(h, TCM_GETCURSEL, 0, 0), -1);
	CHECK(!SendMessageW(h, TCM_DELETEITEM, 5, 0));
	CHECK(SendMessageW(h, TCM_DELETEALLITEMS, 0, 0));
	CHECK_EQ(SendMessageW(h, TCM_GETITEMCOUNT, 0, 0), 0);

	/* Extra bytes, once empty; multiline wrapping. */
	CHECK(SendMessageW(h, TCM_SETITEMEXTRA, 2 * sizeof(LPARAM), 0));
	struct {
		TCITEMHEADERW hdr;
		LPARAM data[2];
	} big;
	memset(&big, 0, sizeof(big));
	big.hdr.mask = TCIF_PARAM;
	big.data[0] = 5;
	big.data[1] = 6;
	CHECK_EQ(SendMessageW(h, TCM_INSERTITEMW, 0, (LPARAM)&big), 0);
	memset(&big, 0, sizeof(big));
	big.hdr.mask = TCIF_PARAM;
	CHECK(SendMessageW(h, TCM_GETITEMW, 0, (LPARAM)&big));
	CHECK(big.data[0] == 5 && big.data[1] == 6);
	CHECK(DestroyWindow(h));

	h = make("SysTabControl32", TCS_FIXEDWIDTH | TCS_MULTILINE, 200, 100);
	SendMessageW(h, TCM_SETITEMSIZE, 0, MAKELPARAM(80, 20));
	memset(&it, 0, sizeof(it));
	for (int i = 0; i < 5; i++)
		SendMessageW(h, TCM_INSERTITEMW, i, (LPARAM)&it);
	CHECK_EQ(SendMessageW(h, TCM_GETROWCOUNT, 0, 0), 3);
	CHECK(SendMessageW(h, TCM_GETITEMRECT, 2, (LPARAM)&rc));
	CHECK(rc.left == 0 && rc.top == 20);
	CHECK(DestroyWindow(h));
}


static void test_rebar(void)
{
	USER(LRESULT, SendMessageW, HWND, UINT, WPARAM, LPARAM);
	USER(LRESULT, SendMessageA, HWND, UINT, WPARAM, LPARAM);
	USER(BOOL, DestroyWindow, HWND);
	USER(BOOL, GetWindowRect, HWND, LPRECT);
	USER(BOOL, GetClientRect, HWND, LPRECT);
	HWND h = make("ReBarWindow32", 0, 50, 10);
	WCHAR buf[32];
	RECT rc, pc;

	CHECK(h != NULL);
	if (!h)
		return;
	HWND child = make("STATIC", 0, 10, 10);
	REBARBANDINFOW bi;
	memset(&bi, 0, sizeof(bi));
	bi.cbSize = sizeof(bi);
	bi.fMask = RBBIM_STYLE | RBBIM_CHILD | RBBIM_CHILDSIZE | RBBIM_SIZE |
		   RBBIM_ID | RBBIM_LPARAM;
	bi.fStyle = RBBS_NOGRIPPER;
	bi.hwndChild = child;
	bi.cxMinChild = 50;
	bi.cyMinChild = 24;
	bi.cx = 120;
	bi.wID = 7;
	bi.lParam = 70;
	ncodes = 0;
	CHECK(SendMessageW(h, RB_INSERTBANDW, -1, (LPARAM)&bi));
	CHECK_EQ(seen(RBN_HEIGHTCHANGE), 1);
	REBARBANDINFOA ba;
	memset(&ba, 0, sizeof(ba));
	ba.cbSize = offsetof(REBARBANDINFOA, cyChild);	/* the oldest size */
	ba.fMask = RBBIM_TEXT | RBBIM_SIZE | RBBIM_ID;
	ba.lpText = (LPSTR)"Links";
	ba.cx = 80;
	ba.wID = 8;
	CHECK(SendMessageA(h, RB_INSERTBANDA, -1, (LPARAM)&ba));
	ba.cbSize = 8;
	CHECK(!SendMessageA(h, RB_INSERTBANDA, -1, (LPARAM)&ba));
	CHECK_EQ(SendMessageW(h, RB_GETBANDCOUNT, 0, 0), 2);
	CHECK_EQ(SendMessageW(h, RB_IDTOINDEX, 8, 0), 1);
	CHECK_EQ(SendMessageW(h, RB_IDTOINDEX, 9, 0), -1);

	/* One row, across the parent; the child after its band's header. */
	CHECK_EQ(SendMessageW(h, RB_GETROWCOUNT, 0, 0), 1);
	CHECK_EQ(SendMessageW(h, RB_GETBARHEIGHT, 0, 0), 28);
	CHECK_EQ(SendMessageW(h, RB_GETROWHEIGHT, 1, 0), 28);
	GetClientRect(parent, &pc);
	GetWindowRect(h, &rc);
	CHECK(rc.right - rc.left == pc.right && rc.bottom - rc.top == 28);
	CHECK(SendMessageW(h, RB_GETRECT, 0, (LPARAM)&rc));
	CHECK(rc.left == 0 && rc.right == 120);
	CHECK(SendMessageW(h, RB_GETRECT, 1, (LPARAM)&rc));
	CHECK(rc.left == 120 && rc.right == pc.right);
	GetWindowRect(child, &rc);
	CHECK(rc.right - rc.left == 120 && rc.bottom - rc.top == 24);

	/* Band info back. */
	memset(&bi, 0, sizeof(bi));
	bi.cbSize = sizeof(bi);
	bi.fMask = RBBIM_TEXT | RBBIM_ID | RBBIM_SIZE | RBBIM_HEADERSIZE;
	bi.lpText = buf;
	bi.cch = 32;
	CHECK(SendMessageW(h, RB_GETBANDINFOW, 1, (LPARAM)&bi));
	CHECK(!strcmp(a(buf), "Links") && bi.wID == 8 && bi.cx == 80);
	CHECK(bi.cxHeader > 10);
	bi.fMask = RBBIM_LPARAM | RBBIM_CHILD;
	CHECK(SendMessageW(h, RB_GETBANDINFOW, 0, (LPARAM)&bi));
	CHECK(bi.lParam == 70 && bi.hwndChild == child);
	CHECK(!SendMessageW(h, RB_GETBANDINFOW, 2, (LPARAM)&bi));

	/* Hit test. */
	RBHITTESTINFO hi;
	memset(&hi, 0, sizeof(hi));
	hi.pt.x = 125;
	hi.pt.y = 5;
	CHECK_EQ(SendMessageW(h, RB_HITTEST, 0, (LPARAM)&hi), 1);
	CHECK(hi.iBand == 1 && hi.flags == RBHT_GRABBER);
	hi.pt.x = 5;
	CHECK_EQ(SendMessageW(h, RB_HITTEST, 0, (LPARAM)&hi), 0);
	CHECK_EQ(hi.flags, RBHT_CLIENT);
	hi.pt.y = 50;
	CHECK_EQ(SendMessageW(h, RB_HITTEST, 0, (LPARAM)&hi), -1);
	CHECK_EQ(hi.flags, RBHT_NOWHERE);

	/* A break starts a second row. */
	memset(&bi, 0, sizeof(bi));
	bi.cbSize = sizeof(bi);
	bi.fMask = RBBIM_STYLE | RBBIM_ID;
	bi.fStyle = RBBS_BREAK;
	bi.wID = 9;
	CHECK(SendMessageW(h, RB_INSERTBANDW, -1, (LPARAM)&bi));
	CHECK_EQ(SendMessageW(h, RB_GETROWCOUNT, 0, 0), 2);
	CHECK(SendMessageW(h, RB_GETRECT, 2, (LPARAM)&rc) && rc.top == 28);
	CHECK(SendMessageW(h, RB_SHOWBAND, 2, FALSE));
	CHECK_EQ(SendMessageW(h, RB_GETROWCOUNT, 0, 0), 1);
	CHECK(SendMessageW(h, RB_SHOWBAND, 2, TRUE));

	/* Dragging, which the parent can refuse. */
	SendMessageW(h, RB_BEGINDRAG, 1, MAKELPARAM(120, 5));
	SendMessageW(h, RB_DRAGMOVE, 0, MAKELPARAM(150, 5));
	SendMessageW(h, RB_ENDDRAG, 0, 0);
	CHECK(SendMessageW(h, RB_GETRECT, 0, (LPARAM)&rc) && rc.right == 150);
	CHECK_EQ(seen(RBN_ENDDRAG), 1);
	refuse = TRUE;
	SendMessageW(h, RB_BEGINDRAG, 1, MAKELPARAM(150, 5));
	refuse = FALSE;
	SendMessageW(h, RB_DRAGMOVE, 0, MAKELPARAM(200, 5));
	CHECK(SendMessageW(h, RB_GETRECT, 0, (LPARAM)&rc) && rc.right == 150);
	SendMessageW(h, RB_MINIMIZEBAND, 0, 0);
	CHECK(SendMessageW(h, RB_GETRECT, 0, (LPARAM)&rc) && rc.right == 50);
	SendMessageW(h, RB_MAXIMIZEBAND, 0, FALSE);
	CHECK(SendMessageW(h, RB_GETRECT, 1, (LPARAM)&rc));
	int links_min = rc.right - rc.left;
	CHECK(SendMessageW(h, RB_GETRECT, 0, (LPARAM)&rc) &&
	      rc.right == pc.right - links_min);

	/* Moving, colours, bar info, deleting. */
	CHECK(SendMessageW(h, RB_MOVEBAND, 0, 1));
	CHECK_EQ(SendMessageW(h, RB_IDTOINDEX, 7, 0), 1);
	CHECK_EQ((COLORREF)SendMessageW(h, RB_SETBKCOLOR, 0, RGB(1, 2, 3)),
		 CLR_NONE);
	CHECK_EQ(SendMessageW(h, RB_GETBKCOLOR, 0, 0), RGB(1, 2, 3));
	REBARINFO ri = { sizeof(ri), RBIM_IMAGELIST, (HIMAGELIST)0x55 };
	CHECK(SendMessageW(h, RB_SETBARINFO, 0, (LPARAM)&ri));
	ri.himl = NULL;
	CHECK(SendMessageW(h, RB_GETBARINFO, 0, (LPARAM)&ri) &&
	      ri.himl == (HIMAGELIST)0x55);
	ri.himl = NULL;
	SendMessageW(h, RB_SETBARINFO, 0, (LPARAM)&ri);
	ncodes = 0;
	CHECK(SendMessageW(h, RB_DELETEBAND, 2, 0));
	CHECK(seen(RBN_DELETINGBAND) == 1 && seen(RBN_DELETEDBAND) == 1);
	CHECK_EQ(SendMessageW(h, RB_GETBANDCOUNT, 0, 0), 2);
	CHECK(!SendMessageW(h, RB_DELETEBAND, 2, 0));
	CHECK(DestroyWindow(h));
	CHECK(DestroyWindow(child));
}


static int seen_command(WORD code)
{
	int n = 0;
	for (int i = 0; i < ncommands; i++)
		n += commands[i] == code;
	return n;
}

static void test_small_controls(void)
{
	USER(LRESULT, SendMessageW, HWND, UINT, WPARAM, LPARAM);
	USER(LRESULT, SendMessageA, HWND, UINT, WPARAM, LPARAM);
	USER(BOOL, DestroyWindow, HWND);
	USER(BOOL, GetWindowRect, HWND, LPRECT);
	USER(int, GetWindowTextA, HWND, LPSTR, int);
	USER(BOOL, SetWindowTextA, HWND, LPCSTR);
	WCHAR buf[64];

	/* Hot keys: the rules replace a forbidden combination. */
	HWND h = make("msctls_hotkey32", 0, 100, 20);
	CHECK(h != NULL);
	SendMessageW(h, HKM_SETHOTKEY, MAKEWORD('A', HOTKEYF_CONTROL), 0);
	CHECK_EQ(SendMessageW(h, HKM_GETHOTKEY, 0, 0),
		 MAKEWORD('A', HOTKEYF_CONTROL));
	SendMessageW(h, HKM_SETRULES, HKCOMB_NONE | HKCOMB_S,
		     MAKELPARAM(HOTKEYF_CONTROL | HOTKEYF_ALT, 0));
	ncommands = 0;
	SendMessageW(h, WM_KEYDOWN, 'B', 0);
	CHECK_EQ(SendMessageW(h, HKM_GETHOTKEY, 0, 0),
		 MAKEWORD('B', HOTKEYF_CONTROL | HOTKEYF_ALT));
	CHECK_EQ(seen_command(EN_CHANGE), 1);
	SendMessageW(h, WM_KEYDOWN, VK_CONTROL, 0);
	SendMessageW(h, WM_KEYDOWN, VK_SHIFT, 0);
	SendMessageW(h, WM_KEYDOWN, 'C', 0);
	CHECK_EQ(SendMessageW(h, HKM_GETHOTKEY, 0, 0),
		 MAKEWORD('C', HOTKEYF_CONTROL | HOTKEYF_SHIFT));
	SendMessageW(h, WM_KEYUP, VK_CONTROL, 0);
	SendMessageW(h, WM_KEYUP, VK_SHIFT, 0);
	SendMessageW(h, WM_KEYDOWN, VK_BACK, 0);
	CHECK_EQ(SendMessageW(h, HKM_GETHOTKEY, 0, 0), 0);
	CHECK(DestroyWindow(h));

	/* Animation: an AVI file opens and plays; anything else doesn't. */
	char path[] = "/tmp/seance-aviXXXXXX";
	int fd = mkstemp(path);
	CHECK(fd >= 0);
	static const unsigned char avi[] = {
		'R', 'I', 'F', 'F', 100, 0, 0, 0, 'A', 'V', 'I', ' ',
		'L', 'I', 'S', 'T', 64, 0, 0, 0, 'h', 'd', 'r', 'l',
		'a', 'v', 'i', 'h', 56, 0, 0, 0,
		0x40, 0x9c, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
		12, 0, 0, 0,	/* dwTotalFrames */
		0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
	};
	CHECK(fd >= 0 && write(fd, avi, sizeof(avi)) == (ssize_t)sizeof(avi));
	if (fd >= 0)
		close(fd);
	char winpath[64];
	snprintf(winpath, sizeof(winpath), "Z:%s", path);
	h = make("SysAnimate32", 0, 50, 50);
	CHECK(!SendMessageW(h, ACM_PLAY, 1, MAKELONG(0, -1)));
	CHECK(SendMessageA(h, ACM_OPENA, 0, (LPARAM)winpath));
	ncommands = 0;
	CHECK(SendMessageW(h, ACM_PLAY, 1, MAKELONG(0, -1)));
	CHECK(seen_command(ACN_START) == 1 && seen_command(ACN_STOP) == 1);
	CHECK(!SendMessageW(h, ACM_ISPLAYING, 0, 0));
	CHECK(SendMessageW(h, ACM_PLAY, -1, MAKELONG(0, -1)));
	CHECK(SendMessageW(h, ACM_ISPLAYING, 0, 0));
	CHECK(SendMessageW(h, ACM_STOP, 0, 0));
	CHECK(!SendMessageW(h, ACM_ISPLAYING, 0, 0));
	CHECK_EQ(seen_command(ACN_STOP), 2);
	CHECK(SendMessageW(h, ACM_OPENW, 0, 0));	/* closes */
	CHECK(!SendMessageW(h, ACM_PLAY, 1, 0));
	CHECK(!SendMessageA(h, ACM_OPENA, 0, (LPARAM)"Z:/nonexistent.avi"));
	CHECK(DestroyWindow(h));
	unlink(path);

	/* IP addresses. */
	h = make("SysIPAddress32", 0, 120, 20);
	CHECK(SendMessageW(h, IPM_ISBLANK, 0, 0));
	DWORD ip = 1;
	CHECK_EQ(SendMessageW(h, IPM_GETADDRESS, 0, (LPARAM)&ip), 0);
	CHECK_EQ(ip, 0);
	SendMessageW(h, IPM_SETADDRESS, 0, MAKEIPADDRESS(192, 168, 1, 20));
	CHECK_EQ(SendMessageW(h, IPM_GETADDRESS, 0, (LPARAM)&ip), 4);
	CHECK_EQ(ip, (DWORD)MAKEIPADDRESS(192, 168, 1, 20));
	char text[32];
	GetWindowTextA(h, text, 32);
	CHECK(!strcmp(text, "192.168.1.20"));
	CHECK(SendMessageW(h, IPM_SETRANGE, 3, MAKEIPRANGE(1, 10)));
	CHECK(!SendMessageW(h, IPM_SETRANGE, 4, MAKEIPRANGE(1, 10)));
	CHECK(!SendMessageW(h, IPM_SETRANGE, 0, MAKEIPRANGE(9, 1)));
	SendMessageW(h, IPM_GETADDRESS, 0, (LPARAM)&ip);
	CHECK_EQ(FOURTH_IPADDRESS(ip), 10);
	SendMessageW(h, IPM_CLEARADDRESS, 0, 0);
	/* Typing: digits, a dot to move on, a full field moving on. */
	ncodes = 0;
	SendMessageW(h, IPM_SETFOCUS, 0, 0);
	SendMessageW(h, WM_CHAR, '1', 0);
	SendMessageW(h, WM_CHAR, '3', 0);
	SendMessageW(h, WM_CHAR, '.', 0);
	SendMessageW(h, WM_CHAR, '2', 0);
	SendMessageW(h, WM_CHAR, '5', 0);
	SendMessageW(h, WM_CHAR, '5', 0);
	SendMessageW(h, WM_CHAR, '7', 0);
	CHECK_EQ(seen(IPN_FIELDCHANGED), 2);
	CHECK_EQ(SendMessageW(h, IPM_GETADDRESS, 0, (LPARAM)&ip), 3);
	CHECK_EQ(ip, (DWORD)MAKEIPADDRESS(31, 255, 7, 0));
	SetWindowTextA(h, "10.0..4");
	CHECK_EQ(SendMessageW(h, IPM_GETADDRESS, 0, (LPARAM)&ip), 3);
	CHECK_EQ(ip, (DWORD)MAKEIPADDRESS(10, 0, 0, 4));
	GetWindowTextA(h, text, 32);
	CHECK(!strcmp(text, "10.0..4"));
	CHECK(DestroyWindow(h));

	/* Pagers: the parent sizes the child, the position is clamped. */
	h = make("SysPager", PGS_HORZ, 100, 30);
	HWND child = CreateWindowExW_(h);
	CHECK(child != NULL);
	SendMessageW(h, PGM_SETCHILD, 0, (LPARAM)child);
	RECT rc;
	GetWindowRect(child, &rc);
	CHECK(rc.right - rc.left == 500 && rc.bottom - rc.top == 30);
	CHECK_EQ(SendMessageW(h, PGM_GETBUTTONSTATE, 0, PGB_TOPORLEFT),
		 PGF_INVISIBLE);
	CHECK_EQ(SendMessageW(h, PGM_GETBUTTONSTATE, 0, PGB_BOTTOMORRIGHT),
		 PGF_NORMAL);
	SendMessageW(h, PGM_SETPOS, 0, 1000);
	CHECK_EQ(SendMessageW(h, PGM_GETPOS, 0, 0), 400);
	CHECK_EQ(SendMessageW(h, PGM_GETBUTTONSTATE, 0, PGB_BOTTOMORRIGHT),
		 PGF_INVISIBLE);
	CHECK_EQ(SendMessageW(h, PGM_SETBORDER, 0, 5), 0);
	CHECK_EQ(SendMessageW(h, PGM_GETBORDER, 0, 0), 5);
	CHECK_EQ(SendMessageW(h, PGM_GETPOS, 0, 0), 400);	/* max is 410 */
	SendMessageW(h, PGM_SETPOS, 0, 1000);
	CHECK_EQ(SendMessageW(h, PGM_GETPOS, 0, 0), 410);
	CHECK_EQ(SendMessageW(h, PGM_SETBUTTONSIZE, 0, 20), 12);
	CHECK_EQ(SendMessageW(h, PGM_GETBUTTONSIZE, 0, 0), 20);
	CHECK_EQ((COLORREF)SendMessageW(h, PGM_SETBKCOLOR, 0, RGB(1, 2, 3)) !=
		 RGB(1, 2, 3), 1);
	CHECK_EQ(SendMessageW(h, PGM_GETBKCOLOR, 0, 0), RGB(1, 2, 3));
	CHECK(DestroyWindow(h));

	/* The native font control keeps its font. */
	h = make("NativeFontCtl", NFS_ALL, 10, 10);
	SendMessageW(h, WM_SETFONT, (WPARAM)0x1234, 0);
	CHECK_EQ(SendMessageW(h, WM_GETFONT, 0, 0), 0x1234);
	CHECK(DestroyWindow(h));
	(void)buf;
}


static SYSTEMTIME day(int y, int m, int d)
{
	SYSTEMTIME st;
	memset(&st, 0, sizeof(st));
	st.wYear = (WORD)y;
	st.wMonth = (WORD)m;
	st.wDay = (WORD)d;
	return st;
}

static void test_date_controls(void)
{
	USER(LRESULT, SendMessageW, HWND, UINT, WPARAM, LPARAM);
	USER(LRESULT, SendMessageA, HWND, UINT, WPARAM, LPARAM);
	USER(BOOL, DestroyWindow, HWND);
	USER(int, GetWindowTextA, HWND, LPSTR, int);
	SYSTEMTIME st, r[2];
	char text[64];

	/* The month calendar, one day selected. */
	HWND h = make("SysMonthCal32", MCS_DAYSTATE, 200, 200);
	CHECK(h != NULL);
	CHECK(SendMessageW(h, MCM_GETCURSEL, 0, (LPARAM)&st));
	CHECK(st.wYear >= 2000);
	st = day(2024, 2, 29);
	ncodes = 0;
	CHECK(SendMessageW(h, MCM_SETCURSEL, 0, (LPARAM)&st));
	CHECK_EQ(seen(MCN_GETDAYSTATE), 1);	/* a new month */
	CHECK(last_daystate_start.wMonth == 1 && last_daystate_start.wDay == 28);
	memset(&st, 0, sizeof(st));
	CHECK(SendMessageW(h, MCM_GETCURSEL, 0, (LPARAM)&st));
	CHECK(st.wYear == 2024 && st.wMonth == 2 && st.wDay == 29 &&
	      st.wDayOfWeek == 4);
	st = day(2023, 2, 29);
	CHECK(!SendMessageW(h, MCM_SETCURSEL, 0, (LPARAM)&st));
	CHECK(!SendMessageW(h, MCM_SETSELRANGE, 0, (LPARAM)r));

	/* Ranges keep the selection inside. */
	r[0] = day(2024, 3, 5);
	r[1] = day(2024, 6, 1);
	CHECK(SendMessageW(h, MCM_SETRANGE, GDTR_MIN | GDTR_MAX, (LPARAM)r));
	CHECK(SendMessageW(h, MCM_GETCURSEL, 0, (LPARAM)&st) && st.wMonth == 3 &&
	      st.wDay == 5);
	memset(r, 0, sizeof(r));
	CHECK_EQ(SendMessageW(h, MCM_GETRANGE, 0, (LPARAM)r), GDTR_MIN | GDTR_MAX);
	CHECK(r[1].wMonth == 6);
	st = day(2024, 7, 1);
	CHECK(!SendMessageW(h, MCM_SETCURSEL, 0, (LPARAM)&st));
	SendMessageW(h, MCM_SETRANGE, 0, 0);

	/* The grid: March 2024 starts on a Friday; Sunday first. */
	st = day(2024, 3, 15);
	SendMessageW(h, MCM_SETCURSEL, 0, (LPARAM)&st);
	CHECK_EQ(SendMessageW(h, MCM_GETMONTHRANGE, GMR_VISIBLE, (LPARAM)r), 1);
	CHECK(r[0].wDay == 1 && r[1].wDay == 31);
	CHECK_EQ(SendMessageW(h, MCM_GETMONTHRANGE, GMR_DAYSTATE, (LPARAM)r), 3);
	CHECK(r[0].wMonth == 2 && r[0].wDay == 25 && r[1].wMonth == 4 &&
	      r[1].wDay == 6);
	MCHITTESTINFO hi;
	memset(&hi, 0, sizeof(hi));
	hi.cbSize = MCHITTESTINFO_V1_SIZE;
	hi.pt.x = 5 * 23 + 3;	/* Friday */
	hi.pt.y = 20 + 16 + 3;	/* the first week */
	CHECK_EQ(SendMessageW(h, MCM_HITTEST, 0, (LPARAM)&hi), MCHT_CALENDARDATE);
	CHECK(hi.st.wMonth == 3 && hi.st.wDay == 1);
	hi.pt.x = 3;
	CHECK_EQ(SendMessageW(h, MCM_HITTEST, 0, (LPARAM)&hi),
		 MCHT_CALENDARDATEPREV);
	hi.pt.y = 5;
	hi.pt.x = 80;
	CHECK_EQ(SendMessageW(h, MCM_HITTEST, 0, (LPARAM)&hi), MCHT_TITLEBK);
	hi.pt.x = 500;
	CHECK_EQ(SendMessageW(h, MCM_HITTEST, 0, (LPARAM)&hi), MCHT_NOWHERE);
	/* Clicking a day selects it. */
	ncodes = 0;
	SendMessageW(h, WM_LBUTTONDOWN, 0, MAKELPARAM(2 * 23 + 3, 20 + 16 + 16 + 3));
	CHECK(seen(MCN_SELCHANGE) == 1 && seen(MCN_SELECT) == 1);
	CHECK(last_sel.stSelStart.wDay == 5 && last_sel.stSelEnd.wDay == 5);
	/* First day of the week. */
	CHECK_EQ(SendMessageW(h, MCM_GETFIRSTDAYOFWEEK, 0, 0), MAKELONG(6, 0));
	CHECK_EQ(SendMessageW(h, MCM_SETFIRSTDAYOFWEEK, 0, 0), MAKELONG(6, 0));
	CHECK_EQ(SendMessageW(h, MCM_GETFIRSTDAYOFWEEK, 0, 0), MAKELONG(0, 1));
	SendMessageW(h, MCM_GETMONTHRANGE, GMR_DAYSTATE, (LPARAM)r);
	CHECK(r[0].wMonth == 2 && r[0].wDay == 26);	/* a Monday */
	/* Today, colours, sizes. */
	st = day(2020, 1, 2);
	SendMessageW(h, MCM_SETTODAY, 0, (LPARAM)&st);
	memset(&st, 0, sizeof(st));
	CHECK(SendMessageW(h, MCM_GETTODAY, 0, (LPARAM)&st) &&
	      st.wYear == 2020 && st.wDay == 2);
	CHECK(SendMessageW(h, MCM_SETCOLOR, MCSC_TEXT, RGB(1, 2, 3)) != -1);
	CHECK_EQ(SendMessageW(h, MCM_GETCOLOR, MCSC_TEXT, 0), RGB(1, 2, 3));
	CHECK_EQ(SendMessageW(h, MCM_GETCOLOR, 9, 0), -1);
	RECT rc;
	CHECK(SendMessageW(h, MCM_GETMINREQRECT, 0, (LPARAM)&rc) &&
	      rc.right > 0 && rc.bottom > 0);
	MONTHDAYSTATE ds[3] = { 1, 2, 4 };
	CHECK(SendMessageW(h, MCM_SETDAYSTATE, 3, (LPARAM)ds));
	CHECK(DestroyWindow(h));

	/* A multiple selection. */
	h = make("SysMonthCal32", MCS_MULTISELECT, 200, 200);
	CHECK(!SendMessageW(h, MCM_GETCURSEL, 0, (LPARAM)&st));
	CHECK_EQ(SendMessageW(h, MCM_GETMAXSELCOUNT, 0, 0), 7);
	r[0] = day(2024, 5, 10);
	r[1] = day(2024, 5, 14);
	CHECK(SendMessageW(h, MCM_SETSELRANGE, 0, (LPARAM)r));
	memset(r, 0, sizeof(r));
	CHECK(SendMessageW(h, MCM_GETSELRANGE, 0, (LPARAM)r));
	CHECK(r[0].wDay == 10 && r[1].wDay == 14);
	CHECK(SendMessageW(h, MCM_SETMAXSELCOUNT, 3, 0));
	r[0] = day(2024, 5, 10);
	r[1] = day(2024, 5, 14);
	CHECK(!SendMessageW(h, MCM_SETSELRANGE, 0, (LPARAM)r));
	CHECK(DestroyWindow(h));

	/* The date and time picker. */
	h = make("SysDateTimePick32", DTS_SHOWNONE, 150, 20);
	CHECK(h != NULL);
	CHECK_EQ(SendMessageW(h, DTM_GETSYSTEMTIME, 0, (LPARAM)&st), GDT_VALID);
	st = day(2024, 7, 4);
	st.wHour = 15;
	st.wMinute = 5;
	st.wSecond = 9;
	CHECK(SendMessageW(h, DTM_SETSYSTEMTIME, GDT_VALID, (LPARAM)&st));
	GetWindowTextA(h, text, 64);
	CHECK(!strcmp(text, "7/4/2024"));
	CHECK(SendMessageA(h, DTM_SETFORMATA, 0,
			   (LPARAM)"dddd, MMM dd yy 'at' hh:mm:ss tt"));
	GetWindowTextA(h, text, 64);
	CHECK(!strcmp(text, "Thursday, Jul 04 24 at 03:05:09 PM"));
	SendMessageW(h, DTM_SETFORMATW, 0, (LPARAM)u"HH'h'mm");
	GetWindowTextA(h, text, 64);
	CHECK(!strcmp(text, "15h05"));
	CHECK(SendMessageW(h, DTM_SETSYSTEMTIME, GDT_NONE, 0));
	CHECK_EQ(SendMessageW(h, DTM_GETSYSTEMTIME, 0, (LPARAM)&st), GDT_NONE);
	CHECK_EQ(SendMessageW(h, DTM_GETSYSTEMTIME, 0, 0), GDT_ERROR);
	st = day(2024, 13, 1);
	CHECK(!SendMessageW(h, DTM_SETSYSTEMTIME, GDT_VALID, (LPARAM)&st));
	r[0] = day(2024, 1, 1);
	r[1] = day(2024, 12, 31);
	CHECK(SendMessageW(h, DTM_SETRANGE, GDTR_MIN | GDTR_MAX, (LPARAM)r));
	st = day(2025, 1, 1);
	CHECK(!SendMessageW(h, DTM_SETSYSTEMTIME, GDT_VALID, (LPARAM)&st));
	CHECK_EQ(SendMessageW(h, DTM_GETRANGE, 0, (LPARAM)r), GDTR_MIN | GDTR_MAX);
	CHECK(!SendMessageW(h, DTM_GETMONTHCAL, 0, 0));
	CHECK(DestroyWindow(h));
	h = make("SysDateTimePick32", 0, 150, 20);
	CHECK(!SendMessageW(h, DTM_SETSYSTEMTIME, GDT_NONE, 0));
	h = (DestroyWindow(h), make("SysDateTimePick32", DTS_TIMEFORMAT, 150, 20));
	st = day(2024, 7, 4);
	st.wHour = 0;
	st.wMinute = 30;
	SendMessageW(h, DTM_SETSYSTEMTIME, GDT_VALID, (LPARAM)&st);
	GetWindowTextA(h, text, 64);
	CHECK(!strcmp(text, "12:30:00 AM"));
	CHECK(DestroyWindow(h));
}


/* How wide text is in the default font, as the controls measure it. */
static int comctl_width(const WCHAR *t)
{
	GDI(HDC, CreateCompatibleDC, HDC);
	GDI(BOOL, GetTextExtentPoint32W, HDC, LPCWSTR, int, SIZE *);
	GDI(BOOL, DeleteDC, HDC);
	SIZE sz = { 0, 0 };
	int n = 0;
	while (t[n])
		n++;
	HDC dc = CreateCompatibleDC(NULL);
	GetTextExtentPoint32W(dc, t, n, &sz);
	DeleteDC(dc);
	return sz.cx;
}

/* Handlers that change or destroy a control while it notifies. */

static HWND hook_target;
static int hook_count;

static LRESULT pop_again(const NMHDR *hdr)
{
	USER(LRESULT, SendMessageW, HWND, UINT, WPARAM, LPARAM);
	if (hdr->code == TTN_POP) {
		hook_count++;
		SendMessageW(hook_target, TTM_POP, 0, 0);	/* no second one */
	}
	return 0;
}

static LRESULT destroy_on(const NMHDR *hdr)
{
	USER(BOOL, DestroyWindow, HWND);
	if (hook_target && hdr->hwndFrom == hook_target &&
	    hdr->code == (UINT)hook_count) {
		HWND h = hook_target;
		hook_target = NULL;
		DestroyWindow(h);
	}
	return 0;
}

static LRESULT delete_tab(const NMHDR *hdr)
{
	USER(LRESULT, SendMessageW, HWND, UINT, WPARAM, LPARAM);
	if (hdr->code == TCN_SELCHANGING)
		SendMessageW(hook_target, TCM_DELETEITEM, 0, 0);
	return 0;
}

/* An IPN_FIELDCHANGED handler that moves the focus itself. */
static LRESULT refocus_ip(const NMHDR *hdr)
{
	USER(LRESULT, SendMessageW, HWND, UINT, WPARAM, LPARAM);
	if (hdr->code == IPN_FIELDCHANGED) {
		if (hook_count++ == 0)
			SendMessageW(hook_target, IPM_SETFOCUS, 3, 0);
	}
	return 0;
}

static LRESULT STUB_WINAPI nested_sub(HWND h, UINT m, WPARAM wp, LPARAM lp,
				      UINT_PTR id, DWORD_PTR ref)
{
	FN(LRESULT, DefSubclassProc, HWND, UINT, WPARAM, LPARAM);
	USER(LRESULT, SendMessageW, HWND, UINT, WPARAM, LPARAM);
	if (m == WM_USER + 1)
		SendMessageW(h, WM_USER + 2, 0, 0);
	return DefSubclassProc(h, m, wp, lp);
}

static LRESULT STUB_WINAPI removing_sub(HWND h, UINT m, WPARAM wp, LPARAM lp,
					UINT_PTR id, DWORD_PTR ref)
{
	FN(LRESULT, DefSubclassProc, HWND, UINT, WPARAM, LPARAM);
	FN(BOOL, RemoveWindowSubclass, HWND, SUBCLASSPROC, UINT_PTR);
	if (m == WM_USER + 2)
		RemoveWindowSubclass(h, removing_sub, id);
	if (m == WM_USER + 1)
		hook_count += 100;	/* must not run: removed by then */
	return DefSubclassProc(h, m, wp, lp);
}

static void test_reentrancy(void)
{
	USER(LRESULT, SendMessageW, HWND, UINT, WPARAM, LPARAM);
	USER(LRESULT, SendMessageA, HWND, UINT, WPARAM, LPARAM);
	USER(BOOL, DestroyWindow, HWND);
	USER(BOOL, IsWindow, HWND);
	FN(BOOL, SetWindowSubclass, HWND, SUBCLASSPROC, UINT_PTR, DWORD_PTR);
	FN(BOOL, GetWindowSubclass, HWND, SUBCLASSPROC, UINT_PTR, DWORD_PTR *);

	/* A subclass removed inside a nested message is skipped, and
	 * freed only once the outer dispatch is done. */
	HWND h = make("tooltips_class32", 0, 10, 10);
	CHECK(SetWindowSubclass(h, removing_sub, 1, 0));
	CHECK(SetWindowSubclass(h, nested_sub, 2, 0));
	hook_count = 0;
	SendMessageW(h, WM_USER + 1, 0, 0);
	CHECK_EQ(hook_count, 0);
	CHECK(!GetWindowSubclass(h, removing_sub, 1, NULL));
	CHECK(GetWindowSubclass(h, nested_sub, 2, NULL));
	SendMessageW(h, WM_USER + 1, 0, 0);
	CHECK(DestroyWindow(h));

	/* A TTN_POP handler that pops again gets one TTN_POP. */
	h = make("tooltips_class32", 0, 10, 10);
	TTTOOLINFOW ti;
	memset(&ti, 0, sizeof(ti));
	ti.cbSize = sizeof(ti);
	ti.hwnd = parent;
	ti.uId = 1;
	set_rect(&ti.rect, 0, 0, 50, 20);
	ti.lpszText = (LPWSTR)u"Tip";
	CHECK(SendMessageW(h, TTM_ADDTOOLW, 0, (LPARAM)&ti));
	MSG m;
	memset(&m, 0, sizeof(m));
	m.hwnd = parent;
	m.message = WM_MOUSEMOVE;
	m.lParam = MAKELPARAM(5, 5);
	SendMessageW(h, TTM_RELAYEVENT, 0, (LPARAM)&m);
	hook_target = h;
	hook_count = 0;
	notify_hook = pop_again;
	SendMessageW(h, TTM_POP, 0, 0);
	notify_hook = NULL;
	CHECK_EQ(hook_count, 1);
	/* A TTN_SHOW handler that destroys the tooltip. */
	hook_count = (int)TTN_SHOW;
	notify_hook = destroy_on;
	SendMessageW(h, TTM_RELAYEVENT, 0, (LPARAM)&m);
	notify_hook = NULL;
	CHECK(!IsWindow(h));

	/* A tab control whose TCN_SELCHANGING handler deletes a tab: the
	 * click no longer means that tab, so nothing is selected. */
	h = make("SysTabControl32", TCS_FIXEDWIDTH, 200, 100);
	TCITEMW it;
	memset(&it, 0, sizeof(it));
	it.mask = TCIF_TEXT;
	it.pszText = (LPWSTR)u"one";
	for (int i = 0; i < 3; i++)
		SendMessageW(h, TCM_INSERTITEMW, i, (LPARAM)&it);
	RECT rc;
	CHECK(SendMessageW(h, TCM_GETITEMRECT, 2, (LPARAM)&rc));
	hook_target = h;
	notify_hook = delete_tab;
	SendMessageW(h, WM_LBUTTONDOWN, 0, MAKELPARAM(rc.left + 2, 5));
	notify_hook = NULL;
	CHECK_EQ(SendMessageW(h, TCM_GETITEMCOUNT, 0, 0), 2);
	/* The handler deleted the selected tab; the click selected none. */
	CHECK_EQ(SendMessageW(h, TCM_GETCURSEL, 0, 0), -1);
	/* With TCM_SETCURSEL the button state follows the selection. */
	CHECK(DestroyWindow(h));
	h = make("SysTabControl32", TCS_BUTTONS, 200, 100);
	for (int i = 0; i < 2; i++)
		SendMessageW(h, TCM_INSERTITEMW, i, (LPARAM)&it);
	SendMessageW(h, TCM_SETCURSEL, 1, 0);
	it.mask = TCIF_STATE;
	it.dwStateMask = TCIS_BUTTONPRESSED;
	CHECK(SendMessageW(h, TCM_GETITEMW, 1, (LPARAM)&it) &&
	      it.dwState == TCIS_BUTTONPRESSED);
	CHECK(SendMessageW(h, TCM_GETITEMW, 0, (LPARAM)&it) && !it.dwState);
	/* TCM_SETCURSEL(-1) clears the focus with the selection. */
	SendMessageW(h, TCM_SETCURSEL, (WPARAM)-1, 0);
	CHECK_EQ(SendMessageW(h, TCM_GETCURSEL, 0, 0), -1);
	CHECK_EQ(SendMessageW(h, TCM_GETCURFOCUS, 0, 0), -1);
	CHECK(DestroyWindow(h));

	/* A TTN_POP handler that destroys a tracking tooltip as
	 * TTM_TRACKACTIVATE(FALSE) hides it. */
	h = make("tooltips_class32", 0, 10, 10);
	memset(&ti, 0, sizeof(ti));
	ti.cbSize = sizeof(ti);
	ti.uFlags = TTF_TRACK;
	ti.hwnd = parent;
	ti.uId = 2;
	ti.lpszText = (LPWSTR)u"Tracked";
	CHECK(SendMessageW(h, TTM_ADDTOOLW, 0, (LPARAM)&ti));
	SendMessageW(h, TTM_TRACKACTIVATE, TRUE, (LPARAM)&ti);
	hook_target = h;
	hook_count = (int)TTN_POP;
	notify_hook = destroy_on;
	SendMessageW(h, TTM_TRACKACTIVATE, FALSE, (LPARAM)&ti);
	notify_hook = NULL;
	CHECK(!IsWindow(h));

	/* A toolbar destroyed by its TBN_HOTITEMCHANGE handler. */
	h = make("ToolbarWindow32", CCS_NORESIZE, 200, 30);
	SendMessageW(h, TB_BUTTONSTRUCTSIZE, sizeof(TBBUTTON), 0);
	TBBUTTON tb;
	memset(&tb, 0, sizeof(tb));
	tb.idCommand = 7;
	tb.fsState = TBSTATE_ENABLED;
	tb.iString = -1;
	CHECK(SendMessageW(h, TB_ADDBUTTONSW, 1, (LPARAM)&tb));
	hook_target = h;
	hook_count = (int)TBN_HOTITEMCHANGE;
	notify_hook = destroy_on;
	SendMessageW(h, TB_SETHOTITEM, 0, 0);
	notify_hook = NULL;
	CHECK(!IsWindow(h));

	/* An IPN_FIELDCHANGED handler that moves the focus elsewhere: one
	 * notification, and the focus goes where the handler put it. */
	h = make("SysIPAddress32", 0, 120, 20);
	hook_target = h;
	hook_count = 0;
	notify_hook = refocus_ip;
	SendMessageW(h, IPM_SETFOCUS, 1, 0);
	notify_hook = NULL;
	CHECK_EQ(hook_count, 1);
	SendMessageW(h, WM_CHAR, '7', 0);
	DWORD addr = 0;
	CHECK_EQ(SendMessageW(h, IPM_GETADDRESS, 0, (LPARAM)&addr), 1);
	CHECK_EQ(addr, 7);
	CHECK(DestroyWindow(h));

	/* A rebar destroyed by its RBN_DELETINGBAND handler. */
	h = make("ReBarWindow32", CCS_NORESIZE, 200, 30);
	REBARBANDINFOW bi;
	memset(&bi, 0, sizeof(bi));
	bi.cbSize = sizeof(bi);
	bi.fMask = RBBIM_ID | RBBIM_SIZE;
	bi.cx = 50;
	CHECK(SendMessageW(h, RB_INSERTBANDW, -1, (LPARAM)&bi));
	CHECK(SendMessageW(h, RB_INSERTBANDW, -1, (LPARAM)&bi));
	/* RB_SIZETORECT lays the bands out in the rectangle's width. */
	RECT want;
	set_rect(&want, 0, 0, 300, 30);
	SendMessageW(h, RB_SIZETORECT, 0, (LPARAM)&want);
	CHECK(SendMessageW(h, RB_GETRECT, 1, (LPARAM)&rc) && rc.right == 300);
	hook_target = h;
	hook_count = (int)RBN_DELETINGBAND;
	notify_hook = destroy_on;
	CHECK(!SendMessageW(h, RB_DELETEBAND, 0, 0));
	notify_hook = NULL;
	CHECK(!IsWindow(h));

	/* ComboBoxEx: an ANSI caller gets ANSI text, and a CBEN_DELETEITEM
	 * handler that destroys the control. */
	h = make("ComboBoxEx32", 0, 200, 100);
	COMBOBOXEXITEMW ci;
	memset(&ci, 0, sizeof(ci));
	ci.mask = CBEIF_TEXT;
	ci.iItem = -1;
	ci.pszText = (LPWSTR)u"alpha";
	CHECK_EQ(SendMessageW(h, CBEM_INSERTITEMW, 0, (LPARAM)&ci), 0);
	CHECK_EQ(SendMessageA(h, CB_GETLBTEXTLEN, 0, 0), 5);
	char text[16];
	memset(text, 'x', sizeof(text));
	CHECK_EQ(SendMessageA(h, CB_GETLBTEXT, 0, (LPARAM)text), 5);
	CHECK(!strcmp(text, "alpha"));
	CHECK_EQ(SendMessageA(h, CB_FINDSTRINGEXACT, (WPARAM)-1,
			      (LPARAM)"ALPHA"), 0);
	hook_target = h;
	hook_count = (int)CBEN_DELETEITEM;
	notify_hook = destroy_on;
	SendMessageW(h, CB_RESETCONTENT, 0, 0);
	notify_hook = NULL;
	CHECK(!IsWindow(h));

	/* SysLink: an unterminated tag is plain text. */
	{
		USER(HWND, CreateWindowExW, DWORD, LPCWSTR, LPCWSTR, DWORD, int,
		     int, int, int, HWND, HMENU, HINSTANCE, LPVOID);
		h = CreateWindowExW(0, w("SysLink"), w("go <a href=\"x\""),
				    WS_CHILD, 0, 0, 300, 20, parent, NULL,
				    NULL, NULL);
	}
	CHECK(h != NULL);
	LITEM li;
	memset(&li, 0, sizeof(li));
	li.mask = LIF_ITEMINDEX;
	CHECK(!SendMessageW(h, LM_GETITEM, 0, (LPARAM)&li));
	CHECK(DestroyWindow(h));
}

static void test_comboex_link(void)
{
	USER(LRESULT, SendMessageW, HWND, UINT, WPARAM, LPARAM);
	USER(LRESULT, SendMessageA, HWND, UINT, WPARAM, LPARAM);
	USER(BOOL, DestroyWindow, HWND);
	USER(HWND, CreateWindowExW, DWORD, LPCWSTR, LPCWSTR, DWORD, int, int,
	     int, int, HWND, HMENU, HINSTANCE, LPVOID);
	USER(BOOL, SetWindowTextW, HWND, LPCWSTR);
	USER(int, GetWindowTextA, HWND, LPSTR, int);
	WCHAR buf[64];

	/* ComboBoxEx. */
	HWND h = make("ComboBoxEx32", 0, 150, 100);
	CHECK(h != NULL);
	COMBOBOXEXITEMW it;
	memset(&it, 0, sizeof(it));
	it.mask = CBEIF_TEXT | CBEIF_IMAGE | CBEIF_LPARAM | CBEIF_INDENT;
	static const char *const names[3] = { "Red", "Green", "blue" };
	ncodes = 0;
	for (int i = 0; i < 3; i++) {
		it.iItem = -1;
		it.pszText = w(names[i]);
		it.iImage = i;
		it.lParam = 10 + i;
		it.iIndent = i;
		CHECK_EQ(SendMessageW(h, CBEM_INSERTITEMW, 0, (LPARAM)&it), i);
	}
	CHECK(seen(CBEN_INSERTITEM) == 3 && last_cbe.lParam == 12);
	COMBOBOXEXITEMA ia;
	memset(&ia, 0, sizeof(ia));
	ia.mask = CBEIF_TEXT;
	ia.iItem = 0;
	ia.pszText = (LPSTR)"Black";
	CHECK_EQ(SendMessageA(h, CBEM_INSERTITEMA, 0, (LPARAM)&ia), 0);
	it.mask = CBEIF_TEXT | CBEIF_LPARAM;
	it.iItem = 4;
	it.pszText = LPSTR_TEXTCALLBACKW;
	it.lParam = 7;
	CHECK_EQ(SendMessageW(h, CBEM_INSERTITEMW, 0, (LPARAM)&it), 4);
	CHECK_EQ(SendMessageW(h, CB_GETCOUNT, 0, 0), 5);

	memset(&it, 0, sizeof(it));
	it.mask = CBEIF_TEXT | CBEIF_IMAGE | CBEIF_LPARAM | CBEIF_INDENT;
	it.iItem = 2;
	it.pszText = buf;
	it.cchTextMax = 64;
	CHECK(SendMessageW(h, CBEM_GETITEMW, 0, (LPARAM)&it));
	CHECK(!strcmp(a(buf), "Green") && it.iImage == 1 && it.lParam == 11 &&
	      it.iIndent == 1);
	it.iItem = 4;
	CHECK(SendMessageW(h, CBEM_GETITEMW, 0, (LPARAM)&it));
	CHECK(!strcmp(a(buf), "cb7"));
	CHECK_EQ(SendMessageW(h, CB_GETLBTEXT, 4, (LPARAM)buf), 3);
	CHECK_EQ(SendMessageW(h, CB_GETLBTEXTLEN, 1, 0), 3);
	CHECK_EQ(SendMessageW(h, CB_GETITEMDATA, 3, 0), 12);
	CHECK(SendMessageW(h, CB_SETITEMDATA, 3, 99));
	CHECK_EQ(SendMessageW(h, CB_GETITEMDATA, 3, 0), 99);
	CHECK_EQ(SendMessageW(h, CB_GETITEMDATA, 9, 0), CB_ERR);

	/* Selection and finding. */
	CHECK_EQ(SendMessageW(h, CB_GETCURSEL, 0, 0), -1);
	CHECK_EQ(SendMessageW(h, CB_SETCURSEL, 2, 0), 2);
	it.iItem = -1;	/* the selection */
	it.mask = CBEIF_TEXT;
	CHECK(SendMessageW(h, CBEM_GETITEMW, 0, (LPARAM)&it));
	CHECK(!strcmp(a(buf), "Green"));
	ncommands = 0;
	SendMessageW(h, WM_KEYDOWN, 0x28, 0);	/* down */
	CHECK(SendMessageW(h, CB_GETCURSEL, 0, 0) == 3 &&
	      seen_command(CBN_SELCHANGE) == 1);
	CHECK_EQ(SendMessageW(h, CB_FINDSTRINGEXACT, -1, (LPARAM)u"BLUE"), 3);
	CHECK_EQ(SendMessageW(h, CB_FINDSTRINGEXACT, -1, (LPARAM)u"Gre"), CB_ERR);
	CHECK_EQ(SendMessageW(h, CB_FINDSTRING, -1, (LPARAM)u"Gre"), 2);
	CHECK_EQ(SendMessageW(h, CB_FINDSTRING, 2, (LPARAM)u"b"), 3);
	CHECK_EQ(SendMessageW(h, CBEM_SETEXTENDEDSTYLE, 0,
			      CBES_EX_CASESENSITIVE), 0);
	CHECK_EQ(SendMessageW(h, CB_FINDSTRINGEXACT, -1, (LPARAM)u"BLUE"),
		 CB_ERR);
	CHECK_EQ(SendMessageW(h, CB_ADDSTRING, 0, (LPARAM)u"x"), CB_ERR);
	CHECK(SendMessageW(h, CB_SHOWDROPDOWN, TRUE, 0));
	CHECK(SendMessageW(h, CB_GETDROPPEDSTATE, 0, 0));
	CHECK_EQ(seen_command(CBN_DROPDOWN), 1);
	CHECK(SendMessageW(h, CBEM_GETCOMBOCONTROL, 0, 0) == (LRESULT)h);

	/* Deleting (CBEN_DELETEITEM), set item. */
	ncodes = 0;
	CHECK_EQ(SendMessageW(h, CBEM_DELETEITEM, 0, 0), 4);
	CHECK(seen(CBEN_DELETEITEM) == 1);
	CHECK_EQ(SendMessageW(h, CB_GETCURSEL, 0, 0), 2);
	it.mask = CBEIF_TEXT;
	it.iItem = 0;
	it.pszText = (LPWSTR)u"Crimson";
	CHECK(SendMessageW(h, CBEM_SETITEMW, 0, (LPARAM)&it));
	CHECK_EQ(SendMessageW(h, CB_GETLBTEXT, 0, (LPARAM)buf), 7);
	SendMessageW(h, CB_RESETCONTENT, 0, 0);
	CHECK_EQ(seen(CBEN_DELETEITEM), 5);
	CHECK_EQ(SendMessageW(h, CB_GETCOUNT, 0, 0), 0);
	CHECK(DestroyWindow(h));

	/* SysLink: markup, items, hit test, clicks. */
	h = CreateWindowExW(0, w("SysLink"),
			    w("See <a href=\"http://a.example/\" id=\"one\">"
			      "this</a> or <A id='two'>that</A>."),
			    WS_CHILD, 0, 0, 300, 20, parent, NULL, NULL, NULL);
	CHECK(h != NULL);
	link_window = h;
	LITEM li;
	memset(&li, 0, sizeof(li));
	li.mask = LIF_ITEMINDEX | LIF_STATE | LIF_ITEMID | LIF_URL;
	li.iLink = 0;
	li.stateMask = LIS_ENABLED | LIS_FOCUSED;
	CHECK(SendMessageW(h, LM_GETITEM, 0, (LPARAM)&li));
	CHECK(!strcmp(a(li.szUrl), "http://a.example/") &&
	      !strcmp(a(li.szID), "one") &&
	      li.state == (LIS_ENABLED | LIS_FOCUSED));
	memset(&li, 0, sizeof(li));
	li.mask = LIF_ITEMID | LIF_STATE;
	memcpy(li.szID, u"two", 8);
	li.stateMask = ~0u;
	CHECK(SendMessageW(h, LM_GETITEM, 0, (LPARAM)&li));
	CHECK(li.iLink == 1 && li.szUrl[0] == 0 && li.state == LIS_ENABLED);
	li.mask = LIF_ITEMINDEX;
	li.iLink = 2;
	CHECK(!SendMessageW(h, LM_GETITEM, 0, (LPARAM)&li));
	/* Set a URL and the visited state. */
	memset(&li, 0, sizeof(li));
	li.mask = LIF_ITEMINDEX | LIF_URL | LIF_STATE;
	li.iLink = 1;
	memcpy(li.szUrl, u"x:y", 8);
	li.state = LIS_VISITED;
	li.stateMask = LIS_VISITED;
	CHECK(SendMessageW(h, LM_SETITEM, 0, (LPARAM)&li));
	memset(&li, 0, sizeof(li));
	li.mask = LIF_ITEMINDEX | LIF_URL | LIF_STATE;
	li.iLink = 1;
	li.stateMask = LIS_VISITED;
	CHECK(SendMessageW(h, LM_GETITEM, 0, (LPARAM)&li));
	CHECK(!strcmp(a(li.szUrl), "x:y") && li.state == LIS_VISITED);
	/* Hit test: the second link lies after "See this or ". */
	CHECK_EQ(SendMessageW(h, LM_GETIDEALHEIGHT, 0, 0), 16);
	int x0 = comctl_width(u"See this or "), x1 = comctl_width(u"See this or that");
	LHITTESTINFO hi;
	memset(&hi, 0, sizeof(hi));
	hi.pt.x = (x0 + x1) / 2;
	hi.pt.y = 5;
	CHECK(SendMessageW(h, LM_HITTEST, 0, (LPARAM)&hi));
	CHECK(hi.item.iLink == 1 && !strcmp(a(hi.item.szID), "two"));
	hi.pt.x = 1;
	CHECK(!SendMessageW(h, LM_HITTEST, 0, (LPARAM)&hi));
	last_link = -9;
	SendMessageW(h, WM_LBUTTONDOWN, 0, MAKELPARAM((x0 + x1) / 2, 5));
	CHECK_EQ(last_link, 1);
	last_link = -9;
	SendMessageW(h, WM_KEYDOWN, VK_RETURN, 0);
	CHECK_EQ(last_link, 1);
	SendMessageW(h, WM_KEYDOWN, VK_TAB, 0);
	last_link = -9;
	SendMessageW(h, WM_KEYDOWN, VK_RETURN, 0);
	CHECK_EQ(last_link, 0);
	/* New markup replaces the links; the window keeps the markup. */
	SetWindowTextW(h, w("<a>only</a>"));
	char text[64];
	GetWindowTextA(h, text, 64);
	CHECK(!strcmp(text, "<a>only</a>"));
	li.mask = LIF_ITEMINDEX;
	li.iLink = 1;
	CHECK(!SendMessageW(h, LM_GETITEM, 0, (LPARAM)&li));
	li.iLink = 0;
	CHECK(SendMessageW(h, LM_GETITEM, 0, (LPARAM)&li));
	link_window = NULL;
	CHECK(DestroyWindow(h));
}

static void test_list_controls(void)
{
	USER_WLP(Set, HWND, int, LONG_PTR);
	SetWindowLongPtrW(parent, GWLP_WNDPROC, (LONG_PTR)record_proc);
	test_header();
	test_listview();
	test_treeview();
	test_toolbar();
	test_tooltip();
	test_tab();
	test_rebar();
	test_small_controls();
	test_date_controls();
	test_comboex_link();
	test_reentrancy();
}

static void test_ordinals(void)
{
	static const struct {
		WORD ord;
		const char *name;
	} known[] = {
		{ 6, "CreateStatusWindowA" }, { 8, "CreateMappedBitmap" },
		{ 16, "CreateUpDownControl" }, { 17, "InitCommonControls" },
		{ 71, "Alloc" }, { 73, "Free" }, { 234, "Str_SetPtrA" },
		{ 236, "Str_SetPtrW" }, { 320, "DSA_Create" },
		{ 324, "DSA_InsertItem" }, { 328, "DPA_Create" },
		{ 334, "DPA_InsertPtr" }, { 338, "DPA_Sort" },
		{ 339, "DPA_Search" }, { 340, "DPA_CreateEx" },
		{ 341, "SendNotify" }, { 344, "TaskDialog" },
		{ 345, "TaskDialogIndirect" }, { 385, "DPA_EnumCallback" },
		{ 388, "DSA_DestroyCallback" }, { 390, "ImageList_SetColorTable" },
		{ 410, "SetWindowSubclass" }, { 413, "DefSubclassProc" },
		{ 421, "TextOutWrap" },
	};

	for (size_t i = 0; i < sizeof(known) / sizeof(known[0]); i++) {
		void *by_ord = mod_get_ordinal(cc, known[i].ord);
		void *by_name = mod_get_proc(cc, known[i].name);
		if (!by_name || by_ord != by_name)
			fprintf(stderr, "  ordinal %u: %s\n", known[i].ord,
				known[i].name);
		CHECK(by_name && by_ord == by_name);
	}
	CHECK(mod_get_ordinal(cc, 1) == NULL);
	CHECK(mod_get_proc(cc, "PropertySheet") ==
	      mod_get_proc(cc, "PropertySheetA"));
	CHECK(mod_get_proc(cc, "ImageList_LoadImage") ==
	      mod_get_proc(cc, "ImageList_LoadImageA"));
}

static void test_fixture(void)
{
	char path[4096];
	const char *dir = getenv("VFW_FIXTURES");

	snprintf(path, sizeof(path), "%s/pe_comctl.dll", dir ? dir : ".");
	struct module *m = mod_load(path, NULL);
	if (!m) {
		printf("pe_comctl.dll not built: skipping\n");
		return;
	}
	int (WINAPI *run)(void) = mod_get_proc(m, "comctl_run");
	CHECK(run != NULL);
	if (run) {
		int r = run();
		if (r)
			fprintf(stderr, "  comctl_run failed at check %d\n", r);
		CHECK_EQ(r, 0);
	}
	mod_free(m);
}

/* Flat scroll bars, over user32's scroll bar state. */
static void test_flatsb(void)
{
	FN(BOOL, InitializeFlatSB, HWND);
	FN(HRESULT, UninitializeFlatSB, HWND);
	FN(BOOL, FlatSB_GetScrollProp, HWND, INT, int *);
	FN(BOOL, FlatSB_SetScrollProp, HWND, UINT, INT_PTR, BOOL);
	FN(int, FlatSB_SetScrollInfo, HWND, int, LPSCROLLINFO, BOOL);
	FN(BOOL, FlatSB_GetScrollInfo, HWND, int, LPSCROLLINFO);
	FN(int, FlatSB_SetScrollPos, HWND, int, int, BOOL);
	FN(int, FlatSB_GetScrollPos, HWND, int);
	FN(BOOL, FlatSB_SetScrollRange, HWND, int, int, int, BOOL);
	FN(BOOL, FlatSB_GetScrollRange, HWND, int, int *, int *);
	FN(BOOL, FlatSB_ShowScrollBar, HWND, int, BOOL);
	FN(BOOL, FlatSB_EnableScrollBar, HWND, int, UINT);
	USER(HWND, CreateWindowExW, DWORD, LPCWSTR, LPCWSTR, DWORD, int, int,
	     int, int, HWND, HMENU, HINSTANCE, LPVOID);
	USER(BOOL, DestroyWindow, HWND);
	USER(int, GetSystemMetrics, int);

	HWND h = CreateWindowExW(0, w("STATIC"), NULL, WS_CHILD | WS_VSCROLL,
				 0, 0, 50, 50, parent, NULL, NULL, NULL);
	CHECK(h != NULL);
	int v = -1, lo = -1, hi = -1;
	CHECK(!FlatSB_GetScrollProp(h, WSB_PROP_CYVSCROLL, &v));
	CHECK(InitializeFlatSB(h) && InitializeFlatSB(h));
	CHECK(FlatSB_GetScrollProp(h, WSB_PROP_CYVSCROLL, &v) &&
	      v == GetSystemMetrics(20));
	CHECK(FlatSB_GetScrollProp(h, WSB_PROP_VSTYLE, &v) &&
	      v == FSB_REGULAR_MODE);
	CHECK(FlatSB_SetScrollProp(h, WSB_PROP_VSTYLE, FSB_FLAT_MODE, FALSE));
	CHECK(FlatSB_GetScrollProp(h, WSB_PROP_VSTYLE, &v) &&
	      v == FSB_FLAT_MODE);
	CHECK(!FlatSB_SetScrollProp(h, WSB_PROP_VSTYLE, 7, FALSE));
	CHECK(!FlatSB_SetScrollProp(h, WSB_PROP_WINSTYLE, 0, FALSE));
	CHECK(!FlatSB_SetScrollProp(h, 3, 0, FALSE));	/* two bits */
	CHECK(FlatSB_GetScrollProp(h, WSB_PROP_WINSTYLE, &v) &&
	      v == WS_VSCROLL);

	/* The default range, then a page that holds the position back. */
	CHECK(FlatSB_GetScrollRange(h, SB_VERT, &lo, &hi) && lo == 0 &&
	      hi == 100);
	CHECK(FlatSB_GetScrollRange(h, SB_CTL, &lo, &hi) && lo == 0 &&
	      hi == 0);
	SCROLLINFO si = { sizeof(si), SIF_ALL, 0, 99, 10, 95, 0 };
	CHECK_EQ(FlatSB_SetScrollInfo(h, SB_VERT, &si, FALSE), 90);
	memset(&si, 0, sizeof(si));
	si.cbSize = sizeof(si);
	si.fMask = SIF_ALL;
	CHECK(FlatSB_GetScrollInfo(h, SB_VERT, &si));
	CHECK(si.nMin == 0 && si.nMax == 99 && si.nPage == 10 &&
	      si.nPos == 90 && si.nTrackPos == 90);
	si.fMask = 0;
	CHECK(!FlatSB_GetScrollInfo(h, SB_VERT, &si));
	CHECK_EQ(FlatSB_SetScrollPos(h, SB_VERT, -5, FALSE), 90);
	CHECK_EQ(FlatSB_GetScrollPos(h, SB_VERT), 0);
	si = (SCROLLINFO){ sizeof(si), SIF_PAGE, 0, 0, 500, 0, 0 };
	FlatSB_SetScrollInfo(h, SB_VERT, &si, FALSE);	/* the whole range */
	si.fMask = SIF_PAGE;
	CHECK(FlatSB_GetScrollInfo(h, SB_VERT, &si) && si.nPage == 100);
	CHECK(FlatSB_SetScrollRange(h, SB_HORZ, 10, 20, FALSE));
	CHECK_EQ(FlatSB_GetScrollPos(h, SB_HORZ), 10);
	CHECK(!FlatSB_SetScrollRange(h, SB_HORZ, -0x7fffffff - 1, 0x7fffffff,
				     FALSE));
	CHECK(!FlatSB_GetScrollRange(h, 5, &lo, &hi));

	/* Shown and enabled. */
	CHECK(FlatSB_ShowScrollBar(h, SB_BOTH, TRUE));
	CHECK(FlatSB_GetScrollProp(h, WSB_PROP_WINSTYLE, &v) &&
	      v == (WS_HSCROLL | WS_VSCROLL));
	CHECK(FlatSB_ShowScrollBar(h, SB_VERT, FALSE));
	CHECK(FlatSB_GetScrollProp(h, WSB_PROP_WINSTYLE, &v) &&
	      v == WS_HSCROLL);
	CHECK(FlatSB_EnableScrollBar(h, SB_BOTH, ESB_DISABLE_LTUP));
	CHECK(!FlatSB_EnableScrollBar(h, SB_BOTH, ESB_DISABLE_LTUP));
	CHECK(FlatSB_EnableScrollBar(h, SB_VERT, ESB_ENABLE_BOTH));
	CHECK(!FlatSB_EnableScrollBar(h, SB_VERT, ESB_ENABLE_BOTH));
	CHECK(FlatSB_EnableScrollBar(h, SB_VERT, ESB_DISABLE_BOTH));

	CHECK_EQ(UninitializeFlatSB(h), S_OK);
	CHECK(!FlatSB_GetScrollProp(h, WSB_PROP_VSTYLE, &v));
	CHECK(!InitializeFlatSB(NULL));
	CHECK_EQ(FlatSB_GetScrollPos(NULL, SB_VERT), 0);
	DestroyWindow(h);
}

/* MRU lists, in the registry. */
static int WINAPI by_first(LPCVOID a, LPCVOID b, DWORD n)
{
	return *(const BYTE *)a != *(const BYTE *)b;
}

static void test_mru(void)
{
	FN(HANDLE, CreateMRUListA, const MRUINFOA *);
	FN(HANDLE, CreateMRUListW, const MRUINFOW *);
	FN(void, FreeMRUList, HANDLE);
	FN(INT, AddMRUStringA, HANDLE, LPCSTR);
	FN(INT, AddMRUStringW, HANDLE, LPCWSTR);
	FN(INT, FindMRUStringA, HANDLE, LPCSTR, int *);
	FN(INT, FindMRUStringW, HANDLE, LPCWSTR, int *);
	FN(INT, EnumMRUListA, HANDLE, INT, LPVOID, DWORD);
	FN(INT, EnumMRUListW, HANDLE, INT, LPVOID, DWORD);
	FN(BOOL, DelMRUString, HANDLE, INT);
	FN(INT, AddMRUData, HANDLE, LPCVOID, DWORD);
	FN(INT, FindMRUData, HANDLE, LPCVOID, DWORD, int *);
	CHECK(CreateMRUListA == mod_get_ordinal(cc, 151));
	CHECK(AddMRUStringW == mod_get_ordinal(cc, 401));
	CHECK(FindMRUData == mod_get_ordinal(cc, 169));

	struct module *adv = mod_load("advapi32.dll", NULL);
	CHECK(adv != NULL);
	if (!adv)
		return;
	LONG (WINAPI *RegOpenKeyExA)(HKEY, LPCSTR, DWORD, REGSAM, PHKEY) =
		(LONG (WINAPI *)(HKEY, LPCSTR, DWORD, REGSAM, PHKEY))
		get_from(adv, "RegOpenKeyExA");
	LONG (WINAPI *RegQueryValueExA)(HKEY, LPCSTR, LPDWORD, LPDWORD, LPBYTE,
					LPDWORD) =
		(LONG (WINAPI *)(HKEY, LPCSTR, LPDWORD, LPDWORD, LPBYTE,
				 LPDWORD))get_from(adv, "RegQueryValueExA");
	LONG (WINAPI *RegCloseKey)(HKEY) =
		(LONG (WINAPI *)(HKEY))get_from(adv, "RegCloseKey");
	LONG (WINAPI *RegDeleteTreeA)(HKEY, LPCSTR) =
		(LONG (WINAPI *)(HKEY, LPCSTR))get_from(adv, "RegDeleteTreeA");
	static const char sub[] = "Software\\seance-test\\MRU";
	RegDeleteTreeA(HKEY_CURRENT_USER, sub);

	MRUINFOA info = { sizeof(info), 3, MRU_STRING, HKEY_CURRENT_USER, sub,
			  { NULL } };
	HANDLE m = CreateMRUListA(&info);
	CHECK(m != NULL);
	if (!m)
		return;
	CHECK_EQ(EnumMRUListA(m, -1, NULL, 0), 0);
	CHECK_EQ(AddMRUStringA(m, "one"), 0);
	CHECK_EQ(AddMRUStringA(m, "two"), 1);
	CHECK_EQ(AddMRUStringA(m, "three"), 2);
	CHECK_EQ(AddMRUStringA(m, "ONE"), 0);	/* moves up */
	CHECK_EQ(AddMRUStringA(m, "four"), 1);	/* takes two's place */
	int slot = -1;
	CHECK_EQ(FindMRUStringA(m, "two", &slot), -1);
	CHECK(FindMRUStringA(m, "Three", &slot) == 2 && slot == 2);
	CHECK_EQ(EnumMRUListA(m, -1, NULL, 0), 3);
	char buf[16];
	CHECK(EnumMRUListA(m, 1, buf, sizeof(buf)) == 3 && !strcmp(buf, "one"));
	CHECK(EnumMRUListA(m, 0, buf, 3) == 2 && !strcmp(buf, "fo"));
	CHECK_EQ(EnumMRUListA(m, 3, buf, sizeof(buf)), -1);
	CHECK_EQ(AddMRUStringW(m, w("x")), -1);		/* the wrong kind */
	CHECK_EQ(AddMRUData(m, "x", 1), -1);

	HKEY k = NULL;
	DWORD type, n = sizeof(buf);
	CHECK_EQ(RegOpenKeyExA(HKEY_CURRENT_USER, sub, 0, KEY_READ, &k),
		 ERROR_SUCCESS);
	CHECK(RegQueryValueExA(k, "MRUList", NULL, &type, (BYTE *)buf, &n) ==
		      ERROR_SUCCESS && type == REG_SZ && !strcmp(buf, "bac"));
	n = sizeof(buf);
	CHECK(RegQueryValueExA(k, "b", NULL, &type, (BYTE *)buf, &n) ==
		      ERROR_SUCCESS && !strcmp(buf, "four"));
	FreeMRUList(m);

	/* Read back as it was; then one deleted. */
	m = CreateMRUListA(&info);
	CHECK(m && EnumMRUListA(m, -1, NULL, 0) == 3);
	CHECK(EnumMRUListA(m, 2, buf, sizeof(buf)) == 5 &&
	      !strcmp(buf, "three"));
	CHECK(DelMRUString(m, 0));
	CHECK(!DelMRUString(m, 2));
	CHECK(EnumMRUListA(m, 0, buf, sizeof(buf)) == 3 && !strcmp(buf, "one"));
	n = sizeof(buf);
	CHECK(RegQueryValueExA(k, "MRUList", NULL, &type, (BYTE *)buf, &n) ==
		      ERROR_SUCCESS && !strcmp(buf, "ac"));
	n = sizeof(buf);
	CHECK(RegQueryValueExA(k, "b", NULL, &type, (BYTE *)buf, &n) != 
	      ERROR_SUCCESS);
	FreeMRUList(m);
	RegCloseKey(k);
	RegDeleteTreeA(HKEY_CURRENT_USER, sub);

	/* Wide strings, written only when freed. */
	MRUINFOW wi = { sizeof(wi), 2, MRU_STRING | MRU_CACHEWRITE,
			HKEY_CURRENT_USER, w("Software\\seance-test\\MRU"),
			{ NULL } };
	m = CreateMRUListW(&wi);
	CHECK(m != NULL);
	CHECK_EQ(AddMRUStringW(m, w("Alpha")), 0);
	CHECK_EQ(AddMRUStringW(m, w("beta")), 1);
	CHECK(FindMRUStringW(m, w("ALPHA"), &slot) == 1 && slot == 0);
	WCHAR wb[8];
	CHECK(EnumMRUListW(m, 0, wb, 8) == 4 && wb[0] == 'b' && wb[4] == 0);
	CHECK(RegOpenKeyExA(HKEY_CURRENT_USER, sub, 0, KEY_READ, &k) ==
	      ERROR_SUCCESS);
	n = sizeof(buf);
	CHECK(RegQueryValueExA(k, "MRUList", NULL, &type, (BYTE *)buf, &n) !=
	      ERROR_SUCCESS);
	FreeMRUList(m);
	n = sizeof(buf);
	CHECK(RegQueryValueExA(k, "MRUList", NULL, &type, (BYTE *)buf, &n) ==
		      ERROR_SUCCESS && !strcmp(buf, "ba"));
	m = CreateMRUListW(&wi);
	CHECK(m && EnumMRUListW(m, 1, wb, 8) == 5 && wb[0] == 'A');
	FreeMRUList(m);
	RegCloseKey(k);
	RegDeleteTreeA(HKEY_CURRENT_USER, sub);

	/* Data, with a comparison of the first byte only. */
	info.fFlags = MRU_BINARY;
	info.u.binary_cmpfn = by_first;
	m = CreateMRUListA(&info);
	CHECK(m != NULL);
	static const BYTE d1[] = { 1, 2, 3 }, d2[] = { 2, 0 }, d3[] = { 1, 9 };
	CHECK_EQ(AddMRUData(m, d1, sizeof(d1)), 0);
	CHECK_EQ(AddMRUData(m, d2, sizeof(d2)), 1);
	CHECK(FindMRUData(m, d3, sizeof(d3), &slot) == 1 && slot == 0);
	BYTE db[4] = { 0 };
	CHECK(EnumMRUListA(m, 1, db, 2) == 3 && db[0] == 1 && db[1] == 2 &&
	      db[2] == 0);
	CHECK_EQ(AddMRUStringA(m, "x"), -1);
	FreeMRUList(m);
	m = CreateMRUListA(&info);
	CHECK(m && FindMRUData(m, d2, sizeof(d2), NULL) == 0);
	FreeMRUList(m);
	RegDeleteTreeA(HKEY_CURRENT_USER, "Software\\seance-test");

	info.cbSize = 8;
	CHECK(CreateMRUListA(&info) == NULL);
	CHECK_EQ(AddMRUStringA(NULL, "x"), -1);
	FreeMRUList(NULL);
	mod_free(adv);
}

int main(void)
{
	CHECK_EQ(runtime_enter(), 0);
	cc = mod_load("comctl32.dll", NULL);
	u32 = mod_load("user32.dll", NULL);
	gdi = mod_load("gdi32.dll", NULL);
	CHECK(cc && cc->builtin && !cc->builtin->fallback && u32 && gdi);
	if (!cc || !u32 || !gdi)
		return test_summary("test_comctl32");
	struct module *ocx = mod_load("COMCTL32.OCX", NULL);
	CHECK(ocx && ocx->builtin == cc->builtin);
	mod_free(ocx);

	test_dpa();
	test_dpa_merge_stream();
	test_dsa();
	test_mru();
	test_alloc_str();
	test_classes();

	USER(HWND, CreateWindowExW, DWORD, LPCWSTR, LPCWSTR, DWORD, int, int,
	     int, int, HWND, HMENU, HINSTANCE, LPVOID);
	USER(BOOL, DestroyWindow, HWND);
	parent = CreateWindowExW(0, w("STATIC"), NULL, WS_OVERLAPPEDWINDOW, 0,
				 0, 400, 300, NULL, NULL, NULL, NULL);
	CHECK(parent != NULL);
	test_trackbar();
	test_progress();
	test_updown();
	test_status();
	test_subclass();
	test_imagelist();
	test_flatsb();
	test_sheets();
	test_task_dialog();
	test_misc();
	test_list_controls();
	DestroyWindow(parent);
	test_ordinals();
	test_fixture();

	mod_free(gdi);
	mod_free(u32);
	mod_free(cc);
	return test_summary("test_comctl32");
}
