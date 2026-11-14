/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * Does what packed codecs (DivX 5.11, VSS H.264) and sloppy ones do to the
 * loader: its DllMain finds kernel32 by scanning down from its own return
 * address for an MZ header, 64 KB at a time, then resolves GetProcAddress by
 * walking kernel32's export directory; and it hands CreateThread a cdecl
 * routine, which leaves its argument on the stack.
 */
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
