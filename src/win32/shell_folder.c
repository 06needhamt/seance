/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * shell32's namespace: IShellFolder and IEnumIDList over the desktop, My
 * Computer, the Recycle Bin and the file system, with SHGetDesktopFolder,
 * SHBindToParent and SHParseDisplayName.
 *
 * The desktop holds My Computer, the Recycle Bin and what is in the
 * desktop directory; My Computer holds the drives; a directory holds its
 * files and directories (dot files are hidden); the Recycle Bin holds what
 * was deleted into it, named as it was before. Item ID lists are this
 * shell32's own (shell32.c): each item is absolute, so a folder's
 * children, ParseDisplayName's results and their combinations with the
 * folder's list all work alike. Names can be parsed as Windows paths or
 * as "::{CLSID}" for My Computer and the Recycle Bin.
 */
#include <dirent.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <unistd.h>

#include "win32/codepage.h"
#include "win32/internal.h"
#include "win32/k32.h"
#include "win32/path.h"
#include "win32/shell.h"

extern const struct builtin_dll builtin_ole32;
extern const struct builtin_dll builtin_kernel32;
extern const struct builtin_dll builtin_shell32;

#define InterlockedIncrement(p) __atomic_add_fetch((p), 1, __ATOMIC_SEQ_CST)
#define InterlockedDecrement(p) __atomic_sub_fetch((p), 1, __ATOMIC_SEQ_CST)
#ifndef MAKE_HRESULT
#define MAKE_HRESULT(sev, fac, code) \
	((HRESULT)(((ULONG)(sev) << 31) | ((ULONG)(fac) << 16) | (ULONG)(code)))
#endif

#define MY_COMPUTER "::{20D04FE0-3AEA-1069-A2D8-08002B30309D}"
#define RECYCLE_BIN "::{645FF040-5081-101B-9F08-00AA002F954E}"

static void *task_alloc(SIZE_T n)
{
	LPVOID (WINAPI *alloc)(SIZE_T) =
		builtin_export(&builtin_ole32, "CoTaskMemAlloc");
	return alloc ? alloc(n) : NULL;
}

static void pidl_free(LPITEMIDLIST p)
{
	heap_free(heap_process(), 0, p);
}

static int same_guid(REFIID a, const GUID *b)
{
	return a && !memcmp(a, b, sizeof(GUID));
}

/* What an item is: a virtual folder (csidl >= 0) or a Windows path. */
struct item {
	int csidl;
	char *path;
};

static int item_of(LPCITEMIDLIST pidl, struct item *it)
{
	it->path = NULL;
	it->csidl = shell_pidl_csidl(pidl);
	if (it->csidl >= 0)
		return 0;
	it->path = shell_pidl_path(pidl);
	return it->path ? 0 : -1;
}

static int is_dir_win(const char *win)
{
	struct stat st;
	char *u = path_to_unix(win);
	int r = u && !stat(u, &st) && S_ISDIR(st.st_mode);
	free(u);
	return r;
}

static int exists_win(const char *win)
{
	char *u = path_to_unix(win);
	int r = u && !access(u, F_OK);
	free(u);
	return r;
}

static const char *base_of(const char *path)
{
	const char *b = strrchr(path, '\\');
	return b && b[1] ? b + 1 : path;
}

/* Joins a directory and a name with one backslash, malloc()ed. */
static char *win_join(const char *dir, const char *name)
{
	size_t n = strlen(dir);
	char *p = malloc(n + strlen(name) + 2);
	if (p)
		sprintf(p, "%s%s%s", dir, n && dir[n - 1] == '\\' ? "" : "\\",
			name);
	return p;
}

/* The Recycle Bin's directory, made if missing. */
static int in_bin(const char *path)
{
	char *bin = shell_recycle_dir();
	size_t n = bin ? strlen(bin) : 0;
	int r = bin && !strncasecmp(path, bin, n) && path[n] == '\\';
	free(bin);
	return r;
}

/* The folders. */

enum kind { DESKTOP, DRIVES, BITBUCKET, DIRECTORY };

