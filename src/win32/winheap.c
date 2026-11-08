/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * Memory for the guest's malloc, operator new and HeapAlloc.
 *
 * Zeroed unless VFW_ZERO_MEMORY=0: memory fresh from a Windows heap
 * usually is, and code only ever run on one can depend on it.
 *
 * On x86-64 the memory comes from addresses below 4 GB when it can. A
 * 64-bit process on Windows that isn't high-entropy aware gets its heaps
 * there, and codecs of that time keep pointers in 32-bit variables (CineForm's
 * DriverProc does with the instance it returns from DRV_OPEN: it starts with
 * `mov ebx, ecx`). Blocks come in size classes (four per doubling) from
 * MAP_32BIT chunks, are never returned to the system, and are reused by size;
 * when the low 2 GB can't give more, libc does. VFW_LOWHEAP=0 turns it off.
 */
#include <malloc.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>

#include "win32/internal.h"

int win_zero_memory(void)
{
	static int zero = -1;

	if (zero < 0) {
		const char *env = getenv("VFW_ZERO_MEMORY");
		zero = !(env && !strcmp(env, "0"));
	}
	return zero;
}

#if !defined(__x86_64__)

void *win_alloc(size_t n, size_t size)
{
	return win_zero_memory() ? calloc(n, size) : malloc(n * size);
}

void *win_calloc(size_t n, size_t size) { return calloc(n, size); }
void *win_realloc(void *p, size_t n) { return realloc(p, n); }
void win_free(void *p) { free(p); }
size_t win_usable(void *p) { return malloc_usable_size(p); }

#else

#include <pthread.h>

#define HEADER   16
#define NCLASS   128
#define NCHUNK   64
#define CHUNK_SIZE (64u << 20)

struct block {
	size_t size;		/* with the header: the class's */
	size_t next_free;	/* (reuses the header's second word) */
};

static pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;
static struct block *free_list[NCLASS];
static struct { BYTE *base; size_t size; } chunks[NCHUNK];
static int nchunks;
static BYTE *bump, *bump_end;
static int disabled = -1;

static int class_of(size_t sz, size_t *rounded)
{
	if (sz <= 128) {
		*rounded = (sz + 15) & ~(size_t)15;
		return (int)(*rounded / 16) - 1;
	}
	int e = 63 - __builtin_clzl(sz - 1);
	size_t g = (size_t)1 << (e - 2);

	*rounded = (sz + g - 1) & ~(g - 1);
	e = 63 - __builtin_clzl(*rounded - 1);
	return 8 + (e - 7) * 4 + (int)(((*rounded - 1) >> (e - 2)) & 3);
}

static int owned(const void *p)
{
	for (int i = 0; i < nchunks; i++)
		if ((const BYTE *)p >= chunks[i].base &&
		    (const BYTE *)p < chunks[i].base + chunks[i].size)
			return 1;
	return 0;
}

/* A block of the class from the low chunks, or NULL. Called locked. */
static struct block *low_take(size_t rounded, int cls)
{
	struct block *b = free_list[cls];

	if (b) {
		free_list[cls] = (struct block *)b->next_free;
		return b;
	}
	if ((size_t)(bump_end - bump) < rounded) {
		size_t len = rounded > CHUNK_SIZE ? (rounded + 0xfffff) & ~(size_t)0xfffff
						  : CHUNK_SIZE;
		void *c;

		if (nchunks == NCHUNK || rounded > ((size_t)1 << 30))
			return NULL;
		c = mmap(NULL, len, PROT_READ | PROT_WRITE,
			 MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE | MAP_32BIT,
			 -1, 0);
		if (c == MAP_FAILED)
			return NULL;
		chunks[nchunks].base = c;
		chunks[nchunks++].size = len;
		bump = c;
		bump_end = bump + len;
	}
	b = (struct block *)bump;
	bump += rounded;
	return b;
}

static void *low_alloc(size_t n, int zero)
{
	size_t rounded;
	int cls;
	struct block *b;

	if (disabled < 0) {
		const char *e = getenv("VFW_LOWHEAP");

		disabled = e && !strcmp(e, "0");
	}
	if (disabled || n > ((size_t)1 << 30))
		return NULL;
	cls = class_of(n + HEADER, &rounded);
	pthread_mutex_lock(&lock);
	b = low_take(rounded, cls);
	pthread_mutex_unlock(&lock);
	if (!b)
		return NULL;
	b->size = rounded;
	if (zero)
		memset((BYTE *)b + HEADER, 0, rounded - HEADER);
	return (BYTE *)b + HEADER;
}

static void *alloc_n(size_t n, int zero)
{
	void *p = low_alloc(n, zero);

	if (p)
		return p;
	return zero ? calloc(1, n) : malloc(n);
}

void *win_alloc(size_t n, size_t size)
{
	if (size && n > SIZE_MAX / size)
		return NULL;
	return alloc_n(n * size, win_zero_memory());
}

void *win_calloc(size_t n, size_t size)
{
	if (size && n > SIZE_MAX / size)
		return NULL;
	return alloc_n(n * size, 1);
}

static int is_low(const void *p)
{
	int r;

	pthread_mutex_lock(&lock);
	r = owned(p);
	pthread_mutex_unlock(&lock);
	return r;
}

size_t win_usable(void *p)
{
	if (p && is_low(p))
		return ((struct block *)((BYTE *)p - HEADER))->size - HEADER;
	return malloc_usable_size(p);
}

void win_free(void *p)
{
	if (!p)
		return;
	if (!is_low(p)) {
		free(p);
		return;
	}
	struct block *b = (struct block *)((BYTE *)p - HEADER);
	size_t rounded;
	int cls = class_of(b->size, &rounded);

	pthread_mutex_lock(&lock);
	b->next_free = (size_t)free_list[cls];
	free_list[cls] = b;
	pthread_mutex_unlock(&lock);
}

void *win_realloc(void *p, size_t n)
{
	void *q;
	size_t have;

	if (!p)
		return alloc_n(n, 0);
	if (!n) {
		win_free(p);
		return NULL;
	}
	if (!is_low(p))
		return realloc(p, n);
	have = win_usable(p);
	if (n <= have)
		return p;
	q = alloc_n(n, 0);
	if (!q)
		return NULL;
	memcpy(q, p, have);
	win_free(p);
	return q;
}

#endif
