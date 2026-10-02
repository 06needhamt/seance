/* SPDX-License-Identifier: GPL-2.0-only */
#include <errno.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <time.h>

#include "common/error.h"
#include "common/log.h"
#include "win32/codepage.h"
#include "win32/internal.h"
#include "win32/path.h"
#include "win32/reg.h"

#define MAX_KEY_NAME 255

static pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;
static struct reg_key root;	/* unnamed; its subkeys are the hives */
static int loaded;
static int loading;		/* no change reports while reading the file */
static unsigned generation;	/* counts reloads */
static char file[4096];		/* "" to keep the registry in memory only */

void (*reg_change_hook)(const struct reg_key *k, DWORD what);

static void changed(const struct reg_key *k, DWORD what)
{
	if (reg_change_hook && !loading)
		reg_change_hook(k, what);
}

static const char *const hives[] = {
	"HKEY_LOCAL_MACHINE", "HKEY_CURRENT_USER", "HKEY_USERS",
	"HKEY_CURRENT_CONFIG",
};

/* Short and aliased hive names, as .reg files may spell them. */
static const struct {
	const char *name, *path;
} hive_aliases[] = {
	{ "HKEY_CLASSES_ROOT", "HKEY_LOCAL_MACHINE\\Software\\Classes" },
	{ "HKCR", "HKEY_LOCAL_MACHINE\\Software\\Classes" },
	{ "HKLM", "HKEY_LOCAL_MACHINE" },
	{ "HKCU", "HKEY_CURRENT_USER" },
	{ "HKU", "HKEY_USERS" },
	{ "HKCC", "HKEY_CURRENT_CONFIG" },
};

static FILETIME now(void)
{
	FILETIME ft;
	ULONGLONG t = ((ULONGLONG)time(NULL) + 11644473600ULL) * 10000000ULL;

	ft.dwLowDateTime = (DWORD)t;
	ft.dwHighDateTime = (DWORD)(t >> 32);
	return ft;
}

/* The tree. */

static void free_key(struct reg_key *k)
{
	for (size_t i = 0; i < k->nsub; i++)
		free_key(k->sub[i]);
	for (size_t i = 0; i < k->nval; i++) {
		free(k->val[i].name);
		free(k->val[i].data);
	}
	free(k->sub);
	free(k->val);
	free(k->name);
	if (k != &root)
		free(k);
}

/* The index of subkey name in k, or where it would go (*found = 0). */
static size_t find_sub(const struct reg_key *k, const char *name, size_t len,
		       int *found)
{
	size_t lo = 0, hi = k->nsub;

	while (lo < hi) {
		size_t mid = (lo + hi) / 2;
		const char *n = k->sub[mid]->name;
		int c = strncasecmp(n, name, len);
		if (!c)
			c = n[len] ? 1 : 0;
		if (!c) {
			*found = 1;
			return mid;
		}
		if (c < 0)
			lo = mid + 1;
		else
			hi = mid;
	}
	*found = 0;
	return lo;
}

static struct reg_key *add_sub(struct reg_key *k, const char *name, size_t len,
			       size_t at, int is_volatile)
{
	struct reg_key *c = calloc(1, sizeof(*c));
	struct reg_key **sub = realloc(k->sub, (k->nsub + 1) * sizeof(*sub));

	if (sub)
		k->sub = sub;
	if (!c || !sub || !(c->name = strndup(name, len))) {
		free(c);
		return NULL;
	}
	memmove(&sub[at + 1], &sub[at], (k->nsub - at) * sizeof(*sub));
	sub[at] = c;
	k->nsub++;
	c->parent = k;
	c->is_volatile = is_volatile;
	c->modified = now();
	changed(k, REG_NOTIFY_CHANGE_NAME);
	return c;
}

