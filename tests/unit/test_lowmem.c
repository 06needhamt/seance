/*
 * lowmem: after lowmem_reserve(), everything allocated from then on - heap
 * blocks large and small, mmap()s, thread stacks, TEBs, VirtualAlloc, DLL
 * images - is below 2 GB, and lowmem_run() gives a command a low stack and
 * low arguments.
 */
#include <pthread.h>
#include <stdint.h>
#include <sys/mman.h>

#include "common/sanitizer.h"
#include "loader/modules.h"
#include "runtime/lowmem.h"
#include "runtime/runtime.h"
#include "runtime/teb.h"
#include "test.h"

#define LOW(p) ((uintptr_t)(p) < 0x80000000u)

static void *thread_stack(void *arg)
{
	int local;
	*(int *)arg = LOW(&local);
	return NULL;
}

static int job_status;

static int job(int argc, char **argv)
{
	int local;
	CHECK(LOW(&local));
	CHECK_EQ(argc, 2);
	CHECK(LOW(argv) && LOW(argv[0]) && LOW(argv[1]));
	CHECK_STR(argv[1], "two");
	CHECK(argv[2] == NULL);
	CHECK_EQ(runtime_enter(), 0);
	CHECK(LOW(teb_current()));
	job_status = 42;
	return 7;
}

int main(void)
{
	const char *off = getenv("VFW_LOW_MEMORY");
	if (off && !strcmp(off, "0")) {
		printf("VFW_LOW_MEMORY=0: nothing to test\n");
		return TEST_SKIP;
	}
	CHECK_EQ(lowmem_reserve(), 0);
	if (VFW_SANITIZER_BUILD) {
		/* The sanitizer's allocator doesn't go through it: stays off. */
		CHECK(!lowmem_active());
		return test_summary("test_lowmem");
	}
	if (!lowmem_active()) {
		printf("no address space above 2 GB to reserve (32-bit kernel?)\n");
		return TEST_SKIP;
	}
	CHECK_EQ(lowmem_reserve(), 0);	/* once is enough */

	/* The heap, small and large (mmap()ed) blocks. */
	void *small = malloc(100), *big = malloc(64 << 20);
	CHECK(small && LOW(small));
	CHECK(big && LOW(big));
	free(small);
	free(big);
	void *m = mmap(NULL, 1 << 20, PROT_READ | PROT_WRITE,
		       MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
	CHECK(m != MAP_FAILED && LOW(m));
	munmap(m, 1 << 20);

	/* Thread stacks. */
	pthread_t t;
	int low_stack = 0;
	CHECK_EQ(pthread_create(&t, NULL, thread_stack, &low_stack), 0);
	pthread_join(t, NULL);
	CHECK(low_stack);

	/* A command run the way main() runs it. */
	char a0[] = "one", a1[] = "two";
	char *argv[] = { a0, a1, NULL };
	CHECK_EQ(lowmem_run(job, 2, argv), 7);
	CHECK_EQ(job_status, 42);

	/* DLL images placed away from their base, and VirtualAlloc. */
	CHECK_EQ(runtime_enter(), 0);
	struct module *k32 = mod_load("kernel32.dll", NULL);
	CHECK(k32 != NULL);
	if (k32) {
		void *(__attribute__((stdcall)) *valloc)(void *, SIZE_T, DWORD, DWORD) =
			mod_get_proc(k32, "VirtualAlloc");
		void *v = valloc ? valloc(NULL, 32 << 20, MEM_COMMIT | MEM_RESERVE,
					  PAGE_READWRITE) : NULL;
		CHECK(v && LOW(v));
		void *(__attribute__((stdcall)) *halloc)(HANDLE, DWORD, SIZE_T) =
			mod_get_proc(k32, "HeapAlloc");
		HANDLE (__attribute__((stdcall)) *heap)(void) =
			mod_get_proc(k32, "GetProcessHeap");
		void *hb = halloc && heap ? halloc(heap(), 0, 8 << 20) : NULL;
		CHECK(hb && LOW(hb));
		mod_free(k32);
	}
	const char *dir = getenv("VFW_FIXTURES");
	char path[4096];
	snprintf(path, sizeof(path), "%s/pe_reloc.dll", dir ? dir : ".");
	if (dir && !access(path, R_OK)) {
		setenv("VFW_FORCE_RELOC", "1", 1);
		struct module *r = mod_load(path, NULL);
		CHECK(r && LOW(r->base) && r->img->relocated);
		mod_free(r);
		unsetenv("VFW_FORCE_RELOC");
	}
	return test_summary("test_lowmem");
}
