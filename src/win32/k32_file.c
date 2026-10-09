/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * kernel32: files, standard handles, directories, search, temporary files,
 * paths, drives, console output and file mappings.
 */
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <fnmatch.h>
#include <limits.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/statvfs.h>
#include <time.h>
#include <unistd.h>

#include "win32/codepage.h"
#include "win32/handle.h"
#include "win32/internal.h"
#include "win32/k32.h"
#include "win32/path.h"

#define FILE_FLAG_BACKUP_SEMANTICS 0x02000000
#define FILE_READ_DATA   0x0001
#define FILE_WRITE_DATA  0x0002
#define FILE_APPEND_DATA 0x0004
#define MOVEFILE_REPLACE_EXISTING 0x1
#define DRIVE_NO_ROOT_DIR 1
#define DRIVE_FIXED 3
#define EPOCH_DIFF 11644473600ull

struct file {
	struct kobject obj;
	int fd;
	int std;		/* 0-2 for the standard streams: never closed */
	int delete_on_close;
	char *path;
};

static void file_destroy(struct kobject *o)
{
	struct file *f = (struct file *)o;

	if (!f->std && f->fd >= 0)
		close(f->fd);
	if (f->delete_on_close && f->path)
		unlink(f->path);
	free(f->path);
	free(f);
}

static HANDLE file_handle_new(int fd, int std, const char *path)
{
	struct file *f = calloc(1, sizeof(*f));

	if (!f) {
		if (!std)
			close(fd);
		win_set_error(ERROR_NOT_ENOUGH_MEMORY);
		return INVALID_HANDLE_VALUE;
	}
	kobj_init(&f->obj, KOBJ_FILE, file_destroy);
	f->fd = fd;
	f->std = std;
	f->path = path ? strdup(path) : NULL;
	HANDLE h = handle_new(&f->obj);
	return h ? h : INVALID_HANDLE_VALUE;
}

int file_handle_fd(HANDLE h)
{
	struct file *f = (struct file *)handle_ref(h, KOBJ_FILE);
	int fd;

	if (!f)
		return -1;
	fd = f->fd;
	kobj_unref(&f->obj);
	return fd;
}

/* Standard handles, created on first use. */

static HANDLE std_handles[3];
static pthread_mutex_t std_lock = PTHREAD_MUTEX_INITIALIZER;

static int std_index(DWORD which)
{
	switch (which) {
	case STD_INPUT_HANDLE:  return 0;
	case STD_OUTPUT_HANDLE: return 1;
	case STD_ERROR_HANDLE:  return 2;
	default:                return -1;
	}
}

HANDLE file_std_handle(DWORD which)
{
	int i = std_index(which);
	HANDLE h;

	if (i < 0) {
		win_set_error(ERROR_INVALID_HANDLE);
		return INVALID_HANDLE_VALUE;
	}
	pthread_mutex_lock(&std_lock);
	if (!std_handles[i])
		std_handles[i] = file_handle_new(i, 1, NULL);
	h = std_handles[i];
	pthread_mutex_unlock(&std_lock);
	return h;
}

WINFN(HANDLE) k32_GetStdHandle(DWORD which)
{
	return file_std_handle(which);
}

WINFN(BOOL) k32_SetStdHandle(DWORD which, HANDLE h)
{
	int i = std_index(which);

	if (i < 0) {
		win_set_error(ERROR_INVALID_HANDLE);
		return FALSE;
	}
	pthread_mutex_lock(&std_lock);
	std_handles[i] = h;
	pthread_mutex_unlock(&std_lock);
	return TRUE;
}

/* Paths. */

/* Converts a Win32 path argument (ANSI or UTF-16) to a Unix path. */
static char *unix_path(const void *name, int wide)
{
	char *u = wide ? utf16_to_utf8_dup(name) : acp_to_utf8(name);
	char *p = u ? path_to_unix(u) : NULL;

	free(u);
	if (!p)
		win_set_error(ERROR_PATH_NOT_FOUND);
	return p;
}

/* ENOENT means FILE_NOT_FOUND only if the directory exists. */
static DWORD not_found_error(const char *path)
{
	char dir[PATH_MAX];
	const char *slash = strrchr(path, '/');

	snprintf(dir, sizeof(dir), "%.*s", slash ? (int)(slash - path) : 1,
		 slash && slash != path ? path : "/");
	return access(dir, F_OK) ? ERROR_PATH_NOT_FOUND : ERROR_FILE_NOT_FOUND;
}

static void set_path_error(const char *path)
{
	win_set_error(errno == ENOENT ? not_found_error(path) :
		      win_error_from_errno(errno));
}

/* Files. */

static HANDLE create_file(const void *name, int wide, DWORD access,
			  DWORD disposition, DWORD flags)
{
	char *u = wide ? utf16_to_utf8_dup(name) : acp_to_utf8(name);
	int oflags = O_CLOEXEC;

	if (!u) {
		win_set_error(ERROR_INVALID_PARAMETER);
		return INVALID_HANDLE_VALUE;
	}
	/* Device names. */
	if (!strcasecmp(u, "CONOUT$") || !strcasecmp(u, "CONIN$")) {
		int std = !strcasecmp(u, "CONIN$") ? 0 : 1;
		free(u);
		return file_handle_new(std, 1, NULL);
	}
	int is_nul = !strcasecmp(u, "NUL") || !strcasecmp(u, "\\\\.\\NUL");
	char *path = is_nul ? strdup("/dev/null") : path_to_unix(u);
	free(u);
	if (!path) {
		win_set_error(ERROR_PATH_NOT_FOUND);
		return INVALID_HANDLE_VALUE;
	}

	int rd = (access & (GENERIC_READ | GENERIC_ALL | FILE_READ_DATA)) != 0;
	int wr = (access & (GENERIC_WRITE | GENERIC_ALL | FILE_WRITE_DATA |
			    FILE_APPEND_DATA)) != 0;
	oflags |= wr ? (rd ? O_RDWR : O_WRONLY) : O_RDONLY;

	struct stat st;
	int exists = !stat(path, &st);
	switch (disposition) {
	case CREATE_NEW:        oflags |= O_CREAT | O_EXCL; break;
	case CREATE_ALWAYS:     oflags |= O_CREAT | O_TRUNC; break;
	case OPEN_EXISTING:     break;
	case OPEN_ALWAYS:       oflags |= O_CREAT; break;
	case TRUNCATE_EXISTING: oflags |= O_TRUNC; break;
	default:
		free(path);
		win_set_error(ERROR_INVALID_PARAMETER);
		return INVALID_HANDLE_VALUE;
	}
	if (exists && S_ISDIR(st.st_mode)) {
		if (!(flags & FILE_FLAG_BACKUP_SEMANTICS) || wr) {
			free(path);
			win_set_error(ERROR_ACCESS_DENIED);
			return INVALID_HANDLE_VALUE;
		}
		oflags = O_RDONLY | O_DIRECTORY | O_CLOEXEC;
	}
	if ((oflags & O_TRUNC) && !wr)
		oflags = (oflags & ~(O_RDONLY | O_WRONLY | O_RDWR)) | O_RDWR;

	int fd = open(path, oflags, 0644);
	if (fd < 0) {
		DWORD err = errno == EEXIST ? ERROR_FILE_EXISTS :
			    errno == ENOENT ? not_found_error(path) :
			    win_error_from_errno(errno);
		TRACE("CreateFile(%s) failed: %s", path, strerror(errno));
		free(path);
		win_set_error(err);
		return INVALID_HANDLE_VALUE;
	}

	HANDLE h = file_handle_new(fd, 0, path);
	if (h != INVALID_HANDLE_VALUE) {
		struct file *f = (struct file *)handle_ref(h, KOBJ_FILE);
		f->delete_on_close = (flags & FILE_FLAG_DELETE_ON_CLOSE) != 0;
		kobj_unref(&f->obj);
		win_set_error(exists && (disposition == CREATE_ALWAYS ||
					 disposition == OPEN_ALWAYS) ?
			      ERROR_ALREADY_EXISTS : ERROR_SUCCESS);
	}
	TRACE("CreateFile(%s) = %p", path, h);
	free(path);
	return h;
}