struct folder {
	IShellFolder iface;
	LONG ref;
	enum kind kind;
	char *path;		/* DIRECTORY's */
};

static const IShellFolderVtbl folder_vtbl;

static IShellFolder *new_folder(enum kind kind, const char *path)
{
	struct folder *f = calloc(1, sizeof(*f));

	if (!f)
		return NULL;
	f->iface.lpVtbl = &folder_vtbl;
	f->ref = 1;
	f->kind = kind;
	if (path && !(f->path = strdup(path))) {
		free(f);
		return NULL;
	}
	return &f->iface;
}

static struct folder *impl(IShellFolder *i)
{
	return (struct folder *)i;
}

/* The folder an item opens as, or NULL if it is no folder. */
static IShellFolder *folder_for(const struct item *it)
{
	if (it->csidl == CSIDL_DESKTOP)
		return new_folder(DESKTOP, NULL);
	if (it->csidl == CSIDL_DRIVES)
		return new_folder(DRIVES, NULL);
	if (it->csidl == CSIDL_BITBUCKET)
		return new_folder(BITBUCKET, NULL);
	if (it->csidl < 0 && it->path && is_dir_win(it->path))
		return new_folder(DIRECTORY, it->path);
	return NULL;
}

static HRESULT WINAPI f_QueryInterface(IShellFolder *This, REFIID riid,
				       void **ppv)
{
	if (!ppv)
		return E_POINTER;
	if (same_guid(riid, &IID_IUnknown) || same_guid(riid, &IID_IShellFolder)) {
		*ppv = This;
		InterlockedIncrement(&impl(This)->ref);
		return S_OK;
	}
	*ppv = NULL;
	return E_NOINTERFACE;
}

static ULONG WINAPI f_AddRef(IShellFolder *This)
{
	return (ULONG)InterlockedIncrement(&impl(This)->ref);
}

static ULONG WINAPI f_Release(IShellFolder *This)
{
	struct folder *f = impl(This);
	LONG r = InterlockedDecrement(&f->ref);
	if (!r) {
		free(f->path);
		free(f);
	}
	return (ULONG)r;
}

/* A path from a name as the folder sees it, or NULL. */
static char *resolve_name(struct folder *f, const char *name)
{
	int absolute = (name[0] && name[1] == ':') ||
		       (name[0] == '\\' && name[1] == '\\');
	if (absolute)
		return strdup(name);
	if (f->kind == DIRECTORY)
		return win_join(f->path, name);
	if (f->kind == DESKTOP) {
		char *desk = shell_folder_path(CSIDL_DESKTOPDIRECTORY);
		char *p = desk ? win_join(desk, name) : NULL;
		free(desk);
		return p;
	}
	return NULL;
}

static ULONG attributes(const struct item *it);

static HRESULT WINAPI f_ParseDisplayName(IShellFolder *This, HWND hwnd,
					 IBindCtx *bc, LPWSTR name,
					 ULONG *eaten, LPITEMIDLIST *pidl,
					 ULONG *attrs)
{
	if (!name || !pidl)
		return E_INVALIDARG;
	*pidl = NULL;
	char *n = utf16_to_utf8_dup(name);
	if (!n)
		return E_OUTOFMEMORY;
	struct item it = { -1, NULL };
	HRESULT hr = S_OK;

	if (!strcasecmp(n, MY_COMPUTER)) {
		it.csidl = CSIDL_DRIVES;
	} else if (!strcasecmp(n, RECYCLE_BIN)) {
		it.csidl = CSIDL_BITBUCKET;
	} else {
		char *p = resolve_name(impl(This), n);
		/* Drop a trailing backslash but a drive root's. */
		size_t l = p ? strlen(p) : 0;
		if (l > 3 && p[l - 1] == '\\')
			p[l - 1] = 0;
		if (!p)
			hr = E_INVALIDARG;
		else if (!exists_win(p))
			hr = HRESULT_FROM_WIN32(ERROR_FILE_NOT_FOUND);
		it.path = p;
	}
	if (hr == S_OK) {
		*pidl = it.csidl >= 0 ? shell_pidl_virtual(it.csidl) :
			shell_pidl_from_path(it.path);
		if (!*pidl)
			hr = E_OUTOFMEMORY;
	}
	if (hr == S_OK && eaten)
		*eaten = (ULONG)wstrlen(name);
	if (hr == S_OK && attrs)
		*attrs &= attributes(&it);
	free(it.path);
	free(n);
	return hr;
}

