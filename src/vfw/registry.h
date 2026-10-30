/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * The installed codecs: which DLL serves each type.handler pair, the way
 * the [drivers32] section of system.ini (or the Drivers32 registry key)
 * does on Windows.
 *
 * Entries come from $VFW_INI, by default $VFW_ROOT/vfw.ini (next to
 * drive_c), in lines such as
 *
 *   [drivers32]
 *   vidc.TST0=/path/to/testcodec.dll
 *   vidc.MSVC=C:\windows\system32\msvidc32.dll
 *   msacm.imaadpcm=imaadp32.acm
 *
 * and from vfw_register() at run time. Relative paths are taken from the
 * ini file's directory; Windows paths go through the drive mapping.
 */
#ifndef VFW_VFW_REGISTRY_H
#define VFW_VFW_REGISTRY_H

#include "win32.h"

struct vfw_codec {
	FOURCC type;		/* 'vidc' */
	FOURCC handler;		/* as registered, e.g. 'TST0' */
	char *path;		/* Unix path or bare DLL name; NULL for proc */
	DRIVERPROC proc;	/* ICInstall(ICINSTALL_FUNCTION) */
	int from_ini;
};

/* The path of the ini file (which may not exist). */
const char *vfw_ini_path(void);

/* Re-reads the ini file (after VFW_INI / VFW_ROOT change, for tests). */
void vfw_registry_reload(void);

/*
 * The codec registered for type.handler (compared case-insensitively), or
 * NULL. The result stays valid until the next reload.
 */
const struct vfw_codec *vfw_find(FOURCC type, FOURCC handler);

/* The i-th codec of a type (any type when type is 0), or NULL. */
const struct vfw_codec *vfw_enum(FOURCC type, unsigned i);

/* The first codec whose DLL is path (by file identity or name), or NULL. */
const struct vfw_codec *vfw_find_path(const char *path);

/*
 * handlers.c: up to max FOURCCs the DLL at path may want to be opened
 * under, read from its code, data and version resource, the likelier
 * first. where, if not NULL, gets each one's VFW_FOURCC_FROM_*.
 */
enum {
	VFW_FOURCC_FROM_CMP,		/* compared with in code */
	VFW_FOURCC_FROM_TABLE,		/* a word alone in data */
	VFW_FOURCC_FROM_PAIR,		/* an upper and lower case pair */
	VFW_FOURCC_FROM_VERSION,	/* a word of the version resource */
	VFW_FOURCC_FROM_STRING,		/* a string or its first word */
	VFW_FOURCC_FROM_MOV,		/* stored or pushed in code */
};
size_t vfw_dll_handlers(const char *path, FOURCC *out, unsigned char *where,
			size_t max);
/* What VFW_FOURCC_FROM_* says, e.g. "compared with in its code". */
const char *vfw_fourcc_from_name(unsigned from);

/*
 * Adds type.handler=path to the ini file's [drivers32], or replaces the
 * entry it has, creating the file if need be. Returns 0, or -1 with
 * vfw_error(). The table in memory is not reloaded.
 */
int vfw_ini_remember(FOURCC type, FOURCC handler, const char *path);

/* Adds or replaces a run-time entry. Returns 0 or -1. */
int vfw_register(FOURCC type, FOURCC handler, const char *path);

/* Adds or replaces a run-time entry served by a driver procedure. */
int vfw_register_proc(FOURCC type, FOURCC handler, DRIVERPROC proc);

/*
 * Adds or replaces a run-time entry from a value as an ini line gives it
 * (a Windows path, a path relative to the ini file, or a bare DLL name).
 */
int vfw_register_value(FOURCC type, FOURCC handler, const char *value);

/* Counts changes to the table, for views kept in step with it. */
unsigned vfw_registry_generation(void);

/* Removes an entry. Returns 0, or -1 if there was none. */
int vfw_unregister(FOURCC type, FOURCC handler);

/*
 * ACM drivers, the "msacm.<name>" entries (which aren't type.handler
 * pairs, so vfw_find and vfw_enum don't see them), kept in the order the
 * file gives them.
 */
int vfw_is_acm_alias(const char *name);
/* The i-th: 0 and malloc()ed copies, or -1 past the end. */
int vfw_acm_get(unsigned i, char **alias, char **path);
/* Adds or replaces one from a value as an ini line gives it. 0 or -1. */
int vfw_acm_register_value(const char *alias, const char *value);
/* Removes one. 0, or -1 if there was none. */
int vfw_acm_unregister(const char *alias);

/* Parses "TST0" or "vidc.TST0" (handler padded with spaces). 0 or -1. */
int vfw_parse_fourcc(const char *s, FOURCC *type, FOURCC *handler);

#endif /* VFW_VFW_REGISTRY_H */
