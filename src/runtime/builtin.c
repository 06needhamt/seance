/* SPDX-License-Identifier: GPL-2.0-only */
#include <fnmatch.h>
#include <pthread.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <stddef.h>
#include <stdint.h>
#include <sys/mman.h>

#include "runtime/builtin.h"
#include "runtime/gate.h"

extern const struct builtin_dll builtin_kernel32;
extern const struct builtin_dll builtin_kernelbase;
extern const struct builtin_dll builtin_ntdll;
extern const struct builtin_dll builtin_winmm;
extern const struct builtin_dll builtin_msvfw32;
extern const struct builtin_dll builtin_user32, builtin_gdi32, builtin_advapi32,
	builtin_version, builtin_ole32, builtin_oleaut32, builtin_oleacc,
	builtin_shell32,
	builtin_comctl32, builtin_comdlg32, builtin_winspool, builtin_setupapi,
	builtin_avifil32, builtin_quartz, builtin_msdmo, builtin_msacm32,
	builtin_msvcrt, builtin_crtdll, builtin_psapi, builtin_oledlg,
	builtin_dbghelp, builtin_netapi32;
extern const struct builtin_dll builtin_shlwapi;
extern const struct builtin_dll builtin_bcrypt, builtin_crypt32, builtin_d3d9,
	builtin_dxva2, builtin_d3d11, builtin_dxgi, builtin_ncrypt, builtin_ws2_32,
	builtin_wsock32, builtin_rpcrt4;

const struct builtin_dll *const builtin_dlls[] = {
	&builtin_kernel32,
	&builtin_kernelbase,
	&builtin_ntdll,
	&builtin_winmm,
	&builtin_msvfw32,
	&builtin_user32,
	&builtin_gdi32,
	&builtin_advapi32,
	&builtin_version,
	&builtin_ole32,
	&builtin_oleaut32,
	&builtin_oleacc,
	&builtin_shell32,
	&builtin_shlwapi,
	&builtin_bcrypt,
	&builtin_crypt32,
	&builtin_d3d9,
	&builtin_dxva2,
	&builtin_d3d11,
	&builtin_dxgi,
	&builtin_ncrypt,
	&builtin_ws2_32,
	&builtin_wsock32,
	&builtin_comctl32,
	&builtin_comdlg32,
	&builtin_winspool,
	&builtin_setupapi,
	&builtin_avifil32,
	&builtin_quartz,
	&builtin_msdmo,
	&builtin_msacm32,
	&builtin_msvcrt,
	&builtin_crtdll,
	&builtin_psapi,
	&builtin_oledlg,
	&builtin_dbghelp,
	&builtin_netapi32,
	&builtin_rpcrt4,
	NULL
};

#define MAX_DLLS 96

/* Sorted views of each DLL's named exports, built on first use. */
struct export_index {
	const struct builtin_dll *dll;
	const struct builtin_export **sorted;
	size_t n;
};

static struct export_index indices[MAX_DLLS];
static pthread_mutex_t index_lock = PTHREAD_MUTEX_INITIALIZER;

const struct builtin_dll *builtin_find(const char *name)
{
	for (const struct builtin_dll *const *d = builtin_dlls; *d; d++) {
		if (!strcasecmp((*d)->name, name))
			return *d;
		for (const char *const *a = (*d)->aliases; a && *a; a++)
			if (!fnmatch(*a, name, FNM_CASEFOLD))
				return *d;
	}
	return NULL;
}

static int compare_entries(const void *a, const void *b)
{
	const struct builtin_export *x = *(const struct builtin_export *const *)a;
	const struct builtin_export *y = *(const struct builtin_export *const *)b;
	return strcmp(x->name, y->name);
}

