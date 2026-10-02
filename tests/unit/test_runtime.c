/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * The runtime core: TEBs behind %fs, TLS slots, built-in modules and trap
 * thunks for unimplemented imports.
 */
#include <pthread.h>
#include <signal.h>
#include <string.h>
#include <sys/wait.h>

#include "common/error.h"
#include "loader/modules.h"
#include "runtime/builtin.h"
#include "runtime/runtime.h"
#include "runtime/teb.h"
#include "test.h"

static DWORD fs_read(DWORD offset)
{
	DWORD v;
	__asm__ volatile("movl %%fs:(%1), %0" : "=r"(v) : "r"(offset));
	return v;
}

static void test_teb(void)
{
	CHECK_EQ(runtime_enter(), 0);
	struct win_teb *teb = teb_current();
	CHECK(teb != NULL);
	if (!teb)
		return;

	CHECK(teb_init_thread() == teb);	/* idempotent */
	CHECK_EQ(fs_read(0x18), (DWORD)(uintptr_t)teb);
	CHECK_EQ(fs_read(0x00), 0xffffffff);	/* empty SEH chain */
	CHECK_EQ(fs_read(0x30), (DWORD)(uintptr_t)peb_get());
	CHECK_EQ(fs_read(0x20), (DWORD)getpid());
	/* The frame address, not a local: ASan may move locals off the stack. */
	BYTE *frame = __builtin_frame_address(0);
	CHECK((BYTE *)teb->StackLimit < frame && frame < (BYTE *)teb->StackBase);

	struct win_peb *peb = peb_get();
	CHECK_EQ(peb->OSMajorVersion, 5);
	CHECK_EQ(peb->OSMinorVersion, 1);
	CHECK(peb->NumberOfProcessors >= 1);

	WORD cw;
	__asm__ volatile("fnstcw %0" : "=m"(cw));
	CHECK_EQ(cw, 0x27f);
}

struct thread_result {
	struct win_teb *teb;
	DWORD fs_self;
	void *slot_value;
	DWORD tid;
};

static DWORD shared_slot;

static void *thread_main(void *arg)
{
	struct thread_result *r = arg;

	if (runtime_enter())
		return NULL;
	r->teb = teb_current();
	r->fs_self = fs_read(0x18);
	r->slot_value = teb_tls_get(shared_slot);	/* not the main thread's */
	teb_tls_set(shared_slot, (void *)0x5678);
	r->tid = fs_read(0x24);
	teb_exit_thread();
	return NULL;
}

static void test_threads_and_tls(void)
{
	shared_slot = teb_tls_alloc();
	CHECK(shared_slot != 0xffffffff);
	CHECK(teb_tls_valid(shared_slot));
	CHECK(teb_tls_get(shared_slot) == NULL);
	CHECK(teb_tls_set(shared_slot, (void *)0x1234));
	CHECK(teb_tls_get(shared_slot) == (void *)0x1234);
	CHECK(teb_current()->TlsSlots[shared_slot] == (void *)0x1234);

	struct thread_result r = { 0 };
	pthread_t t;
	CHECK_EQ(pthread_create(&t, NULL, thread_main, &r), 0);
	pthread_join(t, NULL);
	CHECK(r.teb != NULL && r.teb != teb_current());
	CHECK_EQ(r.fs_self, (DWORD)(uintptr_t)r.teb);
	CHECK(r.slot_value == NULL);
	CHECK(r.tid != 0 && r.tid != fs_read(0x24));
	/* The other thread's write didn't touch ours. */
	CHECK(teb_tls_get(shared_slot) == (void *)0x1234);

	/* Slots past the 64 in the TEB go to the expansion array. */
	DWORD slots[70];
	int n = 0;
	for (; n < 70; n++)
		if ((slots[n] = teb_tls_alloc()) == 0xffffffff)
			break;
	CHECK_EQ(n, 70);
	DWORD high = slots[69];
	CHECK(high >= TEB_TLS_SLOTS);
	CHECK(teb_tls_set(high, (void *)0x9abc));
	CHECK(teb_tls_get(high) == (void *)0x9abc);
	CHECK(teb_current()->TlsExpansionSlots != NULL);

	/* Freeing clears the slot, and it can be handed out again. */
	for (int i = 0; i < n; i++)
		CHECK(teb_tls_free(slots[i]));
	CHECK(!teb_tls_free(slots[0]));
	CHECK(teb_tls_free(shared_slot));
	DWORD again = teb_tls_alloc();
	CHECK_EQ(again, shared_slot);
	CHECK(teb_tls_get(again) == NULL);
	teb_tls_free(again);
	CHECK(!teb_tls_valid(TEB_TLS_MAX));
}