static void detach(struct reg_key *k)
{
	struct reg_key *p = k->parent;
	int found;
	size_t i = find_sub(p, k->name, strlen(k->name), &found);

	changed(k, REG_CHANGE_DELETED);
	if (found) {
		memmove(&p->sub[i], &p->sub[i + 1], (p->nsub - i - 1) * sizeof(*p->sub));
		p->nsub--;
		p->modified = now();
		changed(p, REG_NOTIFY_CHANGE_NAME);
	}
	free_key(k);
}

/*
 * path with its hive spelled out ("HKCU\x" becomes "HKEY_CURRENT_USER\x"),
 * malloc()ed, or NULL if it names no hive.
 */
static char *canonical(const char *path)
{
	size_t n = strcspn(path, "\\");
	const char *rest = path + n;
	char *out;

	for (size_t i = 0; i < sizeof(hive_aliases) / sizeof(hive_aliases[0]); i++) {
		if (strlen(hive_aliases[i].name) == n &&
		    !strncasecmp(path, hive_aliases[i].name, n)) {
			out = malloc(strlen(hive_aliases[i].path) + strlen(rest) + 1);
			if (out)
				sprintf(out, "%s%s", hive_aliases[i].path, rest);
			return out;
		}
	}
	for (size_t i = 0; i < sizeof(hives) / sizeof(hives[0]); i++)
		if (strlen(hives[i]) == n && !strncasecmp(path, hives[i], n))
			return strdup(path);
	return NULL;
}

/*
 * Walks path from the root; with create, adds missing keys. Empty
 * components (a trailing or doubled backslash) are skipped.
 */
static struct reg_key *walk(const char *path, int create, int is_volatile,
			    int *created, LONG *err)
{
	char *c = canonical(path);
	struct reg_key *k = &root;
	LONG e = ERROR_SUCCESS;

	if (created)
		*created = 0;
	if (!c) {
		e = ERROR_BADKEY;
		k = NULL;
	}
	for (const char *p = c; k && *p;) {
		size_t len = strcspn(p, "\\");
		if (len) {
			int found;
			size_t at = find_sub(k, p, len, &found);
			if (found) {
				k = k->sub[at];
			} else if (!create) {
				e = ERROR_FILE_NOT_FOUND;
				k = NULL;
			} else if (len > MAX_KEY_NAME) {
				e = ERROR_INVALID_PARAMETER;
				k = NULL;
			} else {
				if (!(k = add_sub(k, p, len, at, is_volatile)))
					e = ERROR_NOT_ENOUGH_MEMORY;
				else if (created)
					*created = 1;
			}
		}
		p += len;
		if (*p)
			p++;
	}
	free(c);
	if (err)
		*err = e;
	return k;
}

/* Loading and saving. */

static void compute_file(void)
{
	const char *env = getenv("VFW_REGISTRY");
	const char *root_dir = getenv("VFW_ROOT");
	const char *home = getenv("HOME");

	if (env)
		snprintf(file, sizeof(file), "%s", env);
	else if (root_dir && *root_dir)
		snprintf(file, sizeof(file), "%s/registry.reg", root_dir);
	else
		snprintf(file, sizeof(file), "%s/.seance/registry.reg",
			 home && *home ? home : "/tmp");
}

static void load(void)
{
	void *buf;
	size_t len;

	loaded = 1;
	loading = 1;
	generation++;
	for (size_t i = 0; i < sizeof(hives) / sizeof(hives[0]); i++)
		walk(hives[i], 1, 0, NULL, NULL);
	walk("HKEY_LOCAL_MACHINE\\Software\\Classes", 1, 0, NULL, NULL);

	compute_file();
	if (*file && !vfw_read_file(file, &buf, &len)) {
		TRACE("registry: loading %s", file);
		int bad = reg_import(buf, len);
		if (bad)
			vfw_warn("registry: skipped %d line(s) of %s", bad,
				 file);
		free(buf);
	}
	loading = 0;
}

unsigned reg_generation(void)
{
	return generation;
}