WINFN(HANDLE) k32_CreateFileA(LPCSTR name, DWORD access, DWORD share,
			      LPSECURITY_ATTRIBUTES sa, DWORD disposition,
			      DWORD flags, HANDLE templ)
{
	return create_file(name, 0, access, disposition, flags);
}

WINFN(HANDLE) k32_CreateFileW(LPCWSTR name, DWORD access, DWORD share,
			      LPSECURITY_ATTRIBUTES sa, DWORD disposition,
			      DWORD flags, HANDLE templ)
{
	return create_file(name, 1, access, disposition, flags);
}

static struct file *file_of(HANDLE h)
{
	return (struct file *)handle_ref(h, KOBJ_FILE);
}

WINFN(BOOL) k32_ReadFile(HANDLE h, LPVOID buf, DWORD n, LPDWORD done,
			 LPOVERLAPPED ov)
{
	struct file *f = file_of(h);
	DWORD total = 0;
	BOOL ok = TRUE;

	if (done)
		*done = 0;
	if (!f)
		return FALSE;
	off_t pos = ov ? (off_t)(((ULONGLONG)ov->OffsetHigh << 32) | ov->Offset) : 0;
	/* Disk files are read in full; pipes and consoles return what they have. */
	struct stat st;
	int stream = !fstat(f->fd, &st) && !S_ISREG(st.st_mode);
	while (total < n) {
		ssize_t r = ov ? pread(f->fd, (BYTE *)buf + total, n - total,
				       pos + total) :
			    read(f->fd, (BYTE *)buf + total, n - total);
		if (r < 0) {
			if (errno == EINTR)
				continue;
			ok = WIN_ERRNO(FALSE);
			break;
		}
		total += r;
		if (!r || stream)
			break;
	}
	if (done)
		*done = total;
	if (ov)
		ov->InternalHigh = total;
	kobj_unref(&f->obj);
	return ok;
}

WINFN(BOOL) k32_WriteFile(HANDLE h, LPCVOID buf, DWORD n, LPDWORD done,
			  LPOVERLAPPED ov)
{
	struct file *f = file_of(h);
	DWORD total = 0;
	BOOL ok = TRUE;

	if (done)
		*done = 0;
	if (!f)
		return FALSE;
	off_t pos = ov ? (off_t)(((ULONGLONG)ov->OffsetHigh << 32) | ov->Offset) : 0;
	while (total < n) {
		ssize_t r = ov ? pwrite(f->fd, (const BYTE *)buf + total,
					n - total, pos + total) :
			    write(f->fd, (const BYTE *)buf + total, n - total);
		if (r < 0) {
			if (errno == EINTR)
				continue;
			ok = WIN_ERRNO(FALSE);
			break;
		}
		total += r;
	}
	if (done)
		*done = total;
	if (ov)
		ov->InternalHigh = total;
	kobj_unref(&f->obj);
	return ok;
}

static int seek_whence(DWORD method)
{
	switch (method) {
	case FILE_BEGIN:   return SEEK_SET;
	case FILE_CURRENT: return SEEK_CUR;
	case FILE_END:     return SEEK_END;
	default:           return -1;
	}
}

WINFN(DWORD) k32_SetFilePointer(HANDLE h, LONG low, PLONG high, DWORD method)
{
	struct file *f = file_of(h);
	int whence = seek_whence(method);
	LONGLONG dist = high ? ((LONGLONG)*high << 32) | (DWORD)low : low;

	if (!f)
		return INVALID_SET_FILE_POINTER;
	if (whence < 0) {
		kobj_unref(&f->obj);
		win_set_error(ERROR_INVALID_PARAMETER);
		return INVALID_SET_FILE_POINTER;
	}
	off_t r = lseek(f->fd, dist, whence);
	kobj_unref(&f->obj);
	if (r < 0) {
		win_set_error(errno == EINVAL ? ERROR_NEGATIVE_SEEK :
			      win_error_from_errno(errno));
		return INVALID_SET_FILE_POINTER;
	}
	if (high)
		*high = (LONG)((ULONGLONG)r >> 32);
	win_set_error(ERROR_SUCCESS);
	return (DWORD)r;
}

WINFN(BOOL) k32_SetFilePointerEx(HANDLE h, LARGE_INTEGER dist,
				 PLARGE_INTEGER out, DWORD method)
{
	struct file *f = file_of(h);
	int whence = seek_whence(method);

	if (!f)
		return FALSE;
	off_t r = whence < 0 ? (errno = EINVAL, -1) :
		  lseek(f->fd, dist.QuadPart, whence);
	kobj_unref(&f->obj);
	if (r < 0)
		return WIN_ERRNO(FALSE);
	if (out)
		out->QuadPart = r;
	return TRUE;
}

WINFN(DWORD) k32_GetFileSize(HANDLE h, LPDWORD high)
{
	struct file *f = file_of(h);
	struct stat st;

	if (!f)
		return INVALID_FILE_SIZE;
	int r = fstat(f->fd, &st);
	kobj_unref(&f->obj);
	if (r)
		return WIN_ERRNO(INVALID_FILE_SIZE);
	if (high)
		*high = (DWORD)((ULONGLONG)st.st_size >> 32);
	win_set_error(ERROR_SUCCESS);
	return (DWORD)st.st_size;
}

WINFN(BOOL) k32_GetFileSizeEx(HANDLE h, PLARGE_INTEGER size)
{
	struct file *f = file_of(h);
	struct stat st;

	if (!f)
		return FALSE;
	int r = fstat(f->fd, &st);
	kobj_unref(&f->obj);
	if (r)
		return WIN_ERRNO(FALSE);
	size->QuadPart = st.st_size;
	return TRUE;
}

