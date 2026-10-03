/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * version.dll: GetFileVersionInfo* and VerQueryValue, over a file's
 * RT_VERSION resource.
 *
 * The block GetFileVersionInfo hands out is the resource as stored (a
 * tree of VS_VERSIONINFO-style nodes with UTF-16 keys and strings),
 * sized as Windows sizes it: GetFileVersionInfoSize asks for twice the
 * resource plus four bytes, and the "FE2X" signature follows the block.
 * Windows puts VerQueryValueA's ANSI copies of the strings in that second
 * half; here they are kept aside (see intern()), since the buffer holding
 * a block may be smaller.
 *
 * The harness's built-in DLLs have no resources, so they answer with a
 * version block of their own, versioned like Windows XP SP3's system DLLs
 * (5.1.2600.5512), for codecs that check a system DLL's version.
 */
#include <limits.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "common/error.h"
#include "loader/modules.h"
#include "win32/codepage.h"
#include "win32/internal.h"
#include "win32/k32.h"
#include "win32/path.h"

#define BUILTIN_VERSION "5.1.2600.5512"
#define BUILTIN_VERSION_MS 0x00050001u
#define BUILTIN_VERSION_LS ((2600u << 16) | 5512u)

static const char signature[4] = { 'F', 'E', '2', 'X' };

static WORD rd16(const BYTE *p)
{
	return (WORD)(p[0] | p[1] << 8);
}

/* A built version block (for the built-in DLLs). */
struct builder {
	BYTE *buf;
	size_t len, cap;
	int failed;
};

static void put(struct builder *b, const void *data, size_t len)
{
	if (b->failed || !len)
		return;
	if (b->len + len > b->cap) {
		size_t cap = b->cap ? b->cap * 2 : 512;
		while (cap < b->len + len)
			cap *= 2;
		BYTE *buf = realloc(b->buf, cap);
		if (!buf) {
			b->failed = 1;
			return;
		}
		b->buf = buf;
		b->cap = cap;
	}
	if (data)
		memcpy(b->buf + b->len, data, len);
	else
		memset(b->buf + b->len, 0, len);
	b->len += len;
}

static void align(struct builder *b)
{
	put(b, NULL, (4 - (b->len & 3)) & 3);
}

static void put16(struct builder *b, WORD v)
{
	BYTE x[2] = { (BYTE)v, (BYTE)(v >> 8) };
	put(b, x, 2);
}

/*
 * Starts a node: key (ASCII), its type (1 for text) and value, whose
 * length is in WCHARs for text and bytes otherwise. Returns its offset,
 * for end_node() to fill in its length once its children are written.
 */
static size_t begin_node(struct builder *b, const char *key, WORD type,
			 const void *value, WORD value_len)
{
	align(b);
	size_t start = b->len;
	put16(b, 0);
	put16(b, value_len);
	put16(b, type);
	for (const char *k = key;; k++) {
		put16(b, (BYTE)*k);
		if (!*k)
			break;
	}
	align(b);
	put(b, value, type == 1 ? value_len * 2u : value_len);
	return start;
}

static void end_node(struct builder *b, size_t start)
{
	if (b->failed)
		return;
	size_t len = b->len - start;
	b->buf[start] = (BYTE)len;
	b->buf[start + 1] = (BYTE)(len >> 8);
}

static void string_node(struct builder *b, const char *key, const char *value)
{
	WCHAR w[128];
	int n = cp_to_utf16(CP_UTF8, value, -1, w, 128, 0);

	if (n < 0) {
		n = 1;
		w[0] = 0;
	}
	end_node(b, begin_node(b, key, 1, w, (WORD)n));
}