/* Enumeration: the children's lists, made up front. */

struct enumerator {
	IEnumIDList iface;
	LONG ref;
	LPITEMIDLIST *items;
	ULONG n, at;
};

static const IEnumIDListVtbl enum_vtbl;

static struct enumerator *eimpl(IEnumIDList *i)
{
	return (struct enumerator *)i;
}

static int add_item(struct enumerator *e, LPITEMIDLIST p)
{
	if (!p)
		return -1;
	LPITEMIDLIST *a = realloc(e->items, (e->n + 1) * sizeof(*a));
	if (!a) {
		pidl_free(p);
		return -1;
	}
	e->items = a;
	e->items[e->n++] = p;
	return 0;
}

/* The entries of a directory, filtered by flags. */
static void add_dir(struct enumerator *e, const char *win, DWORD flags,
		    int hide_index)
{
	char *u = path_to_unix(win);
	DIR *d = u ? opendir(u) : NULL;
	struct dirent *de;

	while (d && (de = readdir(d))) {
		if (!strcmp(de->d_name, ".") || !strcmp(de->d_name, ".."))
			continue;
		if (de->d_name[0] == '.' && !(flags & SHCONTF_INCLUDEHIDDEN))
			continue;
		if (hide_index && !strcmp(de->d_name, "INFO"))
			continue;
		char *p = win_join(win, de->d_name);
		if (!p)
			continue;
		int dir = is_dir_win(p);
		if ((dir && (flags & SHCONTF_FOLDERS)) ||
		    (!dir && (flags & SHCONTF_NONFOLDERS)))
			add_item(e, shell_pidl_from_path(p));
		free(p);
	}
	if (d)
		closedir(d);
	free(u);
}

static HRESULT WINAPI f_EnumObjects(IShellFolder *This, HWND hwnd,
				    DWORD flags, IEnumIDList **out)
{
	struct folder *f = impl(This);

	if (!out)
		return E_INVALIDARG;
	*out = NULL;
	struct enumerator *e = calloc(1, sizeof(*e));
	if (!e)
		return E_OUTOFMEMORY;
	e->iface.lpVtbl = &enum_vtbl;
	e->ref = 1;
	if (f->kind == DESKTOP) {
		if (flags & SHCONTF_FOLDERS) {
			add_item(e, shell_pidl_virtual(CSIDL_DRIVES));
			add_item(e, shell_pidl_virtual(CSIDL_BITBUCKET));
		}
		char *desk = shell_folder_path(CSIDL_DESKTOPDIRECTORY);
		if (desk)
			add_dir(e, desk, flags, 0);
		free(desk);
	} else if (f->kind == DRIVES) {
		DWORD (WINAPI *drives)(void) =
			builtin_export(&builtin_kernel32, "GetLogicalDrives");
		DWORD mask = drives ? drives() : 1u << 2;
		for (int i = 0; i < 26 && (flags & SHCONTF_FOLDERS); i++)
			if (mask & (1u << i)) {
				char root[4] = { (char)('A' + i), ':', '\\', 0 };
				add_item(e, shell_pidl_from_path(root));
			}
	} else if (f->kind == BITBUCKET) {
		char *bin = shell_recycle_dir();
		if (bin)
			add_dir(e, bin, flags | SHCONTF_INCLUDEHIDDEN, 1);
		free(bin);
	} else {
		add_dir(e, f->path, flags, 0);
	}
	*out = &e->iface;
	return S_OK;
}

