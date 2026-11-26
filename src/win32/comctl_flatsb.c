/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * comctl32: flat scroll bars and MRU lists.
 *
 * - Flat scroll bars draw nothing here, as no scroll bar does: the
 *   FlatSB_ functions keep and answer user32's scroll bar state, and
 *   InitializeFlatSB gives a window the flat bars' properties (sizes,
 *   colours, styles) for FlatSB_Get/SetScrollProp, which fail on a
 *   window it wasn't called for. UninitializeFlatSB drops them.
 * - An MRU list keeps up to 26 strings or blocks of data under a
 *   registry key: each in a value named by a letter ("a" to "z"), and
 *   the letters in "MRUList", the most recent first. CreateMRUList reads
 *   what is there; adding writes it back at once, or with MRU_CACHEWRITE
 *   when the list is freed. Like Windows', a list is for one thread.
 */
#include <stdlib.h>
#include <string.h>
#include <pthread.h>

#include "win32/comctl.h"
#include "win32/commctrl.h"
#include "win32/internal.h"
#include "win32/winreg.h"

extern const struct builtin_dll builtin_advapi32, builtin_kernel32;

#define ADVAPI32(name) builtin_export(&builtin_advapi32, #name)
#define KERNEL32(name) builtin_export(&builtin_kernel32, #name)

/* Flat scroll bars. */

static struct {
	int (WINAPI *SetScrollInfo)(HWND, int, LPCSCROLLINFO, BOOL);
	BOOL (WINAPI *GetScrollInfo)(HWND, int, LPSCROLLINFO);
	int (WINAPI *SetScrollPos)(HWND, int, int, BOOL);
	int (WINAPI *GetScrollPos)(HWND, int);
	BOOL (WINAPI *SetScrollRange)(HWND, int, int, int, BOOL);
	BOOL (WINAPI *GetScrollRange)(HWND, int, int *, int *);
	BOOL (WINAPI *ShowScrollBar)(HWND, int, BOOL);
	BOOL (WINAPI *EnableScrollBar)(HWND, UINT, UINT);
	BOOL (WINAPI *IsWindow)(HWND);
	LONG (WINAPI *GetWindowLongW)(HWND, int);
	int (WINAPI *GetSystemMetrics)(int);
	DWORD (WINAPI *GetSysColor)(int);
	void (WINAPI *SetLastError)(DWORD);
} sb;
static pthread_once_t sb_once = PTHREAD_ONCE_INIT;

static void sb_init(void)
{
	sb.SetScrollInfo = USER32(SetScrollInfo);
	sb.GetScrollInfo = USER32(GetScrollInfo);
	sb.SetScrollPos = USER32(SetScrollPos);
	sb.GetScrollPos = USER32(GetScrollPos);
	sb.SetScrollRange = USER32(SetScrollRange);
	sb.GetScrollRange = USER32(GetScrollRange);
	sb.ShowScrollBar = USER32(ShowScrollBar);
	sb.EnableScrollBar = USER32(EnableScrollBar);
	sb.IsWindow = USER32(IsWindow);
	sb.GetWindowLongW = USER32(GetWindowLongW);
	sb.GetSystemMetrics = USER32(GetSystemMetrics);
	sb.GetSysColor = USER32(GetSysColor);
	sb.SetLastError = KERNEL32(SetLastError);
}

/* The properties of the windows InitializeFlatSB was called for. */
#define NPROPS 12	/* one per WSB_PROP_ bit */

struct flat {
	HWND hwnd;
	INT_PTR prop[NPROPS];
};

static struct flat *flats;
static size_t nflats;
static pthread_mutex_t flat_lock = PTHREAD_MUTEX_INITIALIZER;

static struct flat *flat_find(HWND h)
{
	for (size_t i = 0; i < nflats; i++)
		if (flats[i].hwnd == h)
			return &flats[i];
	return NULL;
}

/* The index of a single WSB_PROP_ bit, or -1. */
static int prop_index(UINT p)
{
	if (!p || (p & (p - 1)) || p > WSB_PROP_PALETTE)
		return -1;
	return __builtin_ctz(p);
}