static BYTE *builtin_block(const char *name, DWORD *size)
{
	struct builder b = { 0 };
	VS_FIXEDFILEINFO fixed = {
		.dwSignature = VS_FFI_SIGNATURE,
		.dwStrucVersion = VS_FFI_STRUCVERSION,
		.dwFileVersionMS = BUILTIN_VERSION_MS,
		.dwFileVersionLS = BUILTIN_VERSION_LS,
		.dwProductVersionMS = BUILTIN_VERSION_MS,
		.dwProductVersionLS = BUILTIN_VERSION_LS,
		.dwFileFlagsMask = VS_FFI_FILEFLAGSMASK,
		.dwFileOS = VOS_NT_WINDOWS32,
		.dwFileType = VFT_DLL,
	};
	const WORD translation[2] = { 0x0409, 1200 };
	char desc[128], internal[64];

	snprintf(desc, sizeof(desc), "seance built-in %s", name);
	snprintf(internal, sizeof(internal), "%.*s",
		 (int)strcspn(name, "."), name);

	size_t root = begin_node(&b, "VS_VERSION_INFO", 0, &fixed,
				 sizeof(fixed));
	size_t sfi = begin_node(&b, "StringFileInfo", 1, NULL, 0);
	size_t table = begin_node(&b, "040904b0", 1, NULL, 0);
	string_node(&b, "FileDescription", desc);
	string_node(&b, "FileVersion", BUILTIN_VERSION);
	string_node(&b, "InternalName", internal);
	string_node(&b, "OriginalFilename", name);
	string_node(&b, "ProductName", "seance");
	string_node(&b, "ProductVersion", BUILTIN_VERSION);
	end_node(&b, table);
	end_node(&b, sfi);
	size_t vfi = begin_node(&b, "VarFileInfo", 1, NULL, 0);
	end_node(&b, begin_node(&b, "Translation", 0, translation,
				sizeof(translation)));
	end_node(&b, vfi);
	end_node(&b, root);

	if (b.failed) {
		free(b.buf);
		win_set_error(ERROR_NOT_ENOUGH_MEMORY);
		return NULL;
	}
	*size = (DWORD)b.len;
	return b.buf;
}

/* A copy of img's version resource. */
static BYTE *image_block(const struct pe_image *img, DWORD *size)
{
	struct pe_resource res;
	const void *data;
	BYTE *copy;

	if (!pe_dir(img, IMAGE_DIRECTORY_ENTRY_RESOURCE)) {
		win_set_error(ERROR_RESOURCE_DATA_NOT_FOUND);
		return NULL;
	}
	if (pe_find_resource(img, RT_VERSION, 0, PE_RES_ANY_LANG, &res) ||
	    res.size < 6 || res.size > 0xffff ||
	    !(data = pe_ptr(img, res.data_rva, res.size))) {
		win_set_error(ERROR_RESOURCE_TYPE_NOT_FOUND);
		return NULL;
	}
	if (!(copy = malloc(res.size))) {
		win_set_error(ERROR_NOT_ENOUGH_MEMORY);
		return NULL;
	}
	memcpy(copy, data, res.size);
	*size = res.size;
	return copy;
}

static BYTE *file_block(const char *path, DWORD *size)
{
	struct pe_image *img;

	if (access(path, R_OK)) {
		win_set_error(ERROR_FILE_NOT_FOUND);
		return NULL;
	}
	if (!(img = pe_open(path))) {
		TRACE("GetFileVersionInfo(%s): %s", path, vfw_error());
		win_set_error(ERROR_RESOURCE_DATA_NOT_FOUND);
		return NULL;
	}
	BYTE *block = image_block(img, size);
	pe_free(img);
	return block;
}

/*
 * The version resource of the file name (UTF-8), malloc()ed, or NULL with
 * the last error set. Names resolve as LoadLibrary would: a loaded module
 * by its name, a built-in DLL, then the file on the search path, starting
 * next to the caller at ret.
 */
