/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * VFW_ZERO_MEMORY=0: malloc, operator new and HeapAlloc stop zeroing what they
 * hand out, but HEAP_ZERO_MEMORY is still honoured. (The default, zeroing,
 * is checked in test_win32_extras. This is its own program because the
 * setting is read once per process.)
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "loader/modules.h"
#include "runtime/runtime.h"
#include "test.h"

static void *get(struct module *m, const char *name)
{
	void *p = mod_get_proc(m, name);
	if (!p) {
		fprintf(stderr, "no %s\n", name);
		exit(1);
	}
	return p;
}

int main(void)
{
	setenv("VFW_ZERO_MEMORY", "0", 1);
	setenv("VFW_ROOT", "/tmp", 1);
	setenv("VFW_REGISTRY", "", 1);
	CHECK_EQ(runtime_enter(), 0);

	struct module *c = mod_load("msvcrt.dll", NULL);
	struct module *k = mod_load("kernel32.dll", NULL);
	CHECK(c && k);
	if (!c || !k)
		return 1;
	void *(*crt_malloc)(size_t) = (void *(*)(size_t))get(c, "malloc");
	void (*crt_free)(void *) = (void (*)(void *))get(c, "free");
	void *(*operator_new)(size_t) = (void *(*)(size_t))get(c, "??2@YAPAXI@Z");
	void *(WINAPI *get_heap)(void) = (void *(WINAPI *)(void))get(k, "GetProcessHeap");
	void *(WINAPI *heap_alloc)(void *, DWORD, SIZE_T) =
		(void *(WINAPI *)(void *, DWORD, SIZE_T))get(k, "HeapAlloc");
	BOOL (WINAPI *heap_free)(void *, DWORD, void *) =
		(BOOL (WINAPI *)(void *, DWORD, void *))get(k, "HeapFree");

	/* A block given back dirty comes back dirty: the allocators don't clear. */
	int dirty = 0;
	for (int round = 0; round < 8; round++) {
		BYTE *p = crt_malloc(300);
		memset(p, 0xaa, 300);
		crt_free(p);
		p = crt_malloc(300);
		for (int i = 0; i < 300; i++)
			dirty |= p[i] != 0;
		crt_free(p);

		p = operator_new(300);
		memset(p, 0xaa, 300);
		crt_free(p);
		p = operator_new(300);
		for (int i = 0; i < 300; i++)
			dirty |= p[i] != 0;
		crt_free(p);

		p = heap_alloc(get_heap(), 0, 300);
		memset(p, 0xaa, 300);
		heap_free(get_heap(), 0, p);
		p = heap_alloc(get_heap(), 0, 300);
		for (int i = 0; i < 300; i++)
			dirty |= p[i] != 0;
		heap_free(get_heap(), 0, p);
	}
	CHECK(dirty);

	/* HEAP_ZERO_MEMORY is a promise, not a default. */
	for (int round = 0; round < 8; round++) {
		BYTE *p = heap_alloc(get_heap(), 0, 300);
		memset(p, 0xaa, 300);
		heap_free(get_heap(), 0, p);
		p = heap_alloc(get_heap(), 8 /* HEAP_ZERO_MEMORY */, 300);
		int clean = 1;
		for (int i = 0; i < 300; i++)
			clean &= p[i] == 0;
		CHECK(clean);
		heap_free(get_heap(), 0, p);
	}
	mod_free(k);
	mod_free(c);
	return test_summary("test_zero_memory_off");
}
