#include <fnmatch.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/mman.h>

#include "runtime/builtin.h"

extern const struct builtin_dll builtin_kernel32;

const struct builtin_dll *const builtin_dlls[] = {
	&builtin_kernel32,
	NULL
};

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

static int compare_export(const void *key, const void *elem)
{
	const struct builtin_export *e = elem;
	return strcmp(key, e->name ? e->name : "");
}

void *builtin_export(const struct builtin_dll *dll, const char *name)
{
	const struct builtin_export *e =
		bsearch(name, dll->exports, dll->nexports, sizeof(*e),
			compare_export);
	return e ? e->addr : NULL;
}

void *builtin_export_ordinal(const struct builtin_dll *dll, WORD ordinal)
{
	for (size_t i = 0; i < dll->nexports && ordinal; i++)
		if (dll->exports[i].ordinal == ordinal)
			return dll->exports[i].addr;
	return NULL;
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
