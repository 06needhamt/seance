/*
 * The module table: loads PE DLLs together with their dependencies, the
 * way LoadLibrary / GetProcAddress / FreeLibrary do. Names that match a
 * built-in DLL (kernel32, msvcrt, ...) are served by the harness itself.
 *
 * All functions take the loader lock, which is recursive, so DllMain and
 * the stubs it calls may load and free modules too.
 */
#ifndef VFW_LOADER_MODULES_H
#define VFW_LOADER_MODULES_H

#include "loader/pe.h"
#include "runtime/builtin.h"

struct module {
	struct module *next;
	char *name;		/* file name, e.g. "pe_basic.dll" */
	char *path;		/* NULL for a built-in module */
	BYTE *base;		/* the HMODULE */
	struct pe_image *img;	/* NULL for a built-in module */
	const struct builtin_dll *builtin;	/* NULL for a PE module */
	int refcount;
	int attached;		/* DllMain(PROCESS_ATTACH) succeeded */
	int no_thread_calls;	/* DisableThreadLibraryCalls */
	int pinned;		/* never unloaded */

	/* Modules this one holds a reference on (imports and forwarders). */
	struct module **deps;
	size_t ndeps;
};

/*
 * Loads a DLL, or adds a reference if it is already loaded. name is a path
 * (anything containing '/') or a bare file name, matched case-insensitively
 * and given ".dll" if it has no extension. A bare name is first matched
 * against the built-in DLLs, then searched for in the importer's directory
 * (when importer is not NULL), each directory in $VFW_PATH (':'-separated),
 * and the current directory.
 *
 * Set VFW_FORCE_RELOC=1 to load every image away from its preferred base.
 */
struct module *mod_load(const char *name, struct module *importer);

/* A loaded module by file name, without adding a reference. */
struct module *mod_find(const char *name);

/* The loaded module whose HMODULE is base, or that contains addr. */
struct module *mod_from_base(const void *base);
struct module *mod_from_address(const void *addr);

/*
 * The stand-in for the executable (GetModuleHandle(NULL)): a module with
 * no exports, created on first use.
 */
struct module *mod_main(void);

/*
 * Exported addresses, following forwarders; GetProcAddress semantics, so
 * an unimplemented built-in function is NULL (with vfw_error()).
 */
void *mod_get_proc(struct module *mod, const char *name);
void *mod_get_ordinal(struct module *mod, WORD ordinal);

/* Adds a reference (GetModuleHandleEx without UNCHANGED_REFCOUNT). */
void mod_addref(struct module *mod);

/* Keeps mod loaded for good (GET_MODULE_HANDLE_EX_FLAG_PIN). */
void mod_pin(struct module *mod);

/* Drops a reference; at zero, detaches and unloads it and its dependencies. */
void mod_free(struct module *mod);

/*
 * Tells every attached DLL that a thread is starting or ending
 * (DLL_THREAD_ATTACH / DLL_THREAD_DETACH): TLS callbacks, then DllMain
 * unless the DLL called DisableThreadLibraryCalls. Attach goes in load
 * order, detach in reverse. Called on the thread concerned, with the
 * loader lock held, as Windows does.
 */
void mod_thread_notify(DWORD reason);

/* Formats addr as "module+0xrva", or just the address if not in a module. */
void mod_describe_address(const void *addr, char *out, size_t outsz);

/*
 * The same without waiting for the loader lock (for crash reports): 1 with
 * "module+0xrva" in out, or 0 if addr is in no module or the lock is busy.
 */
int mod_describe_address_try(const void *addr, char *out, size_t outsz);

#endif /* VFW_LOADER_MODULES_H */
