/*
 * Windows <-> Unix path mapping, like Wine's:
 *
 *   Z:\x\y       /x/y
 *   C:\x\y       $VFW_ROOT/drive_c/x/y  ($VFW_ROOT defaults to
 *                ~/.seance; C:\WINDOWS\system32 and C:\WINDOWS\TEMP
 *                are created on first use)
 *   x\y, \x\y    relative to the current directory / its drive
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

/* Current directory, as a Windows path (malloc()ed), and changing it. */
char *path_getcwd(void);
int path_chdir(const char *win);

#endif /* VFW_WIN32_PATH_H */
