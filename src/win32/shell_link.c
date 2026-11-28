/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * shell32's shortcuts: CLSID_ShellLink, with IShellLinkA/W, IPersistFile
 * and IPersistStream, served through shell32's DllGetClassObject (ole32
 * knows the class is here).
 *
 * Shortcuts are read and written as .lnk files (the Shell Link Binary File
 * Format, MS-SHLLINK): a header, then the target as LinkInfo with a local
 * base path (in the ANSI code page) and the strings (description,
 * relative path, working directory, arguments, icon location) in UTF-16.
 * Reading also takes a target from a link's item ID list when it is one
 * of this shell32's, and otherwise from LinkInfo; other item kinds aren't
 * understood.
 */
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

extern const struct builtin_dll builtin_shell32;
extern const struct builtin_dll builtin_ole32;

#define InterlockedIncrement(p) __atomic_add_fetch((p), 1, __ATOMIC_SEQ_CST)
#define InterlockedDecrement(p) __atomic_sub_fetch((p), 1, __ATOMIC_SEQ_CST)

#define SW_SHOWNORMAL 1

/* LinkFlags. */
#define HAS_ID_LIST       0x01
#define HAS_LINK_INFO     0x02
#define HAS_NAME          0x04
#define HAS_RELATIVE_PATH 0x08
#define HAS_WORKING_DIR   0x10
#define HAS_ARGUMENTS     0x20
#define HAS_ICON_LOCATION 0x40
#define IS_UNICODE        0x80

struct link {
	IShellLinkA a;
	IShellLinkW w;
	IPersistFile file;
	IPersistStream stream;
	IShellLinkDataList data;
	LONG ref;
	/* UTF-8, or NULL when not set. */
	char *path, *desc, *rel, *dir, *args, *icon, *cur;
	LPITEMIDLIST pidl;
	int icon_index, show;
	WORD hotkey;
	int dirty;
	/* Extra data blocks as they are stored, terminator left out, and
	 * the LinkFlags bits that don't say which sections are there. */
	BYTE *extra;
	size_t extra_len;
	DWORD sldf;
};

/* The LinkFlags bits that say which sections follow the header. */
#define SECTION_BITS 0xff
/* All the extra data a link keeps, and the blocks it may have. */
#define EXTRA_MAX (1u << 20)
#define BLOCKS_MAX 256

#define FROM(iface, member) \
	((struct link *)((char *)(iface) - offsetof(struct link, member)))

static int same_guid(REFIID a, const GUID *b)
{
	return a && !memcmp(a, b, sizeof(GUID));
}

static void set_str(char **field, char *value)
{
	free(*field);
	*field = value;
}

static char *dup_a(LPCSTR s)
{
	return s ? acp_to_utf8(s) : NULL;
}

static char *dup_w(LPCWSTR s)
{
	return s ? utf16_to_utf8_dup(s) : NULL;
}

/* Copies a UTF-8 string out to a cch-char A or W buffer, truncating. */
static void out(const char *s, void *buf, int cch, int wide)
{
	if (!buf || cch <= 0)
		return;
	if (wide) {
		WCHAR *w = utf8_to_utf16_dup(s ? s : "");
		size_t n = w ? wstrlen(w) : 0;
		if (n >= (size_t)cch)
			n = cch - 1;
		if (w)
			memcpy(buf, w, n * sizeof(WCHAR));
		((WCHAR *)buf)[n] = 0;
		free(w);
	} else {
		char *a = utf8_to_acp_dup(s ? s : "");
		size_t n = a ? strlen(a) : 0;
		if (n >= (size_t)cch)
			n = cch - 1;
		if (a)
			memcpy(buf, a, n);
		((char *)buf)[n] = 0;
		free(a);
	}
}

static void link_free(struct link *l)
{
	free(l->path);
	free(l->desc);
	free(l->rel);
	free(l->dir);
	free(l->args);
	free(l->icon);
	free(l->cur);
	free(l->extra);
	if (l->pidl)
		heap_free(heap_process(), 0, l->pidl);
	free(l);
}

static HRESULT query(struct link *l, REFIID riid, void **ppv)
{
	if (!ppv)
		return E_POINTER;
	if (same_guid(riid, &IID_IUnknown) || same_guid(riid, &IID_IShellLinkA))
		*ppv = &l->a;
	else if (same_guid(riid, &IID_IShellLinkW))
		*ppv = &l->w;
	else if (same_guid(riid, &IID_IPersistFile) ||
		 same_guid(riid, &IID_IPersist))
		*ppv = &l->file;
	else if (same_guid(riid, &IID_IPersistStream))
		*ppv = &l->stream;
	else if (same_guid(riid, &IID_IShellLinkDataList))
		*ppv = &l->data;
	else {
		*ppv = NULL;
		return E_NOINTERFACE;
	}
	InterlockedIncrement(&l->ref);
	return S_OK;
}

static ULONG addref(struct link *l)
{
	return (ULONG)InterlockedIncrement(&l->ref);
}

static ULONG release(struct link *l)
{
	LONG r = InterlockedDecrement(&l->ref);
	if (!r)
		link_free(l);
	return (ULONG)r;
}

/*
 * Extra data blocks. Each is a DWORD size (the whole block), a DWORD
 * signature and the data; they are kept in order, unknown ones too, so a
 * link round-trips what it was given.
 */

/* The block's offset in l->extra, or -1. */
static long find_block(const struct link *l, DWORD sig)
{
	size_t at = 0;

	while (l->extra_len - at >= 8) {
		DWORD size, s;
		memcpy(&size, l->extra + at, 4);
		memcpy(&s, l->extra + at + 4, 4);
		if (s == sig)
			return (long)at;
		at += size;
	}
	return -1;
}

static DWORD block_flag(DWORD sig)
{
	switch (sig) {
	case EXP_SZ_LINK_SIG: return SLDF_HAS_EXP_SZ;
	case EXP_SZ_ICON_SIG: return SLDF_HAS_EXP_ICON_SZ;
	case EXP_DARWIN_ID_SIG: return SLDF_HAS_DARWINID;
	}
	return 0;
}

/* The size a block of a known signature must have, 0 for any (>= 8). */
static DWORD block_size(DWORD sig)
{
	switch (sig) {
	case EXP_SZ_LINK_SIG:
	case EXP_SZ_ICON_SIG:
	case EXP_DARWIN_ID_SIG:
		return sizeof(EXP_SZ_LINK);
	case NT_CONSOLE_PROPS_SIG: return sizeof(NT_CONSOLE_PROPS);
	case NT_FE_CONSOLE_PROPS_SIG: return sizeof(NT_FE_CONSOLE_PROPS);
	case EXP_SPECIAL_FOLDER_SIG: return sizeof(EXP_SPECIAL_FOLDER);
	case EXP_TRACKER_SIG: return 0x60;
	case EXP_KNOWN_FOLDER_SIG: return 0x1c;
	}
	return 0;
}

static int block_ok(DWORD size, DWORD sig)
{
	DWORD want = block_size(sig);
	return size >= 8 && size <= EXTRA_MAX && (!want || size == want);
}

static void remove_block(struct link *l, DWORD sig)
{
	long at = find_block(l, sig);
	DWORD size;

	if (at < 0)
		return;
	memcpy(&size, l->extra + at, 4);
	memmove(l->extra + at, l->extra + at + size,
		l->extra_len - at - size);
	l->extra_len -= size;
	l->sldf &= ~block_flag(sig);
	l->dirty = 1;
}

