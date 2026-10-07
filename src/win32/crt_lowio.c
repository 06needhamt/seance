/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * msvcrt: the low-level file descriptor functions (_open, _close, _dup...)
 * and the C runtime's threads.
 *
 * The descriptors are the process's own, so they are shared with the
 * stdio functions; a file name is mapped like any other Windows path.
 * Nothing is translated: every descriptor is a binary one, whatever
 * _setmode is told, since a harness reading a codec's own files gains
 * nothing from CR-LF conversion. There is no table tying a descriptor to
 * a Win32 handle, so _get_osfhandle fails.
 */
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <fnmatch.h>
#include <limits.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "runtime/builtin.h"
#include "win32/codepage.h"
#include "win32/internal.h"
#include "win32/k32.h"
#include "win32/path.h"

#define CRTFN(ret) static ret STUB_CDECL

#define CRT_O_WRONLY   0x0001
#define CRT_O_RDWR     0x0002
#define CRT_O_APPEND   0x0008
#define CRT_O_CREAT    0x0100
#define CRT_O_TRUNC    0x0200
#define CRT_O_EXCL     0x0400
#define CRT_O_BINARY   0x8000

static int unix_flags(int oflag)
{
	int f = (oflag & CRT_O_RDWR) ? O_RDWR : (oflag & CRT_O_WRONLY) ? O_WRONLY : O_RDONLY;

	if (oflag & CRT_O_APPEND)
		f |= O_APPEND;
	if (oflag & CRT_O_CREAT)
		f |= O_CREAT;
	if (oflag & CRT_O_TRUNC)
		f |= O_TRUNC;
	if (oflag & CRT_O_EXCL)
		f |= O_EXCL;
	return f;
}

/* Opens the Windows path name (UTF-8) like open(2); -1 with errno set. */
static int open_win(const char *name, int oflag, int pmode)
{
	char *path = name ? path_to_unix(name) : NULL;
	int fd;

	if (!path) {
		errno = name ? ENOENT : EINVAL;
		return -1;
	}
	fd = open(path, unix_flags(oflag), pmode & 0666 ? pmode & 0666 : 0600);
	free(path);
	return fd;
}

static int open_wide(const WCHAR *name, int oflag, int pmode)
{
	char *u = name ? utf16_to_utf8_dup(name) : NULL;
	int fd = open_win(u, oflag, pmode);

	free(u);
	return fd;
}

CRTFN(int) crt__open(const char *name, int oflag, ...)
{
	int pmode = 0;

	if (oflag & CRT_O_CREAT) {
		va_list ap;

		va_start(ap, oflag);
		pmode = va_arg(ap, int);
		va_end(ap);
	}
	return open_win(name, oflag, pmode);
}

CRTFN(int) crt__wopen(const WCHAR *name, int oflag, ...)
{
	int pmode = 0;

	if (oflag & CRT_O_CREAT) {
		va_list ap;

		va_start(ap, oflag);
		pmode = va_arg(ap, int);
		va_end(ap);
	}
	return open_wide(name, oflag, pmode);
}

/* The sharing mode has no meaning here, so it is ignored. */
CRTFN(int) crt__sopen(const char *name, int oflag, int shflag, ...)
{
	int pmode = 0;

	if (oflag & CRT_O_CREAT) {
		va_list ap;

		va_start(ap, shflag);
		pmode = va_arg(ap, int);
		va_end(ap);
	}
	return open_win(name, oflag, pmode);
}

CRTFN(int) crt__wsopen(const WCHAR *name, int oflag, int shflag, ...)
{
	int pmode = 0;

	if (oflag & CRT_O_CREAT) {
		va_list ap;

		va_start(ap, shflag);
		pmode = va_arg(ap, int);
		va_end(ap);
	}
	return open_wide(name, oflag, pmode);
}

CRTFN(int) crt__close(int fd) { return close(fd); }
CRTFN(int) crt__dup(int fd) { return dup(fd); }
CRTFN(int) crt__dup2(int from, int to) { return dup2(from, to) < 0 ? -1 : 0; }
CRTFN(int) crt__isatty(int fd) { return isatty(fd); }

CRTFN(long long) crt__lseeki64(int fd, long long off, int origin)
{
	off_t n = lseek(fd, off, origin);

	return n < 0 ? -1 : n;
}

/* Returns the previous mode: always binary, as above. */
CRTFN(int) crt__setmode(int fd, int mode)
{
	if (fcntl(fd, F_GETFD) < 0)
		return -1;
	return CRT_O_BINARY;
}

CRTFN(intptr_t) crt__get_osfhandle(int fd)
{
	errno = EBADF;
	return -1;
}

CRTFN(int) crt__unlink(const char *name)
{
	char *path = name ? path_to_unix(name) : NULL;
	int r = path ? unlink(path) : (errno = ENOENT, -1);

	free(path);
	return r;
}

