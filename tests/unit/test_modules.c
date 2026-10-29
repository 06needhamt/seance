/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * Loading DLLs with their dependencies: import binding by name, ordinal and
 * data, forwarders, reference counts, DllMain and TLS notifications, page
 * protections, failure unwinding and delay-load imports.
 */
#include <signal.h>
#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>

#include "common/error.h"
#include "loader/modules.h"
#include "loader/pe.h"
#include "runtime/runtime.h"
#include "runtime/trap.h"
#include "test.h"

typedef int (*int_fn)(void);
typedef int (*add_fn)(int, int);
typedef void (*flag_fn)(int *);
typedef DWORD (*dword_fn)(void);

/* The permissions ("r-xp" etc.) of the mapping containing addr. */
static int page_perms(const void *addr, char perms[5])
{
	FILE *f = fopen("/proc/self/maps", "r");
	char line[512];
	int found = -1;

	if (!f)
		return -1;
	while (fgets(line, sizeof(line), f)) {
		unsigned long lo, hi;
		char p[5];
		if (sscanf(line, "%lx-%lx %4s", &lo, &hi, p) == 3 &&
		    (uintptr_t)addr >= lo && (uintptr_t)addr < hi) {
			memcpy(perms, p, 5);
			found = 0;
			break;
		}
	}
	fclose(f);
	return found;
}

static void test_dependencies(void)
{
	struct module *dep = mod_load(fixture("pe_dep.dll"), NULL);

	CHECK(dep != NULL);
	if (!dep) {
		fprintf(stderr, "  %s\n", vfw_error());
		return;
	}
	CHECK_STR(dep->name, "pe_dep.dll");
	CHECK_EQ(dep->refcount, 1);
	CHECK(dep->attached);

	/* The import was PE_BASIC.DLL; the file is pe_basic.dll. */
	struct module *basic = mod_find("PE_BASIC.DLL");
	CHECK(basic != NULL);
	if (!basic)
		return;
	CHECK_STR(basic->name, "pe_basic.dll");
	CHECK_EQ(basic->refcount, 1);
	CHECK(mod_find("pe_basic") == basic);	/* ".dll" is implied */

	/* Calls through name, data and ordinal imports. */
	int_fn compute = mod_get_proc(dep, "dep_compute");
	int_fn secret = mod_get_proc(dep, "dep_secret");
	CHECK(compute && secret);
	if (compute)
		CHECK_EQ(compute(), 35 + 0x1234);
	if (secret)
		CHECK_EQ(secret(), 0x5ec7e7);

	/* The forwarder resolves into pe_basic. */
	add_fn fwd = mod_get_proc(dep, "fwd_add");
	add_fn add = mod_get_proc(basic, "basic_add");
	CHECK(fwd && fwd == add);
	if (fwd)
		CHECK_EQ(fwd(40, 2), 42);
	CHECK(mod_get_ordinal(dep, 3) == (void *)add);

	/* By ordinal, including the ordinal-only export. */
	int_fn by_ord = mod_get_ordinal(basic, 7);
	CHECK(by_ord != NULL);
	if (by_ord)
		CHECK_EQ(by_ord(), 0x5ec7e7);
	CHECK(mod_get_ordinal(basic, 1) == (void *)add);

	/* Missing exports. */
	CHECK(mod_get_proc(basic, "no_such_export") == NULL);
	CHECK(strstr(vfw_error(), "pe_basic.dll: no export named no_such_export"));
	CHECK(mod_get_ordinal(basic, 99) == NULL);

	/* DllMain ran once. */
	int_fn attached = mod_get_proc(basic, "basic_attach_count");
	CHECK(attached && attached() == 1);

	/* Page protections follow the section flags. */
	char perms[5];
	CHECK_EQ(page_perms(add, perms), 0);
	CHECK_STR(perms, "r-xp");
	int *data = mod_get_proc(basic, "basic_data");
	CHECK(data != NULL);
	CHECK_EQ(page_perms(data, perms), 0);
	CHECK(perms[0] == 'r' && perms[1] == 'w' && perms[2] == '-');
	CHECK_EQ(page_perms(basic->img->base, perms), 0);	/* headers */
	CHECK_STR(perms, "r--p");

	/* Loading again adds a reference; the case and extension don't matter. */
	struct module *again = mod_load("PE_BASIC", dep);
	CHECK(again == basic);
	CHECK_EQ(basic->refcount, 2);
	mod_free(again);
	CHECK_EQ(basic->refcount, 1);

	/* Freeing pe_dep releases pe_basic and runs its DllMain(DETACH). */
	int detached = 0;
	flag_fn on_detach = mod_get_proc(basic, "basic_on_detach");
	CHECK(on_detach != NULL);
	if (on_detach)
		on_detach(&detached);
	mod_free(dep);
	CHECK(mod_find("pe_dep.dll") == NULL);
	CHECK(mod_find("pe_basic.dll") == NULL);
	CHECK_EQ(detached, 1);
}

