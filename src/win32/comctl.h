/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * comctl32 internals shared between its source files.
 */
#ifndef VFW_WIN32_COMCTL_H
#define VFW_WIN32_COMCTL_H

#include "runtime/builtin.h"
#include "win32.h"

extern const struct builtin_dll builtin_user32, builtin_gdi32;

/* A user32 or gdi32 export, typed at the call site. */
#define USER32(name) builtin_export(&builtin_user32, #name)
#define GDI32(name)  builtin_export(&builtin_gdi32, #name)

/* Alloc, ReAlloc, Free: zeroed memory from the process heap. */
void *comctl_alloc(size_t size);
void *comctl_realloc(void *p, size_t size);
void comctl_free(void *p);

/* Helpers for the controls (comctl32.c). Each control keeps its state
 * behind its first window extra bytes. */
WCHAR *comctl_a_to_w(const char *s);
char *comctl_w_to_a(const WCHAR *s);
WCHAR *comctl_w_dup(const WCHAR *s);
int comctl_copy_text_out(const WCHAR *text, void *buf, int size, int wide);
void *comctl_state(HWND h);
void comctl_set_state(HWND h, void *s);
DWORD comctl_style(HWND h);
LRESULT comctl_def(HWND h, UINT msg, WPARAM wp, LPARAM lp);
LRESULT comctl_send(HWND h, UINT msg, WPARAM wp, LPARAM lp);
HWND comctl_parent(HWND h);
BOOL comctl_client_rect(HWND h, RECT *rc);
HWND comctl_create(LPCWSTR cls, DWORD style, int x, int y, int cx, int cy,
		   HWND parent, UINT id);
BOOL comctl_move(HWND h, int x, int y, int cx, int cy);
BOOL comctl_notify_unicode(HWND h);
int comctl_text_width(const WCHAR *text);
LONG comctl_set_style(HWND h, DWORD style);
/* WM_NOTIFY to h's parent, with hdr's header filled in. */
LRESULT comctl_notify(HWND from, UINT code, NMHDR *hdr);
/* The same, to a given window (NULL: the parent). */
LRESULT comctl_notify_to(HWND to, HWND from, UINT code, NMHDR *hdr);

/* SetWindowSubclass, RemoveWindowSubclass and DefSubclassProc. */
BOOL comctl_set_subclass(HWND h, SUBCLASSPROC proc, UINT_PTR id, DWORD_PTR ref);
BOOL comctl_remove_subclass(HWND h, SUBCLASSPROC proc, UINT_PTR id);
LRESULT comctl_def_subclass(HWND h, UINT msg, WPARAM wp, LPARAM lp);

/* The control procedures in the other source files. */
LRESULT STUB_WINAPI comctl_header_proc(HWND h, UINT msg, WPARAM wp, LPARAM lp);
LRESULT STUB_WINAPI comctl_listview_proc(HWND h, UINT msg, WPARAM wp, LPARAM lp);
LRESULT STUB_WINAPI comctl_treeview_proc(HWND h, UINT msg, WPARAM wp, LPARAM lp);
LRESULT STUB_WINAPI comctl_toolbar_proc(HWND h, UINT msg, WPARAM wp, LPARAM lp);
LRESULT STUB_WINAPI comctl_tooltip_proc(HWND h, UINT msg, WPARAM wp, LPARAM lp);

/* The export tables of the other source files. */
extern const struct builtin_export comctl_dpa_exports[];
extern const struct builtin_export comctl_flatsb_exports[];
extern const struct builtin_export comctl_imagelist_exports[];
extern const struct builtin_export comctl_sheet_exports[];

#endif /* VFW_WIN32_COMCTL_H */
