/*
 * shell32: SHFileOperation, SHCreateDirectoryEx and SHGetFileInfo.
 *
 * SHFileOperation copies, moves, renames and deletes lists of files and
 * directories (whole trees), with wildcards in the last component, and
 * never asks: with no UI, every confirmation is answered "Yes to all", so
 * existing files are replaced unless FOF_RENAMEONCOLLISION says to make
 * "Copy of" names instead. There is no Recycle Bin (FOF_ALLOWUNDO deletes
 * outright). It returns 0, or a Win32 error code for the first failure,
 * leaving what was done before it done.
 */
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <fnmatch.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <unistd.h>

#include "loader/pe.h"
#include "win32/codepage.h"
#include "win32/internal.h"
#include "win32/path.h"
#include "win32/shell.h"

/* A double-NUL-terminated list of paths as UTF-8 strings. */
struct list {
	char **item;
	int n;
};

static void list_free(struct list *l)
{
	for (int i = 0; i < l->n; i++)
		free(l->item[i]);
	free(l->item);
	l->item = NULL;
	l->n = 0;
}

static int list_add(struct list *l, char *s)
{
	char **item = s ? realloc(l->item, (l->n + 1) * sizeof(*item)) : NULL;

	if (!item) {
		free(s);
		return -1;
	}
	l->item = item;
	l->item[l->n++] = s;
	return 0;
}

static int list_read(struct list *l, const void *p, int wide)
{
	if (!p)
		return 0;
	if (wide) {
		for (const WCHAR *w = p; *w; w += wstrlen(w) + 1)
			if (list_add(l, utf16_to_utf8_dup(w)))
				return -1;
	} else {
		for (const char *a = p; *a; a += strlen(a) + 1)
			if (list_add(l, acp_to_utf8(a)))
				return -1;
	}
	return 0;
}

static int is_dir(const char *unix)
{
	struct stat st;
	return !stat(unix, &st) && S_ISDIR(st.st_mode);
}

static const char *base_name(const char *path)
{
	const char *b = path;

	for (const char *p = path; *p; p++)
		if (*p == '/' || *p == '\\')
			b = p + 1;
	return b;
}

static char *join(const char *dir, const char *name)
{
	size_t n = strlen(dir);
	char *s = malloc(n + strlen(name) + 2);

	if (s)
		sprintf(s, "%s%s%s", dir, n && dir[n - 1] == '/' ? "" : "/",
			name);
	return s;
}

/*
 * Expands a Windows path whose last component may hold * or ? into the
 * Unix paths it names, matched as Windows does, ignoring case ("*.*" is
 * everything). A path without wildcards is added as it is, existing or
 * not. Returns a Win32 error code.
 */
static DWORD expand(const char *win, int files_only, struct list *out)
{
	const char *last = base_name(win);

	if (!strpbrk(last, "*?")) {
		char *u = path_to_unix(win);
		return u ? (list_add(out, u) ? ERROR_NOT_ENOUGH_MEMORY : 0) :
			   ERROR_PATH_NOT_FOUND;
	}
	char *dir_win = strndup(win, (size_t)(last - win));
	char *dir = dir_win ? path_to_unix(*dir_win ? dir_win : ".") : NULL;
	free(dir_win);
	DIR *d = dir ? opendir(dir) : NULL;
	if (!d) {
		free(dir);
		return ERROR_PATH_NOT_FOUND;
	}
	const char *pat = strcmp(last, "*.*") ? last : "*";
	struct dirent *e;
	DWORD err = 0;
	while (!err && (e = readdir(d))) {
		if (!strcmp(e->d_name, ".") || !strcmp(e->d_name, ".."))
			continue;
		if (fnmatch(pat, e->d_name, FNM_CASEFOLD))
			continue;
		char *p = join(dir, e->d_name);
		if (p && files_only && is_dir(p)) {
			free(p);
			continue;
		}
		if (list_add(out, p))
			err = ERROR_NOT_ENOUGH_MEMORY;
	}
	closedir(d);
	free(dir);
	return err;
}

static DWORD errno_error(void)
{
	return win_error_from_errno(errno);
}

