/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * ole32's structured storage: compound files ("docfiles"), from the
 * [MS-CFB] format description, on byte arrays (ILockBytes) over global
 * memory or a file.
 *
 * A storage is kept in memory as a tree of storages and streams, read
 * whole when it is opened and written whole (a version 3 file: 512-byte
 * sectors, 64-byte mini sectors below 4096 bytes) when it is committed,
 * or, in direct mode, also when its last object is released. A
 * transacted root can be reverted, which reads the file again; objects
 * opened before then answer STG_E_REVERTED. Committing or reverting a
 * storage below the root does nothing, as its changes are the root's.
 *
 * Also here: the CLSID and user type helpers (ReadClassStg, WriteClassStm,
 * WriteFmtUserTypeStg and so on, the latter in the "\1CompObj" stream of
 * [MS-OLEDS]) and OleSave/OleLoad and their stream forms.
 */
#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#include "common/log.h"
#include "win32/codepage.h"
#include "win32/internal.h"
#include "win32/k32.h"
#include "win32/ole.h"
#include "win32/path.h"

#define SECT       512
#define MINI       64
#define CUTOFF     4096
#define FREESECT   0xffffffffu
#define ENDOFCHAIN 0xfffffffeu
#define FATSECT    0xfffffffdu
#define DIFSECT    0xfffffffcu
#define NOSTREAM   0xffffffffu
#define NAME_MAX_CHARS 31

#define STGTY_ROOT 5

static const BYTE signature[8] = {
	0xd0, 0xcf, 0x11, 0xe0, 0xa1, 0xb1, 0x1a, 0xe1
};

/* Callers into a storage may call another (CopyTo), so this recurses. */
static pthread_mutex_t lock = PTHREAD_RECURSIVE_MUTEX_INITIALIZER_NP;

struct file;

/* A storage or stream in the tree; the tree holds one reference. */
struct node {
	LONG refs;
	struct node *parent, *child, *next;
	WCHAR name[NAME_MAX_CHARS + 1];
	int len;
	BYTE type;
	CLSID clsid;
	DWORD state;
	FILETIME ctime, mtime;
	BYTE *data;
	ULONG size, cap;
	int open;
	int gone;
};

/* An open compound file: its bytes, mode and tree. */
struct file {
	LONG refs;
	ILockBytes *lb;
	DWORD mode;
	struct node *root;
	int dirty;
	WCHAR *name;
};

extern const struct builtin_dll builtin_user32, builtin_ole32;

static WCHAR *acp_to_utf16_dup(const char *s)
{
	char *u = acp_to_utf8(s);
	WCHAR *w = u ? utf8_to_utf16_dup(u) : NULL;
	free(u);
	return w;
}

static char *utf16_to_acp_dup(const WCHAR *s)
{
	char *u = utf16_to_utf8_dup(s);
	char *a = u ? utf8_to_acp_dup(u) : NULL;
	free(u);
	return a;
}

static int guid_eq(const GUID *a, const GUID *b)
{
	return !memcmp(a, b, sizeof(GUID));
}

static int writable(DWORD mode)
{
	return (mode & 3) == STGM_WRITE || (mode & 3) == STGM_READWRITE;
}

static void now(FILETIME *ft)
{
	struct timespec ts;
	clock_gettime(CLOCK_REALTIME, &ts);
	/* 100 ns units since 1601. */
	ULONGLONG t = (ULONGLONG)ts.tv_sec * 10000000ull + ts.tv_nsec / 100 +
		      116444736000000000ull;
	ft->dwLowDateTime = (DWORD)t;
	ft->dwHighDateTime = (DWORD)(t >> 32);
}

/* ---- the tree ---- */

static struct node *node_new(const WCHAR *name, int len, BYTE type)
{
	struct node *n = calloc(1, sizeof(*n));
	if (!n)
		return NULL;
	n->refs = 1;
	n->type = type;
	n->len = len;
	memcpy(n->name, name, len * sizeof(WCHAR));
	return n;
}

static void node_unref(struct node *n)
{
	if (n && --n->refs == 0) {
		free(n->data);
		free(n);
	}
}

/* Takes n and everything below it out of the tree. */
static void node_detach(struct node *n)
{
	struct node *c = n->child;
	while (c) {
		struct node *next = c->next;
		node_detach(c);
		c = next;
	}
	n->child = NULL;
	n->parent = NULL;
	n->next = NULL;
	n->gone = 1;
	node_unref(n);
}

static void node_unlink(struct node *n)
{
	struct node **p = &n->parent->child;
	while (*p && *p != n)
		p = &(*p)->next;
	if (*p)
		*p = n->next;
	node_detach(n);
}

static void node_add(struct node *parent, struct node *n)
{
	n->parent = parent;
	n->next = parent->child;
	parent->child = n;
}

static WCHAR upper(WCHAR c)
{
	if (c >= 'a' && c <= 'z')
		return c - 0x20;
	if (c >= 0xe0 && c <= 0xfe && c != 0xf7)
		return c - 0x20;
	return c;
}

/* The order the format sorts siblings in: by length, then upper case. */
static int name_cmp(const WCHAR *a, int al, const WCHAR *b, int bl)
{
	if (al != bl)
		return al < bl ? -1 : 1;
	for (int i = 0; i < al; i++) {
		WCHAR x = upper(a[i]), y = upper(b[i]);
		if (x != y)
			return x < y ? -1 : 1;
	}
	return 0;
}

static struct node *node_find(struct node *parent, const WCHAR *name)
{
	int len = (int)wstrlen(name);
	for (struct node *c = parent->child; c; c = c->next)
		if (!name_cmp(c->name, c->len, name, len))
			return c;
	return NULL;
}

static HRESULT check_name(const WCHAR *name)
{
	if (!name)
		return STG_E_INVALIDPOINTER;
	size_t len = wstrlen(name);
	if (!len || len > NAME_MAX_CHARS)
		return STG_E_INVALIDNAME;
	for (size_t i = 0; i < len; i++)
		if (name[i] == '/' || name[i] == '\\' || name[i] == ':' ||
		    name[i] == '!')
			return STG_E_INVALIDNAME;
	return S_OK;
}

static HRESULT node_resize(struct node *n, ULONG size)
{
	if (size > n->cap) {
		ULONG cap = n->cap ? n->cap : 256;
		while (cap < size)
			cap = cap > 0x7fffffffu ? size : cap * 2;
		BYTE *d = realloc(n->data, cap);
		if (!d)
			return STG_E_MEDIUMFULL;
		n->data = d;
		n->cap = cap;
	}
	if (size > n->size)
		memset(n->data + n->size, 0, size - n->size);
	n->size = size;
	return S_OK;
}

/* ---- reading a compound file ---- */

struct reader {
	const BYTE *buf;
	size_t len;
	ULONG ssz;		/* sector size */
	ULONG *fat, nfat;
	ULONG *minifat, nminifat;
	BYTE *ministream;
	ULONG ministream_len;
	BYTE *dir;
	ULONG ndir;
	BYTE *seen;
};

static DWORD rd32(const BYTE *p)
{
	return p[0] | p[1] << 8 | p[2] << 16 | (DWORD)p[3] << 24;
}

static WORD rd16(const BYTE *p)
{
	return (WORD)(p[0] | p[1] << 8);
}

static const BYTE *sector(struct reader *r, ULONG s)
{
	size_t at = ((size_t)s + 1) * r->ssz;
	if (s >= 0xfffffffau || at + r->ssz > r->len)
		return NULL;
	return r->buf + at;
}

/*
 * Follows a chain from start through table (FAT or mini FAT), copying
 * want bytes (or the whole chain if want is -1) of units of unit bytes.
 */
static BYTE *read_chain(struct reader *r, ULONG start, const ULONG *table,
			ULONG ntable, ULONG unit, ULONGLONG want, ULONG *got,
			int mini)
{
	ULONGLONG cap = want == (ULONGLONG)-1 ? (ULONGLONG)ntable * unit : want;
	if (cap > 0x7fffffff)
		return NULL;
	BYTE *out = malloc(cap ? cap : 1);
	ULONGLONG n = 0;
	ULONG steps = 0;

	if (!out)
		return NULL;
	for (ULONG s = start; s != ENDOFCHAIN && n < cap; s = table[s]) {
		if (s >= ntable || ++steps > ntable) {
			free(out);
			return NULL;
		}
		const BYTE *src;
		if (mini) {
			size_t at = (size_t)s * unit;
			if (at + unit > r->ministream_len) {
				free(out);
				return NULL;
			}
			src = r->ministream + at;
		} else if (!(src = sector(r, s))) {
			free(out);
			return NULL;
		}
		ULONGLONG k = cap - n < unit ? cap - n : unit;
		memcpy(out + n, src, k);
		n += k;
	}
	if (want != (ULONGLONG)-1 && n < want) {
		free(out);
		return NULL;
	}
	*got = (ULONG)n;
	return out;
}

static HRESULT load_entry(struct reader *r, struct node *parent, ULONG id,
			  int depth);

/* The siblings of a red-black tree, in order (order doesn't matter here). */
static HRESULT load_siblings(struct reader *r, struct node *parent, ULONG id,
			     int depth)
{
	if (id == NOSTREAM)
		return S_OK;
	if (id >= r->ndir || r->seen[id] || depth > 1000)
		return STG_E_DOCFILECORRUPT;
	r->seen[id] = 1;
	const BYTE *e = r->dir + (size_t)id * 128;
	HRESULT hr = load_siblings(r, parent, rd32(e + 0x44), depth + 1);
	if (SUCCEEDED(hr))
		hr = load_entry(r, parent, id, depth);
	if (SUCCEEDED(hr))
		hr = load_siblings(r, parent, rd32(e + 0x48), depth + 1);
	return hr;
}

static HRESULT load_entry(struct reader *r, struct node *parent, ULONG id,
			  int depth)
{
	const BYTE *e = r->dir + (size_t)id * 128;
	BYTE type = e[0x42];
	int len = rd16(e + 0x40) / 2;

	if (type != STGTY_STORAGE && type != STGTY_STREAM)
		return S_OK;	/* unused or unknown: skipped */
	if (len < 1 || len > NAME_MAX_CHARS + 1)
		return STG_E_DOCFILECORRUPT;
	WCHAR name[NAME_MAX_CHARS + 1];
	for (int i = 0; i < len; i++)
		name[i] = rd16(e + 2 * i);
	struct node *n = node_new(name, len - 1, type);
	if (!n)
		return E_OUTOFMEMORY;
	memcpy(&n->clsid, e + 0x50, 16);
	n->state = rd32(e + 0x60);
	n->ctime.dwLowDateTime = rd32(e + 0x64);
	n->ctime.dwHighDateTime = rd32(e + 0x68);
	n->mtime.dwLowDateTime = rd32(e + 0x6c);
	n->mtime.dwHighDateTime = rd32(e + 0x70);
	node_add(parent, n);
	if (type == STGTY_STORAGE)
		return load_siblings(r, n, rd32(e + 0x4c), depth + 1);
	ULONG start = rd32(e + 0x74), size = rd32(e + 0x78), got = 0;
	if (!size)
		return S_OK;
	BYTE *data = size < CUTOFF ?
		read_chain(r, start, r->minifat, r->nminifat, MINI, size, &got, 1) :
		read_chain(r, start, r->fat, r->nfat, r->ssz, size, &got, 0);
	if (!data)
		return STG_E_DOCFILECORRUPT;
	n->data = data;
	n->size = n->cap = size;
	return S_OK;
}

static int has_signature(const BYTE *buf, size_t len)
{
	return len >= SECT && !memcmp(buf, signature, 8);
}