char *reg_key_path(const struct reg_key *k)
{
	size_t n = 0;

	for (const struct reg_key *p = k; p && p != &root; p = p->parent)
		n += strlen(p->name) + 1;
	char *out = malloc(n ? n : 1);
	if (!out)
		return NULL;
	out[n ? n - 1 : 0] = '\0';
	for (const struct reg_key *p = k; p && p != &root; p = p->parent) {
		size_t len = strlen(p->name);
		n -= len + 1;
		memcpy(out + n, p->name, len);
		if (n)
			out[n - 1] = '\\';
	}
	return out;
}

void reg_lock(void)
{
	pthread_mutex_lock(&lock);
	if (!loaded)
		load();
}

void reg_unlock(void)
{
	pthread_mutex_unlock(&lock);
}

void reg_reload(void)
{
	pthread_mutex_lock(&lock);
	free_key(&root);
	memset(&root, 0, sizeof(root));
	load();
	pthread_mutex_unlock(&lock);
}

static void put_escaped(FILE *f, const char *s)
{
	fputc('"', f);
	for (; *s; s++) {
		if (*s == '\\' || *s == '"')
			fputc('\\', f);
		fputc(*s, f);
	}
	fputc('"', f);
}

/*
 * The UTF-8 form of REG_SZ data if it can be written back as a quoted
 * string unchanged: NUL-terminated UTF-16, no other NULs or control
 * characters, and valid enough to survive the round trip.
 */
static char *sz_as_text(const struct reg_value *v)
{
	const WCHAR *w = (const WCHAR *)v->data;
	size_t n = v->size / 2;

	if (v->size % 2 || !n || w[n - 1])
		return NULL;
	for (size_t i = 0; i + 1 < n; i++)
		if (w[i] < 0x20)
			return NULL;
	char *u = utf16_to_utf8_dup(w);
	WCHAR *back = u ? utf8_to_utf16_dup(u) : NULL;
	int same = back && wstrlen(back) == n - 1 &&
		   !memcmp(back, w, (n - 1) * sizeof(WCHAR));
	free(back);
	if (!same) {
		free(u);
		return NULL;
	}
	return u;
}

static void put_value(FILE *f, const struct reg_value *v)
{
	char *text;
	int col;

	if (*v->name) {
		put_escaped(f, v->name);
		col = strlen(v->name) + 3;
	} else {
		fputc('@', f);
		col = 1;
	}
	fputc('=', f);
	if (v->type == REG_SZ && (text = sz_as_text(v))) {
		put_escaped(f, text);
		fputc('\n', f);
		free(text);
		return;
	}
	if (v->type == REG_DWORD && v->size == 4) {
		DWORD d;
		memcpy(&d, v->data, 4);
		fprintf(f, "dword:%08x\n", d);
		return;
	}
	col += v->type == REG_BINARY ? fprintf(f, "hex:") :
					fprintf(f, "hex(%x):", v->type);
	for (DWORD i = 0; i < v->size; i++) {
		if (col > 76) {
			fputs("\\\n  ", f);
			col = 2;
		}
		col += fprintf(f, "%02x%s", v->data[i], i + 1 < v->size ? "," : "");
	}
	fputc('\n', f);
}

static void put_key(FILE *f, const struct reg_key *k, char *path, size_t len,
		    size_t size)
{
	if (k->is_volatile)
		return;
	if (k != &root) {
		size_t n = strlen(k->name);
		if (len + n + 2 > size)
			return;
		if (len)
			path[len++] = '\\';
		memcpy(path + len, k->name, n + 1);
		len += n;
		/* Keys with values, and empty ones below the hives. */
		if (k->nval || (!k->nsub && k->parent != &root)) {
			fprintf(f, "\n[%s]\n", path);
			for (size_t i = 0; i < k->nval; i++)
				put_value(f, &k->val[i]);
		}
	}
	for (size_t i = 0; i < k->nsub; i++)
		put_key(f, k->sub[i], path, len, size);
}

