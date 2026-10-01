/*
 * msvcrt's C++ exception support: pe_cxxeh.dll throws and catches as cl
 * compiles C++ (by value, by reference to a base, catch (...), rethrow,
 * a throw from a catch block, a try in a catch block, destructors of the
 * frames in between, SEH exceptions with and without /EHs); an unhandled
 * exception and one leaving a noexcept function end the process, checked
 * in a child, as is RTTI that faults to read (the harness's fault handler
 * makes that an exception). The fixture also covers the SE translator,
 * exception specifications, the RTTI exceptions and longjmp.
 *
 * type_info, std::exception and its subclasses, the RTTI casts over
 * multiple inheritance, C++/CLI's filter helpers, operator new and the
 * handler setters are called directly.
 */
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>

#include "common/sanitizer.h"
#include "loader/modules.h"
#include "runtime/crash.h"
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

/*
 * Runs fn from the fixture in a child, with the harness's fault handler
 * if crash; its wait status (exit 0 if fn returned 0) and stderr.
 */
static int run_child(const char *dll, const char *fn, char *err, size_t size,
		     int crash)
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
		if ((crash && crash_install()) || runtime_enter())
			_exit(90);
		struct module *m = mod_load(dll, NULL);
		int (WINAPI *f)(void) = m ? mod_get_proc(m, fn) : NULL;
		if (!f)
			_exit(91);
		_exit(f() ? 92 : 0);	/* if it comes back at all */
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
	int st = run_child(dll, "cxx_unhandled", err, sizeof(err), 0);
	CHECK(WIFSIGNALED(st) || (WIFEXITED(st) && WEXITSTATUS(st) == 134));
	CHECK(strstr(err, "unhandled exception 0xE06D7363") != NULL);

	st = run_child(dll, "cxx_noexcept", err, sizeof(err), 0);
	CHECK(WIFEXITED(st) && WEXITSTATUS(st) == 42);	/* its terminate handler */

	/* Sanitizer builds leave faults to the sanitizer. */
	if (!VFW_SANITIZER_BUILD) {
		st = run_child(dll, "cxx_rtti_fault", err, sizeof(err), 1);
		CHECK(WIFEXITED(st) && WEXITSTATUS(st) == 0);
	}
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

