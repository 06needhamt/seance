/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * Windows <-> Unix path mapping, like Wine's:
 *
 *   Z:\x\y       /x/y
 *   C:\x\y       $VFW_ROOT/drive_c/x/y  ($VFW_ROOT defaults to
 *                ~/.seance; C:\WINDOWS\system32 and C:\WINDOWS\TEMP
 *                are created on first use)
 *   x\y, \x\y    relative to the current directory / its drive: the
 *                Windows side's own, C:\ unless $VFW_CWD or
 *                SetCurrentDirectory says otherwise, not the harness's
 *
 * Components are matched case-insensitively against what exists, since
 * Windows paths are case-insensitive. All strings here are UTF-8.
 */
#ifndef VFW_WIN32_PATH_H
#define VFW_WIN32_PATH_H

/* The Unix path for a Windows one (malloc()ed), or NULL if unmappable. */
char *path_to_unix(const char *win);

/* The Windows path for an absolute Unix one (malloc()ed). */
char *path_to_windows(const char *unix_path);

/* The Unix directory behind C:. */
const char *path_drive_c(void);

/*
 * The Windows side's current directory, a Windows path (malloc()ed), and
 * changing it (-1 with errno if win is no directory); the harness's own
 * stays where it is.
 */
char *path_getcwd(void);
int path_chdir(const char *win);

#endif /* VFW_WIN32_PATH_H */