/* Reads a whole file into a tree; *root is the new root. */
static HRESULT parse(const BYTE *buf, size_t len, struct node **root)
{
	struct reader r = { .buf = buf, .len = len };
	HRESULT hr = STG_E_DOCFILECORRUPT;

	if (!has_signature(buf, len))
		return STG_E_FILEALREADYEXISTS;
	WORD shift = rd16(buf + 0x1e);
	if (shift != 9 && shift != 12)
		return STG_E_DOCFILECORRUPT;
	r.ssz = 1u << shift;
	ULONG nfatsect = rd32(buf + 0x2c), dir_start = rd32(buf + 0x30);
	ULONG cutoff = rd32(buf + 0x38), minifat_start = rd32(buf + 0x3c);
	ULONG difat = rd32(buf + 0x44), ndifat = rd32(buf + 0x48);
	ULONG per = r.ssz / 4;
	size_t nsect = len / r.ssz;

	if (cutoff != CUTOFF || nfatsect > nsect || ndifat > nsect)
		return STG_E_DOCFILECORRUPT;
	/* The FAT, through the header's 109 entries and the DIFAT chain. */
	r.nfat = nfatsect * per;
	r.fat = malloc((size_t)r.nfat * 4 + 4);
	if (!r.fat)
		return E_OUTOFMEMORY;
	ULONG k = 0;
	for (ULONG i = 0; i < 109 && k < nfatsect; i++, k++) {
		const BYTE *s = sector(&r, rd32(buf + 0x4c + 4 * i));
		if (!s)
			goto out;
		for (ULONG j = 0; j < per; j++)
			r.fat[k * per + j] = rd32(s + 4 * j);
	}
	for (ULONG d = 0; d < ndifat && k < nfatsect; d++) {
		const BYTE *ds = sector(&r, difat);
		if (!ds)
			goto out;
		for (ULONG i = 0; i < per - 1 && k < nfatsect; i++, k++) {
			const BYTE *s = sector(&r, rd32(ds + 4 * i));
			if (!s)
				goto out;
			for (ULONG j = 0; j < per; j++)
				r.fat[k * per + j] = rd32(s + 4 * j);
		}
		difat = rd32(ds + 4 * (per - 1));
	}
	if (k < nfatsect)
		goto out;
	ULONG got = 0;
	r.dir = read_chain(&r, dir_start, r.fat, r.nfat, r.ssz, (ULONGLONG)-1,
			   &got, 0);
	if (!r.dir || got < 128)
		goto out;
	r.ndir = got / 128;
	if (r.dir[0x42] != STGTY_ROOT)
		goto out;
	if (minifat_start != ENDOFCHAIN) {
		BYTE *mf = read_chain(&r, minifat_start, r.fat, r.nfat, r.ssz,
				      (ULONGLONG)-1, &got, 0);
		if (!mf)
			goto out;
		r.nminifat = got / 4;
		r.minifat = malloc((size_t)r.nminifat * 4 + 4);
		for (ULONG i = 0; r.minifat && i < r.nminifat; i++)
			r.minifat[i] = rd32(mf + 4 * i);
		free(mf);
		if (!r.minifat)
			goto out;
	}
	ULONG ms_start = rd32(r.dir + 0x74), ms_size = rd32(r.dir + 0x78);
	if (ms_size && ms_start != ENDOFCHAIN) {
		r.ministream = read_chain(&r, ms_start, r.fat, r.nfat, r.ssz,
					  ms_size, &r.ministream_len, 0);
		if (!r.ministream)
			goto out;
	}
	r.seen = calloc(r.ndir, 1);
	if (!r.seen)
		goto out;
	r.seen[0] = 1;
	static const WCHAR root_name[] = { 'R','o','o','t',' ','E','n','t','r','y' };
	struct node *n = node_new(root_name, 10, STGTY_STORAGE);
	if (!n) {
		hr = E_OUTOFMEMORY;
		goto out;
	}
	memcpy(&n->clsid, r.dir + 0x50, 16);
	n->state = rd32(r.dir + 0x60);
	n->ctime.dwLowDateTime = rd32(r.dir + 0x64);
	n->ctime.dwHighDateTime = rd32(r.dir + 0x68);
	n->mtime.dwLowDateTime = rd32(r.dir + 0x6c);
	n->mtime.dwHighDateTime = rd32(r.dir + 0x70);
	hr = load_siblings(&r, n, rd32(r.dir + 0x4c), 0);
	if (FAILED(hr))
		node_detach(n);
	else
		*root = n;
out:
	free(r.fat);
	free(r.minifat);
	free(r.ministream);
	free(r.dir);
	free(r.seen);
	return hr;
}

/* ---- writing one ---- */

struct writer {
	struct node **dirs;	/* in directory order */
	ULONG ndir, cap;
	ULONG *left, *right, *child;
	BYTE *color;
};

static int add_dir(struct writer *w, struct node *n)
{
	if (w->ndir == w->cap) {
		ULONG cap = w->cap ? w->cap * 2 : 16;
		struct node **d = realloc(w->dirs, cap * sizeof(*d));
		if (!d)
			return -1;
		w->dirs = d;
		w->cap = cap;
	}
	w->dirs[w->ndir++] = n;
	return 0;
}

static int cmp_nodes(const void *a, const void *b)
{
	const struct node *x = *(struct node *const *)a;
	const struct node *y = *(struct node *const *)b;
	return name_cmp(x->name, x->len, y->name, y->len);
}

static int collect(struct writer *w, struct node *n)
{
	if (add_dir(w, n))
		return -1;
	for (struct node *c = n->child; c; c = c->next)
		if (collect(w, c))
			return -1;
	return 0;
}

static ULONG index_of(struct writer *w, struct node *n)
{
	for (ULONG i = 0; i < w->ndir; i++)
		if (w->dirs[i] == n)
			return i;
	return NOSTREAM;
}

/*
 * A balanced tree of the sorted siblings: all black but for the deepest
 * level of a tree that isn't full, which is red, so it is a red-black one.
 */
static ULONG build_tree(struct writer *w, struct node **sorted, int lo, int hi,
			int depth, int red_depth)
{
	if (lo > hi)
		return NOSTREAM;
	int mid = lo + (hi - lo + 1) / 2;
	ULONG id = index_of(w, sorted[mid]);
	w->left[id] = build_tree(w, sorted, lo, mid - 1, depth + 1, red_depth);
	w->right[id] = build_tree(w, sorted, mid + 1, hi, depth + 1, red_depth);
	w->color[id] = depth == red_depth ? 0 : 1;
	return id;
}

static int link_children(struct writer *w, struct node *n)
{
	int count = 0;
	for (struct node *c = n->child; c; c = c->next)
		count++;
	ULONG id = index_of(w, n);
	w->child[id] = NOSTREAM;
	if (!count)
		return 0;
	struct node **sorted = malloc(count * sizeof(*sorted));
	if (!sorted)
		return -1;
	int i = 0;
	for (struct node *c = n->child; c; c = c->next)
		sorted[i++] = c;
	qsort(sorted, count, sizeof(*sorted), cmp_nodes);
	int depth = 0;
	while ((1 << (depth + 1)) - 1 < count)
		depth++;
	/* A full tree of depth d has 2^(d+1)-1 nodes. */
	int red = (1 << (depth + 1)) - 1 == count ? -1 : depth;
	w->child[id] = build_tree(w, sorted, 0, count - 1, 0, red);
	free(sorted);
	for (struct node *c = n->child; c; c = c->next)
		if (c->type == STGTY_STORAGE && link_children(w, c))
			return -1;
	return 0;
}

static void wr32(BYTE *p, DWORD v)
{
	p[0] = (BYTE)v;
	p[1] = (BYTE)(v >> 8);
	p[2] = (BYTE)(v >> 16);
	p[3] = (BYTE)(v >> 24);
}

static void wr16(BYTE *p, WORD v)
{
	p[0] = (BYTE)v;
	p[1] = (BYTE)(v >> 8);
}

static ULONG sectors_for(ULONGLONG bytes, ULONG unit)
{
	return (ULONG)((bytes + unit - 1) / unit);
}

/* Lays a chain of count sectors from first out in the FAT. */
static void chain(ULONG *fat, ULONG first, ULONG count)
{
	for (ULONG i = 0; i < count; i++)
		fat[first + i] = i + 1 < count ? first + i + 1 : ENDOFCHAIN;
}

/* Serialises the tree under root; *out is malloc()ed. */
static HRESULT serialise(struct node *root, BYTE **out, size_t *out_len)
{
	struct writer w = { 0 };
	HRESULT hr = E_OUTOFMEMORY;
	BYTE *buf = NULL;
	ULONG *fat = NULL;
	ULONG *start = NULL;

	if (collect(&w, root))
		goto out;
	w.left = malloc(w.ndir * sizeof(ULONG));
	w.right = malloc(w.ndir * sizeof(ULONG));
	w.child = malloc(w.ndir * sizeof(ULONG));
	w.color = malloc(w.ndir);
	start = malloc(w.ndir * sizeof(ULONG));
	if (!w.left || !w.right || !w.child || !w.color || !start)
		goto out;
	w.left[0] = w.right[0] = NOSTREAM;
	w.color[0] = 1;
	if (link_children(&w, root))
		goto out;

	/* Sizes of each region, in sectors. */
	ULONG big = 0, mini = 0;
	for (ULONG i = 1; i < w.ndir; i++) {
		struct node *n = w.dirs[i];
		if (n->type != STGTY_STREAM || !n->size)
			continue;
		if (n->size >= CUTOFF)
			big += sectors_for(n->size, SECT);
		else
			mini += sectors_for(n->size, MINI);
	}
	ULONG ms_sect = sectors_for((ULONGLONG)mini * MINI, SECT);
	ULONG mf_sect = sectors_for((ULONGLONG)mini * 4, SECT);
	ULONG dir_sect = sectors_for((ULONGLONG)w.ndir * 128, SECT);
	ULONG data = big + ms_sect + mf_sect + dir_sect, nfat = 0, ndifat = 0;
	for (;;) {
		ULONG f = sectors_for((ULONGLONG)(data + nfat + ndifat) * 4, SECT);
		ULONG d = f > 109 ? sectors_for(f - 109, SECT / 4 - 1) : 0;
		if (f == nfat && d == ndifat)
			break;
		nfat = f;
		ndifat = d;
	}
	ULONG total = data + nfat + ndifat;
	size_t len = (size_t)(total + 1) * SECT;
	buf = calloc(1, len);
	fat = malloc((size_t)nfat * (SECT / 4) * sizeof(ULONG));
	if (!buf || !fat)
		goto out;
	for (ULONG i = 0; i < nfat * (SECT / 4); i++)
		fat[i] = FREESECT;

	/* Big streams, then the mini stream, mini FAT, directory, FAT, DIFAT. */
	ULONG at = 0, mat = 0;
	BYTE *ms = buf + SECT + (size_t)big * SECT;
	BYTE *mf = ms + (size_t)ms_sect * SECT;
	ULONG ms_first = big, mf_first = big + ms_sect;
	ULONG dir_first = mf_first + mf_sect, fat_first = dir_first + dir_sect;
	ULONG difat_first = fat_first + nfat;
	for (ULONG i = 0; i < mini; i++)
		wr32(mf + 4 * i, i + 1);	/* patched below */
	for (ULONG i = 1; i < w.ndir; i++) {
		struct node *n = w.dirs[i];
		start[i] = n->type == STGTY_STREAM ? ENDOFCHAIN : 0;
		if (n->type != STGTY_STREAM || !n->size)
			continue;
		if (n->size >= CUTOFF) {
			ULONG c = sectors_for(n->size, SECT);
			memcpy(buf + SECT + (size_t)at * SECT, n->data, n->size);
			chain(fat, at, c);
			start[i] = at;
			at += c;
		} else {
			ULONG c = sectors_for(n->size, MINI);
			memcpy(ms + (size_t)mat * MINI, n->data, n->size);
			for (ULONG j = 0; j < c; j++)
				wr32(mf + 4 * (mat + j),
				     j + 1 < c ? mat + j + 1 : ENDOFCHAIN);
			start[i] = mat;
			mat += c;
		}
	}
	for (ULONG i = mini; i < mf_sect * (SECT / 4); i++)
		wr32(mf + 4 * i, FREESECT);
	chain(fat, ms_first, ms_sect);
	chain(fat, mf_first, mf_sect);
	chain(fat, dir_first, dir_sect);
	for (ULONG i = 0; i < nfat; i++)
		fat[fat_first + i] = FATSECT;
	for (ULONG i = 0; i < ndifat; i++)
		fat[difat_first + i] = DIFSECT;

	/* The directory. */
	BYTE *dir = buf + SECT + (size_t)dir_first * SECT;
	for (ULONG i = 0; i < dir_sect * (SECT / 128); i++) {
		BYTE *e = dir + (size_t)i * 128;
		if (i >= w.ndir) {
			wr32(e + 0x44, NOSTREAM);
			wr32(e + 0x48, NOSTREAM);
			wr32(e + 0x4c, NOSTREAM);
			continue;
		}
		struct node *n = w.dirs[i];
		for (int j = 0; j < n->len; j++)
			wr16(e + 2 * j, n->name[j]);
		wr16(e + 0x40, (WORD)((n->len + 1) * 2));
		e[0x42] = i ? n->type : STGTY_ROOT;
		e[0x43] = w.color[i];
		wr32(e + 0x44, w.left[i]);
		wr32(e + 0x48, w.right[i]);
		wr32(e + 0x4c, n->type == STGTY_STREAM ? NOSTREAM : w.child[i]);
		if (n->type != STGTY_STREAM) {
			memcpy(e + 0x50, &n->clsid, 16);
			wr32(e + 0x60, n->state);
			wr32(e + 0x64, n->ctime.dwLowDateTime);
			wr32(e + 0x68, n->ctime.dwHighDateTime);
			wr32(e + 0x6c, n->mtime.dwLowDateTime);
			wr32(e + 0x70, n->mtime.dwHighDateTime);
		} else {
			wr32(e + 0x60, n->state);
		}
		if (!i) {
			wr32(e + 0x74, ms_sect ? ms_first : ENDOFCHAIN);
			wr32(e + 0x78, mini * MINI);
		} else if (n->type == STGTY_STREAM) {
			wr32(e + 0x74, start[i]);
			wr32(e + 0x78, n->size);
		}
	}

	/* The FAT and DIFAT sectors. */
	for (ULONG i = 0; i < nfat * (SECT / 4); i++)
		wr32(buf + SECT + (size_t)fat_first * SECT + 4 * i, fat[i]);
	for (ULONG d = 0; d < ndifat; d++) {
		BYTE *s = buf + SECT + (size_t)(difat_first + d) * SECT;
		for (ULONG j = 0; j < SECT / 4 - 1; j++) {
			ULONG k = 109 + d * (SECT / 4 - 1) + j;
			wr32(s + 4 * j, k < nfat ? fat_first + k : FREESECT);
		}
		wr32(s + SECT - 4, d + 1 < ndifat ? difat_first + d + 1 :
						   ENDOFCHAIN);
	}

	/* The header. */
	memcpy(buf, signature, 8);
	wr16(buf + 0x18, 0x3e);
	wr16(buf + 0x1a, 3);
	wr16(buf + 0x1c, 0xfffe);
	wr16(buf + 0x1e, 9);
	wr16(buf + 0x20, 6);
	wr32(buf + 0x2c, nfat);
	wr32(buf + 0x30, dir_first);
	wr32(buf + 0x38, CUTOFF);
	wr32(buf + 0x3c, mf_sect ? mf_first : ENDOFCHAIN);
	wr32(buf + 0x40, mf_sect);
	wr32(buf + 0x44, ndifat ? difat_first : ENDOFCHAIN);
	wr32(buf + 0x48, ndifat);
	for (ULONG i = 0; i < 109; i++)
		wr32(buf + 0x4c + 4 * i, i < nfat ? fat_first + i : FREESECT);

	*out = buf;
	*out_len = len;
	buf = NULL;
	hr = S_OK;
out:
	free(buf);
	free(fat);
	free(start);
	free(w.dirs);
	free(w.left);
	free(w.right);
	free(w.child);
	free(w.color);
	return hr;
}