/* Adds a block, replacing one of the same signature. */
static HRESULT add_block(struct link *l, const void *block)
{
	DWORD size, sig;

	memcpy(&size, block, 4);
	memcpy(&sig, (const BYTE *)block + 4, 4);
	if (!block_ok(size, sig))
		return E_INVALIDARG;
	remove_block(l, sig);
	if (l->extra_len + size > EXTRA_MAX)
		return E_OUTOFMEMORY;
	BYTE *p = realloc(l->extra, l->extra_len + size);
	if (!p)
		return E_OUTOFMEMORY;
	memcpy(p + l->extra_len, block, size);
	l->extra = p;
	l->extra_len += size;
	l->sldf |= block_flag(sig);
	l->dirty = 1;
	return S_OK;
}

/* An EXP_SZ_LINK-shaped block holding s in the code page and in UTF-16;
 * S_FALSE when s won't fit one. */
static HRESULT set_sz_block(struct link *l, DWORD sig, const char *s)
{
	EXP_SZ_LINK b;
	char *a = utf8_to_acp_dup(s);
	WCHAR *w = utf8_to_utf16_dup(s);
	HRESULT hr = E_OUTOFMEMORY;

	if (a && w) {
		hr = S_FALSE;
		if (strlen(a) < MAX_PATH && wstrlen(w) < MAX_PATH) {
			memset(&b, 0, sizeof(b));
			b.dbh.cbSize = sizeof(b);
			b.dbh.dwSignature = sig;
			memcpy(b.szTarget, a, strlen(a));
			memcpy(b.swzTarget, w, wstrlen(w) * sizeof(WCHAR));
			hr = add_block(l, &b);
		}
	}
	free(a);
	free(w);
	return hr;
}

/* The string in an EXP_SZ_LINK-shaped block, as UTF-8: the UTF-16 one
 * when it is there, else the code page one; NULL when neither is. */
static char *sz_block(const struct link *l, DWORD sig)
{
	long at = find_block(l, sig);
	EXP_SZ_LINK b;

	if (at < 0)
		return NULL;
	memcpy(&b, l->extra + at, sizeof(b));
	b.szTarget[MAX_PATH - 1] = 0;
	b.swzTarget[MAX_PATH - 1] = 0;
	if (b.swzTarget[0])
		return utf16_to_utf8_dup(b.swzTarget);
	return b.szTarget[0] ? acp_to_utf8(b.szTarget) : NULL;
}

/* Keeps a path or icon location with %NAME% references in its block, or
 * drops the block when the new one has none. */
static void env_block(struct link *l, DWORD sig, const char *raw)
{
	if (raw && strchr(raw, '%')) {
		if (set_sz_block(l, sig, raw) != S_OK)
			remove_block(l, sig);
	} else {
		remove_block(l, sig);
	}
}

/* The target's details for GetPath's WIN32_FIND_DATA. */
static void find_data(const char *path, void *fd, int wide)
{
	if (!fd)
		return;
	memset(fd, 0, wide ? sizeof(WIN32_FIND_DATAW) :
			     sizeof(WIN32_FIND_DATAA));
	if (!path)
		return;
	char *u = path_to_unix(path);
	struct stat st;
	WIN32_FIND_DATAA *a = fd;
	if (u && !stat(u, &st)) {
		ULONGLONG t = ((ULONGLONG)st.st_mtime + 11644473600ULL) *
			      10000000ULL;
		a->dwFileAttributes = S_ISDIR(st.st_mode) ?
				      FILE_ATTRIBUTE_DIRECTORY :
				      FILE_ATTRIBUTE_ARCHIVE;
		a->ftLastWriteTime.dwLowDateTime = (DWORD)t;
		a->ftLastWriteTime.dwHighDateTime = (DWORD)(t >> 32);
		a->ftCreationTime = a->ftLastAccessTime = a->ftLastWriteTime;
		a->nFileSizeHigh = (DWORD)((ULONGLONG)st.st_size >> 32);
		a->nFileSizeLow = (DWORD)st.st_size;
	}
	free(u);
	const char *base = strrchr(path, '\\');
	base = base ? base + 1 : path;
	if (wide)
		out(base, ((WIN32_FIND_DATAW *)fd)->cFileName, MAX_PATH, 1);
	else
		out(base, a->cFileName, MAX_PATH, 0);
}

/* The A and W methods share these, with wide saying which. */

static HRESULT get_path(struct link *l, void *buf, int cch, void *fd,
			int wide, DWORD flags)
{
	if (!buf || cch <= 0)
		return E_INVALIDARG;
	/* The path as it was set, %NAME% references and all. */
	char *raw = (flags & SLGP_RAWPATH) ? sz_block(l, EXP_SZ_LINK_SIG) :
					     NULL;
	if (raw) {
		out(raw, buf, cch, wide);
		free(raw);
		find_data(l->path, fd, wide);
		return S_OK;
	}
	out(l->path, buf, cch, wide);
	find_data(l->path, fd, wide);
	return l->path ? S_OK : S_FALSE;
}

static HRESULT get_id_list(struct link *l, LPITEMIDLIST *pidl)
{
	LPITEMIDLIST (WINAPI *ilclone)(LPCITEMIDLIST) =
		builtin_export(&builtin_shell32, "ILClone");

	if (!pidl)
		return E_INVALIDARG;
	*pidl = NULL;
	if (l->pidl)
		*pidl = ilclone(l->pidl);
	else if (l->path)
		*pidl = shell_pidl_from_path(l->path);
	else
		return S_FALSE;
	return *pidl ? S_OK : E_OUTOFMEMORY;
}

static HRESULT set_id_list(struct link *l, LPCITEMIDLIST pidl)
{
	LPITEMIDLIST (WINAPI *ilclone)(LPCITEMIDLIST) =
		builtin_export(&builtin_shell32, "ILClone");
	LPITEMIDLIST c = pidl ? ilclone(pidl) : NULL;

	if (pidl && !c)
		return E_OUTOFMEMORY;
	if (l->pidl)
		heap_free(heap_process(), 0, l->pidl);
	l->pidl = c;
	set_str(&l->path, pidl ? shell_pidl_path(pidl) : NULL);
	l->dirty = 1;
	return S_OK;
}

/* A target path made absolute against the current directory. */
static char *full_path(const char *p)
{
	char *(*cwd)(void) = path_getcwd;

	if (!p || !*p)
		return NULL;
	if ((p[0] && p[1] == ':') || (p[0] == '\\' && p[1] == '\\'))
		return strdup(p);
	char *dir = cwd();
	char *r = dir ? malloc(strlen(dir) + strlen(p) + 3) : NULL;
	if (r) {
		if (p[0] == '\\')
			sprintf(r, "%c:%s", dir[0], p);
		else
			sprintf(r, "%s%s%s", dir,
				dir[strlen(dir) - 1] == '\\' ? "" : "\\", p);
	}
	free(dir);
	return r;
}

static HRESULT set_path(struct link *l, char *p)
{
	if (!p)
		return E_INVALIDARG;
	/* A path with %NAME% references is kept as it is in its block, and
	 * the target is its expansion now. */
	char *exp = strchr(p, '%') ? k32_expand_env(p) : NULL;
	if (strchr(p, '%') && !exp) {
		free(p);
		return E_OUTOFMEMORY;
	}
	env_block(l, EXP_SZ_LINK_SIG, *p ? p : NULL);
	char *full = exp ? (*exp ? full_path(exp) : NULL) :
		     *p ? full_path(p) : NULL;
	free(exp);
	free(p);
	if (l->pidl) {
		heap_free(heap_process(), 0, l->pidl);
		l->pidl = NULL;
	}
	set_str(&l->path, full);
	l->dirty = 1;
	if (!full)
		return S_OK;
	char *u = path_to_unix(full);
	int there = u && !access(u, F_OK);
	free(u);
	return there ? S_OK : S_FALSE;
}