static HRESULT WINAPI f_BindToObject(IShellFolder *This, LPCITEMIDLIST pidl,
				     IBindCtx *bc, REFIID riid, void **ppv)
{
	struct item it;

	if (!ppv)
		return E_INVALIDARG;
	*ppv = NULL;
	if (!pidl || item_of(pidl, &it))
		return E_INVALIDARG;
	IShellFolder *sf = folder_for(&it);
	free(it.path);
	if (!sf)
		return E_INVALIDARG;
	HRESULT hr = sf->lpVtbl->QueryInterface(sf, riid, ppv);
	sf->lpVtbl->Release(sf);
	return hr;
}

static HRESULT WINAPI f_BindToStorage(IShellFolder *This, LPCITEMIDLIST pidl,
				      IBindCtx *bc, REFIID riid, void **ppv)
{
	if (ppv)
		*ppv = NULL;
	return E_NOTIMPL;
}

/* An item's name as shown (UTF-8, malloc()ed). */
static char *shown_name(const struct item *it)
{
	if (it->csidl == CSIDL_DESKTOP)
		return strdup("Desktop");
	if (it->csidl == CSIDL_DRIVES)
		return strdup("My Computer");
	if (it->csidl == CSIDL_BITBUCKET)
		return strdup("Recycle Bin");
	if (it->csidl >= 0 || !it->path)
		return strdup("");
	if (strlen(it->path) == 3 && it->path[1] == ':') {
		char buf[32];
		snprintf(buf, sizeof(buf), "Local Disk (%c:)", it->path[0]);
		return strdup(buf);
	}
	if (in_bin(it->path)) {
		char *orig = shell_recycle_original(base_of(it->path));
		if (orig) {
			char *r = strdup(base_of(orig));
			free(orig);
			return r;
		}
	}
	return strdup(base_of(it->path));
}

static HRESULT WINAPI f_CompareIDs(IShellFolder *This, LPARAM lparam,
				   LPCITEMIDLIST a, LPCITEMIDLIST b)
{
	struct item ia, ib;

	if (!a || !b || item_of(a, &ia))
		return E_INVALIDARG;
	if (item_of(b, &ib)) {
		free(ia.path);
		return E_INVALIDARG;
	}
	int r;
	/* Virtual folders first, then folders, then by name. */
	int va = ia.csidl >= 0, vb = ib.csidl >= 0;
	int da = va || is_dir_win(ia.path), db = vb || is_dir_win(ib.path);
	if (va != vb)
		r = va ? -1 : 1;
	else if (va)
		r = ia.csidl < ib.csidl ? -1 : ia.csidl > ib.csidl;
	else if (da != db)
		r = da ? -1 : 1;
	else {
		char *na = shown_name(&ia), *nb = shown_name(&ib);
		r = na && nb ? strcasecmp(na, nb) : 0;
		if (!r && na && nb)
			r = strcasecmp(ia.path, ib.path);
		r = r < 0 ? -1 : r > 0;
		free(na);
		free(nb);
	}
	free(ia.path);
	free(ib.path);
	return MAKE_HRESULT(0, 0, (USHORT)(SHORT)r);
}

static HRESULT WINAPI f_CreateViewObject(IShellFolder *This, HWND owner,
					 REFIID riid, void **ppv)
{
	if (ppv)
		*ppv = NULL;
	return E_NOTIMPL;
}

