#include <fnmatch.h>
#include <pthread.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/mman.h>

#include "runtime/builtin.h"

extern const struct builtin_dll builtin_kernel32;
extern const struct builtin_dll builtin_ntdll;
extern const struct builtin_dll builtin_winmm;
extern const struct builtin_dll builtin_msvfw32;
extern const struct builtin_dll builtin_user32, builtin_gdi32, builtin_advapi32,
	builtin_version, builtin_ole32, builtin_oleaut32, builtin_shell32,
	builtin_comctl32, builtin_comdlg32, builtin_msvcrt;

const struct builtin_dll *const builtin_dlls[] = {
	&builtin_kernel32,
	&builtin_ntdll,
	&builtin_winmm,
	&builtin_msvfw32,
	&builtin_user32,
	&builtin_gdi32,
	&builtin_advapi32,
	&builtin_version,
	&builtin_ole32,
	/* Placeholders (fallback only, no exports yet). */
	&builtin_oleaut32,
	&builtin_shell32,
	&builtin_comctl32,
	&builtin_comdlg32,
	&builtin_msvcrt,
	NULL
};

#define MAX_DLLS 32

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

BYTE *builtin_fake_image(void)
{
	BYTE *base = mmap(NULL, 0x1000, PROT_READ | PROT_WRITE,
			  MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
	if (base == MAP_FAILED)
		return NULL;

	IMAGE_DOS_HEADER *dos = (IMAGE_DOS_HEADER *)base;
	IMAGE_NT_HEADERS32 *nt = (IMAGE_NT_HEADERS32 *)(base + 0x40);
	dos->e_magic = IMAGE_DOS_SIGNATURE;
	dos->e_lfanew = 0x40;
	nt->Signature = IMAGE_NT_SIGNATURE;
	nt->FileHeader.Machine = IMAGE_FILE_MACHINE_I386;
	nt->FileHeader.SizeOfOptionalHeader = sizeof(nt->OptionalHeader);
	nt->FileHeader.Characteristics = IMAGE_FILE_EXECUTABLE_IMAGE |
					 IMAGE_FILE_32BIT_MACHINE | IMAGE_FILE_DLL;
	nt->OptionalHeader.Magic = IMAGE_NT_OPTIONAL_HDR32_MAGIC;
	nt->OptionalHeader.ImageBase = (DWORD)(uintptr_t)base;
	nt->OptionalHeader.SectionAlignment = 0x1000;
	nt->OptionalHeader.FileAlignment = 0x200;
	nt->OptionalHeader.MajorOperatingSystemVersion = 5;
	nt->OptionalHeader.MinorOperatingSystemVersion = 1;
	nt->OptionalHeader.MajorSubsystemVersion = 5;
	nt->OptionalHeader.MinorSubsystemVersion = 1;
	nt->OptionalHeader.SizeOfImage = 0x1000;
	nt->OptionalHeader.SizeOfHeaders = 0x1000;
	nt->OptionalHeader.Subsystem = IMAGE_SUBSYSTEM_WINDOWS_GUI;
	nt->OptionalHeader.NumberOfRvaAndSizes = IMAGE_NUMBEROF_DIRECTORY_ENTRIES;
	mprotect(base, 0x1000, PROT_READ);
	return base;
}

void builtin_free_image(BYTE *base)
{
	if (base)
		munmap(base, 0x1000);
}