/*
 * SetRelativePath's argument is the shortcut's own file name; what is kept
 * is the target relative to it (".\\x.exe", "..\\bin\\x.exe"), or nothing
 * when the two are on different drives or there is no target.
 */
static HRESULT set_relative(struct link *l, char *lnk)
{
	if (!lnk)
		return E_OUTOFMEMORY;
	char *from = full_path(lnk), *rel = NULL;
	free(lnk);
	if (!from)
		return E_OUTOFMEMORY;
	char *slash = strrchr(from, '\\');
	const char *to = l->path;
	if (slash && to && !strncasecmp(from, to, 2)) {
		*slash = 0;
		/* The directories the two share, component by component. */
		size_t common = 0, i = 0;
		for (;; i++) {
			char a = from[i], b = to[i];
			if ((!a || a == '\\') && (b == '\\'))
				common = i;
			if (!a || !b ||
			    k32_toupper_w((unsigned char)a) !=
			    k32_toupper_w((unsigned char)b))
				break;
		}
		int ups = 0;
		for (const char *c = from + common; *c; c++)
			if (*c == '\\')
				ups++;
		if (from[common] && from[common] != '\\')
			ups++;
		const char *rest = to + common + (to[common] == '\\');
		rel = malloc(3 * (size_t)ups + strlen(rest) + 3);
		if (rel) {
			char *o = rel;
			if (!ups)
				o += sprintf(o, ".\\");
			for (int k = 0; k < ups; k++)
				o += sprintf(o, "..\\");
			strcpy(o, rest);
		}
	}
	free(from);
	set_str(&l->rel, rel);
	l->dirty = 1;
	return S_OK;
}

static HRESULT set_field(struct link *l, char **field, char *v, int given)
{
	if (given && !v)
		return E_OUTOFMEMORY;
	set_str(field, v);
	l->dirty = 1;
	return S_OK;
}

/* An icon location with %NAME% references also goes in its block. */
static HRESULT set_icon(struct link *l, char *v, int given, int index)
{
	HRESULT hr = set_field(l, &l->icon, v, given);

	if (hr == S_OK) {
		l->icon_index = index;
		env_block(l, EXP_SZ_ICON_SIG, l->icon);
	}
	return hr;
}

static HRESULT resolve(struct link *l, DWORD flags)
{
	int there = 0;
	if (l->path) {
		char *u = path_to_unix(l->path);
		there = u && !access(u, F_OK);
		free(u);
		if (there)
			return S_OK;
	} else if (!(l->rel && l->cur)) {
		return S_OK;	/* nothing to look for */
	}
	/* The relative path, from where the link was loaded, may find it. */
	if (l->rel && l->cur) {
		char *dir = strdup(l->cur);
		/* Loaded as C:/x/y.lnk is loaded from C:\\x. */
		for (char *c = dir; c && *c; c++)
			if (*c == '/')
				*c = '\\';
		char *slash = dir ? strrchr(dir, '\\') : NULL;
		if (slash) {
			*slash = 0;
			const char *r = l->rel;
			while (r[0] == '.' && r[1] == '\\')
				r += 2;
			/* Each "..\\" climbs out of a directory. */
			while (r[0] == '.' && r[1] == '.' && r[2] == '\\') {
				char *up = strrchr(dir, '\\');
				if (!up)
					break;
				*up = 0;
				r += 3;
			}
			char *p = malloc(strlen(dir) + strlen(r) + 2);
			if (p) {
				sprintf(p, "%s\\%s", dir, r);
				char *pu = path_to_unix(p);
				if (pu && !access(pu, F_OK)) {
					/* The ID list, if any, names the old
					 * place: it goes, so neither GetIDList
					 * nor a save says that place. */
					if (l->pidl) {
						heap_free(heap_process(), 0,
							  l->pidl);
						l->pidl = NULL;
					}
					set_str(&l->path, p);
					p = NULL;
					there = 1;
					if (flags & SLR_UPDATE)
						l->dirty = 1;
				}
				free(pu);
				free(p);
			}
		}
		free(dir);
	}
	return there ? S_OK : HRESULT_FROM_WIN32(ERROR_FILE_NOT_FOUND);
}

/* IShellLinkA. */

#define LA(This) FROM(This, a)

static HRESULT WINAPI a_QueryInterface(IShellLinkA *This, REFIID r, void **p)
{
	return query(LA(This), r, p);
}
static ULONG WINAPI a_AddRef(IShellLinkA *This) { return addref(LA(This)); }
static ULONG WINAPI a_Release(IShellLinkA *This) { return release(LA(This)); }
static HRESULT WINAPI a_GetPath(IShellLinkA *This, LPSTR f, int cch,
				WIN32_FIND_DATAA *fd, DWORD flags)
{
	return get_path(LA(This), f, cch, fd, 0, flags);
}
static HRESULT WINAPI a_GetIDList(IShellLinkA *This, LPITEMIDLIST *p)
{
	return get_id_list(LA(This), p);
}
static HRESULT WINAPI a_SetIDList(IShellLinkA *This, LPCITEMIDLIST p)
{
	return set_id_list(LA(This), p);
}
static HRESULT WINAPI a_GetDescription(IShellLinkA *This, LPSTR s, int cch)
{
	out(LA(This)->desc, s, cch, 0);
	return S_OK;
}
static HRESULT WINAPI a_SetDescription(IShellLinkA *This, LPCSTR s)
{
	return set_field(LA(This), &LA(This)->desc, dup_a(s), s != NULL);
}
static HRESULT WINAPI a_GetWorkingDirectory(IShellLinkA *This, LPSTR s,
					    int cch)
{
	out(LA(This)->dir, s, cch, 0);
	return S_OK;
}
static HRESULT WINAPI a_SetWorkingDirectory(IShellLinkA *This, LPCSTR s)
{
	return set_field(LA(This), &LA(This)->dir, dup_a(s), s != NULL);
}
static HRESULT WINAPI a_GetArguments(IShellLinkA *This, LPSTR s, int cch)
{
	out(LA(This)->args, s, cch, 0);
	return S_OK;
}
static HRESULT WINAPI a_SetArguments(IShellLinkA *This, LPCSTR s)
{
	return set_field(LA(This), &LA(This)->args, dup_a(s), s != NULL);
}
static HRESULT WINAPI a_GetHotkey(IShellLinkA *This, WORD *k)
{
	if (!k)
		return E_INVALIDARG;
	*k = LA(This)->hotkey;
	return S_OK;
}
static HRESULT WINAPI a_SetHotkey(IShellLinkA *This, WORD k)
{
	LA(This)->hotkey = k;
	LA(This)->dirty = 1;
	return S_OK;
}
static HRESULT WINAPI a_GetShowCmd(IShellLinkA *This, int *s)
{
	if (!s)
		return E_INVALIDARG;
	*s = LA(This)->show;
	return S_OK;
}
static HRESULT WINAPI a_SetShowCmd(IShellLinkA *This, int s)
{
	LA(This)->show = s;
	LA(This)->dirty = 1;
	return S_OK;
}
static HRESULT WINAPI a_GetIconLocation(IShellLinkA *This, LPSTR s, int cch,
					int *i)
{
	out(LA(This)->icon, s, cch, 0);
	if (i)
		*i = LA(This)->icon_index;
	return S_OK;
}
static HRESULT WINAPI a_SetIconLocation(IShellLinkA *This, LPCSTR s, int i)
{
	return set_icon(LA(This), dup_a(s), s != NULL, i);
}
static HRESULT WINAPI a_SetRelativePath(IShellLinkA *This, LPCSTR s, DWORD r)
{
	if (!s)
		return E_INVALIDARG;
	return set_relative(LA(This), dup_a(s));
}
static HRESULT WINAPI a_Resolve(IShellLinkA *This, HWND h, DWORD flags)
{
	return resolve(LA(This), flags);
}
static HRESULT WINAPI a_SetPath(IShellLinkA *This, LPCSTR s)
{
	return set_path(LA(This), dup_a(s));
}