CRTFN(int) crt__wunlink(const WCHAR *name)
{
	char *u = name ? utf16_to_utf8_dup(name) : NULL;
	int r = crt__unlink(u);

	free(u);
	return r;
}

CRTFN(int) crt__mkdir(const char *name)
{
	char *path = name ? path_to_unix(name) : NULL;
	int r = path ? mkdir(path, 0777) : (errno = ENOENT, -1);

	free(path);
	return r;
}

CRTFN(int) crt__wmkdir(const WCHAR *name)
{
	char *u = name ? utf16_to_utf8_dup(name) : NULL;
	int r = crt__mkdir(u);

	free(u);
	return r;
}

CRTFN(int) crt__rmdir(const char *name)
{
	char *path = name ? path_to_unix(name) : NULL;
	int r = path ? rmdir(path) : (errno = ENOENT, -1);

	free(path);
	return r;
}

CRTFN(int) crt__wrmdir(const WCHAR *name)
{
	char *u = name ? utf16_to_utf8_dup(name) : NULL;
	int r = crt__rmdir(u);

	free(u);
	return r;
}

CRTFN(int) crt__waccess(const WCHAR *name, int mode)
{
	char *u = name ? utf16_to_utf8_dup(name) : NULL;
	char *path = u ? path_to_unix(u) : NULL;
	int r = path ? access(path, mode) : (errno = ENOENT, -1);

	free(path);
	free(u);
	return r;
}

/* The C runtime's find and path functions follow. */

/* _findfirst and friends: the directory is read lazily, name by name. */

struct crt_find {
	DIR *dir;
	char *dirpath;
	char *mask;
};

/* The layout of _finddata_t and _wfinddata_t: a common head, then the name. */
struct finddata_head {
	unsigned attrib;
	int32_t time_create, time_access, time_write;
	uint32_t size;
};

struct finddata32 {
	struct finddata_head h;
	char name[260];
};

struct wfinddata32 {
	struct finddata_head h;
	WCHAR name[260];
};

#define CRT_A_RDONLY 0x01
#define CRT_A_SUBDIR 0x10
#define CRT_A_ARCH   0x20

/* Windows wildcards: "*.*" matches everything, even names without a dot. */
static int find_match(const char *mask, const char *name)
{
	if (!strcmp(mask, "*.*") || !strcmp(mask, "*"))
		return 1;
	return !fnmatch(mask, name, FNM_CASEFOLD | FNM_PERIOD);
}

/* The next entry matching, as its name and attributes; 0, or -1 at the end. */
static int find_step(struct crt_find *f, char *name, size_t size,
		     struct finddata_head *h)
{
	struct dirent *e;

	while ((e = readdir(f->dir))) {
		char full[PATH_MAX];
		struct stat st;

		if (!find_match(f->mask, e->d_name))
			continue;
		snprintf(full, sizeof(full), "%s/%s", f->dirpath, e->d_name);
		snprintf(name, size, "%s", e->d_name);
		h->attrib = CRT_A_ARCH;
		h->time_create = h->time_access = h->time_write = -1;
		h->size = 0;
		if (!stat(full, &st)) {
			h->attrib = S_ISDIR(st.st_mode) ? CRT_A_SUBDIR : CRT_A_ARCH;
			if (!(st.st_mode & 0222))
				h->attrib |= CRT_A_RDONLY;
			h->time_create = (int32_t)st.st_ctime;
			h->time_access = (int32_t)st.st_atime;
			h->time_write = (int32_t)st.st_mtime;
			h->size = S_ISDIR(st.st_mode) ? 0 : (uint32_t)st.st_size;
		}
		return 0;
	}
	return -1;
}

/* Opens the directory part of a pattern (UTF-8); NULL with errno set. */
static struct crt_find *find_open(const char *pattern)
{
	struct crt_find *f = calloc(1, sizeof(*f));
	char *copy = pattern ? strdup(pattern) : NULL;

	if (!f || !copy) {
		free(f);
		free(copy);
		errno = pattern ? ENOMEM : EINVAL;
		return NULL;
	}
	char *sep = strrchr(copy, '\\');
	char *slash = strrchr(copy, '/');
	if (slash > sep)
		sep = slash;
	const char *mask = sep ? sep + 1 : copy;
	if (sep)
		*sep = '\0';
	f->mask = strdup(*mask ? mask : "*");
	f->dirpath = path_to_unix(sep ? (*copy ? copy : "\\") : ".");
	free(copy);
	if (!f->mask || !f->dirpath || !(f->dir = opendir(f->dirpath))) {
		if (!errno)
			errno = ENOENT;
		if (f->dir)
			closedir(f->dir);
		free(f->mask);
		free(f->dirpath);
		free(f);
		return NULL;
	}
	return f;
}

static void find_free(struct crt_find *f)
{
	closedir(f->dir);
	free(f->mask);
	free(f->dirpath);
	free(f);
}

