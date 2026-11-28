/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * shell32: the "Properties" verb. A modeless property sheet titled
 * "<name> Properties" with a General page: the name (which can be
 * changed for a single item), type, location, size (and for folders what
 * they contain), the created, modified and accessed times, and the
 * read-only attribute (three-state for several items that differ).
 * OK and Apply rename the item and set the attribute; the sheet destroys
 * itself once it ends, as Explorer's does.
 */
#include <dirent.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>

#include "win32/codepage.h"
#include "win32/commctrl.h"
#include "win32/internal.h"
#include "win32/k32.h"
#include "win32/path.h"
#include "win32/shell.h"
#include "win32/shellapi.h"
#include "win32/user.h"

extern const struct builtin_dll builtin_user32, builtin_comctl32,
	builtin_shell32, builtin_kernel32;

#define U32(name) builtin_export(&builtin_user32, name)
#define CC(name) builtin_export(&builtin_comctl32, name)
#define K32(name) builtin_export(&builtin_kernel32, name)

#define WIN_SHEET_DONE 0x03FA	/* private: a hidden sheet is destroyed */
#define SHEET_SUB_ID 0x50524f50	/* "PROP" */

enum { ID_NAME = 101, ID_TYPE, ID_LOCATION, ID_SIZE, ID_CONTAINS,
       ID_CREATED, ID_MODIFIED, ID_ACCESSED, ID_READONLY = 110 };

struct props {
	UINT n;
	char **path;		/* Windows paths */
	int can_rename;
	int ready;		/* WM_INITDIALOG is done */
	DWORD ro_state;		/* BST_* when the page was filled */
	WORD *tmpl;		/* the page's template, kept while it lives */
};

/* A growing DLGTEMPLATE. */
struct tbuf {
	WORD *w;
	size_t n, cap;
	int bad;
};

static void t_word(struct tbuf *t, WORD v)
{
	if (t->n == t->cap) {
		size_t cap = t->cap ? t->cap * 2 : 256;
		WORD *g = realloc(t->w, cap * sizeof(*g));
		if (!g) {
			t->bad = 1;
			return;
		}
		t->w = g;
		t->cap = cap;
	}
	t->w[t->n++] = v;
}

static void t_dword(struct tbuf *t, DWORD v)
{
	t_word(t, LOWORD(v));
	t_word(t, HIWORD(v));
}

static void t_text(struct tbuf *t, const char *s)
{
	WCHAR *w = utf8_to_utf16_dup(s ? s : "");
	for (const WCHAR *c = w; c && *c; c++)
		t_word(t, *c);
	t_word(t, 0);
	if (!w)
		t->bad = 1;
	free(w);
}

static void t_item(struct tbuf *t, DWORD style, int x, int y, int cx,
		   int cy, WORD id, WORD cls, const char *text)
{
	if (t->n & 1)
		t_word(t, 0);		/* DWORD-aligned */
	t_dword(t, style | WS_CHILD | WS_VISIBLE);
	t_dword(t, 0);
	t_word(t, (WORD)x);
	t_word(t, (WORD)y);
	t_word(t, (WORD)cx);
	t_word(t, (WORD)cy);
	t_word(t, id);
	t_word(t, 0xffff);
	t_word(t, cls);
	t_text(t, text);
	t_word(t, 0);			/* no creation data */
}

static WORD *page_template(const struct props *p)
{
	static const char *const label[] = { "Type:", "Location:", "Size:",
		"Contains:", "Created:", "Modified:", "Accessed:" };
	static const int row[] = { 26, 38, 50, 62, 80, 92, 104 };
	struct tbuf t = { 0 };
	int items = 1 + 2 * 7 + 2;

	t_dword(&t, WS_CHILD | DS_CONTROL | WS_CAPTION | DS_SETFONT);
	t_dword(&t, 0);
	t_word(&t, (WORD)items);
	t_word(&t, 0);
	t_word(&t, 0);
	t_word(&t, 227);
	t_word(&t, 160);
	t_word(&t, 0);			/* no menu */
	t_word(&t, 0);			/* the dialog class */
	t_text(&t, "General");
	t_word(&t, 8);
	t_text(&t, "MS Shell Dlg");
	t_item(&t, WS_BORDER | WS_TABSTOP | ES_AUTOHSCROLL |
	       (p->can_rename ? 0 : ES_READONLY), 8, 6, 211, 12, ID_NAME,
	       0x81, "");
	for (int i = 0; i < 7; i++) {
		t_item(&t, SS_LEFT, 8, row[i], 50, 8, 0xffff, 0x82, label[i]);
		t_item(&t, SS_LEFT | SS_NOPREFIX, 60, row[i], 160, 8,
		       (WORD)(ID_TYPE + i), 0x82, "");
	}
	t_item(&t, SS_LEFT, 8, 122, 50, 8, 0xffff, 0x82, "Attributes:");
	t_item(&t, WS_TABSTOP | (p->n > 1 ? BS_AUTO3STATE : BS_AUTOCHECKBOX),
	       60, 121, 80, 10, ID_READONLY, 0x80, "&Read-only");
	if (t.bad) {
		free(t.w);
		return NULL;
	}
	return t.w;
}