static const IShellLinkAVtbl a_vtbl = {
	a_QueryInterface, a_AddRef, a_Release, a_GetPath, a_GetIDList,
	a_SetIDList, a_GetDescription, a_SetDescription,
	a_GetWorkingDirectory, a_SetWorkingDirectory, a_GetArguments,
	a_SetArguments, a_GetHotkey, a_SetHotkey, a_GetShowCmd, a_SetShowCmd,
	a_GetIconLocation, a_SetIconLocation, a_SetRelativePath, a_Resolve,
	a_SetPath,
};

/* IShellLinkW. */

#define LW(This) FROM(This, w)

static HRESULT WINAPI w_QueryInterface(IShellLinkW *This, REFIID r, void **p)
{
	return query(LW(This), r, p);
}
static ULONG WINAPI w_AddRef(IShellLinkW *This) { return addref(LW(This)); }
static ULONG WINAPI w_Release(IShellLinkW *This) { return release(LW(This)); }
static HRESULT WINAPI w_GetPath(IShellLinkW *This, LPWSTR f, int cch,
				WIN32_FIND_DATAW *fd, DWORD flags)
{
	return get_path(LW(This), f, cch, fd, 1, flags);
}
static HRESULT WINAPI w_GetIDList(IShellLinkW *This, LPITEMIDLIST *p)
{
	return get_id_list(LW(This), p);
}
static HRESULT WINAPI w_SetIDList(IShellLinkW *This, LPCITEMIDLIST p)
{
	return set_id_list(LW(This), p);
}
static HRESULT WINAPI w_GetDescription(IShellLinkW *This, LPWSTR s, int cch)
{
	out(LW(This)->desc, s, cch, 1);
	return S_OK;
}
static HRESULT WINAPI w_SetDescription(IShellLinkW *This, LPCWSTR s)
{
	return set_field(LW(This), &LW(This)->desc, dup_w(s), s != NULL);
}
static HRESULT WINAPI w_GetWorkingDirectory(IShellLinkW *This, LPWSTR s,
					    int cch)
{
	out(LW(This)->dir, s, cch, 1);
	return S_OK;
}
static HRESULT WINAPI w_SetWorkingDirectory(IShellLinkW *This, LPCWSTR s)
{
	return set_field(LW(This), &LW(This)->dir, dup_w(s), s != NULL);
}
static HRESULT WINAPI w_GetArguments(IShellLinkW *This, LPWSTR s, int cch)
{
	out(LW(This)->args, s, cch, 1);
	return S_OK;
}
static HRESULT WINAPI w_SetArguments(IShellLinkW *This, LPCWSTR s)
{
	return set_field(LW(This), &LW(This)->args, dup_w(s), s != NULL);
}
static HRESULT WINAPI w_GetHotkey(IShellLinkW *This, WORD *k)
{
	return a_GetHotkey(&LW(This)->a, k);
}
static HRESULT WINAPI w_SetHotkey(IShellLinkW *This, WORD k)
{
	return a_SetHotkey(&LW(This)->a, k);
}
static HRESULT WINAPI w_GetShowCmd(IShellLinkW *This, int *s)
{
	return a_GetShowCmd(&LW(This)->a, s);
}
static HRESULT WINAPI w_SetShowCmd(IShellLinkW *This, int s)
{
	return a_SetShowCmd(&LW(This)->a, s);
}
static HRESULT WINAPI w_GetIconLocation(IShellLinkW *This, LPWSTR s, int cch,
					int *i)
{
	out(LW(This)->icon, s, cch, 1);
	if (i)
		*i = LW(This)->icon_index;
	return S_OK;
}
static HRESULT WINAPI w_SetIconLocation(IShellLinkW *This, LPCWSTR s, int i)
{
	return set_icon(LW(This), dup_w(s), s != NULL, i);
}
static HRESULT WINAPI w_SetRelativePath(IShellLinkW *This, LPCWSTR s, DWORD r)
{
	if (!s)
		return E_INVALIDARG;
	return set_relative(LW(This), dup_w(s));
}
static HRESULT WINAPI w_Resolve(IShellLinkW *This, HWND h, DWORD flags)
{
	return resolve(LW(This), flags);
}
static HRESULT WINAPI w_SetPath(IShellLinkW *This, LPCWSTR s)
{
	return set_path(LW(This), dup_w(s));
}

static const IShellLinkWVtbl w_vtbl = {
	w_QueryInterface, w_AddRef, w_Release, w_GetPath, w_GetIDList,
	w_SetIDList, w_GetDescription, w_SetDescription,
	w_GetWorkingDirectory, w_SetWorkingDirectory, w_GetArguments,
	w_SetArguments, w_GetHotkey, w_SetHotkey, w_GetShowCmd, w_SetShowCmd,
	w_GetIconLocation, w_SetIconLocation, w_SetRelativePath, w_Resolve,
	w_SetPath,
};

/* The .lnk format. */

struct buf {
	BYTE *p;
	size_t len, cap;
	int failed;
};

static void put(struct buf *b, const void *data, size_t n)
{
	if (b->failed)
		return;
	if (b->len + n > b->cap) {
		size_t cap = b->cap ? b->cap * 2 : 512;
		while (cap < b->len + n)
			cap *= 2;
		BYTE *p = realloc(b->p, cap);
		if (!p) {
			b->failed = 1;
			return;
		}
		b->p = p;
		b->cap = cap;
	}
	memcpy(b->p + b->len, data, n);
	b->len += n;
}

static void put32(struct buf *b, DWORD v)
{
	put(b, &v, 4);
}

static void put16(struct buf *b, WORD v)
{
	put(b, &v, 2);
}

/* A StringData entry: a count, then that many UTF-16 characters. */
static void put_string(struct buf *b, const char *s)
{
	WCHAR *w = utf8_to_utf16_dup(s);
	size_t n = w ? wstrlen(w) : 0;

	if (!w || n > 0xffff) {
		free(w);
		b->failed = 1;
		return;
	}
	put16(b, (WORD)n);
	put(b, w, n * sizeof(WCHAR));
	free(w);
}

/* The bytes of an item ID list, its terminator included. */
static size_t pidl_bytes(LPCITEMIDLIST pidl)
{
	const BYTE *p = (const BYTE *)pidl;
	size_t n = 0;
	WORD cb;

	for (;;) {
		memcpy(&cb, p + n, 2);
		if (!cb)
			return n + 2;
		n += cb;
	}
}

