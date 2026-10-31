/* SPDX-License-Identifier: GPL-2.0-only */
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
#include "runtime/crash.h"
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

int mod_describe_address_try(const void *addr, char *out, size_t outsz)
{
	const BYTE *a = addr;
	int found = 0;

	if (pthread_mutex_trylock(&loader_lock))
		return 0;
	for (struct module *m = modules; m && !found; m = m->next)
		if (a >= m->base && a < m->base + module_size(m)) {
			snprintf(out, outsz, "%s+0x%x", m->name,
				 (unsigned)(a - m->base));
			found = 1;
		}
	UNLOCK();
	return found;
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

int mod_search(const char *name, const struct module *importer,
	       char *out, size_t outsz)
{
	return search(name, importer, out, outsz);
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

/*
 * Delay-load imports, bound at load time. Unlike a normal import, one that
 * can't be had doesn't stop the load: the DLL would only have failed when
 * it first called it, so it is bound to a trap saying why.
 */
struct delay_ctx {
	struct module *m;
	char failed[256];	/* the last DLL that couldn't be loaded */
	char why[160];
};

static void *resolve_delayed(void *ctx, const struct pe_import *imp)
{
	struct delay_ctx *d = ctx;
	struct module *dep = NULL;
	char why[160];

	if (strcasecmp(d->failed, imp->dll)) {
		dep = dep_get(d->m, imp->dll);
		if (!dep) {
			snprintf(d->failed, sizeof(d->failed), "%s", imp->dll);
			snprintf(d->why, sizeof(d->why),
				 "that delay-loaded DLL could not be loaded (%s)",
				 vfw_error());
			TRACE("%s: delay-loaded %s: %s", d->m->name, imp->dll,
			      vfw_error());
		}
	}
	if (!dep)
		return trap_thunk_why(imp->dll, imp->name, imp->ordinal, d->why);

	void *p = imp->name ? lookup_name(dep, imp->name, imp->ordinal, 0) :
			      lookup_ordinal(dep, imp->ordinal, 0);
	if (p)
		return p;
	if (dep->builtin)
		return trap_thunk(dep->name, imp->name, imp->ordinal);
	snprintf(why, sizeof(why), "%s doesn't export it", dep->name);
	TRACE("%s: delay-loaded %s!%s: not exported", d->m->name, imp->dll,
	      imp->name ? imp->name : "#");
	return trap_thunk_why(dep->name, imp->name, imp->ordinal, why);
}

static void *delay_handle(void *ctx, const char *dll)
{
	struct delay_ctx *d = ctx;
	char want[256];

	module_name(dll, want, sizeof(want));
	for (size_t i = 0; i < d->m->ndeps; i++)
		if (!strcasecmp(d->m->deps[i]->name, want))
			return d->m->deps[i]->base;
	return NULL;
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
static void detach_calls(void *arg)
{
	struct module *m = arg;

	pe_tls_notify(m->img, DLL_PROCESS_DETACH);
	pe_call_entry(m->img, DLL_PROCESS_DETACH);
}

/*
 * A DLL that crashes as it is unloaded (packed.dll, whose unpacker jumps
 * into memory gone by then) has done its work: the crash is reported,
 * then let go.
 */
static void detach(struct module *m)
{
	if (!m->img)
		return;
	if (crash_try(detach_calls, m))
		vfw_warn("%s crashed while being unloaded; carrying on",
			 m->name);
}

void mod_thread_notify(DWORD reason)
{
	LOCK();
	size_t n = 0;
	for (struct module *m = modules; m; m = m->next)
		n++;
	struct module **list = n ? malloc(n * sizeof(*list)) : NULL;
	if (list) {
		/* modules is newest first. */
		size_t i = n;
		for (struct module *m = modules; m; m = m->next)
			list[--i] = m;
		for (size_t k = 0; k < n; k++) {
			struct module *m = list[reason == DLL_THREAD_DETACH ? n - 1 - k : k];
			if (!m->img || !m->attached)
				continue;
			pe_tls_notify(m->img, reason);
			if (!m->no_thread_calls)
				pe_call_entry(m->img, reason);
		}
		free(list);
	}
	UNLOCK();
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

	/*
	 * Its work is done by a 16-bit DLL it thunks to (Mvicod32.dll, to
	 * MVICOD16.DLL), which can't run here; DllMain would only fail, or
	 * say "Wrong OS" first.
	 */
	const char *thunk = pe_win9x_thunk(img);
	if (thunk) {
		vfw_set_error("%s: a Windows 9x DLL that runs through a 16-bit "
			      "one (it imports %s, for thunks); only 32-bit code "
			      "runs here", base_name(path), thunk);
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
	struct delay_ctx delay = { .m = m };
	if (pe_bind_imports(img, resolve_import, m) ||
	    pe_bind_delay_imports(img, resolve_delayed, delay_handle, &delay) ||
	    pe_tls_init(img) ||
	    pe_protect(img)) {
		error_prefix(m->name);
		goto fail;
	}
	TRACE("loaded %s at %p%s", path, (void *)m->base,
	      img->relocated ? " (relocated)" : "");
	pe_tls_notify(img, DLL_PROCESS_ATTACH);
	unsigned mark = mod_missing_mark();
	if (!pe_call_entry(img, DLL_PROCESS_ATTACH)) {
		char looked[512];
		mod_missing_since(mark, looked, sizeof(looked));
		int one = !strchr(looked, ',');
		vfw_set_error("%s: DllMain(DLL_PROCESS_ATTACH) failed%s%s%s%s%s",
			      m->name, looked[0] ? "; it looked for " : "", looked,
			      !looked[0] ? "" : one ? ", which isn't here (add its" :
			      ", which aren't here (add their",
			      looked[0] ? " directory to VFW_PATH)" : "", "");
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

#define MAX_MISSING 16

static char *missing[MAX_MISSING];	/* a ring of the last ones */
static unsigned nmissing;		/* how many ever noted */
static pthread_mutex_t missing_lock = PTHREAD_MUTEX_INITIALIZER;

void mod_note_missing(const char *name)
{
	char *copy = strdup(name);

	if (!copy)
		return;
	pthread_mutex_lock(&missing_lock);
	free(missing[nmissing % MAX_MISSING]);
	missing[nmissing++ % MAX_MISSING] = copy;
	pthread_mutex_unlock(&missing_lock);
}

unsigned mod_missing_mark(void)
{
	pthread_mutex_lock(&missing_lock);
	unsigned n = nmissing;
	pthread_mutex_unlock(&missing_lock);
	return n;
}

void mod_missing_since(unsigned mark, char *buf, size_t size)
{
	size_t len = 0;

	buf[0] = 0;
	pthread_mutex_lock(&missing_lock);
	if (nmissing - mark > MAX_MISSING)
		mark = nmissing - MAX_MISSING;
	for (unsigned i = mark; i < nmissing; i++) {
		const char *name = missing[i % MAX_MISSING];
		int seen = 0;
		for (unsigned k = mark; k < i && !seen; k++)
			seen = !strcasecmp(missing[k % MAX_MISSING], name);
		if (!seen && len < size)
			len += snprintf(buf + len, size - len, "%s%s",
					len ? ", " : "", name);
	}
	pthread_mutex_unlock(&missing_lock);
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
			/* The DLL missing, not one that imports it. */
			mod_note_missing(want);
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
		struct module *m = new_module("seance.exe", NULL);
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

size_t mod_list(HMODULE *out, size_t max)
{
	struct module *main = mod_main();
	size_t n = 0;

	LOCK();
	if (main && n++ < max)
		out[0] = (HMODULE)main->base;
	for (struct module *m = modules; m; m = m->next)
		if (m != main && n++ < max)
			out[n - 1] = (HMODULE)m->base;
	UNLOCK();
	return n;
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