static void props_free(struct props *p)
{
	if (!p)
		return;
	for (UINT i = 0; i < p->n; i++)
		free(p->path[i]);
	free(p->path);
	free(p->tmpl);
	free(p);
}

static const char *base_name(const char *path)
{
	const char *b = strrchr(path, '\\');
	return b && b[1] ? b + 1 : path;
}

/* "1,234". */
static void group(char *out, size_t cch, unsigned long long v)
{
	char d[32];
	int n = snprintf(d, sizeof(d), "%llu", v), k = 0;
	for (int i = 0; i < n && (size_t)k + 2 < cch; i++) {
		if (i && (n - i) % 3 == 0)
			out[k++] = ',';
		out[k++] = d[i];
	}
	out[k] = 0;
}

/* "1.50 KB (1,536 bytes)", as Explorer shows a size. */
static void size_text(char *out, size_t cch, unsigned long long v)
{
	static const char *const unit[] = { "KB", "MB", "GB", "TB" };
	char g[40];

	group(g, sizeof(g), v);
	if (v < 1024) {
		snprintf(out, cch, "%s bytes", g);
		return;
	}
	double x = (double)v / 1024;
	int u = 0;
	while (x >= 1024 && u < 3) {
		x /= 1024;
		u++;
	}
	snprintf(out, cch, x < 10 ? "%.2f %s (%s bytes)" :
		 x < 100 ? "%.1f %s (%s bytes)" : "%.0f %s (%s bytes)",
		 x, unit[u], g);
}

static void time_text(char *out, size_t cch, time_t t)
{
	struct tm tm;
	if (localtime_r(&t, &tm))
		strftime(out, cch, "%Y-%m-%d %H:%M:%S", &tm);
	else
		snprintf(out, cch, "-");
}

struct tally {
	unsigned long long bytes;
	unsigned long files, dirs;
};

/* What a folder holds, without following links. */
static void count_dir(const char *dir, struct tally *t, int depth)
{
	DIR *d = depth < 64 ? opendir(dir) : NULL;
	struct dirent *e;

	if (!d)
		return;
	while ((e = readdir(d))) {
		if (!strcmp(e->d_name, ".") || !strcmp(e->d_name, ".."))
			continue;
		size_t len = strlen(dir) + strlen(e->d_name) + 2;
		char *full = malloc(len);
		struct stat st;
		if (!full)
			break;
		snprintf(full, len, "%s/%s", dir, e->d_name);
		if (!lstat(full, &st)) {
			if (S_ISDIR(st.st_mode)) {
				t->dirs++;
				count_dir(full, t, depth + 1);
			} else {
				t->files++;
				t->bytes += (unsigned long long)st.st_size;
			}
		}
		free(full);
	}
	closedir(d);
}

static void set_text(HWND page, int id, const char *s)
{
	HWND (WINAPI *item)(HWND, int) = U32("GetDlgItem");
	BOOL (WINAPI *set)(HWND, LPCWSTR) = U32("SetWindowTextW");
	WCHAR *w = utf8_to_utf16_dup(s);
	HWND h = item(page, id);

	if (h && w)
		set(h, w);
	free(w);
}

static DWORD attributes(const char *path)
{
	DWORD (WINAPI *get)(LPCWSTR) = K32("GetFileAttributesW");
	WCHAR *w = utf8_to_utf16_dup(path);
	DWORD a = w ? get(w) : INVALID_FILE_ATTRIBUTES;
	free(w);
	return a;
}

