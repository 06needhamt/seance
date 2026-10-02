/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * kernel32: private profile (INI file) functions. Many VfW codecs keep
 * their settings in an INI file in C:\WINDOWS or next to themselves.
 *
 * Each call reads (and, for writes, rewrites) the whole file; they are
 * small and rarely touched outside configuration. Sections and keys match
 * case-insensitively, as on Windows.
 */
#include <errno.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#include "common/error.h"
#include "win32/codepage.h"
#include "win32/internal.h"
#include "win32/k32.h"
#include "win32/path.h"

struct ini_line {
	char *text;		/* the raw line, without the newline */
};

struct ini {
	struct ini_line *lines;
	size_t n;
};

/* The internal (UTF-8) buffer; callers' own sizes apply when converting. */
#define PROFILE_MAX 65536

static pthread_mutex_t ini_lock = PTHREAD_MUTEX_INITIALIZER;

static void ini_free(struct ini *ini)
{
	for (size_t i = 0; i < ini->n; i++)
		free(ini->lines[i].text);
	free(ini->lines);
	ini->lines = NULL;
	ini->n = 0;
}

static int ini_append(struct ini *ini, size_t at, const char *text)
{
	struct ini_line *l = realloc(ini->lines, (ini->n + 1) * sizeof(*l));
	char *t = strdup(text);

	if (!l || !t) {
		free(t);
		if (l)
			ini->lines = l;
		return -1;
	}
	ini->lines = l;
	memmove(&l[at + 1], &l[at], (ini->n - at) * sizeof(*l));
	l[at].text = t;
	ini->n++;
	return 0;
}

static void ini_remove(struct ini *ini, size_t at)
{
	free(ini->lines[at].text);
	memmove(&ini->lines[at], &ini->lines[at + 1],
		(ini->n - at - 1) * sizeof(*ini->lines));
	ini->n--;
}

static void ini_load(struct ini *ini, const char *path)
{
	void *buf;
	size_t len;

	ini->lines = NULL;
	ini->n = 0;
	if (vfw_read_file(path, &buf, &len))
		return;
	char *s = buf, *end = s + len;
	while (s < end) {
		char *nl = memchr(s, '\n', end - s);
		size_t l = nl ? (size_t)(nl - s) : (size_t)(end - s);
		if (l && s[l - 1] == '\r')
			l--;
		char *line = strndup(s, l);
		if (line) {
			ini_append(ini, ini->n, line);
			free(line);
		}
		s = nl ? nl + 1 : end;
	}
	free(buf);
}

static int ini_save(const struct ini *ini, const char *path)
{
	FILE *f = fopen(path, "wb");

	if (!f)
		return -1;
	for (size_t i = 0; i < ini->n; i++)
		fprintf(f, "%s\r\n", ini->lines[i].text);
	return fclose(f);
}

static char *trim(char *s)
{
	while (*s == ' ' || *s == '\t')
		s++;
	char *e = s + strlen(s);
	while (e > s && (e[-1] == ' ' || e[-1] == '\t'))
		*--e = '\0';
	return s;
}

/* The section name if line i is "[name]", written into buf. */
static int section_at(const struct ini *ini, size_t i, char *buf, size_t size)
{
	char tmp[512];

	snprintf(tmp, sizeof(tmp), "%s", ini->lines[i].text);
	char *t = trim(tmp);
	if (t[0] != '[')
		return 0;
	char *close = strchr(t, ']');
	if (!close)
		return 0;
	*close = '\0';
	snprintf(buf, size, "%s", trim(t + 1));
	return 1;
}

/* If line i is "key=value", its key and value (into bufs). */
static int entry_at(const struct ini *ini, size_t i, char *key, size_t ksize,
		    char *value, size_t vsize)
{
	char tmp[4096];

	snprintf(tmp, sizeof(tmp), "%s", ini->lines[i].text);
	char *t = trim(tmp);
	if (!*t || *t == ';' || *t == '[')
		return 0;
	char *eq = strchr(t, '=');
	if (!eq)
		return 0;
	*eq = '\0';
	snprintf(key, ksize, "%s", trim(t));
	char *v = trim(eq + 1);
	/* Values may be quoted. */
	size_t vl = strlen(v);
	if (vl >= 2 && ((v[0] == '"' && v[vl - 1] == '"') ||
			(v[0] == '\'' && v[vl - 1] == '\''))) {
		v[vl - 1] = '\0';
		v++;
	}
	snprintf(value, vsize, "%s", v);
	return 1;
}

