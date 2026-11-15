/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * Does what packed codecs (DivX 5.11, VSS H.264) and sloppy ones do to the
 * loader: its DllMain finds kernel32 by scanning down from its own return
 * address for an MZ header, 64 KB at a time, then resolves GetProcAddress by
 * walking kernel32's export directory; and it hands CreateThread a cdecl
 * routine, which leaves its argument on the stack.
 */
#include <stddef.h>

#include "fixture.h"

static HMODULE k32;
static FARPROC (WINAPI *get_proc)(HMODULE, LPCSTR);

/* The export of module base called name, found as a packer finds it. */
static void *walk_exports(const BYTE *base, const char *name)
{
	const IMAGE_DOS_HEADER *dos = (const IMAGE_DOS_HEADER *)base;
	const IMAGE_NT_HEADERS *nt = (const IMAGE_NT_HEADERS *)(base + dos->e_lfanew);
	const IMAGE_DATA_DIRECTORY *dir =
		&nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_EXPORT];

	if (nt->Signature != IMAGE_NT_SIGNATURE || !dir->VirtualAddress)
		return NULL;
	const IMAGE_EXPORT_DIRECTORY *ed =
		(const IMAGE_EXPORT_DIRECTORY *)(base + dir->VirtualAddress);
	const DWORD *names = (const DWORD *)(base + ed->AddressOfNames);
	const WORD *ords = (const WORD *)(base + ed->AddressOfNameOrdinals);
	const DWORD *funcs = (const DWORD *)(base + ed->AddressOfFunctions);
	/* Binary search, as the names are sorted. */
	DWORD lo = 0, hi = ed->NumberOfNames;
	while (lo < hi) {
		DWORD mid = (lo + hi) / 2;
		const char *a = (const char *)base + names[mid], *b = name;
		while (*a && *a == *b) {
			a++;
			b++;
		}
		int c = (BYTE)*a - (BYTE)*b;
		if (!c)
			return (void *)(base + funcs[ords[mid]]);
		if (c < 0)
			lo = mid + 1;
		else
			hi = mid;
	}
	return NULL;
}

/* Down from addr to the nearest 64 KB boundary holding an MZ header. */
static const BYTE *find_mz(const void *addr)
{
	ULONG_PTR p = (ULONG_PTR)addr & ~(ULONG_PTR)0xffff;

	/* A real packer goes on until it faults; this gives up after 1 MB. */
	for (int i = 0; i < 16; i++, p -= 0x10000)
		if (*(const WORD *)p == IMAGE_DOS_SIGNATURE)
			return (const BYTE *)p;
	return NULL;
}

BOOL WINAPI DllMain(HINSTANCE self, DWORD reason, LPVOID reserved)
{
	if (reason == DLL_PROCESS_ATTACH) {
		const BYTE *base = find_mz(__builtin_return_address(0));
		if (base) {
			k32 = (HMODULE)base;
			get_proc = (FARPROC (WINAPI *)(HMODULE, LPCSTR))
				walk_exports(base, "GetProcAddress");
		}
	}
	return TRUE;
}

/* 1 if DllMain found kernel32 and its GetProcAddress by itself. */
EXPORT DWORD pk_found(void)
{
	return k32 && get_proc;
}

/* lstrlenA(s) through what DllMain found. */
EXPORT DWORD pk_strlen(const char *s)
{
	if (!get_proc)
		return 0;
	int (WINAPI *len)(LPCSTR) = (int (WINAPI *)(LPCSTR))get_proc(k32, "lstrlenA");
	return len ? (DWORD)len(s) : 0;
}

/* The module handle DllMain found, to compare with GetModuleHandle's. */
EXPORT HMODULE pk_kernel32(void)
{
	return k32;
}

static DWORD __cdecl cdecl_routine(void *arg)
{
	return (DWORD)(ULONG_PTR)arg * 2;
}

/* Runs a cdecl routine as a thread and returns its exit code. */
EXPORT DWORD pk_cdecl_thread(DWORD value)
{
	HANDLE h = CreateThread(NULL, 0, (LPTHREAD_START_ROUTINE)(void *)cdecl_routine,
				(void *)(ULONG_PTR)value, 0, NULL);
	DWORD code = 0;

	if (!h)
		return 0;
	WaitForSingleObject(h, INFINITE);
	GetExitCodeThread(h, &code);
	CloseHandle(h);
	return code;
}

static DWORD breaks;