/* ---- files ---- */

static HRESULT lb_size(ILockBytes *lb, ULONGLONG *size)
{
	STATSTG st;
	HRESULT hr = lb->lpVtbl->Stat(lb, &st, STATFLAG_NONAME);
	if (SUCCEEDED(hr))
		*size = st.cbSize.QuadPart;
	return hr;
}

static HRESULT lb_read_all(ILockBytes *lb, BYTE **out, size_t *len)
{
	ULONGLONG size;
	HRESULT hr = lb_size(lb, &size);
	if (FAILED(hr))
		return hr;
	if (size > 0x7fffffff)
		return STG_E_INSUFFICIENTMEMORY;
	BYTE *buf = malloc(size ? size : 1);
	ULONG got = 0;
	ULARGE_INTEGER zero = { 0 };
	if (!buf)
		return E_OUTOFMEMORY;
	hr = size ? lb->lpVtbl->ReadAt(lb, zero, buf, (ULONG)size, &got) : S_OK;
	if (FAILED(hr) || got != size) {
		free(buf);
		return FAILED(hr) ? hr : STG_E_READFAULT;
	}
	*out = buf;
	*len = (size_t)size;
	return S_OK;
}

/* Reads the file's tree (a new, empty one if it has no bytes). */
static HRESULT file_load(struct file *f, int empty_ok)
{
	BYTE *buf = NULL;
	size_t len = 0;
	HRESULT hr = lb_read_all(f->lb, &buf, &len);
	struct node *root = NULL;

	if (FAILED(hr))
		return hr;
	if (!len && empty_ok) {
		static const WCHAR rn[] = { 'R','o','o','t',' ','E','n','t','r','y' };
		root = node_new(rn, 10, STGTY_STORAGE);
		hr = root ? S_OK : E_OUTOFMEMORY;
	} else {
		hr = parse(buf, len, &root);
	}
	free(buf);
	if (SUCCEEDED(hr)) {
		if (f->root)
			node_detach(f->root);
		f->root = root;
	}
	return hr;
}

static HRESULT file_flush(struct file *f)
{
	if (!f->dirty || !writable(f->mode))
		return S_OK;
	BYTE *buf;
	size_t len;
	HRESULT hr = serialise(f->root, &buf, &len);
	if (FAILED(hr))
		return hr;
	ULARGE_INTEGER zero = { 0 }, size;
	ULONG written = 0;
	size.QuadPart = len;
	hr = f->lb->lpVtbl->SetSize(f->lb, size);
	if (SUCCEEDED(hr))
		hr = f->lb->lpVtbl->WriteAt(f->lb, zero, buf, (ULONG)len, &written);
	if (SUCCEEDED(hr) && written != len)
		hr = STG_E_WRITEFAULT;
	if (SUCCEEDED(hr))
		f->lb->lpVtbl->Flush(f->lb);
	free(buf);
	if (SUCCEEDED(hr))
		f->dirty = 0;
	return hr;
}

static void file_unref(struct file *f)
{
	if (--f->refs)
		return;
	if (!(f->mode & STGM_TRANSACTED))
		file_flush(f);
	node_detach(f->root);
	f->lb->lpVtbl->Release(f->lb);
	free(f->name);
	free(f);
}

static void touch(struct file *f, struct node *n)
{
	f->dirty = 1;
	for (; n; n = n->parent)
		if (n->type == STGTY_STORAGE)
			now(&n->mtime);
}

/* ---- streams ---- */

struct stm {
	IStream iface;
	LONG refs;
	struct file *f;
	struct node *n;
	DWORD mode;
	ULONG pos;
};

static const IStreamVtbl stm_vtbl;

static struct stm *stm_impl(IStream *s)
{
	return (struct stm *)s;
}

static HRESULT stm_new(struct file *f, struct node *n, DWORD mode,
		       IStream **out)
{
	struct stm *s = calloc(1, sizeof(*s));
	if (!s)
		return E_OUTOFMEMORY;
	s->iface.lpVtbl = &stm_vtbl;
	s->refs = 1;
	s->f = f;
	s->n = n;
	s->mode = mode;
	f->refs++;
	n->refs++;
	n->open++;
	*out = &s->iface;
	return S_OK;
}

WINFN(HRESULT) stm_QueryInterface(IStream *self, REFIID riid, void **ppv)
{
	if (!ppv)
		return E_POINTER;
	if (guid_eq(riid, &IID_IUnknown) || guid_eq(riid, &IID_IStream) ||
	    guid_eq(riid, &IID_ISequentialStream)) {
		*ppv = self;
		self->lpVtbl->AddRef(self);
		return S_OK;
	}
	*ppv = NULL;
	return E_NOINTERFACE;
}

WINFN(ULONG) stm_AddRef(IStream *self)
{
	return (ULONG)__atomic_add_fetch(&stm_impl(self)->refs, 1,
					 __ATOMIC_SEQ_CST);
}

WINFN(ULONG) stm_Release(IStream *self)
{
	struct stm *s = stm_impl(self);
	LONG r = __atomic_sub_fetch(&s->refs, 1, __ATOMIC_SEQ_CST);
	if (!r) {
		pthread_mutex_lock(&lock);
		s->n->open--;
		node_unref(s->n);
		file_unref(s->f);
		pthread_mutex_unlock(&lock);
		free(s);
	}
	return (ULONG)r;
}

WINFN(HRESULT) stm_Read(IStream *self, void *pv, ULONG cb, ULONG *read)
{
	struct stm *s = stm_impl(self);
	ULONG n = 0;
	HRESULT hr = S_OK;

	if (!pv)
		return STG_E_INVALIDPOINTER;
	pthread_mutex_lock(&lock);
	if (s->n->gone)
		hr = STG_E_REVERTED;
	else if (s->pos < s->n->size) {
		n = s->n->size - s->pos;
		if (n > cb)
			n = cb;
		memcpy(pv, s->n->data + s->pos, n);
		s->pos += n;
	}
	pthread_mutex_unlock(&lock);
	if (read)
		*read = n;
	return hr;
}

WINFN(HRESULT) stm_Write(IStream *self, const void *pv, ULONG cb,
			 ULONG *written)
{
	struct stm *s = stm_impl(self);
	HRESULT hr = S_OK;

	if (written)
		*written = 0;
	if (!pv && cb)
		return STG_E_INVALIDPOINTER;
	pthread_mutex_lock(&lock);
	if (s->n->gone)
		hr = STG_E_REVERTED;
	else if (!writable(s->mode))
		hr = STG_E_ACCESSDENIED;
	else if (cb && s->pos + cb < s->pos)
		hr = STG_E_MEDIUMFULL;
	else if (cb) {
		if (s->pos + cb > s->n->size)
			hr = node_resize(s->n, s->pos + cb);
		if (SUCCEEDED(hr)) {
			memcpy(s->n->data + s->pos, pv, cb);
			s->pos += cb;
			touch(s->f, s->n->parent);
			if (written)
				*written = cb;
		}
	}
	pthread_mutex_unlock(&lock);
	return hr;
}

WINFN(HRESULT) stm_Seek(IStream *self, LARGE_INTEGER move, DWORD origin,
			ULARGE_INTEGER *pos)
{
	struct stm *s = stm_impl(self);
	LONGLONG base;

	if (s->n->gone)
		return STG_E_REVERTED;
	switch (origin) {
	case STREAM_SEEK_SET: base = 0; break;
	case STREAM_SEEK_CUR: base = s->pos; break;
	case STREAM_SEEK_END: base = s->n->size; break;
	default: return STG_E_INVALIDFUNCTION;
	}
	LONGLONG to = base + move.QuadPart;
	if (to < 0 || to > 0xffffffffLL)
		return STG_E_INVALIDFUNCTION;
	s->pos = (ULONG)to;
	if (pos)
		pos->QuadPart = s->pos;
	return S_OK;
}

WINFN(HRESULT) stm_SetSize(IStream *self, ULARGE_INTEGER size)
{
	struct stm *s = stm_impl(self);
	HRESULT hr;

	if (size.u.HighPart)
		return STG_E_MEDIUMFULL;
	pthread_mutex_lock(&lock);
	if (s->n->gone)
		hr = STG_E_REVERTED;
	else if (!writable(s->mode))
		hr = STG_E_ACCESSDENIED;
	else if ((hr = node_resize(s->n, size.u.LowPart)) == S_OK)
		touch(s->f, s->n->parent);
	pthread_mutex_unlock(&lock);
	return hr;
}

WINFN(HRESULT) stm_CopyTo(IStream *self, IStream *to, ULARGE_INTEGER cb,
			  ULARGE_INTEGER *read, ULARGE_INTEGER *written)
{
	ULONGLONG done_r = 0, done_w = 0;
	BYTE buf[4096];
	HRESULT hr = S_OK;

	if (!to)
		return STG_E_INVALIDPOINTER;
	while (done_r < cb.QuadPart) {
		ULONG want = cb.QuadPart - done_r < sizeof(buf) ?
			(ULONG)(cb.QuadPart - done_r) : sizeof(buf);
		ULONG got = 0, put = 0;
		hr = stm_Read(self, buf, want, &got);
		if (FAILED(hr) || !got)
			break;
		done_r += got;
		hr = to->lpVtbl->Write(to, buf, got, &put);
		done_w += put;
		if (FAILED(hr))
			break;
	}
	if (read)
		read->QuadPart = done_r;
	if (written)
		written->QuadPart = done_w;
	return hr;
}