static BYTE *serialize(struct link *l, size_t *len)
{
	struct buf b = { 0 };
	DWORD flags = IS_UNICODE;
	char *base = l->path ? utf8_to_acp_dup(l->path) : NULL;
	WCHAR *wbase = l->path ? utf8_to_utf16_dup(l->path) : NULL;
	size_t idlen = l->pidl ? pidl_bytes(l->pidl) : 0;

	if (l->path && (!base || !wbase)) {
		free(base);
		free(wbase);
		return NULL;
	}
	if (idlen > 0xffff) {
		free(base);
		free(wbase);
		return NULL;
	}
	if (l->pidl)
		flags |= HAS_ID_LIST;
	if (base)
		flags |= HAS_LINK_INFO;
	if (l->desc)
		flags |= HAS_NAME;
	if (l->rel)
		flags |= HAS_RELATIVE_PATH;
	if (l->dir)
		flags |= HAS_WORKING_DIR;
	if (l->args)
		flags |= HAS_ARGUMENTS;
	if (l->icon)
		flags |= HAS_ICON_LOCATION;
	flags |= l->sldf & ~SECTION_BITS;
	if (l->sldf & SLDF_FORCE_NO_LINKINFO) {
		free(base);
		free(wbase);
		base = NULL;
		wbase = NULL;
		flags &= ~HAS_LINK_INFO;
	}

	/* The header. */
	BYTE zero[24] = { 0 };
	DWORD attrs = 0, size = 0;
	ULONGLONG t = 0;
	if (l->path) {
		char *u = path_to_unix(l->path);
		struct stat st;
		if (u && !stat(u, &st)) {
			attrs = S_ISDIR(st.st_mode) ? FILE_ATTRIBUTE_DIRECTORY :
						      FILE_ATTRIBUTE_ARCHIVE;
			size = (DWORD)st.st_size;
			t = ((ULONGLONG)st.st_mtime + 11644473600ULL) *
			    10000000ULL;
		}
		free(u);
	}
	put32(&b, 0x4c);
	put(&b, &CLSID_ShellLink, sizeof(GUID));
	put32(&b, flags);
	put32(&b, attrs);
	put(&b, &t, 8);		/* created */
	put(&b, &t, 8);		/* accessed */
	put(&b, &t, 8);		/* written */
	put32(&b, size);
	put32(&b, (DWORD)l->icon_index);
	put32(&b, (DWORD)l->show);
	put16(&b, l->hotkey);
	put(&b, zero, 10);

	/* The target's item IDs, when the link was given them. */
	if (l->pidl) {
		put16(&b, (WORD)idlen);
		put(&b, l->pidl, idlen);
	}

	/* LinkInfo: a fixed volume, the local base path in the ANSI code
	 * page and (header 0x24) in UTF-16, no suffix. */
	if (base) {
		DWORD vol = 0x11;	/* VolumeID: 16 bytes, then "" */
		DWORD hdr = 0x24, at_vol = hdr, at_base = hdr + vol;
		DWORD at_suffix = at_base + (DWORD)strlen(base) + 1;
		DWORD at_wbase = (at_suffix + 1 + 1) & ~1u;	/* aligned */
		DWORD wbytes = (DWORD)(wstrlen(wbase) + 1) * sizeof(WCHAR);
		DWORD at_wsuffix = at_wbase + wbytes;
		DWORD total = at_wsuffix + sizeof(WCHAR);
		put32(&b, total);
		put32(&b, hdr);
		put32(&b, 1);	/* VolumeIDAndLocalBasePath */
		put32(&b, at_vol);
		put32(&b, at_base);
		put32(&b, 0);	/* no network path */
		put32(&b, at_suffix);
		put32(&b, at_wbase);
		put32(&b, at_wsuffix);
		put32(&b, vol);
		put32(&b, 3);	/* DRIVE_FIXED */
		put32(&b, 0x5ea9ce00);	/* serial */
		put32(&b, 0x10);	/* label, empty */
		put(&b, "", 1);
		put(&b, base, strlen(base) + 1);
		put(&b, "", 1);
		if ((at_suffix + 1) & 1)
			put(&b, "", 1);
		put(&b, wbase, wbytes);
		put16(&b, 0);
	}
	free(base);
	free(wbase);
	if (l->desc)
		put_string(&b, l->desc);
	if (l->rel)
		put_string(&b, l->rel);
	if (l->dir)
		put_string(&b, l->dir);
	if (l->args)
		put_string(&b, l->args);
	if (l->icon)
		put_string(&b, l->icon);
	put(&b, l->extra, l->extra_len);
	put32(&b, 0);	/* the terminal block */
	if (b.failed) {
		free(b.p);
		return NULL;
	}
	*len = b.len;
	return b.p;
}

static int get32(const BYTE *p, size_t n, size_t at, DWORD *v)
{
	if (at > n || n - at < 4)
		return -1;
	memcpy(v, p + at, 4);
	return 0;
}

/* A NUL-terminated ANSI string at at, within n bytes, as UTF-8. */
static char *ansi_at(const BYTE *p, size_t n, size_t at)
{
	if (at >= n || !memchr(p + at, 0, n - at))
		return NULL;
	return acp_to_utf8((const char *)p + at);
}

/* A NUL-terminated UTF-16 string at at, within n bytes, as UTF-8. */
static char *wide_at(const BYTE *p, size_t n, size_t at)
{
	size_t k;
	WCHAR c;

	if (at >= n)
		return NULL;
	for (k = at; k + 1 < n; k += 2) {
		memcpy(&c, p + k, 2);
		if (!c)
			break;
	}
	if (k + 1 >= n)
		return NULL;
	WCHAR *w = malloc(k - at + 2);
	if (!w)
		return NULL;
	memcpy(w, p + at, k - at);
	w[(k - at) / 2] = 0;
	char *r = utf16_to_utf8_dup(w);
	free(w);
	return r;
}

/* Whether the n bytes at p are a list of whole items, ending in a zero
 * terminator at the very end. */
static int id_list_ok(const BYTE *p, size_t n)
{
	size_t k = 0;
	WORD cb;

	for (;;) {
		if (n - k < 2)
			return 0;
		memcpy(&cb, p + k, 2);
		if (!cb)
			return k + 2 == n;
		if (cb < 2 || cb > n - k - 2)
			return 0;
		k += cb;
	}
}

/* Base path and suffix, joined; NULL without a base, or without a suffix
 * the link has (one that wouldn't decode): not the base alone. */
static char *join_base(char *base, char *suffix, int has_suffix)
{
	char *path = NULL;

	if (base && (suffix || !has_suffix)) {
		size_t bl = strlen(base);
		path = malloc(bl + (suffix ? strlen(suffix) : 0) + 2);
		if (path)
			sprintf(path, "%s%s%s", base,
				suffix && *suffix && bl &&
				base[bl - 1] != '\\' ? "\\" : "",
				suffix ? suffix : "");
	}
	free(base);
	free(suffix);
	return path;
}