WINFN(DWORD) k32_GetFileType(HANDLE h)
{
	struct file *f = file_of(h);
	struct stat st;
	DWORD type = FILE_TYPE_UNKNOWN;

	if (!f)
		return type;
	if (!fstat(f->fd, &st))
		type = S_ISREG(st.st_mode) || S_ISDIR(st.st_mode) ? FILE_TYPE_DISK :
		       S_ISCHR(st.st_mode) ? FILE_TYPE_CHAR : FILE_TYPE_PIPE;
	kobj_unref(&f->obj);
	return type;
}

WINFN(BOOL) k32_FlushFileBuffers(HANDLE h)
{
	struct file *f = file_of(h);

	if (!f)
		return FALSE;
	if (!f->std)
		fsync(f->fd);
	kobj_unref(&f->obj);
	return TRUE;
}

WINFN(BOOL) k32_SetEndOfFile(HANDLE h)
{
	struct file *f = file_of(h);

	if (!f)
		return FALSE;
	off_t pos = lseek(f->fd, 0, SEEK_CUR);
	int r = pos < 0 ? -1 : ftruncate(f->fd, pos);
	kobj_unref(&f->obj);
	return r ? WIN_ERRNO(FALSE) : TRUE;
}

static void timespec_to_filetime(const struct timespec *ts, LPFILETIME ft)
{
	ULONGLONG v = ((ULONGLONG)ts->tv_sec + EPOCH_DIFF) * 10000000ull +
		      ts->tv_nsec / 100;
	ft->dwLowDateTime = (DWORD)v;
	ft->dwHighDateTime = (DWORD)(v >> 32);
}

WINFN(BOOL) k32_GetFileTime(HANDLE h, LPFILETIME created, LPFILETIME accessed,
			    LPFILETIME written)
{
	struct file *f = file_of(h);
	struct stat st;

	if (!f)
		return FALSE;
	int r = fstat(f->fd, &st);
	kobj_unref(&f->obj);
	if (r)
		return WIN_ERRNO(FALSE);
	if (created)
		timespec_to_filetime(&st.st_mtim, created);
	if (accessed)
		timespec_to_filetime(&st.st_atim, accessed);
	if (written)
		timespec_to_filetime(&st.st_mtim, written);
	return TRUE;
}

WINFN(BOOL) k32_SetFileTime(HANDLE h, const FILETIME *c, const FILETIME *a,
			    const FILETIME *w)
{
	struct file *f = file_of(h);

	if (!f)
		return FALSE;
	kobj_unref(&f->obj);
	return TRUE;
}

WINFN(BOOL) k32_LockFile(HANDLE h, DWORD a, DWORD b, DWORD c, DWORD d) { return TRUE; }
WINFN(BOOL) k32_UnlockFile(HANDLE h, DWORD a, DWORD b, DWORD c, DWORD d) { return TRUE; }

/* Files by name. */

static BOOL delete_file(const void *name, int wide)
{
	char *p = unix_path(name, wide);
	struct stat st;
	BOOL ok;

	if (!p)
		return FALSE;
	if (!stat(p, &st) && S_ISDIR(st.st_mode)) {
		win_set_error(ERROR_ACCESS_DENIED);
		ok = FALSE;
	} else if (unlink(p)) {
		set_path_error(p);
		ok = FALSE;
	} else {
		ok = TRUE;
	}
	free(p);
	return ok;
}

WINFN(BOOL) k32_DeleteFileA(LPCSTR name) { return delete_file(name, 0); }
WINFN(BOOL) k32_DeleteFileW(LPCWSTR name) { return delete_file(name, 1); }

static DWORD attributes_of(const struct stat *st)
{
	if (S_ISDIR(st->st_mode))
		return FILE_ATTRIBUTE_DIRECTORY;
	return (st->st_mode & 0222) ? FILE_ATTRIBUTE_ARCHIVE :
				      FILE_ATTRIBUTE_ARCHIVE | FILE_ATTRIBUTE_READONLY;
}

static DWORD file_attributes(const void *name, int wide)
{
	char *p = unix_path(name, wide);
	struct stat st;
	DWORD a;

	if (!p)
		return INVALID_FILE_ATTRIBUTES;
	if (stat(p, &st)) {
		set_path_error(p);
		a = INVALID_FILE_ATTRIBUTES;
	} else {
		a = attributes_of(&st);
	}
	free(p);
	return a;
}

WINFN(DWORD) k32_GetFileAttributesA(LPCSTR name) { return file_attributes(name, 0); }
WINFN(DWORD) k32_GetFileAttributesW(LPCWSTR name) { return file_attributes(name, 1); }

WINFN(BOOL) k32_SetFileAttributesA(LPCSTR name, DWORD attrs)
{
	return file_attributes(name, 0) != INVALID_FILE_ATTRIBUTES;
}

WINFN(BOOL) k32_SetFileAttributesW(LPCWSTR name, DWORD attrs)
{
	return file_attributes(name, 1) != INVALID_FILE_ATTRIBUTES;
}

static BOOL create_directory(const void *name, int wide)
{
	char *p = unix_path(name, wide);
	BOOL ok = TRUE;

	if (!p)
		return FALSE;
	if (mkdir(p, 0755)) {
		if (errno == EEXIST)
			win_set_error(ERROR_ALREADY_EXISTS);
		else
			set_path_error(p);
		ok = FALSE;
	}
	free(p);
	return ok;
}

WINFN(BOOL) k32_CreateDirectoryA(LPCSTR name, LPSECURITY_ATTRIBUTES sa) { return create_directory(name, 0); }
WINFN(BOOL) k32_CreateDirectoryW(LPCWSTR name, LPSECURITY_ATTRIBUTES sa) { return create_directory(name, 1); }

static BOOL remove_directory(const void *name, int wide)
{
	char *p = unix_path(name, wide);
	BOOL ok = TRUE;

	if (!p)
		return FALSE;
	if (rmdir(p)) {
		set_path_error(p);
		ok = FALSE;
	}
	free(p);
	return ok;
}

WINFN(BOOL) k32_RemoveDirectoryA(LPCSTR name) { return remove_directory(name, 0); }
WINFN(BOOL) k32_RemoveDirectoryW(LPCWSTR name) { return remove_directory(name, 1); }

static BOOL move_file(const void *from, const void *to, DWORD flags, int wide)
{
	char *a = unix_path(from, wide), *b = unix_path(to, wide);
	BOOL ok = FALSE;

	if (a && b) {
		if (!(flags & MOVEFILE_REPLACE_EXISTING) && !access(b, F_OK))
			win_set_error(ERROR_ALREADY_EXISTS);
		else if (rename(a, b))
			set_path_error(a);
		else
			ok = TRUE;
	}
	free(a);
	free(b);
	return ok;
}

WINFN(BOOL) k32_MoveFileA(LPCSTR a, LPCSTR b) { return move_file(a, b, 0, 0); }
WINFN(BOOL) k32_MoveFileW(LPCWSTR a, LPCWSTR b) { return move_file(a, b, 0, 1); }
WINFN(BOOL) k32_MoveFileExA(LPCSTR a, LPCSTR b, DWORD f) { return move_file(a, b, f, 0); }
WINFN(BOOL) k32_MoveFileExW(LPCWSTR a, LPCWSTR b, DWORD f) { return move_file(a, b, f, 1); }

