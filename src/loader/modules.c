/*
 * Module table: search, load with dependencies, resolve exports, unload.
 */
#include <dirent.h>
#include <limits.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <unistd.h>

#include "common/error.h"
#include "common/log.h"
#include "loader/modules.h"
#include "runtime/runtime.h"
#include "runtime/teb.h"
#include "runtime/trap.h"

#define MAX_FORWARD_DEPTH 16

static struct module *modules;
static struct module *main_module;
static pthread_mutex_t loader_lock = PTHREAD_RECURSIVE_MUTEX_INITIALIZER_NP;

#define LOCK()   pthread_mutex_lock(&loader_lock)
#define UNLOCK() pthread_mutex_unlock(&loader_lock)

static const char *base_name(const char *path)
{
	const char *s = strrchr(path, '/');
	return s ? s + 1 : path;
}

/* Like Windows, a module name without an extension means NAME.dll. */
static void module_name(const char *name, char *out, size_t outsz)
{
	name = base_name(name);
	snprintf(out, outsz, "%s%s", name, strchr(name, '.') ? "" : ".dll");
}

/* Prefixes the current error message with "<prefix>: ". */
static void error_prefix(const char *prefix)
{
	char msg[512];

	snprintf(msg, sizeof(msg), "%s", vfw_error());
	vfw_set_error("%s: %s", prefix, msg);
}

struct module *mod_find(const char *name)
{
	char want[256];
	struct module *found = NULL;

	module_name(name, want, sizeof(want));
	LOCK();
	for (struct module *m = modules; m && !found; m = m->next)
		if (!strcasecmp(m->name, want))
			found = m;
	UNLOCK();
	return found;
}

struct module *mod_from_base(const void *base)
{
	struct module *found = NULL;

	LOCK();
	for (struct module *m = modules; m && !found; m = m->next)
		if (m->base == base)
			found = m;
	if (!found && main_module && main_module->base == base)
		found = main_module;
	UNLOCK();
	return found;
}

static size_t module_size(const struct module *m)
{
	return m->img ? m->img->map_len : 0x1000;
}

struct module *mod_from_address(const void *addr)
{
	struct module *found = NULL;
	const BYTE *a = addr;

	LOCK();
	for (struct module *m = modules; m && !found; m = m->next)
		if (a >= m->base && a < m->base + module_size(m))
			found = m;
	UNLOCK();
	return found;
}

void mod_describe_address(const void *addr, char *out, size_t outsz)
{
	struct module *m = mod_from_address(addr);

	if (m)
		snprintf(out, outsz, "%s+0x%x", m->name,
			 (unsigned)((const BYTE *)addr - m->base));
	else
		snprintf(out, outsz, "%p", addr);
}

/* Looks for name in dir, ignoring case. Returns 0 and the path, or -1. */
static int find_in_dir(const char *dir, const char *name, char *out,
		       size_t outsz)
{
	snprintf(out, outsz, "%s/%s", dir, name);
	if (!access(out, R_OK))
		return 0;

	DIR *d = opendir(dir);
	if (!d)
		return -1;
	struct dirent *e;
	int found = -1;
	while ((e = readdir(d))) {
		if (!strcasecmp(e->d_name, name)) {
			snprintf(out, outsz, "%s/%s", dir, e->d_name);
			found = 0;
			break;
		}
	}
	closedir(d);
	return found;
}

static int search(const char *name, const struct module *importer,
		  char *out, size_t outsz)
{
	if (importer && importer->path) {
		char dir[PATH_MAX];
		const char *slash = strrchr(importer->path, '/');
		if (slash)
			snprintf(dir, sizeof(dir), "%.*s",
				 (int)(slash - importer->path), importer->path);
		else
			snprintf(dir, sizeof(dir), ".");
		if (!find_in_dir(dir, name, out, outsz))
			return 0;
	}

	const char *env = getenv("VFW_PATH");
	if (env) {
		char *copy = strdup(env), *save = NULL;
		for (char *dir = strtok_r(copy, ":", &save); dir;
		     dir = strtok_r(NULL, ":", &save)) {
			if (!find_in_dir(dir, name, out, outsz)) {
				free(copy);
				return 0;
			}
		}
		free(copy);
	}

	return find_in_dir(".", name, out, outsz);
}

static int force_reloc(void)
{
	const char *v = getenv("VFW_FORCE_RELOC");
	return v && *v && strcmp(v, "0");
}

/* The dependency of m called name, loading it (and holding a ref) if new. */
static struct module *dep_get(struct module *m, const char *name)
{
	char want[256];

	module_name(name, want, sizeof(want));
	if (!strcasecmp(m->name, want))
		return m;
	for (size_t i = 0; i < m->ndeps; i++)
		if (!strcasecmp(m->deps[i]->name, want))
			return m->deps[i];