static void fill_page(HWND page, struct props *p)
{
	LRESULT (WINAPI *send)(HWND, int, UINT, WPARAM, LPARAM) =
		U32("SendDlgItemMessageW");
	DWORD_PTR (WINAPI *file_info)(LPCWSTR, DWORD, SHFILEINFOW *, UINT,
				      UINT) =
		builtin_export(&builtin_shell32, "SHGetFileInfoW");
	char buf[600];
	struct tally t = { 0, 0, 0 };
	int ro = 0, rw = 0, folders = 0;
	time_t created = 0, modified = 0, accessed = 0;

	for (UINT i = 0; i < p->n; i++) {
		char *u = path_to_unix(p->path[i]);
		struct stat st;
		if (u && !stat(u, &st)) {
			if (S_ISDIR(st.st_mode)) {
				folders++;
				if (p->n > 1)
					t.dirs++;
				count_dir(u, &t, 0);
			} else {
				if (p->n > 1)
					t.files++;
				t.bytes += (unsigned long long)st.st_size;
			}
			created = st.st_ctime;
			modified = st.st_mtime;
			accessed = st.st_atime;
		}
		free(u);
		DWORD a = attributes(p->path[i]);
		if (a != INVALID_FILE_ATTRIBUTES &&
		    !(a & FILE_ATTRIBUTE_DIRECTORY))
			*(a & FILE_ATTRIBUTE_READONLY ? &ro : &rw) += 1;
	}

	if (p->n == 1) {
		set_text(page, ID_NAME, strlen(p->path[0]) <= 3 ? p->path[0] :
			 base_name(p->path[0]));
		SHFILEINFOW fi;
		WCHAR *w = utf8_to_utf16_dup(p->path[0]);
		memset(&fi, 0, sizeof(fi));
		char *type = w && file_info(w, 0, &fi, sizeof(fi),
					    SHGFI_TYPENAME) ?
			     utf16_to_utf8_dup(fi.szTypeName) : NULL;
		free(w);
		set_text(page, ID_TYPE, type && *type ? type :
			 folders ? "File Folder" : "File");
		free(type);
	} else {
		snprintf(buf, sizeof(buf), "%u items", p->n);
		set_text(page, ID_NAME, buf);
		set_text(page, ID_TYPE, folders == (int)p->n ? "All folders" :
			 folders ? "Multiple types" : "Files");
	}
	char *loc = strdup(p->path[0]);
	if (loc) {
		char *s = strrchr(loc, '\\');
		if (s && s > loc && s[-1] == ':')
			s[1] = 0;	/* C:\x is in C:\ */
		else if (s)
			*s = 0;
		set_text(page, ID_LOCATION, strlen(p->path[0]) <= 3 ? "" : loc);
		free(loc);
	}
	size_text(buf, sizeof(buf), t.bytes);
	set_text(page, ID_SIZE, buf);
	if (folders || p->n > 1) {
		char f[32], d[32];
		group(f, sizeof(f), t.files);
		group(d, sizeof(d), t.dirs);
		snprintf(buf, sizeof(buf), "%s File%s, %s Folder%s", f,
			 t.files == 1 ? "" : "s", d, t.dirs == 1 ? "" : "s");
		set_text(page, ID_CONTAINS, buf);
	}
	if (p->n == 1) {
		time_text(buf, sizeof(buf), created);
		set_text(page, ID_CREATED, buf);
		time_text(buf, sizeof(buf), modified);
		set_text(page, ID_MODIFIED, buf);
		time_text(buf, sizeof(buf), accessed);
		set_text(page, ID_ACCESSED, buf);
	}
	p->ro_state = ro && rw ? BST_INDETERMINATE :
		      ro ? BST_CHECKED : BST_UNCHECKED;
	send(page, ID_READONLY, BM_SETCHECK, p->ro_state, 0);
	if (!ro && !rw) {
		BOOL (WINAPI *enable)(HWND, BOOL) = U32("EnableWindow");
		HWND (WINAPI *item)(HWND, int) = U32("GetDlgItem");
		enable(item(page, ID_READONLY), FALSE);	/* folders only */
	}
}