/* The subclasses: one class each, by both names, with RTTI of their own. */
static void test_subclasses(void)
{
	typedef struct exception *(THISCALL *ctor_fn)(struct exception *);
	typedef struct exception *(THISCALL *ctor_str_fn)(struct exception *,
							  const char *);
	typedef struct exception *(THISCALL *ctor_ref_fn)(struct exception *,
							  const char *const *);
	typedef struct exception *(THISCALL *copy_fn)(struct exception *,
						      const struct exception *);
	typedef void *(THISCALL *vdtor_fn)(struct exception *, unsigned);
	typedef const char *(THISCALL *what_fn)(const struct exception *);
	ctor_str_fn cast_ctor = get("??0bad_cast@@QAE@PBD@Z");
	ctor_ref_fn cast_ctor_ref = get("??0bad_cast@@QAE@ABQBD@Z");
	ctor_fn cast_default = get("??_Fbad_cast@std@@QAEXXZ");
	copy_fn cast_copy = get("??0bad_cast@std@@QAE@ABV01@@Z");
	ctor_fn typeid_default = get("??_Fbad_typeid@@QAEXXZ");
	ctor_str_fn non_rtti_ctor = get("??0__non_rtti_object@std@@QAE@PBD@Z");
	ctor_ref_fn exc_ctor = get("??0exception@std@@QAE@ABQBD@Z");
	ctor_ref_fn exc_noalloc = (ctor_ref_fn)get("??0exception@@QAE@ABQBDH@Z");
	copy_fn assign = get("??4exception@@QAEAAV0@ABV0@@Z");
	vdtor_fn vector_dtor = get("??_Ebad_cast@@UAEPAXI@Z");
	vdtor_fn scalar_dtor = get("??_G__non_rtti_object@@UAEPAXI@Z");
	what_fn what = get("?what@exception@std@@UBEPBDXZ");
	const void *const *cast_vt = get("??_7bad_cast@@6B@");
	const void *const *typeid_vt = get("??_7bad_typeid@std@@6B@");
	const void *const *non_rtti_vt = get("??_7__non_rtti_object@@6B@");
	const void *(CDECL *rttypeid)(void *) = get("__RTtypeid");
	void *(CDECL *rtcast)(void *, int, const void *, const void *, int) =
		get("__RTDynamicCast");
	typedef const char *(THISCALL *name_fn)(const void *);
	name_fn ti_name = get("?name@type_info@@QBEPBDXZ");
	struct exception a, b, c, d;
	const char *msg = "cast";

	CHECK(get("??_7bad_cast@std@@6B@") == cast_vt);
	CHECK(cast_ctor(&a, "x") == &a && a.vtable == cast_vt && a.do_free);
	CHECK_STR(what(&a), "x");
	CHECK(cast_ctor_ref(&b, &msg) == &b && b.vtable == cast_vt);
	CHECK(cast_default(&c) == &c && c.vtable == cast_vt);
	CHECK_STR(what(&c), "bad cast");
	CHECK(cast_copy(&d, &b) == &d && d.vtable == cast_vt && d.name != b.name);
	CHECK_STR(what(&d), "cast");

	/* operator= keeps the left side's class. */
	struct exception e;
	CHECK(exc_ctor(&e, &msg) == &e && e.vtable == get("??_7exception@@6B@"));
	CHECK(assign(&d, &e) == &d && d.vtable == cast_vt);
	CHECK_STR(what(&d), "cast");

	/* typeid() of them, and a cast to std::exception by its later name. */
	const void *ti = rttypeid(&a);
	CHECK(ti != NULL);
	if (ti)
		CHECK_STR(ti_name(ti), "class bad_cast");
	struct type_desc std_exc = { get("??_7type_info@@6B@"), NULL,
				     ".?AVexception@std@@" };
	CHECK(rtcast(&a, 0, ti, &std_exc, 0) == &a);
	struct exception t, n;
	CHECK(typeid_default(&t) == &t && t.vtable == typeid_vt);
	CHECK_STR(what(&t), "bad typeid");
	CHECK(non_rtti_ctor(&n, "nr") == &n && n.vtable == non_rtti_vt);
	struct type_desc typeid_d = { std_exc.vtable, NULL, ".?AVbad_typeid@@" };
	CHECK(rtcast(&n, 0, rttypeid(&n), &typeid_d, 0) == &n);	/* its base */
	CHECK(rtcast(&n, 0, rttypeid(&n), &std_exc, 0) == &n);
	CHECK(!rtcast(&t, 0, rttypeid(&t), ti, 0));		/* not a bad_cast */

	scalar_dtor(&n, 0);
	CHECK(!n.name);
	vector_dtor(&a, 0);
	vector_dtor(&b, 0);
	vector_dtor(&c, 0);
	vector_dtor(&t, 0);
	vector_dtor(&e, 0);

	/* exception(const char *const &, int): the name not copied. */
	CHECK(exc_noalloc(&e, &msg) == &e && e.name == msg && !e.do_free);

	/* delete[] through the vector deleting destructor: each, then the block. */
	INT_PTR *block = malloc(sizeof(INT_PTR) + 2 * sizeof(struct exception));
	CHECK(block != NULL);
	if (block) {
		struct exception *arr = (struct exception *)(block + 1);
		block[0] = 2;
		cast_ctor(&arr[0], "one");
		cast_ctor(&arr[1], "two");
		CHECK(vector_dtor(arr, 3) == arr);
	}
	vector_dtor(&d, 0);	/* the destructor alone */
	CHECK(!d.name && !d.do_free);
}

/* RTTI as cl lays it out, for the casts over multiple inheritance. */
struct rtti_base {
	const struct type_desc *type;
	int nbases;
	int this_offset, vbase_descr, vbase_offset;
	unsigned attributes;
};

