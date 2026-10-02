/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * Keeping the memory DLL code sees below 2 GB.
 *
 * A 32-bit process on a 64-bit kernel can use all 4 GB, and Linux puts
 * mmap()ed memory, thread stacks and the main stack near the top. Windows
 * gives a program without IMAGE_FILE_LARGE_ADDRESS_AWARE only the low 2 GB,
 * and code written for it may treat pointers as signed or use the top bit.
 *
 * lowmem_reserve() fills every free range from 2 GB up with inaccessible
 * reservations, so that later mappings - the heap's, thread stacks, TEBs,
 * VirtualAlloc, DLL images placed away from their base - all land below
 * 2 GB. It leaves room below the main stack for it to grow. What was
 * mapped before (libc, the main stack) stays where it is, so
 * lowmem_run() runs a command on a thread created afterwards, whose stack
 * is low, with a low copy of its arguments.
 */
#include <errno.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/resource.h>

#include "common/log.h"
#include "common/sanitizer.h"
#include "runtime/lowmem.h"

#define LOW_LIMIT   0x80000000ull
#define ADDR_TOP    0x100000000ull
#define STACK_ROOM  (256ull << 20)	/* main stack growth when unlimited */
#define WORK_STACK  (16u << 20)

/*
 * AddressSanitizer lays out and allocates memory its own way (its
 * allocator doesn't go through the reservation), so the reservation is off
 * in sanitizer builds.
 */

static int reserved;
static uint64_t reserved_bytes;

struct range {
	uint64_t start, end;
};

/* The mappings at or above LOW_LIMIT, sorted, from /proc/self/maps. */
static int read_maps(struct range *r, int max, uint64_t *stack_start)
{
	FILE *f = fopen("/proc/self/maps", "r");
	char line[512];
	int n = 0;

	*stack_start = 0;
	if (!f)
		return -1;
	while (fgets(line, sizeof(line), f)) {
		unsigned long long s, e;
		if (sscanf(line, "%llx-%llx", &s, &e) != 2)
			continue;
		if (strstr(line, "[stack]"))
			*stack_start = s;
		if (e <= LOW_LIMIT || s >= ADDR_TOP)
			continue;
		if (n == max)
			break;
		r[n].start = s < LOW_LIMIT ? LOW_LIMIT : s;
		r[n].end = e > ADDR_TOP ? ADDR_TOP : e;
		n++;
	}
	fclose(f);
	return n;
}

static void reserve(uint64_t start, uint64_t end)
{
	if (end <= start)
		return;
	void *p = mmap((void *)(uintptr_t)start, (size_t)(end - start), PROT_NONE,
		       MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE |
		       MAP_FIXED_NOREPLACE, -1, 0);
	if (p == MAP_FAILED)
		return;
	if ((uintptr_t)p != start) {	/* an old kernel ignoring NOREPLACE */
		munmap(p, (size_t)(end - start));
		return;
	}
	reserved_bytes += end - start;
}

int lowmem_reserve(void)
{
	static struct range maps[1024];
	uint64_t stack_start;
	const char *env = getenv("VFW_LOW_MEMORY");

	if (VFW_SANITIZER_BUILD || sizeof(void *) != 4 || reserved ||
	    (env && !strcmp(env, "0")))
		return 0;
	int n = read_maps(maps, 1024, &stack_start);
	if (n < 0)
		return -1;

	/* Leave the main stack room to grow, as far as its limit allows. */
	uint64_t keep_from = ADDR_TOP;
	if (stack_start) {
		struct rlimit rl;
		uint64_t room = STACK_ROOM;
		if (!getrlimit(RLIMIT_STACK, &rl) && rl.rlim_cur != RLIM_INFINITY &&
		    rl.rlim_cur < STACK_ROOM)
			room = rl.rlim_cur;
		room += 16ull << 20;	/* the kernel's guard gap and some slack */
		keep_from = stack_start > LOW_LIMIT + room ? stack_start - room :
			    LOW_LIMIT;
	}

	uint64_t at = LOW_LIMIT;
	for (int i = 0; i < n; i++) {
		uint64_t gap_end = maps[i].start < keep_from ? maps[i].start : keep_from;
		reserve(at, gap_end);
		if (maps[i].end > at)
			at = maps[i].end;
	}
	reserve(at, keep_from);
	reserved = 1;
	TRACE("reserved %llu MB above 2 GB", (unsigned long long)(reserved_bytes >> 20));
	return 0;
}

int lowmem_active(void)
{
	return reserved && reserved_bytes > 0;
}

struct job {
	int (*fn)(int, char **);
	int argc;
	char **argv;
	int status;
};

static void *run_job(void *p)
{
	struct job *j = p;
	j->status = j->fn(j->argc, j->argv);
	return NULL;
}

int lowmem_run(int (*fn)(int argc, char **argv), int argc, char **argv)
{
	if (lowmem_reserve() || !lowmem_active())
		return fn(argc, argv);

	/* The arguments, copied to the (now low) heap. */
	struct job j = { fn, argc, calloc((size_t)argc + 1, sizeof(char *)), 1 };
	if (!j.argv)
		return fn(argc, argv);
	for (int i = 0; i < argc; i++)
		if (!(j.argv[i] = strdup(argv[i])))
			return fn(argc, argv);

	pthread_attr_t attr;
	pthread_t t;
	pthread_attr_init(&attr);
	pthread_attr_setstacksize(&attr, WORK_STACK);
	int err = pthread_create(&t, &attr, run_job, &j);
	pthread_attr_destroy(&attr);
	if (err) {
		fprintf(stderr, "seance: cannot start the worker thread: %s\n",
			strerror(err));
		return 1;
	}
	pthread_join(t, NULL);
	for (int i = 0; i < argc; i++)
		free(j.argv[i]);
	free(j.argv);
	return j.status;
}