static ULONG attributes(const struct item *it)
{
	if (it->csidl == CSIDL_DRIVES)
		return SFGAO_FOLDER | SFGAO_HASSUBFOLDER |
		       SFGAO_FILESYSANCESTOR | SFGAO_BROWSABLE;
	if (it->csidl == CSIDL_BITBUCKET)
		return SFGAO_FOLDER | SFGAO_BROWSABLE;
	if (it->csidl >= 0)
		return SFGAO_FOLDER | SFGAO_HASSUBFOLDER |
		       SFGAO_FILESYSANCESTOR | SFGAO_FILESYSTEM;
	if (!it->path)
		return 0;
	char *u = path_to_unix(it->path);
	struct stat st;
	ULONG a = 0;
	if (u && !stat(u, &st)) {
		a = SFGAO_FILESYSTEM | SFGAO_CANCOPY | SFGAO_CANMOVE |
		    SFGAO_CANLINK;
		if (strlen(it->path) > 3)
			a |= SFGAO_CANRENAME | SFGAO_CANDELETE;
		if (S_ISDIR(st.st_mode))
			a |= SFGAO_FOLDER | SFGAO_FILESYSANCESTOR |
			     SFGAO_STORAGE | SFGAO_BROWSABLE |
			     SFGAO_HASSUBFOLDER;
		else
			a |= SFGAO_STREAM;
		if (access(u, W_OK))
			a |= SFGAO_READONLY;
		if (base_of(it->path)[0] == '.')
			a |= SFGAO_HIDDEN;
		size_t n = strlen(it->path);
		if (n > 4 && !strcasecmp(it->path + n - 4, ".lnk"))
			a |= SFGAO_LINK;
	}
	free(u);
	return a;
}

static HRESULT WINAPI f_GetAttributesOf(IShellFolder *This, UINT n,
					LPCITEMIDLIST *pidls, ULONG *attrs)
{
	if (!attrs || (n && !pidls))
		return E_INVALIDARG;
	ULONG all = n ? 0xffffffff : attributes(&(struct item){
		impl(This)->kind == DESKTOP ? CSIDL_DESKTOP :
		impl(This)->kind == DRIVES ? CSIDL_DRIVES :
		impl(This)->kind == BITBUCKET ? CSIDL_BITBUCKET : -1,
		impl(This)->path });
	for (UINT i = 0; i < n; i++) {
		struct item it;
		if (item_of(pidls[i], &it))
			return E_INVALIDARG;
		all &= attributes(&it);
		free(it.path);
	}
	*attrs &= all;
	return S_OK;
}

static HRESULT WINAPI f_GetUIObjectOf(IShellFolder *This, HWND owner, UINT n,
				      LPCITEMIDLIST *pidls, REFIID riid,
				      UINT *reserved, void **ppv)
{
	if (ppv)
		*ppv = NULL;
	return E_NOINTERFACE;
}

static HRESULT put_strret(STRRET *out, const char *s)
{
	WCHAR *w = utf8_to_utf16_dup(s ? s : "");
	size_t n = w ? (wstrlen(w) + 1) * sizeof(WCHAR) : 0;

	out->uType = STRRET_WSTR;
	out->u.pOleStr = w ? task_alloc(n) : NULL;
	if (out->u.pOleStr)
		memcpy(out->u.pOleStr, w, n);
	free(w);
	return out->u.pOleStr ? S_OK : E_OUTOFMEMORY;
}

static HRESULT WINAPI f_GetDisplayNameOf(IShellFolder *This,
					 LPCITEMIDLIST pidl, DWORD flags,
					 STRRET *name)
{
	struct item it;

	if (!name || !pidl || item_of(pidl, &it))
		return E_INVALIDARG;
	char *s;
	if (!(flags & SHGDN_FORPARSING)) {
		s = shown_name(&it);
	} else if (it.csidl == CSIDL_DRIVES) {
		s = strdup(MY_COMPUTER);
	} else if (it.csidl == CSIDL_BITBUCKET) {
		s = strdup(RECYCLE_BIN);
	} else if (it.csidl == CSIDL_DESKTOP) {
		s = shell_folder_path(CSIDL_DESKTOPDIRECTORY);
	} else if (it.csidl >= 0) {
		s = strdup("");
	} else if (flags & SHGDN_INFOLDER) {
		s = strdup(strlen(it.path) == 3 ? it.path : base_of(it.path));
	} else {
		s = strdup(it.path);
	}
	free(it.path);
	HRESULT hr = s ? put_strret(name, s) : E_OUTOFMEMORY;
	free(s);
	return hr;
}