/* The line range [*start, *end) holding section's entries, or -1. */
static int find_section(const struct ini *ini, const char *section,
			size_t *start, size_t *end)
{
	char name[512];

	for (size_t i = 0; i < ini->n; i++) {
		if (!section_at(ini, i, name, sizeof(name)) || strcasecmp(name, section))
			continue;
		size_t j = i + 1;
		while (j < ini->n && !section_at(ini, j, name, sizeof(name)))
			j++;
		*start = i + 1;
		*end = j;
		return (int)i;
	}
	return -1;
}

/* The Unix path of an INI file: a bare name lives in C:\WINDOWS. */
static char *ini_path(const char *file)
{
	char win[1024];

	if (!file)
		file = "win.ini";
	if (strchr(file, '\\') || strchr(file, '/') || strchr(file, ':'))
		snprintf(win, sizeof(win), "%s", file);
	else
		snprintf(win, sizeof(win), "C:\\WINDOWS\\%s", file);
	return path_to_unix(win);
}

/*
 * The core of GetPrivateProfileString, on UTF-8 strings. Returns the
 * characters written, excluding the final NUL, with Windows' truncation
 * rules: size - 1 for a truncated value, size - 2 for a truncated list.
 */
static DWORD get_string(const char *section, const char *key, const char *def,
			char *out, DWORD size, const char *file)
{
	struct ini ini;
	char *path = ini_path(file);
	static _Thread_local char buf[PROFILE_MAX];
	size_t n = 0;
	int list = !section || !key;

	if (!size)
		return 0;
	pthread_mutex_lock(&ini_lock);
	ini.lines = NULL;
	ini.n = 0;
	if (path)
		ini_load(&ini, path);
	pthread_mutex_unlock(&ini_lock);
	free(path);

	if (!section) {
		/* All section names, each NUL-terminated, then a final NUL. */
		char name[512];
		for (size_t i = 0; i < ini.n; i++) {
			if (!section_at(&ini, i, name, sizeof(name)))
				continue;
			size_t l = strlen(name) + 1;
			if (n + l < sizeof(buf)) {
				memcpy(buf + n, name, l);
				n += l;
			}
		}
	} else {
		size_t start, end;
		int found = find_section(&ini, section, &start, &end) >= 0;
		char k[512], v[4096];
		const char *value = NULL;
		for (size_t i = start; found && i < end; i++) {
			if (!entry_at(&ini, i, k, sizeof(k), v, sizeof(v)))
				continue;
			if (!key) {
				size_t l = strlen(k) + 1;
				if (n + l < sizeof(buf)) {
					memcpy(buf + n, k, l);
					n += l;
				}
			} else if (!strcasecmp(k, key)) {
				value = v;
				break;
			}
		}
		if (key) {
			if (!value)
				value = def ? def : "";
			/* The default loses trailing blanks, like Windows. */
			snprintf(buf, sizeof(buf), "%s", value);
			if (value == def) {
				char *e = buf + strlen(buf);
				while (e > buf && e[-1] == ' ')
					*--e = '\0';
			}
			n = strlen(buf);
		}
	}
	ini_free(&ini);

	if (!list) {
		if (n >= size) {
			memcpy(out, buf, size - 1);
			out[size - 1] = '\0';
			return size - 1;
		}
		memcpy(out, buf, n + 1);
		return n;
	}
	if (n + 1 > size) {
		if (size < 2) {
			out[0] = '\0';
			return 0;
		}
		memcpy(out, buf, size - 2);
		out[size - 2] = out[size - 1] = '\0';
		return size - 2;
	}
	memcpy(out, buf, n);
	out[n] = '\0';
	if (!n && size > 1)
		out[1] = '\0';
	return n;
}

/* WritePrivateProfileString on UTF-8: key NULL deletes the section, value
 * NULL deletes the key. */
static BOOL write_string(const char *section, const char *key,
			 const char *value, const char *file)
{
	struct ini ini;
	char *path = ini_path(file);
	BOOL ok = FALSE;

	if (!section || !path) {
		free(path);
		win_set_error(ERROR_INVALID_PARAMETER);
		return FALSE;
	}
	pthread_mutex_lock(&ini_lock);
	ini_load(&ini, path);

	size_t start, end;
	int header = find_section(&ini, section, &start, &end);
	if (!key) {
		if (header >= 0)
			for (size_t i = end; i-- > (size_t)header;)
				ini_remove(&ini, i);
		ok = TRUE;
	} else {
		char k[512], v[4096];
		size_t at = end;
		int replaced = 0;
		for (size_t i = start; header >= 0 && i < end; i++) {
			if (entry_at(&ini, i, k, sizeof(k), v, sizeof(v)) &&
			    !strcasecmp(k, key)) {
				ini_remove(&ini, i);
				at = i;
				replaced = 1;
				break;
			}
		}
		if (value) {
			/* A replaced entry keeps its original key spelling. */
			char line[8192];
			snprintf(line, sizeof(line), "%s=%s", replaced ? k : key, value);
			if (header < 0) {
				char head[520];
				snprintf(head, sizeof(head), "[%s]", section);
				ini_append(&ini, ini.n, head);
				at = ini.n;
			} else if (!replaced) {
				/* After the section's last entry, before blank lines. */
				while (at > start && !*trim(ini.lines[at - 1].text))
					at--;
			}
			ini_append(&ini, at, line);
		}
		ok = TRUE;
	}
	if (ok && ini_save(&ini, path)) {
		win_set_error(win_error_from_errno(errno));
		ok = FALSE;
	}
	pthread_mutex_unlock(&ini_lock);
	ini_free(&ini);
	free(path);
	return ok;
}

