/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * shell32's Recycle Bin, as on XP: C:\RECYCLER\<the user's SID>. A file or
 * directory deleted with FOF_ALLOWUNDO is moved there as Dc<n> with its
 * extension kept (n counting up), and its original path is recorded in
 * the bin's index, seance's own text file "INFO" (Windows keeps a binary
 * INFO2 instead), one "Dc<n>.ext<TAB>original path" line per item.
 * SHQueryRecycleBin counts and sizes what is there, SHEmptyRecycleBin
 * deletes it, and the bin's folder (shell_folder.c) lists it by the
 * original names.
 */
#include <dirent.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <unistd.h>

#include "win32/codepage.h"
#include "win32/internal.h"
#include "win32/path.h"
#include "win32/shell.h"

/* The user's SID, as advapi32 makes it (one machine, user 1000). */
#define BIN "C:\\RECYCLER\\S-1-5-21-1004336348-1177238915-682003330-1000"
#define INDEX "INFO"

char *shell_recycle_dir(void)
{
	char *u = path_to_unix(BIN);
	int bad = !u || shell_mkdirs(u);

	free(u);
	return bad ? NULL : strdup(BIN);
}

/* The bin's Unix directory, made if missing. */
static char *bin_unix(void)
{
	char *u = path_to_unix(BIN);
	if (u && shell_mkdirs(u)) {
		free(u);
		return NULL;
	}
	return u;
}

static char *join2(const char *a, const char *b)
{
	char *p = malloc(strlen(a) + strlen(b) + 2);
	if (p)
		sprintf(p, "%s/%s", a, b);
	return p;
}

char *shell_recycle_original(const char *name)
{
	char *dir = bin_unix(), *idx = dir ? join2(dir, INDEX) : NULL;
	FILE *f = idx ? fopen(idx, "r") : NULL;
	char line[4096], *found = NULL;
	size_t n = strlen(name);

	while (f && !found && fgets(line, sizeof(line), f)) {
		line[strcspn(line, "\n")] = 0;
		if (!strncasecmp(line, name, n) && line[n] == '\t')
			found = strdup(line + n + 1);
	}
	if (f)
		fclose(f);
	free(idx);
	free(dir);
	return found;
}

DWORD shell_recycle(const char *unix_path)
{
	struct stat st;

	if (lstat(unix_path, &st))
		return errno == ENOENT ? ERROR_FILE_NOT_FOUND :
					 win_error_from_errno(errno);
	char *dir = bin_unix();
	char *orig = path_to_windows(unix_path);
	if (!dir || !orig) {
		free(dir);
		free(orig);
		return ERROR_PATH_NOT_FOUND;
	}
	/* The extension of a file (not a directory) is kept. */
	const char *base = strrchr(unix_path, '/');
	base = base ? base + 1 : unix_path;
	const char *dot = S_ISDIR(st.st_mode) ? NULL : strrchr(base, '.');
	char name[64], *dest = NULL;
	for (unsigned i = 1; i < 100000; i++) {
		snprintf(name, sizeof(name), "Dc%u%.16s", i, dot ? dot : "");
		free(dest);
		dest = join2(dir, name);
		if (!dest || access(dest, F_OK))
			break;
	}
	DWORD err = 0;
	if (!dest)
		err = ERROR_NOT_ENOUGH_MEMORY;
	else if (rename(unix_path, dest))
		err = win_error_from_errno(errno);
	if (!err) {
		char *idx = join2(dir, INDEX);
		FILE *f = idx ? fopen(idx, "a") : NULL;
		if (f) {
			fprintf(f, "%s\t%s\n", name, orig);
			fclose(f);
		}
		free(idx);
	}
	free(dest);
	free(orig);
	free(dir);
	return err;
}

