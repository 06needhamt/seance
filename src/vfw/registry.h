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

/* Adds or replaces a run-time entry. Returns 0 or -1. */
int vfw_register(FOURCC type, FOURCC handler, const char *path);

/* Adds or replaces a run-time entry served by a driver procedure. */
int vfw_register_proc(FOURCC type, FOURCC handler, DRIVERPROC proc);

/* Removes an entry. Returns 0, or -1 if there was none. */
int vfw_unregister(FOURCC type, FOURCC handler);

/* Parses "TST0" or "vidc.TST0" (handler padded with spaces). 0 or -1. */
int vfw_parse_fourcc(const char *s, FOURCC *type, FOURCC *handler);

#endif /* VFW_VFW_REGISTRY_H */