static DWORD delete_tree(const char *path)
{
	struct stat st;

	if (lstat(path, &st))
		return errno == ENOENT ? ERROR_FILE_NOT_FOUND : errno_error();
	if (!S_ISDIR(st.st_mode))
		return unlink(path) ? errno_error() : 0;
	DIR *d = opendir(path);
	if (!d)
		return errno_error();
	struct dirent *e;
	DWORD err = 0;
	while (!err && (e = readdir(d))) {
		if (!strcmp(e->d_name, ".") || !strcmp(e->d_name, ".."))
			continue;
		char *p = join(path, e->d_name);
		err = p ? delete_tree(p) : ERROR_NOT_ENOUGH_MEMORY;
		free(p);
	}
	closedir(d);
	return err ? err : rmdir(path) ? errno_error() : 0;
}

static DWORD copy_one(const char *from, const char *to)
{
	int in = open(from, O_RDONLY | O_CLOEXEC), out = -1;
	DWORD err = 0;
	char buf[65536];
	ssize_t n;

	if (in < 0)
		return errno == ENOENT ? ERROR_FILE_NOT_FOUND : errno_error();
	out = open(to, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0644);
	if (out < 0) {
		err = errno_error();
		close(in);
		return err;
	}
	while ((n = read(in, buf, sizeof(buf))) > 0)
		if (write(out, buf, (size_t)n) != n) {
			n = -1;
			break;
		}
	if (n < 0)
		err = errno_error();
	close(in);
	if (close(out) && !err)
		err = errno_error();
	return err;
}

static DWORD copy_tree(const char *from, const char *to, int recurse)
{
	if (!is_dir(from))
		return copy_one(from, to);
	if (mkdir(to, 0755) && errno != EEXIST)
		return errno_error();
	if (!recurse)
		return 0;
	DIR *d = opendir(from);
	if (!d)
		return errno_error();
	struct dirent *e;
	DWORD err = 0;
	while (!err && (e = readdir(d))) {
		if (!strcmp(e->d_name, ".") || !strcmp(e->d_name, ".."))
			continue;
		char *a = join(from, e->d_name), *b = join(to, e->d_name);
		err = a && b ? copy_tree(a, b, 1) : ERROR_NOT_ENOUGH_MEMORY;
		free(a);
		free(b);
	}
	closedir(d);
	return err;
}

/* "Copy of x", "Copy (2) of x", ... in to's directory, not yet taken. */
static char *collision_name(const char *to)
{
	const char *name = base_name(to);
	size_t dir_len = (size_t)(name - to);

	for (int i = 1; i < 1000; i++) {
		char *s = malloc(dir_len + strlen(name) + 32);
		if (!s)
			return NULL;
		memcpy(s, to, dir_len);
		if (i == 1)
			sprintf(s + dir_len, "Copy of %s", name);
		else
			sprintf(s + dir_len, "Copy (%d) of %s", i, name);
		if (access(s, F_OK))
			return s;
		free(s);
	}
	return NULL;
}

/* Copies or moves one source to one destination path. */
static DWORD transfer(const char *from, const char *to, int move,
		      FILEOP_FLAGS flags)
{
	char *dest = NULL;
	DWORD err;

	if (access(from, F_OK))
		return ERROR_FILE_NOT_FOUND;
	if (!strcmp(from, to)) {
		/* Onto itself: only a copy under a new name makes sense. */
		if (move || !(flags & FOF_RENAMEONCOLLISION))
			return ERROR_ALREADY_EXISTS;
		if (!(dest = collision_name(to)))
			return ERROR_NOT_ENOUGH_MEMORY;
		err = copy_tree(from, dest, !(flags & FOF_NORECURSION));
		free(dest);
		return err;
	}
	/* Nor into its own subtree. */
	size_t n = strlen(from);
	if (!strncmp(to, from, n) && to[n] == '/')
		return ERROR_INVALID_PARAMETER;
	if ((flags & FOF_RENAMEONCOLLISION) && !access(to, F_OK)) {
		if (!(dest = collision_name(to)))
			return ERROR_NOT_ENOUGH_MEMORY;
		to = dest;
	} else if (!access(to, F_OK) && is_dir(to) != is_dir(from)) {
		return ERROR_ALREADY_EXISTS;
	} else if (!access(to, F_OK) && !is_dir(to)) {
		unlink(to);	/* "Yes to all" */
	}

	if (move && !rename(from, to)) {
		err = 0;
	} else if (move && errno != EXDEV && errno != ENOTEMPTY &&
		   errno != EEXIST) {
		err = errno_error();
	} else {
		err = copy_tree(from, to, !(flags & FOF_NORECURSION) || move);
		if (!err && move)
			err = delete_tree(from);
	}
	free(dest);
	return err;
}

