/*
 * comctl32 through its export table: dynamic arrays, the Alloc and Str_
 * helpers, the controls' state, window subclassing, image lists, the
 * headless property sheets and task dialogs, the rest, and its ordinals.
 */
#include <stdlib.h>
#include <string.h>

#include "loader/modules.h"
#include "runtime/runtime.h"
#include "test.h"
#include "win32/codepage.h"

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
	USER(LONG, GetWindowLongW, HWND, int);
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
	CHECK(GetWindowLongW(h, GWL_WNDPROC) != (LONG)(ULONG_PTR)base_proc);
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
	CHECK_EQ(GetWindowLongW(h, GWL_WNDPROC), (LONG)(ULONG_PTR)base_proc);
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
	USER(LONG, SetWindowLongW, HWND, int, LONG);

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
	SetWindowLongW(parent, GWL_WNDPROC, (LONG)(ULONG_PTR)parent_proc);
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
	test_dsa();
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
	test_sheets();
	test_task_dialog();
	test_misc();
	DestroyWindow(parent);
	test_ordinals();
	test_fixture();

	mod_free(gdi);
	mod_free(u32);
	mod_free(cc);
	return test_summary("test_comctl32");
}