WINFN(BOOL) cc_InitializeFlatSB(HWND h)
{
	pthread_once(&sb_once, sb_init);
	if (!sb.IsWindow(h))
		return FALSE;
	pthread_mutex_lock(&flat_lock);
	/* Forget windows destroyed without UninitializeFlatSB. */
	for (size_t i = 0; i < nflats; )
		if (!sb.IsWindow(flats[i].hwnd))
			flats[i] = flats[--nflats];
		else
			i++;
	BOOL ok = TRUE;
	if (!flat_find(h)) {
		struct flat *g = realloc(flats, (nflats + 1) * sizeof(*flats));
		if (!g) {
			ok = FALSE;
		} else {
			flats = g;
			struct flat *f = &flats[nflats++];
			memset(f, 0, sizeof(*f));
			f->hwnd = h;
			f->prop[0] = sb.GetSystemMetrics(20);	/* SM_CYVSCROLL */
			f->prop[1] = sb.GetSystemMetrics(21);	/* SM_CXHSCROLL */
			f->prop[2] = sb.GetSystemMetrics(SM_CYHSCROLL);
			f->prop[3] = sb.GetSystemMetrics(SM_CXVSCROLL);
			f->prop[4] = sb.GetSystemMetrics(SM_CXHTHUMB);
			f->prop[5] = sb.GetSystemMetrics(9);	/* SM_CYVTHUMB */
			f->prop[6] = f->prop[7] = sb.GetSysColor(COLOR_BTNFACE);
			f->prop[8] = f->prop[9] = FSB_REGULAR_MODE;
		}
	}
	pthread_mutex_unlock(&flat_lock);
	return ok;
}

WINFN(HRESULT) cc_UninitializeFlatSB(HWND h)
{
	pthread_mutex_lock(&flat_lock);
	struct flat *f = flat_find(h);
	if (f)
		*f = flats[--nflats];
	pthread_mutex_unlock(&flat_lock);
	return S_OK;
}

static BOOL get_prop(HWND h, UINT index, INT_PTR *out)
{
	int i = prop_index(index);

	pthread_once(&sb_once, sb_init);
	if (!out || i < 0)
		return FALSE;
	if (index == WSB_PROP_WINSTYLE) {	/* the window's own */
		if (!sb.IsWindow(h))
			return FALSE;
		pthread_mutex_lock(&flat_lock);
		BOOL known = flat_find(h) != NULL;
		pthread_mutex_unlock(&flat_lock);
		if (!known)
			return FALSE;
		*out = sb.GetWindowLongW(h, GWL_STYLE) &
		       (WS_HSCROLL | WS_VSCROLL);
		return TRUE;
	}
	pthread_mutex_lock(&flat_lock);
	struct flat *f = flat_find(h);
	if (f)
		*out = f->prop[i];
	pthread_mutex_unlock(&flat_lock);
	return f != NULL;
}

WINFN(BOOL) cc_FlatSB_GetScrollProp(HWND h, INT index, int * out)
{
	INT_PTR v;

	if (!out || !get_prop(h, (UINT)index, &v))
		return FALSE;
	*out = (INT)v;
	return TRUE;
}

WINFN(BOOL) cc_FlatSB_GetScrollPropPtr(HWND h, INT index, INT_PTR *out)
{
	return get_prop(h, (UINT)index, out);
}

/* WSB_PROP_WINSTYLE is read-only. */
WINFN(BOOL) cc_FlatSB_SetScrollProp(HWND h, UINT index, INT_PTR value,
				    BOOL redraw)
{
	int i = prop_index(index);

	if (i < 0 || index == WSB_PROP_WINSTYLE)
		return FALSE;
	if ((index == WSB_PROP_VSTYLE || index == WSB_PROP_HSTYLE) &&
	    (value < FSB_REGULAR_MODE || value > FSB_FLAT_MODE))
		return FALSE;
	pthread_mutex_lock(&flat_lock);
	struct flat *f = flat_find(h);
	if (f)
		f->prop[i] = value;
	pthread_mutex_unlock(&flat_lock);
	return f != NULL;
}

WINFN(BOOL) cc_FlatSB_EnableScrollBar(HWND h, int bar, UINT arrows)
{
	pthread_once(&sb_once, sb_init);
	return sb.EnableScrollBar(h, (UINT)bar, arrows);
}

WINFN(BOOL) cc_FlatSB_ShowScrollBar(HWND h, int bar, BOOL show)
{
	pthread_once(&sb_once, sb_init);
	return sb.ShowScrollBar(h, bar, show);
}

WINFN(BOOL) cc_FlatSB_GetScrollRange(HWND h, int bar, int * min, int * max)
{
	pthread_once(&sb_once, sb_init);
	return sb.GetScrollRange(h, bar, min, max);
}