WINFN(DWORD) k32_GetPrivateProfileStringA(LPCSTR section, LPCSTR key,
					  LPCSTR def, LPSTR out, DWORD size,
					  LPCSTR file)
{
	char *s = acp_to_utf8(section), *k = acp_to_utf8(key);
	char *d = acp_to_utf8(def), *f = acp_to_utf8(file);
	char *tmp = malloc(PROFILE_MAX);
	DWORD r = 0;

	if (tmp && size) {
		DWORD n = get_string(s, k, d, tmp, PROFILE_MAX, f);
		/* Convert, preserving the embedded NULs of a list. */
		int wn = cp_to_utf16(CP_UTF8, tmp, n + 1, NULL, 0, 0);
		WCHAR *w = malloc(wn * sizeof(WCHAR));
		if (w) {
			cp_to_utf16(CP_UTF8, tmp, n + 1, w, wn, 0);
			int an = cp_from_utf16(CP_ACP, w, wn, NULL, 0, '?', NULL);
			char *a = malloc(an + 1);
			if (a) {
				cp_from_utf16(CP_ACP, w, wn, a, an, '?', NULL);
				a[an] = '\0';
				DWORD alen = an - 1;	/* without the final NUL */
				int list = !section || !key;
				if (alen + (list ? 2 : 1) > size) {
					DWORD keep = size - (list ? 2 : 1);
					memcpy(out, a, keep);
					out[keep] = '\0';
					if (list && size >= 2)
						out[keep + 1] = '\0';
					r = keep;
				} else {
					memcpy(out, a, alen + 1);
					if (list)
						out[alen + 1] = '\0';
					r = alen;
				}
				free(a);
			}
			free(w);
		}
	}
	free(tmp);
	free(s);
	free(k);
	free(d);
	free(f);
	return r;
}

WINFN(DWORD) k32_GetPrivateProfileStringW(LPCWSTR section, LPCWSTR key,
					  LPCWSTR def, LPWSTR out, DWORD size,
					  LPCWSTR file)
{
	char *s = utf16_to_utf8_dup(section), *k = utf16_to_utf8_dup(key);
	char *d = utf16_to_utf8_dup(def), *f = utf16_to_utf8_dup(file);
	char *tmp = malloc(PROFILE_MAX);
	DWORD r = 0;

	if (tmp && size) {
		DWORD n = get_string(s, k, d, tmp, PROFILE_MAX, f);
		int list = !section || !key;
		int wn = cp_to_utf16(CP_UTF8, tmp, n + 1, NULL, 0, 0);
		WCHAR *w = malloc((wn + 1) * sizeof(WCHAR));
		if (w) {
			cp_to_utf16(CP_UTF8, tmp, n + 1, w, wn, 0);
			DWORD wlen = wn - 1;
			if (wlen + (list ? 2 : 1) > size) {
				DWORD keep = size - (list ? 2 : 1);
				memcpy(out, w, keep * sizeof(WCHAR));
				out[keep] = 0;
				if (list && size >= 2)
					out[keep + 1] = 0;
				r = keep;
			} else {
				memcpy(out, w, (wlen + 1) * sizeof(WCHAR));
				if (list)
					out[wlen + 1] = 0;
				r = wlen;
			}
			free(w);
		}
	}
	free(tmp);
	free(s);
	free(k);
	free(d);
	free(f);
	return r;
}

/* GetPrivateProfileInt: a leading decimal number, else the default. */
static UINT parse_int(const char *s, int def)
{
	char *end;
	long v = strtol(s, &end, 10);
	return end == s ? (UINT)def : (UINT)v;
}

WINFN(UINT) k32_GetPrivateProfileIntA(LPCSTR section, LPCSTR key, INT def,
				      LPCSTR file)
{
	char buf[64];
	char *s = acp_to_utf8(section), *k = acp_to_utf8(key), *f = acp_to_utf8(file);
	UINT r = def;

	if (s && k && get_string(s, k, "", buf, sizeof(buf), f))
		r = parse_int(buf, def);
	free(s);
	free(k);
	free(f);
	return r;
}