/* Steps over the breakpoint, as packers' anti-debugging handlers do. */
static LONG WINAPI skip_break(EXCEPTION_POINTERS *ep)
{
	if (ep->ExceptionRecord->ExceptionCode != EXCEPTION_BREAKPOINT)
		return EXCEPTION_CONTINUE_SEARCH;
#ifdef _WIN64
	if (*(const BYTE *)ep->ContextRecord->Rip != 0xcc)
		return EXCEPTION_CONTINUE_SEARCH;
	ep->ContextRecord->Rip++;
#else
	if (*(const BYTE *)ep->ContextRecord->Eip != 0xcc)
		return EXCEPTION_CONTINUE_SEARCH;
	ep->ContextRecord->Eip++;
#endif
	breaks++;
	return EXCEPTION_CONTINUE_EXECUTION;
}

/* Runs int3 and DebugBreak under a filter that steps over them: 2. */
EXPORT DWORD pk_breakpoints(void)
{
	LPTOP_LEVEL_EXCEPTION_FILTER old = SetUnhandledExceptionFilter(skip_break);
	breaks = 0;
	__asm__ volatile("int3");
	DebugBreak();
	SetUnhandledExceptionFilter(old);
	return breaks;
}

static DWORD steps;

/* Counts single steps, which come with the trap flag already cleared. */
static LONG WINAPI count_step(EXCEPTION_POINTERS *ep)
{
	if (ep->ExceptionRecord->ExceptionCode != EXCEPTION_SINGLE_STEP ||
	    (ep->ContextRecord->EFlags & 0x100))
		return EXCEPTION_CONTINUE_SEARCH;
	steps++;
	return EXCEPTION_CONTINUE_EXECUTION;
}

/* Sets the trap flag over two nops: one single step, so 1. */
EXPORT DWORD pk_single_step(void)
{
	LPTOP_LEVEL_EXCEPTION_FILTER old = SetUnhandledExceptionFilter(count_step);
	steps = 0;
#ifdef _WIN64
	__asm__ volatile("pushfq\n orq $0x100, (%%rsp)\n popfq\n nop\n nop" ::: "cc", "memory");
#else
	__asm__ volatile("pushfl\n orl $0x100, (%%esp)\n popfl\n nop\n nop" ::: "cc", "memory");
#endif
	SetUnhandledExceptionFilter(old);
	return steps;
}

static const BYTE *step_at;

/*
 * Steps over a breakpoint with the trap flag set, as anti-debugging code
 * does; the single step that follows is reported after the instruction
 * past the breakpoint, as on Windows, not inside the resume.
 */
static LONG WINAPI break_then_step(EXCEPTION_POINTERS *ep)
{
	CONTEXT *c = ep->ContextRecord;

	switch (ep->ExceptionRecord->ExceptionCode) {
	case EXCEPTION_BREAKPOINT:
#ifdef _WIN64
		c->Rip++;
#else
		c->Eip++;
#endif
		c->EFlags |= 0x100;
		return EXCEPTION_CONTINUE_EXECUTION;
	case EXCEPTION_SINGLE_STEP:
		step_at = ep->ExceptionRecord->ExceptionAddress;
		return EXCEPTION_CONTINUE_EXECUTION;
	}
	return EXCEPTION_CONTINUE_SEARCH;
}

/* 1 if the step came right after the nop that follows the int3. */
EXPORT DWORD pk_break_step(void)
{
	LPTOP_LEVEL_EXCEPTION_FILTER old = SetUnhandledExceptionFilter(break_then_step);
	step_at = NULL;
	__asm__ volatile("int3\n nop\n movl %%eax, %%eax\n nop" ::: "memory");
	SetUnhandledExceptionFilter(old);
	/* int3 (CC), nop (90), then the step: at the mov (89 C0). */
	return step_at && step_at[-2] == 0xcc && step_at[-1] == 0x90 &&
	       step_at[0] == 0x89 && step_at[1] == 0xc0;
}

static DWORD hw_seen;

