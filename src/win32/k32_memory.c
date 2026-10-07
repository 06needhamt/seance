/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * kernel32: heaps, Local/Global memory and virtual memory.
 */
#include <errno.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/sysinfo.h>
#include <unistd.h>

#include "loader/modules.h"
#include "win32/internal.h"
#include "win32/k32.h"

#define PAGE     0x1000u
#define GRANULE  0x10000u		/* allocation granularity */
#define USER_TOP 0x7fff0000u		/* end of the Windows user address space */
/*
 * The real top of a 32-bit process on a 64-bit Linux kernel. Unlike on
 * Windows, mmap() hands out addresses above 2 GB (glibc's heap and our
 * reservations usually live near 0xf7000000), so queries must cover them.
 */
#define ADDRESS_TOP 0xffff0000u

/* Heaps. */

#define HEAP_MAGIC 0x50414548		/* "HEAP" */

struct block {
	struct block *prev, *next;	/* the heap's list, for HeapDestroy */
	struct heap *heap;
	SIZE_T size;
	struct block *hash_next;	/* the live-block set */
	DWORD pad[3];
};

_Static_assert(sizeof(struct block) == 32, "keeps allocations 16-byte aligned");

struct heap {
	DWORD magic;
	DWORD flags;
	pthread_mutex_t lock;
	struct block *blocks;
	struct heap *next;
};

static struct heap *heaps;
static struct heap *process_heap;
static pthread_mutex_t heaps_lock = PTHREAD_MUTEX_INITIALIZER;

static struct heap *heap_new(DWORD flags)
{
	struct heap *h = calloc(1, sizeof(*h));
	pthread_mutexattr_t attr;

	if (!h)
		return NULL;
	h->magic = HEAP_MAGIC;
	h->flags = flags;
	pthread_mutexattr_init(&attr);
	pthread_mutexattr_settype(&attr, PTHREAD_MUTEX_RECURSIVE);
	pthread_mutex_init(&h->lock, &attr);
	pthread_mutexattr_destroy(&attr);
	pthread_mutex_lock(&heaps_lock);
	h->next = heaps;
	heaps = h;
	pthread_mutex_unlock(&heaps_lock);
	return h;
}

HANDLE heap_process(void)
{
	pthread_mutex_lock(&heaps_lock);
	int need = !process_heap;
	pthread_mutex_unlock(&heaps_lock);
	if (need) {
		struct heap *h = heap_new(HEAP_GROWABLE);
		pthread_mutex_lock(&heaps_lock);
		if (!process_heap) {
			process_heap = h;
			peb_get()->ProcessHeap = h;
		}
		pthread_mutex_unlock(&heaps_lock);
	}
	return process_heap;
}

/* The heap behind a handle, or NULL if it is not a live heap. */
static struct heap *heap_get(HANDLE handle)
{
	struct heap *found = NULL;

	pthread_mutex_lock(&heaps_lock);
	for (struct heap *h = heaps; h && !found; h = h->next)
		if (h == handle && h->magic == HEAP_MAGIC)
			found = h;
	pthread_mutex_unlock(&heaps_lock);
	if (!found)
		win_set_error(ERROR_INVALID_HANDLE);
	return found;
}

/*
 * Every live block is in one hash set keyed by its user pointer, so that
 * freeing a pointer twice, or one that never came from a heap, is caught
 * without reading memory that may no longer be ours.
 */
static struct block **live;
static size_t live_buckets, live_count;
static pthread_mutex_t live_lock = PTHREAD_MUTEX_INITIALIZER;

static size_t live_hash(const void *p, size_t buckets)
{
	uintptr_t v = (uintptr_t)p >> 4;
	return (v * 2654435761u) & (buckets - 1);
}

static void live_grow(void)
{
	size_t n = live_buckets ? live_buckets * 2 : 1024;
	struct block **t = calloc(n, sizeof(*t));

	if (!t)
		return;		/* keep the old table; chains just get longer */
	for (size_t i = 0; i < live_buckets; i++) {
		for (struct block *b = live[i], *next; b; b = next) {
			next = b->hash_next;
			size_t k = live_hash(b + 1, n);
			b->hash_next = t[k];
			t[k] = b;
		}
	}
	free(live);
	live = t;
	live_buckets = n;
}

static int live_insert(struct block *b)
{
	pthread_mutex_lock(&live_lock);
	if (live_count >= live_buckets * 2)
		live_grow();
	if (!live_buckets) {
		pthread_mutex_unlock(&live_lock);
		return -1;
	}
	size_t k = live_hash(b + 1, live_buckets);
	b->hash_next = live[k];
	live[k] = b;
	live_count++;
	pthread_mutex_unlock(&live_lock);
	return 0;
}

/* Finds (and with remove, takes out) the live block for user pointer p. */
static struct block *live_find(const void *p, int remove)
{
	struct block *found = NULL;