struct rtti_hierarchy {
	unsigned signature, attributes;
	int nbases;
	const struct rtti_base *const *bases;
};

struct rtti_locator {
	unsigned signature;
	int offset;
	unsigned cd_offset;
	const struct type_desc *type;
	const struct rtti_hierarchy *hierarchy;
};

#define MULTINH 1
#define NOTVISIBLE 1
#define AMBIGUOUS 2

/* An object with vftable pointers at 0 and 8. */
struct two_vt {
	const void *vt0;
	int a;
	const void *vt8;
	int b;
};

static void test_rtti(void)
{
	const void *(CDECL *rttypeid)(void *) = get("__RTtypeid");
	void *(CDECL *rtcast)(void *, int, const void *, const void *, int) =
		get("__RTDynamicCast");
	void *(CDECL *to_void)(void *) = get("__RTCastToVoid");
	const void *tv = get("??_7type_info@@6B@");
	struct type_desc td_x = { tv, NULL, ".?AVX@@" }, td_y = { tv, NULL, ".?AVY@@" },
			 td_z = { tv, NULL, ".?AVZ@@" }, td_m = { tv, NULL, ".?AVM@@" },
			 td_n = { tv, NULL, ".?AVN@@" }, td_k = { tv, NULL, ".?AVK@@" },
			 td_s = { tv, NULL, ".?AVS@@" };

	/* M : X, Y, Y at 8, and a private S at 4: down and across. */
	const struct rtti_base m_m = { &td_m, 3, 0, -1, 0, 0 },
			       m_x = { &td_x, 0, 0, -1, 0, 0 },
			       m_y = { &td_y, 0, 8, -1, 0, 0 },
			       m_s = { &td_s, 0, 4, -1, 0, NOTVISIBLE };
	const struct rtti_base *const m_bases[] = { &m_m, &m_x, &m_y, &m_s };
	const struct rtti_hierarchy m_h = { 0, MULTINH, 4, m_bases };
	const struct rtti_locator m_l0 = { 0, 0, 0, &td_m, &m_h },
				  m_l8 = { 0, 8, 0, &td_m, &m_h };
	const void *m_vt0[2] = { &m_l0, NULL }, *m_vt8[2] = { &m_l8, NULL };
	struct two_vt m = { &m_vt0[1], 1, &m_vt8[1], 2 };
	void *m8 = (char *)&m + 8;

	CHECK(rttypeid(m8) == &td_m);
	CHECK(to_void(m8) == &m && !to_void(NULL));
	CHECK(rtcast(&m, 0, &td_x, &td_y, 0) == m8);	/* across */
	CHECK(rtcast(m8, 0, &td_y, &td_x, 0) == &m);
	CHECK(rtcast(m8, 0, &td_y, &td_m, 0) == &m);	/* down */
	CHECK(!rtcast(&m, 0, &td_x, &td_z, 0));
	CHECK(!rtcast(&m, 0, &td_x, &td_s, 0));	/* not public */
	CHECK(!rtcast(NULL, 0, &td_x, &td_y, 1));	/* NULL even for a reference */

	/*
	 * N : X, Z with Z : X, Z at 8: X twice, so only a down-cast finds
	 * the Z, from either X; there is no cast to an X from the Z.
	 */
	const struct rtti_base n_n = { &td_n, 3, 0, -1, 0, 0 },
			       n_x0 = { &td_x, 0, 0, -1, 0, AMBIGUOUS },
			       n_z = { &td_z, 1, 8, -1, 0, 0 },
			       n_x8 = { &td_x, 0, 8, -1, 0, AMBIGUOUS };
	const struct rtti_base *const n_bases[] = { &n_n, &n_x0, &n_z, &n_x8 };
	const struct rtti_hierarchy n_h = { 0, MULTINH, 4, n_bases };
	const struct rtti_locator n_l0 = { 0, 0, 0, &td_n, &n_h },
				  n_l8 = { 0, 8, 0, &td_n, &n_h };
	const void *n_vt0[2] = { &n_l0, NULL }, *n_vt8[2] = { &n_l8, NULL };
	struct two_vt n = { &n_vt0[1], 1, &n_vt8[1], 2 };
	void *n8 = (char *)&n + 8;

	CHECK(rtcast(n8, 0, &td_x, &td_z, 0) == n8);
	CHECK(rtcast(&n, 0, &td_x, &td_z, 0) == n8);	/* across: one Z */
	CHECK(!rtcast(n8, 0, &td_z, &td_x, 0));		/* X: which one? */
	CHECK(rtcast(n8, 0, &td_x, &td_n, 0) == &n);

	/*
	 * K with a Z and an X in it at 0, and another of each at 8: the
	 * source's own offset (its vftable pointer's, less vfdelta) picks.
	 */
	const struct rtti_base k_k = { &td_k, 4, 0, -1, 0, 0 },
			       k_z0 = { &td_z, 1, 0, -1, 0, AMBIGUOUS },
			       k_x0 = { &td_x, 0, 0, -1, 0, AMBIGUOUS },
			       k_z8 = { &td_z, 1, 8, -1, 0, AMBIGUOUS },
			       k_x8 = { &td_x, 0, 8, -1, 0, AMBIGUOUS };
	const struct rtti_base *const k_bases[] = { &k_k, &k_z0, &k_x0, &k_z8,
						    &k_x8 };
	const struct rtti_hierarchy k_h = { 0, MULTINH, 5, k_bases };
	const struct rtti_locator k_l0 = { 0, 0, 0, &td_k, &k_h },
				  k_l8 = { 0, 8, 0, &td_k, &k_h };
	const void *k_vt0[2] = { &k_l0, NULL }, *k_vt8[2] = { &k_l8, NULL };
	struct two_vt k = { &k_vt0[1], 1, &k_vt8[1], 2 };
	void *k8 = (char *)&k + 8;

	CHECK(rtcast(k8, 0, &td_x, &td_z, 0) == k8);
	CHECK(rtcast(&k, 0, &td_x, &td_z, 0) == &k);
	CHECK(rtcast(k8, 8, &td_x, &td_z, 0) == &k);	/* the X at 0's */
	CHECK(!rtcast(k8, 4, &td_x, &td_z, 0));		/* no X at 4 */

	/* Single inheritance: S : X, and a private base Y of S. */
	const struct rtti_base s_s = { &td_s, 2, 0, -1, 0, 0 },
			       s_x = { &td_x, 0, 0, -1, 0, 0 },
			       s_y = { &td_y, 0, 4, -1, 0, NOTVISIBLE };
	const struct rtti_base *const s_bases[] = { &s_s, &s_x, &s_y };
	const struct rtti_hierarchy s_h = { 0, 0, 3, s_bases };
	const struct rtti_locator s_l = { 0, 0, 0, &td_s, &s_h };
	const void *s_vt[2] = { &s_l, NULL };
	struct { const void *vt; int a, b; } s = { &s_vt[1], 1, 2 };
	CHECK(rtcast(&s, 0, &td_x, &td_s, 0) == &s);
	CHECK(rtcast(&s, 0, &td_s, &td_x, 0) == &s);
	CHECK(!rtcast(&s, 0, &td_x, &td_y, 0));
	/* A type matched by name, as one from another module is. */
	struct type_desc td_s2 = { tv, NULL, ".?AVS@@" };
	CHECK(rtcast(&s, 0, &td_x, &td_s2, 0) == &s);
}