WINFN(UINT) k32_GetPrivateProfileIntW(LPCWSTR section, LPCWSTR key, INT def,
				      LPCWSTR file)
{
	char buf[64];
	char *s = utf16_to_utf8_dup(section), *k = utf16_to_utf8_dup(key);
	char *f = utf16_to_utf8_dup(file);
	UINT r = def;

	if (s && k && get_string(s, k, "", buf, sizeof(buf), f))
		r = parse_int(buf, def);
	free(s);
	free(k);
	free(f);
	return r;
}

WINFN(BOOL) k32_WritePrivateProfileStringA(LPCSTR section, LPCSTR key,
					   LPCSTR value, LPCSTR file)
{
	char *s = acp_to_utf8(section), *k = acp_to_utf8(key);
	char *v = acp_to_utf8(value), *f = acp_to_utf8(file);
	BOOL r = write_string(s, k, v, f);

	free(s);
	free(k);
	free(v);
	free(f);
	return r;
}

WINFN(BOOL) k32_WritePrivateProfileStringW(LPCWSTR section, LPCWSTR key,
					   LPCWSTR value, LPCWSTR file)
{
	char *s = utf16_to_utf8_dup(section), *k = utf16_to_utf8_dup(key);
	char *v = utf16_to_utf8_dup(value), *f = utf16_to_utf8_dup(file);
	BOOL r = write_string(s, k, v, f);

	free(s);
	free(k);
	free(v);
	free(f);
	return r;
}

/* Struct variants: hex bytes followed by a one-byte checksum (their sum). */
WINFN(BOOL) k32_WritePrivateProfileStructA(LPCSTR section, LPCSTR key,
					   LPVOID data, UINT size, LPCSTR file)
{
	char *hex;
	BYTE sum = 0;
	BOOL r;

	if (!data)
		return k32_WritePrivateProfileStringA(section, key, NULL, file);
	if (!(hex = malloc(size * 2 + 3))) {
		win_set_error(ERROR_NOT_ENOUGH_MEMORY);
		return FALSE;
	}
	for (UINT i = 0; i < size; i++) {
		BYTE b = ((BYTE *)data)[i];
		sprintf(hex + 2 * i, "%02X", b);
		sum += b;
	}
	sprintf(hex + 2 * size, "%02X", sum);
	r = k32_WritePrivateProfileStringA(section, key, hex, file);
	free(hex);
	return r;
}

WINFN(BOOL) k32_GetPrivateProfileStructA(LPCSTR section, LPCSTR key,
					 LPVOID data, UINT size, LPCSTR file)
{
	char *hex = malloc(size * 2 + 8);
	BOOL ok = FALSE;

	if (!hex)
		return FALSE;
	DWORD n = k32_GetPrivateProfileStringA(section, key, "", hex, size * 2 + 8,
					       file);
	if (n == size * 2 + 2) {
		BYTE sum = 0, b;
		ok = TRUE;
		for (UINT i = 0; i <= size && ok; i++) {
			unsigned v;
			ok = sscanf(hex + 2 * i, "%2x", &v) == 1;
			b = v;
			if (i < size) {
				((BYTE *)data)[i] = b;
				sum += b;
			} else {
				ok = ok && b == sum;
			}
		}
	}
	free(hex);
	if (!ok)
		win_set_error(ERROR_INVALID_DATA);
	return ok;
}

WINFN(DWORD) k32_GetProfileStringA(LPCSTR section, LPCSTR key, LPCSTR def,
				   LPSTR out, DWORD size)
{
	return k32_GetPrivateProfileStringA(section, key, def, out, size, "win.ini");
}

WINFN(UINT) k32_GetProfileIntA(LPCSTR section, LPCSTR key, INT def)
{
	return k32_GetPrivateProfileIntA(section, key, def, "win.ini");
}

WINFN(BOOL) k32_WriteProfileStringA(LPCSTR section, LPCSTR key, LPCSTR value)
{
	return k32_WritePrivateProfileStringA(section, key, value, "win.ini");
}

#define E(name) { #name, 0, (void *)k32_##name }

const struct builtin_export k32_profile_exports[] = {
	E(GetPrivateProfileIntA),
	E(GetPrivateProfileIntW),
	E(GetPrivateProfileStringA),
	E(GetPrivateProfileStringW),
	E(GetPrivateProfileStructA),
	E(GetProfileIntA),
	E(GetProfileStringA),
	E(WritePrivateProfileStringA),
	E(WritePrivateProfileStringW),
	E(WritePrivateProfileStructA),
	E(WriteProfileStringA),
	BUILTIN_END
};