static void test_builtin_tables(void)
{
	/* Names must be unique within each DLL, and all findable. */
	for (const struct builtin_dll *const *d = builtin_dlls; *d; d++) {
		const struct builtin_dll *dll = *d;
		size_t n = builtin_count(dll);
		int unique = 1;

		/*
		 * Every DLL has exports. fallback (msvcrt.dll's) governs
		 * precedence (a real DLL of that name on the search path
		 * still wins), not whether there's anything behind it.
		 */
		CHECK(n > 0);
		CHECK(!dll->fallback || !strcmp(dll->name, "msvcrt.dll"));
		for (size_t i = 0; i < n; i++) {
			const struct builtin_export *e = builtin_entry(dll, i);
			const struct builtin_export *prev =
				i ? builtin_entry(dll, i - 1) : NULL;
			if (prev && !strcmp(prev->name, e->name)) {
				fprintf(stderr, "%s exports %s twice\n",
					dll->name, e->name);
				unique = 0;
			}
			CHECK(e->addr != NULL);
		}
		CHECK(unique);
		for (size_t i = 0; i < n && unique; i++) {
			const struct builtin_export *e = builtin_entry(dll, i);
			CHECK(builtin_export(dll, e->name) == e->addr);
		}
	}
	CHECK(builtin_entry(builtin_dlls[0], builtin_count(builtin_dlls[0])) == NULL);

	CHECK(builtin_find("KERNEL32.DLL") != NULL);
	CHECK(builtin_find("kernel32.dll") == builtin_find("Kernel32.Dll"));
	CHECK(builtin_find("kernel33.dll") == NULL);
	CHECK(builtin_find("pe_basic.dll") == NULL);

	/* The system DLLs a codec may import, and their aliases. */
	static const char *const names[] = {
		"winmm.dll", "msvfw32.dll", "user32.dll", "gdi32.dll",
		"advapi32.dll", "version.dll", "ole32.dll", "msvcrt.dll",
		"MSVCR71.DLL", "msvcrt20.dll", "ucrtbase.dll",
		"api-ms-win-crt-runtime-l1-1-0.dll",
	};
	for (size_t i = 0; i < sizeof(names) / sizeof(names[0]); i++)
		if (!builtin_find(names[i])) {
			fprintf(stderr, "no built-in for %s\n", names[i]);
			CHECK(0);
		}
	CHECK(!builtin_find("winmm.dll")->fallback);
	CHECK(!builtin_find("msvfw32.dll")->fallback);
	CHECK(!builtin_find("version.dll")->fallback);
	CHECK(!builtin_find("ole32.dll")->fallback);
	CHECK(!builtin_find("shell32.dll")->fallback);
	CHECK(!builtin_find("oleaut32.dll")->fallback);
	CHECK(!builtin_find("comdlg32.dll")->fallback);
	CHECK(!builtin_find("comctl32.dll")->fallback);
	CHECK(builtin_find("msvcr90.dll") == builtin_find("msvcrt.dll"));
	CHECK(builtin_find("msvcrt.dll")->fallback);
	CHECK(builtin_find("msvcr.dll") == NULL);
}

/*
 * A fallback built-in is used only when no DLL of that name is found; a
 * native copy on the search path wins.
 */
static void test_fallback_builtins(void)
{
	struct module *m = mod_load("MSVCR71", NULL);
	CHECK(m && m->builtin && m->builtin->fallback && !m->img);
	if (m)
		CHECK(mod_get_proc(m, "printf") != NULL);
	mod_free(m);

	char dir[4096], dst[4200];
	const char *t = getenv("TMPDIR");
	snprintf(dir, sizeof(dir), "%s/vfw-rt-XXXXXX", t && *t ? t : "/tmp");
	if (!mkdtemp(dir)) {
		CHECK(0);
		return;
	}
	snprintf(dst, sizeof(dst), "%s/msvcr71.dll", dir);
	void *data;
	size_t len;
	CHECK_EQ(vfw_read_file(fixture("pe_basic.dll"), &data, &len), 0);
	FILE *f = fopen(dst, "wb");
	CHECK(f && fwrite(data, 1, len, f) == len);
	if (f)
		fclose(f);
	free(data);

	setenv("VFW_PATH", dir, 1);
	m = mod_load("msvcr71.dll", NULL);
	CHECK(m && !m->builtin && m->img);
	mod_free(m);
	unsetenv("VFW_PATH");
	unlink(dst);
	rmdir(dir);

	/* kernel32 is always the built-in one. */
	m = mod_load("kernel32.dll", NULL);
	CHECK(m && m->builtin && !m->builtin->fallback);
	mod_free(m);
}