static BOOL copy_file(const void *from, const void *to, BOOL fail_if_exists,
		      int wide)
{
	char *a = unix_path(from, wide), *b = unix_path(to, wide);
	int in = -1, out = -1;
	BOOL ok = FALSE;

	if (!a || !b)
		goto done;
	if ((in = open(a, O_RDONLY | O_CLOEXEC)) < 0) {
		set_path_error(a);
		goto done;
	}
	out = open(b, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC |
		   (fail_if_exists ? O_EXCL : 0), 0644);
	if (out < 0) {
		win_set_error(errno == EEXIST ? ERROR_FILE_EXISTS :
			      win_error_from_errno(errno));
		goto done;
	}
	char buf[65536];
	ssize_t n;
	while ((n = read(in, buf, sizeof(buf))) > 0)
		if (write(out, buf, n) != n)
			break;
	ok = n == 0;
	if (!ok)
		win_set_error(win_error_from_errno(errno));
done:
	if (in >= 0)
		close(in);
	if (out >= 0)
		close(out);
	free(a);
	free(b);
	return ok;
}

WINFN(BOOL) k32_CopyFileA(LPCSTR a, LPCSTR b, BOOL f) { return copy_file(a, b, f, 0); }
WINFN(BOOL) k32_CopyFileW(LPCWSTR a, LPCWSTR b, BOOL f) { return copy_file(a, b, f, 1); }

/* FindFirstFile / FindNextFile. */

struct find {
	struct kobject obj;
	DIR *dir;
	char *dirpath;
	char *mask;
	int wide;
};

static void find_destroy(struct kobject *o)
{
	struct find *f = (struct find *)o;

	if (f->dir)
		closedir(f->dir);
	free(f->dirpath);
	free(f->mask);
	free(f);
}

/* Windows wildcards: "*.*" matches everything, even names without a dot. */
static int wild_match(const char *mask, const char *name)
{
	if (!strcmp(mask, "*.*") || !strcmp(mask, "*"))
		return 1;
	return !fnmatch(mask, name, FNM_CASEFOLD | FNM_PERIOD);
}

static void fill_find_data(struct find *f, const char *name, void *data)
{
	char full[PATH_MAX];
	struct stat st;
	FILETIME ft = { 0, 0 };
	DWORD attrs = FILE_ATTRIBUTE_ARCHIVE, hi = 0, lo = 0;

	snprintf(full, sizeof(full), "%s/%s", f->dirpath, name);
	if (!stat(full, &st)) {
		attrs = attributes_of(&st);
		timespec_to_filetime(&st.st_mtim, &ft);
		if (!S_ISDIR(st.st_mode)) {
			hi = (DWORD)((ULONGLONG)st.st_size >> 32);
			lo = (DWORD)st.st_size;
		}
	}
	if (f->wide) {
		WIN32_FIND_DATAW *d = data;
		memset(d, 0, sizeof(*d));
		d->dwFileAttributes = attrs;
		d->ftCreationTime = d->ftLastAccessTime = d->ftLastWriteTime = ft;
		d->nFileSizeHigh = hi;
		d->nFileSizeLow = lo;
		cp_to_utf16(CP_UTF8, name, -1, d->cFileName, MAX_PATH, 0);
		d->cFileName[MAX_PATH - 1] = 0;
	} else {
		WIN32_FIND_DATAA *d = data;
		char *a = utf8_to_acp_dup(name);
		memset(d, 0, sizeof(*d));
		d->dwFileAttributes = attrs;
		d->ftCreationTime = d->ftLastAccessTime = d->ftLastWriteTime = ft;
		d->nFileSizeHigh = hi;
		d->nFileSizeLow = lo;
		snprintf(d->cFileName, MAX_PATH, "%s", a ? a : name);
		free(a);
	}
}

static BOOL find_next(struct find *f, void *data)
{
	struct dirent *e;

	while ((e = readdir(f->dir))) {
		if (wild_match(f->mask, e->d_name)) {
			fill_find_data(f, e->d_name, data);
			return TRUE;
		}
	}
	win_set_error(ERROR_NO_MORE_FILES);
	return FALSE;
}

static HANDLE find_first(const void *pattern, void *data, int wide)
{
	char *u = wide ? utf16_to_utf8_dup(pattern) : acp_to_utf8(pattern);
	struct find *f;

	if (!u) {
		win_set_error(ERROR_INVALID_PARAMETER);
		return INVALID_HANDLE_VALUE;
	}
	char *sep = strrchr(u, '\\');
	char *slash = strrchr(u, '/');
	if (slash > sep)
		sep = slash;
	const char *mask = sep ? sep + 1 : u;
	if (sep)
		*sep = '\0';

	f = calloc(1, sizeof(*f));
	if (!f) {
		free(u);
		win_set_error(ERROR_NOT_ENOUGH_MEMORY);
		return INVALID_HANDLE_VALUE;
	}
	kobj_init(&f->obj, KOBJ_FIND, find_destroy);
	f->wide = wide;
	f->mask = strdup(*mask ? mask : "*");
	f->dirpath = path_to_unix(sep ? (*u ? u : "\\") : ".");
	free(u);
	if (!f->mask || !f->dirpath || !(f->dir = opendir(f->dirpath))) {
		win_set_error(f->dirpath ? ERROR_PATH_NOT_FOUND : ERROR_NOT_ENOUGH_MEMORY);
		find_destroy(&f->obj);
		return INVALID_HANDLE_VALUE;
	}
	if (!find_next(f, data)) {
		find_destroy(&f->obj);
		win_set_error(ERROR_FILE_NOT_FOUND);
		return INVALID_HANDLE_VALUE;
	}
	HANDLE h = handle_new(&f->obj);
	return h ? h : INVALID_HANDLE_VALUE;
}

WINFN(HANDLE) k32_FindFirstFileA(LPCSTR pattern, LPWIN32_FIND_DATAA data)
{
	return find_first(pattern, data, 0);
}

WINFN(HANDLE) k32_FindFirstFileW(LPCWSTR pattern, LPWIN32_FIND_DATAW data)
{
	return find_first(pattern, data, 1);
}

WINFN(HANDLE) k32_FindFirstFileExW(LPCWSTR pattern, DWORD level,
				   LPVOID data, DWORD op, LPVOID filter,
				   DWORD flags)
{
	/*
	 * The basic level only leaves out the short name, which we never
	 * give. A directories-only search is a hint a caller must check for
	 * itself, so is ignored like on filesystems that can't do it.
	 */
	if (level > FindExInfoBasic || op > FindExSearchLimitToDevices) {
		win_set_error(ERROR_INVALID_PARAMETER);
		return INVALID_HANDLE_VALUE;
	}
	return find_first(pattern, data, 1);
}