static int bad_name(const char *n)
{
	return !*n || strpbrk(n, "\\/:*?\"<>|") || !strcmp(n, ".") ||
	       !strcmp(n, "..");
}

/* OK or Apply: FALSE leaves the page up (an invalid name). */
static BOOL apply(HWND page, struct props *p)
{
	LRESULT (WINAPI *send)(HWND, int, UINT, WPARAM, LPARAM) =
		U32("SendDlgItemMessageW");
	BOOL (WINAPI *set_attr)(LPCWSTR, DWORD) = K32("SetFileAttributesW");
	BOOL (WINAPI *move)(LPCWSTR, LPCWSTR) = K32("MoveFileW");
	UINT (WINAPI *get_text)(HWND, int, LPWSTR, int) = U32("GetDlgItemTextW");

	if (p->can_rename) {
		WCHAR w[MAX_PATH];
		w[0] = 0;
		get_text(page, ID_NAME, w, MAX_PATH);
		char *name = utf16_to_utf8_dup(w);
		const char *old = base_name(p->path[0]);
		if (!name)
			return FALSE;
		if (strcmp(name, old)) {
			if (bad_name(name)) {
				fprintf(stderr, "Rename [%s]\n  (refused: a "
					"name cannot contain \\ / : * ? \" "
					"< > |)\n", old);
				free(name);
				return FALSE;
			}
			size_t dir = (size_t)(old - p->path[0]);
			char *to = malloc(dir + strlen(name) + 1);
			WCHAR *wf = utf8_to_utf16_dup(p->path[0]);
			WCHAR *wt = NULL;
			if (to) {
				memcpy(to, p->path[0], dir);
				strcpy(to + dir, name);
				wt = utf8_to_utf16_dup(to);
			}
			if (!wf || !wt || !move(wf, wt)) {
				fprintf(stderr, "Rename [%s]\n  (failed)\n", old);
				free(to);
				to = NULL;
			}
			free(wf);
			free(wt);
			if (!to) {
				free(name);
				return FALSE;
			}
			free(p->path[0]);
			p->path[0] = to;
		}
		free(name);
	}
	DWORD ro = (DWORD)send(page, ID_READONLY, BM_GETCHECK, 0, 0);
	if (ro != p->ro_state && ro != BST_INDETERMINATE) {
		for (UINT i = 0; i < p->n; i++) {
			DWORD a = attributes(p->path[i]);
			WCHAR *w = utf8_to_utf16_dup(p->path[i]);
			if (a != INVALID_FILE_ATTRIBUTES && w &&
			    !(a & FILE_ATTRIBUTE_DIRECTORY))
				set_attr(w, ro == BST_CHECKED ?
					 a | FILE_ATTRIBUTE_READONLY :
					 a & ~FILE_ATTRIBUTE_READONLY);
			free(w);
		}
		p->ro_state = ro;
	}
	return TRUE;
}

WINFN(INT_PTR) general_proc(HWND page, UINT msg, WPARAM wp, LPARAM lp)
{
	LONG_PTR (WINAPI *get_long)(HWND, int) = U32(USER_GETLONGPTR_W);
	LONG_PTR (WINAPI *set_long)(HWND, int, LONG_PTR) =
		U32(USER_SETLONGPTR_W);
	HWND (WINAPI *parent)(HWND) = U32("GetParent");
	LRESULT (WINAPI *send)(HWND, UINT, WPARAM, LPARAM) =
		U32("SendMessageW");
	struct props *p = (struct props *)get_long(page, DWLP_USER);

	switch (msg) {
	case WM_INITDIALOG: {
		const PROPSHEETPAGEW *psp = (const PROPSHEETPAGEW *)lp;
		p = psp ? (struct props *)psp->lParam : NULL;
		set_long(page, DWLP_USER, (LONG_PTR)p);
		if (p) {
			fill_page(page, p);
			p->ready = 1;
		}
		return TRUE;
	}
	case WM_COMMAND:
		if (p && p->ready &&
		    ((LOWORD(wp) == ID_NAME && HIWORD(wp) == EN_CHANGE) ||
		     (LOWORD(wp) == ID_READONLY && HIWORD(wp) == BN_CLICKED)))
			send(parent(page), PSM_CHANGED, (WPARAM)page, 0);
		return FALSE;
	case WM_NOTIFY:
		if (p && lp && ((const NMHDR *)lp)->code == PSN_APPLY) {
			set_long(page, DWLP_MSGRESULT, apply(page, p) ?
				 PSNRET_NOERROR : PSNRET_INVALID_NOCHANGEPAGE);
			return TRUE;
		}
		return FALSE;
	}
	return FALSE;
}

