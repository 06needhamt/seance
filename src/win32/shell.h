/*
 * Shared between shell32's source files.
 */
#ifndef VFW_WIN32_SHELL_H
#define VFW_WIN32_SHELL_H

#include "win32.h"

/*
 * The Windows path (UTF-8, malloc()ed) an item ID list stands for: an
 * empty list is the desktop directory. NULL for a virtual folder (My
 * Computer, the Recycle Bin, ...) or a list this shell32 didn't make.
 */
char *shell_pidl_path(LPCITEMIDLIST pidl);

/* Creates a directory (Unix path) and any missing parents: 0 or -1. */
int shell_mkdirs(const char *unix_path);

#endif /* VFW_WIN32_SHELL_H */