static void test_tls(void)
{
	struct module *m = mod_load(fixture("pe_tls.dll"), NULL);

	CHECK(m != NULL);
	if (!m)
		return;
	dword_fn reasons = mod_get_proc(m, "tls_reasons");
	dword_fn index = mod_get_proc(m, "tls_index");
	dword_fn first = mod_get_proc(m, "tls_callback_first");
	CHECK(reasons && index && first);
	if (reasons && index && first) {
		CHECK_EQ(reasons(), 1u << DLL_PROCESS_ATTACH);
		CHECK(first() != 0);	/* the callback ran before DllMain */
		CHECK(m->img->has_tls);
		CHECK_EQ(index(), m->img->tls_index);
	}
	mod_free(m);
}

static void test_load_failures(void)
{
	/* A dependency that cannot be found; pe_basic must be released. */
	CHECK(mod_load(fixture("pe_missing.dll"), NULL) == NULL);
	CHECK(strstr(vfw_error(), "zz_nosuch.dll: not found") != NULL);
	CHECK(mod_find("pe_missing.dll") == NULL);
	CHECK(mod_find("pe_basic.dll") == NULL);

	/* DllMain refuses to attach. */
	CHECK(mod_load(fixture("pe_fail.dll"), NULL) == NULL);
	CHECK(strstr(vfw_error(), "DllMain(DLL_PROCESS_ATTACH) failed") != NULL);
	CHECK(mod_find("pe_fail.dll") == NULL);

	/* Not found, and not a PE file. */
	CHECK(mod_load("definitely_not_here.dll", NULL) == NULL);
	CHECK(strstr(vfw_error(), "not found") != NULL);
	CHECK(mod_load("/dev/null", NULL) == NULL);
}

static void test_search_path(void)
{
	/* A bare name is found through $VFW_PATH. */
	setenv("VFW_PATH", "/nonexistent", 1);
	CHECK(mod_load("pe_basic.dll", NULL) == NULL);

	char path[4096];
	snprintf(path, sizeof(path), "/nonexistent:%s", getenv("VFW_FIXTURES"));
	setenv("VFW_PATH", path, 1);
	struct module *m = mod_load("Pe_Basic", NULL);
	CHECK(m != NULL);
	mod_free(m);
	unsetenv("VFW_PATH");
}

static void test_force_reloc(void)
{
	setenv("VFW_FORCE_RELOC", "1", 1);
	struct module *dep = mod_load(fixture("pe_dep.dll"), NULL);
	CHECK(dep != NULL);
	if (dep) {
		struct module *basic = mod_find("pe_basic.dll");
		CHECK(dep->img->relocated);
		CHECK(basic && basic->img->relocated);
		int_fn compute = mod_get_proc(dep, "dep_compute");
		CHECK(compute && compute() == 35 + 0x1234);
		mod_free(dep);
	}

	/* An image without relocations cannot honour it. */
	CHECK(mod_load(fixture("pe_noreloc.dll"), NULL) == NULL);
	CHECK(strstr(vfw_error(), "no relocations") != NULL);
	unsetenv("VFW_FORCE_RELOC");
}

struct import_list {
	char text[16][64];
	int n;
};

static int list_import(void *ctx, const struct pe_import *imp)
{
	struct import_list *l = ctx;

	if (l->n < 16) {
		if (imp->name)
			snprintf(l->text[l->n], 64, "%s!%s", imp->dll, imp->name);
		else
			snprintf(l->text[l->n], 64, "%s!#%u", imp->dll,
				 imp->ordinal);
	}
	l->n++;
	return 0;
}

/*
 * pe_delay.dll with its delay import directory pointed at the tables it
 * exports as delay_directory (GNU ld leaves the directory empty), written
 * to dir. Returns the path, or NULL.
 */
static const char *patched_delay_dll(const char *dir)
{
	static char path[4200];
	void *data;
	size_t len;

	if (vfw_read_file(fixture("pe_delay.dll"), &data, &len))
		return NULL;
	struct pe_image *img = pe_parse(data, len);
	struct pe_export e;
	int ok = img && !pe_find_export(img, "delay_directory", 0, &e);
	if (ok) {
		size_t off = (size_t)((IMAGE_DOS_HEADER *)data)->e_lfanew + 4 +
			     sizeof(IMAGE_FILE_HEADER) +
			     offsetof(IMAGE_OPTIONAL_HEADER32, DataDirectory) +
			     IMAGE_DIRECTORY_ENTRY_DELAY_IMPORT * sizeof(IMAGE_DATA_DIRECTORY);
		IMAGE_DATA_DIRECTORY d = { e.rva, 3 * sizeof(IMAGE_DELAYLOAD_DESCRIPTOR) };
		memcpy((BYTE *)data + off, &d, sizeof(d));
		snprintf(path, sizeof(path), "%s/pe_delay.dll", dir);
		FILE *f = fopen(path, "wb");
		ok = f && fwrite(data, 1, len, f) == len;
		if (f)
			fclose(f);
	}
	pe_free(img);
	free(data);
	return ok ? path : NULL;
}