WINFN(BOOL) cc_FlatSB_GetScrollInfo(HWND h, int bar, LPSCROLLINFO si)
{
	pthread_once(&sb_once, sb_init);
	return sb.GetScrollInfo(h, bar, si);
}

WINFN(int) cc_FlatSB_GetScrollPos(HWND h, int bar)
{
	pthread_once(&sb_once, sb_init);
	return sb.GetScrollPos(h, bar);
}

WINFN(int) cc_FlatSB_SetScrollPos(HWND h, int bar, int pos, BOOL redraw)
{
	pthread_once(&sb_once, sb_init);
	return sb.SetScrollPos(h, bar, pos, redraw);
}

WINFN(int) cc_FlatSB_SetScrollInfo(HWND h, int bar, LPSCROLLINFO si,
				   BOOL redraw)
{
	pthread_once(&sb_once, sb_init);
	return sb.SetScrollInfo(h, bar, si, redraw);
}

WINFN(int) cc_FlatSB_SetScrollRange(HWND h, int bar, int min, int max,
				    BOOL redraw)
{
	pthread_once(&sb_once, sb_init);
	return sb.SetScrollRange(h, bar, min, max, redraw);
}

/* MRU lists. */

#define MRU_MAGIC 0x4c55524d	/* "MRUL" */
#define MRU_LETTERS 26

struct mru_item {
	DWORD size;
	BYTE data[];
};

struct mru {
	DWORD magic;
	int wide;
	UINT flags, max;
	HKEY key;
	void *cmp;			/* the caller's, or NULL */
	struct mru_item *slot[MRU_LETTERS];
	BYTE order[MRU_LETTERS];	/* slots, the most recent first */
	UINT count;
	int dirty;
};

static struct {
	LONG (WINAPI *RegCreateKeyExA)(HKEY, LPCSTR, DWORD, LPSTR, DWORD,
				       REGSAM, void *, PHKEY, LPDWORD);
	LONG (WINAPI *RegCreateKeyExW)(HKEY, LPCWSTR, DWORD, LPWSTR, DWORD,
				       REGSAM, void *, PHKEY, LPDWORD);
	LONG (WINAPI *RegQueryValueExA)(HKEY, LPCSTR, LPDWORD, LPDWORD,
					LPBYTE, LPDWORD);
	LONG (WINAPI *RegSetValueExA)(HKEY, LPCSTR, DWORD, DWORD,
				      const BYTE *, DWORD);
	LONG (WINAPI *RegDeleteValueA)(HKEY, LPCSTR);
	LONG (WINAPI *RegCloseKey)(HKEY);
	int (WINAPI *lstrcmpiA)(LPCSTR, LPCSTR);
	int (WINAPI *lstrcmpiW)(LPCWSTR, LPCWSTR);
} reg;
static pthread_once_t reg_once = PTHREAD_ONCE_INIT;

/* A string list's strings are REG_SZ in its own encoding: the W
 * functions write and read those of a wide list. */
static LONG (WINAPI *set_w)(HKEY, LPCWSTR, DWORD, DWORD, const BYTE *, DWORD);
static LONG (WINAPI *query_w)(HKEY, LPCWSTR, LPDWORD, LPDWORD, LPBYTE,
			      LPDWORD);

static void reg_init(void)
{
	reg.RegCreateKeyExA = ADVAPI32(RegCreateKeyExA);
	reg.RegCreateKeyExW = ADVAPI32(RegCreateKeyExW);
	reg.RegQueryValueExA = ADVAPI32(RegQueryValueExA);
	reg.RegSetValueExA = ADVAPI32(RegSetValueExA);
	reg.RegDeleteValueA = ADVAPI32(RegDeleteValueA);
	reg.RegCloseKey = ADVAPI32(RegCloseKey);
	reg.lstrcmpiA = KERNEL32(lstrcmpiA);
	reg.lstrcmpiW = KERNEL32(lstrcmpiW);
	set_w = ADVAPI32(RegSetValueExW);
	query_w = ADVAPI32(RegQueryValueExW);
}

static struct mru *mru_valid(HANDLE h)
{
	struct mru *m = h;
	return m && m->magic == MRU_MAGIC ? m : NULL;
}