	pthread_mutex_lock(&live_lock);
	if (live_buckets) {
		struct block **link = &live[live_hash(p, live_buckets)];
		for (; *link; link = &(*link)->hash_next) {
			if (*link + 1 == p) {
				found = *link;
				if (remove) {
					*link = found->hash_next;
					live_count--;
				}
				break;
			}
		}
	}
	pthread_mutex_unlock(&live_lock);
	return found;
}

/* The block for p if it is live and belongs to heap h. */
static struct block *block_of(struct heap *h, const void *p)
{
	struct block *b = live_find(p, 0);

	return b && b->heap == h ? b : NULL;
}

/* A thorough check for HeapValidate: is p really in h's block list? */
static int block_listed(struct heap *h, const void *p)
{
	for (struct block *x = h->blocks; x; x = x->next)
		if (x + 1 == p)
			return 1;
	return 0;
}

void *heap_alloc(HANDLE handle, DWORD flags, SIZE_T size)
{
	struct heap *h = heap_get(handle);
	struct block *b;

	if (!h)
		return NULL;
	if (size > 0x7fff0000u - sizeof(*b)) {
		win_set_error(ERROR_NOT_ENOUGH_MEMORY);
		return NULL;
	}
	/*
	 * Zeroed by default, which HEAP_ZERO_MEMORY only promises: memory fresh
	 * from a Windows heap usually is, and code that was never run on anything
	 * else can depend on it (VFW_ZERO_MEMORY=0 turns this off). Honour the
	 * flag even then.
	 */
	b = win_alloc(1, sizeof(*b) + size);
	if (b && !win_zero_memory()) {
		memset(b, 0, sizeof(*b));
		if (flags & HEAP_ZERO_MEMORY)
			memset(b + 1, 0, size);
	}
	if (!b) {
		win_set_error(ERROR_NOT_ENOUGH_MEMORY);
		return NULL;
	}
	b->heap = h;
	b->size = size;
	b->prev = NULL;
	if (live_insert(b)) {
		free(b);
		win_set_error(ERROR_NOT_ENOUGH_MEMORY);
		return NULL;
	}
	pthread_mutex_lock(&h->lock);
	b->next = h->blocks;
	if (h->blocks)
		h->blocks->prev = b;
	h->blocks = b;
	pthread_mutex_unlock(&h->lock);
	return b + 1;
}

int win_zero_memory(void)
{
	static int zero = -1;

	if (zero < 0) {
		const char *env = getenv("VFW_ZERO_MEMORY");
		zero = !(env && !strcmp(env, "0"));
	}
	return zero;
}

void *win_alloc(size_t n, size_t size)
{
	return win_zero_memory() ? calloc(n, size) : malloc(n * size);
}

static void unlink_block(struct heap *h, struct block *b)
{
	if (b->prev)
		b->prev->next = b->next;
	else
		h->blocks = b->next;
	if (b->next)
		b->next->prev = b->prev;
}

BOOL heap_free(HANDLE handle, DWORD flags, void *p)
{
	struct heap *h;
	struct block *b;

	if (!p)
		return TRUE;
	if (!(h = heap_get(handle)))
		return FALSE;
	pthread_mutex_lock(&h->lock);
	b = block_of(h, p);
	if (b) {
		live_find(p, 1);
		unlink_block(h, b);
	}
	pthread_mutex_unlock(&h->lock);
	if (!b) {
		TRACE("HeapFree(%p, %p): not a block of this heap", handle, p);
		win_set_error(ERROR_INVALID_PARAMETER);
		return FALSE;
	}
	free(b);
	return TRUE;
}

SIZE_T heap_size(HANDLE handle, DWORD flags, const void *p)
{
	struct heap *h = heap_get(handle);
	SIZE_T size = (SIZE_T)-1;

	if (!h)
		return size;
	pthread_mutex_lock(&h->lock);
	struct block *b = block_of(h, p);
	if (b)
		size = b->size;
	pthread_mutex_unlock(&h->lock);
	if (!b)
		win_set_error(ERROR_INVALID_PARAMETER);
	return size;
}