/* Bytes under path, all the way down. */
static long long tree_size(const char *path, long long *items)
{
	struct stat st;
	long long n = 0;

	if (lstat(path, &st))
		return 0;
	if (!S_ISDIR(st.st_mode))
		return st.st_size;
	DIR *d = opendir(path);
	struct dirent *e;
	while (d && (e = readdir(d))) {
		if (!strcmp(e->d_name, ".") || !strcmp(e->d_name, ".."))
			continue;
		char *p = join2(path, e->d_name);
		if (p)
			n += tree_size(p, NULL);
		free(p);
	}
	if (d)
		closedir(d);
	(void)items;
	return n;
}

static void remove_tree(const char *path)
{
	struct stat st;

	if (lstat(path, &st))
		return;
	if (S_ISDIR(st.st_mode)) {
		DIR *d = opendir(path);
		struct dirent *e;
		while (d && (e = readdir(d))) {
			if (!strcmp(e->d_name, ".") || !strcmp(e->d_name, ".."))
				continue;
			char *p = join2(path, e->d_name);
			if (p)
				remove_tree(p);
			free(p);
		}
		if (d)
			closedir(d);
		rmdir(path);
	} else {
		unlink(path);
	}
}

/*
 * Walks the bin's items (not its index); with empty, deletes them. A
 * root other than drive C: (where the only bin is) has none.
 */
static int walk_bin(const char *root, long long *size, long long *items,
		    int empty)
{
	*size = *items = 0;
	if (root && *root && (root[0] | 0x20) != 'c')
		return 0;
	char *dir = bin_unix();
	if (!dir)
		return -1;
	DIR *d = opendir(dir);
	struct dirent *e;
	while (d && (e = readdir(d))) {
		if (!strcmp(e->d_name, ".") || !strcmp(e->d_name, "..") ||
		    !strcmp(e->d_name, INDEX))
			continue;
		char *p = join2(dir, e->d_name);
		if (!p)
			continue;
		(*items)++;
		*size += tree_size(p, NULL);
		if (empty)
			remove_tree(p);
		free(p);
	}
	if (d)
		closedir(d);
	if (empty) {
		char *idx = join2(dir, INDEX);
		if (idx)
			unlink(idx);
		free(idx);
	}
	free(dir);
	return 0;
}

static HRESULT query_bin(const char *root, SHQUERYRBINFO *info)
{
	if (!info || info->cbSize != sizeof(*info))
		return E_INVALIDARG;
	long long size, items;
	if (walk_bin(root, &size, &items, 0))
		return E_FAIL;
	info->i64Size = size;
	info->i64NumItems = items;
	return S_OK;
}

WINFN(HRESULT) sh_SHQueryRecycleBinA(LPCSTR root, LPSHQUERYRBINFO info)
{
	char *r = root ? acp_to_utf8(root) : NULL;
	HRESULT hr = query_bin(r, info);
	free(r);
	return hr;
}

WINFN(HRESULT) sh_SHQueryRecycleBinW(LPCWSTR root, LPSHQUERYRBINFO info)
{
	char *r = root ? utf16_to_utf8_dup(root) : NULL;
	HRESULT hr = query_bin(r, info);
	free(r);
	return hr;
}

static HRESULT empty_bin(const char *root)
{
	long long size, items;
	return walk_bin(root, &size, &items, 1) ? E_FAIL : S_OK;
}

WINFN(HRESULT) sh_SHEmptyRecycleBinA(HWND hwnd, LPCSTR root, DWORD flags)
{
	char *r = root ? acp_to_utf8(root) : NULL;
	HRESULT hr = empty_bin(r);
	free(r);
	return hr;
}

WINFN(HRESULT) sh_SHEmptyRecycleBinW(HWND hwnd, LPCWSTR root, DWORD flags)
{
	char *r = root ? utf16_to_utf8_dup(root) : NULL;
	HRESULT hr = empty_bin(r);
	free(r);
	return hr;
}

#define E(name) { #name, 0, (void *)sh_##name }

const struct builtin_export shell_recycle_exports[] = {
	E(SHEmptyRecycleBinA),
	E(SHEmptyRecycleBinW),
	E(SHQueryRecycleBinA),
	E(SHQueryRecycleBinW),
	BUILTIN_END
};