static BOOL find_next_handle(HANDLE h, void *data)
{
	struct find *f = (struct find *)handle_ref(h, KOBJ_FIND);
	BOOL r;

	if (!f)
		return FALSE;
	r = find_next(f, data);
	kobj_unref(&f->obj);
	return r;
}

WINFN(BOOL) k32_FindNextFileA(HANDLE h, LPWIN32_FIND_DATAA d) { return find_next_handle(h, d); }
WINFN(BOOL) k32_FindNextFileW(HANDLE h, LPWIN32_FIND_DATAW d) { return find_next_handle(h, d); }

WINFN(BOOL) k32_FindClose(HANDLE h)
{
	struct find *f = (struct find *)handle_ref(h, KOBJ_FIND);

	if (!f)
		return FALSE;
	kobj_unref(&f->obj);
	return handle_close(h);
}

/* Directories and temporary files. */

#define TEMP_DIR "C:\\WINDOWS\\TEMP\\"

WINFN(DWORD) k32_GetTempPathA(DWORD size, LPSTR buf) { return copy_out_a(TEMP_DIR, buf, size); }
WINFN(DWORD) k32_GetTempPathW(DWORD size, LPWSTR buf) { return copy_out_w(TEMP_DIR, buf, size); }

static UINT temp_file_name(const char *dir, const char *prefix, UINT unique,
			   char *out, size_t outsz)
{
	int create = !unique;

	if (!unique)
		unique = (UINT)(time(NULL) ^ getpid()) & 0xffff;
	for (int tries = 0; tries < 0x10000; tries++) {
		UINT n = unique & 0xffff;
		if (!n)
			n = 1;
		snprintf(out, outsz, "%s%s%.3s%X.TMP", dir,
			 dir[0] && dir[strlen(dir) - 1] != '\\' ? "\\" : "",
			 prefix ? prefix : "", n);
		if (!create)
			return n;
		char *p = path_to_unix(out);
		int fd = p ? open(p, O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC, 0644) : -1;
		int err = errno;
		free(p);
		if (fd >= 0) {
			close(fd);
			return n;
		}
		if (err != EEXIST) {
			win_set_error(win_error_from_errno(err));
			return 0;
		}
		unique = n + 1;
	}
	win_set_error(ERROR_FILE_EXISTS);
	return 0;
}

WINFN(UINT) k32_GetTempFileNameA(LPCSTR dir, LPCSTR prefix, UINT unique,
				 LPSTR out)
{
	char *d = acp_to_utf8(dir), *p = acp_to_utf8(prefix);
	char name[MAX_PATH * 3];
	UINT r = d ? temp_file_name(d, p, unique, name, sizeof(name)) : 0;

	if (r)
		copy_out_a(name, out, MAX_PATH);
	free(d);
	free(p);
	return r;
}

WINFN(UINT) k32_GetTempFileNameW(LPCWSTR dir, LPCWSTR prefix, UINT unique,
				 LPWSTR out)
{
	char *d = utf16_to_utf8_dup(dir), *p = utf16_to_utf8_dup(prefix);
	char name[MAX_PATH * 3];
	UINT r = d ? temp_file_name(d, p, unique, name, sizeof(name)) : 0;

	if (r)
		copy_out_w(name, out, MAX_PATH);
	free(d);
	free(p);
	return r;
}

/* The full Windows form of name: absolute, with . and .. resolved. */
char *k32_full_path(const char *name)
{
	char buf[PATH_MAX * 2];
	char *cwd = path_getcwd();

	if (!cwd)
		return NULL;
	if (name[0] && name[1] == ':' && (name[2] == '\\' || name[2] == '/'))
		snprintf(buf, sizeof(buf), "%s", name);
	else if (name[0] && name[1] == ':')
		snprintf(buf, sizeof(buf), "%c:\\%s", name[0], name + 2);
	else if (name[0] == '\\' || name[0] == '/')
		snprintf(buf, sizeof(buf), "%c:%s",
			 path_rooted_drive(name) == 'c' ? 'C' : 'Z', name);
	else
		snprintf(buf, sizeof(buf), "%s\\%s", cwd, name);
	free(cwd);

	/* Normalise separators, then collapse "." and "..". */
	for (char *p = buf; *p; p++)
		if (*p == '/')
			*p = '\\';
	char out[PATH_MAX * 2];
	size_t n = 3, root = 3;
	memcpy(out, buf, 3);
	for (char *p = buf + 3; *p;) {
		while (*p == '\\')
			p++;
		char *end = strchr(p, '\\');
		size_t len = end ? (size_t)(end - p) : strlen(p);
		if (!len)
			break;
		if (len == 1 && p[0] == '.') {
			/* skip */
		} else if (len == 2 && p[0] == '.' && p[1] == '.') {
			/* Drop the last component and its separator. */
			while (n > root && out[n - 1] != '\\')
				n--;
			if (n > root)
				n--;
		} else {
			if (n > root && out[n - 1] != '\\')
				out[n++] = '\\';
			memcpy(out + n, p, len);
			n += len;
		}
		p += len;
	}
	/* Keep a trailing separator the caller gave. */
	size_t blen = strlen(buf);
	if (blen > 3 && buf[blen - 1] == '\\' && out[n - 1] != '\\')
		out[n++] = '\\';
	out[n] = '\0';
	out[0] = (out[0] >= 'a' && out[0] <= 'z') ? out[0] - 32 : out[0];
	return strdup(out);
}

static DWORD full_path_name(const void *name, int wide, DWORD size, void *buf,
			    void **filepart)
{
	char *u = wide ? utf16_to_utf8_dup(name) : acp_to_utf8(name);
	char *full = u ? k32_full_path(u) : NULL;
	DWORD r = 0;

	free(u);
	if (!full) {
		win_set_error(ERROR_INVALID_PARAMETER);
		return 0;
	}
	r = wide ? copy_out_w(full, buf, size) : copy_out_a(full, buf, size);
	if (filepart && r < size) {
		if (wide) {
			WCHAR *w = buf, *last = NULL;
			for (WCHAR *p = w; *p; p++)
				if (*p == '\\')
					last = p + 1;
			*filepart = last && *last ? last : NULL;
		} else {
			char *s = buf;
			char *last = strrchr(s, '\\');
			*filepart = last && last[1] ? last + 1 : NULL;
		}
	}
	free(full);
	return r;
}

WINFN(DWORD) k32_GetFullPathNameA(LPCSTR name, DWORD size, LPSTR buf,
				  LPSTR *filepart)
{
	return full_path_name(name, 0, size, buf, (void **)filepart);
}

WINFN(DWORD) k32_GetFullPathNameW(LPCWSTR name, DWORD size, LPWSTR buf,
				  LPWSTR *filepart)
{
	return full_path_name(name, 1, size, buf, (void **)filepart);
}

WINFN(DWORD) k32_GetCurrentDirectoryA(DWORD size, LPSTR buf)
{
	char *cwd = path_getcwd();
	DWORD r = cwd ? copy_out_a(cwd, buf, size) : 0;
	free(cwd);
	return r;
}