void *heap_realloc(HANDLE handle, DWORD flags, void *p, SIZE_T size)
{
	struct heap *h = heap_get(handle);

	if (!h)
		return NULL;
	if (!p) {
		win_set_error(ERROR_INVALID_PARAMETER);
		return NULL;
	}
	pthread_mutex_lock(&h->lock);
	struct block *b = block_of(h, p);
	if (!b) {
		pthread_mutex_unlock(&h->lock);
		win_set_error(ERROR_INVALID_PARAMETER);
		return NULL;
	}
	SIZE_T old = b->size;
	if (flags & HEAP_REALLOC_IN_PLACE_ONLY) {
		int fits = size <= old;
		if (fits)
			b->size = size;
		pthread_mutex_unlock(&h->lock);
		if (!fits) {
			win_set_error(ERROR_NOT_ENOUGH_MEMORY);
			return NULL;
		}
		return p;
	}

	unlink_block(h, b);
	live_find(p, 1);
	struct block *nb = realloc(b, sizeof(*b) + size);
	struct block *keep = nb ? nb : b;	/* on failure b is untouched */
	if (nb)
		nb->size = size;
	live_insert(keep);
	keep->prev = NULL;
	keep->next = h->blocks;
	if (h->blocks)
		h->blocks->prev = keep;
	h->blocks = keep;
	pthread_mutex_unlock(&h->lock);

	if (!nb) {
		win_set_error(ERROR_NOT_ENOUGH_MEMORY);
		return NULL;
	}
	if ((flags & HEAP_ZERO_MEMORY) && size > old)
		memset((BYTE *)(nb + 1) + old, 0, size - old);
	return nb + 1;
}

WINFN(HANDLE) k32_GetProcessHeap(void)
{
	return heap_process();
}

WINFN(DWORD) k32_GetProcessHeaps(DWORD n, PHANDLE out)
{
	DWORD count = 0;

	heap_process();
	pthread_mutex_lock(&heaps_lock);
	for (struct heap *h = heaps; h; h = h->next) {
		if (count < n)
			out[count] = h;
		count++;
	}
	pthread_mutex_unlock(&heaps_lock);
	return count;
}

WINFN(HANDLE) k32_HeapCreate(DWORD flags, SIZE_T initial, SIZE_T maximum)
{
	struct heap *h = heap_new(flags);

	if (!h)
		win_set_error(ERROR_NOT_ENOUGH_MEMORY);
	return h;
}

WINFN(BOOL) k32_HeapDestroy(HANDLE handle)
{
	struct heap *h = heap_get(handle);

	if (!h || h == process_heap)
		return FALSE;
	pthread_mutex_lock(&heaps_lock);
	for (struct heap **p = &heaps; *p; p = &(*p)->next) {
		if (*p == h) {
			*p = h->next;
			break;
		}
	}
	pthread_mutex_unlock(&heaps_lock);

	pthread_mutex_lock(&h->lock);
	for (struct block *b = h->blocks, *next; b; b = next) {
		next = b->next;
		live_find(b + 1, 1);
		free(b);
	}
	h->blocks = NULL;
	pthread_mutex_unlock(&h->lock);
	h->magic = 0;
	pthread_mutex_destroy(&h->lock);
	free(h);
	return TRUE;
}

WINFN(LPVOID) k32_HeapAlloc(HANDLE h, DWORD flags, SIZE_T size)
{
	return heap_alloc(h, flags, size);
}

WINFN(BOOL) k32_HeapFree(HANDLE h, DWORD flags, LPVOID p)
{
	return heap_free(h, flags, p);
}

WINFN(LPVOID) k32_HeapReAlloc(HANDLE h, DWORD flags, LPVOID p, SIZE_T size)
{
	return heap_realloc(h, flags, p, size);
}

WINFN(SIZE_T) k32_HeapSize(HANDLE h, DWORD flags, LPCVOID p)
{
	return heap_size(h, flags, p);
}

WINFN(BOOL) k32_HeapValidate(HANDLE h, DWORD flags, LPCVOID p)
{
	struct heap *heap = heap_get(h);
	BOOL ok;

	if (!heap)
		return FALSE;
	if (!p)
		return TRUE;
	pthread_mutex_lock(&heap->lock);
	ok = block_listed(heap, p);
	pthread_mutex_unlock(&heap->lock);
	return ok;
}

WINFN(SIZE_T) k32_HeapCompact(HANDLE h, DWORD flags)
{
	return heap_get(h) ? 0x100000 : 0;
}

WINFN(BOOL) k32_HeapLock(HANDLE h)
{
	struct heap *heap = heap_get(h);
	if (heap)
		pthread_mutex_lock(&heap->lock);
	return heap != NULL;
}

WINFN(BOOL) k32_HeapUnlock(HANDLE h)
{
	struct heap *heap = heap_get(h);
	if (heap)
		pthread_mutex_unlock(&heap->lock);
	return heap != NULL;
}

/* Every heap is an ordinary one: no low-fragmentation or other front end. */
WINFN(BOOL) k32_HeapQueryInformation(HANDLE h, int cls, PVOID info, SIZE_T len,
				     PSIZE_T needed)
{
	if (cls != 0 /* HeapCompatibilityInformation */) {
		win_set_error(ERROR_INVALID_PARAMETER);
		return FALSE;
	}
	if (needed)
		*needed = sizeof(ULONG);
	if (len < sizeof(ULONG)) {
		win_set_error(ERROR_INSUFFICIENT_BUFFER);
		return FALSE;
	}
	*(ULONG *)info = 0;
	return TRUE;
}

