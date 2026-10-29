/* SPDX-License-Identifier: GPL-2.0-only */
#include <pthread.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>

#include "loader/modules.h"
#include "runtime/builtin.h"
#include "runtime/crash.h"
#include "runtime/trap.h"

#define THUNK_SIZE  16
#define POOL_SIZE   0x10000

struct trap_record {
	struct trap_record *next;	/* all records, kept for the process */
	char what[128];
	char why[160];		/* "" for an unimplemented built-in function */
};

/*
 * Each thunk is "mov eax, record; jmp vfw_trap_entry". The entry point
 * passes the record and the caller's return address to trap_report().
 */
__asm__(
	".text\n"
	".globl vfw_trap_entry\n"
	"vfw_trap_entry:\n"
	"	movl (%esp), %ecx\n"	/* return address into the caller */
	"	pushl %ecx\n"
	"	pushl %eax\n"
	"	call vfw_trap_report\n"
	"	ud2\n");

void vfw_trap_entry(void);
void vfw_trap_report(const struct trap_record *r, const void *ret)
	__attribute__((noreturn, used, force_align_arg_pointer));

void vfw_trap_report(const struct trap_record *r, const void *ret)
{
	char where[256];

	/* The call instruction is just before the return address. */
	mod_describe_address((const BYTE *)ret - 1, where, sizeof(where));
	if (r->why[0])
		fprintf(stderr, "seance: %s called from %s, but %s\n",
			r->what, where, r->why);
	else
		fprintf(stderr, "seance: unimplemented function %s called "
			"from %s\n", r->what, where);
	fflush(stderr);
	crash_exit(SIGABRT);
}

static pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;
static struct trap_record *records;
static BYTE *pool;
static size_t pool_used = POOL_SIZE;
static BYTE *pools[64];
static int npools;

void *trap_thunk(const char *dll, const char *name, WORD ordinal)
{
	return trap_thunk_why(dll, name, ordinal, NULL);
}

void *trap_thunk_why(const char *dll, const char *name, WORD ordinal,
		     const char *why)
{
	struct trap_record *r = calloc(1, sizeof(*r));
	if (!r)
		return NULL;
	if (why)
		snprintf(r->why, sizeof(r->why), "%s", why);
	if (name)
		snprintf(r->what, sizeof(r->what), "%s!%s", dll, name);
	else
		snprintf(r->what, sizeof(r->what), "%s!#%u", dll, ordinal);

	pthread_mutex_lock(&lock);
	if (pool_used + THUNK_SIZE > POOL_SIZE) {
		if (npools == (int)(sizeof(pools) / sizeof(pools[0])))
			goto fail;
		BYTE *p = mmap(NULL, POOL_SIZE, PROT_READ | PROT_WRITE | PROT_EXEC,
			       MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
		if (p == MAP_FAILED)
			goto fail;
		pools[npools++] = p;
		pool = p;
		pool_used = 0;
	}
	BYTE *t = pool + pool_used;
	pool_used += THUNK_SIZE;
	r->next = records;
	records = r;
	pthread_mutex_unlock(&lock);

	DWORD rec = (DWORD)(uintptr_t)r;
	DWORD rel = (DWORD)((uintptr_t)vfw_trap_entry - (uintptr_t)(t + 10));
	t[0] = 0xb8;			/* mov eax, imm32 */
	memcpy(t + 1, &rec, 4);
	t[5] = 0xe9;			/* jmp rel32 */
	memcpy(t + 6, &rel, 4);
	memset(t + 10, 0xcc, THUNK_SIZE - 10);	/* int3 padding */
	return t;

fail:
	pthread_mutex_unlock(&lock);
	free(r);
	return NULL;
}

int trap_is_thunk(const void *addr)
{
	int found = 0;

	pthread_mutex_lock(&lock);
	for (int i = 0; i < npools && !found; i++)
		found = (const BYTE *)addr >= pools[i] &&
			(const BYTE *)addr < pools[i] + POOL_SIZE;
	pthread_mutex_unlock(&lock);
	return found;
}