WINFN(DWORD) k32_GetCurrentDirectoryW(DWORD size, LPWSTR buf)
{
	char *cwd = path_getcwd();
	DWORD r = cwd ? copy_out_w(cwd, buf, size) : 0;
	free(cwd);
	return r;
}

static BOOL set_current_directory(const void *name, int wide)
{
	char *u = wide ? utf16_to_utf8_dup(name) : acp_to_utf8(name);
	int r = u ? path_chdir(u) : -1;

	free(u);
	return r ? WIN_ERRNO(FALSE) : TRUE;
}

WINFN(BOOL) k32_SetCurrentDirectoryA(LPCSTR name) { return set_current_directory(name, 0); }
WINFN(BOOL) k32_SetCurrentDirectoryW(LPCWSTR name) { return set_current_directory(name, 1); }

/* Short and long names are the same here. */
WINFN(DWORD) k32_GetShortPathNameA(LPCSTR in, LPSTR out, DWORD size)
{
	char *u = acp_to_utf8(in);
	DWORD r = u ? copy_out_a(u, out, size) : 0;
	free(u);
	return r;
}

WINFN(DWORD) k32_GetShortPathNameW(LPCWSTR in, LPWSTR out, DWORD size)
{
	char *u = utf16_to_utf8_dup(in);
	DWORD r = u ? copy_out_w(u, out, size) : 0;
	free(u);
	return r;
}

WINFN(DWORD) k32_GetLongPathNameA(LPCSTR in, LPSTR out, DWORD size)
{
	return k32_GetShortPathNameA(in, out, size);
}

WINFN(DWORD) k32_GetLongPathNameW(LPCWSTR in, LPWSTR out, DWORD size)
{
	return k32_GetShortPathNameW(in, out, size);
}

/* Drives: C: (the sandbox) and Z: (the Unix root). */

static UINT drive_type(char d)
{
	d |= 0x20;
	return d == 'c' || d == 'z' ? DRIVE_FIXED : DRIVE_NO_ROOT_DIR;
}

WINFN(UINT) k32_GetDriveTypeA(LPCSTR root)
{
	if (!root) {
		char *cwd = path_getcwd();
		UINT t = cwd ? drive_type(cwd[0]) : DRIVE_FIXED;
		free(cwd);
		return t;
	}
	return drive_type(root[0]);
}

WINFN(UINT) k32_GetDriveTypeW(LPCWSTR root)
{
	return root ? drive_type((char)root[0]) : k32_GetDriveTypeA(NULL);
}

WINFN(DWORD) k32_GetLogicalDrives(void)
{
	return (1u << 2) | (1u << 25);
}

static BOOL disk_space(const char *root, ULONGLONG *free_bytes,
		       ULONGLONG *total_bytes, DWORD *block)
{
	struct statvfs sv;
	const char *path = root && (root[0] | 0x20) == 'c' ? path_drive_c() : "/";

	if (statvfs(path, &sv))
		return WIN_ERRNO(FALSE);
	*free_bytes = (ULONGLONG)sv.f_bavail * sv.f_frsize;
	*total_bytes = (ULONGLONG)sv.f_blocks * sv.f_frsize;
	*block = sv.f_frsize;
	return TRUE;
}

WINFN(BOOL) k32_GetDiskFreeSpaceA(LPCSTR root, LPDWORD spc, LPDWORD bps,
				  LPDWORD free_clusters, LPDWORD total_clusters)
{
	ULONGLONG f, t;
	DWORD block;

	if (!disk_space(root, &f, &t, &block))
		return FALSE;
	/* Old callers use 32-bit arithmetic: cap at 2 GB as Windows did. */
	if (f > 0x7fffffff)
		f = 0x7fffffff;
	if (t > 0x7fffffff)
		t = 0x7fffffff;
	if (spc)
		*spc = 8;
	if (bps)
		*bps = 512;
	if (free_clusters)
		*free_clusters = f / 4096;
	if (total_clusters)
		*total_clusters = t / 4096;
	return TRUE;
}

WINFN(BOOL) k32_GetDiskFreeSpaceExA(LPCSTR root, ULARGE_INTEGER *avail,
				    ULARGE_INTEGER *total, ULARGE_INTEGER *free_all)
{
	ULONGLONG f, t;
	DWORD block;

	if (!disk_space(root, &f, &t, &block))
		return FALSE;
	if (avail)
		avail->QuadPart = f;
	if (total)
		total->QuadPart = t;
	if (free_all)
		free_all->QuadPart = f;
	return TRUE;
}

WINFN(BOOL) k32_GetVolumeInformationA(LPCSTR root, LPSTR name, DWORD name_size,
				      LPDWORD serial, LPDWORD max_comp,
				      LPDWORD flags, LPSTR fs, DWORD fs_size)
{
	if (name && name_size)
		snprintf(name, name_size, "%s", "VFW");
	if (serial)
		*serial = 0x56465721;
	if (max_comp)
		*max_comp = 255;
	if (flags)
		*flags = 0x3;		/* case sensitive and preserving */
	if (fs && fs_size)
		snprintf(fs, fs_size, "%s", "NTFS");
	return TRUE;
}

WINFN(BOOL) k32_GetFileInformationByHandle(HANDLE h,
					   LPBY_HANDLE_FILE_INFORMATION info)
{
	struct file *f = file_of(h);
	struct stat st;

	if (!f)
		return FALSE;
	if (fstat(f->fd, &st)) {
		kobj_unref(&f->obj);
		return WIN_ERRNO(FALSE);
	}
	kobj_unref(&f->obj);
	memset(info, 0, sizeof(*info));
	info->dwFileAttributes = attributes_of(&st);
	timespec_to_filetime(&st.st_mtim, &info->ftLastWriteTime);
	timespec_to_filetime(&st.st_atim, &info->ftLastAccessTime);
	timespec_to_filetime(&st.st_ctim, &info->ftCreationTime);
	info->dwVolumeSerialNumber = (DWORD)st.st_dev;
	if (!S_ISDIR(st.st_mode)) {
		info->nFileSizeHigh = (DWORD)((ULONGLONG)st.st_size >> 32);
		info->nFileSizeLow = (DWORD)st.st_size;
	}
	info->nNumberOfLinks = (DWORD)st.st_nlink;
	info->nFileIndexHigh = (DWORD)((ULONGLONG)st.st_ino >> 32);
	info->nFileIndexLow = (DWORD)st.st_ino;
	return TRUE;
}

/* The \\?\ path of the file behind a handle, as UTF-8 (malloc()ed). */
static char *final_path(HANDLE h)
{
	struct file *f = file_of(h);
	char *win = NULL, *out = NULL;

	if (!f)
		return NULL;
	if (f->path && f->path[0] == '/')
		win = path_to_windows(f->path);
	kobj_unref(&f->obj);
	if (!win) {
		win_set_error(ERROR_INVALID_HANDLE);
		return NULL;
	}
	if (asprintf(&out, "\\\\?\\%s", win) < 0)
		out = NULL;
	free(win);
	return out;
}