static int dtor_calls;

static void THISCALL count_dtor(void *p)
{
	dtor_calls++;
}

static void CDECL count_del_dtor(void *p)
{
	dtor_calls += 10;
}

static void WINAPI vec_dtor(void *p, size_t size, int count,
			    void (THISCALL *dtor)(void *))
{
	for (int i = 0; i < count; i++)
		dtor((char *)p + i * size);
}

/* ThrowInfo for an int and for an object with a destructor, in C. */
struct cxx_type {
	unsigned flags;
	const void *type;
	int this_offset, vbase_descr, vbase_offset;
	unsigned size;
	void *copy_ctor;
};

struct type_table1 {
	unsigned count;
	const struct cxx_type *types[2];
};

struct throw_info {
	unsigned flags;
	void *dtor;
	void *custom;
	const struct type_table1 *table;
};

static struct type_desc td_int = { NULL, NULL, ".H" };
static struct type_desc td_obj = { NULL, NULL, ".?AVObj@@" };
static struct type_desc td_objbase = { NULL, NULL, ".?AVObjBase@@" };
static const struct cxx_type ct_int = { 1, &td_int, 0, -1, 0, 4, NULL };
static const struct type_table1 tt_int = { 1, { &ct_int } };
static const struct throw_info ti_int = { 0, NULL, NULL, &tt_int };
static const struct cxx_type ct_obj = { 0, &td_obj, 0, -1, 0, 8, NULL };
static const struct cxx_type ct_objbase = { 0, &td_objbase, 4, -1, 0, 4, NULL };
static const struct type_table1 tt_obj = { 2, { &ct_obj, &ct_objbase } };
static const struct throw_info ti_obj = { 0, (void *)count_dtor, NULL, &tt_obj };
static const struct throw_info ti_const_obj = { 1, (void *)count_dtor, NULL,
						&tt_obj };