LONG reg_save(void)
{
	char tmp[sizeof(file) + 8], path[8192];
	FILE *f;

	if (!*file)
		return ERROR_SUCCESS;
	if (!getenv("VFW_REGISTRY"))
		path_drive_c();		/* creates $VFW_ROOT */
	snprintf(tmp, sizeof(tmp), "%s.tmp", file);
	if (!(f = fopen(tmp, "w"))) {
		vfw_warn("registry: can't write %s: %s", tmp, strerror(errno));
		return ERROR_CANTWRITE;
	}
	fputs("Windows Registry Editor Version 5.00\n", f);
	path[0] = '\0';
	put_key(f, &root, path, 0, sizeof(path));
	if (fclose(f) || rename(tmp, file)) {
		vfw_warn("registry: can't write %s: %s", file, strerror(errno));
		remove(tmp);
		return ERROR_CANTWRITE;
	}
	return ERROR_SUCCESS;
}

/* Keys and values. */

struct reg_key *reg_open(const char *path)
{
	return walk(path, 0, 0, NULL, NULL);
}

struct reg_key *reg_create(const char *path, int is_volatile, int *created,
			   LONG *err)
{
	return walk(path, 1, is_volatile, created, err);
}

LONG reg_delete(struct reg_key *k)
{
	if (k->nsub || k->parent == &root)
		return ERROR_ACCESS_DENIED;
	detach(k);
	return ERROR_SUCCESS;
}

struct reg_value *reg_get(struct reg_key *k, const char *name)
{
	if (!name)
		name = "";
	for (size_t i = 0; i < k->nval; i++)
		if (!strcasecmp(k->val[i].name, name))
			return &k->val[i];
	return NULL;
}

LONG reg_set(struct reg_key *k, const char *name, DWORD type,
	     const void *data, DWORD size)
{
	struct reg_value *v = reg_get(k, name);
	BYTE *copy = malloc(size ? size : 1);

	if (!copy)
		return ERROR_NOT_ENOUGH_MEMORY;
	if (size)
		memcpy(copy, data, size);
	if (!v) {
		struct reg_value *val = realloc(k->val, (k->nval + 1) * sizeof(*val));
		char *n = strdup(name ? name : "");
		if (val)
			k->val = val;
		if (!val || !n) {
			free(n);
			free(copy);
			return ERROR_NOT_ENOUGH_MEMORY;
		}
		v = &k->val[k->nval++];
		v->name = n;
	} else {
		free(v->data);
	}
	v->type = type;
	v->data = copy;
	v->size = size;
	k->modified = now();
	changed(k, REG_NOTIFY_CHANGE_LAST_SET);
	return ERROR_SUCCESS;
}

LONG reg_unset(struct reg_key *k, const char *name)
{
	struct reg_value *v = reg_get(k, name);

	if (!v)
		return ERROR_FILE_NOT_FOUND;
	free(v->name);
	free(v->data);
	size_t i = v - k->val;
	memmove(&k->val[i], &k->val[i + 1], (k->nval - i - 1) * sizeof(*v));
	k->nval--;
	k->modified = now();
	changed(k, REG_NOTIFY_CHANGE_LAST_SET);
	return ERROR_SUCCESS;
}

/* Importing .reg files. */

/*
 * A "quoted string" at *p (just past the opening quote), with regedit's
 * \\ and \" escapes (and Wine's \n); advances past the closing quote.
 */
static char *parse_quoted(const char **p)
{
	const char *s = *p;
	char *out = malloc(strlen(s) + 1), *o = out;

	if (!out)
		return NULL;
	for (; *s && *s != '"'; s++) {
		if (*s == '\\' && (s[1] == '\\' || s[1] == '"'))
			*o++ = *++s;
		else if (*s == '\\' && s[1] == 'n' && ++s)
			*o++ = '\n';
		else
			*o++ = *s;
	}
	if (*s != '"') {
		free(out);
		return NULL;
	}
	*o = '\0';
	*p = s + 1;
	return out;
}