WINFN(HRESULT) stm_Commit(IStream *self, DWORD flags)
{
	struct stm *s = stm_impl(self);
	HRESULT hr = S_OK;

	pthread_mutex_lock(&lock);
	if (s->n->gone)
		hr = STG_E_REVERTED;
	else if (!(s->f->mode & STGM_TRANSACTED))
		hr = file_flush(s->f);
	pthread_mutex_unlock(&lock);
	return hr;
}

WINFN(HRESULT) stm_Revert(IStream *self)
{
	return S_OK;
}

WINFN(HRESULT) stm_LockRegion(IStream *self, ULARGE_INTEGER offset,
			      ULARGE_INTEGER cb, DWORD type)
{
	return STG_E_INVALIDFUNCTION;
}

WINFN(HRESULT) stm_UnlockRegion(IStream *self, ULARGE_INTEGER offset,
				ULARGE_INTEGER cb, DWORD type)
{
	return STG_E_INVALIDFUNCTION;
}

static HRESULT fill_stat(struct node *n, STATSTG *st, DWORD flags,
			 DWORD mode, const WCHAR *root_name)
{
	memset(st, 0, sizeof(*st));
	if (!(flags & STATFLAG_NONAME)) {
		const WCHAR *name = root_name ? root_name : n->name;
		size_t len = root_name ? wstrlen(root_name) : (size_t)n->len;
		st->pwcsName = ole_taskmem_alloc((len + 1) * sizeof(WCHAR));
		if (!st->pwcsName)
			return STG_E_INSUFFICIENTMEMORY;
		memcpy(st->pwcsName, name, len * sizeof(WCHAR));
		st->pwcsName[len] = 0;
	}
	st->type = n->type;
	st->cbSize.QuadPart = n->type == STGTY_STREAM ? n->size : 0;
	st->mtime = n->mtime;
	st->ctime = n->ctime;
	st->grfMode = mode;
	st->clsid = n->clsid;
	st->grfStateBits = n->state;
	return S_OK;
}

WINFN(HRESULT) stm_Stat(IStream *self, STATSTG *st, DWORD flags)
{
	struct stm *s = stm_impl(self);
	HRESULT hr;

	if (!st)
		return STG_E_INVALIDPOINTER;
	pthread_mutex_lock(&lock);
	hr = s->n->gone ? STG_E_REVERTED :
		fill_stat(s->n, st, flags, s->mode, NULL);
	pthread_mutex_unlock(&lock);
	return hr;
}

WINFN(HRESULT) stm_Clone(IStream *self, IStream **clone)
{
	struct stm *s = stm_impl(self);
	HRESULT hr;

	if (!clone)
		return STG_E_INVALIDPOINTER;
	*clone = NULL;
	pthread_mutex_lock(&lock);
	hr = s->n->gone ? STG_E_REVERTED : stm_new(s->f, s->n, s->mode, clone);
	if (SUCCEEDED(hr))
		stm_impl(*clone)->pos = s->pos;
	pthread_mutex_unlock(&lock);
	return hr;
}

static const IStreamVtbl stm_vtbl = {
	stm_QueryInterface, stm_AddRef, stm_Release, stm_Read, stm_Write,
	stm_Seek, stm_SetSize, stm_CopyTo, stm_Commit, stm_Revert,
	stm_LockRegion, stm_UnlockRegion, stm_Stat, stm_Clone,
};

/* ---- storages ---- */

struct stg {
	IStorage iface;
	LONG refs;
	struct file *f;
	struct node *n;
	DWORD mode;
};

static const IStorageVtbl stg_vtbl;

static struct stg *stg_impl(IStorage *s)
{
	return (struct stg *)s;
}

static HRESULT stg_new(struct file *f, struct node *n, DWORD mode,
		       IStorage **out)
{
	struct stg *s = calloc(1, sizeof(*s));
	if (!s)
		return E_OUTOFMEMORY;
	s->iface.lpVtbl = &stg_vtbl;
	s->refs = 1;
	s->f = f;
	s->n = n;
	s->mode = mode;
	f->refs++;
	n->refs++;
	n->open++;
	*out = &s->iface;
	return S_OK;
}

WINFN(HRESULT) stg_QueryInterface(IStorage *self, REFIID riid, void **ppv)
{
	if (!ppv)
		return E_POINTER;
	if (guid_eq(riid, &IID_IUnknown) || guid_eq(riid, &IID_IStorage)) {
		*ppv = self;
		self->lpVtbl->AddRef(self);
		return S_OK;
	}
	*ppv = NULL;
	return E_NOINTERFACE;
}

WINFN(ULONG) stg_AddRef(IStorage *self)
{
	return (ULONG)__atomic_add_fetch(&stg_impl(self)->refs, 1,
					 __ATOMIC_SEQ_CST);
}

WINFN(ULONG) stg_Release(IStorage *self)
{
	struct stg *s = stg_impl(self);
	LONG r = __atomic_sub_fetch(&s->refs, 1, __ATOMIC_SEQ_CST);
	if (!r) {
		pthread_mutex_lock(&lock);
		s->n->open--;
		node_unref(s->n);
		file_unref(s->f);
		pthread_mutex_unlock(&lock);
		free(s);
	}
	return (ULONG)r;
}

/* The checks every element operation makes of this storage. */
static HRESULT usable(struct stg *s, int write)
{
	if (s->n->gone)
		return STG_E_REVERTED;
	if (write && !writable(s->mode))
		return STG_E_ACCESSDENIED;
	return S_OK;
}

/*
 * Makes an element: a new one, or with STGM_CREATE in place of one of
 * the same name that isn't open.
 */
static HRESULT make_element(struct stg *s, LPCOLESTR name, DWORD mode,
			    BYTE type, struct node **out)
{
	HRESULT hr = check_name(name);
	if (FAILED(hr))
		return hr;
	if (FAILED(hr = usable(s, 1)))
		return hr;
	struct node *old = node_find(s->n, name);
	if (old) {
		if (!(mode & STGM_CREATE))
			return STG_E_FILEALREADYEXISTS;
		if (old->open)
			return STG_E_ACCESSDENIED;
		node_unlink(old);
	}
	struct node *n = node_new(name, (int)wstrlen(name), type);
	if (!n)
		return STG_E_INSUFFICIENTMEMORY;
	if (type == STGTY_STORAGE) {
		now(&n->ctime);
		n->mtime = n->ctime;
	}
	node_add(s->n, n);
	touch(s->f, s->n);
	*out = n;
	return S_OK;
}

/* Elements are opened exclusively, and no more writably than here. */
static HRESULT check_open_mode(struct stg *s, DWORD mode)
{
	if ((mode & 0x70) != STGM_SHARE_EXCLUSIVE)
		return STG_E_INVALIDFLAG;
	if ((mode & 3) == 3)
		return STG_E_INVALIDFLAG;
	if (writable(mode) && !writable(s->mode))
		return STG_E_ACCESSDENIED;
	return S_OK;
}

WINFN(HRESULT) stg_CreateStream(IStorage *self, LPCOLESTR name, DWORD mode,
				DWORD r1, DWORD r2, IStream **out)
{
	struct stg *s = stg_impl(self);
	struct node *n;
	HRESULT hr;

	if (!out)
		return STG_E_INVALIDPOINTER;
	*out = NULL;
	if (FAILED(hr = check_open_mode(s, mode)))
		return hr;
	pthread_mutex_lock(&lock);
	hr = make_element(s, name, mode, STGTY_STREAM, &n);
	if (SUCCEEDED(hr))
		hr = stm_new(s->f, n, mode, out);
	pthread_mutex_unlock(&lock);
	return hr;
}

static HRESULT open_element(struct stg *s, LPCOLESTR name, DWORD mode,
			    BYTE type, struct node **out)
{
	HRESULT hr = check_open_mode(s, mode);
	if (FAILED(hr))
		return hr;
	if (!name)
		return STG_E_INVALIDPOINTER;
	if (FAILED(hr = usable(s, 0)))
		return hr;
	struct node *n = node_find(s->n, name);
	if (!n || n->type != type)
		return STG_E_FILENOTFOUND;
	if (n->open)
		return STG_E_ACCESSDENIED;
	*out = n;
	return S_OK;
}

WINFN(HRESULT) stg_OpenStream(IStorage *self, LPCOLESTR name, void *r1,
			      DWORD mode, DWORD r2, IStream **out)
{
	struct stg *s = stg_impl(self);
	struct node *n;
	HRESULT hr;

	if (!out)
		return STG_E_INVALIDPOINTER;
	*out = NULL;
	pthread_mutex_lock(&lock);
	hr = open_element(s, name, mode, STGTY_STREAM, &n);
	if (SUCCEEDED(hr))
		hr = stm_new(s->f, n, mode, out);
	pthread_mutex_unlock(&lock);
	return hr;
}

WINFN(HRESULT) stg_CreateStorage(IStorage *self, LPCOLESTR name, DWORD mode,
				 DWORD r1, DWORD r2, IStorage **out)
{
	struct stg *s = stg_impl(self);
	struct node *n;
	HRESULT hr;

	if (!out)
		return STG_E_INVALIDPOINTER;
	*out = NULL;
	if (FAILED(hr = check_open_mode(s, mode)))
		return hr;
	pthread_mutex_lock(&lock);
	hr = make_element(s, name, mode, STGTY_STORAGE, &n);
	if (SUCCEEDED(hr))
		hr = stg_new(s->f, n, mode, out);
	pthread_mutex_unlock(&lock);
	return hr;
}

WINFN(HRESULT) stg_OpenStorage(IStorage *self, LPCOLESTR name,
			       IStorage *priority, DWORD mode, SNB exclude,
			       DWORD reserved, IStorage **out)
{
	struct stg *s = stg_impl(self);
	struct node *n;
	HRESULT hr;

	if (!out)
		return STG_E_INVALIDPOINTER;
	*out = NULL;
	if (priority)
		return STG_E_INVALIDPARAMETER;
	pthread_mutex_lock(&lock);
	hr = open_element(s, name, mode, STGTY_STORAGE, &n);
	if (SUCCEEDED(hr))
		hr = stg_new(s->f, n, mode, out);
	pthread_mutex_unlock(&lock);
	return hr;
}

static int excluded(SNB snb, const WCHAR *name, int len)
{
	for (; snb && *snb; snb++)
		if (!name_cmp(*snb, (int)wstrlen(*snb), name, len))
			return 1;
	return 0;
}

static HRESULT copy_node_to(struct node *n, IStorage *dest, const WCHAR *as);

/* Copies the elements below src (and its class) into dest. */
static HRESULT copy_children(struct node *src, IStorage *dest, int streams,
			     int storages, SNB exclude)
{
	HRESULT hr = dest->lpVtbl->SetClass(dest, &src->clsid);
	if (FAILED(hr))
		return hr;
	dest->lpVtbl->SetStateBits(dest, src->state, 0xffffffff);
	for (struct node *c = src->child; c; c = c->next) {
		if (excluded(exclude, c->name, c->len))
			continue;
		if ((c->type == STGTY_STREAM && !streams) ||
		    (c->type == STGTY_STORAGE && !storages))
			continue;
		WCHAR name[NAME_MAX_CHARS + 1];
		memcpy(name, c->name, c->len * sizeof(WCHAR));
		name[c->len] = 0;
		if (FAILED(hr = copy_node_to(c, dest, name)))
			return hr;
	}
	return S_OK;
}

static HRESULT copy_node_to(struct node *n, IStorage *dest, const WCHAR *as)
{
	const DWORD mode = STGM_CREATE | STGM_READWRITE | STGM_SHARE_EXCLUSIVE;
	HRESULT hr;

	if (n->type == STGTY_STREAM) {
		IStream *out;
		hr = dest->lpVtbl->CreateStream(dest, as, mode, 0, 0, &out);
		if (FAILED(hr))
			return hr;
		ULONG put = 0;
		ULARGE_INTEGER size;
		size.QuadPart = n->size;
		hr = out->lpVtbl->SetSize(out, size);
		if (SUCCEEDED(hr) && n->size)
			hr = out->lpVtbl->Write(out, n->data, n->size, &put);
		out->lpVtbl->Release(out);
		return hr;
	}
	IStorage *sub;
	hr = dest->lpVtbl->CreateStorage(dest, as, mode, 0, 0, &sub);
	if (FAILED(hr))
		return hr;
	hr = copy_children(n, sub, 1, 1, NULL);
	sub->lpVtbl->Release(sub);
	return hr;
}

