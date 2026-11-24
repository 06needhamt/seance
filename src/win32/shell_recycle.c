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
#include <pthread.h>
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

/* Moving an item in and recording it, and emptying, go one at a time. */
static pthread_mutex_t bin_lock = PTHREAD_MUTEX_INITIALIZER;

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
		/* Names are made unique as spelled (Dc1.TXT and Dc1.txt can
		 * both be there on a case-sensitive host). */
		if (!strncmp(line, name, n) && line[n] == '\t')
			found = strdup(line + n + 1);
	}
	if (f)
		fclose(f);
	free(idx);
	free(dir);
	return found;
}

static int dir_empty(const char *path)
{
	DIR *d = opendir(path);
	struct dirent *e;
	int empty = 1;

	while (d && empty && (e = readdir(d)))
		if (strcmp(e->d_name, ".") && strcmp(e->d_name, ".."))
			empty = 0;
	if (d)
		closedir(d);
	return empty;
}

/* Appends name's record to the index; 0 or a Win32 error. */
static DWORD index_add(const char *dir, const char *name, const char *orig)
{
	char *idx = join2(dir, INDEX);
	FILE *f = idx ? fopen(idx, "a") : NULL;
	DWORD err = 0;

	if (!idx)
		err = ERROR_NOT_ENOUGH_MEMORY;
	else if (!f)
		err = win_error_from_errno(errno);
	else {
		if (fprintf(f, "%s\t%s\n", name, orig) < 0)
			err = ERROR_GEN_FAILURE;
		if (fclose(f) && !err)
			err = ERROR_GEN_FAILURE;
	}
	free(idx);
	return err;
}

DWORD shell_recycle(const char *unix_path, int norecurse)
{
	struct stat st;

	if (lstat(unix_path, &st))
		return errno == ENOENT ? ERROR_FILE_NOT_FOUND :
					 win_error_from_errno(errno);
	/* FOF_NORECURSION: a directory with something in it stays. */
	if (norecurse && S_ISDIR(st.st_mode) && !dir_empty(unix_path))
		return ERROR_DIR_NOT_EMPTY;
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
	DWORD err = ERROR_CANNOT_MAKE;

	pthread_mutex_lock(&bin_lock);
	/* The first free Dc<n>, taken by the move itself, so nothing that
	 * appears there meanwhile is replaced. */
	for (unsigned i = 1; i < 100000; i++) {
		snprintf(name, sizeof(name), "Dc%u%.16s", i, dot ? dot : "");
		free(dest);
		dest = join2(dir, name);
		if (!dest) {
			err = ERROR_NOT_ENOUGH_MEMORY;
			break;
		}
		if (!path_rename_noreplace(unix_path, dest)) {
			err = 0;
			break;
		}
		if (errno != EEXIST && errno != ENOTEMPTY) {
			err = win_error_from_errno(errno);
			break;
		}
	}
	if (!err && (err = index_add(dir, name, orig)) != 0) {
		/* Unrecorded, it couldn't be told apart in the bin: put it
		 * back, unless something has taken its place. */
		path_rename_noreplace(dest, unix_path);
	}
	pthread_mutex_unlock(&bin_lock);
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

/* Deletes path and all below it; -1 if any of it is left. */
static int remove_tree(const char *path)
{
	struct stat st;
	int bad = 0;

	if (lstat(path, &st))
		return errno == ENOENT ? 0 : -1;
	if (S_ISDIR(st.st_mode)) {
		DIR *d = opendir(path);
		struct dirent *e;
		if (!d)
			bad = 1;
		while (d && (e = readdir(d))) {
			if (!strcmp(e->d_name, ".") || !strcmp(e->d_name, ".."))
				continue;
			char *p = join2(path, e->d_name);
			if (!p || remove_tree(p))
				bad = 1;
			free(p);
		}
		if (d)
			closedir(d);
		if (rmdir(path))
			bad = 1;
	} else if (unlink(path)) {
		bad = 1;
	}
	return bad ? -1 : 0;
}

/* Keeps only the index records of items still in the bin; with none
 * left, the index goes. */
static void index_prune(const char *dir)
{
	char *idx = join2(dir, INDEX), *tmp = idx ? join2(dir, INDEX ".new") : NULL;
	FILE *in = tmp ? fopen(idx, "r") : NULL, *out = NULL;
	char line[4096];
	int kept = 0, bad = 0;

	if (in)
		out = fopen(tmp, "w");
	while (in && out && fgets(line, sizeof(line), in)) {
		char *tab = strchr(line, '\t');
		if (!tab)
			continue;
		*tab = 0;
		char *p = join2(dir, line);
		*tab = '\t';
		struct stat st;
		if (p && !lstat(p, &st)) {
			kept++;
			if (fputs(line, out) < 0)
				bad = 1;
		}
		free(p);
	}
	if (in)
		fclose(in);
	if (out && fclose(out))
		bad = 1;
	if (idx && in && out && !bad) {
		if (kept)
			rename(tmp, idx);
		else {
			unlink(tmp);
			unlink(idx);
		}
	} else if (tmp) {
		unlink(tmp);
	}
	free(tmp);
	free(idx);
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
	int bad = 0;
	pthread_mutex_lock(&bin_lock);
	DIR *d = opendir(dir);
	struct dirent *e;
	if (!d)
		bad = 1;
	errno = 0;
	while (d && (e = readdir(d))) {
		if (!strcmp(e->d_name, ".") || !strcmp(e->d_name, "..") ||
		    !strcmp(e->d_name, INDEX) ||
		    !strcmp(e->d_name, INDEX ".new"))
			continue;
		char *p = join2(dir, e->d_name);
		if (!p) {
			bad = 1;
			continue;
		}
		(*items)++;
		*size += tree_size(p, NULL);
		if (empty && remove_tree(p))
			bad = 1;
		free(p);
		errno = 0;
	}
	if (d && errno)
		bad = 1;	/* readdir failed part way */
	if (d)
		closedir(d);
	/* What couldn't be deleted keeps its record. */
	if (empty)
		index_prune(dir);
	pthread_mutex_unlock(&bin_lock);
	free(dir);
	return bad ? -1 : 0;
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
	if (root && !r)
		return E_OUTOFMEMORY;	/* not "all drives" */
	HRESULT hr = query_bin(r, info);
	free(r);
	return hr;
}

WINFN(HRESULT) sh_SHQueryRecycleBinW(LPCWSTR root, LPSHQUERYRBINFO info)
{
	char *r = root ? utf16_to_utf8_dup(root) : NULL;
	if (root && !r)
		return E_OUTOFMEMORY;	/* not "all drives" */
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
	if (root && !r)
		return E_OUTOFMEMORY;	/* not "all drives" */
	HRESULT hr = empty_bin(r);
	free(r);
	return hr;
}

WINFN(HRESULT) sh_SHEmptyRecycleBinW(HWND hwnd, LPCWSTR root, DWORD flags)
{
	char *r = root ? utf16_to_utf8_dup(root) : NULL;
	if (root && !r)
		return E_OUTOFMEMORY;	/* not "all drives" */
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
