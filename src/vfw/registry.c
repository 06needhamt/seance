/*
 * The installed codecs.
 */
#include <ctype.h>
#include <limits.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <unistd.h>

#include "common/error.h"
#include "common/log.h"
#include "vfw/registry.h"
#include "win32/path.h"

static pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;
static struct vfw_codec *codecs;
static unsigned ncodecs;
static int loaded;
static unsigned generation;	/* counts changes to the table */
static char ini_path[PATH_MAX];

static FOURCC upper(FOURCC f)
{
	FOURCC r = 0;
	for (int i = 0; i < 4; i++)
		r |= (FOURCC)toupper((f >> (8 * i)) & 0xff) << (8 * i);
	return r;
}

static int same(FOURCC a, FOURCC b)
{
	return upper(a) == upper(b);
}

int vfw_parse_fourcc(const char *s, FOURCC *type, FOURCC *handler)
{
	const char *dot = strchr(s, '.');
	const char *h = s;
	FOURCC t = mmioFOURCC('v', 'i', 'd', 'c'), v = 0;

	if (dot) {
		if (dot - s != 4)
			return -1;
		t = mmioFOURCC(s[0], s[1], s[2], s[3]);
		h = dot + 1;
	}
	size_t n = strlen(h);
	if (n == 0 || n > 4)
		return -1;
	for (size_t i = 0; i < 4; i++)
		v |= (FOURCC)(BYTE)(i < n ? h[i] : ' ') << (8 * i);
	*type = t;
	*handler = v;
	return 0;
}

static void compute_ini_path(void)
{
	const char *ini = getenv("VFW_INI");
	const char *root = getenv("VFW_ROOT");
	const char *home = getenv("HOME");

	if (ini && *ini)
		snprintf(ini_path, sizeof(ini_path), "%s", ini);
	else if (root && *root)
		snprintf(ini_path, sizeof(ini_path), "%s/vfw.ini", root);
	else
		snprintf(ini_path, sizeof(ini_path), "%s/.seance/vfw.ini",
			 home && *home ? home : "/tmp");
}

static char *trim(char *s)
{
	while (isspace((unsigned char)*s))
		s++;
	char *e = s + strlen(s);
	while (e > s && isspace((unsigned char)e[-1]))
		*--e = '\0';
	return s;
}

static int exists(const char *p)
{
	struct stat st;
	return !stat(p, &st) && S_ISREG(st.st_mode);
}

/* Where a path from the ini file points. malloc()ed. */
static char *resolve(const char *value)
{
	char buf[PATH_MAX];

	if (strchr(value, ':') || strchr(value, '\\')) {
		char *u = path_to_unix(value);
		return u ? u : strdup(value);
	}
	if (value[0] == '/')
		return strdup(value);

	/* Relative to the ini file's directory, if the file is there. */
	const char *slash = strrchr(ini_path, '/');
	if (slash) {
		snprintf(buf, sizeof(buf), "%.*s/%s", (int)(slash - ini_path),
			 ini_path, value);
		if (exists(buf) || strchr(value, '/'))
			return strdup(buf);
	}
	/* A bare name: system32, else the loader's search. */
	snprintf(buf, sizeof(buf), "%s/windows/system32/%s", path_drive_c(), value);
	return strdup(exists(buf) ? buf : value);
}

static int add(FOURCC type, FOURCC handler, char *path, DRIVERPROC proc,
	       int from_ini)
{
	generation++;
	for (unsigned i = 0; i < ncodecs; i++)
		if (same(codecs[i].type, type) && same(codecs[i].handler, handler)) {
			free(codecs[i].path);
			codecs[i].path = path;
			codecs[i].proc = proc;
			codecs[i].from_ini = from_ini;
			return 0;
		}
	struct vfw_codec *c = realloc(codecs, (ncodecs + 1) * sizeof(*c));
	if (!c) {
		free(path);
		return -1;
	}
	codecs = c;
	codecs[ncodecs++] = (struct vfw_codec){ type, handler, path, proc,
						 from_ini };
	return 0;
}

static void load_ini(void)
{
	FILE *f;
	char line[1024];
	int in_section = 0;

	compute_ini_path();
	loaded = 1;
	generation++;
	if (!(f = fopen(ini_path, "r")))
		return;
	while (fgets(line, sizeof(line), f)) {
		char *s = trim(line);
		if (!*s || *s == ';' || *s == '#')
			continue;
		if (*s == '[') {
			char *e = strchr(s, ']');
			if (e)
				*e = '\0';
			in_section = !strcasecmp(trim(s + 1), "drivers32");
			continue;
		}
		char *eq = strchr(s, '=');
		if (!in_section || !eq)
			continue;
		*eq = '\0';
		char *key = trim(s), *value = trim(eq + 1);
		size_t vl = strlen(value);
		if (vl >= 2 && value[0] == '"' && value[vl - 1] == '"') {
			value[vl - 1] = '\0';
			value++;
		}
		FOURCC type, handler;
		if (!*value || vfw_parse_fourcc(key, &type, &handler) ||
		    !strchr(key, '.')) {
			TRACE("%s: ignoring [drivers32] entry '%s'", ini_path, key);
			continue;
		}
		char *path = resolve(value);
		if (path)
			add(type, handler, path, NULL, 1);
	}
	fclose(f);
}

