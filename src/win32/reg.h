/*
 * The registry behind advapi32: a tree of keys and values, kept in memory
 * and saved after every change to a .reg file in the format regedit
 * exports ("Windows Registry Editor Version 5.00", written as UTF-8), so a
 * codec's settings persist between runs and can be seeded by hand or from
 * a real Windows export (UTF-16 or REGEDIT4 files are read too).
 *
 * The file is $VFW_REGISTRY, else $VFW_ROOT/registry.reg (default
 * ~/.vfw-harness/registry.reg). VFW_REGISTRY set but empty keeps the
 * registry in memory only.
 *
 * Keys are named by full paths from a hive, e.g.
 * "HKEY_LOCAL_MACHINE\Software\Foo" (UTF-8); names match
 * case-insensitively. Value data is stored as Windows stores it (strings
 * as UTF-16). Everything but reg_reload() must be called with the
 * registry lock held, and a struct reg_key is only valid until it is
 * released: callers find keys by path each time.
 */
#ifndef VFW_WIN32_REG_H
#define VFW_WIN32_REG_H

#include <stddef.h>

#include "win32.h"

struct reg_value {
	char *name;		/* "" for the default value */
	DWORD type;
	BYTE *data;
	DWORD size;
};

struct reg_key {
	char *name;
	struct reg_key *parent;
	struct reg_key **sub;	/* sorted by name, case-insensitively */
	size_t nsub;
	struct reg_value *val;	/* in creation order */
	size_t nval;
	int is_volatile;	/* REG_OPTION_VOLATILE: never saved */
	FILETIME modified;
};

void reg_lock(void);
void reg_unlock(void);

/* The key at path, or NULL. */
struct reg_key *reg_open(const char *path);

/*
 * The key at path, created with any missing parents if need be (*created
 * says whether it was). NULL with a Win32 error in *err on failure.
 */
struct reg_key *reg_create(const char *path, int is_volatile, int *created,
			   LONG *err);

/* Deletes k, which must have no subkeys. Returns a Win32 error code. */
LONG reg_delete(struct reg_key *k);

struct reg_value *reg_get(struct reg_key *k, const char *name);
LONG reg_set(struct reg_key *k, const char *name, DWORD type,
	     const void *data, DWORD size);
LONG reg_unset(struct reg_key *k, const char *name);

/* Saves the tree after a change; a Win32 error code. */
LONG reg_save(void);

/*
 * Forgets the tree and reads the file again, recomputing its path from
 * the environment (for tests).
 */
void reg_reload(void);

/*
 * Imports .reg text (any of the encodings above) into the tree, as regedit
 * would; unparseable lines are skipped with a warning. Returns the number
 * of lines skipped.
 */
int reg_import(const void *text, size_t len);

#endif /* VFW_WIN32_REG_H */