WINFN(BOOL) k32_HeapSetInformation(HANDLE h, int cls, PVOID info, SIZE_T len)
{
	return TRUE;
}

/* Local and global memory: fixed blocks are process-heap pointers;
 * moveable ones are handles to a descriptor, marked by bit 1. */

#define MOVEABLE_MAGIC 0x4d454d47	/* "GMEM" */

struct moveable {
	DWORD magic;
	void *ptr;
	SIZE_T size;
	UINT flags;
	int locks;
	struct moveable *next;
};

static struct moveable *moveables;
static pthread_mutex_t moveable_lock = PTHREAD_MUTEX_INITIALIZER;

static struct moveable *moveable_of(HGLOBAL h)
{
	if (((uintptr_t)h & 3) != 2)
		return NULL;
	struct moveable *m = (struct moveable *)((BYTE *)h - 2);
	struct moveable *found = NULL;
	pthread_mutex_lock(&moveable_lock);
	for (struct moveable *x = moveables; x && !found; x = x->next)
		if (x == m && m->magic == MOVEABLE_MAGIC)
			found = m;
	pthread_mutex_unlock(&moveable_lock);
	return found;
}

HGLOBAL global_alloc(UINT flags, SIZE_T size)
{
	DWORD hflags = (flags & GMEM_ZEROINIT) ? HEAP_ZERO_MEMORY : 0;

	if (!(flags & GMEM_MOVEABLE))
		return heap_alloc(heap_process(), hflags, size);

	struct moveable *m = calloc(1, sizeof(*m));
	if (!m) {
		win_set_error(ERROR_NOT_ENOUGH_MEMORY);
		return NULL;
	}
	m->magic = MOVEABLE_MAGIC;
	m->size = size;
	m->flags = flags;
	m->ptr = heap_alloc(heap_process(), hflags, size ? size : 1);
	if (!m->ptr) {
		free(m);
		return NULL;
	}
	pthread_mutex_lock(&moveable_lock);
	m->next = moveables;
	moveables = m;
	pthread_mutex_unlock(&moveable_lock);
	return (BYTE *)m + 2;
}

HGLOBAL global_free(HGLOBAL h)
{
	struct moveable *m = moveable_of(h);

	if (!h)
		return NULL;
	if (!m)
		return heap_free(heap_process(), 0, h) ? NULL : h;

	pthread_mutex_lock(&moveable_lock);
	for (struct moveable **p = &moveables; *p; p = &(*p)->next) {
		if (*p == m) {
			*p = m->next;
			break;
		}
	}
	pthread_mutex_unlock(&moveable_lock);
	heap_free(heap_process(), 0, m->ptr);
	m->magic = 0;
	free(m);
	return NULL;
}

LPVOID global_lock(HGLOBAL h)
{
	struct moveable *m = moveable_of(h);

	if (!m)
		return h;
	__atomic_add_fetch(&m->locks, 1, __ATOMIC_SEQ_CST);
	return m->ptr;
}

BOOL global_unlock(HGLOBAL h)
{
	struct moveable *m = moveable_of(h);

	if (!m || m->locks <= 0)
		return FALSE;
	return __atomic_sub_fetch(&m->locks, 1, __ATOMIC_SEQ_CST) > 0;
}

SIZE_T global_size(HGLOBAL h)
{
	struct moveable *m = moveable_of(h);
	SIZE_T s;

	if (m)
		return m->size;
	s = heap_size(heap_process(), 0, h);
	return s == (SIZE_T)-1 ? 0 : s;
}

HGLOBAL global_realloc(HGLOBAL h, SIZE_T size, UINT flags)
{
	struct moveable *m = moveable_of(h);
	DWORD hflags = (flags & GMEM_ZEROINIT) ? HEAP_ZERO_MEMORY : 0;

	if (flags & GMEM_MODIFY) {
		if (m)
			m->flags = (m->flags & ~GMEM_DISCARDABLE) |
				   (flags & GMEM_DISCARDABLE);
		return h;
	}
	if (m) {
		void *p = heap_realloc(heap_process(), hflags, m->ptr,
				       size ? size : 1);
		if (!p)
			return NULL;
		m->ptr = p;
		m->size = size;
		return h;
	}
	/* A fixed block may only move if the caller allows it. */
	if (!(flags & GMEM_MOVEABLE))
		hflags |= HEAP_REALLOC_IN_PLACE_ONLY;
	return heap_realloc(heap_process(), hflags, h, size);
}

static HGLOBAL global_handle(LPCVOID p)
{
	HGLOBAL found = NULL;

	pthread_mutex_lock(&moveable_lock);
	for (struct moveable *m = moveables; m && !found; m = m->next)
		if (m->ptr == p)
			found = (BYTE *)m + 2;
	pthread_mutex_unlock(&moveable_lock);
	return found ? found : (HGLOBAL)p;
}