static void ensure_loaded(void)
{
	if (!loaded)
		load_ini();
}

const char *vfw_ini_path(void)
{
	pthread_mutex_lock(&lock);
	ensure_loaded();
	pthread_mutex_unlock(&lock);
	return ini_path;
}

void vfw_registry_reload(void)
{
	pthread_mutex_lock(&lock);
	for (unsigned i = 0; i < ncodecs; i++)
		free(codecs[i].path);
	free(codecs);
	codecs = NULL;
	ncodecs = 0;
	load_ini();
	pthread_mutex_unlock(&lock);
}

const struct vfw_codec *vfw_find(FOURCC type, FOURCC handler)
{
	const struct vfw_codec *r = NULL;

	pthread_mutex_lock(&lock);
	ensure_loaded();
	for (unsigned i = 0; i < ncodecs && !r; i++)
		if (same(codecs[i].type, type) && same(codecs[i].handler, handler))
			r = &codecs[i];
	pthread_mutex_unlock(&lock);
	return r;
}

const struct vfw_codec *vfw_enum(FOURCC type, unsigned n)
{
	const struct vfw_codec *r = NULL;

	pthread_mutex_lock(&lock);
	ensure_loaded();
	for (unsigned i = 0; i < ncodecs && !r; i++)
		if (!type || same(codecs[i].type, type)) {
			if (n == 0)
				r = &codecs[i];
			else
				n--;
		}
	pthread_mutex_unlock(&lock);
	return r;
}

const struct vfw_codec *vfw_find_path(const char *path)
{
	const struct vfw_codec *r = NULL;
	struct stat want, st;
	int have = !stat(path, &want);
	const char *base = strrchr(path, '/');

	base = base ? base + 1 : path;
	pthread_mutex_lock(&lock);
	ensure_loaded();
	for (unsigned i = 0; i < ncodecs && !r; i++) {
		const char *p = codecs[i].path;
		if (!p)
			continue;
		const char *b = strrchr(p, '/');
		if (have && !stat(p, &st) ? st.st_dev == want.st_dev &&
					     st.st_ino == want.st_ino :
		    !strcasecmp(b ? b + 1 : p, base))
			r = &codecs[i];
	}
	pthread_mutex_unlock(&lock);
	return r;
}

int vfw_register(FOURCC type, FOURCC handler, const char *path)
{
	char *copy = strdup(path);
	int r;

	if (!copy) {
		vfw_set_error("out of memory");
		return -1;
	}
	pthread_mutex_lock(&lock);
	ensure_loaded();
	r = add(type, handler, copy, NULL, 0);
	pthread_mutex_unlock(&lock);
	if (r)
		vfw_set_error("out of memory");
	return r;
}

int vfw_register_proc(FOURCC type, FOURCC handler, DRIVERPROC proc)
{
	pthread_mutex_lock(&lock);
	ensure_loaded();
	int r = add(type, handler, NULL, proc, 0);
	pthread_mutex_unlock(&lock);
	if (r)
		vfw_set_error("out of memory");
	return r;
}

int vfw_unregister(FOURCC type, FOURCC handler)
{
	int r = -1;

	pthread_mutex_lock(&lock);
	ensure_loaded();
	for (unsigned i = 0; i < ncodecs; i++)
		if (same(codecs[i].type, type) && same(codecs[i].handler, handler)) {
			free(codecs[i].path);
			codecs[i] = codecs[--ncodecs];
			generation++;
			r = 0;
			break;
		}
	pthread_mutex_unlock(&lock);
	return r;
}

int vfw_register_value(FOURCC type, FOURCC handler, const char *value)
{
	pthread_mutex_lock(&lock);
	ensure_loaded();
	char *path = resolve(value);
	int r = path ? add(type, handler, path, NULL, 0) : -1;
	pthread_mutex_unlock(&lock);
	if (r)
		vfw_set_error("out of memory");
	return r;
}

unsigned vfw_registry_generation(void)
{
	pthread_mutex_lock(&lock);
	ensure_loaded();
	unsigned g = generation;
	pthread_mutex_unlock(&lock);
	return g;
}