static void name_of(int slot, char a[2], WCHAR w[2])
{
	a[0] = (char)('a' + slot);
	a[1] = 0;
	w[0] = (WCHAR)('a' + slot);
	w[1] = 0;
}

static int write_list(struct mru *m)
{
	char letters[MRU_LETTERS + 1];

	for (UINT i = 0; i < m->count; i++)
		letters[i] = (char)('a' + m->order[i]);
	letters[m->count] = 0;
	int ok = 1;
	for (int s = 0; s < MRU_LETTERS; s++) {
		char a[2];
		WCHAR w[2];
		name_of(s, a, w);
		struct mru_item *it = m->slot[s];
		if (!it)
			continue;
		DWORD type = (m->flags & MRU_BINARY) ? REG_BINARY : REG_SZ;
		LONG r = m->wide && type == REG_SZ ?
			 set_w(m->key, w, 0, type, it->data, it->size) :
			 reg.RegSetValueExA(m->key, a, 0, type, it->data,
					    it->size);
		ok &= r == ERROR_SUCCESS;
	}
	ok &= reg.RegSetValueExA(m->key, "MRUList", 0, REG_SZ,
				 (const BYTE *)letters, m->count + 1) ==
	      ERROR_SUCCESS;
	if (ok)
		m->dirty = 0;
	return ok;
}

static void changed(struct mru *m)
{
	m->dirty = 1;
	if (!(m->flags & MRU_CACHEWRITE))
		write_list(m);
}

/* The size in bytes of a string of the list's encoding, its NUL too. */
static DWORD str_size(const struct mru *m, const void *s)
{
	return m->wide ? (DWORD)(wstrlen(s) + 1) * sizeof(WCHAR) :
			 (DWORD)strlen(s) + 1;
}

static void read_list(struct mru *m)
{
	char letters[64];
	DWORD type, size = sizeof(letters) - 1;

	if (reg.RegQueryValueExA(m->key, "MRUList", NULL, &type,
				 (BYTE *)letters, &size) != ERROR_SUCCESS ||
	    type != REG_SZ)
		return;
	letters[size < sizeof(letters) ? size : sizeof(letters) - 1] = 0;
	for (const char *p = letters; *p && m->count < m->max; p++) {
		int s = *p - 'a';
		if (s < 0 || s >= (int)m->max || m->slot[s])
			continue;	/* not ours, or twice */
		char a[2];
		WCHAR w[2];
		name_of(s, a, w);
		DWORD n = 0, t;
		LONG r = m->wide && !(m->flags & MRU_BINARY) ?
			 query_w(m->key, w, NULL, &t, NULL, &n) :
			 reg.RegQueryValueExA(m->key, a, NULL, &t, NULL, &n);
		if (r != ERROR_SUCCESS || n > 0x100000)
			continue;
		struct mru_item *it = malloc(sizeof(*it) + n + sizeof(WCHAR));
		if (!it)
			continue;
		r = m->wide && !(m->flags & MRU_BINARY) ?
		    query_w(m->key, w, NULL, &t, it->data, &n) :
		    reg.RegQueryValueExA(m->key, a, NULL, &t, it->data, &n);
		if (r != ERROR_SUCCESS) {
			free(it);
			continue;
		}
		if (!(m->flags & MRU_BINARY)) {	/* always terminated */
			memset(it->data + n, 0, sizeof(WCHAR));
			n = str_size(m, it->data);
		}
		it->size = n;
		m->slot[s] = it;
		m->order[m->count++] = (BYTE)s;
	}
}

static HANDLE create(const void *info, int wide)
{
	const MRUINFOA *a = info;	/* the same layout as MRUINFOW */

	pthread_once(&reg_once, reg_init);
	if (!a || a->cbSize != sizeof(MRUINFOA) || !a->hKey ||
	    !a->lpszSubKey || !a->uMax)
		return NULL;
	struct mru *m = calloc(1, sizeof(*m));
	if (!m)
		return NULL;
	m->magic = MRU_MAGIC;
	m->wide = wide;
	m->flags = a->fFlags;
	m->max = a->uMax > MRU_LETTERS ? MRU_LETTERS : a->uMax;
	m->cmp = (void *)a->u.string_cmpfn;
	LONG r = wide ? reg.RegCreateKeyExW(a->hKey, (LPCWSTR)a->lpszSubKey,
					    0, NULL, 0, KEY_ALL_ACCESS, NULL,
					    &m->key, NULL) :
			reg.RegCreateKeyExA(a->hKey, a->lpszSubKey, 0, NULL, 0,
					    KEY_ALL_ACCESS, NULL, &m->key,
					    NULL);
	if (r != ERROR_SUCCESS) {
		free(m);
		return NULL;
	}
	read_list(m);
	return m;
}