WINFN(HRESULT) stg_CopyTo(IStorage *self, DWORD niid, const IID *iids,
			  SNB exclude, IStorage *dest)
{
	struct stg *s = stg_impl(self);
	int streams = 1, storages = 1;
	HRESULT hr;

	if (!dest)
		return STG_E_INVALIDPOINTER;
	if (dest == self)
		return STG_E_ACCESSDENIED;
	for (DWORD i = 0; iids && i < niid; i++) {
		if (guid_eq(&iids[i], &IID_IStream))
			streams = 0;
		if (guid_eq(&iids[i], &IID_IStorage))
			storages = 0;
	}
	pthread_mutex_lock(&lock);
	hr = usable(s, 0);
	if (SUCCEEDED(hr))
		hr = copy_children(s->n, dest, streams, storages, exclude);
	pthread_mutex_unlock(&lock);
	return hr;
}

WINFN(HRESULT) stg_MoveElementTo(IStorage *self, LPCOLESTR name,
				 IStorage *dest, LPCOLESTR new_name,
				 DWORD flags)
{
	struct stg *s = stg_impl(self);
	HRESULT hr;

	if (!name || !dest || !new_name)
		return STG_E_INVALIDPOINTER;
	if (flags != STGMOVE_MOVE && flags != STGMOVE_COPY)
		return STG_E_INVALIDFLAG;
	pthread_mutex_lock(&lock);
	hr = usable(s, flags == STGMOVE_MOVE);
	struct node *n = SUCCEEDED(hr) ? node_find(s->n, name) : NULL;
	if (SUCCEEDED(hr) && !n)
		hr = STG_E_FILENOTFOUND;
	else if (SUCCEEDED(hr) && n->open)
		hr = STG_E_ACCESSDENIED;
	if (SUCCEEDED(hr)) {
		n->refs++;
		hr = copy_node_to(n, dest, new_name);
		if (SUCCEEDED(hr) && flags == STGMOVE_MOVE && !n->gone) {
			node_unlink(n);
			touch(s->f, s->n);
		}
		node_unref(n);
	}
	pthread_mutex_unlock(&lock);
	return hr;
}

WINFN(HRESULT) stg_Commit(IStorage *self, DWORD flags)
{
	struct stg *s = stg_impl(self);
	HRESULT hr;

	pthread_mutex_lock(&lock);
	hr = usable(s, 0);
	if (SUCCEEDED(hr) && !s->n->parent)
		hr = file_flush(s->f);
	else if (SUCCEEDED(hr) && !(s->f->mode & STGM_TRANSACTED))
		hr = file_flush(s->f);
	pthread_mutex_unlock(&lock);
	return hr;
}

WINFN(HRESULT) stg_Revert(IStorage *self)
{
	struct stg *s = stg_impl(self);
	HRESULT hr = S_OK;

	pthread_mutex_lock(&lock);
	if (s->n->gone)
		hr = STG_E_REVERTED;
	else if (!s->n->parent && (s->f->mode & STGM_TRANSACTED) &&
		 s->f->dirty) {
		/* The objects open on the old tree are reverted with it. */
		struct node *old = s->f->root;
		s->f->root = NULL;
		hr = file_load(s->f, 1);
		if (SUCCEEDED(hr)) {
			s->f->dirty = 0;
			old->open--;
			node_detach(old);	/* the tree's reference */
			s->n = s->f->root;
			s->n->refs++;
			s->n->open++;
			node_unref(old);	/* this object's */
		} else {
			s->f->root = old;
		}
	}
	pthread_mutex_unlock(&lock);
	return hr;
}

/* Enumerators hold a snapshot of the names and their STATSTGs. */
struct enm {
	IEnumSTATSTG iface;
	LONG refs;
	STATSTG *items;
	ULONG count, pos;
};

static const IEnumSTATSTGVtbl enm_vtbl;

static struct enm *enm_impl(IEnumSTATSTG *e)
{
	return (struct enm *)e;
}

static struct enm *enm_new(ULONG count)
{
	struct enm *e = calloc(1, sizeof(*e));
	if (e && count && !(e->items = calloc(count, sizeof(STATSTG)))) {
		free(e);
		return NULL;
	}
	if (e) {
		e->iface.lpVtbl = &enm_vtbl;
		e->refs = 1;
		e->count = count;
	}
	return e;
}

WINFN(HRESULT) enm_QueryInterface(IEnumSTATSTG *self, REFIID riid, void **ppv)
{
	if (!ppv)
		return E_POINTER;
	if (guid_eq(riid, &IID_IUnknown) || guid_eq(riid, &IID_IEnumSTATSTG)) {
		*ppv = self;
		self->lpVtbl->AddRef(self);
		return S_OK;
	}
	*ppv = NULL;
	return E_NOINTERFACE;
}

WINFN(ULONG) enm_AddRef(IEnumSTATSTG *self)
{
	return (ULONG)__atomic_add_fetch(&enm_impl(self)->refs, 1,
					 __ATOMIC_SEQ_CST);
}

WINFN(ULONG) enm_Release(IEnumSTATSTG *self)
{
	struct enm *e = enm_impl(self);
	LONG r = __atomic_sub_fetch(&e->refs, 1, __ATOMIC_SEQ_CST);
	if (!r) {
		for (ULONG i = 0; i < e->count; i++)
			free(e->items[i].pwcsName);
		free(e->items);
		free(e);
	}
	return (ULONG)r;
}

WINFN(HRESULT) enm_Next(IEnumSTATSTG *self, ULONG count, STATSTG *out,
			ULONG *fetched)
{
	struct enm *e = enm_impl(self);
	ULONG n = 0;

	if (!out || (count > 1 && !fetched))
		return STG_E_INVALIDPOINTER;
	while (n < count && e->pos < e->count) {
		STATSTG *src = &e->items[e->pos];
		size_t len = wstrlen(src->pwcsName);
		out[n] = *src;
		out[n].pwcsName = ole_taskmem_alloc((len + 1) * sizeof(WCHAR));
		if (!out[n].pwcsName) {
			for (ULONG i = 0; i < n; i++)
				ole_taskmem_free(out[i].pwcsName);
			if (fetched)
				*fetched = 0;
			return STG_E_INSUFFICIENTMEMORY;
		}
		memcpy(out[n].pwcsName, src->pwcsName, (len + 1) * sizeof(WCHAR));
		n++;
		e->pos++;
	}
	if (fetched)
		*fetched = n;
	return n == count ? S_OK : S_FALSE;
}

WINFN(HRESULT) enm_Skip(IEnumSTATSTG *self, ULONG count)
{
	struct enm *e = enm_impl(self);
	ULONG left = e->count - e->pos;
	e->pos += count < left ? count : left;
	return count <= left ? S_OK : S_FALSE;
}

WINFN(HRESULT) enm_Reset(IEnumSTATSTG *self)
{
	enm_impl(self)->pos = 0;
	return S_OK;
}

WINFN(HRESULT) enm_Clone(IEnumSTATSTG *self, IEnumSTATSTG **clone)
{
	struct enm *e = enm_impl(self), *c;

	if (!clone)
		return STG_E_INVALIDPOINTER;
	*clone = NULL;
	if (!(c = enm_new(e->count)))
		return STG_E_INSUFFICIENTMEMORY;
	for (ULONG i = 0; i < e->count; i++) {
		size_t len = wstrlen(e->items[i].pwcsName);
		c->items[i] = e->items[i];
		c->items[i].pwcsName = malloc((len + 1) * sizeof(WCHAR));
		if (!c->items[i].pwcsName) {
			c->count = i;
			enm_Release(&c->iface);
			return STG_E_INSUFFICIENTMEMORY;
		}
		memcpy(c->items[i].pwcsName, e->items[i].pwcsName,
		       (len + 1) * sizeof(WCHAR));
	}
	c->pos = e->pos;
	*clone = &c->iface;
	return S_OK;
}

static const IEnumSTATSTGVtbl enm_vtbl = {
	enm_QueryInterface, enm_AddRef, enm_Release, enm_Next, enm_Skip,
	enm_Reset, enm_Clone,
};

WINFN(HRESULT) stg_EnumElements(IStorage *self, DWORD r1, void *r2, DWORD r3,
				IEnumSTATSTG **out)
{
	struct stg *s = stg_impl(self);
	HRESULT hr;

	if (!out)
		return STG_E_INVALIDPOINTER;
	*out = NULL;
	pthread_mutex_lock(&lock);
	if (FAILED(hr = usable(s, 0))) {
		pthread_mutex_unlock(&lock);
		return hr;
	}
	ULONG count = 0;
	for (struct node *c = s->n->child; c; c = c->next)
		count++;
	struct enm *e = enm_new(count);
	ULONG i = 0;
	for (struct node *c = s->n->child; e && c; c = c->next, i++) {
		STATSTG *st = &e->items[count - 1 - i];	/* oldest first */
		fill_stat(c, st, STATFLAG_NONAME, 0, NULL);
		st->pwcsName = malloc((c->len + 1) * sizeof(WCHAR));
		if (!st->pwcsName) {
			e->count = 0;
			enm_Release(&e->iface);
			e = NULL;
			break;
		}
		memcpy(st->pwcsName, c->name, c->len * sizeof(WCHAR));
		st->pwcsName[c->len] = 0;
	}
	pthread_mutex_unlock(&lock);
	if (!e)
		return STG_E_INSUFFICIENTMEMORY;
	*out = &e->iface;
	return S_OK;
}

WINFN(HRESULT) stg_DestroyElement(IStorage *self, LPCOLESTR name)
{
	struct stg *s = stg_impl(self);
	HRESULT hr;

	if (!name)
		return STG_E_INVALIDPOINTER;
	pthread_mutex_lock(&lock);
	hr = usable(s, 1);
	struct node *n = SUCCEEDED(hr) ? node_find(s->n, name) : NULL;
	if (SUCCEEDED(hr) && !n)
		hr = STG_E_FILENOTFOUND;
	else if (SUCCEEDED(hr) && n->open)
		hr = STG_E_ACCESSDENIED;
	if (SUCCEEDED(hr)) {
		node_unlink(n);
		touch(s->f, s->n);
	}
	pthread_mutex_unlock(&lock);
	return hr;
}

WINFN(HRESULT) stg_RenameElement(IStorage *self, LPCOLESTR old_name,
				 LPCOLESTR new_name)
{
	struct stg *s = stg_impl(self);
	HRESULT hr;

	if (!old_name)
		return STG_E_INVALIDPOINTER;
	if (FAILED(hr = check_name(new_name)))
		return hr;
	pthread_mutex_lock(&lock);
	hr = usable(s, 1);
	struct node *n = SUCCEEDED(hr) ? node_find(s->n, old_name) : NULL;
	if (SUCCEEDED(hr) && !n)
		hr = STG_E_FILENOTFOUND;
	else if (SUCCEEDED(hr) && node_find(s->n, new_name))
		hr = STG_E_FILEALREADYEXISTS;
	else if (SUCCEEDED(hr) && n->open)
		hr = STG_E_ACCESSDENIED;
	if (SUCCEEDED(hr)) {
		n->len = (int)wstrlen(new_name);
		memcpy(n->name, new_name, n->len * sizeof(WCHAR));
		n->name[n->len] = 0;
		touch(s->f, s->n);
	}
	pthread_mutex_unlock(&lock);
	return hr;
}

WINFN(HRESULT) stg_SetElementTimes(IStorage *self, LPCOLESTR name,
				   const FILETIME *ctime,
				   const FILETIME *atime,
				   const FILETIME *mtime)
{
	struct stg *s = stg_impl(self);
	HRESULT hr;

	pthread_mutex_lock(&lock);
	hr = usable(s, 1);
	struct node *n = SUCCEEDED(hr) ? (name ? node_find(s->n, name) : s->n) :
					 NULL;
	if (SUCCEEDED(hr) && !n)
		hr = STG_E_FILENOTFOUND;
	if (SUCCEEDED(hr)) {
		if (ctime)
			n->ctime = *ctime;
		if (mtime)
			n->mtime = *mtime;
		s->f->dirty = 1;
	}
	pthread_mutex_unlock(&lock);
	return hr;
}

WINFN(HRESULT) stg_SetClass(IStorage *self, REFCLSID clsid)
{
	struct stg *s = stg_impl(self);
	HRESULT hr;

	if (!clsid)
		return STG_E_INVALIDPOINTER;
	pthread_mutex_lock(&lock);
	if (SUCCEEDED(hr = usable(s, 1))) {
		s->n->clsid = *clsid;
		s->f->dirty = 1;
	}
	pthread_mutex_unlock(&lock);
	return hr;
}