static DWORD file_op(UINT func, const struct list *from,
		     const struct list *to, FILEOP_FLAGS flags)
{
	struct list src = { 0 };
	DWORD err = 0;

	if (!from->n)
		return ERROR_INVALID_PARAMETER;

	if (func == FO_RENAME) {
		if (from->n != 1 || to->n != 1)
			return ERROR_INVALID_PARAMETER;
		char *a = path_to_unix(from->item[0]);
		char *b = path_to_unix(to->item[0]);
		if (!a || !b)
			err = ERROR_PATH_NOT_FOUND;
		else if (access(a, F_OK))
			err = ERROR_FILE_NOT_FOUND;
		else if (!access(b, F_OK) && strcmp(a, b))
			err = ERROR_ALREADY_EXISTS;
		else if (rename(a, b))
			err = errno_error();
		free(a);
		free(b);
		return err;
	}

	int wild = 0;
	for (int i = 0; !err && i < from->n; i++) {
		wild |= strpbrk(base_name(from->item[i]), "*?") != NULL;
		err = expand(from->item[i],
			     (flags & FOF_FILESONLY) &&
			     strpbrk(base_name(from->item[i]), "*?"), &src);
	}
	if (err) {
		list_free(&src);
		return err;
	}

	if (func == FO_DELETE) {
		for (int i = 0; !err && i < src.n; i++)
			err = delete_tree(src.item[i]);
		list_free(&src);
		return err;
	}
	if (func != FO_COPY && func != FO_MOVE) {
		list_free(&src);
		return ERROR_INVALID_PARAMETER;
	}
	if (!to->n) {
		list_free(&src);
		return ERROR_INVALID_PARAMETER;
	}

	int move = func == FO_MOVE;
	if (flags & FOF_MULTIDESTFILES && to->n == from->n && !wild) {
		/* Each source to its own destination. */
		for (int i = 0; !err && i < src.n; i++) {
			char *d = path_to_unix(to->item[i]);
			err = d ? transfer(src.item[i], d, move, flags) :
				  ERROR_PATH_NOT_FOUND;
			free(d);
		}
	} else {
		/*
		 * Into a directory: one that exists, or one made for several
		 * sources or a wildcard. A single source to a path that isn't
		 * a directory is copied to that name.
		 */
		char *dest = path_to_unix(to->item[0]);
		if (!dest) {
			list_free(&src);
			return ERROR_PATH_NOT_FOUND;
		}
		int into = is_dir(dest) || src.n > 1 || wild;
		if (into && !is_dir(dest) && shell_mkdirs(dest))
			err = errno_error();
		for (int i = 0; !err && i < src.n; i++) {
			char *d = into ? join(dest, base_name(src.item[i])) :
				  strdup(dest);
			err = d ? transfer(src.item[i], d, move, flags) :
				  ERROR_NOT_ENOUGH_MEMORY;
			free(d);
		}
		free(dest);
	}
	list_free(&src);
	return err;
}

WINFN(int) sh_SHFileOperationA(SHFILEOPSTRUCTA *op)
{
	struct list from = { 0 }, to = { 0 };

	if (!op)
		return ERROR_INVALID_PARAMETER;
	op->fAnyOperationsAborted = FALSE;
	op->hNameMappings = NULL;
	DWORD err = list_read(&from, op->pFrom, 0) || list_read(&to, op->pTo, 0) ?
		    ERROR_NOT_ENOUGH_MEMORY : file_op(op->wFunc, &from, &to,
						      op->fFlags);
	list_free(&from);
	list_free(&to);
	return (int)err;
}

WINFN(int) sh_SHFileOperationW(SHFILEOPSTRUCTW *op)
{
	struct list from = { 0 }, to = { 0 };

	if (!op)
		return ERROR_INVALID_PARAMETER;
	op->fAnyOperationsAborted = FALSE;
	op->hNameMappings = NULL;
	DWORD err = list_read(&from, op->pFrom, 1) || list_read(&to, op->pTo, 1) ?
		    ERROR_NOT_ENOUGH_MEMORY : file_op(op->wFunc, &from, &to,
						      op->fFlags);
	list_free(&from);
	list_free(&to);
	return (int)err;
}

