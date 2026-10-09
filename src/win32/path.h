/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * Windows <-> Unix path mapping, like Wine's:
 *
 *   Z:\x\y       /x/y
 *   C:\x\y       $VFW_ROOT/drive_c/x/y  ($VFW_ROOT defaults to
 *                ~/.seance; C:\WINDOWS\system32 and C:\WINDOWS\TEMP
 *                are created on first use)
 *   x\y, \x\y    relative to the current directory / its drive; but
 *                with the current directory on Z:, \x\y is on C: unless
 *                only / has an x, as codecs that write "\Windows\codec.ini"
 *                or "\codec.log" mean the system drive (which the current
 *                drive is on Windows), not /Windows or /codec.log
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

/*
 * The drive ('c' or 'z') a path rooted without one ("\x\y") is on: C:
 * when the current drive is, or C: has an x, or / has none; else Z:.
 */
char path_rooted_drive(const char *rooted);

/* The Unix directory behind C:. */
const char *path_drive_c(void);

/* Current directory, as a Windows path (malloc()ed), and changing it. */
char *path_getcwd(void);
int path_chdir(const char *win);

#endif /* VFW_WIN32_PATH_H */
