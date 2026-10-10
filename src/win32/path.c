/* SPDX-License-Identifier: GPL-2.0-only */
#include <dirent.h>
#include <errno.h>
#include <limits.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <unistd.h>

#include "win32/path.h"

static char drive_c[PATH_MAX];
static pthread_once_t once = PTHREAD_ONCE_INIT;

static void mkdirs(const char *path)
{
	char tmp[PATH_MAX];

	snprintf(tmp, sizeof(tmp), "%s", path);
	for (char *p = tmp + 1; *p; p++) {
		if (*p == '/') {
			*p = '\0';
			mkdir(tmp, 0755);
			*p = '/';
		}
	}
	mkdir(tmp, 0755);
}

static void init_root(void)
{
	const char *root = getenv("VFW_ROOT");
	const char *home = getenv("HOME");
	char dir[PATH_MAX + 32];

	if (root && *root)
		snprintf(drive_c, sizeof(drive_c), "%s/drive_c", root);
	else
		snprintf(drive_c, sizeof(drive_c), "%s/.seance/drive_c",
			 home && *home ? home : "/tmp");
	snprintf(dir, sizeof(dir), "%s/windows/system32", drive_c);
	mkdirs(dir);
	snprintf(dir, sizeof(dir), "%s/windows/temp", drive_c);
	mkdirs(dir);
}

const char *path_drive_c(void)
{
	pthread_once(&once, init_root);
	return drive_c;
}

/*
 * Appends component name to the Unix path in out, preferring an existing
 * entry that differs only in case. Returns -1 if the result is too long.
 */
static int append_component(char *out, size_t outsz, const char *name)
{
	size_t len = strlen(out);
	const char *sep = len && out[len - 1] == '/' ? "" : "/";
	char try[PATH_MAX + NAME_MAX + 2];

	snprintf(try, sizeof(try), "%s%s%s", out, sep, name);
	if (access(try, F_OK)) {
		DIR *d = opendir(len ? out : "/");
		if (d) {
			struct dirent *e;
			while ((e = readdir(d))) {
				if (!strcasecmp(e->d_name, name)) {
					snprintf(try, sizeof(try), "%s%s%s", out,
						 sep, e->d_name);
					break;
				}
			}
			closedir(d);
		}
	}
	size_t n = strlen(try);
	if (n >= outsz)
		return -1;
	memcpy(out, try, n + 1);
	return 0;
}

/* Removes the last component ("..") without going above root_len. */
static void pop_component(char *out, size_t root_len)
{
	char *slash = strrchr(out, '/');

	if (slash && (size_t)(slash - out) >= root_len)
		*slash = '\0';
	if (strlen(out) < root_len)
		out[root_len] = '\0';
	if (!out[0])
		strcpy(out, "/");
}

/*
 * The current directory DLLs see: the Windows process's own, kept apart
 * from the harness's. A program run on Windows has one on the system
 * drive, so it starts at C:\\ ($VFW_CWD, a Windows or Unix directory,
 * says otherwise), and drive-less rooted paths ("\\Windows\\codec.ini")
 * and relative ones a DLL uses are taken from it. SetCurrentDirectory
 * changes it without moving the harness, whose own relative paths (the
 * files on its command line) stay the user's.
 */
static char win_cwd[PATH_MAX];
static pthread_mutex_t cwd_lock = PTHREAD_MUTEX_INITIALIZER;

/* $VFW_CWD as a Windows path, or NULL for C:\\. Run once, with win_cwd C:\\. */
static char *initial_cwd(void)
{
	const char *v = getenv("VFW_CWD");
	struct stat st;
	char *w = NULL;

	if (!v || !*v)
		return NULL;
	char *u = v[0] == '/' ? strdup(v) : NULL;
	if (!u) {
		/* Taken from C:\\, as a Windows path would be. */
		u = path_to_unix(v);
	}
	if (u && !stat(u, &st) && S_ISDIR(st.st_mode)) {
		w = path_to_windows(u);
	} else {
		fprintf(stderr, "seance: VFW_CWD: %s is not a directory; using "
			"C:\\\n", v);
	}
	free(u);
	return w;
}

