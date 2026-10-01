/*
 * msvcrt's C++ exception support: pe_cxxeh.dll throws and catches as cl
 * compiles C++ (by value, by reference to a base, catch (...), rethrow,
 * a throw from a catch block, a try in a catch block, destructors of the
 * frames in between, SEH exceptions with and without /EHs); an unhandled
 * exception and one leaving a noexcept function end the process, checked
 * in a child. type_info, std::exception, operator new and the handler
 * setters are called directly.
 */
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>

#include "common/sanitizer.h"
#include "loader/modules.h"
#include "runtime/runtime.h"
#include "test.h"

#define THISCALL __attribute__((thiscall))

static struct module *crt;

static void *get(const char *name)
{
	void *p = mod_get_proc(crt, name);
	if (!p) {
		fprintf(stderr, "msvcrt has no %s\n", name);
		exit(1);
	}
	return p;
}

/* Runs fn from the fixture in a child; its wait status and stderr. */
static int run_child(const char *dll, const char *fn, char *err, size_t size)
{
	int fds[2];
	if (pipe(fds))
		return -1;
	pid_t pid = fork();
	if (pid < 0)
		return -1;
	if (!pid) {
		dup2(fds[1], STDERR_FILENO);
		close(fds[0]);
		close(fds[1]);
		if (runtime_enter())
			_exit(90);
		struct module *m = mod_load(dll, NULL);
		void (WINAPI *f)(void) = m ? mod_get_proc(m, fn) : NULL;
		if (!f)
			_exit(91);
		f();
		_exit(92);	/* it should not come back */
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

static void test_fixture(void)
{
	const char *dll = fixture("pe_cxxeh.dll");
	struct module *m = mod_load(dll, NULL);
	CHECK(m != NULL);
	if (!m)
		return;
	int (WINAPI *run)(void) = mod_get_proc(m, "cxx_run");
	CHECK(run != NULL);
	if (run) {
		int r = run();
		if (r)
			fprintf(stderr, "  cxx_run failed at check %d\n", r);
		CHECK_EQ(r, 0);
	}
	mod_free(m);

	char err[1024];
	int st = run_child(dll, "cxx_unhandled", err, sizeof(err));
	CHECK(WIFSIGNALED(st) || (WIFEXITED(st) && WEXITSTATUS(st) == 134));
	CHECK(strstr(err, "unhandled exception 0xE06D7363") != NULL);

	st = run_child(dll, "cxx_noexcept", err, sizeof(err));
	CHECK(WIFEXITED(st) && WEXITSTATUS(st) == 42);	/* its terminate handler */
}

/* A type descriptor as MSVC emits one. */
struct type_desc {
	const void *vtable;
	char *name;
	char mangled[32];
};

static void test_type_info(void)
{
	typedef const char *(THISCALL *name_fn)(struct type_desc *);
	typedef int (THISCALL *cmp_fn)(const struct type_desc *,
				       const struct type_desc *);
	name_fn name = get("?name@type_info@@QBEPBDXZ");
	name_fn raw = get("?raw_name@type_info@@QBEPBDXZ");
	cmp_fn eq = get("??8type_info@@QBEHABV0@@Z");
	cmp_fn ne = get("??9type_info@@QBEHABV0@@Z");
	cmp_fn before = get("?before@type_info@@QBEHABV0@@Z");
	const void *const *vtable = get("??_7type_info@@6B@");
	const char *(CDECL *std_name)(void *, void *) = get("__std_type_info_name");
	int (CDECL *std_cmp)(const void *, const void *) =
		get("__std_type_info_compare");
	size_t (CDECL *std_hash)(const void *) = get("__std_type_info_hash");
	static const struct {
		const char *mangled, *name;
	} names[] = {
		{ ".?AVDerived@@", "class Derived" },
		{ ".?AUpoint@geo@@", "struct geo::point" },
		{ ".?AW4mode@@", "enum mode" },
		{ ".H", "int" },
		{ ".PBD", "char const *" },
		{ ".PAVcodec@vfw@@", "class vfw::codec *" },
		{ "._N", "bool" },
		{ ".?AV?$vector@H@std@@", "?AV?$vector@H@std@@" },	/* templates: as is */
	};

	CHECK(vtable[0] != NULL);
	for (size_t i = 0; i < sizeof(names) / sizeof(names[0]); i++) {
		struct type_desc t = { vtable, NULL, "" };
		strcpy(t.mangled, names[i].mangled);
		CHECK_STR(name(&t), names[i].name);
		CHECK(t.name && !strcmp(t.name, names[i].name));	/* kept */
		CHECK_STR(raw(&t), names[i].mangled);
		CHECK_STR(std_name(&t.name, NULL), names[i].name);
		free(t.name);
	}

	struct type_desc a = { vtable, NULL, ".?AVA@@" }, a2 = { vtable, NULL, ".?AVA@@" },
			 b = { vtable, NULL, ".?AVB@@" };
	CHECK(eq(&a, &a2) && !ne(&a, &a2) && ne(&a, &b));
	CHECK(before(&a, &b) && !before(&b, &a));
	CHECK_EQ(std_cmp(&a.name, &a2.name), 0);
	CHECK(std_cmp(&a.name, &b.name) < 0);
	CHECK(std_hash(&a.name) == std_hash(&a2.name));
	CHECK(std_hash(&a.name) != std_hash(&b.name));
}

struct exception {
	const void *const *vtable;
	char *name;
	int do_free;
};

static void test_exception(void)
{
	typedef struct exception *(THISCALL *ctor_fn)(struct exception *);
	typedef struct exception *(THISCALL *ctor_name_fn)(struct exception *,
							   const char *const *);
	typedef struct exception *(THISCALL *copy_fn)(struct exception *,
						      const struct exception *);
	typedef void (THISCALL *dtor_fn)(struct exception *);
	typedef const char *(THISCALL *what_fn)(const struct exception *);
	ctor_fn ctor = get("??0exception@@QAE@XZ");
	ctor_name_fn ctor_name = get("??0exception@@QAE@ABQBD@Z");
	copy_fn copy = get("??0exception@@QAE@ABV0@@Z");
	copy_fn assign = get("??4exception@@QAEAAV0@ABV0@@Z");
	dtor_fn dtor = get("??1exception@@UAE@XZ");
	what_fn what = get("?what@exception@@UBEPBDXZ");
	const void *const *vtable = get("??_7exception@@6B@");
	struct exception e, c, d;

	CHECK(ctor(&e) == &e && e.vtable == vtable);
	CHECK_STR(what(&e), "Unknown exception");
	/* The vtable's what() is the same function. */
	CHECK(vtable[1] == (const void *)what);

	char buf[16] = "bad codec";
	const char *p = buf;
	CHECK(ctor_name(&c, &p) == &c && c.do_free && c.name != buf);
	buf[0] = 'X';	/* copied, so unchanged */
	CHECK_STR(what(&c), "bad codec");
	CHECK(copy(&d, &c) == &d && d.name != c.name);
	CHECK_STR(what(&d), "bad codec");
	CHECK(assign(&e, &d) == &e);
	CHECK_STR(what(&e), "bad codec");
	dtor(&c);
	dtor(&d);
	dtor(&e);
	CHECK(!e.name && !e.do_free);

	struct { const char *what; unsigned char do_free; } s = { "msg", 1 }, t;
	void (CDECL *std_copy)(const void *, void *) = get("__std_exception_copy");
	void (CDECL *std_destroy)(void *) = get("__std_exception_destroy");
	std_copy(&s, &t);
	CHECK(t.what != s.what && t.do_free && !strcmp(t.what, "msg"));
	std_destroy(&t);
	CHECK(!t.what && !t.do_free);
	s.do_free = 0;
	std_copy(&s, &t);
	CHECK(t.what == s.what && !t.do_free);
}

static int handler_calls;

static int CDECL new_handler(size_t size)
{
	return ++handler_calls < 3;	/* ask for two retries, then give up */
}

static void CDECL term_a(void) { }
static void CDECL term_b(void) { }

static void test_misc(void)
{
	void *(CDECL *op_new)(size_t) = get("??2@YAPAXI@Z");
	void (CDECL *op_delete)(void *) = get("??3@YAXPAX@Z");
	void *(CDECL *op_new_array)(size_t) = get("??_U@YAPAXI@Z");
	void (CDECL *op_delete_sized)(void *, size_t) = get("??3@YAXPAXI@Z");
	void (CDECL *op_delete_array_sized)(void *, size_t) = get("??_V@YAXPAXI@Z");
	int (CDECL *set_new_mode)(int) = get("_set_new_mode");
	int (CDECL *callnewh)(size_t) = get("_callnewh");
	void *(CDECL *crt_malloc)(size_t) = get("malloc");
	void *(CDECL *crt_calloc)(size_t, size_t) = get("calloc");
	void *(CDECL *crt_realloc)(void *, size_t) = get("realloc");
	void (CDECL *crt_free)(void *) = get("free");
	int (CDECL *(CDECL *set_new_handler)(int (CDECL *)(size_t)))(size_t) =
		get("?_set_new_handler@@YAP6AHI@ZP6AHI@Z@Z");
	void (CDECL *(CDECL *set_terminate)(void (CDECL *)(void)))(void) =
		get("?set_terminate@@YAP6AXXZP6AXXZ@Z");
	void (CDECL *(CDECL *get_terminate)(void))(void) = get("_get_terminate");
	int (CDECL *uncaught)(void) = get("__uncaught_exception");

	char *p = op_new(10);
	CHECK(p != NULL);
	memset(p, 1, 10);
	op_delete(p);
	CHECK((p = op_new(0)) != NULL);	/* a unique pointer, even for 0 */
	op_delete(p);
	CHECK((p = op_new_array(32)) != NULL);
	op_delete(p);
	CHECK((p = op_new(24)) != NULL);
	op_delete_sized(p, 24);
	CHECK((p = op_new_array(48)) != NULL);
	op_delete_array_sized(p, 48);

	/*
	 * Out of memory: the new handler is asked until it gives up. (The
	 * sanitizers' allocator reports an impossible size instead.)
	 */
	CHECK(set_new_handler(new_handler) == NULL);
	if (!VFW_SANITIZER_BUILD) {
		CHECK(op_new((size_t)-64) == NULL);
		CHECK_EQ(handler_calls, 3);
	}
	handler_calls = 0;
	CHECK_EQ(callnewh(1), 1);
	CHECK_EQ(handler_calls, 1);

	/* malloc, calloc and realloc ask it only under _set_new_mode(1). */
	if (!VFW_SANITIZER_BUILD) {
		handler_calls = 0;
		CHECK(crt_malloc((size_t)-64) == NULL);
		CHECK_EQ(handler_calls, 0);
		CHECK_EQ(set_new_mode(1), 0);
		CHECK(crt_malloc((size_t)-64) == NULL);
		CHECK_EQ(handler_calls, 3);
		handler_calls = 0;
		CHECK(crt_calloc((size_t)-64, 1) == NULL);
		CHECK_EQ(handler_calls, 3);
		handler_calls = 0;
		CHECK(crt_calloc(0x10000, 0x10000) == NULL);	/* overflows */
		CHECK_EQ(handler_calls, 0);
		CHECK((p = crt_malloc(16)) != NULL);
		CHECK(crt_realloc(p, (size_t)-64) == NULL);
		CHECK_EQ(handler_calls, 3);
		crt_free(p);
		CHECK_EQ(set_new_mode(0), 1);
	}
	CHECK(set_new_handler(NULL) == new_handler);
	CHECK_EQ(callnewh(1), 0);

	CHECK(set_terminate(term_a) == NULL);
	CHECK(set_terminate(term_b) == term_a);
	CHECK(get_terminate() == term_b);
	set_terminate(NULL);
	CHECK(!uncaught());

	/* vcruntime140 is served by the same CRT. */
	struct module *vc = mod_load("vcruntime140.dll", NULL);
	CHECK(vc && vc->builtin == crt->builtin);
	CHECK(vc && mod_get_proc(vc, "__CxxFrameHandler3") ==
		    mod_get_proc(crt, "__CxxFrameHandler"));
	mod_free(vc);
}

int main(void)
{
	CHECK_EQ(runtime_enter(), 0);
	crt = mod_load("msvcrt.dll", NULL);
	CHECK(crt && crt->builtin);
	if (!crt)
		return test_summary("test_cxxeh");

	test_type_info();
	test_exception();
	test_misc();
	test_fixture();

	mod_free(crt);
	return test_summary("test_cxxeh");
}