static const struct export_index *get_index(const struct builtin_dll *dll)
{
	struct export_index *ix = NULL;

	pthread_mutex_lock(&index_lock);
	for (int i = 0; i < MAX_DLLS && !ix; i++) {
		if (indices[i].dll == dll) {
			ix = &indices[i];
		} else if (!indices[i].dll) {
			ix = &indices[i];
			size_t n = 0;
			for (const struct builtin_export *const *t = dll->tables; *t; t++)
				for (const struct builtin_export *e = *t; e->addr; e++)
					n += e->name != NULL;
			ix->sorted = malloc((n ? n : 1) * sizeof(*ix->sorted));
			if (!ix->sorted) {
				ix = NULL;
				break;
			}
			n = 0;
			for (const struct builtin_export *const *t = dll->tables; *t; t++)
				for (const struct builtin_export *e = *t; e->addr; e++)
					if (e->name)
						ix->sorted[n++] = e;
			qsort(ix->sorted, n, sizeof(*ix->sorted), compare_entries);
			ix->n = n;
			ix->dll = dll;
		}
	}
	pthread_mutex_unlock(&index_lock);
	return ix;
}

static int compare_key(const void *key, const void *elem)
{
	const struct builtin_export *e = *(const struct builtin_export *const *)elem;
	return strcmp(key, e->name);
}

void *builtin_export(const struct builtin_dll *dll, const char *name)
{
	const struct export_index *ix = get_index(dll);
	const struct builtin_export *const *e;

	if (!ix)
		return NULL;
	e = bsearch(name, ix->sorted, ix->n, sizeof(*ix->sorted), compare_key);
	return e ? (*e)->addr : NULL;
}

void *builtin_export_ordinal(const struct builtin_dll *dll, WORD ordinal)
{
	if (!ordinal)
		return NULL;
	for (const struct builtin_export *const *t = dll->tables; *t; t++)
		for (const struct builtin_export *e = *t; e->addr; e++)
			if (e->ordinal == ordinal)
				return e->addr;
	return NULL;
}

size_t builtin_count(const struct builtin_dll *dll)
{
	const struct export_index *ix = get_index(dll);
	return ix ? ix->n : 0;
}

const struct builtin_export *builtin_entry(const struct builtin_dll *dll,
					   size_t i)
{
	const struct export_index *ix = get_index(dll);
	return ix && i < ix->n ? ix->sorted[i] : NULL;
}

int builtin_shadowable(const struct builtin_dll *b, const char *name)
{
	if (b->fallback == BUILTIN_FALLBACK_ALIASES)
		return strcasecmp(name, b->name) != 0;
	return b->fallback != 0;
}

/*
 * A module header for a built-in DLL, at a 64 KB boundary as Windows maps
 * DLLs, so GetModuleHandle has something to return and code that walks a
 * PE header (packers, resource lookups) finds one. For a DLL other than the
 * CRT (whose data exports can't be reached through a jump), it also gets an
 * export directory whose entries are jumps to the harness's functions, in
 * the image: a packer that finds kernel32's base by scanning down from a
 * return address resolves GetProcAddress and LoadLibraryA by walking it.
 * kernel32's image also carries the call gate (runtime/gate.h), so the
 * routines the gate calls return into kernel32.
 */
#define STUB_SIZE 16
#define IMAGE_ALIGN 0x10000

static size_t round_up(size_t n, size_t to)
{
	return (n + to - 1) & ~(to - 1);
}

