/* SPDX-License-Identifier: GPL-2.0-only */
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

/* The last item of pidl (NULL for the empty list, the desktop). */
const BYTE *shell_pidl_last(LPCITEMIDLIST pidl);
/* The virtual folder's CSIDL a list stands for (the desktop for the
 * empty list), or -1 for a path. */
int shell_pidl_csidl(LPCITEMIDLIST pidl);
/* One-item lists, allocated as ILClone's. */
LPITEMIDLIST shell_pidl_from_path(const char *win_path);
LPITEMIDLIST shell_pidl_virtual(int csidl);
/* A special folder's Windows path (made if missing), malloc()ed, or NULL. */
char *shell_folder_path(int csidl);

/*
 * The Recycle Bin (shell_recycle.c): moves a file or directory (Unix path)
 * there; a Win32 error code. With norecurse, a directory that isn't
 * empty is refused (ERROR_DIR_NOT_EMPTY).
 */
DWORD shell_recycle(const char *unix_path, int norecurse);
/* Its Windows directory (malloc()ed), made if missing, or NULL. */
char *shell_recycle_dir(void);
/* The original Windows path of an item in it (by its name there). */
char *shell_recycle_original(const char *name);

/* Creates a directory (Unix path) and any missing parents: 0 or -1. */
int shell_mkdirs(const char *unix_path);

#endif /* VFW_WIN32_SHELL_H */
