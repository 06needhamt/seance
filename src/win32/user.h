/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * user32 internals shared between its source files.
 */
#ifndef VFW_WIN32_USER_H
#define VFW_WIN32_USER_H

#include "runtime/builtin.h"
#include "win32.h"

/* The pretend screen (the same as gdi.h's). */
#define USER_SCREEN_WIDTH  1024
#define USER_SCREEN_HEIGHT 768

/* Whether hwnd is a live window (the desktop included). */
int user_is_window(HWND hwnd);

/* DestroyWindow, for the clipboard: an owner renders what it promised. */
void user_clipboard_owner_destroyed(HWND hwnd);

/* The desktop window. */
HWND user_desktop(void);

/* A window's client rectangle ({0, 0, width, height}); 0 if no window. */
int user_client_rect(HWND hwnd, RECT *rc);

/* Milliseconds since boot (GetTickCount). */
DWORD user_ticks(void);

/* The command of the accelerator in e[0..n) a keyboard message is, or -1. */
int user_accel_match(const ACCEL *e, int n, const MSG *msg);

/* Updates the calling thread's key state for a keyboard message it takes. */
void user_key_message(const MSG *m);

/* The default window procedures (u32_window.c). */
LRESULT WINAPI user_DefWindowProcW(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp);
LRESULT WINAPI user_DefWindowProcA(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp);

/*
 * The dialog class's procedure and the standard controls' (u32_dialog.c).
 * A control keeps its state behind a pointer in its window extra bytes.
 */
/* Get/SetWindowLongPtrW by export name: the plain ones on i386, where
 * the Ptr forms are macros. */
#ifdef __x86_64__
#define USER_GETLONGPTR_W "GetWindowLongPtrW"
#define USER_SETLONGPTR_W "SetWindowLongPtrW"
#else
#define USER_GETLONGPTR_W "GetWindowLongW"
#define USER_SETLONGPTR_W "SetWindowLongW"
#endif

#define USER_CONTROL_EXTRA ((int)sizeof(void *))
LRESULT STUB_WINAPI user_DefDlgProcW(HWND hwnd, UINT msg, WPARAM wp,
				     LPARAM lp);
LRESULT STUB_WINAPI user_DefDlgProcA(HWND hwnd, UINT msg, WPARAM wp,
				     LPARAM lp);
LRESULT STUB_WINAPI user_static_proc(HWND hwnd, UINT msg, WPARAM wp,
				     LPARAM lp);
LRESULT STUB_WINAPI user_button_proc(HWND hwnd, UINT msg, WPARAM wp,
				     LPARAM lp);
LRESULT STUB_WINAPI user_edit_proc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp);
LRESULT STUB_WINAPI user_listbox_proc(HWND hwnd, UINT msg, WPARAM wp,
				      LPARAM lp);
LRESULT STUB_WINAPI user_combobox_proc(HWND hwnd, UINT msg, WPARAM wp,
				       LPARAM lp);

/*
 * For comctl32's property sheets: a property page's dialog, a child of
 * parent whatever its template's style (WS_CHILD and DS_CONTROL, shown),
 * and a template's size in dialog units and caption (malloc'd, or NULL):
 * 0, or -1 if it isn't a template. size is (size_t)-1 if not known.
 */
HWND user_dialog_child(HINSTANCE inst, const void *tmpl, size_t size,
		       HWND parent, DLGPROC proc, LPARAM param, int unicode);
int user_dialog_template_info(const void *tmpl, size_t size, int *cx,
			      int *cy, WCHAR **caption);

#endif /* VFW_WIN32_USER_H */