/* Maps size bytes at a multiple of IMAGE_ALIGN, or NULL. */
static BYTE *map_aligned(size_t size)
{
	size_t span = size + IMAGE_ALIGN;
	BYTE *p = mmap(NULL, span, PROT_READ | PROT_WRITE,
		       MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
	if (p == MAP_FAILED)
		return NULL;
	BYTE *base = (BYTE *)round_up((uintptr_t)p, IMAGE_ALIGN);
	if (base > p)
		munmap(p, (size_t)(base - p));
	size_t tail = (size_t)(p + span - (base + size));
	if (tail)
		munmap(base + size, tail);
	return base;
}

static void put_stub(BYTE *t, const void *target)
{
#if defined(__x86_64__)
	uint64_t to = (uintptr_t)target;
	t[0] = 0xff;			/* jmp [rip + 0] */
	t[1] = 0x25;
	memset(t + 2, 0, 4);
	memcpy(t + 6, &to, 8);
	memset(t + 14, 0xcc, STUB_SIZE - 14);
#else
	DWORD rel = (DWORD)((uintptr_t)target - (uintptr_t)(t + 5));
	t[0] = 0xe9;			/* jmp rel32 */
	memcpy(t + 1, &rel, 4);
	memset(t + 5, 0xcc, STUB_SIZE - 5);
#endif
}

BYTE *builtin_fake_image(const struct builtin_dll *dll)
{
	int is_k32 = dll == &builtin_kernel32;
	size_t n = dll && !dll->fallback ? builtin_count(dll) : 0;
	const char *dllname = dll ? dll->name : "seance.exe";

	/* .edata: directory, functions, names, ordinals, then the strings. */
	size_t strings = strlen(dllname) + 1;
	for (size_t i = 0; i < n; i++)
		strings += strlen(builtin_entry(dll, i)->name) + 1;
	size_t edata_size = n ? sizeof(IMAGE_EXPORT_DIRECTORY) +
			    n * (4 + 4 + 2) + strings : 0;
	size_t gate = is_k32 ? round_up(gate_code_size(), STUB_SIZE) : 0;
	size_t text_size = gate + n * STUB_SIZE;
	DWORD edata_rva = 0x1000;
	DWORD text_rva = (DWORD)round_up(edata_rva + edata_size, 0x1000);
	size_t size = text_size ? round_up(text_rva + text_size, 0x1000) :
		      edata_rva + round_up(edata_size, 0x1000);

	BYTE *base = map_aligned(size);
	if (!base)
		return NULL;

	IMAGE_DOS_HEADER *dos = (IMAGE_DOS_HEADER *)base;
#if defined(__x86_64__)
	IMAGE_NT_HEADERS64 *nt = (IMAGE_NT_HEADERS64 *)(base + 0x40);
#else
	IMAGE_NT_HEADERS32 *nt = (IMAGE_NT_HEADERS32 *)(base + 0x40);
#endif
	dos->e_magic = IMAGE_DOS_SIGNATURE;
	dos->e_lfanew = 0x40;
	nt->Signature = IMAGE_NT_SIGNATURE;
#if defined(__x86_64__)
	nt->FileHeader.Machine = IMAGE_FILE_MACHINE_AMD64;
	nt->FileHeader.Characteristics = IMAGE_FILE_EXECUTABLE_IMAGE |
					 IMAGE_FILE_LARGE_ADDRESS_AWARE;
	nt->OptionalHeader.Magic = IMAGE_NT_OPTIONAL_HDR64_MAGIC;
	nt->OptionalHeader.ImageBase = (uintptr_t)base;
#else
	nt->FileHeader.Machine = IMAGE_FILE_MACHINE_I386;
	nt->FileHeader.Characteristics = IMAGE_FILE_EXECUTABLE_IMAGE |
					 IMAGE_FILE_32BIT_MACHINE;
	nt->OptionalHeader.Magic = IMAGE_NT_OPTIONAL_HDR32_MAGIC;
	nt->OptionalHeader.ImageBase = (DWORD)(uintptr_t)base;
#endif
	if (dll)
		nt->FileHeader.Characteristics |= IMAGE_FILE_DLL;
	nt->FileHeader.SizeOfOptionalHeader = sizeof(nt->OptionalHeader);
	nt->OptionalHeader.SectionAlignment = 0x1000;
	nt->OptionalHeader.FileAlignment = 0x200;
	nt->OptionalHeader.MajorOperatingSystemVersion = 5;
	nt->OptionalHeader.MinorOperatingSystemVersion = 1;
	nt->OptionalHeader.MajorSubsystemVersion = 5;
	nt->OptionalHeader.MinorSubsystemVersion = 1;
	nt->OptionalHeader.SizeOfImage = (DWORD)size;
	nt->OptionalHeader.SizeOfHeaders = 0x1000;
	nt->OptionalHeader.Subsystem = IMAGE_SUBSYSTEM_WINDOWS_GUI;
	nt->OptionalHeader.NumberOfRvaAndSizes = IMAGE_NUMBEROF_DIRECTORY_ENTRIES;

	IMAGE_SECTION_HEADER *sec = (IMAGE_SECTION_HEADER *)
		((BYTE *)&nt->OptionalHeader + sizeof(nt->OptionalHeader));
	WORD nsec = 0;
	if (text_size) {
		memcpy(sec[nsec].Name, ".text", 5);
		sec[nsec].VirtualAddress = text_rva;
		sec[nsec].Misc.VirtualSize = (DWORD)text_size;
		sec[nsec].Characteristics = IMAGE_SCN_CNT_CODE |
			IMAGE_SCN_MEM_EXECUTE | IMAGE_SCN_MEM_READ;
		nt->OptionalHeader.BaseOfCode = text_rva;
		nt->OptionalHeader.SizeOfCode = (DWORD)text_size;
		nsec++;
	}
	if (edata_size) {
		memcpy(sec[nsec].Name, ".edata", 6);
		sec[nsec].VirtualAddress = edata_rva;
		sec[nsec].Misc.VirtualSize = (DWORD)edata_size;
		sec[nsec].Characteristics = IMAGE_SCN_CNT_INITIALIZED_DATA |
					    IMAGE_SCN_MEM_READ;
		nsec++;
	}
	nt->FileHeader.NumberOfSections = nsec;

	if (n) {
		IMAGE_EXPORT_DIRECTORY *ed = (IMAGE_EXPORT_DIRECTORY *)(base + edata_rva);
		DWORD *funcs = (DWORD *)(ed + 1);
		DWORD *names = funcs + n;
		WORD *ords = (WORD *)(names + n);
		char *str = (char *)(ords + n);
		DWORD stubs = text_rva + (DWORD)gate;

		ed->Base = 1;
		ed->NumberOfFunctions = (DWORD)n;
		ed->NumberOfNames = (DWORD)n;
		ed->AddressOfFunctions = (DWORD)((BYTE *)funcs - base);
		ed->AddressOfNames = (DWORD)((BYTE *)names - base);
		ed->AddressOfNameOrdinals = (DWORD)((BYTE *)ords - base);
		ed->Name = (DWORD)((BYTE *)str - base);
		strcpy(str, dllname);
		str += strlen(dllname) + 1;
		for (size_t i = 0; i < n; i++) {
			const struct builtin_export *e = builtin_entry(dll, i);
			funcs[i] = stubs + (DWORD)(i * STUB_SIZE);
			names[i] = (DWORD)((BYTE *)str - base);
			ords[i] = (WORD)i;
			strcpy(str, e->name);
			str += strlen(e->name) + 1;
			put_stub(base + funcs[i], e->addr);
		}
		nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_EXPORT].VirtualAddress = edata_rva;
		nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_EXPORT].Size = (DWORD)edata_size;
	}
	if (gate) {
		memcpy(base + text_rva, gate_code(), gate_code_size());
		memset(base + text_rva + gate_code_size(), 0xcc,
		       gate - gate_code_size());
	}

	mprotect(base, text_size ? text_rva : size, PROT_READ);
	if (text_size)
		mprotect(base + text_rva, size - text_rva, PROT_READ | PROT_EXEC);
	/* Published only once executable; kernel32's image is never unmapped. */
	if (gate && !gate_copy())
		gate_set_copy(base + text_rva);
	return base;
}

void builtin_free_image(BYTE *base)
{
	if (!base)
		return;
	const IMAGE_DOS_HEADER *dos = (const IMAGE_DOS_HEADER *)base;
	DWORD size;
	/* At the same offset in both optional header layouts. */
	memcpy(&size, base + dos->e_lfanew + 4 + sizeof(IMAGE_FILE_HEADER) +
		      offsetof(IMAGE_OPTIONAL_HEADER32, SizeOfImage), sizeof(size));
	const BYTE *gate = gate_copy();
	if (gate >= base && gate < base + size)
		return;		/* the call gate lives here */
	munmap(base, size);
}
