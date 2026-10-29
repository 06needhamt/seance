/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * Guarded buffers (guard.h). Each takes its own mapping: the buffer, its
 * size rounded up to whole pages, and one PROT_NONE page after. The table
 * of live buffers is read by crash reports in signal context, so entries
 * are published with atomic stores and read without the lock.
 */
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <sys/mman.h>
#include <unistd.h>

#include "runtime/guard.h"

#define MAX_GUARDED 256
#define HEAP_HEADER 32

struct guarded {
	char *base;		/* the mapping; NULL when the entry is free */
	size_t map;		/* its length, the guard page included */
	char *user;		/* what the caller got */
	size_t n;
	const char *what;
};

static struct guarded table[MAX_GUARDED];
static pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;

void *guard_alloc(size_t n, const char *what)
{
	size_t page = (size_t)sysconf(_SC_PAGESIZE);

	if (!n)
		n = 1;
	if (n > SIZE_MAX - 3 * page)
		return NULL;
	/*
	 * Windows' heap puts a header of about 32 bytes before a big block
	 * and commits whole pages, so a buffer of whole pages there has most
	 * of a page after it; avimszh.dll writes into that, as it would
	 * have here with none.
	 */
	size_t body = (n + HEAP_HEADER + page - 1) / page * page;
	char *base = mmap(NULL, body + page, PROT_READ | PROT_WRITE,
			  MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
	if (base == MAP_FAILED)
		return NULL;
	if (mprotect(base + body, page, PROT_NONE)) {
		munmap(base, body + page);
		return NULL;
	}
	char *user = base;

	int slot = -1;
	pthread_mutex_lock(&lock);
	for (int i = 0; i < MAX_GUARDED && slot < 0; i++)
		if (!table[i].base) {
			slot = i;
			table[i].map = body + page;
			table[i].user = user;
			table[i].n = n;
			table[i].what = what ? what : "buffer";
			__atomic_store_n(&table[i].base, base, __ATOMIC_RELEASE);
		}
	pthread_mutex_unlock(&lock);
	if (slot < 0) {
		munmap(base, body + page);
		return NULL;
	}
	return user;
}

void guard_free(void *p)
{
	if (!p)
		return;
	pthread_mutex_lock(&lock);
	for (int i = 0; i < MAX_GUARDED; i++)
		if (table[i].base && table[i].user == p) {
			char *base = table[i].base;
			size_t map = table[i].map;
			__atomic_store_n(&table[i].base, NULL, __ATOMIC_RELEASE);
			munmap(base, map);
			break;
		}
	pthread_mutex_unlock(&lock);
}

int guard_describe(const void *addr, char *out, size_t size)
{
	const char *a = addr;

	for (int i = 0; i < MAX_GUARDED; i++) {
		char *base = __atomic_load_n(&table[i].base, __ATOMIC_ACQUIRE);
		if (!base || a < base || a >= base + table[i].map)
			continue;
		const struct guarded *g = &table[i];
		if (a >= g->user + g->n)
			snprintf(out, size, "%lu bytes past the end of the "
				 "%lu-byte %s", (unsigned long)(a - g->user - g->n),
				 (unsigned long)g->n, g->what);
		else if (a >= g->user)
			snprintf(out, size, "byte %lu of the %lu-byte %s",
				 (unsigned long)(a - g->user),
				 (unsigned long)g->n, g->what);
		else
			continue;
		return 1;
	}
	return 0;
}