static UINT global_flags(HGLOBAL h)
{
	struct moveable *m = moveable_of(h);

	if (!m)
		return 0;
	return (m->locks & GMEM_LOCKCOUNT) | (m->flags & GMEM_DISCARDABLE);
}

WINFN(HGLOBAL) k32_GlobalAlloc(UINT flags, SIZE_T size) { return global_alloc(flags, size); }
WINFN(HGLOBAL) k32_GlobalFree(HGLOBAL h) { return global_free(h); }
WINFN(LPVOID) k32_GlobalLock(HGLOBAL h) { return global_lock(h); }
WINFN(BOOL) k32_GlobalUnlock(HGLOBAL h) { return global_unlock(h); }
WINFN(SIZE_T) k32_GlobalSize(HGLOBAL h) { return global_size(h); }
WINFN(HGLOBAL) k32_GlobalReAlloc(HGLOBAL h, SIZE_T size, UINT flags) { return global_realloc(h, size, flags); }
WINFN(HGLOBAL) k32_GlobalHandle(LPCVOID p) { return global_handle(p); }
WINFN(UINT) k32_GlobalFlags(HGLOBAL h) { return global_flags(h); }
WINFN(HLOCAL) k32_LocalAlloc(UINT flags, SIZE_T size) { return global_alloc(flags, size); }
WINFN(HLOCAL) k32_LocalFree(HLOCAL h) { return global_free(h); }
WINFN(LPVOID) k32_LocalLock(HLOCAL h) { return global_lock(h); }
WINFN(BOOL) k32_LocalUnlock(HLOCAL h) { return global_unlock(h); }
WINFN(SIZE_T) k32_LocalSize(HLOCAL h) { return global_size(h); }
WINFN(HLOCAL) k32_LocalReAlloc(HLOCAL h, SIZE_T size, UINT flags) { return global_realloc(h, size, flags); }
WINFN(HLOCAL) k32_LocalHandle(LPCVOID p) { return global_handle(p); }
WINFN(UINT) k32_LocalFlags(HLOCAL h) { return global_flags(h); }

/* Virtual memory. */

struct region {
	BYTE *base;
	SIZE_T size;
	DWORD alloc_protect;
	struct region *next;
};

static struct region *regions;
static pthread_mutex_t region_lock = PTHREAD_MUTEX_INITIALIZER;

static int posix_prot(DWORD protect)
{
	switch (protect & 0xff) {
	case PAGE_READONLY:          return PROT_READ;
	case PAGE_READWRITE:
	case PAGE_WRITECOPY:         return PROT_READ | PROT_WRITE;
	case PAGE_EXECUTE:
	case PAGE_EXECUTE_READ:      return PROT_READ | PROT_EXEC;
	case PAGE_EXECUTE_READWRITE:
	case PAGE_EXECUTE_WRITECOPY: return PROT_READ | PROT_WRITE | PROT_EXEC;
	case PAGE_NOACCESS:          return PROT_NONE;
	default:                     return -1;
	}
}

static DWORD win_protect(const char *perms)
{
	int r = perms[0] == 'r', w = perms[1] == 'w', x = perms[2] == 'x';

	if (x)
		return w ? PAGE_EXECUTE_READWRITE : r ? PAGE_EXECUTE_READ : PAGE_EXECUTE;
	if (w)
		return PAGE_READWRITE;
	return r ? PAGE_READONLY : PAGE_NOACCESS;
}

static struct region *region_at(const void *addr)
{
	struct region *found = NULL;

	pthread_mutex_lock(&region_lock);
	for (struct region *r = regions; r && !found; r = r->next)
		if ((const BYTE *)addr >= r->base &&
		    (const BYTE *)addr < r->base + r->size)
			found = r;
	pthread_mutex_unlock(&region_lock);
	return found;
}

/* Reserves len bytes (a multiple of GRANULE) at a 64K boundary. */
static void *reserve_anywhere(SIZE_T len, int prot)
{
	SIZE_T total = len + GRANULE;
	BYTE *p = mmap(NULL, total, prot, MAP_PRIVATE | MAP_ANONYMOUS |
		       MAP_NORESERVE, -1, 0);
	if (p == MAP_FAILED)
		return NULL;
	uintptr_t start = ((uintptr_t)p + GRANULE - 1) & ~(uintptr_t)(GRANULE - 1);
	if (start > (uintptr_t)p)
		munmap(p, start - (uintptr_t)p);
	SIZE_T tail = (uintptr_t)p + total - (start + len);
	if (tail)
		munmap((void *)(start + len), tail);
	return (void *)start;
}

