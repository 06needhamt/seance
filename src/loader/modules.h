/*
 * The module table: loads PE DLLs together with their dependencies, the
 * way LoadLibrary / GetProcAddress / FreeLibrary do.
 *
 * Not thread-safe yet; Phase 2 adds a loader lock with the thread support.
 */
#ifndef VFW_LOADER_MODULES_H
#define VFW_LOADER_MODULES_H

#include "loader/pe.h"

struct module {
	struct module *next;
	char *name;		/* file name as found on disk, e.g. "pe_basic.dll" */
	char *path;
	struct pe_image *img;
	int refcount;
	int attached;		/* DllMain(PROCESS_ATTACH) succeeded */

	/* Modules this one holds a reference on (imports and forwarders). */
	struct module **deps;
	size_t ndeps;
};

/*
 * Loads a DLL, or adds a reference if it is already loaded. name is a path
 * (anything containing '/') or a bare file name, matched case-insensitively
 * and given ".dll" if it has no extension. A bare name is searched for in
 * the importer's directory (when importer is not NULL), then each directory
 * in $VFW_PATH (':'-separated), then the current directory.
 *
 * Set VFW_FORCE_RELOC=1 to load every image away from its preferred base.
 */
struct module *mod_load(const char *name, struct module *importer);

/* A loaded module by file name, without adding a reference. */
struct module *mod_find(const char *name);

/* Exported addresses, following forwarders. NULL (with vfw_error()) if none. */
void *mod_get_proc(struct module *mod, const char *name);
void *mod_get_ordinal(struct module *mod, WORD ordinal);

/* Drops a reference; at zero, detaches and unloads it and its dependencies. */
void mod_free(struct module *mod);

#endif /* VFW_LOADER_MODULES_H */