WINFN(DWORD) k32_GetFinalPathNameByHandleA(HANDLE h, LPSTR buf, DWORD size,
					   DWORD flags)
{
	char *p = final_path(h);
	DWORD n;

	if (!p)
		return 0;
	n = copy_out_a(p, buf, size);
	free(p);
	return n;
}

WINFN(DWORD) k32_GetFinalPathNameByHandleW(HANDLE h, LPWSTR buf, DWORD size,
					   DWORD flags)
{
	char *p = final_path(h);
	DWORD n;

	if (!p)
		return 0;
	n = copy_out_w(p, buf, size);
	free(p);
	return n;
}

/* Overlapped I/O is never left pending, so there is nothing to cancel. */
WINFN(BOOL) k32_CancelIoEx(HANDLE h, LPOVERLAPPED o)
{
	win_set_error(ERROR_NOT_FOUND);
	return FALSE;
}

WINFN(BOOL) k32_PeekNamedPipe(HANDLE h, LPVOID buf, DWORD size, LPDWORD read,
			      LPDWORD avail, LPDWORD left)
{
	win_set_error(ERROR_INVALID_FUNCTION);
	return FALSE;
}

/* Console. */

WINFN(BOOL) k32_GetConsoleMode(HANDLE h, LPDWORD mode)
{
	int fd = file_handle_fd(h);

	if (fd < 0 || !isatty(fd)) {
		win_set_error(ERROR_INVALID_HANDLE);
		return FALSE;
	}
	*mode = 3;
	return TRUE;
}

WINFN(BOOL) k32_SetConsoleMode(HANDLE h, DWORD mode) { return TRUE; }
WINFN(UINT) k32_GetConsoleCP(void) { return CODEPAGE_OEM; }
WINFN(UINT) k32_GetConsoleOutputCP(void) { return CODEPAGE_OEM; }
WINFN(BOOL) k32_SetConsoleCtrlHandler(void *handler, BOOL add) { return TRUE; }
WINFN(BOOL) k32_AllocConsole(void) { return TRUE; }
WINFN(BOOL) k32_FreeConsole(void) { return TRUE; }
WINFN(BOOL) k32_SetConsoleTitleA(LPCSTR title) { return TRUE; }

/* There is no console to read from or describe; the callers cope. */
WINFN(BOOL) k32_ReadConsoleW(HANDLE h, LPVOID buf, DWORD n, LPDWORD read,
			     LPVOID control)
{
	win_set_error(ERROR_INVALID_HANDLE);
	return FALSE;
}

WINFN(BOOL) k32_GetConsoleScreenBufferInfo(HANDLE h,
					   PCONSOLE_SCREEN_BUFFER_INFO info)
{
	win_set_error(ERROR_INVALID_HANDLE);
	return FALSE;
}

WINFN(BOOL) k32_SetConsoleTextAttribute(HANDLE h, WORD attrs)
{
	win_set_error(ERROR_INVALID_HANDLE);
	return FALSE;
}

WINFN(BOOL) k32_WriteConsoleA(HANDLE h, const void *buf, DWORD n,
			      LPDWORD written, LPVOID reserved)
{
	return k32_WriteFile(h, buf, n, written, NULL);
}

WINFN(BOOL) k32_WriteConsoleW(HANDLE h, const void *buf, DWORD n,
			      LPDWORD written, LPVOID reserved)
{
	int len = cp_from_utf16(CP_UTF8, buf, n, NULL, 0, '?', NULL);
	char *u = malloc(len + 1);
	DWORD done;
	BOOL ok;

	if (!u) {
		win_set_error(ERROR_NOT_ENOUGH_MEMORY);
		return FALSE;
	}
	cp_from_utf16(CP_UTF8, buf, n, u, len, '?', NULL);
	ok = k32_WriteFile(h, u, len, &done, NULL);
	free(u);
	if (written)
		*written = ok ? n : 0;
	return ok;
}

/* File mappings. */

struct mapping {
	struct kobject obj;
	int fd;
	ULONGLONG size;
	DWORD protect;
};

static void mapping_destroy(struct kobject *o)
{
	struct mapping *m = (struct mapping *)o;

	if (m->fd >= 0)
		close(m->fd);
	free(m);
}

static HANDLE create_mapping(HANDLE file, DWORD protect, DWORD hi, DWORD lo,
			     const char *name)
{
	ULONGLONG size = ((ULONGLONG)hi << 32) | lo;
	struct mapping *m;
	int fd;

	if (name && *name) {
		struct kobject *existing = kobj_find_named(name);
		if (existing) {
			if (existing->type != KOBJ_MAPPING) {
				kobj_unref(existing);
				win_set_error(ERROR_INVALID_HANDLE);
				return NULL;
			}
			HANDLE h = handle_new(existing);
			if (h)
				win_set_error(ERROR_ALREADY_EXISTS);
			return h;
		}
	}

	if (file == INVALID_HANDLE_VALUE) {
		if (!size) {
			win_set_error(ERROR_INVALID_PARAMETER);
			return NULL;
		}
		fd = memfd_create("vfw-mapping", MFD_CLOEXEC);
		if (fd < 0 || ftruncate(fd, size)) {
			if (fd >= 0)
				close(fd);
			return WIN_ERRNO(NULL);
		}
	} else {
		int src = file_handle_fd(file);
		struct stat st;
		if (src < 0)
			return NULL;
		fd = fcntl(src, F_DUPFD_CLOEXEC, 0);
		if (fd < 0 || fstat(fd, &st)) {
			if (fd >= 0)
				close(fd);
			return WIN_ERRNO(NULL);
		}
		if (!size)
			size = st.st_size;
		else if ((ULONGLONG)st.st_size < size &&
			 (protect & 0xff) == PAGE_READWRITE && ftruncate(fd, size)) {
			close(fd);
			return WIN_ERRNO(NULL);
		}
	}

	if (!(m = calloc(1, sizeof(*m)))) {
		close(fd);
		win_set_error(ERROR_NOT_ENOUGH_MEMORY);
		return NULL;
	}
	kobj_init(&m->obj, KOBJ_MAPPING, mapping_destroy);
	m->fd = fd;
	m->size = size;
	m->protect = protect;
	if (name && *name)
		kobj_set_name(&m->obj, name);
	HANDLE h = handle_new(&m->obj);
	if (h)
		win_set_error(ERROR_SUCCESS);
	return h;
}

WINFN(HANDLE) k32_CreateFileMappingA(HANDLE file, LPSECURITY_ATTRIBUTES sa,
				     DWORD protect, DWORD hi, DWORD lo,
				     LPCSTR name)
{
	char *u = acp_to_utf8(name);
	HANDLE h = create_mapping(file, protect, hi, lo, u);
	free(u);
	return h;
}

WINFN(HANDLE) k32_CreateFileMappingW(HANDLE file, LPSECURITY_ATTRIBUTES sa,
				     DWORD protect, DWORD hi, DWORD lo,
				     LPCWSTR name)
{
	char *u = utf16_to_utf8_dup(name);
	HANDLE h = create_mapping(file, protect, hi, lo, u);
	free(u);
	return h;
}