static BYTE *version_block(const char *name, const void *ret, DWORD *size)
{
	char path[PATH_MAX];
	const char *base = name;

	if (!name || !*name) {
		win_set_error(ERROR_INVALID_PARAMETER);
		return NULL;
	}
	for (const char *p = name; *p; p++)
		if (*p == '\\' || *p == '/')
			base = p + 1;
	const struct builtin_dll *b = builtin_find(base);

	if (base != name) {
		if (b && !b->fallback)
			return builtin_block(b->name, size);
		char *u = path_to_unix(name);
		if (u && !access(u, F_OK)) {
			BYTE *block = file_block(u, size);
			free(u);
			return block;
		}
		free(u);
		if (b)
			return builtin_block(b->name, size);
		win_set_error(ERROR_FILE_NOT_FOUND);
		return NULL;
	}

	struct module *m = mod_find(base);
	if (m) {
		if (!m->img)
			return builtin_block(m->builtin ? m->builtin->name :
					     m->name, size);
		mod_addref(m);
		BYTE *block = image_block(m->img, size);
		mod_free(m);
		return block;
	}
	if (b && !b->fallback)
		return builtin_block(b->name, size);
	if (!mod_search(base, win_caller_module(ret), path, sizeof(path)))
		return file_block(path, size);
	if (b)
		return builtin_block(b->name, size);
	win_set_error(ERROR_FILE_NOT_FOUND);
	return NULL;
}

static DWORD info_size(const char *name, const void *ret, LPDWORD handle)
{
	DWORD size;
	BYTE *block;

	if (handle)
		*handle = 0;
	if (!(block = version_block(name, ret, &size)))
		return 0;
	free(block);
	return size * 2 + sizeof(signature);
}

static BOOL info_get(const char *name, const void *ret, DWORD len, void *data)
{
	DWORD size;
	BYTE *block;

	if (!data || !len) {
		win_set_error(ERROR_INVALID_DATA);
		return FALSE;
	}
	if (!(block = version_block(name, ret, &size)))
		return FALSE;

	/* The block's own length, which queries trust, fits the copy. */
	DWORD total = rd16(block);
	if (total > size)
		total = size;
	if (len < total) {
		total = len & ~3u;
		block[0] = (BYTE)total;
		block[1] = (BYTE)(total >> 8);
	}
	memcpy(data, block, len < size ? len : size);
	free(block);
	/* The signature Windows leaves where the ANSI half starts. */
	if (len >= total + sizeof(signature))
		memcpy((BYTE *)data + total, signature, sizeof(signature));
	return TRUE;
}

WINFN(DWORD) ver_GetFileVersionInfoSizeA(LPCSTR name, LPDWORD handle)
{
	char *u = acp_to_utf8(name);
	DWORD n = info_size(u, __builtin_return_address(0), handle);
	free(u);
	return n;
}

WINFN(DWORD) ver_GetFileVersionInfoSizeW(LPCWSTR name, LPDWORD handle)
{
	char *u = utf16_to_utf8_dup(name);
	DWORD n = info_size(u, __builtin_return_address(0), handle);
	free(u);
	return n;
}

/* The Ex forms' flags (localised or neutral resources) change nothing. */
WINFN(DWORD) ver_GetFileVersionInfoSizeExA(DWORD flags, LPCSTR name,
					   LPDWORD handle)
{
	char *u = acp_to_utf8(name);
	DWORD n = info_size(u, __builtin_return_address(0), handle);
	free(u);
	return n;
}

WINFN(DWORD) ver_GetFileVersionInfoSizeExW(DWORD flags, LPCWSTR name,
					   LPDWORD handle)
{
	char *u = utf16_to_utf8_dup(name);
	DWORD n = info_size(u, __builtin_return_address(0), handle);
	free(u);
	return n;
}

WINFN(BOOL) ver_GetFileVersionInfoA(LPCSTR name, DWORD handle, DWORD len,
				    LPVOID data)
{
	char *u = acp_to_utf8(name);
	BOOL ok = info_get(u, __builtin_return_address(0), len, data);
	free(u);
	return ok;
}

WINFN(BOOL) ver_GetFileVersionInfoW(LPCWSTR name, DWORD handle, DWORD len,
				    LPVOID data)
{
	char *u = utf16_to_utf8_dup(name);
	BOOL ok = info_get(u, __builtin_return_address(0), len, data);
	free(u);
	return ok;
}