/* "xx,xx,..." into a malloc()ed buffer; -1 on bad syntax. */
static int parse_hex(const char *s, BYTE **data, DWORD *size)
{
	size_t cap = strlen(s) / 2 + 1, n = 0;
	BYTE *out = malloc(cap);

	if (!out)
		return -1;
	while (*s == ' ' || *s == '\t')
		s++;
	while (*s) {
		char *end;
		unsigned long b = strtoul(s, &end, 16);
		if (end == s || end - s > 2 || b > 0xff || n >= cap)
			goto bad;
		out[n++] = (BYTE)b;
		s = end;
		while (*s == ' ' || *s == '\t')
			s++;
		if (*s == ',')
			s++;
		else if (*s)
			goto bad;
		while (*s == ' ' || *s == '\t')
			s++;
	}
	*data = out;
	*size = n;
	return 0;
bad:
	free(out);
	return -1;
}

/* An ANSI string (REGEDIT4 hex(2)/hex(7) data) as UTF-16. */
static int ansi_to_wide(BYTE **data, DWORD *size)
{
	int n = cp_to_utf16(CP_ACP, (const char *)*data, *size, NULL, 0, 0);
	WCHAR *w = malloc((n > 0 ? n : 1) * sizeof(WCHAR));

	if (!w || n < 0)
		return free(w), -1;
	cp_to_utf16(CP_ACP, (const char *)*data, *size, w, n, 0);
	free(*data);
	*data = (BYTE *)w;
	*size = n * sizeof(WCHAR);
	return 0;
}

/* One "name"=data line under key; -1 if it can't be parsed. */
static int import_value(const char *key, const char *line, int regedit4)
{
	const char *p = line;
	char *name;
	BYTE *data = NULL;
	DWORD type, size;

	if (*p == '@') {
		name = strdup("");
		p++;
	} else {
		p++;
		name = parse_quoted(&p);
	}
	if (!name)
		return -1;
	while (*p == ' ' || *p == '\t')
		p++;
	if (*p++ != '=')
		goto bad;
	while (*p == ' ' || *p == '\t')
		p++;

	struct reg_key *k = reg_open(key);
	if (!k)
		goto bad;
	if (!strcmp(p, "-")) {
		reg_unset(k, name);
		free(name);
		return 0;
	}
	if (*p == '"') {
		p++;
		char *s = parse_quoted(&p);
		while (s && (*p == ' ' || *p == '\t'))
			p++;
		if (!s || *p) {
			free(s);
			goto bad;
		}
		data = (BYTE *)utf8_to_utf16_dup(s);
		free(s);
		if (!data)
			goto bad;
		type = REG_SZ;
		size = (wstrlen((WCHAR *)data) + 1) * sizeof(WCHAR);
	} else if (!strncasecmp(p, "dword:", 6)) {
		char *end;
		unsigned long d = strtoul(p + 6, &end, 16);
		if (end == p + 6 || end - (p + 6) > 8 || *end)
			goto bad;
		DWORD dw = (DWORD)d;
		type = REG_DWORD;
		size = 4;
		if (!(data = malloc(4)))
			goto bad;
		memcpy(data, &dw, 4);
	} else if (!strncasecmp(p, "hex", 3)) {
		p += 3;
		type = REG_BINARY;
		if (*p == '(') {
			char *end;
			type = strtoul(p + 1, &end, 16);
			if (end == p + 1 || *end != ')')
				goto bad;
			p = end + 1;
		}
		if (*p++ != ':' || parse_hex(p, &data, &size))
			goto bad;
		if (regedit4 && (type == REG_EXPAND_SZ || type == REG_MULTI_SZ) &&
		    ansi_to_wide(&data, &size))
			goto bad;
	} else {
		goto bad;
	}
	reg_set(k, name, type, data, size);
	free(data);
	free(name);
	return 0;
bad:
	free(data);
	free(name);
	return -1;
}

