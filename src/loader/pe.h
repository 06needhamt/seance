/*
 * PE32 image parsing and mapping.
 *
 * A struct pe_image starts as a parsed, read-only view of a file (pe_parse),
 * which is enough for inspection. pe_map() then lays it out in memory and
 * relocates it; pe_bind_imports(), pe_tls_init(), pe_protect(),
 * pe_tls_notify() and pe_call_entry() finish loading. modules.c drives that
 * sequence.
 *
 * Every RVA read from the image is bounds-checked, so malformed files fail
 * with an error (see vfw_error()) rather than crashing.
 */
#ifndef VFW_LOADER_PE_H
#define VFW_LOADER_PE_H

#include <stddef.h>

#include "win32.h"

struct pe_image {
	/* The file, owned by the image. */
	BYTE *file;
	size_t file_len;

	IMAGE_NT_HEADERS32 nt;		/* copy; DataDirectory zero-filled */
	IMAGE_SECTION_HEADER *sections;	/* copy of the section table */
	unsigned nsections;

	/* Set by pe_map(); NULL for a parsed-only image. */
	BYTE *base;
	size_t map_len;
	int relocated;			/* not at nt.OptionalHeader.ImageBase */

	/* Set by pe_tls_init(). */
	int has_tls;
	DWORD tls_index;
};

/* Parses (and copies) a PE32 i386 image. Returns NULL on error. */
struct pe_image *pe_parse(const void *buf, size_t len);
struct pe_image *pe_open(const char *path);

/* Unmaps (without DllMain/TLS notifications) and frees the image. */
void pe_free(struct pe_image *img);

/*
 * Returns a pointer to len bytes at rva, or NULL if any of them lie outside
 * the image. For a parsed-only image, bytes past a section's raw data are
 * not in the file and are also rejected.
 */
const void *pe_ptr(const struct pe_image *img, DWORD rva, DWORD len);

/* A NUL-terminated string at rva, or NULL if it runs off the image. */
const char *pe_str(const struct pe_image *img, DWORD rva);

/* The data directory entry, or NULL if absent or empty. */
const IMAGE_DATA_DIRECTORY *pe_dir(const struct pe_image *img, int index);

/* Mapping. */
#define PE_MAP_FORCE_RELOC 0x1	/* never use the preferred base */

int pe_map(struct pe_image *img, unsigned flags);

/*
 * Applies base relocations to an in-memory image of image_len bytes, given
 * the .reloc data. Exposed separately so it can be unit tested.
 */
int pe_relocate(BYTE *image, size_t image_len, const BYTE *relocs,
		size_t relocs_len, DWORD delta);

/* Sets per-section page protections on a mapped image. */
int pe_protect(struct pe_image *img);

/* Imports. */
struct pe_import {
	const char *dll;
	const char *name;	/* NULL when imported by ordinal */
	WORD ordinal;		/* the ordinal, or the name's hint */
	DWORD iat_rva;		/* where the resolved address goes */
	int delayed;		/* from the delay-load directory */
};

typedef int (*pe_import_fn)(void *ctx, const struct pe_import *imp);

/* Calls fn for each import, stopping (and returning -1) if fn does. */
int pe_foreach_import(const struct pe_image *img, pe_import_fn fn, void *ctx);
int pe_foreach_delay_import(const struct pe_image *img, pe_import_fn fn,
			    void *ctx);

/*
 * Resolves every (non-delayed) import of a mapped image through resolve and
 * writes the results into the IAT. resolve returns NULL after calling
 * vfw_set_error() to fail the bind.
 */
typedef void *(*pe_resolve_fn)(void *ctx, const struct pe_import *imp);
int pe_bind_imports(struct pe_image *img, pe_resolve_fn resolve, void *ctx);

/*
 * Binds the delay-load imports eagerly, as if each had been called: every
 * slot of each delay IAT gets resolve's answer, and each descriptor's
 * module handle slot gets handle's (NULL if the DLL isn't there). The DLL's
 * own delay-load helper then never runs. resolve returns NULL to fail the
 * bind, as for pe_bind_imports.
 */
typedef void *(*pe_handle_fn)(void *ctx, const char *dll);
int pe_bind_delay_imports(struct pe_image *img, pe_resolve_fn resolve,
			  pe_handle_fn handle, void *ctx);

/* Exports. */
struct pe_export {
	WORD ordinal;
	const char *name;	/* NULL for an ordinal-only export */
	DWORD rva;
	const char *forwarder;	/* "DLL.func" or "DLL.#n", else NULL */
};

/* The module name recorded in the export directory, or NULL. */
const char *pe_export_dll_name(const struct pe_image *img);

typedef int (*pe_export_fn)(void *ctx, const struct pe_export *exp);
int pe_foreach_export(const struct pe_image *img, pe_export_fn fn, void *ctx);

/* Return 0 and fill *out, or -1 if there is no such export. */
int pe_find_export(const struct pe_image *img, const char *name, WORD hint,
		   struct pe_export *out);
int pe_find_export_ordinal(const struct pe_image *img, WORD ordinal,
			   struct pe_export *out);

/* TLS. */

/* Calls fn with the RVA of each TLS callback. */
typedef int (*pe_tls_fn)(void *ctx, DWORD rva);
int pe_foreach_tls_callback(const struct pe_image *img, pe_tls_fn fn,
			    void *ctx);

/*
 * Assigns the image a TLS index and stores it at AddressOfIndex. Call it
 * before pe_protect(), since the index may live in a read-only section.
 */
int pe_tls_init(struct pe_image *img);
/*
 * Runs the TLS callbacks (after pe_protect(), as they must be executable),
 * e.g. for DLL_PROCESS_ATTACH before DllMain.
 */
void pe_tls_notify(struct pe_image *img, DWORD reason);

/* Calls DllMain(base, reason, NULL). Returns its result; TRUE if none. */
BOOL pe_call_entry(struct pe_image *img, DWORD reason);

/* Resources. */

/*
 * A resource type or name: an integer id, or (like MAKEINTRESOURCE) a
 * pointer to an ASCII string when the value is above 0xffff.
 */
typedef uintptr_t pe_resid;
#define PE_RES_ANY_LANG 0xffffffffu

struct pe_resource {
	DWORD type_id, name_id;		/* 0 if the entry is named */
	const char *type_name;		/* UTF-8, or NULL if an id */
	const char *name_name;
	DWORD lang;
	DWORD data_rva;
	DWORD size;
	DWORD codepage;
	DWORD entry_rva;		/* its IMAGE_RESOURCE_DATA_ENTRY */
};

typedef int (*pe_resource_fn)(void *ctx, const struct pe_resource *res);
int pe_foreach_resource(const struct pe_image *img, pe_resource_fn fn,
			void *ctx);

/*
 * Finds a resource; returns 0 and fills *out, or -1. A type or name of 0
 * matches the first entry. The type_name and name_name of *out are not set.
 */
int pe_find_resource(const struct pe_image *img, pe_resid type,
		     pe_resid name, DWORD lang, struct pe_resource *out);

/*
 * Parses the RT_VERSION resource. fixed may be NULL. Each StringFileInfo
 * entry is passed to fn as UTF-8 (table is e.g. "040904b0").
 */
typedef int (*pe_version_fn)(void *ctx, const char *table, const char *key,
			     const char *value);
int pe_version_info(const struct pe_image *img, VS_FIXEDFILEINFO *fixed,
		    pe_version_fn fn, void *ctx);

#endif /* VFW_LOADER_PE_H */