/* Runs fn from pe_delay.dll in a child; returns its status and stderr. */
static int run_trapping(struct module *m, const char *fn, char *err, size_t size)
{
	int fds[2];
	if (pipe(fds))
		return -1;
	fflush(NULL);
	pid_t pid = fork();
	if (!pid) {
		dup2(fds[1], 2);
		close(fds[0]);
		int_fn f = mod_get_proc(m, fn);
		if (f)
			f();
		_exit(3);
	}
	close(fds[1]);
	size_t n = 0;
	ssize_t r;
	while (n + 1 < size && (r = read(fds[0], err + n, size - 1 - n)) > 0)
		n += (size_t)r;
	err[n] = 0;
	close(fds[0]);
	int status = 0;
	waitpid(pid, &status, 0);
	return status;
}

static void test_delay_load(void)
{
	char dir[4096], err[1024];
	const char *t = getenv("TMPDIR");

	snprintf(dir, sizeof(dir), "%s/vfw-delay-XXXXXX", t && *t ? t : "/tmp");
	if (!mkdtemp(dir)) {
		CHECK(0);
		return;
	}
	const char *path = patched_delay_dll(dir);
	CHECK(path != NULL);
	if (!path)
		return;

	/* The delay imports, as info lists them. */
	struct pe_image *img = pe_open(path);
	CHECK(img != NULL);
	struct import_list l = { .n = 0 };
	CHECK(img && pe_foreach_delay_import(img, list_import, &l) == 0);
	CHECK_EQ(l.n, 4);
	CHECK_STR(l.text[0], "PE_BASIC.DLL!basic_add");
	CHECK_STR(l.text[1], "PE_BASIC.DLL!#1");
	CHECK_STR(l.text[2], "PE_BASIC.DLL!basic_gone");
	CHECK_STR(l.text[3], "zz_nosuch.dll!nosuch_function");
	pe_free(img);

	/* pe_basic.dll is found through VFW_PATH; zz_nosuch.dll nowhere. */
	setenv("VFW_PATH", getenv("VFW_FIXTURES"), 1);
	struct module *m = mod_load(path, NULL);
	unsetenv("VFW_PATH");
	CHECK(m != NULL);
	if (!m) {
		fprintf(stderr, "  %s\n", vfw_error());
		return;
	}
	add_fn add = mod_get_proc(m, "delay_add");
	add_fn add_ord = mod_get_proc(m, "delay_add_by_ordinal");
	dword_fn helper = mod_get_proc(m, "delay_helper_calls");
	HMODULE (*handle)(int) = mod_get_proc(m, "delay_module");
	CHECK(add && add_ord && helper && handle);
	struct module *basic = mod_find("pe_basic.dll");
	CHECK(basic != NULL);
	if (add && add_ord && helper && handle && basic) {
		CHECK_EQ(add(2, 3), 5);
		CHECK_EQ(add_ord(4, 5), 9);
		CHECK_EQ(helper(), 0);		/* bound at load: no lazy path */
		CHECK(handle(0) == (HMODULE)basic->base);
		CHECK(handle(1) == NULL);	/* never loaded */
	}

	/* What can't be had is a trap that says why. */
	{
		int st = run_trapping(m, "delay_nosuch", err, sizeof(err));
		CHECK(WIFEXITED(st) && WEXITSTATUS(st) == 128 + SIGABRT);
		CHECK(strstr(err, "zz_nosuch.dll!nosuch_function called from "
			      "pe_delay.dll+0x"));
		CHECK(strstr(err, "but that delay-loaded DLL could not be loaded"));
		st = run_trapping(m, "delay_gone", err, sizeof(err));
		CHECK(WIFEXITED(st) && WEXITSTATUS(st) == 128 + SIGABRT);
		CHECK(strstr(err, "pe_basic.dll!basic_gone called from pe_delay.dll"));
		CHECK(strstr(err, "but pe_basic.dll doesn't export it"));
	}

	/* Unloading releases what the delay imports loaded. */
	mod_free(m);
	CHECK(mod_find("pe_basic.dll") == NULL);
	unlink(path);
	rmdir(dir);
}

int main(void)
{
	/* Crashes and traps exit 128 + the signal, whatever we inherit. */
	unsetenv("VFW_CORE");
	test_dependencies();
	test_tls();
	test_load_failures();
	test_search_path();
	test_force_reloc();
	CHECK_EQ(runtime_enter(), 0);
	test_delay_load();
	return test_summary("test_modules");
}