/* The current directory, a Windows path, into cwd (PATH_MAX bytes). */
static void current(char *cwd)
{
	static int ready;

	pthread_mutex_lock(&cwd_lock);
	if (!ready) {
		ready = 1;
		snprintf(win_cwd, sizeof(win_cwd), "C:\\");
		pthread_mutex_unlock(&cwd_lock);
		char *w = initial_cwd();
		pthread_mutex_lock(&cwd_lock);
		if (w)
			snprintf(win_cwd, sizeof(win_cwd), "%s", w);
		free(w);
	}
	snprintf(cwd, PATH_MAX, "%s", win_cwd);
	pthread_mutex_unlock(&cwd_lock);
}

/* The Unix directory for the current one, into out; -1 if it has none. */
static int unix_dir(const char *cwd, char *out, size_t size)
{
	char *u = path_to_unix(cwd);
	int r = u && strlen(u) < size ? 0 : -1;

	if (!r)
		snprintf(out, size, "%s", u);
	free(u);
	return r;
}

char *path_to_unix(const char *win)
{
	char out[PATH_MAX];
	size_t root_len;
	const char *p = win;

	if (!win)
		return NULL;
	/* \\?\C:\... */
	if (!strncmp(p, "\\\\?\\", 4))
		p += 4;

	char cwd[PATH_MAX];
	current(cwd);
	if (p[0] && p[1] == ':') {
		char drive = p[0] | 0x20;
		if (drive != 'z' && drive != 'c')
			return NULL;
		p += 2;
		/* "C:x": from that drive's current directory, if it is current. */
		if (*p != '\\' && *p != '/' && (cwd[0] | 0x20) == drive) {
			if (unix_dir(cwd, out, sizeof(out)))
				return NULL;
		} else {
			snprintf(out, sizeof(out), "%s",
				 drive == 'c' ? path_drive_c() : "/");
		}
	} else if (*p == '\\' || *p == '/') {
		/* Rooted: on the current directory's drive. */
		snprintf(out, sizeof(out), "%s",
			 (cwd[0] | 0x20) == 'c' ? path_drive_c() : "/");
	} else if (unix_dir(cwd, out, sizeof(out))) {
		return NULL;
	}
	root_len = strcmp(out, "/") ? strlen(out) : 0;

	char comp[NAME_MAX + 1];
	while (*p) {
		while (*p == '\\' || *p == '/')
			p++;
		size_t n = 0;
		while (p[n] && p[n] != '\\' && p[n] != '/')
			n++;
		if (!n)
			break;
		if (n > NAME_MAX)
			return NULL;
		memcpy(comp, p, n);
		comp[n] = '\0';
		p += n;

		/* Windows ignores trailing dots and spaces. */
		while (n && (comp[n - 1] == ' ' || (comp[n - 1] == '.' &&
			     strcmp(comp, ".") && strcmp(comp, ".."))))
			comp[--n] = '\0';
		if (!n || !strcmp(comp, "."))
			continue;
		if (!strcmp(comp, "..")) {
			pop_component(out, root_len);
			continue;
		}
		if (append_component(out, sizeof(out), comp))
			return NULL;
	}
	return strdup(out);
}

char *path_to_windows(const char *unix_path)
{
	const char *c = path_drive_c();
	size_t clen = strlen(c);
	char out[PATH_MAX];
	const char *rest;

	if (!strncmp(unix_path, c, clen) &&
	    (unix_path[clen] == '/' || !unix_path[clen])) {
		rest = unix_path + clen;
		snprintf(out, sizeof(out), "C:%s", *rest ? rest : "\\");
	} else {
		snprintf(out, sizeof(out), "Z:%s", unix_path);
	}
	for (char *p = out; *p; p++)
		if (*p == '/')
			*p = '\\';
	return strdup(out);
}

char *path_getcwd(void)
{
	char cwd[PATH_MAX];

	current(cwd);
	return strdup(cwd);
}

int path_chdir(const char *win)
{
	struct stat st;
	char *u = path_to_unix(win);

	if (!u) {
		errno = ENOENT;
		return -1;
	}
	int failed = stat(u, &st);
	int e = failed ? errno : ENOTDIR;
	if (failed || !S_ISDIR(st.st_mode)) {
		free(u);
		errno = e;
		return -1;
	}
	char *w = path_to_windows(u);
	free(u);
	if (!w)
		return -1;
	pthread_mutex_lock(&cwd_lock);
	snprintf(win_cwd, sizeof(win_cwd), "%s", w);
	pthread_mutex_unlock(&cwd_lock);
	free(w);
	return 0;
}