WINFN(HRESULT) stg_SetStateBits(IStorage *self, DWORD bits, DWORD mask)
{
	struct stg *s = stg_impl(self);
	HRESULT hr;

	pthread_mutex_lock(&lock);
	if (SUCCEEDED(hr = usable(s, 1))) {
		s->n->state = (s->n->state & ~mask) | (bits & mask);
		s->f->dirty = 1;
	}
	pthread_mutex_unlock(&lock);
	return hr;
}

WINFN(HRESULT) stg_Stat(IStorage *self, STATSTG *st, DWORD flags)
{
	struct stg *s = stg_impl(self);
	HRESULT hr;

	if (!st)
		return STG_E_INVALIDPOINTER;
	pthread_mutex_lock(&lock);
	hr = usable(s, 0);
	if (SUCCEEDED(hr))
		hr = fill_stat(s->n, st, flags, s->mode,
			       s->n->parent ? NULL : s->f->name);
	if (SUCCEEDED(hr) && !s->n->parent && !s->f->name &&
	    !(flags & STATFLAG_NONAME)) {
		/* A storage on a byte array is named after it, if at all. */
		ole_taskmem_free(st->pwcsName);
		st->pwcsName = NULL;
	}
	pthread_mutex_unlock(&lock);
	return hr;
}

static const IStorageVtbl stg_vtbl = {
	stg_QueryInterface, stg_AddRef, stg_Release, stg_CreateStream,
	stg_OpenStream, stg_CreateStorage, stg_OpenStorage, stg_CopyTo,
	stg_MoveElementTo, stg_Commit, stg_Revert, stg_EnumElements,
	stg_DestroyElement, stg_RenameElement, stg_SetElementTimes,
	stg_SetClass, stg_SetStateBits, stg_Stat,
};

/* ---- byte arrays ---- */

/* On global memory: the block's size is the array's. */
struct hlb {
	ILockBytes iface;
	LONG refs;
	HGLOBAL h;
	BOOL delete_on_release;
};

/* On a host file. */
struct flb {
	ILockBytes iface;
	LONG refs;
	int fd;
	char *unlink_path;
	WCHAR *name;
};

static const ILockBytesVtbl hlb_vtbl, flb_vtbl;

WINFN(HRESULT) lb_QueryInterface(ILockBytes *self, REFIID riid, void **ppv)
{
	if (!ppv)
		return E_POINTER;
	if (guid_eq(riid, &IID_IUnknown) || guid_eq(riid, &IID_ILockBytes)) {
		*ppv = self;
		self->lpVtbl->AddRef(self);
		return S_OK;
	}
	*ppv = NULL;
	return E_NOINTERFACE;
}

WINFN(ULONG) hlb_AddRef(ILockBytes *self)
{
	return (ULONG)__atomic_add_fetch(&((struct hlb *)self)->refs, 1,
					 __ATOMIC_SEQ_CST);
}

WINFN(ULONG) hlb_Release(ILockBytes *self)
{
	struct hlb *l = (struct hlb *)self;
	LONG r = __atomic_sub_fetch(&l->refs, 1, __ATOMIC_SEQ_CST);
	if (!r) {
		if (l->delete_on_release)
			global_free(l->h);
		free(l);
	}
	return (ULONG)r;
}

WINFN(HRESULT) hlb_ReadAt(ILockBytes *self, ULARGE_INTEGER off, void *pv,
			  ULONG cb, ULONG *read)
{
	struct hlb *l = (struct hlb *)self;
	SIZE_T size = global_size(l->h);
	ULONG n = 0;

	if (!pv)
		return STG_E_INVALIDPOINTER;
	if (off.QuadPart < size) {
		n = size - off.QuadPart < cb ? (ULONG)(size - off.QuadPart) : cb;
		BYTE *p = global_lock(l->h);
		if (!p)
			return STG_E_READFAULT;
		memcpy(pv, p + off.QuadPart, n);
		global_unlock(l->h);
	}
	if (read)
		*read = n;
	return S_OK;
}

WINFN(HRESULT) hlb_SetSize(ILockBytes *self, ULARGE_INTEGER size)
{
	struct hlb *l = (struct hlb *)self;

	if (size.u.HighPart)
		return STG_E_MEDIUMFULL;
	if (size.u.LowPart == global_size(l->h))
		return S_OK;
	HGLOBAL h = global_realloc(l->h, size.u.LowPart, GMEM_MOVEABLE);
	if (!h)
		return STG_E_MEDIUMFULL;
	l->h = h;
	return S_OK;
}

WINFN(HRESULT) hlb_WriteAt(ILockBytes *self, ULARGE_INTEGER off,
			   const void *pv, ULONG cb, ULONG *written)
{
	struct hlb *l = (struct hlb *)self;

	if (written)
		*written = 0;
	if (!cb)
		return S_OK;
	if (!pv)
		return STG_E_INVALIDPOINTER;
	ULONGLONG end = off.QuadPart + cb;
	if (end > 0xffffffffu)
		return STG_E_MEDIUMFULL;
	if (end > global_size(l->h)) {
		ULARGE_INTEGER size;
		size.QuadPart = end;
		HRESULT hr = hlb_SetSize(self, size);
		if (FAILED(hr))
			return hr;
	}
	BYTE *p = global_lock(l->h);
	if (!p)
		return STG_E_WRITEFAULT;
	memcpy(p + off.QuadPart, pv, cb);
	global_unlock(l->h);
	if (written)
		*written = cb;
	return S_OK;
}

WINFN(HRESULT) lb_Flush(ILockBytes *self)
{
	return S_OK;
}

WINFN(HRESULT) lb_LockRegion(ILockBytes *self, ULARGE_INTEGER off,
			     ULARGE_INTEGER cb, DWORD type)
{
	return STG_E_INVALIDFUNCTION;
}

WINFN(HRESULT) lb_UnlockRegion(ILockBytes *self, ULARGE_INTEGER off,
			       ULARGE_INTEGER cb, DWORD type)
{
	return STG_E_INVALIDFUNCTION;
}

WINFN(HRESULT) hlb_Stat(ILockBytes *self, STATSTG *st, DWORD flags)
{
	if (!st)
		return STG_E_INVALIDPOINTER;
	memset(st, 0, sizeof(*st));
	st->type = STGTY_LOCKBYTES;
	st->cbSize.QuadPart = global_size(((struct hlb *)self)->h);
	return S_OK;
}

static const ILockBytesVtbl hlb_vtbl = {
	lb_QueryInterface, hlb_AddRef, hlb_Release, hlb_ReadAt, hlb_WriteAt,
	lb_Flush, hlb_SetSize, lb_LockRegion, lb_UnlockRegion, hlb_Stat,
};

WINFN(HRESULT) ole_CreateILockBytesOnHGlobal(HGLOBAL h, BOOL delete_on_release,
					     ILockBytes **out)
{
	if (!out)
		return E_INVALIDARG;
	*out = NULL;
	struct hlb *l = calloc(1, sizeof(*l));
	if (!l)
		return E_OUTOFMEMORY;
	l->h = h ? h : global_alloc(GMEM_MOVEABLE | GMEM_NODISCARD, 0);
	if (!l->h) {
		free(l);
		return E_OUTOFMEMORY;
	}
	l->iface.lpVtbl = &hlb_vtbl;
	l->refs = 1;
	l->delete_on_release = delete_on_release;
	*out = &l->iface;
	return S_OK;
}

WINFN(HRESULT) ole_GetHGlobalFromILockBytes(ILockBytes *lb, HGLOBAL *out)
{
	if (!out)
		return E_INVALIDARG;
	*out = NULL;
	if (!lb || lb->lpVtbl != &hlb_vtbl)
		return E_INVALIDARG;
	*out = ((struct hlb *)lb)->h;
	return S_OK;
}

WINFN(ULONG) flb_AddRef(ILockBytes *self)
{
	return (ULONG)__atomic_add_fetch(&((struct flb *)self)->refs, 1,
					 __ATOMIC_SEQ_CST);
}

WINFN(ULONG) flb_Release(ILockBytes *self)
{
	struct flb *l = (struct flb *)self;
	LONG r = __atomic_sub_fetch(&l->refs, 1, __ATOMIC_SEQ_CST);
	if (!r) {
		close(l->fd);
		if (l->unlink_path)
			unlink(l->unlink_path);
		free(l->unlink_path);
		free(l->name);
		free(l);
	}
	return (ULONG)r;
}

WINFN(HRESULT) flb_ReadAt(ILockBytes *self, ULARGE_INTEGER off, void *pv,
			  ULONG cb, ULONG *read)
{
	struct flb *l = (struct flb *)self;
	ULONG n = 0;

	if (!pv)
		return STG_E_INVALIDPOINTER;
	while (n < cb) {
		ssize_t k = pread(l->fd, (BYTE *)pv + n, cb - n,
				  (off_t)(off.QuadPart + n));
		if (k < 0 && errno == EINTR)
			continue;
		if (k < 0) {
			if (read)
				*read = n;
			return STG_E_READFAULT;
		}
		if (!k)
			break;
		n += (ULONG)k;
	}
	if (read)
		*read = n;
	return S_OK;
}

WINFN(HRESULT) flb_WriteAt(ILockBytes *self, ULARGE_INTEGER off,
			   const void *pv, ULONG cb, ULONG *written)
{
	struct flb *l = (struct flb *)self;
	ULONG n = 0;

	if (!pv && cb)
		return STG_E_INVALIDPOINTER;
	while (n < cb) {
		ssize_t k = pwrite(l->fd, (const BYTE *)pv + n, cb - n,
				   (off_t)(off.QuadPart + n));
		if (k < 0 && errno == EINTR)
			continue;
		if (k <= 0) {
			if (written)
				*written = n;
			return errno == ENOSPC ? STG_E_MEDIUMFULL :
						 STG_E_WRITEFAULT;
		}
		n += (ULONG)k;
	}
	if (written)
		*written = n;
	return S_OK;
}

WINFN(HRESULT) flb_SetSize(ILockBytes *self, ULARGE_INTEGER size)
{
	struct flb *l = (struct flb *)self;
	return ftruncate(l->fd, (off_t)size.QuadPart) ? STG_E_MEDIUMFULL : S_OK;
}

WINFN(HRESULT) flb_Stat(ILockBytes *self, STATSTG *st, DWORD flags)
{
	struct flb *l = (struct flb *)self;
	struct stat sb;

	if (!st)
		return STG_E_INVALIDPOINTER;
	memset(st, 0, sizeof(*st));
	if (fstat(l->fd, &sb))
		return STG_E_ACCESSDENIED;
	st->type = STGTY_LOCKBYTES;
	st->cbSize.QuadPart = (ULONGLONG)sb.st_size;
	if (!(flags & STATFLAG_NONAME) && l->name) {
		size_t len = wstrlen(l->name);
		st->pwcsName = ole_taskmem_alloc((len + 1) * sizeof(WCHAR));
		if (!st->pwcsName)
			return STG_E_INSUFFICIENTMEMORY;
		memcpy(st->pwcsName, l->name, (len + 1) * sizeof(WCHAR));
	}
	return S_OK;
}

static const ILockBytesVtbl flb_vtbl = {
	lb_QueryInterface, flb_AddRef, flb_Release, flb_ReadAt, flb_WriteAt,
	lb_Flush, flb_SetSize, lb_LockRegion, lb_UnlockRegion, flb_Stat,
};

/* Opens (or with create, makes) a file as a byte array. */
static HRESULT file_lockbytes(const WCHAR *name, DWORD mode, int create,
			      ILockBytes **out)
{
	char *u8 = utf16_to_utf8_dup(name);
	char *path = u8 ? path_to_unix(u8) : NULL;
	free(u8);
	if (!path)
		return STG_E_PATHNOTFOUND;
	int flags = writable(mode) ? O_RDWR : O_RDONLY;
	if (create)
		flags |= O_CREAT | ((mode & STGM_CREATE) ? O_TRUNC : O_EXCL);
	int fd = open(path, flags | O_CLOEXEC, 0644);
	if (fd < 0) {
		int e = errno;
		free(path);
		switch (e) {
		case ENOENT: return create ? STG_E_PATHNOTFOUND :
					     STG_E_FILENOTFOUND;
		case ENOTDIR: return STG_E_PATHNOTFOUND;
		case EEXIST: return STG_E_FILEALREADYEXISTS;
		default: return STG_E_ACCESSDENIED;
		}
	}
	struct flb *l = calloc(1, sizeof(*l));
	if (!l) {
		close(fd);
		free(path);
		return E_OUTOFMEMORY;
	}
	l->iface.lpVtbl = &flb_vtbl;
	l->refs = 1;
	l->fd = fd;
	l->name = malloc((wstrlen(name) + 1) * sizeof(WCHAR));
	if (l->name)
		memcpy(l->name, name, (wstrlen(name) + 1) * sizeof(WCHAR));
	if (mode & STGM_DELETEONRELEASE)
		l->unlink_path = path;
	else
		free(path);
	*out = &l->iface;
	return S_OK;
}