WINFN(HANDLE) k32_OpenFileMappingA(DWORD access, BOOL inherit, LPCSTR name)
{
	char *u = acp_to_utf8(name);
	struct kobject *obj = u ? kobj_find_named(u) : NULL;

	free(u);
	if (!obj || obj->type != KOBJ_MAPPING) {
		kobj_unref(obj);
		win_set_error(ERROR_FILE_NOT_FOUND);
		return NULL;
	}
	return handle_new(obj);
}

struct view {
	void *addr;
	size_t len;
	struct view *next;
};

static struct view *views;
static pthread_mutex_t view_lock = PTHREAD_MUTEX_INITIALIZER;

static LPVOID map_view(HANDLE h, DWORD access, DWORD hi, DWORD lo, SIZE_T bytes,
		       LPVOID base)
{
	struct mapping *m = (struct mapping *)handle_ref(h, KOBJ_MAPPING);
	ULONGLONG off = ((ULONGLONG)hi << 32) | lo;

	if (!m)
		return NULL;
	if (off > m->size || (off & 0xfff)) {
		kobj_unref(&m->obj);
		win_set_error(ERROR_INVALID_PARAMETER);
		return NULL;
	}
	size_t len = bytes ? bytes : (size_t)(m->size - off);
	int prot = PROT_READ;
	int flags = MAP_SHARED;
	if (access & FILE_MAP_WRITE)	/* FILE_MAP_ALL_ACCESS includes it */
		prot |= PROT_WRITE;
	if (access == FILE_MAP_COPY) {
		prot |= PROT_WRITE;
		flags = MAP_PRIVATE;
	}
	if (access & FILE_MAP_EXECUTE)
		prot |= PROT_EXEC;
	if (base)
		flags |= MAP_FIXED_NOREPLACE;
	void *p = mmap(base, len, prot, flags, m->fd, off);
	kobj_unref(&m->obj);
	if (p == MAP_FAILED || (base && p != base)) {
		if (p != MAP_FAILED)
			munmap(p, len);
		return WIN_ERRNO(NULL);
	}

	struct view *v = malloc(sizeof(*v));
	if (!v) {
		munmap(p, len);
		win_set_error(ERROR_NOT_ENOUGH_MEMORY);
		return NULL;
	}
	v->addr = p;
	v->len = len;
	pthread_mutex_lock(&view_lock);
	v->next = views;
	views = v;
	pthread_mutex_unlock(&view_lock);
	return p;
}

WINFN(LPVOID) k32_MapViewOfFile(HANDLE h, DWORD access, DWORD hi, DWORD lo,
				SIZE_T bytes)
{
	return map_view(h, access, hi, lo, bytes, NULL);
}

WINFN(LPVOID) k32_MapViewOfFileEx(HANDLE h, DWORD access, DWORD hi, DWORD lo,
				  SIZE_T bytes, LPVOID base)
{
	return map_view(h, access, hi, lo, bytes, base);
}

WINFN(BOOL) k32_UnmapViewOfFile(LPCVOID addr)
{
	struct view *v = NULL;

	pthread_mutex_lock(&view_lock);
	for (struct view **p = &views; *p; p = &(*p)->next) {
		if ((*p)->addr == addr) {
			v = *p;
			*p = v->next;
			break;
		}
	}
	pthread_mutex_unlock(&view_lock);
	if (!v) {
		win_set_error(ERROR_INVALID_ADDRESS);
		return FALSE;
	}
	munmap(v->addr, v->len);
	free(v);
	return TRUE;
}

WINFN(BOOL) k32_FlushViewOfFile(LPCVOID addr, SIZE_T bytes)
{
	void *start = (void *)((uintptr_t)addr & ~(uintptr_t)0xfff);
	msync(start, bytes ? bytes + ((uintptr_t)addr & 0xfff) : 0x1000, MS_SYNC);
	return TRUE;
}

#define E(name) { #name, 0, (void *)k32_##name }

const struct builtin_export k32_file_exports[] = {
	E(AllocConsole),
	E(CancelIoEx),
	E(CopyFileA),
	E(CopyFileW),
	E(CreateDirectoryA),
	E(CreateDirectoryW),
	E(CreateFileA),
	E(CreateFileMappingA),
	E(CreateFileMappingW),
	E(CreateFileW),
	E(DeleteFileA),
	E(DeleteFileW),
	E(FindClose),
	E(FindFirstFileA),
	E(FindFirstFileExW),
	E(FindFirstFileW),
	E(FindNextFileA),
	E(FindNextFileW),
	E(FlushFileBuffers),
	E(FlushViewOfFile),
	E(FreeConsole),
	E(GetConsoleCP),
	E(GetConsoleMode),
	E(GetConsoleOutputCP),
	E(GetConsoleScreenBufferInfo),
	E(GetCurrentDirectoryA),
	E(GetCurrentDirectoryW),
	E(GetDiskFreeSpaceA),
	E(GetDiskFreeSpaceExA),
	E(GetDriveTypeA),
	E(GetDriveTypeW),
	E(GetFileAttributesA),
	E(GetFileAttributesW),
	E(GetFileInformationByHandle),
	E(GetFileSize),
	E(GetFileSizeEx),
	E(GetFileTime),
	E(GetFileType),
	E(GetFinalPathNameByHandleA),
	E(GetFinalPathNameByHandleW),
	E(GetFullPathNameA),
	E(GetFullPathNameW),
	E(GetLogicalDrives),
	E(GetLongPathNameA),
	E(GetLongPathNameW),
	E(GetShortPathNameA),
	E(GetShortPathNameW),
	E(GetStdHandle),
	E(GetTempFileNameA),
	E(GetTempFileNameW),
	E(GetTempPathA),
	E(GetTempPathW),
	E(GetVolumeInformationA),
	E(LockFile),
	E(MapViewOfFile),
	E(MapViewOfFileEx),
	E(MoveFileA),
	E(MoveFileExA),
	E(MoveFileExW),
	E(MoveFileW),
	E(OpenFileMappingA),
	E(PeekNamedPipe),
	E(ReadConsoleW),
	E(ReadFile),
	E(RemoveDirectoryA),
	E(RemoveDirectoryW),
	E(SetConsoleCtrlHandler),
	E(SetConsoleMode),
	E(SetConsoleTextAttribute),
	E(SetConsoleTitleA),
	E(SetCurrentDirectoryA),
	E(SetCurrentDirectoryW),
	E(SetEndOfFile),
	E(SetFileAttributesA),
	E(SetFileAttributesW),
	E(SetFilePointer),
	E(SetFilePointerEx),
	E(SetFileTime),
	E(SetStdHandle),
	E(UnlockFile),
	E(UnmapViewOfFile),
	E(WriteConsoleA),
	E(WriteConsoleW),
	E(WriteFile),
	BUILTIN_END
};