/* Checks each hardware fault as Windows reports it, and steps over it. */
static LONG WINAPI check_fault(EXCEPTION_POINTERS *ep)
{
	const EXCEPTION_RECORD *r = ep->ExceptionRecord;
#ifdef _WIN64
	DWORD64 *pc = &ep->ContextRecord->Rip;
#else
	DWORD *pc = &ep->ContextRecord->Eip;
#endif
	const BYTE *at = (const BYTE *)(ULONG_PTR)*pc;

	switch (r->ExceptionCode) {
	case EXCEPTION_ACCESS_VIOLATION:
		/* "int 1" (CD 01): refused, as a read of -1, at itself. */
		if (at[0] != 0xcd || at[1] != 0x01 || r->NumberParameters != 2 ||
		    r->ExceptionInformation[0] != 0 ||
		    r->ExceptionInformation[1] != (ULONG_PTR)-1)
			return EXCEPTION_CONTINUE_SEARCH;
		hw_seen |= 1;
		*pc += 2;
		return EXCEPTION_CONTINUE_EXECUTION;
	case EXCEPTION_SINGLE_STEP:
		/* icebp (F1): past it. */
		if (at[-1] != 0xf1)
			return EXCEPTION_CONTINUE_SEARCH;
		hw_seen |= 2;
		return EXCEPTION_CONTINUE_EXECUTION;
	case EXCEPTION_PRIV_INSTRUCTION:
		/* hlt: at itself. */
		if (at[0] != 0xf4)
			return EXCEPTION_CONTINUE_SEARCH;
		hw_seen |= 4;
		*pc += 1;
		return EXCEPTION_CONTINUE_EXECUTION;
	}
	return EXCEPTION_CONTINUE_SEARCH;
}

/* "int 1", icebp and hlt, as protectors use them: 7 if each came right. */
EXPORT DWORD pk_hw_faults(void)
{
	LPTOP_LEVEL_EXCEPTION_FILTER old = SetUnhandledExceptionFilter(check_fault);
	hw_seen = 0;
	__asm__ volatile(".byte 0xcd, 0x01\n nop" ::: "memory");
	__asm__ volatile(".byte 0xf1\n nop" ::: "memory");
	__asm__ volatile("hlt\n nop" ::: "memory");
	SetUnhandledExceptionFilter(old);
	return hw_seen;
}

/* PEB_LDR_DATA and LDR_DATA_TABLE_ENTRY, as far as a module walk goes. */
struct pk_ustr {
	USHORT len, max;
	WCHAR *buf;
};

struct pk_ldr_entry {
	LIST_ENTRY load, memory, init;
	void *base;
	void *entry;
	ULONG size;
	struct pk_ustr full, name;
};

static int name_is(const struct pk_ustr *u, const char *want)
{
	size_t n = u->len / sizeof(WCHAR), i;

	for (i = 0; i < n && want[i]; i++) {
		WCHAR c = u->buf[i];
		if (c >= 'A' && c <= 'Z')
			c += 'a' - 'A';
		if (c != (WCHAR)want[i])
			return 0;
	}
	return i == n && !want[i];
}

/*
 * Walks PEB->Ldr as shellcode-style API lookup does. Bits: 1 the main
 * image heads the load order, 2 kernel32 is listed at its handle, 4 ntdll
 * heads the initialisation order, 8 this DLL is listed with its size.
 */
EXPORT DWORD pk_peb_ldr(void)
{
	BYTE *peb;
#ifdef _WIN64
	__asm__("movq %%gs:0x60, %0" : "=r"(peb));
	BYTE *ldr = *(BYTE **)(peb + 0x18);
	LIST_ENTRY *load = (LIST_ENTRY *)(ldr + 0x10);
	LIST_ENTRY *init = (LIST_ENTRY *)(ldr + 0x30);
	void *image = *(void **)(peb + 0x10);
#else
	__asm__("movl %%fs:0x30, %0" : "=r"(peb));
	BYTE *ldr = *(BYTE **)(peb + 0x0c);
	LIST_ENTRY *load = (LIST_ENTRY *)(ldr + 0x0c);
	LIST_ENTRY *init = (LIST_ENTRY *)(ldr + 0x1c);
	void *image = *(void **)(peb + 0x08);
#endif
	DWORD r = 0;
	HMODULE self = NULL;

	if (!ldr)
		return 0;
	GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
			   GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
			   (LPCSTR)(void *)pk_peb_ldr, &self);
	if (load->Flink != load &&
	    ((struct pk_ldr_entry *)load->Flink)->base == image)
		r |= 1;
	for (LIST_ENTRY *e = load->Flink; e != load; e = e->Flink) {
		struct pk_ldr_entry *m = (struct pk_ldr_entry *)e;
		if (name_is(&m->name, "kernel32.dll") &&
		    m->base == GetModuleHandleA("kernel32.dll"))
			r |= 2;
		if (m->base == (void *)self && m->size >= 0x1000)
			r |= 8;
	}
	if (init->Flink != init) {
		struct pk_ldr_entry *m = (struct pk_ldr_entry *)
			((BYTE *)init->Flink - offsetof(struct pk_ldr_entry, init));
		if (name_is(&m->name, "ntdll.dll"))
			r |= 4;
	}
	return r;
}