/* ---- creating and opening ---- */

static HRESULT open_root(ILockBytes *lb, DWORD mode, int create,
			 const WCHAR *name, IStorage **out)
{
	struct file *f = calloc(1, sizeof(*f));
	HRESULT hr;

	if (!f)
		return E_OUTOFMEMORY;
	f->lb = lb;
	lb->lpVtbl->AddRef(lb);
	f->mode = mode;
	if (name) {
		f->name = malloc((wstrlen(name) + 1) * sizeof(WCHAR));
		if (f->name)
			memcpy(f->name, name, (wstrlen(name) + 1) * sizeof(WCHAR));
	}
	pthread_mutex_lock(&lock);
	if (create) {
		static const WCHAR rn[] = { 'R','o','o','t',' ','E','n','t','r','y' };
		f->root = node_new(rn, 10, STGTY_STORAGE);
		hr = f->root ? S_OK : E_OUTOFMEMORY;
		if (SUCCEEDED(hr)) {
			now(&f->root->ctime);
			f->root->mtime = f->root->ctime;
			f->dirty = 1;
			/* The header is there from the start, as on Windows. */
			hr = file_flush(f);
			f->dirty = 0;
		}
	} else {
		hr = file_load(f, 0);
	}
	if (SUCCEEDED(hr)) {
		f->refs = 1;
		hr = stg_new(f, f->root, mode, out);
		file_unref(f);	/* the storage holds it now */
	} else {
		if (f->root)
			node_detach(f->root);
		lb->lpVtbl->Release(lb);
		free(f->name);
		free(f);
	}
	pthread_mutex_unlock(&lock);
	return hr;
}

static HRESULT check_root_mode(DWORD mode)
{
	if ((mode & 3) == 3 || (mode & 0x70) > STGM_SHARE_DENY_NONE)
		return STG_E_INVALIDFLAG;
	if ((mode & STGM_CREATE) && (mode & STGM_CONVERT))
		return STG_E_INVALIDFLAG;
	return S_OK;
}

static const WCHAR contents_name[] = {
	3, 'C', 'O', 'N', 'T', 'E', 'N', 'T', 'S', 0
};

WINFN(HRESULT) ole_StgCreateDocfile(LPCOLESTR name, DWORD mode, DWORD reserved,
				    IStorage **out)
{
	ILockBytes *lb = NULL;
	HRESULT hr;
	BYTE *old = NULL;
	size_t old_len = 0;

	if (!out)
		return STG_E_INVALIDPOINTER;
	*out = NULL;
	if (reserved)
		return STG_E_INVALIDPARAMETER;
	if (FAILED(hr = check_root_mode(mode)))
		return hr;
	if (!writable(mode))
		return STG_E_INVALIDFLAG;
	if (!name) {
		/* A temporary storage, in memory. */
		hr = ole_CreateILockBytesOnHGlobal(NULL, TRUE, &lb);
	} else if (mode & STGM_CONVERT) {
		/* What the file held goes into a CONTENTS stream. */
		hr = file_lockbytes(name, mode, 0, &lb);
		if (SUCCEEDED(hr))
			hr = lb_read_all(lb, &old, &old_len);
		if (hr == STG_E_FILENOTFOUND) {
			hr = file_lockbytes(name, mode | STGM_CREATE, 1, &lb);
		}
	} else {
		hr = file_lockbytes(name, mode, 1, &lb);
	}
	if (FAILED(hr)) {
		if (lb)
			lb->lpVtbl->Release(lb);
		return hr;
	}
	hr = open_root(lb, mode, 1, name, out);
	lb->lpVtbl->Release(lb);
	if (SUCCEEDED(hr) && old) {
		IStream *s;
		hr = (*out)->lpVtbl->CreateStream(*out, contents_name,
						  STGM_READWRITE |
						  STGM_SHARE_EXCLUSIVE, 0, 0, &s);
		if (SUCCEEDED(hr)) {
			hr = s->lpVtbl->Write(s, old, (ULONG)old_len, NULL);
			s->lpVtbl->Release(s);
		}
		if (SUCCEEDED(hr) && !(mode & STGM_TRANSACTED))
			hr = (*out)->lpVtbl->Commit(*out, 0);
		if (SUCCEEDED(hr))
			hr = STG_S_CONVERTED;
		else {
			(*out)->lpVtbl->Release(*out);
			*out = NULL;
		}
	}
	free(old);
	return hr;
}

WINFN(HRESULT) ole_StgCreateDocfileOnILockBytes(ILockBytes *lb, DWORD mode,
						DWORD reserved, IStorage **out)
{
	HRESULT hr;

	if (!out || !lb)
		return STG_E_INVALIDPOINTER;
	*out = NULL;
	if (reserved)
		return STG_E_INVALIDPARAMETER;
	if (FAILED(hr = check_root_mode(mode)))
		return hr;
	if (!(mode & STGM_CREATE)) {
		/* Without STGM_CREATE, a storage already there isn't replaced. */
		ULONGLONG size = 0;
		BYTE head[8];
		ULONG got = 0;
		ULARGE_INTEGER zero = { 0 };
		if (SUCCEEDED(lb_size(lb, &size)) && size >= SECT &&
		    SUCCEEDED(lb->lpVtbl->ReadAt(lb, zero, head, 8, &got)) &&
		    got == 8 && !memcmp(head, signature, 8))
			return STG_E_FILEALREADYEXISTS;
	}
	return open_root(lb, mode, 1, NULL, out);
}

WINFN(HRESULT) ole_StgOpenStorageOnILockBytes(ILockBytes *lb, IStorage *prio,
					      DWORD mode, SNB exclude,
					      DWORD reserved, IStorage **out)
{
	HRESULT hr;

	if (!out || !lb)
		return STG_E_INVALIDPOINTER;
	*out = NULL;
	if (FAILED(hr = check_root_mode(mode)))
		return hr;
	return open_root(lb, mode, 0, NULL, out);
}

WINFN(HRESULT) ole_StgOpenStorage(LPCOLESTR name, IStorage *prio, DWORD mode,
				  SNB exclude, DWORD reserved, IStorage **out)
{
	ILockBytes *lb;
	HRESULT hr;

	if (!out)
		return STG_E_INVALIDPOINTER;
	*out = NULL;
	if (!name)
		return STG_E_INVALIDNAME;
	if (FAILED(hr = check_root_mode(mode)))
		return hr;
	if (FAILED(hr = file_lockbytes(name, mode, 0, &lb)))
		return hr;
	hr = open_root(lb, mode, 0, name, out);
	lb->lpVtbl->Release(lb);
	return hr;
}

WINFN(HRESULT) ole_StgIsStorageILockBytes(ILockBytes *lb)
{
	BYTE head[8];
	ULONG got = 0;
	ULARGE_INTEGER zero = { 0 };

	if (!lb)
		return STG_E_INVALIDPOINTER;
	HRESULT hr = lb->lpVtbl->ReadAt(lb, zero, head, 8, &got);
	if (FAILED(hr))
		return hr;
	return got == 8 && !memcmp(head, signature, 8) ? S_OK : S_FALSE;
}

WINFN(HRESULT) ole_StgIsStorageFile(LPCOLESTR name)
{
	ILockBytes *lb;
	HRESULT hr;

	if (!name)
		return STG_E_INVALIDNAME;
	if (FAILED(hr = file_lockbytes(name, STGM_READ, 0, &lb)))
		return hr;
	hr = ole_StgIsStorageILockBytes(lb);
	lb->lpVtbl->Release(lb);
	return hr;
}

WINFN(HRESULT) ole_StgCreateStorageEx(LPCOLESTR name, DWORD mode, DWORD fmt,
				      DWORD attrs, void *opts, void *sd,
				      REFIID riid, void **out)
{
	IStorage *stg;
	HRESULT hr;

	if (!out)
		return STG_E_INVALIDPOINTER;
	*out = NULL;
	if (fmt != STGFMT_STORAGE && fmt != STGFMT_DOCFILE)
		return fmt == STGFMT_FILE ? STG_E_UNIMPLEMENTEDFUNCTION :
					    STG_E_INVALIDPARAMETER;
	hr = ole_StgCreateDocfile(name, mode, 0, &stg);
	if (FAILED(hr))
		return hr;
	HRESULT qi = stg->lpVtbl->QueryInterface(stg, riid, out);
	stg->lpVtbl->Release(stg);
	return FAILED(qi) ? qi : hr;
}

WINFN(HRESULT) ole_StgOpenStorageEx(LPCOLESTR name, DWORD mode, DWORD fmt,
				    DWORD attrs, void *opts, void *sd,
				    REFIID riid, void **out)
{
	IStorage *stg;
	HRESULT hr;

	if (!out)
		return STG_E_INVALIDPOINTER;
	*out = NULL;
	if (fmt != STGFMT_STORAGE && fmt != STGFMT_DOCFILE && fmt != STGFMT_ANY)
		return fmt == STGFMT_FILE ? STG_E_UNIMPLEMENTEDFUNCTION :
					    STG_E_INVALIDPARAMETER;
	hr = ole_StgOpenStorage(name, NULL, mode, NULL, 0, &stg);
	if (FAILED(hr))
		return hr;
	hr = stg->lpVtbl->QueryInterface(stg, riid, out);
	stg->lpVtbl->Release(stg);
	return hr;
}

/* ---- classes and user types ---- */

WINFN(HRESULT) ole_ReadClassStg(IStorage *stg, CLSID *clsid)
{
	STATSTG st;

	if (!stg || !clsid)
		return E_INVALIDARG;
	HRESULT hr = stg->lpVtbl->Stat(stg, &st, STATFLAG_NONAME);
	*clsid = SUCCEEDED(hr) ? st.clsid : (CLSID){ 0 };
	return hr;
}

WINFN(HRESULT) ole_WriteClassStg(IStorage *stg, REFCLSID clsid)
{
	if (!stg || !clsid)
		return E_INVALIDARG;
	return stg->lpVtbl->SetClass(stg, clsid);
}

WINFN(HRESULT) ole_ReadClassStm(IStream *stm, CLSID *clsid)
{
	ULONG got = 0;

	if (!stm || !clsid)
		return E_INVALIDARG;
	HRESULT hr = stm->lpVtbl->Read(stm, clsid, sizeof(*clsid), &got);
	if (FAILED(hr))
		return hr;
	return got == sizeof(*clsid) ? S_OK : STG_E_READFAULT;
}

WINFN(HRESULT) ole_WriteClassStm(IStream *stm, REFCLSID clsid)
{
	if (!stm || !clsid)
		return E_INVALIDARG;
	return stm->lpVtbl->Write(stm, clsid, sizeof(*clsid), NULL);
}

static const WCHAR compobj_name[] = {
	1, 'C', 'o', 'm', 'p', 'O', 'b', 'j', 0
};
static const WCHAR ole_name[] = { 1, 'O', 'l', 'e', 0 };

#define UNICODE_MARKER 0x71b239f4

struct buf {
	BYTE *p;
	size_t len, cap;
	int bad;
};

static void put(struct buf *b, const void *p, size_t n)
{
	if (b->len + n > b->cap) {
		size_t cap = b->cap ? b->cap * 2 : 256;
		while (cap < b->len + n)
			cap *= 2;
		BYTE *q = realloc(b->p, cap);
		if (!q) {
			b->bad = 1;
			return;
		}
		b->p = q;
		b->cap = cap;
	}
	memcpy(b->p + b->len, p, n);
	b->len += n;
}

static void put32(struct buf *b, DWORD v)
{
	BYTE x[4];
	wr32(x, v);
	put(b, x, 4);
}

/* A LengthPrefixedAnsiString: the length with the NUL, or 0 for none. */
static void put_ansi(struct buf *b, const char *s)
{
	if (!s || !*s) {
		put32(b, 0);
		return;
	}
	put32(b, (DWORD)strlen(s) + 1);
	put(b, s, strlen(s) + 1);
}