/*
 * SHCreateDirectoryEx: a full path, with any missing parents.
 * ERROR_ALREADY_EXISTS if it is already there (ERROR_FILE_EXISTS if as a
 * file), ERROR_BAD_PATHNAME for a relative path.
 */
static int create_directory(const char *win)
{
	if (!win || !((win[0] && win[1] == ':' && (win[2] == '\\' || win[2] == '/')) ||
		      (win[0] == '\\' && win[1] == '\\'))) {
		win_set_error(ERROR_BAD_PATHNAME);
		return ERROR_BAD_PATHNAME;
	}
	char *u = path_to_unix(win);
	int r = ERROR_SUCCESS;
	if (!u)
		r = ERROR_PATH_NOT_FOUND;
	else if (!access(u, F_OK))
		r = is_dir(u) ? ERROR_ALREADY_EXISTS : ERROR_FILE_EXISTS;
	else if (shell_mkdirs(u))
		r = (int)errno_error();
	free(u);
	win_set_error((DWORD)r);
	return r;
}

WINFN(int) sh_SHCreateDirectoryExA(HWND hwnd, LPCSTR path, void *sa)
{
	char *u = acp_to_utf8(path);
	int r = create_directory(u);
	free(u);
	return r;
}

WINFN(int) sh_SHCreateDirectoryExW(HWND hwnd, LPCWSTR path, void *sa)
{
	char *u = utf16_to_utf8_dup(path);
	int r = create_directory(u);
	free(u);
	return r;
}

WINFN(int) sh_SHCreateDirectory(HWND hwnd, LPCWSTR path)
{
	return sh_SHCreateDirectoryExW(hwnd, path, NULL);
}

/*
 * SHGetFileInfo: attributes, display and type names as XP gives them
 * ("File Folder", "Application", "TXT File"), no icons (hIcon NULL, index
 * 0), and SHGFI_EXETYPE from the PE header.
 */
struct file_info {
	DWORD attributes;	/* SFGAO_* */
	char display[MAX_PATH];
	char type[80];
};

static DWORD exe_type(const char *unix)
{
	struct pe_image *img = pe_open(unix);
	DWORD r = 0;

	if (img && !(img->nt.FileHeader.Characteristics & IMAGE_FILE_DLL)) {
		const IMAGE_OPTIONAL_HEADER32 *o = &img->nt.OptionalHeader;
		/* "PE", and for GUI programs the subsystem version. */
		r = 0x4550;
		if (o->Subsystem == IMAGE_SUBSYSTEM_WINDOWS_GUI)
			r |= (DWORD)(o->MajorSubsystemVersion << 8 |
				     o->MinorSubsystemVersion) << 16;
	}
	pe_free(img);
	return r;
}

static DWORD_PTR file_info(const char *win, DWORD attrs, UINT flags,
			   struct file_info *fi)
{
	char *u = path_to_unix(win);
	struct stat st;
	int dir;

	memset(fi, 0, sizeof(*fi));
	if (!u)
		return 0;
	if (flags & SHGFI_USEFILEATTRIBUTES) {
		dir = !!(attrs & FILE_ATTRIBUTE_DIRECTORY);
		st.st_mode = dir ? S_IFDIR | 0755 : S_IFREG | 0644;
	} else if (stat(u, &st)) {
		free(u);
		return 0;
	} else {
		dir = S_ISDIR(st.st_mode);
	}

	if (flags & SHGFI_EXETYPE) {
		DWORD r = dir ? 0 : exe_type(u);
		free(u);
		return r;
	}
	free(u);

	fi->attributes = SFGAO_FILESYSTEM | SFGAO_CANCOPY | SFGAO_CANMOVE |
			 SFGAO_CANRENAME | SFGAO_CANDELETE | SFGAO_CANLINK;
	fi->attributes |= dir ? SFGAO_FOLDER | SFGAO_FILESYSANCESTOR |
			  SFGAO_HASSUBFOLDER : SFGAO_STREAM;
	if (!(st.st_mode & 0200))
		fi->attributes |= SFGAO_READONLY;

