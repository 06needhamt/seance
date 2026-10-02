/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * Uses comctl32 from Win32 code, importing it by ordinal only (see
 * comctl_ord.def): a sorted DPA with a comparison callback, a DSA,
 * Alloc and Str_SetPtr, and a subclassed trackbar whose subclass
 * procedure calls back into DefSubclassProc; and comdlg32 by name.
 * comctl_run returns 0, or the number of the first check that failed.
 */
#include "fixture.h"
#include <commctrl.h>
#include <commdlg.h>

LPVOID WINAPI Alloc(DWORD);
BOOL WINAPI Free(LPVOID);
BOOL WINAPI Str_SetPtrA(LPSTR *, LPCSTR);

static int WINAPI by_value(void *a, void *b, LPARAM lp)
{
	return (int)(INT_PTR)a - (int)(INT_PTR)b;
}

static int subclass_calls;

static LRESULT WINAPI sub(HWND h, UINT m, WPARAM wp, LPARAM lp, UINT_PTR id,
			  DWORD_PTR ref)
{
	if (m == TBM_GETPOS)
		subclass_calls += (int)ref;
	return DefSubclassProc(h, m, wp, lp);
}

EXPORT int comctl_run(void)
{
	static const WCHAR cls[] = { 'm', 's', 'c', 't', 'l', 's', '_', 't',
		'r', 'a', 'c', 'k', 'b', 'a', 'r', '3', '2', 0 };
	char title[16];

	InitCommonControls();

	HDPA d = DPA_Create(4);
	if (!d || DPA_InsertPtr(d, 0, (void *)3) != 0 ||
	    DPA_InsertPtr(d, DA_LAST, (void *)1) != 1 ||
	    DPA_InsertPtr(d, DA_LAST, (void *)2) != 2)
		return 1;
	if (!DPA_Sort(d, by_value, 0) || DPA_GetPtr(d, 0) != (void *)1 ||
	    DPA_GetPtr(d, 2) != (void *)3 || DPA_GetPtrCount(d) != 3)
		return 2;
	if (!DPA_Destroy(d))
		return 3;

	POINT pt = { 5, 6 };
	HDSA s = DSA_Create(sizeof(POINT), 2);
	if (!s || DSA_InsertItem(s, 0, &pt) != 0 ||
	    ((POINT *)DSA_GetItemPtr(s, 0))->y != 6 || !DSA_Destroy(s))
		return 4;

	char *str = NULL;
	BYTE *m = Alloc(8);
	if (!m || m[7] || !Free(m) || !Str_SetPtrA(&str, "codec") ||
	    lstrcmpA(str, "codec") || !Str_SetPtrA(&str, NULL) || str)
		return 5;

	/* A trackbar, subclassed; its state answers through the chain. */
	HWND t = CreateWindowExW(0, cls, NULL, 0, 0, 0, 100, 20, NULL, NULL,
				 NULL, NULL);
	if (!t)
		return 6;
	SendMessageW(t, TBM_SETRANGE, FALSE, MAKELONG(0, 10));
	SendMessageW(t, TBM_SETPOS, TRUE, 7);
	if (!SetWindowSubclass(t, sub, 1, 2) ||
	    SendMessageW(t, TBM_GETPOS, 0, 0) != 7 || subclass_calls != 2)
		return 7;
	if (!RemoveWindowSubclass(t, sub, 1) ||
	    SendMessageW(t, TBM_GETPOS, 0, 0) != 7 || subclass_calls != 2)
		return 8;
	DestroyWindow(t);

	if (GetFileTitleA("C:\\codecs\\x.ini", title, sizeof(title)) ||
	    lstrcmpA(title, "x.ini"))
		return 9;
	return 0;
}

BOOL WINAPI DllMain(HINSTANCE inst, DWORD reason, LPVOID reserved)
{
	return TRUE;
}
