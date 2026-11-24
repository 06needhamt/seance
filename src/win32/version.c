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
 *
 * A 16-bit (NE) file's version resource has ANSI keys and strings and no
 * type fields; it is read into the 32-bit layout, so the same queries
 * work on it. VerLanguageName names languages from a table of Windows'
 * names; VerFindFile and VerInstallFile place and copy files as setup
 * programs of the 16-bit era expect, expanding COMPRESS.EXE (SZDD) files.
 */
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <pthread.h>
#include <sys/stat.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
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

/* begin_node() with a UTF-16 key. */
static size_t begin_node_w(struct builder *b, const WCHAR *key, WORD type,
			   const void *value, WORD value_len)
{
	align(b);
	size_t start = b->len;
	put16(b, 0);
	put16(b, value_len);
	put16(b, type);
	for (const WCHAR *k = key;; k++) {
		put16(b, *k);
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
	if (len > 0xffff) {	/* too big for its WORD length */
		b->failed = 1;
		return;
	}
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

/*
 * 16-bit files. A node is a WORD length, a WORD value length (bytes), the
 * key (ANSI, NUL-terminated), the value and the children, each aligned
 * to 4 bytes from the block's start. Which values are text isn't
 * recorded: they are the entries of the string tables, the grandchildren
 * of "StringFileInfo".
 */
enum { NE_PLAIN, NE_TABLES, NE_STRINGS };

static int ne_node(struct builder *b, const BYTE *blk, const BYTE *p,
		   const BYTE *limit, int kind, int depth)
{
	if (depth > 8 || limit - p < 5)
		return -1;
	WORD len = rd16(p), vlen = rd16(p + 2);
	if (len < 5 || len > limit - p)
		return -1;
	const BYTE *end = p + len, *key = p + 4;
	const BYTE *z = memchr(key, 0, end - key);
	if (!z)
		return -1;
	const BYTE *val = blk + ((size_t)(z + 1 - blk + 3) & ~(size_t)3);
	if (val > end)
		val = end;
	if (vlen > end - val)
		vlen = (WORD)(end - val);
	const BYTE *kids = blk + ((size_t)(val + vlen - blk + 3) & ~(size_t)3);

	WCHAR wkey[256];
	int kn = cp_to_utf16(CP_ACP, (const char *)key, (int)(z - key), wkey,
			     255, 0);
	wkey[kn < 0 ? 0 : kn] = 0;

	size_t start;
	if (kind == NE_STRINGS) {
		/* No more characters than bytes, and the NUL. */
		WCHAR *w = malloc(((size_t)vlen + 1) * sizeof(WCHAR));
		int n = w && vlen ? cp_to_utf16(CP_ACP, (const char *)val, vlen,
						w, vlen, 0) : 0;
		if (!w || n < 0) {
			free(w);
			b->failed = 1;
			return -1;
		}
		if (!n || w[n - 1])	/* text values end in a NUL */
			w[n++] = 0;
		start = begin_node_w(b, wkey, 1, w, (WORD)n);
		free(w);
	} else {
		start = begin_node_w(b, wkey, 0, val, vlen);
	}

	int child_kind = NE_PLAIN;
	static const WCHAR sfi[] = { 'S', 't', 'r', 'i', 'n', 'g', 'F', 'i',
				     'l', 'e', 'I', 'n', 'f', 'o', 0 };
	if (kind == NE_TABLES)
		child_kind = NE_STRINGS;
	else if (depth == 1) {
		int same = 1;
		for (int i = 0; same && (sfi[i] || wkey[i]); i++)
			if (k32_toupper_w(sfi[i]) != k32_toupper_w(wkey[i]))
				same = 0;
		if (same)
			child_kind = NE_TABLES;
	}
	for (const BYTE *c = kids; c + 5 <= end;) {
		if (ne_node(b, blk, c, end, child_kind, depth + 1))
			break;
		c = blk + ((size_t)(c + rd16(c) - blk + 3) & ~(size_t)3);
	}
	end_node(b, start);
	return 0;
}

/* The NE file's RT_VERSION resource, in the 32-bit layout. */
static BYTE *ne_block(const BYTE *f, size_t n, DWORD *size)
{
	if (n < 0x40 || f[0] != 'M' || f[1] != 'Z')
		return NULL;
	size_t ne = f[0x3c] | f[0x3d] << 8 | (size_t)f[0x3e] << 16 |
		    (size_t)f[0x3f] << 24;
	if (ne > n || n - ne < 0x40 || f[ne] != 'N' || f[ne + 1] != 'E')
		return NULL;
	size_t rt = ne + rd16(f + ne + 0x24);
	if (rd16(f + ne + 0x24) == rd16(f + ne + 0x26) || rt + 2 > n)
		return NULL;	/* no resource table */
	WORD shift = rd16(f + rt);
	if (shift > 16)
		return NULL;
	for (size_t t = rt + 2; t + 8 <= n && rd16(f + t);) {
		WORD type = rd16(f + t), count = rd16(f + t + 2);
		size_t names = t + 8;
		if (names + (size_t)count * 12 > n)
			return NULL;
		if (type == (0x8000 | 16)) {	/* RT_VERSION */
			if (!count)
				return NULL;
			size_t off = (size_t)rd16(f + names) << shift;
			size_t len = (size_t)rd16(f + names + 2) << shift;
			if (off > n || len > n - off)
				len = off > n ? 0 : n - off;
			if (len < 5)
				return NULL;
			WORD blen = rd16(f + off);
			if (blen < len)
				len = blen;
			struct builder b = { 0 };
			if (ne_node(&b, f + off, f + off, f + off + len,
				    NE_PLAIN, 0) || b.failed || !b.len) {
				free(b.buf);
				return NULL;
			}
			*size = (DWORD)b.len;
			return b.buf;
		}
		t = names + (size_t)count * 12;
	}
	return NULL;
}

static BYTE *read_file(const char *path, size_t *n)
{
	FILE *f = fopen(path, "rb");
	BYTE *buf = NULL;
	struct stat st;

	if (!f)
		return NULL;
	if (!fstat(fileno(f), &st) && st.st_size > 0 &&
	    st.st_size < 64 * 1024 * 1024 && (buf = malloc(st.st_size)) &&
	    fread(buf, 1, st.st_size, f) == (size_t)st.st_size) {
		*n = st.st_size;
	} else {
		free(buf);
		buf = NULL;
	}
	fclose(f);
	return buf;
}

static BYTE *file_block(const char *path, DWORD *size)
{
	struct pe_image *img;

	if (access(path, R_OK)) {
		win_set_error(ERROR_FILE_NOT_FOUND);
		return NULL;
	}
	if (!(img = pe_open(path))) {
		size_t n;
		BYTE *f = read_file(path, &n), *block = NULL;
		if (f) {
			block = ne_block(f, n, size);
			free(f);
		}
		if (block)
			return block;
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

	/* A path names that file, KnownDLL or not: this reads it, not loads it. */
	if (base != name) {
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
	if (b && !builtin_shadowable(b, base))
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

/* Language names, as Windows XP gives them. */
static const struct { WORD id; const char *name; } languages[] = {
	{ 0x0000, "Language Neutral" }, { 0x0400, "Process Default Language" },
	{ 0x0401, "Arabic (Saudi Arabia)" }, { 0x0402, "Bulgarian" },
	{ 0x0403, "Catalan" }, { 0x0404, "Chinese (Taiwan)" },
	{ 0x0405, "Czech" }, { 0x0406, "Danish" },
	{ 0x0407, "German (Germany)" }, { 0x0408, "Greek" },
	{ 0x0409, "English (United States)" },
	{ 0x040a, "Spanish (Traditional Sort)" }, { 0x040b, "Finnish" },
	{ 0x040c, "French (France)" }, { 0x040d, "Hebrew" },
	{ 0x040e, "Hungarian" }, { 0x040f, "Icelandic" },
	{ 0x0410, "Italian (Italy)" }, { 0x0411, "Japanese" },
	{ 0x0412, "Korean" }, { 0x0413, "Dutch (Netherlands)" },
	{ 0x0414, "Norwegian (Bokmal)" }, { 0x0415, "Polish" },
	{ 0x0416, "Portuguese (Brazil)" }, { 0x0418, "Romanian" },
	{ 0x0419, "Russian" }, { 0x041a, "Croatian" }, { 0x041b, "Slovak" },
	{ 0x041c, "Albanian" }, { 0x041d, "Swedish" }, { 0x041e, "Thai" },
	{ 0x041f, "Turkish" }, { 0x0420, "Urdu" }, { 0x0421, "Indonesian" },
	{ 0x0422, "Ukrainian" }, { 0x0423, "Belarusian" },
	{ 0x0424, "Slovenian" }, { 0x0425, "Estonian" }, { 0x0426, "Latvian" },
	{ 0x0427, "Lithuanian" }, { 0x0429, "Farsi" }, { 0x042a, "Vietnamese" },
	{ 0x042b, "Armenian" }, { 0x042d, "Basque" },
	{ 0x042f, "FYRO Macedonian" }, { 0x0436, "Afrikaans" },
	{ 0x0437, "Georgian" }, { 0x0438, "Faroese" }, { 0x0439, "Hindi" },
	{ 0x043e, "Malay (Malaysia)" }, { 0x0441, "Swahili" },
	{ 0x0456, "Galician" }, { 0x0804, "Chinese (PRC)" },
	{ 0x0807, "German (Switzerland)" },
	{ 0x0809, "English (United Kingdom)" }, { 0x080a, "Spanish (Mexico)" },
	{ 0x080c, "French (Belgium)" }, { 0x0810, "Italian (Switzerland)" },
	{ 0x0813, "Dutch (Belgium)" }, { 0x0814, "Norwegian (Nynorsk)" },
	{ 0x0816, "Portuguese (Portugal)" }, { 0x081a, "Serbian (Latin)" },
	{ 0x081d, "Swedish (Finland)" },
	{ 0x0c04, "Chinese (Hong Kong S.A.R.)" },
	{ 0x0c07, "German (Austria)" }, { 0x0c09, "English (Australia)" },
	{ 0x0c0a, "Spanish (International Sort)" },
	{ 0x0c0c, "French (Canada)" }, { 0x0c1a, "Serbian (Cyrillic)" },
	{ 0x1004, "Chinese (Singapore)" }, { 0x1009, "English (Canada)" },
	{ 0x100c, "French (Switzerland)" },
	{ 0x1409, "English (New Zealand)" }, { 0x1809, "English (Ireland)" },
	{ 0x1c09, "English (South Africa)" },
};

static const char *language_name(DWORD lang)
{
	for (size_t i = 0; i < sizeof(languages) / sizeof(languages[0]); i++)
		if (languages[i].id == lang)
			return languages[i].name;
	return languages[0].name;	/* unknown: neutral, as Windows */
}

/*
 * The name's length; the buffer gets as much as fits. Past the buffer
 * the return is the length it would need.
 */
WINFN(DWORD) ver_VerLanguageNameA(DWORD lang, LPSTR buf, DWORD size)
{
	const char *name = language_name(lang);
	DWORD n = (DWORD)strlen(name);

	if (buf && size) {
		DWORD c = n < size ? n : size - 1;
		memcpy(buf, name, c);
		buf[c] = '\0';
	}
	return n;
}

WINFN(DWORD) ver_VerLanguageNameW(DWORD lang, LPWSTR buf, DWORD size)
{
	const char *name = language_name(lang);
	DWORD n = (DWORD)strlen(name);

	if (buf && size) {
		DWORD c = n < size ? n : size - 1;
		for (DWORD i = 0; i < c; i++)
			buf[i] = (BYTE)name[i];
		buf[c] = 0;
	}
	return n;
}

/* Setup: where a file goes, and copying it there. */

#define VFFF_ISSHAREDFILE 0x0001
#define VFF_CURNEDEST 0x0001
#define VFF_FILEINUSE 0x0002
#define VFF_BUFFTOOSMALL 0x0004

#define VIFF_FORCEINSTALL 0x0001
#define VIFF_DONTDELETEOLD 0x0002

#define VIF_TEMPFILE 0x00000001
#define VIF_MISMATCH 0x00000002
#define VIF_SRCOLD 0x00000004
#define VIF_DIFFLANG 0x00000008
#define VIF_DIFFCODEPG 0x00000010
#define VIF_DIFFTYPE 0x00000020
#define VIF_WRITEPROT 0x00000040
#define VIF_FILEINUSE 0x00000080
#define VIF_OUTOFSPACE 0x00000100
#define VIF_ACCESSVIOLATION 0x00000200
#define VIF_SHARINGVIOLATION 0x00000400
#define VIF_CANNOTCREATE 0x00000800
#define VIF_CANNOTDELETE 0x00001000
#define VIF_CANNOTRENAME 0x00002000
#define VIF_CANNOTDELETECUR 0x00004000
#define VIF_OUTOFMEMORY 0x00008000
#define VIF_CANNOTREADSRC 0x00010000
#define VIF_CANNOTREADDST 0x00020000
#define VIF_BUFFTOOSMALL 0x00040000

#define VER_SYSTEM_DIR "C:\\WINDOWS\\system32"

/* dir\name (UTF-8, Windows form), malloc()ed. */
static char *join(const char *dir, const char *name)
{
	size_t n = strlen(dir);
	char *p = malloc(n + strlen(name) + 2);

	if (p)
		sprintf(p, "%s%s%s", dir, n && dir[n - 1] != '\\' &&
			dir[n - 1] != '/' ? "\\" : "", name);
	return p;
}

static int win_exists(const char *dir, const char *name)
{
	char *w = join(dir, name), *u = w ? path_to_unix(w) : NULL;
	int r = u && !access(u, F_OK);

	free(w);
	free(u);
	return r;
}

static int same_dir(const char *a, const char *b)
{
	size_t na = strlen(a), nb = strlen(b);

	while (na && (a[na - 1] == '\\' || a[na - 1] == '/'))
		na--;
	while (nb && (b[nb - 1] == '\\' || b[nb - 1] == '/'))
		nb--;
	return na == nb && !strncasecmp(a, b, na);
}

/* Copies s into buf of *size characters; 1 if it didn't fit. */
static int put_out(const char *s, char *buf, UINT *size)
{
	UINT need = (UINT)strlen(s) + 1, room = size ? *size : 0;
	int small = need > room;

	if (buf && room) {
		UINT c = small ? room - 1 : need - 1;
		memcpy(buf, s, c);
		buf[c] = '\0';
	}
	if (size)
		*size = need;
	return small;
}

/*
 * A shared file goes to the system directory, another to the
 * application's; an existing copy is looked for in the system directory,
 * the Windows directory and then the application's (cur is "" if there
 * is none). On NT the Windows directory given is ignored.
 */
static DWORD find_file(DWORD flags, const char *name, const char *appdir,
		       char *cur, UINT *curlen, char *dest, UINT *destlen)
{
	const char *d = (flags & VFFF_ISSHAREDFILE) || !appdir || !*appdir ?
			VER_SYSTEM_DIR : appdir;
	const char *c = "";
	DWORD r = 0;

	if (name && *name) {
		if (win_exists(VER_SYSTEM_DIR, name))
			c = VER_SYSTEM_DIR;
		else if (win_exists("C:\\WINDOWS", name))
			c = "C:\\WINDOWS";
		else if (appdir && *appdir && win_exists(appdir, name))
			c = appdir;
	}
	if (*c && !same_dir(c, d))
		r |= VFF_CURNEDEST;
	if (put_out(c, cur, curlen) | put_out(d, dest, destlen))
		r |= VFF_BUFFTOOSMALL;
	return r;
}

WINFN(DWORD) ver_VerFindFileA(DWORD flags, LPCSTR name, LPCSTR windir,
			      LPCSTR appdir, LPSTR cur, PUINT curlen,
			      LPSTR dest, PUINT destlen)
{
	char *n = acp_to_utf8(name), *a = acp_to_utf8(appdir);
	char c[PATH_MAX], d[PATH_MAX];
	UINT cl = sizeof(c), dl = sizeof(d);
	DWORD r = find_file(flags, n, a, c, &cl, d, &dl);

	free(n);
	free(a);
	r &= ~VFF_BUFFTOOSMALL;
	char *ca = utf8_to_acp_dup(c), *da = utf8_to_acp_dup(d);
	if (put_out(ca ? ca : "", cur, curlen) |
	    put_out(da ? da : "", dest, destlen))
		r |= VFF_BUFFTOOSMALL;
	free(ca);
	free(da);
	return r;
}

/* The W form's lengths are in WCHARs. */
static int put_out_w(const char *s, WCHAR *buf, UINT *size)
{
	WCHAR *w = utf8_to_utf16_dup(s);
	UINT need = 1, room = size ? *size : 0;

	while (w && w[need - 1])
		need++;
	int small = need > room;
	if (buf && room && w) {
		UINT c = small ? room - 1 : need - 1;
		memcpy(buf, w, c * sizeof(WCHAR));
		buf[c] = 0;
	}
	free(w);
	if (size)
		*size = need;
	return small;
}

WINFN(DWORD) ver_VerFindFileW(DWORD flags, LPCWSTR name, LPCWSTR windir,
			      LPCWSTR appdir, LPWSTR cur, PUINT curlen,
			      LPWSTR dest, PUINT destlen)
{
	char *n = utf16_to_utf8_dup(name), *a = utf16_to_utf8_dup(appdir);
	char c[PATH_MAX], d[PATH_MAX];
	UINT cl = sizeof(c), dl = sizeof(d);
	DWORD r = find_file(flags, n, a, c, &cl, d, &dl);

	free(n);
	free(a);
	r &= ~VFF_BUFFTOOSMALL;
	if (put_out_w(c, cur, curlen) | put_out_w(d, dest, destlen))
		r |= VFF_BUFFTOOSMALL;
	return r;
}

/*
 * COMPRESS.EXE's SZDD files: "SZDD\x88\xf0\x27\x33", 'A', the last
 * character of the name, the expanded size, then LZSS with a 4 KiB
 * window of spaces, written from 16 bytes before its end: a flag byte
 * whose bits (low first) say literal (1) or a 12-bit position and a
 * 4-bit length less 3.
 */
static int is_szdd(const BYTE *in, size_t n)
{
	static const BYTE magic[8] = { 'S', 'Z', 'D', 'D', 0x88, 0xf0, 0x27,
				       0x33 };
	return n >= 14 && !memcmp(in, magic, 8) && in[8] == 'A';
}

/* The expansion of an SZDD file (is_szdd), or NULL if it can't be made. */
static BYTE *szdd_expand(const BYTE *in, size_t n, size_t *out_n)
{
	size_t size = in[10] | in[11] << 8 | (size_t)in[12] << 16 |
		      (size_t)in[13] << 24;
	if (size > 256u * 1024 * 1024)
		return NULL;
	BYTE *out = malloc(size ? size : 1), win[4096];
	size_t o = 0, i = 14;
	unsigned pos = 4096 - 16;

	if (!out)
		return NULL;
	memset(win, ' ', sizeof(win));
	while (i < n && o < size) {
		BYTE flags = in[i++];
		for (int bit = 0; bit < 8 && i < n && o < size; bit++) {
			if (flags & (1 << bit)) {
				out[o++] = win[pos] = in[i++];
				pos = (pos + 1) & 4095;
			} else {
				if (i + 1 >= n)
					break;
				unsigned at = in[i] | (in[i + 1] & 0xf0) << 4;
				unsigned len = (in[i + 1] & 0x0f) + 3;
				i += 2;
				for (unsigned k = 0; k < len && o < size; k++) {
					BYTE c = win[(at + k) & 4095];
					out[o++] = win[pos] = c;
					pos = (pos + 1) & 4095;
				}
			}
		}
	}
	if (o != size) {	/* cut short */
		free(out);
		return NULL;
	}
	*out_n = o;
	return out;
}

/* A file's fixed version info and first translation; 0 if it has none. */
static int file_version(const char *unix_path, VS_FIXEDFILEINFO *fixed,
			DWORD *translation)
{
	DWORD size;
	BYTE *block = file_block(unix_path, &size);
	struct node n;
	static const WCHAR root[] = { '\\', 0 };
	static const WCHAR tr[] = { '\\', 'V', 'a', 'r', 'F', 'i', 'l', 'e',
				    'I', 'n', 'f', 'o', '\\', 'T', 'r', 'a',
				    'n', 's', 'l', 'a', 't', 'i', 'o', 'n',
				    0 };
	int ok = 0;

	if (!block)
		return 0;
	if (!query(block, root, &n) && n.value_len >= sizeof(*fixed)) {
		memcpy(fixed, n.value, sizeof(*fixed));
		ok = 1;
	}
	*translation = 0;
	if (!query(block, tr, &n) && n.value_len >= 4)
		memcpy(translation, n.value, 4);
	free(block);
	return ok;
}

/*
 * The source, expanded if it was compressed, written to fd (a new file,
 * which is closed). Compressed data that doesn't expand is unreadable.
 */
static DWORD copy_expanded(const char *src, int fd)
{
	size_t n, out_n = 0;
	BYTE *data = read_file(src, &n);
	DWORD r = 0;

	if (!data) {
		/* An empty file reads as nothing: copy it as such. */
		struct stat st;
		if (stat(src, &st) || st.st_size)
			r = VIF_CANNOTREADSRC;
		n = 0;
	} else if (is_szdd(data, n)) {
		BYTE *exp = szdd_expand(data, n, &out_n);
		free(data);
		data = exp;
		n = out_n;
		if (!exp)
			r = VIF_CANNOTREADSRC;
	}
	for (size_t done = 0; !r && done < n;) {
		ssize_t k = write(fd, data + done, n - done);
		if (k < 0 && errno == EINTR)
			continue;
		if (k <= 0)
			r = VIF_OUTOFSPACE;
		else
			done += (size_t)k;
	}
	if (close(fd) && !r)
		r = VIF_OUTOFSPACE;
	free(data);
	return r;
}

/*
 * Copies src\srcname to a temporary file in dest, and unless forced,
 * compares its version with the one installed (in cur, else dest): an
 * older source, another language, code page or file type leaves the
 * temporary file and says so. Otherwise the temporary file replaces
 * dest\destname, and the copy in cur is deleted unless told not to.
 * Given the temporary file's name from a first call, it is used as is.
 */
static DWORD install_file(DWORD flags, const char *srcname,
			  const char *destname, const char *srcdir,
			  const char *destdir, const char *curdir,
			  char *tmp, size_t tmpsize)
{
	char *src_w = NULL, *src = NULL, *tmp_w = NULL, *tmp_u = NULL;
	char *dst_w = NULL, *dst_u = NULL, *cur_w = NULL, *cur_u = NULL;
	DWORD r = 0;
	int fd = -1;

	if (!srcname || !destname || !destdir)
		return VIF_CANNOTREADSRC;
	if (!srcdir)
		srcdir = "";
	if (!curdir || !*curdir)
		curdir = destdir;
	dst_w = join(destdir, destname);
	cur_w = join(curdir, destname);
	if (!dst_w || !cur_w || !(dst_u = path_to_unix(dst_w)) ||
	    !(cur_u = path_to_unix(cur_w))) {
		r = VIF_OUTOFMEMORY;
		goto out;
	}

	if (*tmp) {
		if (!(tmp_w = join(destdir, tmp)) ||
		    !(tmp_u = path_to_unix(tmp_w))) {
			r = VIF_OUTOFMEMORY;
			goto out;
		}
		if (access(tmp_u, F_OK)) {
			r = VIF_CANNOTREADSRC;
			goto out;
		}
	} else {
		src_w = join(srcdir, srcname);
		if (!src_w || !(src = path_to_unix(src_w))) {
			r = VIF_OUTOFMEMORY;
			goto out;
		}
		if (access(src, R_OK)) {
			r = VIF_CANNOTREADSRC;
			goto out;
		}
		for (unsigned i = 0; i < 0x10000; i++) {
			char name[16];
			snprintf(name, sizeof(name), "VER%04X.TMP", i);
			free(tmp_w);
			free(tmp_u);
			tmp_u = NULL;
			if (!(tmp_w = join(destdir, name)) ||
			    !(tmp_u = path_to_unix(tmp_w))) {
				r = VIF_OUTOFMEMORY;
				goto out;
			}
			/* Made here, so no other install takes it too. */
			fd = open(tmp_u, O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC,
				  0644);
			if (fd >= 0) {
				snprintf(tmp, tmpsize, "%s", name);
				break;
			}
			if (errno != EEXIST) {
				r = errno == EACCES || errno == EROFS ?
				    VIF_WRITEPROT : VIF_CANNOTCREATE;
				goto out;
			}
		}
		if (fd < 0) {
			r = VIF_CANNOTCREATE;
			goto out;
		}
		if ((r = copy_expanded(src, fd))) {
			unlink(tmp_u);
			*tmp = '\0';
			goto out;
		}
	}

	if (!(flags & VIFF_FORCEINSTALL) && !access(cur_u, F_OK)) {
		VS_FIXEDFILEINFO a, b;
		DWORD ta, tb;
		if (file_version(tmp_u, &a, &ta) &&
		    file_version(cur_u, &b, &tb)) {
			if (a.dwFileVersionMS < b.dwFileVersionMS ||
			    (a.dwFileVersionMS == b.dwFileVersionMS &&
			     a.dwFileVersionLS < b.dwFileVersionLS))
				r |= VIF_SRCOLD;
			if ((ta & 0xffff) != (tb & 0xffff))
				r |= VIF_DIFFLANG;
			if ((ta >> 16) != (tb >> 16))
				r |= VIF_DIFFCODEPG;
			if (a.dwFileOS != b.dwFileOS ||
			    a.dwFileType != b.dwFileType ||
			    a.dwFileSubtype != b.dwFileSubtype)
				r |= VIF_DIFFTYPE;
		}
		if (r) {
			r |= VIF_MISMATCH | VIF_TEMPFILE;
			goto out;
		}
	}

	/*
	 * Whether the copy in cur is the one being replaced: the same file,
	 * however its directory was spelt.
	 */
	struct stat cs, ds;
	int same = !strcmp(cur_u, dst_u) ||
		   (!stat(cur_u, &cs) && !stat(dst_u, &ds) &&
		    cs.st_dev == ds.st_dev && cs.st_ino == ds.st_ino);
	if (!access(dst_u, F_OK) && unlink(dst_u)) {
		r = errno == EACCES || errno == EPERM ? VIF_ACCESSVIOLATION :
		    VIF_CANNOTDELETE;
		r |= VIF_TEMPFILE;
		goto out;
	}
	if (rename(tmp_u, dst_u)) {
		r = VIF_CANNOTRENAME | VIF_TEMPFILE;
		goto out;
	}
	*tmp = '\0';
	if (!same && !same_dir(curdir, destdir) &&
	    !(flags & VIFF_DONTDELETEOLD) &&
	    !access(cur_u, F_OK) && unlink(cur_u))
		r = VIF_CANNOTDELETECUR;
out:
	free(src_w);
	free(src);
	free(tmp_w);
	free(tmp_u);
	free(dst_w);
	free(dst_u);
	free(cur_w);
	free(cur_u);
	return r;
}

WINFN(DWORD) ver_VerInstallFileA(DWORD flags, LPCSTR srcname,
				 LPCSTR destname, LPCSTR srcdir,
				 LPCSTR destdir, LPCSTR curdir, LPSTR tmpfile,
				 PUINT tmplen)
{
	char *s = acp_to_utf8(srcname), *d = acp_to_utf8(destname);
	char *sd = acp_to_utf8(srcdir), *dd = acp_to_utf8(destdir);
	char *cd = acp_to_utf8(curdir);
	char tmp[64] = "";

	if (tmpfile && tmplen && *tmplen)
		snprintf(tmp, sizeof(tmp), "%.*s", (int)*tmplen, tmpfile);
	DWORD r = install_file(flags, s, d, sd, dd, cd, tmp, sizeof(tmp));
	if (put_out(tmp, tmpfile, tmplen))
		r |= VIF_BUFFTOOSMALL;
	free(s);
	free(d);
	free(sd);
	free(dd);
	free(cd);
	return r;
}

WINFN(DWORD) ver_VerInstallFileW(DWORD flags, LPCWSTR srcname,
				 LPCWSTR destname, LPCWSTR srcdir,
				 LPCWSTR destdir, LPCWSTR curdir,
				 LPWSTR tmpfile, PUINT tmplen)
{
	char *s = utf16_to_utf8_dup(srcname), *d = utf16_to_utf8_dup(destname);
	char *sd = utf16_to_utf8_dup(srcdir), *dd = utf16_to_utf8_dup(destdir);
	char *cd = utf16_to_utf8_dup(curdir);
	char tmp[64] = "";

	if (tmpfile && tmplen && *tmplen) {
		UINT i = 0;
		for (; i < *tmplen && i < sizeof(tmp) - 1 && tmpfile[i]; i++)
			tmp[i] = (char)tmpfile[i];
		tmp[i] = '\0';
	}
	DWORD r = install_file(flags, s, d, sd, dd, cd, tmp, sizeof(tmp));
	if (put_out_w(tmp, tmpfile, tmplen))
		r |= VIF_BUFFTOOSMALL;
	free(s);
	free(d);
	free(sd);
	free(dd);
	free(cd);
	return r;
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
	{ "VerFindFileA", 0, ver_VerFindFileA },
	{ "VerFindFileW", 0, ver_VerFindFileW },
	{ "VerInstallFileA", 0, ver_VerInstallFileA },
	{ "VerInstallFileW", 0, ver_VerInstallFileW },
	{ "VerLanguageNameA", 0, ver_VerLanguageNameA },
	{ "VerLanguageNameW", 0, ver_VerLanguageNameW },
	BUILTIN_END
};

static const struct builtin_export *const tables[] = { exports, NULL };

const struct builtin_dll builtin_version = { "version.dll", NULL, tables, 0 };