	const char *name = base_name(win);
	snprintf(fi->display, sizeof(fi->display), "%s", *name ? name : win);

	const char *dot = dir ? NULL : strrchr(name, '.');
	if (dir)
		snprintf(fi->type, sizeof(fi->type), "File Folder");
	else if (dot && !strcasecmp(dot, ".exe"))
		snprintf(fi->type, sizeof(fi->type), "Application");
	else if (dot && !strcasecmp(dot, ".dll"))
		snprintf(fi->type, sizeof(fi->type), "Application Extension");
	else if (dot && dot[1]) {
		char ext[64];
		size_t i = 0;
		for (const char *p = dot + 1; *p && i + 1 < sizeof(ext); p++)
			ext[i++] = *p >= 'a' && *p <= 'z' ? *p - 32 : *p;
		ext[i] = 0;
		snprintf(fi->type, sizeof(fi->type), "%s File", ext);
	} else {
		snprintf(fi->type, sizeof(fi->type), "File");
	}
	return 1;
}

static void text_out(const char *s, void *buf, size_t chars, int wide)
{
	if (wide) {
		if (cp_to_utf16(CP_UTF8, s, -1, buf, (int)chars, 0) < 0)
			((WCHAR *)buf)[0] = 0;
		return;
	}
	char *a = utf8_to_acp_dup(s);
	snprintf(buf, chars, "%s", a ? a : "");
	free(a);
}

static DWORD_PTR get_file_info(const void *path, DWORD attrs, void *out,
			       UINT cb, UINT flags, int wide)
{
	char *win = (flags & SHGFI_PIDL) ? shell_pidl_path(path) :
		    wide ? utf16_to_utf8_dup(path) : acp_to_utf8(path);
	struct file_info fi;
	DWORD_PTR r = win ? file_info(win, attrs, flags, &fi) : 0;

	free(win);
	if (!r || (flags & SHGFI_EXETYPE) || !out)
		return r;
	if (cb < (wide ? sizeof(SHFILEINFOW) : sizeof(SHFILEINFOA)))
		return 0;

	if (wide) {
		SHFILEINFOW *w = out;
		if (flags & (SHGFI_ICON | SHGFI_SYSICONINDEX | SHGFI_ICONLOCATION)) {
			w->hIcon = NULL;
			w->iIcon = 0;
		}
		if (flags & SHGFI_ATTRIBUTES)
			w->dwAttributes = fi.attributes;
		if (flags & SHGFI_DISPLAYNAME)
			text_out(fi.display, w->szDisplayName, MAX_PATH, 1);
		if (flags & SHGFI_TYPENAME)
			text_out(fi.type, w->szTypeName, 80, 1);
	} else {
		SHFILEINFOA *a = out;
		if (flags & (SHGFI_ICON | SHGFI_SYSICONINDEX | SHGFI_ICONLOCATION)) {
			a->hIcon = NULL;
			a->iIcon = 0;
		}
		if (flags & SHGFI_ATTRIBUTES)
			a->dwAttributes = fi.attributes;
		if (flags & SHGFI_DISPLAYNAME)
			text_out(fi.display, a->szDisplayName, MAX_PATH, 0);
		if (flags & SHGFI_TYPENAME)
			text_out(fi.type, a->szTypeName, 80, 0);
	}
	return r;
}

WINFN(DWORD_PTR) sh_SHGetFileInfoA(LPCSTR path, DWORD attrs, SHFILEINFOA *out,
				   UINT cb, UINT flags)
{
	return get_file_info(path, attrs, out, cb, flags, 0);
}

WINFN(DWORD_PTR) sh_SHGetFileInfoW(LPCWSTR path, DWORD attrs, SHFILEINFOW *out,
				   UINT cb, UINT flags)
{
	return get_file_info(path, attrs, out, cb, flags, 1);
}

#define E(name) { #name, 0, (void *)sh_##name }

const struct builtin_export shell_file_exports[] = {
	E(SHCreateDirectory), E(SHCreateDirectoryExA), E(SHCreateDirectoryExW),
	E(SHFileOperationA), E(SHFileOperationW),
	E(SHGetFileInfoA), E(SHGetFileInfoW),
	BUILTIN_END
};