WINFN(BOOL) ver_GetFileVersionInfoExA(DWORD flags, LPCSTR name, DWORD handle,
				      DWORD len, LPVOID data)
{
	char *u = acp_to_utf8(name);
	BOOL ok = info_get(u, __builtin_return_address(0), len, data);
	free(u);
	return ok;
}

WINFN(BOOL) ver_GetFileVersionInfoExW(DWORD flags, LPCWSTR name,
				      DWORD handle, DWORD len, LPVOID data)
{
	char *u = utf16_to_utf8_dup(name);
	BOOL ok = info_get(u, __builtin_return_address(0), len, data);
	free(u);
	return ok;
}

/* A node of a version block, bounds-checked against its parent. */
struct node {
	const BYTE *start, *end;
	const BYTE *key;		/* UTF-16, NUL-terminated within end */
	const BYTE *value;
	WORD value_len;			/* WCHARs for text, else bytes */
	WORD type;			/* 1: text */
	const BYTE *children;
};

static const BYTE *align4(const BYTE *block, const BYTE *p)
{
	return block + ((size_t)(p - block + 3) & ~(size_t)3);
}

static int parse_node(const BYTE *block, const BYTE *p, const BYTE *limit,
		      struct node *n)
{
	if (limit - p < 6)
		return -1;
	WORD len = rd16(p);
	if (len < 6 || len > limit - p)
		return -1;
	n->start = p;
	n->end = p + len;
	n->value_len = rd16(p + 2);
	n->type = rd16(p + 4);
	n->key = p + 6;

	const BYTE *k = n->key;
	while (k + 2 <= n->end && rd16(k))
		k += 2;
	if (k + 2 > n->end)
		return -1;
	n->value = align4(block, k + 2);
	size_t bytes = n->type == 1 ? n->value_len * 2u : n->value_len;
	if (n->value > n->end || bytes > (size_t)(n->end - n->value)) {
		/* A missing or short value: report none. */
		if (n->value > n->end)
			n->value = n->end;
		n->value_len = 0;
		bytes = 0;
	}
	n->children = align4(block, n->value + bytes);
	return 0;
}

static int key_equals(const BYTE *key, const WCHAR *want, size_t n)
{
	for (size_t i = 0; i < n; i++) {
		WCHAR c = rd16(key + 2 * i);
		if (!c || k32_toupper_w(c) != k32_toupper_w(want[i]))
			return 0;
	}
	return rd16(key + 2 * n) == 0;
}

/* Walks sub (e.g. "\StringFileInfo\040904b0\FileVersion") down block. */
static int query(const BYTE *block, const WCHAR *sub, struct node *out)
{
	struct node n;

	if (!block || !sub || parse_node(block, block, block + rd16(block), &n))
		return -1;
	while (*sub) {
		while (*sub == '\\')
			sub++;
		if (!*sub)
			break;
		size_t len = 0;
		while (sub[len] && sub[len] != '\\')
			len++;

		struct node child;
		int found = 0;
		for (const BYTE *p = n.children; p < n.end && !found;
		     p = align4(block, child.end)) {
			if (parse_node(block, p, n.end, &child))
				break;
			found = key_equals(child.key, sub, len);
		}
		if (!found)
			return -1;
		n = child;
		sub += len;
	}
	*out = n;
	return 0;
}

WINFN(BOOL) ver_VerQueryValueW(LPCVOID block, LPCWSTR sub, LPVOID *buf,
			       PUINT len)
{
	struct node n;

	if (query(block, sub, &n)) {
		if (len)
			*len = 0;
		win_set_error(ERROR_RESOURCE_TYPE_NOT_FOUND);
		return FALSE;
	}
	if (buf)
		*buf = (void *)n.value;
	if (len)
		*len = n.value_len;
	return TRUE;
}

/*
 * VerQueryValueA's ANSI copies of strings. Windows writes them into the
 * room GetFileVersionInfo leaves after the block, but VerQueryValueA is
 * given only the block, not the size of the buffer holding it, which may
 * be smaller (a truncated copy, or a raw resource from LockResource). So
 * they are kept here instead, for good and once per distinct string, so
 * the pointers handed out stay valid.
 */