WINFN(LPVOID) k32_VirtualAlloc(LPVOID addr, SIZE_T size, DWORD type,
			       DWORD protect)
{
	int prot = posix_prot(protect);

	TRACE("VirtualAlloc(%p, 0x%x, 0x%x, 0x%x)", addr, (unsigned)size,
	      type, protect);
	if (!size || prot < 0 || !(type & (MEM_COMMIT | MEM_RESERVE | MEM_RESET))) {
		win_set_error(ERROR_INVALID_PARAMETER);
		return NULL;
	}
	if (type & MEM_RESET)
		return addr;

	if ((type & MEM_RESERVE) || !addr) {
		BYTE *want = addr ? (BYTE *)((uintptr_t)addr & ~(uintptr_t)(GRANULE - 1)) :
			     NULL;
		SIZE_T end = addr ? (SIZE_T)((BYTE *)addr + size - want) : size;
		SIZE_T len = (end + PAGE - 1) & ~(SIZE_T)(PAGE - 1);
		int mprot = (type & MEM_COMMIT) ? prot : PROT_NONE;
		BYTE *base;

		if (want) {
			base = mmap(want, len, mprot, MAP_PRIVATE | MAP_ANONYMOUS |
				    MAP_NORESERVE | MAP_FIXED_NOREPLACE, -1, 0);
			if (base != MAP_FAILED && base != want) {
				munmap(base, len);
				base = MAP_FAILED;
			}
			if (base == MAP_FAILED) {
				win_set_error(ERROR_INVALID_ADDRESS);
				return NULL;
			}
		} else {
			len = (len + GRANULE - 1) & ~(SIZE_T)(GRANULE - 1);
			base = reserve_anywhere(len, mprot);
			if (!base) {
				win_set_error(ERROR_NOT_ENOUGH_MEMORY);
				return NULL;
			}
		}

		struct region *r = malloc(sizeof(*r));
		if (!r) {
			munmap(base, len);
			win_set_error(ERROR_NOT_ENOUGH_MEMORY);
			return NULL;
		}
		r->base = base;
		r->size = len;
		r->alloc_protect = protect;
		pthread_mutex_lock(&region_lock);
		r->next = regions;
		regions = r;
		pthread_mutex_unlock(&region_lock);
		/* A reservation starts at the 64 KiB boundary at or below addr. */
		return base;
	}

	/* Commit pages inside an existing reservation. */
	BYTE *start = (BYTE *)((uintptr_t)addr & ~(uintptr_t)(PAGE - 1));
	BYTE *end = (BYTE *)(((uintptr_t)addr + size + PAGE - 1) &
			     ~(uintptr_t)(PAGE - 1));
	struct region *r = region_at(start);
	if (!r || end > r->base + r->size) {
		win_set_error(ERROR_INVALID_ADDRESS);
		return NULL;
	}
	if (mprotect(start, end - start, prot))
		return WIN_ERRNO(NULL);
	return start;
}

WINFN(BOOL) k32_VirtualFree(LPVOID addr, SIZE_T size, DWORD type)
{
	struct region *r = region_at(addr);

	if (!r) {
		win_set_error(ERROR_INVALID_ADDRESS);
		return FALSE;
	}
	if (type & MEM_RELEASE) {
		if (size || addr != r->base) {
			win_set_error(ERROR_INVALID_PARAMETER);
			return FALSE;
		}
		pthread_mutex_lock(&region_lock);
		for (struct region **p = &regions; *p; p = &(*p)->next) {
			if (*p == r) {
				*p = r->next;
				break;
			}
		}
		pthread_mutex_unlock(&region_lock);
		munmap(r->base, r->size);
		free(r);
		return TRUE;
	}
	if (type & MEM_DECOMMIT) {
		BYTE *start = (BYTE *)((uintptr_t)addr & ~(uintptr_t)(PAGE - 1));
		BYTE *end = size ? (BYTE *)(((uintptr_t)addr + size + PAGE - 1) &
					    ~(uintptr_t)(PAGE - 1)) :
			    r->base + r->size;
		if (end > r->base + r->size)
			end = r->base + r->size;
		madvise(start, end - start, MADV_DONTNEED);
		mprotect(start, end - start, PROT_NONE);
		return TRUE;
	}
	win_set_error(ERROR_INVALID_PARAMETER);
	return FALSE;
}

