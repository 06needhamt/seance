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
#include <errno.h>
#include <fcntl.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdlib.h>
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
	BUILTIN_END
};