struct interned {
	struct interned *next;
	size_t len;
	char s[];
};
static struct interned *interned;
static pthread_mutex_t intern_lock = PTHREAD_MUTEX_INITIALIZER;

static char *intern(const char *s, size_t len)
{
	struct interned *i;

	pthread_mutex_lock(&intern_lock);
	for (i = interned; i; i = i->next)
		if (i->len == len && !memcmp(i->s, s, len))
			break;
	if (!i && (i = heap_alloc(heap_process(), 0, sizeof(*i) + len + 1))) {
		i->len = len;
		memcpy(i->s, s, len);
		i->s[len] = '\0';
		i->next = interned;
		interned = i;
	}
	pthread_mutex_unlock(&intern_lock);
	return i ? i->s : NULL;
}

WINFN(BOOL) ver_VerQueryValueA(LPCVOID block, LPCSTR sub, LPVOID *buf,
			       PUINT len)
{
	const BYTE *b = block;
	struct node n;
	int wn = sub ? cp_to_utf16(CP_ACP, sub, -1, NULL, 0, 0) : -1;
	WCHAR *wsub = wn > 0 ? malloc(wn * sizeof(WCHAR)) : NULL;
	int found;

	if (wsub)
		cp_to_utf16(CP_ACP, sub, -1, wsub, wn, 0);
	found = wsub && !query(b, wsub, &n);
	free(wsub);
	if (!found) {
		if (len)
			*len = 0;
		win_set_error(ERROR_RESOURCE_TYPE_NOT_FOUND);
		return FALSE;
	}

	UINT out_len = n.value_len;
	const void *out = n.value;
	if (n.type == 1 && n.value_len) {
		WCHAR *w = malloc(n.value_len * sizeof(WCHAR));
		char *a = NULL;
		int an = -1;

		if (w) {
			memcpy(w, n.value, n.value_len * sizeof(WCHAR));
			an = cp_from_utf16(CP_ACP, w, n.value_len, NULL, 0,
					   '?', NULL);
		}
		if (an > 0 && (a = malloc(an)))
			cp_from_utf16(CP_ACP, w, n.value_len, a, an, '?', NULL);
		free(w);
		if (!a) {
			if (len)
				*len = 0;
			win_set_error(ERROR_NOT_ENOUGH_MEMORY);
			return FALSE;
		}

		out = intern(a, an && !a[an - 1] ? an - 1 : an);
		free(a);
		out_len = an;
		if (!out) {
			if (len)
				*len = 0;
			win_set_error(ERROR_NOT_ENOUGH_MEMORY);
			return FALSE;
		}
	}
	if (buf)
		*buf = (void *)out;
	if (len)
		*len = out_len;
	return TRUE;
}

static const struct builtin_export exports[] = {
	{ "GetFileVersionInfoA", 0, ver_GetFileVersionInfoA },
	{ "GetFileVersionInfoExA", 0, ver_GetFileVersionInfoExA },
	{ "GetFileVersionInfoExW", 0, ver_GetFileVersionInfoExW },
	{ "GetFileVersionInfoSizeA", 0, ver_GetFileVersionInfoSizeA },
	{ "GetFileVersionInfoSizeExA", 0, ver_GetFileVersionInfoSizeExA },
	{ "GetFileVersionInfoSizeExW", 0, ver_GetFileVersionInfoSizeExW },
	{ "GetFileVersionInfoSizeW", 0, ver_GetFileVersionInfoSizeW },
	{ "GetFileVersionInfoW", 0, ver_GetFileVersionInfoW },
	{ "VerQueryValueA", 0, ver_VerQueryValueA },
	{ "VerQueryValueW", 0, ver_VerQueryValueW },
	BUILTIN_END
};

static const struct builtin_export *const tables[] = { exports, NULL };

const struct builtin_dll builtin_version = { "version.dll", NULL, tables, 0 };