WINFN(HANDLE) cc_CreateMRUListA(const MRUINFOA *info) { return create(info, 0); }
WINFN(HANDLE) cc_CreateMRUListW(const MRUINFOW *info) { return create(info, 1); }

/* The lazy forms read the list just the same. */
WINFN(HANDLE) cc_CreateMRUListLazyA(const MRUINFOA *info, DWORD a, DWORD b,
				    DWORD c)
{
	return create(info, 0);
}

WINFN(HANDLE) cc_CreateMRUListLazyW(const MRUINFOW *info, DWORD a, DWORD b,
				    DWORD c)
{
	return create(info, 1);
}

WINFN(void) cc_FreeMRUList(HANDLE h)
{
	struct mru *m = mru_valid(h);

	if (!m)
		return;
	if (m->dirty)
		write_list(m);
	for (int s = 0; s < MRU_LETTERS; s++)
		free(m->slot[s]);
	reg.RegCloseKey(m->key);
	m->magic = 0;
	free(m);
}

static int same(const struct mru *m, const struct mru_item *it,
		const void *data, DWORD size)
{
	if (!(m->flags & MRU_BINARY)) {
		if (m->cmp)
			return m->wide ?
			       !((MRUSTRINGCMPPROCW)m->cmp)(data,
							    (LPCWSTR)it->data) :
			       !((MRUSTRINGCMPPROCA)m->cmp)(data,
							    (LPCSTR)it->data);
		return m->wide ? !reg.lstrcmpiW(data, (LPCWSTR)it->data) :
				 !reg.lstrcmpiA(data, (LPCSTR)it->data);
	}
	if (m->cmp)	/* never past the end of the item */
		return it->size >= size &&
		       !((MRUBINARYCMPPROC)m->cmp)(data, it->data, size);
	return it->size == size && !memcmp(data, it->data, size);
}

/* The position in the list of data, or -1; *slot gets its slot. */
static int find(struct mru *m, const void *data, DWORD size, int *slot)
{
	for (UINT i = 0; i < m->count; i++)
		if (same(m, m->slot[m->order[i]], data, size)) {
			if (slot)
				*slot = m->order[i];
			return (int)i;
		}
	return -1;
}

/* Puts data first: what is there already moves up, else it takes a free
 * slot or the least recent one. Answers its slot, or -1. */
static int add(struct mru *m, const void *data, DWORD size)
{
	int slot, at = find(m, data, size, &slot);

	if (at < 0) {
		struct mru_item *it = malloc(sizeof(*it) + size);
		if (!it)
			return -1;
		it->size = size;
		memcpy(it->data, data, size);
		if (m->count < m->max) {
			for (slot = 0; m->slot[slot]; slot++)
				;
			at = (int)m->count++;
		} else {
			at = (int)m->count - 1;
			slot = m->order[at];
			free(m->slot[slot]);
		}
		m->slot[slot] = it;
	}
	memmove(m->order + 1, m->order, (size_t)at);
	m->order[0] = (BYTE)slot;
	changed(m);
	return slot;
}

WINFN(INT) cc_AddMRUStringA(HANDLE h, LPCSTR s)
{
	struct mru *m = mru_valid(h);

	if (!m || m->wide || (m->flags & MRU_BINARY) || !s)
		return -1;
	return add(m, s, str_size(m, s));
}

WINFN(INT) cc_AddMRUStringW(HANDLE h, LPCWSTR s)
{
	struct mru *m = mru_valid(h);

	if (!m || !m->wide || (m->flags & MRU_BINARY) || !s)
		return -1;
	return add(m, s, str_size(m, s));
}

WINFN(INT) cc_AddMRUData(HANDLE h, LPCVOID data, DWORD size)
{
	struct mru *m = mru_valid(h);

	if (!m || !(m->flags & MRU_BINARY) || (!data && size))
		return -1;
	return add(m, data, size);
}

WINFN(INT) cc_FindMRUStringA(HANDLE h, LPCSTR s, int * slot)
{
	struct mru *m = mru_valid(h);

	if (!m || m->wide || (m->flags & MRU_BINARY) || !s)
		return -1;
	return find(m, s, str_size(m, s), slot);
}