static HRESULT parse(struct link *l, const BYTE *p, size_t n)
{
	DWORD hsize, flags, v;
	HRESULT hr = E_FAIL;

	if (get32(p, n, 0, &hsize) || hsize != 0x4c || n < 0x4c ||
	    memcmp(p + 4, &CLSID_ShellLink, sizeof(GUID)))
		return E_FAIL;
	get32(p, n, 0x14, &flags);
	get32(p, n, 0x38, &v);
	int icon_index = (int)v;
	get32(p, n, 0x3c, &v);
	int show = (int)v;
	WORD hotkey;
	memcpy(&hotkey, p + 0x40, 2);
	size_t at = 0x4c;
	char *path = NULL, *str[5] = { NULL };
	LPITEMIDLIST pidl = NULL;

	if (flags & HAS_ID_LIST) {
		WORD idsize;
		if (n - at < 2)
			return E_FAIL;
		memcpy(&idsize, p + at, 2);
		at += 2;
		if (idsize > n - at)
			return E_FAIL;
		if (idsize) {
			/* Every item has to end inside the list before any of
			 * it is walked. */
			if (!id_list_ok(p + at, idsize))
				return E_FAIL;
			pidl = heap_alloc(heap_process(), 0, idsize);
			if (!pidl)
				return E_OUTOFMEMORY;
			memcpy(pidl, p + at, idsize);
			/* Only this shell32's own items say where they
			 * point; others are kept as they are. */
			path = shell_pidl_path(pidl);
		}
		at += idsize;
	}
	if (flags & HAS_LINK_INFO) {
		DWORD size, lhdr, lflags, base_at, suffix_at;
		DWORD wbase_at = 0, wsuffix_at = 0;
		if (get32(p, n, at, &size) || size < 0x1c || size > n - at)
			goto bad;
		const BYTE *li = p + at;
		get32(li, size, 4, &lhdr);
		get32(li, size, 8, &lflags);
		get32(li, size, 16, &base_at);
		get32(li, size, 24, &suffix_at);
		if (lhdr >= 0x24 && (get32(li, size, 0x1c, &wbase_at) ||
				     get32(li, size, 0x20, &wsuffix_at)))
			goto bad;
		if ((lflags & 1) && !path) {
			/* The UTF-16 path, when there is one, keeps what the
			 * code page can't. */
			if (lhdr >= 0x24 && wbase_at)
				path = join_base(wide_at(li, size, wbase_at),
						 wsuffix_at ?
						 wide_at(li, size, wsuffix_at) :
						 NULL, wsuffix_at != 0);
			if (!path)
				path = join_base(ansi_at(li, size, base_at),
						 suffix_at ?
						 ansi_at(li, size, suffix_at) :
						 NULL, suffix_at != 0);
			/* A local path that won't read whole is an error,
			 * not some other target. */
			if (!path)
				goto bad;
		}
		at += size;
	}
	static const DWORD bits[5] = { HAS_NAME, HAS_RELATIVE_PATH,
				       HAS_WORKING_DIR, HAS_ARGUMENTS,
				       HAS_ICON_LOCATION };
	for (int i = 0; i < 5; i++) {
		if (!(flags & bits[i]))
			continue;
		WORD count;
		if (n - at < 2)
			goto bad;
		memcpy(&count, p + at, 2);
		at += 2;
		if (flags & IS_UNICODE) {
			if (count * 2u > n - at)
				goto bad;
			WCHAR *w = malloc((count + 1) * sizeof(WCHAR));
			if (w) {
				memcpy(w, p + at, count * sizeof(WCHAR));
				w[count] = 0;
				str[i] = utf16_to_utf8_dup(w);
			}
			free(w);
			at += count * 2u;
		} else {
			if (count > n - at)
				goto bad;
			char *a = malloc(count + 1u);
			if (a) {
				memcpy(a, p + at, count);
				a[count] = 0;
				str[i] = acp_to_utf8(a);
			}
			free(a);
			at += count;
		}
		/* A field that is there isn't dropped quietly. */
		if (!str[i]) {
			hr = E_OUTOFMEMORY;
			goto bad;
		}
	}
	/* Extra data, up to the terminal block (a size under 4) or, in a
	 * link that leaves that out, the end. */
	size_t xstart = at;
	for (int i = 0;; i++) {
		DWORD size = 0;
		if (n - at < 4 || (get32(p, n, at, &size), size < 4))
			break;
		if (i == BLOCKS_MAX || size < 8 || size > n - at ||
		    at - xstart + size > EXTRA_MAX)
			goto bad;
		at += size;
	}
	size_t xlen = at - xstart;
	BYTE *extra = xlen ? malloc(xlen) : NULL;
	if (xlen && !extra) {
		hr = E_OUTOFMEMORY;
		goto bad;
	}
	if (xlen)
		memcpy(extra, p + xstart, xlen);
	free(l->extra);
	l->extra = extra;
	l->extra_len = xlen;
	l->sldf = flags & ~SECTION_BITS;
	/* A path or icon kept with %NAME% references is taken from its
	 * block, expanded here. */
	char *raw = (flags & SLDF_HAS_EXP_SZ) ?
		    sz_block(l, EXP_SZ_LINK_SIG) : NULL;
	if (raw) {
		char *e = k32_expand_env(raw);
		char *full = e && *e ? full_path(e) : NULL;
		if (full) {
			free(path);
			path = full;
		}
		free(e);
		free(raw);
	}
	if (!str[4] && (flags & SLDF_HAS_EXP_ICON_SZ))
		str[4] = sz_block(l, EXP_SZ_ICON_SIG);
	set_str(&l->path, path);
	set_str(&l->desc, str[0]);
	set_str(&l->rel, str[1]);
	set_str(&l->dir, str[2]);
	set_str(&l->args, str[3]);
	set_str(&l->icon, str[4]);
	if (l->pidl)
		heap_free(heap_process(), 0, l->pidl);
	l->pidl = pidl;
	l->icon_index = icon_index;
	l->show = show;
	l->hotkey = hotkey;
	l->dirty = 0;
	return S_OK;
bad:
	free(path);
	for (int i = 0; i < 5; i++)
		free(str[i]);
	if (pidl)
		heap_free(heap_process(), 0, pidl);
	return hr;
}

/* IPersistFile. */

#define LF(This) FROM(This, file)

static HRESULT WINAPI pf_QueryInterface(IPersistFile *This, REFIID r, void **p)
{
	return query(LF(This), r, p);
}
static ULONG WINAPI pf_AddRef(IPersistFile *This) { return addref(LF(This)); }
static ULONG WINAPI pf_Release(IPersistFile *This) { return release(LF(This)); }
static HRESULT WINAPI pf_GetClassID(IPersistFile *This, CLSID *c)
{
	if (!c)
		return E_POINTER;
	*c = CLSID_ShellLink;
	return S_OK;
}
static HRESULT WINAPI pf_IsDirty(IPersistFile *This)
{
	return LF(This)->dirty ? S_OK : S_FALSE;
}

static HRESULT WINAPI pf_Load(IPersistFile *This, LPCOLESTR name, DWORD mode)
{
	struct link *l = LF(This);
	char *win = name ? utf16_to_utf8_dup(name) : NULL;
	char *u = win ? path_to_unix(win) : NULL;
	FILE *f = u ? fopen(u, "rb") : NULL;
	HRESULT hr;

	if (!name) {
		hr = E_INVALIDARG;
	} else if (!win) {
		hr = E_OUTOFMEMORY;
	} else if (!u) {
		hr = HRESULT_FROM_WIN32(ERROR_PATH_NOT_FOUND);
	} else if (!f) {
		hr = HRESULT_FROM_WIN32(errno == ENOENT ? ERROR_FILE_NOT_FOUND :
					win_error_from_errno(errno));
	} else {
		BYTE *data = NULL;
		size_t n = 0, cap = 0, got;
		hr = S_OK;
		do {
			if (n == cap) {
				BYTE *p = realloc(data, cap = cap ? cap * 2 : 4096);
				if (!p) {
					hr = E_OUTOFMEMORY;
					break;
				}
				data = p;
			}
			got = fread(data + n, 1, cap - n, f);
			n += got;
		} while (got && n < (1u << 24));
		if (hr == S_OK)
			hr = parse(l, data, n);
		free(data);
		fclose(f);
		if (hr == S_OK)
			set_str(&l->cur, win ? strdup(win) : NULL);
	}
	free(u);
	free(win);
	return hr;
}

/*
 * Writes data to the Unix path u through a temporary file beside it, so an
 * existing shortcut is only replaced by a complete one.
 */