static void test_builtin_modules(void)
{
	struct module *k32 = mod_load("KERNEL32", NULL);
	CHECK(k32 != NULL);
	if (!k32)
		return;
	CHECK(k32->builtin != NULL && k32->img == NULL);
	CHECK(mod_load("kernel32.dll", NULL) == k32);
	CHECK_EQ(k32->refcount, 2);
	mod_free(k32);

	CHECK(mod_get_proc(k32, "GetTickCount") != NULL);
	CHECK(mod_get_proc(k32, "SeanceTestMissing") == NULL);
	CHECK(strstr(vfw_error(), "not implemented") != NULL);
	CHECK(mod_from_base(k32->base) == k32);
	CHECK(mod_from_address(k32->base + 0x10) == k32);

	/* The HMODULE points at a valid (empty) PE header. */
	struct pe_image *hdr = pe_parse(k32->base, 0x1000);
	CHECK(hdr != NULL);
	if (hdr) {
		CHECK(hdr->nt.FileHeader.Characteristics & IMAGE_FILE_DLL);
		pe_free(hdr);
	}
	mod_free(k32);
	CHECK(mod_find("kernel32.dll") == NULL);

	struct module *exe = mod_main();
	CHECK(exe != NULL && mod_main() == exe);
	CHECK(peb_get()->ImageBaseAddress == exe->base);
	mod_free(exe);			/* never unloaded */
	CHECK(mod_from_base(exe->base) == exe);
}

typedef DWORD (*dword_arg_fn)(DWORD);
typedef DWORD (*dword_fn)(void);
typedef WORD (*word_fn)(void);

static void test_fixture(void)
{
	struct module *m = mod_load(fixture("pe_runtime.dll"), NULL);
	CHECK(m != NULL);
	if (!m) {
		fprintf(stderr, "  %s\n", vfw_error());
		return;
	}

	/* kernel32 is imported under two spellings but loaded once. */
	struct module *k32 = mod_find("kernel32.dll");
	CHECK(k32 && k32->builtin);
	CHECK_EQ(m->ndeps, 1);

	dword_arg_fn fs = mod_get_proc(m, "rt_fs");
	dword_arg_fn last_error = mod_get_proc(m, "rt_last_error");
	dword_fn tid = mod_get_proc(m, "rt_thread_id");
	word_fn fpu = mod_get_proc(m, "rt_fpu_control");
	CHECK(fs && last_error && tid && fpu);
	if (!fs || !last_error || !tid || !fpu)
		return;

	CHECK_EQ(fs(0x18), (DWORD)(uintptr_t)teb_current());
	CHECK_EQ(fs(0x24), tid());
	CHECK_EQ(last_error(1234), 1234);
	CHECK_EQ(teb_current()->LastErrorValue, 1234);
	CHECK_EQ(fpu(), 0x27f);

	/* Calling the unimplemented import stops the process with a report. */
	dword_fn missing = mod_get_proc(m, "rt_call_missing");
	int pipefd[2];
	CHECK(missing != NULL && pipe(pipefd) == 0);
	pid_t pid = fork();
	if (pid == 0) {
		dup2(pipefd[1], 2);
		missing();
		_exit(0);
	}
	close(pipefd[1]);
	char msg[512] = "";
	ssize_t len = read(pipefd[0], msg, sizeof(msg) - 1);
	msg[len > 0 ? len : 0] = '\0';
	close(pipefd[0]);
	int status;
	waitpid(pid, &status, 0);
	CHECK(WIFSIGNALED(status) && WTERMSIG(status) == SIGABRT);
	CHECK(strstr(msg, "unimplemented function kernel32.dll!SeanceTestMissing") ||
	      strstr(msg, "unimplemented function KERNEL32.dll!SeanceTestMissing"));
	CHECK(strstr(msg, "called from pe_runtime.dll+0x") != NULL);
	if (!strstr(msg, "called from pe_runtime.dll+0x"))
		fprintf(stderr, "  child said: %s", msg);

	mod_free(m);
	CHECK(mod_find("kernel32.dll") == NULL);
}

int main(void)
{
	test_teb();
	test_threads_and_tls();
	test_builtin_tables();
	test_builtin_modules();
	test_fixture();
	test_fallback_builtins();
	return test_summary("test_runtime");
}