WINFN(INT) cc_FindMRUStringW(HANDLE h, LPCWSTR s, int * slot)
{
	struct mru *m = mru_valid(h);

	if (!m || !m->wide || (m->flags & MRU_BINARY) || !s)
		return -1;
	return find(m, s, str_size(m, s), slot);
}

WINFN(INT) cc_FindMRUData(HANDLE h, LPCVOID data, DWORD size, int * slot)
{
	struct mru *m = mru_valid(h);

	if (!m || !(m->flags & MRU_BINARY) || (!data && size))
		return -1;
	return find(m, data, size, slot);
}

/*
 * Item pos of the list into buf (size bytes for data, characters for a
 * string, truncated and terminated), answering the item's size (data)
 * or the characters copied (a string). A negative pos or no buffer
 * answers the number of items.
 */
static INT enum_list(HANDLE h, INT pos, void *buf, DWORD size, int wide)
{
	struct mru *m = mru_valid(h);

	if (!m)
		return -1;
	if (pos < 0 || !buf)
		return (INT)m->count;
	if ((UINT)pos >= m->count)
		return -1;
	struct mru_item *it = m->slot[m->order[pos]];
	if (m->flags & MRU_BINARY) {
		memcpy(buf, it->data, it->size < size ? it->size : size);
		return (INT)it->size;
	}
	if (!size)
		return 0;
	size_t unit = wide ? sizeof(WCHAR) : 1;
	size_t chars = it->size / unit - 1;
	if (chars > size - 1)
		chars = size - 1;
	memcpy(buf, it->data, chars * unit);
	memset((BYTE *)buf + chars * unit, 0, unit);
	return (INT)chars;
}

WINFN(INT) cc_EnumMRUListA(HANDLE h, INT pos, LPVOID buf, DWORD size)
{
	struct mru *m = mru_valid(h);
	return m && m->wide && !(m->flags & MRU_BINARY) ? -1 :
	       enum_list(h, pos, buf, size, 0);
}

WINFN(INT) cc_EnumMRUListW(HANDLE h, INT pos, LPVOID buf, DWORD size)
{
	struct mru *m = mru_valid(h);
	return m && !m->wide && !(m->flags & MRU_BINARY) ? -1 :
	       enum_list(h, pos, buf, size, 1);
}

/* Removes item pos, its value too. */
WINFN(BOOL) cc_DelMRUString(HANDLE h, INT pos)
{
	struct mru *m = mru_valid(h);

	if (!m || pos < 0 || (UINT)pos >= m->count)
		return FALSE;
	int slot = m->order[pos];
	free(m->slot[slot]);
	m->slot[slot] = NULL;
	memmove(m->order + pos, m->order + pos + 1, m->count - pos - 1);
	m->count--;
	char a[2];
	WCHAR w[2];
	name_of(slot, a, w);
	reg.RegDeleteValueA(m->key, a);
	changed(m);
	return TRUE;
}

#define E(name, ord) { #name, ord, (void *)cc_##name }

const struct builtin_export comctl_flatsb_exports[] = {
	E(CreateMRUListA, 151),
	E(FreeMRUList, 152),
	E(AddMRUStringA, 153),
	E(EnumMRUListA, 154),
	E(FindMRUStringA, 155),
	E(DelMRUString, 156),
	E(CreateMRUListLazyA, 157),
	E(AddMRUData, 167),
	E(FindMRUData, 169),
	E(CreateMRUListW, 400),
	E(AddMRUStringW, 401),
	E(FindMRUStringW, 402),
	E(EnumMRUListW, 403),
	E(CreateMRUListLazyW, 404),
	E(FlatSB_EnableScrollBar, 0),
	E(FlatSB_GetScrollInfo, 0),
	E(FlatSB_GetScrollPos, 0),
	E(FlatSB_GetScrollProp, 0),
	E(FlatSB_GetScrollPropPtr, 0),
	E(FlatSB_GetScrollRange, 0),
	E(FlatSB_SetScrollInfo, 0),
	E(FlatSB_SetScrollPos, 0),
	E(FlatSB_SetScrollProp, 0),
	E(FlatSB_SetScrollRange, 0),
	E(FlatSB_ShowScrollBar, 0),
	E(InitializeFlatSB, 0),
	E(UninitializeFlatSB, 0),
	BUILTIN_END
};