/* Describes addr from /proc/self/maps, like VirtualQuery. */
static SIZE_T query(LPCVOID addr, PMEMORY_BASIC_INFORMATION mbi)
{
	uintptr_t a = (uintptr_t)addr & ~(uintptr_t)(PAGE - 1);
	FILE *f = fopen("/proc/self/maps", "r");
	char line[512];
	uintptr_t next_start = ADDRESS_TOP;
	int found = 0;

	memset(mbi, 0, sizeof(*mbi));
	if (!f)
		return 0;
	while (fgets(line, sizeof(line), f)) {
		unsigned long lo, hi, ino = 0;
		char perms[5];
		char path[256] = "";
		if (sscanf(line, "%lx-%lx %4s %*x %*s %lu %255s",
			   &lo, &hi, perms, &ino, path) < 3)
			continue;
		if (a >= lo && a < hi) {
			struct region *r = region_at((void *)a);
			struct module *m = mod_from_address((void *)a);
			int none = !strncmp(perms, "---", 3);

			mbi->BaseAddress = (PVOID)a;
			mbi->RegionSize = hi - a;
			mbi->State = none ? MEM_RESERVE : MEM_COMMIT;
			mbi->Protect = none ? 0 : win_protect(perms);
			if (m) {
				mbi->AllocationBase = m->base;
				mbi->AllocationProtect = PAGE_EXECUTE_WRITECOPY;
				mbi->Type = MEM_IMAGE;
			} else if (r) {
				mbi->AllocationBase = r->base;
				mbi->AllocationProtect = r->alloc_protect;
				mbi->Type = MEM_PRIVATE;
			} else {
				mbi->AllocationBase = (PVOID)lo;
				mbi->AllocationProtect = win_protect(perms);
				mbi->Type = ino ? MEM_MAPPED : MEM_PRIVATE;
			}
			found = 1;
			break;
		}
		if (lo > a && lo < next_start)
			next_start = lo;
	}
	fclose(f);

	if (!found) {
		if (a >= ADDRESS_TOP) {
			win_set_error(ERROR_INVALID_PARAMETER);
			return 0;
		}
		mbi->BaseAddress = (PVOID)a;
		mbi->RegionSize = next_start - a;
		mbi->State = MEM_FREE;
		mbi->Protect = PAGE_NOACCESS;
	}
	return sizeof(*mbi);
}

WINFN(SIZE_T) k32_VirtualQuery(LPCVOID addr, PMEMORY_BASIC_INFORMATION mbi,
			       SIZE_T len)
{
	MEMORY_BASIC_INFORMATION info;

	if (len < sizeof(info)) {
		win_set_error(ERROR_BAD_ARGUMENTS);
		return 0;
	}
	if (!query(addr, &info))
		return 0;
	*mbi = info;
	return sizeof(info);
}

WINFN(BOOL) k32_VirtualProtect(LPVOID addr, SIZE_T size, DWORD protect,
			       PDWORD old)
{
	int prot = posix_prot(protect);
	MEMORY_BASIC_INFORMATION info;

	if (prot < 0) {
		win_set_error(ERROR_INVALID_PARAMETER);
		return FALSE;
	}
	if (old)
		*old = query(addr, &info) ? info.Protect : 0;
	BYTE *start = (BYTE *)((uintptr_t)addr & ~(uintptr_t)(PAGE - 1));
	BYTE *end = (BYTE *)(((uintptr_t)addr + (size ? size : 1) + PAGE - 1) &
			     ~(uintptr_t)(PAGE - 1));
	if (mprotect(start, end - start, prot))
		return WIN_ERRNO(FALSE);
	return TRUE;
}

#define CURRENT_PROCESS ((HANDLE)(LONG_PTR)-1)

WINFN(LPVOID) k32_VirtualAllocEx(HANDLE process, LPVOID addr, SIZE_T size,
				 DWORD type, DWORD protect)
{
	if (process != CURRENT_PROCESS) {
		win_set_error(ERROR_ACCESS_DENIED);
		return NULL;
	}
	return k32_VirtualAlloc(addr, size, type, protect);
}

WINFN(BOOL) k32_VirtualFreeEx(HANDLE process, LPVOID addr, SIZE_T size,
			      DWORD type)
{
	if (process != CURRENT_PROCESS) {
		win_set_error(ERROR_ACCESS_DENIED);
		return FALSE;
	}
	return k32_VirtualFree(addr, size, type);
}

WINFN(BOOL) k32_VirtualLock(LPVOID addr, SIZE_T size) { return TRUE; }
WINFN(BOOL) k32_VirtualUnlock(LPVOID addr, SIZE_T size) { return TRUE; }

WINFN(BOOL) k32_FlushInstructionCache(HANDLE process, LPCVOID addr, SIZE_T size)
{
	return TRUE;
}

/* TRUE if [p, p + size) is not all committed memory with the access. */
static BOOL bad_ptr(const void *p, SIZE_T size, int want_write)
{
	const BYTE *a = p, *end = a + size;
	MEMORY_BASIC_INFORMATION info;

	if (!size)
		return FALSE;
	if (!p || end < a)
		return TRUE;
	while (a < end) {
		if (!query(a, &info) || info.State != MEM_COMMIT)
			return TRUE;
		DWORD pr = info.Protect;
		if (pr == PAGE_NOACCESS || pr == PAGE_EXECUTE)
			return TRUE;
		if (want_write && pr != PAGE_READWRITE &&
		    pr != PAGE_EXECUTE_READWRITE)
			return TRUE;
		a = (const BYTE *)info.BaseAddress + info.RegionSize;
	}
	return FALSE;
}