static HRESULT write_file(const char *u, const BYTE *data, size_t n)
{
	char *tmp = malloc(strlen(u) + 16);
	if (!tmp)
		return E_OUTOFMEMORY;
	sprintf(tmp, "%s.XXXXXX", u);
	int fd = mkstemp(tmp);
	if (fd < 0) {
		HRESULT hr = HRESULT_FROM_WIN32(win_error_from_errno(errno));
		free(tmp);
		return hr;
	}
	FILE *f = fdopen(fd, "wb");
	HRESULT hr = S_OK;
	if (!f) {
		close(fd);
		hr = E_OUTOFMEMORY;
	} else {
		/* Private while written; then the old one's permissions. */
		if (fwrite(data, 1, n, f) != n || fflush(f))
			hr = STG_E_WRITEFAULT;
		if (hr == S_OK && path_replace_mode(fd, u))
			hr = STG_E_ACCESSDENIED;
		if (fclose(f) && hr == S_OK)
			hr = STG_E_WRITEFAULT;
	}
	if (hr == S_OK && rename(tmp, u))
		hr = HRESULT_FROM_WIN32(win_error_from_errno(errno));
	if (hr != S_OK)
		unlink(tmp);
	free(tmp);
	return hr;
}

/*
 * Save(NULL) saves to the current file; Save(name, TRUE) saves as name,
 * which becomes current; Save(name, FALSE) saves a copy, and the current
 * file and the dirty flag stay as they were.
 */
static HRESULT WINAPI pf_Save(IPersistFile *This, LPCOLESTR name, BOOL remember)
{
	struct link *l = LF(This);
	char *win = name ? utf16_to_utf8_dup(name) : (l->cur ? strdup(l->cur) : NULL);
	size_t n;
	BYTE *data = NULL;
	HRESULT hr;

	if (!name && !l->cur)
		return E_INVALIDARG;
	if (!win)
		return E_OUTOFMEMORY;
	char *u = path_to_unix(win);
	data = u ? serialize(l, &n) : NULL;
	if (!u)
		hr = HRESULT_FROM_WIN32(ERROR_PATH_NOT_FOUND);
	else if (!data)
		hr = E_OUTOFMEMORY;
	else
		hr = write_file(u, data, n);
	free(data);
	free(u);
	if (hr == S_OK && (!name || remember)) {
		l->dirty = 0;
		set_str(&l->cur, win);
		win = NULL;
	}
	free(win);
	return hr;
}

static HRESULT WINAPI pf_SaveCompleted(IPersistFile *This, LPCOLESTR name)
{
	return S_OK;
}

static HRESULT WINAPI pf_GetCurFile(IPersistFile *This, LPOLESTR *name)
{
	struct link *l = LF(This);
	LPVOID (WINAPI *alloc)(SIZE_T) =
		builtin_export(&builtin_ole32, "CoTaskMemAlloc");

	if (!name)
		return E_POINTER;
	*name = NULL;
	/* With no file yet, S_FALSE and the default prompt. */
	WCHAR *w = utf8_to_utf16_dup(l->cur ? l->cur : "*.lnk");
	size_t n = w ? (wstrlen(w) + 1) * sizeof(WCHAR) : 0;
	*name = w && alloc ? alloc(n) : NULL;
	if (*name)
		memcpy(*name, w, n);
	free(w);
	if (!*name)
		return E_OUTOFMEMORY;
	return l->cur ? S_OK : S_FALSE;
}

static const IPersistFileVtbl pf_vtbl = {
	pf_QueryInterface, pf_AddRef, pf_Release, pf_GetClassID, pf_IsDirty,
	pf_Load, pf_Save, pf_SaveCompleted, pf_GetCurFile,
};

/* IPersistStream: the same bytes, through a stream. */

#define LS(This) FROM(This, stream)

static HRESULT WINAPI ps_QueryInterface(IPersistStream *This, REFIID r,
					void **p)
{
	return query(LS(This), r, p);
}
static ULONG WINAPI ps_AddRef(IPersistStream *This) { return addref(LS(This)); }
static ULONG WINAPI ps_Release(IPersistStream *This) { return release(LS(This)); }
static HRESULT WINAPI ps_GetClassID(IPersistStream *This, CLSID *c)
{
	return pf_GetClassID(&LS(This)->file, c);
}
static HRESULT WINAPI ps_IsDirty(IPersistStream *This)
{
	return LS(This)->dirty ? S_OK : S_FALSE;
}

/* Reads exactly n bytes; 0 or -1. */
static int read_all(IStream *s, void *p, ULONG n)
{
	ULONG got = 0;
	return s->lpVtbl->Read(s, p, n, &got) == S_OK && got == n ? 0 : -1;
}

static HRESULT WINAPI ps_Load(IPersistStream *This, IStream *s)
{
	struct buf b = { 0 };
	BYTE hdr[0x4c];
	DWORD flags;

	if (!s)
		return E_POINTER;
	/* Read section by section, as the stream may hold more after it. */
	if (read_all(s, hdr, sizeof(hdr)))
		return E_FAIL;
	put(&b, hdr, sizeof(hdr));
	memcpy(&flags, hdr + 0x14, 4);
	HRESULT hr = S_OK;
	if (flags & HAS_ID_LIST) {
		WORD n;
		BYTE *p;
		if (read_all(s, &n, 2) || !(p = malloc(n ? n : 1)))
			hr = E_FAIL;
		else {
			if (read_all(s, p, n))
				hr = E_FAIL;
			put(&b, &n, 2);
			put(&b, p, n);
			free(p);
		}
	}
	if (hr == S_OK && (flags & HAS_LINK_INFO)) {
		DWORD n;
		BYTE *p;
		if (read_all(s, &n, 4) || n < 4 || n > (1u << 20) ||
		    !(p = malloc(n)))
			hr = E_FAIL;
		else {
			memcpy(p, &n, 4);
			if (read_all(s, p + 4, n - 4))
				hr = E_FAIL;
			put(&b, p, n);
			free(p);
		}
	}
	static const DWORD bits[5] = { HAS_NAME, HAS_RELATIVE_PATH,
				       HAS_WORKING_DIR, HAS_ARGUMENTS,
				       HAS_ICON_LOCATION };
	for (int i = 0; hr == S_OK && i < 5; i++) {
		if (!(flags & bits[i]))
			continue;
		WORD count;
		size_t bytes;
		BYTE *p;
		if (read_all(s, &count, 2)) {
			hr = E_FAIL;
			break;
		}
		bytes = (size_t)count * ((flags & IS_UNICODE) ? 2 : 1);
		if (!(p = malloc(bytes ? bytes : 1))) {
			hr = E_OUTOFMEMORY;
			break;
		}
		if (read_all(s, p, (ULONG)bytes)) {
			free(p);
			hr = E_FAIL;
			break;
		}
		put(&b, &count, 2);
		put(&b, p, bytes);
		free(p);
	}
	/* Extra data blocks, up to the terminal block, which has to come. */
	int ended = 0;
	size_t extra = 0;
	for (int i = 0; hr == S_OK && i <= BLOCKS_MAX; i++) {
		DWORD n;
		BYTE *p;
		if (read_all(s, &n, 4)) {
			hr = E_FAIL;
			break;
		}
		if (n < 4) {
			put32(&b, 0);
			ended = 1;
			break;
		}
		if (i == BLOCKS_MAX || n < 8 || n > EXTRA_MAX - extra) {
			hr = E_FAIL;
			break;
		}
		if (!(p = malloc(n))) {
			hr = E_OUTOFMEMORY;
			break;
		}
		memcpy(p, &n, 4);
		if (read_all(s, p + 4, n - 4))
			hr = E_FAIL;
		put(&b, p, n);
		free(p);
		extra += n;
	}
	if (hr == S_OK && !ended)
		hr = E_FAIL;
	if (hr == S_OK && b.failed)
		hr = E_OUTOFMEMORY;
	if (hr == S_OK)
		hr = parse(LS(This), b.p, b.len);
	free(b.p);
	/* A link from a stream belongs to no file. */
	if (hr == S_OK)
		set_str(&LS(This)->cur, NULL);
	return hr;
}