WINFN(UINT) general_callback(HWND h, UINT msg, PROPSHEETPAGEW *psp)
{
	if (msg == PSPCB_RELEASE && psp)
		props_free((struct props *)psp->lParam);
	return 1;
}

/* An ended modeless sheet is hidden with no current page; it goes. */
WINFN(LRESULT) sheet_sub(HWND h, UINT msg, WPARAM wp, LPARAM lp,
			 UINT_PTR id, DWORD_PTR ref)
{
	LRESULT (WINAPI *def)(HWND, UINT, WPARAM, LPARAM) =
		CC("DefSubclassProc");
	LRESULT (WINAPI *send)(HWND, UINT, WPARAM, LPARAM) =
		U32("SendMessageW");
	BOOL (WINAPI *post)(HWND, UINT, WPARAM, LPARAM) = U32("PostMessageW");
	BOOL (WINAPI *destroy)(HWND) = U32("DestroyWindow");

	if (msg == WIN_SHEET_DONE) {
		if (!send(h, PSM_GETCURRENTPAGEHWND, 0, 0))
			destroy(h);
		return 0;
	}
	if (msg == WM_SHOWWINDOW && !wp)
		post(h, WIN_SHEET_DONE, 0, 0);
	return def(h, msg, wp, lp);
}

HRESULT shell_properties(HWND owner, UINT n, char *const *paths)
{
	INT_PTR (WINAPI *sheet)(LPCPROPSHEETHEADERW) = CC("PropertySheetW");
	BOOL (WINAPI *subclass)(HWND, SUBCLASSPROC, UINT_PTR, DWORD_PTR) =
		CC("SetWindowSubclass");
	struct props *p = n ? calloc(1, sizeof(*p)) : NULL;

	if (!n || !paths)
		return E_INVALIDARG;
	if (!p || !(p->path = calloc(n, sizeof(*p->path)))) {
		free(p);
		return E_OUTOFMEMORY;
	}
	for (UINT i = 0; i < n; i++) {
		p->n = i + 1;
		if (!paths[i] || !(p->path[i] = strdup(paths[i]))) {
			props_free(p);
			return paths[i] ? E_OUTOFMEMORY : E_INVALIDARG;
		}
	}
	p->can_rename = n == 1 && strlen(paths[0]) > 3;
	WORD *tmpl = p->tmpl = page_template(p);
	char *title = NULL;
	if (n == 1) {
		title = strdup(strlen(paths[0]) <= 3 ? paths[0] :
			       base_name(paths[0]));
	} else {
		size_t len = strlen(base_name(paths[0])) + 8;
		title = malloc(len);
		if (title)
			snprintf(title, len, "%s, ...", base_name(paths[0]));
	}
	WCHAR *wtitle = title ? utf8_to_utf16_dup(title) : NULL;
	free(title);
	if (!tmpl || !wtitle) {
		free(wtitle);
		props_free(p);
		return E_OUTOFMEMORY;
	}

	PROPSHEETPAGEW page;
	memset(&page, 0, sizeof(page));
	page.dwSize = sizeof(page);
	page.dwFlags = PSP_DLGINDIRECT | PSP_USECALLBACK;
	page.pResource = tmpl;
	page.pfnDlgProc = general_proc;
	page.lParam = (LPARAM)p;
	page.pfnCallback = general_callback;
	PROPSHEETHEADERW h;
	memset(&h, 0, sizeof(h));
	h.dwSize = sizeof(h);
	h.dwFlags = PSH_PROPSHEETPAGE | PSH_PROPTITLE | PSH_MODELESS;
	h.hwndParent = owner;
	h.pszCaption = wtitle;
	h.nPages = 1;
	h.ppsp = &page;
	HWND w = (HWND)sheet(&h);
	free(wtitle);
	if (!w) {
		/* PSPCB_RELEASE has run if the page was taken. */
		return E_FAIL;
	}
	subclass(w, sheet_sub, SHEET_SUB_ID, 0);
	return S_OK;
}