	struct module **deps = realloc(m->deps, (m->ndeps + 1) * sizeof(*deps));
	if (!deps) {
		vfw_set_error("out of memory");
		return NULL;
	}
	m->deps = deps;
	struct module *dep = mod_load(name, m);
	if (dep)
		m->deps[m->ndeps++] = dep;
	return dep;
}

static void *lookup_name(struct module *m, const char *name, WORD hint,
			 int depth);
static void *lookup_ordinal(struct module *m, WORD ordinal, int depth);

static void *export_address(struct module *m, const struct pe_export *e,
			    int depth)
{
	if (!e->forwarder)
		return m->base + e->rva;

	const char *fwd = e->forwarder;
	const char *dot = strrchr(fwd, '.');
	if (!dot || dot == fwd || !dot[1]) {
		vfw_set_error("%s: bad forwarder '%s'", m->name, fwd);
		return NULL;
	}
	if (depth >= MAX_FORWARD_DEPTH) {
		vfw_set_error("%s: forwarder chain too deep at '%s'", m->name, fwd);
		return NULL;
	}

	char dll[256];
	snprintf(dll, sizeof(dll), "%.*s", (int)(dot - fwd), fwd);
	struct module *target = dep_get(m, dll);
	if (!target)
		return NULL;
	if (dot[1] == '#')
		return lookup_ordinal(target, (WORD)strtoul(dot + 2, NULL, 10),
				      depth + 1);
	return lookup_name(target, dot + 1, 0, depth + 1);
}

static void *lookup_name(struct module *m, const char *name, WORD hint,
			 int depth)
{
	struct pe_export e;

	if (m->builtin) {
		void *p = builtin_export(m->builtin, name);
		if (!p)
			vfw_set_error("%s: %s is not implemented", m->name, name);
		return p;
	}
	if (!m->img) {
		vfw_set_error("%s: no export named %s", m->name, name);
		return NULL;
	}
	if (pe_find_export(m->img, name, hint, &e)) {
		error_prefix(m->name);
		return NULL;
	}
	return export_address(m, &e, depth);
}

static void *lookup_ordinal(struct module *m, WORD ordinal, int depth)
{
	struct pe_export e;

	if (m->builtin) {
		void *p = builtin_export_ordinal(m->builtin, ordinal);
		if (!p)
			vfw_set_error("%s: ordinal %u is not implemented",
				      m->name, ordinal);
		return p;
	}
	if (!m->img) {
		vfw_set_error("%s: no export with ordinal %u", m->name, ordinal);
		return NULL;
	}
	if (pe_find_export_ordinal(m->img, ordinal, &e)) {
		error_prefix(m->name);
		return NULL;
	}
	return export_address(m, &e, depth);
}

void *mod_get_proc(struct module *mod, const char *name)
{
	LOCK();
	void *p = lookup_name(mod, name, 0, 0);
	UNLOCK();
	return p;
}

void *mod_get_ordinal(struct module *mod, WORD ordinal)
{
	LOCK();
	void *p = lookup_ordinal(mod, ordinal, 0);
	UNLOCK();
	return p;
}

static void *resolve_import(void *ctx, const struct pe_import *imp)
{
	struct module *m = ctx;
	struct module *dep = dep_get(m, imp->dll);

	if (!dep)
		return NULL;
	void *p = imp->name ? lookup_name(dep, imp->name, imp->ordinal, 0) :
			      lookup_ordinal(dep, imp->ordinal, 0);

	/*
	 * An unimplemented built-in function does not stop the load: the
	 * import is bound to a trap that reports it if it is ever called.
	 */
	if (!p && dep->builtin) {
		if (imp->name)
			TRACE("%s: %s!%s is not implemented; bound to a trap",
			      m->name, dep->name, imp->name);
		else
			TRACE("%s: %s!#%u is not implemented; bound to a trap",
			      m->name, dep->name, imp->ordinal);
		p = trap_thunk(dep->name, imp->name, imp->ordinal);
		if (!p)
			vfw_set_error("out of memory");
	}
	return p;
}

static void unlink_module(struct module *m)
{
	for (struct module **p = &modules; *p; p = &(*p)->next) {
		if (*p == m) {
			*p = m->next;
			return;
		}
	}
}

/*
 * Drops m from every other module's dependency list. Normally nothing still
 * lists a module being unloaded, but a failed load can leave one in a cycle
 * (A imports B imports A, then A fails).
 */
static void forget(struct module *m)
{
	for (struct module *x = modules; x; x = x->next) {
		size_t j = 0;
		for (size_t i = 0; i < x->ndeps; i++)
			if (x->deps[i] != m)
				x->deps[j++] = x->deps[i];
		x->ndeps = j;
	}
}

