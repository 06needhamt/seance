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

	if (p[0] && p[1] == ':') {
		char drive = p[0] | 0x20;
		if (drive == 'z')
			snprintf(out, sizeof(out), "/");
		else if (drive == 'c')
			snprintf(out, sizeof(out), "%s", path_drive_c());
		else
			return NULL;
		/* "C:x" (relative to that drive's cwd) is taken from its root. */
		p += 2;
	} else if (*p == '\\' || *p == '/') {
		snprintf(out, sizeof(out), "%s",
			 path_rooted_drive(p) == 'c' ? path_drive_c() : "/");
	} else {
		if (!getcwd(out, sizeof(out)))
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

/* Whether directory dir has an entry named the n bytes at name, in any case. */
static int has_entry(const char *dir, const char *name, size_t n)
{
	DIR *d = opendir(dir);
	struct dirent *e;
	int found = 0;

	while (d && !found && (e = readdir(d)))
		found = strlen(e->d_name) == n && !strncasecmp(e->d_name, name, n);
	if (d)
		closedir(d);
	return found;
}

char path_rooted_drive(const char *rooted)
{
	char *cwd = path_getcwd();
	char drive = cwd ? (cwd[0] | 0x20) : 'z';

	free(cwd);
	if (drive == 'c')
		return 'c';
	/* Its first component, looked for on C: and in /. */
	const char *p = rooted;
	while (*p == '\\' || *p == '/')
		p++;
	size_t n = strcspn(p, "\\/");
	if (!n || n > NAME_MAX || (p[0] == '.' && (n == 1 || (n == 2 && p[1] == '.'))))
		return 'z';
	if (has_entry(path_drive_c(), p, n) || !has_entry("/", p, n))
		return 'c';
	return 'z';
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

	if (!getcwd(cwd, sizeof(cwd)))
		return strdup("C:\\");
	return path_to_windows(cwd);
}

int path_chdir(const char *win)
{
	char *u = path_to_unix(win);
	int r = u ? chdir(u) : (errno = ENOENT, -1);

	free(u);
	return r;
}