static HRESULT WINAPI f_SetNameOf(IShellFolder *This, HWND hwnd,
				  LPCITEMIDLIST pidl, LPCWSTR name,
				  DWORD flags, LPITEMIDLIST *out)
{
	struct item it;

	if (out)
		*out = NULL;
	if (!pidl || !name || item_of(pidl, &it))
		return E_INVALIDARG;
	if (it.csidl >= 0 || strlen(it.path) <= 3) {
		free(it.path);
		return E_INVALIDARG;
	}
	char *n = utf16_to_utf8_dup(name);
	char *dir = strdup(it.path);
	HRESULT hr = S_OK;
	if (!n || !dir || !*n || strpbrk(n, "\\/:*?\"<>|")) {
		hr = E_INVALIDARG;
	} else {
		*strrchr(dir, '\\') = 0;
		char *to = win_join(*dir && dir[strlen(dir) - 1] == ':' ?
				    strcat(dir, "\\") : dir, n);
		char *uf = path_to_unix(it.path), *ut = to ? path_to_unix(to) : NULL;
		if (!uf || !ut)
			hr = E_OUTOFMEMORY;
		else if (!access(ut, F_OK) && strcasecmp(uf, ut))
			hr = HRESULT_FROM_WIN32(ERROR_ALREADY_EXISTS);
		else if (rename(uf, ut))
			hr = HRESULT_FROM_WIN32(win_error_from_errno(errno));
		else if (out && !(*out = shell_pidl_from_path(to)))
			hr = E_OUTOFMEMORY;
		free(uf);
		free(ut);
		free(to);
	}
	free(n);
	free(dir);
	free(it.path);
	return hr;
}

static const IShellFolderVtbl folder_vtbl = {
	f_QueryInterface, f_AddRef, f_Release, f_ParseDisplayName,
	f_EnumObjects, f_BindToObject, f_BindToStorage, f_CompareIDs,
	f_CreateViewObject, f_GetAttributesOf, f_GetUIObjectOf,
	f_GetDisplayNameOf, f_SetNameOf,
};

/* IEnumIDList. */

static HRESULT WINAPI e_QueryInterface(IEnumIDList *This, REFIID riid,
				       void **ppv)
{
	if (!ppv)
		return E_POINTER;
	if (same_guid(riid, &IID_IUnknown) || same_guid(riid, &IID_IEnumIDList)) {
		*ppv = This;
		InterlockedIncrement(&eimpl(This)->ref);
		return S_OK;
	}
	*ppv = NULL;
	return E_NOINTERFACE;
}

static ULONG WINAPI e_AddRef(IEnumIDList *This)
{
	return (ULONG)InterlockedIncrement(&eimpl(This)->ref);
}

static ULONG WINAPI e_Release(IEnumIDList *This)
{
	struct enumerator *e = eimpl(This);
	LONG r = InterlockedDecrement(&e->ref);
	if (!r) {
		for (ULONG i = 0; i < e->n; i++)
			pidl_free(e->items[i]);
		free(e->items);
		free(e);
	}
	return (ULONG)r;
}

static LPITEMIDLIST pidl_clone(LPCITEMIDLIST p)
{
	LPITEMIDLIST (WINAPI *ilclone)(LPCITEMIDLIST) =
		builtin_export(&builtin_shell32, "ILClone");
	return ilclone(p);
}

static HRESULT WINAPI e_Next(IEnumIDList *This, ULONG n, LPITEMIDLIST *items,
			     ULONG *fetched)
{
	struct enumerator *e = eimpl(This);
	ULONG got = 0;

	if (!items || (n > 1 && !fetched))
		return E_INVALIDARG;
	while (got < n && e->at < e->n) {
		if (!(items[got] = pidl_clone(e->items[e->at])))
			break;
		got++;
		e->at++;
	}
	if (fetched)
		*fetched = got;
	return got == n ? S_OK : S_FALSE;
}

static HRESULT WINAPI e_Skip(IEnumIDList *This, ULONG n)
{
	struct enumerator *e = eimpl(This);
	ULONG left = e->n - e->at;

	e->at += n < left ? n : left;
	return n <= left ? S_OK : S_FALSE;
}