/* A recursive delete, for [-key] lines. */
static void delete_tree(const char *path)
{
	struct reg_key *k = reg_open(path);

	if (k && k->parent != &root)
		detach(k);
}

/* The text as UTF-8, whatever it came in. */
static char *decode(const BYTE *b, size_t len)
{
	if (len >= 2 && b[0] == 0xff && b[1] == 0xfe) {
		size_t n = (len - 2) / 2;
		WCHAR *w = malloc((n + 1) * sizeof(WCHAR));
		if (!w)
			return NULL;
		memcpy(w, b + 2, n * sizeof(WCHAR));
		w[n] = 0;
		char *u = utf16_to_utf8_dup(w);
		free(w);
		return u;
	}
	if (len >= 3 && b[0] == 0xef && b[1] == 0xbb && b[2] == 0xbf) {
		b += 3;
		len -= 3;
	}
	char *s = strndup((const char *)b, len);
	if (s && cp_to_utf16(CP_UTF8, s, -1, NULL, 0, 1) < 0) {
		/* Not UTF-8: an ANSI (REGEDIT4) file. */
		char *u = acp_to_utf8(s);
		free(s);
		s = u;
	}
	return s;
}

static char *trim(char *s)
{
	while (*s == ' ' || *s == '\t')
		s++;
	char *e = s + strlen(s);
	while (e > s && (e[-1] == ' ' || e[-1] == '\t' || e[-1] == '\r'))
		*--e = '\0';
	return s;
}

/* One logical line; returns 1 if it was skipped. */
static int import_line(char *line, char **key, int *regedit4, int lineno)
{
	if (!*line || *line == ';' || *line == '#')
		return 0;
	if (!strcmp(line, "REGEDIT4")) {
		*regedit4 = 1;
		return 0;
	}
	if (!strncmp(line, "Windows Registry Editor", 23))
		return 0;
	if (*line == '[') {
		char *close = strrchr(line, ']');
		LONG err;

		free(*key);
		*key = NULL;
		if (!close) {
			vfw_warn("registry: line %d: bad key line", lineno);
			return 1;
		}
		*close = '\0';
		if (line[1] == '-') {
			char *c = canonical(line + 2);
			if (c)
				delete_tree(c);
			free(c);
			return 0;
		}
		if (!(*key = canonical(line + 1)) ||
		    !reg_create(*key, 0, NULL, &err)) {
			vfw_warn("registry: line %d: can't create key %s",
				 lineno, line + 1);
			free(*key);
			*key = NULL;
			return 1;
		}
		return 0;
	}
	if ((*line == '"' || *line == '@') && *key &&
	    !import_value(*key, line, *regedit4))
		return 0;
	vfw_warn("registry: line %d: can't parse %.60s", lineno, line);
	return 1;
}

int reg_import(const void *text, size_t len)
{
	char *buf = decode(text, len);
	char *key = NULL, *line = NULL;
	size_t n = 0;
	int bad = 0, regedit4 = 0, lineno = 0, start = 1;

	if (!buf)
		return 1;
	for (char *s = buf; s;) {
		char *nl = strchr(s, '\n');
		if (nl)
			*nl = '\0';
		lineno++;
		if (!n)
			start = lineno;
		char *part = trim(s);
		size_t pl = strlen(part);
		s = nl ? nl + 1 : NULL;

		char *grown = realloc(line, n + pl + 1);
		if (!grown) {
			bad++;
			break;
		}
		line = grown;
		memcpy(line + n, part, pl + 1);
		n += pl;
		/* A trailing backslash continues a value onto the next line. */
		if (n && line[n - 1] == '\\' && line[0] != '[' && s) {
			line[--n] = '\0';
			continue;
		}
		bad += import_line(line, &key, &regedit4, start);
		n = 0;
	}
	free(line);
	free(key);
	free(buf);
	return bad;
}