static void WINAPI (*throw_exc)(void *, const void *);

static void THISCALL throwing_dtor(void *p)
{
	int v = 1;
	throw_exc(&v, &ti_int);
}

static void CDECL exit_43(void)
{
	_exit(43);
}

#define CXX_EXC 0xE06D7363

/* What C++/CLI's filters and unwinds use. */
static void test_cli(void)
{
	int (CDECL *filter)(EXCEPTION_POINTERS *, const void *, unsigned, void **) =
		get("__CxxExceptionFilter");
	int (CDECL *detect)(EXCEPTION_POINTERS *) = get("__CxxDetectRethrow");
	unsigned (CDECL *query_size)(void) = get("__CxxQueryExceptionSize");
	int (CDECL *reg)(EXCEPTION_POINTERS *, void *) =
		get("__CxxRegisterExceptionObject");
	void (CDECL *unreg)(void *, int) = get("__CxxUnregisterExceptionObject");
	void (WINAPI *call_dtor)(void *, void *) = get("__CxxCallUnwindDtor");
	void (WINAPI *call_del)(void *, void *) = get("__CxxCallUnwindDelDtor");
	void (WINAPI *call_std)(void *, void *) = get("__CxxCallUnwindStdDelDtor");
	void (WINAPI *call_vec)(void *, void *, size_t, int, void *) =
		get("__CxxCallUnwindVecDtor");
	void (CDECL *(CDECL *set_translator)(void (CDECL *)(unsigned, void *)))(
		unsigned, void *) =
		get("?_set_se_translator@@YAP6AXIPAU_EXCEPTION_POINTERS@@@ZP6AXI0@Z@Z");
	const void *tv = get("??_7type_info@@6B@");
	struct type_desc td_other = { tv, NULL, ".?AVOther@@" };
	td_int.vtable = td_obj.vtable = td_objbase.vtable = tv;

	struct { int tag, value; } obj = { 'o', 5 };
	EXCEPTION_RECORD rec = { CXX_EXC, 1, NULL, NULL, 3,
				 { 0x19930520, (ULONG_PTR)&obj, (ULONG_PTR)&ti_obj } };
	EXCEPTION_POINTERS ep = { &rec, NULL };
	void *slot[2] = { NULL, NULL };

	CHECK_EQ(filter(NULL, &td_obj, 0, NULL), 0);
	CHECK_EQ(filter(&ep, NULL, 0, NULL), 1);	/* catch (...) */
	CHECK_EQ(filter(&ep, &td_other, 0, slot), 0);
	CHECK_EQ(filter(&ep, &td_objbase, 8, slot), 1);	/* by reference */
	CHECK(slot[0] == &obj.value);
	int copy = 0;
	CHECK_EQ(filter(&ep, &td_objbase, 0, (void **)&copy), 1);	/* by value */
	CHECK_EQ(copy, 5);
	rec.ExceptionInformation[2] = (ULONG_PTR)&ti_const_obj;
	CHECK_EQ(filter(&ep, &td_obj, 0, NULL), 0);	/* const, so not */
	CHECK_EQ(filter(&ep, &td_obj, 1, NULL), 1);
	rec.ExceptionInformation[2] = (ULONG_PTR)&ti_obj;
	EXCEPTION_RECORD seh = { 0xC0000005, 0, NULL, NULL, 0, { 0 } };
	EXCEPTION_POINTERS seh_ep = { &seh, NULL };
	CHECK_EQ(filter(&seh_ep, &td_obj, 0, NULL), 0);

	/* throw;: nothing being handled yet, then what is registered. */
	EXCEPTION_RECORD again = { CXX_EXC, 1, NULL, NULL, 3, { 0x19930520, 0, 0 } };
	EXCEPTION_POINTERS again_ep = { &again, NULL };
	CHECK_EQ(filter(&again_ep, &td_obj, 0, NULL), 0);
	CHECK(!detect(&again_ep) && !detect(NULL) && !detect(&ep));
	CHECK(query_size() >= 2 * sizeof(void *));
	void *fi[4], *fi2[4];
	CHECK_EQ(reg(&ep, fi), 1);
	CHECK_EQ(filter(&again_ep, &td_objbase, 8, slot), 1);
	CHECK(slot[0] == &obj.value);
	CHECK(detect(&again_ep) && again_ep.ExceptionRecord == &rec);
	CHECK(!detect(&ep));	/* not a rethrow */
	/* Nested, then both done: the object destroyed once, not in use. */
	CHECK_EQ(reg(NULL, fi2), 1);
	unreg(fi2, 0);
	CHECK_EQ(dtor_calls, 0);
	CHECK_EQ(reg(&ep, fi2), 1);	/* the same object, nested */
	unreg(fi2, 0);
	CHECK_EQ(dtor_calls, 0);	/* the outer one holds it */
	unreg(fi, 1);
	CHECK_EQ(dtor_calls, 0);	/* in use: kept */
	CHECK_EQ(reg(&ep, fi), 1);
	unreg(fi, 0);
	CHECK_EQ(dtor_calls, 1);
	again_ep.ExceptionRecord = &again;
	CHECK(!detect(&again_ep));	/* nothing registered any more */

	/* Destructors during an unwind, each under its own frame. */
	dtor_calls = 0;
	call_dtor((void *)count_dtor, &obj);
	call_del((void *)count_del_dtor, &obj);
	call_std((void *)count_del_dtor, &obj);
	int arr[3];
	call_vec((void *)vec_dtor, arr, sizeof(arr[0]), 3, (void *)count_dtor);
	CHECK_EQ(dtor_calls, 24);

	CHECK(set_translator(NULL) == NULL);

	/* A C++ exception out of one ends the process: terminate(). */
	pid_t pid = fork();
	if (!pid) {
		void (CDECL *(CDECL *set_terminate)(void (CDECL *)(void)))(void) =
			get("?set_terminate@@YAP6AXXZP6AXXZ@Z");
		throw_exc = get("_CxxThrowException");
		set_terminate(exit_43);
		call_dtor((void *)throwing_dtor, &obj);
		_exit(0);
	}
	int st = 0;
	waitpid(pid, &st, 0);
	CHECK(WIFEXITED(st) && WEXITSTATUS(st) == 43);
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
	test_subclasses();
	test_rtti();
	test_cli();
	test_misc();
	test_fixture();

	mod_free(crt);
	return test_summary("test_cxxeh");
}