static HRESULT WINAPI e_Reset(IEnumIDList *This)
{
	eimpl(This)->at = 0;
	return S_OK;
}

static HRESULT WINAPI e_Clone(IEnumIDList *This, IEnumIDList **out)
{
	struct enumerator *e = eimpl(This), *c;

	if (!out)
		return E_INVALIDARG;
	*out = NULL;
	if (!(c = calloc(1, sizeof(*c))))
		return E_OUTOFMEMORY;
	c->iface.lpVtbl = &enum_vtbl;
	c->ref = 1;
	for (ULONG i = 0; i < e->n; i++)
		add_item(c, pidl_clone(e->items[i]));
	c->at = e->at < c->n ? e->at : c->n;
	*out = &c->iface;
	return S_OK;
}

static const IEnumIDListVtbl enum_vtbl = {
	e_QueryInterface, e_AddRef, e_Release, e_Next, e_Skip, e_Reset,
	e_Clone,
};

/* The entry points. */

WINFN(HRESULT) sh_SHGetDesktopFolder(IShellFolder **out)
{
	if (!out)
		return E_INVALIDARG;
	*out = new_folder(DESKTOP, NULL);
	return *out ? S_OK : E_OUTOFMEMORY;
}

/* The folder holding pidl's item, and that item (inside pidl). */
WINFN(HRESULT) sh_SHBindToParent(LPCITEMIDLIST pidl, REFIID riid, void **ppv,
				 LPCITEMIDLIST *last)
{
	struct item it;

	if (!ppv)
		return E_INVALIDARG;
	*ppv = NULL;
	if (last)
		*last = NULL;
	if (!pidl || item_of(pidl, &it))
		return E_INVALIDARG;
	IShellFolder *parent = NULL;
	if (it.csidl >= 0) {
		parent = new_folder(DESKTOP, NULL);
	} else if (strlen(it.path) <= 3) {
		parent = new_folder(DRIVES, NULL);
	} else if (in_bin(it.path)) {
		parent = new_folder(BITBUCKET, NULL);
	} else {
		char *dir = strdup(it.path);
		char *desk = shell_folder_path(CSIDL_DESKTOPDIRECTORY);
		if (dir) {
			*strrchr(dir, '\\') = 0;
			if (strlen(dir) == 2)
				strcat(dir, "\\");
			parent = desk && !strcasecmp(dir, desk) ?
				 new_folder(DESKTOP, NULL) :
				 new_folder(DIRECTORY, dir);
		}
		free(dir);
		free(desk);
	}
	free(it.path);
	if (!parent)
		return E_OUTOFMEMORY;
	HRESULT hr = parent->lpVtbl->QueryInterface(parent, riid, ppv);
	parent->lpVtbl->Release(parent);
	if (hr == S_OK && last) {
		const BYTE *l = shell_pidl_last(pidl);
		*last = (LPCITEMIDLIST)(l ? l : (const BYTE *)pidl);
	}
	return hr;
}

WINFN(HRESULT) sh_SHParseDisplayName(LPCWSTR name, IBindCtx *bc,
				     LPITEMIDLIST *pidl, ULONG want,
				     ULONG *got)
{
	IShellFolder *desk = new_folder(DESKTOP, NULL);
	ULONG attrs = want;

	if (pidl)
		*pidl = NULL;
	if (!desk)
		return E_OUTOFMEMORY;
	if (!name || !pidl) {
		desk->lpVtbl->Release(desk);
		return E_INVALIDARG;
	}
	HRESULT hr = desk->lpVtbl->ParseDisplayName(desk, NULL, bc,
						    (LPWSTR)name, NULL, pidl,
						    want ? &attrs : NULL);
	desk->lpVtbl->Release(desk);
	if (got)
		*got = hr == S_OK && want ? attrs : 0;
	return hr;
}

#define E(name) { #name, 0, (void *)sh_##name }

const struct builtin_export shell_folder_exports[] = {
	E(SHBindToParent),
	E(SHGetDesktopFolder),
	E(SHParseDisplayName),
	BUILTIN_END
};