WINFN(BOOL) k32_IsBadReadPtr(LPCVOID p, UINT_PTR size) { return bad_ptr(p, size, 0); }
WINFN(BOOL) k32_IsBadWritePtr(LPVOID p, UINT_PTR size) { return bad_ptr(p, size, 1); }
WINFN(BOOL) k32_IsBadCodePtr(FARPROC p) { return bad_ptr((void *)p, 1, 0); }

WINFN(BOOL) k32_IsBadStringPtrA(LPCSTR s, UINT_PTR max)
{
	for (UINT_PTR i = 0; i < max; i++) {
		if (bad_ptr(s + i, 1, 0))
			return TRUE;
		if (!s[i])
			break;
	}
	return FALSE;
}

WINFN(BOOL) k32_IsBadStringPtrW(LPCWSTR s, UINT_PTR max)
{
	for (UINT_PTR i = 0; i < max; i++) {
		if (bad_ptr(s + i, 2, 0))
			return TRUE;
		if (!s[i])
			break;
	}
	return FALSE;
}

static void memory_status(DWORDLONG *total_phys, DWORDLONG *avail_phys,
			  DWORD *load)
{
	struct sysinfo si;

	if (sysinfo(&si)) {
		*total_phys = 512ull << 20;
		*avail_phys = 256ull << 20;
	} else {
		*total_phys = (DWORDLONG)si.totalram * si.mem_unit;
		*avail_phys = (DWORDLONG)(si.freeram + si.bufferram) * si.mem_unit;
	}
	*load = *total_phys ? (DWORD)(100 - *avail_phys * 100 / *total_phys) : 0;
}

WINFN(void) k32_GlobalMemoryStatus(LPMEMORYSTATUS ms)
{
	DWORDLONG total, avail;
	DWORD load;

	memory_status(&total, &avail, &load);
	/* 32-bit Windows caps these at 2 GB for old programs. */
	ms->dwLength = sizeof(*ms);
	ms->dwMemoryLoad = load;
	ms->dwTotalPhys = total > 0x7fffffff ? 0x7fffffff : total;
	ms->dwAvailPhys = avail > 0x7fffffff ? 0x7fffffff : avail;
	ms->dwTotalPageFile = ms->dwTotalPhys;
	ms->dwAvailPageFile = ms->dwAvailPhys;
	ms->dwTotalVirtual = USER_TOP - 0x10000;
	ms->dwAvailVirtual = 0x60000000;
}

WINFN(BOOL) k32_GlobalMemoryStatusEx(LPMEMORYSTATUSEX ms)
{
	DWORDLONG total, avail;
	DWORD load;

	if (ms->dwLength != sizeof(*ms)) {
		win_set_error(ERROR_INVALID_PARAMETER);
		return FALSE;
	}
	memory_status(&total, &avail, &load);
	ms->dwMemoryLoad = load;
	ms->ullTotalPhys = total;
	ms->ullAvailPhys = avail;
	ms->ullTotalPageFile = total;
	ms->ullAvailPageFile = avail;
	ms->ullTotalVirtual = USER_TOP - 0x10000;
	ms->ullAvailVirtual = 0x60000000;
	ms->ullAvailExtendedVirtual = 0;
	return TRUE;
}

#define E(name) { #name, 0, (void *)k32_##name }

const struct builtin_export k32_memory_exports[] = {
	E(FlushInstructionCache),
	E(GetProcessHeap),
	E(GetProcessHeaps),
	E(GlobalAlloc),
	E(GlobalFlags),
	E(GlobalFree),
	E(GlobalHandle),
	E(GlobalLock),
	E(GlobalMemoryStatus),
	E(GlobalMemoryStatusEx),
	E(GlobalReAlloc),
	E(GlobalSize),
	E(GlobalUnlock),
	E(HeapAlloc),
	E(HeapCompact),
	E(HeapCreate),
	E(HeapDestroy),
	E(HeapFree),
	E(HeapLock),
	E(HeapQueryInformation),
	E(HeapReAlloc),
	E(HeapSetInformation),
	E(HeapSize),
	E(HeapUnlock),
	E(HeapValidate),
	E(IsBadCodePtr),
	E(IsBadReadPtr),
	E(IsBadStringPtrA),
	E(IsBadStringPtrW),
	E(IsBadWritePtr),
	E(LocalAlloc),
	E(LocalFlags),
	E(LocalFree),
	E(LocalHandle),
	E(LocalLock),
	E(LocalReAlloc),
	E(LocalSize),
	E(LocalUnlock),
	E(VirtualAlloc),
	E(VirtualAllocEx),
	E(VirtualFree),
	E(VirtualFreeEx),
	E(VirtualLock),
	E(VirtualProtect),
	E(VirtualQuery),
	E(VirtualUnlock),
	BUILTIN_END
};
