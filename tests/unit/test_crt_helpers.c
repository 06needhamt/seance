/*
 * The msvcrt 64-bit arithmetic/truncation helpers MSVC-compiled code calls
 * for __int64 math and float-to-int conversion. Called directly here
 * (through the builtin export table, not a loaded DLL) since mingw's own
 * codegen never emits calls to them - there is no way to make a
 * mingw-compiled fixture exercise them by just writing normal C.
 */
#include "runtime/builtin.h"
#include "test.h"

typedef long long (STUB_WINAPI *mulrem_fn)(int, int, int, int);

static long long call_mulrem(const char *name, long long a, long long b)
{
	mulrem_fn fn = (mulrem_fn)builtin_export(builtin_find("msvcrt.dll"), name);
	if (!fn) {
		fprintf(stderr, "msvcrt.dll has no %s\n", name);
		exit(1);
	}
	return fn((int)a, (int)(a >> 32), (int)b, (int)(b >> 32));
}

/* EDX:EAX in and out, no stack arguments - _allshl/_allshr/_aullshr. */
static long long call_shift(const char *name, long long v, int shift)
{
	void *fn = builtin_export(builtin_find("msvcrt.dll"), name);
	unsigned int lo = (unsigned int)v, hi = (unsigned int)((unsigned long long)v >> 32);
	if (!fn) {
		fprintf(stderr, "msvcrt.dll has no %s\n", name);
		exit(1);
	}
	__asm__ __volatile__("call *%[fn]\n\t"
			     : "+a"(lo), "+d"(hi)
			     : [fn] "r"(fn), "c"(shift)
			     : "cc");
	return (long long)(((unsigned long long)hi << 32) | lo);
}

/* The value to convert on the FPU stack, EDX:EAX out - _ftol/_ftol2. */
static long long call_ftol(const char *name, double v)
{
	void *fn = builtin_export(builtin_find("msvcrt.dll"), name);
	unsigned int lo, hi;
	if (!fn) {
		fprintf(stderr, "msvcrt.dll has no %s\n", name);
		exit(1);
	}
	__asm__ __volatile__("fldl %[v]\n\t"
			     "call *%[fn]\n\t"
			     : "=a"(lo), "=d"(hi)
			     : [v] "m"(v), [fn] "r"(fn)
			     : "cc");
	return (long long)(((unsigned long long)hi << 32) | lo);
}

int main(void)
{
	CHECK(builtin_find("msvcrt.dll") != NULL);

	long long a = 123456789012LL, b = -987654321LL;
	/* Truncation on overflow, not undefined behaviour: unsigned. */
	CHECK_EQ(call_mulrem("_allmul", a, b),
		 (long long)((unsigned long long)a * (unsigned long long)b));
	CHECK_EQ(call_mulrem("_alldiv", a, b), a / b);
	CHECK_EQ(call_mulrem("_allrem", a, b), a % b);

	unsigned long long ua = 18446744073709551000ULL, ub = 4000000000ULL;
	CHECK_EQ((long long)(unsigned long long)call_mulrem("_aulldiv",
				(long long)ua, (long long)ub), (long long)(ua / ub));
	CHECK_EQ((long long)(unsigned long long)call_mulrem("_aullrem",
				(long long)ua, (long long)ub), (long long)(ua % ub));

	long long v = 0x0102030405060708LL;
	/* Left shift overflowing into the sign bit is undefined for a
	 * signed left-hand side; unsigned shifts don't have that limit. */
	CHECK_EQ(call_shift("_allshl", v, 4),
		 (long long)((unsigned long long)v << 4));
	CHECK_EQ(call_shift("_allshl", v, 40),
		 (long long)((unsigned long long)v << 40));
	CHECK_EQ(call_shift("_allshr", v, 4), v >> 4);
	CHECK_EQ(call_shift("_allshr", -v, 40), -v >> 40);
	CHECK_EQ((long long)(unsigned long long)call_shift("_aullshr", v, 4),
		 (long long)((unsigned long long)v >> 4));
	CHECK_EQ((long long)(unsigned long long)call_shift("_aullshr", v, 40),
		 (long long)((unsigned long long)v >> 40));

	CHECK_EQ(call_ftol("_ftol", 3.75), 3);
	CHECK_EQ(call_ftol("_ftol", -3.75), -3);
	CHECK_EQ(call_ftol("_ftol2", 42.9), 42);

	return test_summary("test_crt_helpers");
}