static void put_unicode(struct buf *b, const WCHAR *s)
{
	size_t n = s ? wstrlen(s) : 0;
	if (!n) {
		put32(b, 0);
		return;
	}
	put32(b, (DWORD)n + 1);
	for (size_t i = 0; i <= n; i++) {
		BYTE x[2];
		wr16(x, s[i]);
		put(b, x, 2);
	}
}

static int (WINAPI *get_format_name(void))(UINT, LPSTR, int)
{
	return builtin_export(&builtin_user32, "GetClipboardFormatNameA");
}

/* A clipboard format: standard ones by number, registered ones by name. */
static void put_format(struct buf *b, CLIPFORMAT cf, int unicode)
{
	char name[256];
	int (WINAPI *get)(UINT, LPSTR, int) = cf >= 0xc000 ?
		get_format_name() : NULL;
	int n = get ? get(cf, name, sizeof(name)) : 0;

	if (!cf) {
		put32(b, 0);
	} else if (n <= 0) {
		put32(b, 0xffffffff);
		put32(b, cf);
	} else if (!unicode) {
		put_ansi(b, name);
	} else {
		WCHAR *w = acp_to_utf16_dup(name);
		put_unicode(b, w);
		free(w);
	}
}

WINFN(HRESULT) ole_WriteFmtUserTypeStg(IStorage *stg, CLIPFORMAT cf,
				       LPOLESTR user_type)
{
	static const BYTE header[12] = {
		0x01, 0x00, 0xfe, 0xff, 0x03, 0x0a, 0x00, 0x00,
		0xff, 0xff, 0xff, 0xff,
	};
	STATSTG st;
	struct buf b = { 0 };
	IStream *s;
	HRESULT hr;

	if (!stg)
		return E_INVALIDARG;
	if (FAILED(hr = stg->lpVtbl->Stat(stg, &st, STATFLAG_NONAME)))
		return hr;
	char *ansi = user_type ? utf16_to_acp_dup(user_type) : NULL;
	LPOLESTR progid = NULL;
	HRESULT (WINAPI *to_progid)(REFCLSID, LPOLESTR *) =
		builtin_export(&builtin_ole32, "ProgIDFromCLSID");
	if (to_progid && FAILED(to_progid(&st.clsid, &progid)))
		progid = NULL;
	char *progid_a = progid ? utf16_to_acp_dup(progid) : NULL;

	put(&b, header, sizeof(header));
	put(&b, &st.clsid, sizeof(st.clsid));
	put_ansi(&b, ansi);
	put_format(&b, cf, 0);
	put_ansi(&b, progid_a);
	put32(&b, UNICODE_MARKER);
	put_unicode(&b, user_type);
	put_format(&b, cf, 1);
	put_unicode(&b, progid);
	free(ansi);
	free(progid_a);
	ole_taskmem_free(progid);
	if (b.bad) {
		free(b.p);
		return E_OUTOFMEMORY;
	}
	hr = stg->lpVtbl->CreateStream(stg, compobj_name, STGM_CREATE |
				       STGM_WRITE | STGM_SHARE_EXCLUSIVE, 0, 0,
				       &s);
	if (SUCCEEDED(hr)) {
		hr = s->lpVtbl->Write(s, b.p, (ULONG)b.len, NULL);
		s->lpVtbl->Release(s);
	}
	free(b.p);
	return hr;
}

struct rd {
	const BYTE *p;
	size_t len, at;
};

static int get32(struct rd *r, DWORD *v)
{
	if (r->at + 4 > r->len)
		return -1;
	*v = rd32(r->p + r->at);
	r->at += 4;
	return 0;
}

static int get_ansi(struct rd *r, char **out)
{
	DWORD n;
	*out = NULL;
	if (get32(r, &n) || n > r->len - r->at)
		return -1;
	if (n) {
		*out = malloc(n + 1);
		if (!*out)
			return -1;
		memcpy(*out, r->p + r->at, n);
		(*out)[n] = 0;
		r->at += n;
	}
	return 0;
}

static int get_format(struct rd *r, CLIPFORMAT *cf)
{
	DWORD n;
	*cf = 0;
	if (get32(r, &n))
		return -1;
	if (n == 0xffffffff || n == 0xfffffffe) {
		DWORD v;
		if (get32(r, &v))
			return -1;
		*cf = (CLIPFORMAT)v;
		return 0;
	}
	if (!n)
		return 0;
	r->at -= 4;
	char *name;
	if (get_ansi(r, &name))
		return -1;
	UINT (WINAPI *reg)(LPCSTR) =
		builtin_export(&builtin_user32, "RegisterClipboardFormatA");
	*cf = reg && name ? (CLIPFORMAT)reg(name) : 0;
	free(name);
	return 0;
}

WINFN(HRESULT) ole_ReadFmtUserTypeStg(IStorage *stg, CLIPFORMAT *cf,
				      LPOLESTR *user_type)
{
	IStream *s;
	HRESULT hr;

	if (!stg)
		return E_INVALIDARG;
	if (cf)
		*cf = 0;
	if (user_type)
		*user_type = NULL;
	hr = stg->lpVtbl->OpenStream(stg, compobj_name, NULL, STGM_READ |
				     STGM_SHARE_EXCLUSIVE, 0, &s);
	if (FAILED(hr))
		return hr;
	BYTE data[4096];
	ULONG got = 0;
	hr = s->lpVtbl->Read(s, data, sizeof(data), &got);
	s->lpVtbl->Release(s);
	if (FAILED(hr))
		return hr;
	struct rd r = { data, got, 28 };
	char *type = NULL;
	CLIPFORMAT fmt;
	if (got < 28 || get_ansi(&r, &type) || get_format(&r, &fmt)) {
		free(type);
		return STG_E_READFAULT;
	}
	if (cf)
		*cf = fmt;
	if (user_type && type) {
		WCHAR *w = acp_to_utf16_dup(type);
		size_t n = w ? wstrlen(w) : 0;
		*user_type = w ? ole_taskmem_alloc((n + 1) * sizeof(WCHAR)) : NULL;
		if (*user_type)
			memcpy(*user_type, w, (n + 1) * sizeof(WCHAR));
		free(w);
		if (!*user_type)
			hr = E_OUTOFMEMORY;
	}
	free(type);
	return hr;
}

/* The "\1Ole" stream: version, flags (bit 2: convert), then zeros. */
#define OLESTREAM_CONVERT 4

WINFN(HRESULT) ole_SetConvertStg(IStorage *stg, BOOL convert)
{
	BYTE data[20] = { 0x01, 0x00, 0x00, 0x02 };
	IStream *s;
	HRESULT hr;

	if (!stg)
		return E_INVALIDARG;
	hr = stg->lpVtbl->OpenStream(stg, ole_name, NULL, STGM_READWRITE |
				     STGM_SHARE_EXCLUSIVE, 0, &s);
	if (hr == STG_E_FILENOTFOUND) {
		if (!convert)
			return S_OK;
		hr = stg->lpVtbl->CreateStream(stg, ole_name, STGM_READWRITE |
					       STGM_SHARE_EXCLUSIVE, 0, 0, &s);
	} else if (SUCCEEDED(hr)) {
		ULONG got = 0;
		s->lpVtbl->Read(s, data, sizeof(data), &got);
		LARGE_INTEGER zero = { 0 };
		s->lpVtbl->Seek(s, zero, STREAM_SEEK_SET, NULL);
	}
	if (FAILED(hr))
		return hr;
	DWORD flags = rd32(data + 4);
	flags = convert ? flags | OLESTREAM_CONVERT : flags & ~OLESTREAM_CONVERT;
	wr32(data + 4, flags);
	hr = s->lpVtbl->Write(s, data, sizeof(data), NULL);
	s->lpVtbl->Release(s);
	return hr;
}

WINFN(HRESULT) ole_GetConvertStg(IStorage *stg)
{
	BYTE data[20];
	ULONG got = 0;
	IStream *s;

	if (!stg)
		return E_INVALIDARG;
	HRESULT hr = stg->lpVtbl->OpenStream(stg, ole_name, NULL, STGM_READ |
					     STGM_SHARE_EXCLUSIVE, 0, &s);
	if (hr == STG_E_FILENOTFOUND)
		return S_FALSE;
	if (FAILED(hr))
		return hr;
	hr = s->lpVtbl->Read(s, data, sizeof(data), &got);
	s->lpVtbl->Release(s);
	if (FAILED(hr) || got < 8)
		return FAILED(hr) ? hr : S_FALSE;
	return (rd32(data + 4) & OLESTREAM_CONVERT) ? S_OK : S_FALSE;
}

/* ---- saving and loading objects ---- */

static HRESULT create_object(REFCLSID clsid, REFIID riid, void **out)
{
	HRESULT (WINAPI *create)(REFCLSID, IUnknown *, DWORD, REFIID, void **) =
		builtin_export(&builtin_ole32, "CoCreateInstance");
	return create ? create(clsid, NULL, 0x17 /* CLSCTX_ALL */, riid, out) :
			E_UNEXPECTED;
}

WINFN(HRESULT) ole_OleSaveToStream(IPersistStream *obj, IStream *stm)
{
	CLSID clsid = { 0 };
	HRESULT hr;

	if (!stm)
		return E_INVALIDARG;
	if (!obj)
		return ole_WriteClassStm(stm, &clsid);
	if (FAILED(hr = obj->lpVtbl->GetClassID(obj, &clsid)))
		return hr;
	if (FAILED(hr = ole_WriteClassStm(stm, &clsid)))
		return hr;
	return obj->lpVtbl->Save(obj, stm, TRUE);
}

WINFN(HRESULT) ole_OleLoadFromStream(IStream *stm, REFIID riid, void **out)
{
	CLSID clsid;
	IPersistStream *ps;
	HRESULT hr;

	if (!out)
		return E_INVALIDARG;
	*out = NULL;
	if (FAILED(hr = ole_ReadClassStm(stm, &clsid)))
		return hr;
	if (FAILED(hr = create_object(&clsid, &IID_IPersistStream, (void **)&ps)))
		return hr;
	hr = ps->lpVtbl->Load(ps, stm);
	if (SUCCEEDED(hr))
		hr = ps->lpVtbl->QueryInterface(ps, riid, out);
	ps->lpVtbl->Release(ps);
	return hr;
}

WINFN(HRESULT) ole_OleSave(IPersistStorage *obj, IStorage *stg, BOOL same)
{
	CLSID clsid;
	HRESULT hr;

	if (!obj || !stg)
		return E_INVALIDARG;
	if (FAILED(hr = obj->lpVtbl->GetClassID(obj, &clsid)))
		return hr;
	if (FAILED(hr = ole_WriteClassStg(stg, &clsid)))
		return hr;
	hr = obj->lpVtbl->Save(obj, stg, same);
	if (SUCCEEDED(hr))
		stg->lpVtbl->Commit(stg, STGC_DEFAULT);
	return hr;
}

WINFN(HRESULT) ole_OleLoad(IStorage *stg, REFIID riid, void *client_site,
			   void **out)
{
	CLSID clsid;
	IPersistStorage *ps;
	HRESULT hr;

	if (!out)
		return E_INVALIDARG;
	*out = NULL;
	if (FAILED(hr = ole_ReadClassStg(stg, &clsid)))
		return hr;
	if (FAILED(hr = create_object(&clsid, &IID_IPersistStorage, (void **)&ps)))
		return hr;
	hr = ps->lpVtbl->Load(ps, stg);
	if (SUCCEEDED(hr))
		hr = ps->lpVtbl->QueryInterface(ps, riid, out);
	ps->lpVtbl->Release(ps);
	return hr;
}

#define E(name) { #name, 0, (void *)ole_##name }

const struct builtin_export ole_storage_exports[] = {
	E(CreateILockBytesOnHGlobal),
	E(GetConvertStg),
	E(GetHGlobalFromILockBytes),
	E(OleLoad),
	E(OleLoadFromStream),
	E(OleSave),
	E(OleSaveToStream),
	E(ReadClassStg),
	E(ReadClassStm),
	E(ReadFmtUserTypeStg),
	E(SetConvertStg),
	E(StgCreateDocfile),
	E(StgCreateDocfileOnILockBytes),
	E(StgCreateStorageEx),
	E(StgIsStorageFile),
	E(StgIsStorageILockBytes),
	E(StgOpenStorage),
	E(StgOpenStorageEx),
	E(StgOpenStorageOnILockBytes),
	E(WriteClassStg),
	E(WriteClassStm),
	E(WriteFmtUserTypeStg),
	BUILTIN_END
};