CRTFN(intptr_t) crt__findfirst(const char *pattern, struct finddata32 *d)
{
	struct crt_find *f = find_open(pattern);

	if (!f)
		return -1;
	if (find_step(f, d->name, sizeof(d->name), &d->h)) {
		find_free(f);
		errno = ENOENT;
		return -1;
	}
	return (intptr_t)f;
}

CRTFN(int) crt__findnext(intptr_t handle, struct finddata32 *d)
{
	struct crt_find *f = (struct crt_find *)handle;

	if (!f || find_step(f, d->name, sizeof(d->name), &d->h)) {
		errno = ENOENT;
		return -1;
	}
	return 0;
}

CRTFN(intptr_t) crt__wfindfirst(const WCHAR *pattern, struct wfinddata32 *d)
{
	char *u = pattern ? utf16_to_utf8_dup(pattern) : NULL;
	struct crt_find *f = find_open(u);
	char name[1024];

	free(u);
	if (!f)
		return -1;
	if (find_step(f, name, sizeof(name), &d->h)) {
		find_free(f);
		errno = ENOENT;
		return -1;
	}
	cp_to_utf16(CP_UTF8, name, -1, d->name, 260, 0);
	d->name[259] = 0;
	return (intptr_t)f;
}

CRTFN(int) crt__wfindnext(intptr_t handle, struct wfinddata32 *d)
{
	struct crt_find *f = (struct crt_find *)handle;
	char name[1024];

	if (!f || find_step(f, name, sizeof(name), &d->h)) {
		errno = ENOENT;
		return -1;
	}
	cp_to_utf16(CP_UTF8, name, -1, d->name, 260, 0);
	d->name[259] = 0;
	return 0;
}

CRTFN(int) crt__findclose(intptr_t handle)
{
	if (!handle || handle == -1) {
		errno = EINVAL;
		return -1;
	}
	find_free((struct crt_find *)handle);
	return 0;
}

/* The current directory, as a Windows path. */
CRTFN(char *) crt__getcwd(char *buf, int size)
{
	char *cwd = path_getcwd();
	size_t len;

	if (!cwd) {
		errno = ENOMEM;
		return NULL;
	}
	len = strlen(cwd);
	if (!buf) {
		size_t room = size > 0 ? (size_t)size : len + 1;

		if (room < len + 1) {
			free(cwd);
			errno = ERANGE;
			return NULL;
		}
		buf = malloc(room);
		if (!buf) {
			free(cwd);
			errno = ENOMEM;
			return NULL;
		}
		size = (int)room;
	} else if (size <= 0 || (size_t)size < len + 1) {
		free(cwd);
		errno = size <= 0 ? EINVAL : ERANGE;
		return NULL;
	}
	memcpy(buf, cwd, len + 1);
	free(cwd);
	return buf;
}

/*
 * A descriptor for a Win32 file handle: one onto the same file, which the
 * caller then owns as the CRT's, leaving the handle its own to close.
 */
CRTFN(int) crt__open_osfhandle(intptr_t handle, int flags)
{
	int fd = file_handle_fd((HANDLE)handle);

	if (fd < 0) {
		errno = EBADF;
		return -1;
	}
	return dup(fd);
}

/* The stdio file limit is only a number here. */
static int max_stdio = 512;

CRTFN(int) crt__getmaxstdio(void)
{
	return max_stdio;
}

CRTFN(int) crt__setmaxstdio(int n)
{
	if (n < 32 || n > 2048) {
		errno = EINVAL;
		return -1;
	}
	return max_stdio = n;
}

/*
 * Threads. The C runtime's own entry points differ from CreateThread in
 * the type of the start routine's result, which is the same register, and
 * in returning the handle as an integer (0 on failure, with errno set).
 */

CRTFN(uintptr_t) crt__beginthreadex(void *security, unsigned stack,
				    unsigned (STUB_WINAPI *start)(void *),
				    void *arg, unsigned flags, unsigned *tid)
{
	HANDLE h = thread_create_ex(stack, (LPTHREAD_START_ROUTINE)start, arg,
				    flags, (LPDWORD)tid);

	if (!h)
		errno = EAGAIN;
	return (uintptr_t)h;
}

CRTFN(void) crt__endthreadex(unsigned code)
{
	thread_exit(code);
}

#define E(name) { #name, 0, (void *)crt_##name }

const struct builtin_export crt_lowio_exports[] = {
	E(_open), E(_wopen), E(_sopen), E(_wsopen), E(_close), E(_dup), E(_dup2),
	E(_isatty), E(_lseeki64), E(_setmode), E(_get_osfhandle),
	E(_unlink), E(_wunlink), E(_mkdir), E(_wmkdir), E(_rmdir), E(_wrmdir),
	E(_waccess), E(_beginthreadex), E(_endthreadex),
	E(_findfirst), E(_findnext), E(_findclose), E(_wfindfirst), E(_wfindnext),
	E(_getcwd), E(_open_osfhandle), E(_getmaxstdio), E(_setmaxstdio),
	BUILTIN_END
};