/* Frees m, then releases what it depended on. */
static void unload(struct module *m)
{
	struct module **deps = m->deps;
	size_t ndeps = m->ndeps;

	unlink_module(m);
	forget(m);
	if (m->img)
		pe_free(m->img);
	else
		builtin_free_image(m->base);
	free(m->name);
	free(m->path);
	free(m);
	for (size_t i = 0; i < ndeps; i++)
		mod_free(deps[i]);
	free(deps);
}

/* TLS callbacks run before DllMain, for detach as well as attach. */
static void detach(struct module *m)
{
	if (!m->img)
		return;
	pe_tls_notify(m->img, DLL_PROCESS_DETACH);
	pe_call_entry(m->img, DLL_PROCESS_DETACH);
}

static struct module *new_module(const char *name, const char *path)
{
	struct module *m = calloc(1, sizeof(*m));

	if (!m || !(m->name = strdup(name)) ||
	    (path && !(m->path = strdup(path)))) {
		vfw_set_error("out of memory");
		if (m)
			free(m->name);
		free(m);
		return NULL;
	}
	m->refcount = 1;
	return m;
}

static struct module *load_builtin(const char *name,
				   const struct builtin_dll *b)
{
	struct module *m = new_module(name, NULL);

	if (!m)
		return NULL;
	m->builtin = b;
	m->base = builtin_fake_image();
	if (!m->base) {
		vfw_set_error("%s: cannot map a module header", name);
		free(m->name);
		free(m);
		return NULL;
	}
	m->attached = 1;
	m->next = modules;
	modules = m;
	TRACE("loaded built-in %s", name);
	return m;
}

static struct module *load_pe(const char *path)
{
	struct pe_image *img = pe_open(path);
	if (!img)
		return NULL;
	if (!(img->nt.FileHeader.Characteristics & IMAGE_FILE_DLL)) {
		vfw_set_error("%s: not a DLL", path);
		pe_free(img);
		return NULL;
	}

	struct module *m = new_module(base_name(path), path);
	if (!m) {
		pe_free(img);
		return NULL;
	}
	m->img = img;
	/* In the table before its imports, so circular imports find it. */
	m->next = modules;
	modules = m;

	if (pe_map(img, force_reloc() ? PE_MAP_FORCE_RELOC : 0)) {
		error_prefix(m->name);
		goto fail;
	}
	m->base = img->base;
	if (pe_bind_imports(img, resolve_import, m) ||
	    pe_tls_init(img) ||
	    pe_protect(img)) {
		error_prefix(m->name);
		goto fail;
	}
	TRACE("loaded %s at %p%s", path, (void *)m->base,
	      img->relocated ? " (relocated)" : "");
	pe_tls_notify(img, DLL_PROCESS_ATTACH);
	if (!pe_call_entry(img, DLL_PROCESS_ATTACH)) {
		vfw_set_error("%s: DllMain(DLL_PROCESS_ATTACH) failed", m->name);
		/* Windows tells a DLL that refused to attach that it is detaching. */
		detach(m);
		goto fail;
	}
	m->attached = 1;
	return m;

fail:
	unload(m);
	return NULL;
}

struct module *mod_load(const char *name, struct module *importer)
{
	char want[256], path[PATH_MAX];
	struct module *m = NULL;

	if (runtime_enter())
		return NULL;
	module_name(name, want, sizeof(want));

	LOCK();
	if ((m = mod_find(want))) {
		m->refcount++;
		goto out;
	}

	if (strchr(name, '/')) {
		snprintf(path, sizeof(path), "%s", name);
	} else {
		const struct builtin_dll *b = builtin_find(want);
		if (b && !b->fallback) {
			m = load_builtin(want, b);
			goto out;
		}
		if (search(want, importer, path, sizeof(path))) {
			if (b) {
				m = load_builtin(want, b);
				goto out;
			}
			vfw_set_error("%s: not found (searched %s$VFW_PATH and the current directory)",
				      want, importer ? "the importer's directory, " : "");
			goto out;
		}
	}
	m = load_pe(path);
out:
	UNLOCK();
	return m;
}

struct module *mod_main(void)
{
	LOCK();
	if (!main_module) {
		struct module *m = new_module("vfw-harness.exe", NULL);
		if (m && (m->base = builtin_fake_image())) {
			m->attached = 1;
			main_module = m;
			peb_get()->ImageBaseAddress = m->base;
		} else if (m) {
			free(m->name);
			free(m);
		}
	}
	UNLOCK();
	return main_module;
}

void mod_addref(struct module *mod)
{
	if (!mod || mod == main_module)
		return;
	LOCK();
	mod->refcount++;
	UNLOCK();
}

void mod_pin(struct module *mod)
{
	if (!mod || mod == main_module)
		return;
	LOCK();
	mod->pinned = 1;
	UNLOCK();
}

void mod_free(struct module *mod)
{
	if (!mod || mod == main_module || mod->pinned)
		return;
	LOCK();
	if (--mod->refcount <= 0) {
		if (mod->attached)
			detach(mod);
		unload(mod);
	}
	UNLOCK();
}