static HRESULT WINAPI ps_Save(IPersistStream *This, IStream *s, BOOL clear)
{
	size_t n;
	BYTE *data;
	ULONG wrote = 0;

	if (!s)
		return E_POINTER;
	if (!(data = serialize(LS(This), &n)))
		return E_OUTOFMEMORY;
	/* Cleared first: an edit made during Write stays dirty. */
	int was_dirty = LS(This)->dirty;
	if (clear)
		LS(This)->dirty = 0;
	HRESULT hr = s->lpVtbl->Write(s, data, (ULONG)n, &wrote);
	free(data);
	if (hr == S_OK && wrote != n)
		hr = STG_E_MEDIUMFULL;
	if (clear && hr != S_OK)
		LS(This)->dirty |= was_dirty;
	return hr;
}

static HRESULT WINAPI ps_GetSizeMax(IPersistStream *This, ULARGE_INTEGER *size)
{
	size_t n;
	BYTE *data;

	if (!size)
		return E_POINTER;
	if (!(data = serialize(LS(This), &n)))
		return E_OUTOFMEMORY;
	free(data);
	size->QuadPart = n;
	return S_OK;
}

static const IPersistStreamVtbl ps_vtbl = {
	ps_QueryInterface, ps_AddRef, ps_Release, ps_GetClassID, ps_IsDirty,
	ps_Load, ps_Save, ps_GetSizeMax,
};

/* The class factory, and shell32's DllGetClassObject. */

/* IShellLinkDataList. */

#define LD(This) FROM(This, data)

static HRESULT WINAPI dl_QueryInterface(IShellLinkDataList *This, REFIID r,
					void **p)
{
	return query(LD(This), r, p);
}
static ULONG WINAPI dl_AddRef(IShellLinkDataList *This)
{
	return addref(LD(This));
}
static ULONG WINAPI dl_Release(IShellLinkDataList *This)
{
	return release(LD(This));
}
static HRESULT WINAPI dl_AddDataBlock(IShellLinkDataList *This, void *block)
{
	if (!block)
		return E_INVALIDARG;
	return add_block(LD(This), block);
}
/* The copy is LocalAlloc'd, for the caller's LocalFree. */
static HRESULT WINAPI dl_CopyDataBlock(IShellLinkDataList *This, DWORD sig,
				       void **block)
{
	struct link *l = LD(This);
	long at;
	DWORD size;

	if (!block)
		return E_INVALIDARG;
	*block = NULL;
	if ((at = find_block(l, sig)) < 0)
		return E_FAIL;
	memcpy(&size, l->extra + at, 4);
	if (!(*block = global_alloc(0, size)))
		return E_OUTOFMEMORY;
	memcpy(*block, l->extra + at, size);
	return S_OK;
}
static HRESULT WINAPI dl_RemoveDataBlock(IShellLinkDataList *This, DWORD sig)
{
	struct link *l = LD(This);

	if (find_block(l, sig) < 0)
		return E_FAIL;
	remove_block(l, sig);
	return S_OK;
}
/* The flags a save would write: which sections are there, then the
 * rest as they were set or read. */
static HRESULT WINAPI dl_GetFlags(IShellLinkDataList *This, DWORD *flags)
{
	struct link *l = LD(This);
	DWORD f = SLDF_UNICODE;

	if (!flags)
		return E_INVALIDARG;
	if (l->pidl)
		f |= SLDF_HAS_ID_LIST;
	if (l->path && !(l->sldf & SLDF_FORCE_NO_LINKINFO))
		f |= SLDF_HAS_LINK_INFO;
	if (l->desc)
		f |= SLDF_HAS_NAME;
	if (l->rel)
		f |= SLDF_HAS_RELPATH;
	if (l->dir)
		f |= SLDF_HAS_WORKINGDIR;
	if (l->args)
		f |= SLDF_HAS_ARGS;
	if (l->icon)
		f |= SLDF_HAS_ICONLOCATION;
	*flags = f | (l->sldf & ~SECTION_BITS);
	return S_OK;
}
static HRESULT WINAPI dl_SetFlags(IShellLinkDataList *This, DWORD flags)
{
	struct link *l = LD(This);

	l->sldf = flags & ~SECTION_BITS;
	l->dirty = 1;
	return S_OK;
}

static const IShellLinkDataListVtbl dl_vtbl = {
	dl_QueryInterface, dl_AddRef, dl_Release, dl_AddDataBlock,
	dl_CopyDataBlock, dl_RemoveDataBlock, dl_GetFlags, dl_SetFlags,
};

static HRESULT create_link(IUnknown *outer, REFIID riid, void **ppv)
{
	if (!ppv)
		return E_POINTER;
	*ppv = NULL;
	if (outer)
		return CLASS_E_NOAGGREGATION;
	struct link *l = calloc(1, sizeof(*l));
	if (!l)
		return E_OUTOFMEMORY;
	l->a.lpVtbl = &a_vtbl;
	l->w.lpVtbl = &w_vtbl;
	l->file.lpVtbl = &pf_vtbl;
	l->stream.lpVtbl = &ps_vtbl;
	l->data.lpVtbl = &dl_vtbl;
	l->ref = 1;
	l->show = SW_SHOWNORMAL;
	HRESULT hr = query(l, riid, ppv);
	release(l);
	return hr;
}

static HRESULT WINAPI cf_QueryInterface(IClassFactory *This, REFIID riid,
					void **ppv)
{
	if (!ppv)
		return E_POINTER;
	if (same_guid(riid, &IID_IUnknown) ||
	    same_guid(riid, &IID_IClassFactory)) {
		*ppv = This;
		return S_OK;
	}
	*ppv = NULL;
	return E_NOINTERFACE;
}
static ULONG WINAPI cf_AddRef(IClassFactory *This) { return 2; }
static ULONG WINAPI cf_Release(IClassFactory *This) { return 1; }
static HRESULT WINAPI cf_CreateInstance(IClassFactory *This, IUnknown *outer,
					REFIID riid, void **ppv)
{
	return create_link(outer, riid, ppv);
}
static HRESULT WINAPI cf_LockServer(IClassFactory *This, BOOL lock)
{
	return S_OK;
}

static const IClassFactoryVtbl cf_vtbl = {
	cf_QueryInterface, cf_AddRef, cf_Release, cf_CreateInstance,
	cf_LockServer,
};
static IClassFactory link_factory = { &cf_vtbl };

WINFN(HRESULT) sh_DllGetClassObject(REFCLSID clsid, REFIID riid, void **ppv)
{
	if (!ppv)
		return E_POINTER;
	*ppv = NULL;
	if (!same_guid(clsid, &CLSID_ShellLink))
		return CLASS_E_CLASSNOTAVAILABLE;
	return cf_QueryInterface(&link_factory, riid, ppv);
}

WINFN(HRESULT) sh_DllCanUnloadNow(void)
{
	return S_FALSE;
}

#define E(name) { #name, 0, (void *)sh_##name }

const struct builtin_export shell_link_exports[] = {
	E(DllCanUnloadNow),
	E(DllGetClassObject),
	BUILTIN_END
};
