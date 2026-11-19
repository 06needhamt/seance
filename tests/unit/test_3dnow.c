/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * 3DNow! emulation (runtime/amd3dnow.c): each operation, run as the real
 * instruction on this CPU (which doesn't have it, so it faults into the
 * emulator), against results worked out by hand from AMD's definitions.
 * Then the addressing forms, the x87 stack top, the registers it must
 * leave alone, FEMMS, and an operand it can't read.
 */
#include <cpuid.h>
#include <errno.h>
#include <fcntl.h>
#include <float.h>
#include <signal.h>
#include <stdint.h>
#include <sys/mman.h>
#include <sys/wait.h>

#include "runtime/amd3dnow.h"
#include "runtime/crash.h"
#include "test.h"

/* The asm below names MMX registers, which i386 builds don't assume. */
#pragma GCC target("mmx")

/*
 * MMX code takes over the x87 stack, which GCC doesn't infer from the MMX
 * clobbers: without these it keeps floats there across the asm.
 */
#define X87 "st", "st(1)", "st(2)", "st(3)", "st(4)", "st(5)", "st(6)", "st(7)"

typedef union {
	uint64_t q;
	uint32_t d[2];
	int32_t i[2];
	float f[2];
	uint16_t w[4];
	uint8_t b[8];
} mmx;

static mmx F(float lo, float hi)
{
	mmx m;
	m.f[0] = lo;
	m.f[1] = hi;
	return m;
}

static mmx D(uint32_t lo, uint32_t hi)
{
	mmx m;
	m.d[0] = lo;
	m.d[1] = hi;
	return m;
}

/* mm3 = op(mm3, mm5): modrm 11 011 101. */
#define RR(name, imm) \
	static mmx name(mmx a, mmx b) \
	{ \
		mmx r; \
		__asm__ volatile("movq %1, %%mm3\n\t" \
				 "movq %2, %%mm5\n\t" \
				 ".byte 0x0f, 0x0f, 0xdd, " #imm "\n\t" \
				 "movq %%mm3, %0\n\t" \
				 "emms" \
				 : "=m"(r.q) : "m"(a.q), "m"(b.q) \
				 : "mm3", "mm5", "memory", X87); \
		return r; \
	}

RR(pi2fw, 0x0c)
RR(pi2fd, 0x0d)
RR(pf2iw, 0x1c)
RR(pf2id, 0x1d)
RR(pfnacc, 0x8a)
RR(pfpnacc, 0x8e)
RR(pfcmpge, 0x90)
RR(pfmin, 0x94)
RR(pfrcp, 0x96)
RR(pfrsqrt, 0x97)
RR(pfsub, 0x9a)
RR(pfadd, 0x9e)
RR(pfcmpgt, 0xa0)
RR(pfmax, 0xa4)
RR(pfrcpit1, 0xa6)
RR(pfrsqit1, 0xa7)
RR(pfsubr, 0xaa)
RR(pfacc, 0xae)
RR(pfcmpeq, 0xb0)
RR(pfmul, 0xb4)
RR(pfrcpit2, 0xb6)
RR(pmulhrw, 0xb7)
RR(pswapd, 0xbb)
RR(pavgusb, 0xbf)

/* Compared as floats: i386 works out constants in long double. */
#define CHECK_F(m, lo, hi) \
	do { \
		volatile float lo_ = (lo), hi_ = (hi); \
		CHECK((m).f[0] == lo_ && (m).f[1] == hi_); \
		if ((m).f[0] != lo_ || (m).f[1] != hi_) \
			fprintf(stderr, "  got %g, %g (%#x, %#x)\n", \
				(double)(m).f[0], (double)(m).f[1], \
				(m).d[0], (m).d[1]); \
	} while (0)
#define CHECK_D(m, lo, hi) \
	do { \
		CHECK_EQ((m).d[0], (uint32_t)(lo)); \
		CHECK_EQ((m).d[1], (uint32_t)(hi)); \
	} while (0)

static void test_operations(void)
{
	mmx a = F(1.5f, -2.25f), b = F(0.5f, 4.0f), r;

	r = pfadd(a, b);   CHECK_F(r, 2.0f, 1.75f);
	r = pfsub(a, b);   CHECK_F(r, 1.0f, -6.25f);
	r = pfsubr(a, b);  CHECK_F(r, -1.0f, 6.25f);
	r = pfmul(a, b);   CHECK_F(r, 0.75f, -9.0f);
	r = pfacc(a, b);   CHECK_F(r, -0.75f, 4.5f);
	r = pfnacc(a, b);  CHECK_F(r, 3.75f, -3.5f);
	r = pfpnacc(a, b); CHECK_F(r, 3.75f, 4.5f);
	r = pfmin(a, b);   CHECK_F(r, 0.5f, -2.25f);
	r = pfmax(a, b);   CHECK_F(r, 1.5f, 4.0f);
	/* A zero from either is +0; a negative beats a zero to the minimum. */
	r = pfmin(F(-0.0f, -1.0f), F(0.0f, 0.0f));
	CHECK_D(r, 0, 0xbf800000);
	r = pfmax(F(-0.0f, -1.0f), F(-0.0f, -0.0f));
	CHECK_D(r, 0, 0);

	/* No infinities: an overflow gives the largest number of its sign. */
	r = pfadd(F(FLT_MAX, -FLT_MAX), F(FLT_MAX, -FLT_MAX));
	CHECK_F(r, FLT_MAX, -FLT_MAX);
	r = pfmul(F(1e30f, -1e30f), F(1e30f, 1e30f));
	CHECK_F(r, FLT_MAX, -FLT_MAX);

	r = pfcmpgt(a, b);             CHECK_D(r, 0xffffffff, 0);
	r = pfcmpge(a, F(1.5f, 4.0f)); CHECK_D(r, 0xffffffff, 0);
	r = pfcmpge(a, F(2.0f, -3.0f)); CHECK_D(r, 0, 0xffffffff);
	r = pfcmpeq(a, F(1.5f, 4.0f)); CHECK_D(r, 0xffffffff, 0);

	/* The estimates use the source's low half and fill both. */
	r = pfrcp(a, F(0.5f, 99.0f));    CHECK_F(r, 2.0f, 2.0f);
	r = pfrcp(a, F(-0.0f, 1.0f));    CHECK_F(r, -FLT_MAX, -FLT_MAX);
	r = pfrsqrt(a, F(4.0f, 99.0f));  CHECK_F(r, 0.5f, 0.5f);
	r = pfrsqrt(a, F(-16.0f, 1.0f)); CHECK_F(r, -0.25f, -0.25f);
	/* The refining steps pass the estimate on. */
	r = pfrcpit1(a, b); CHECK_F(r, 0.5f, 4.0f);
	r = pfrsqit1(a, b); CHECK_F(r, 0.5f, 4.0f);
	r = pfrcpit2(a, b); CHECK_F(r, 0.5f, 4.0f);
	/* AMD's reciprocal sequence: x0 = pfrcp(b), pfrcpit1, pfrcpit2. */
	mmx x0 = pfrcp(a, F(3.0f, 0.0f));
	r = pfrcpit2(pfrcpit1(F(3.0f, 3.0f), x0), x0);
	CHECK_F(r, 1.0f / 3.0f, 1.0f / 3.0f);

	r = pi2fd(a, D((uint32_t)-3, 1u << 24)); CHECK_F(r, -3.0f, 16777216.0f);
	/* Too many bits for a float: toward zero, not to nearest. */
	r = pi2fd(a, D(16777219, (uint32_t)-16777219));
	CHECK_F(r, 16777218.0f, -16777218.0f);
	r = pi2fw(a, D(0x0001ffff, 0x00007fff)); CHECK_F(r, -1.0f, 32767.0f);
	r = pf2id(a, F(-3.75f, 3e9f));  CHECK_D(r, (uint32_t)-3, 0x7fffffff);
	r = pf2id(a, F(-3e9f, 7.99f));  CHECK_D(r, 0x80000000u, 7);
	r = pf2iw(a, F(40000.0f, -2.5f)); CHECK_D(r, 32767, (uint32_t)-2);
	r = pf2iw(a, F(-1e6f, 0.0f));   CHECK_D(r, (uint32_t)-32768, 0);

	mmx wa, wb;
	wa.w[0] = 0x4000; wa.w[1] = 0x8000; wa.w[2] = 0x7fff; wa.w[3] = 0x0001;
	wb.w[0] = 0x4000; wb.w[1] = 0x8000; wb.w[2] = 0x0002; wb.w[3] = 0xffff;
	r = pmulhrw(wa, wb);
	CHECK_EQ(r.w[0], 0x1000);
	CHECK_EQ(r.w[1], 0x4000);
	CHECK_EQ(r.w[2], 0x0001);
	CHECK_EQ(r.w[3], 0x0000);

	r = pswapd(a, D(0x11111111, 0x22222222));
	CHECK_D(r, 0x22222222, 0x11111111);

	static const uint8_t ba[8] = { 0, 255, 16, 1, 127, 128, 254, 2 };
	static const uint8_t bb[8] = { 1, 255, 15, 0, 128, 128, 255, 3 };
	static const uint8_t avg[8] = { 1, 255, 16, 1, 128, 128, 255, 3 };
	memcpy(wa.b, ba, 8);
	memcpy(wb.b, bb, 8);
	r = pavgusb(wa, wb);
	CHECK(!memcmp(r.b, avg, 8));

	/* The code resumed sees its own errno, whatever the emulator did. */
	errno = 1234;
	r = pfadd(a, b);
	CHECK_EQ(errno, 1234);

	/* No denormals: in or out, they are zeros. */
	r = pfadd(F(1e-40f, 1.0f), F(0.0f, 0.0f));
	CHECK_F(r, 0.0f, 1.0f);
	CHECK(!(r.d[0] & 0x7fffffff));
	r = pfmul(F(1e-20f, 2.0f), F(1e-20f, 2.0f));
	CHECK_F(r, 0.0f, 4.0f);
}

/* mm3 += [p] with pfadd, through each way of addressing p. */
static void test_addressing(void)
{
	static mmx mem[4];
	mmx a = F(1.0f, 2.0f), r;

	mem[1] = F(10.0f, 20.0f);
	/* [eax]: modrm 00 011 000 */
	__asm__ volatile("movq %1, %%mm3\n\t"
			 ".byte 0x0f, 0x0f, 0x18, 0x9e\n\t"
			 "movq %%mm3, %0\n\temms"
			 : "=m"(r.q) : "m"(a.q), "a"(&mem[1]) : "mm3", "memory", X87);
	CHECK_F(r, 11.0f, 22.0f);

	/* ds: [eax + 16]: modrm 01 011 000, disp8 */
	__asm__ volatile("movq %1, %%mm3\n\t"
			 ".byte 0x3e, 0x0f, 0x0f, 0x58, 0x10, 0x9e\n\t"
			 "movq %%mm3, %0\n\temms"
			 : "=m"(r.q) : "m"(a.q), "a"((uintptr_t)&mem[1] - 16)
			 : "mm3", "memory", X87);
	CHECK_F(r, 11.0f, 22.0f);

	/* [eax + ecx*8 + 0x100]: modrm 10 011 100, SIB 11 001 000, disp32 */
	__asm__ volatile("movq %1, %%mm3\n\t"
			 ".byte 0x0f, 0x0f, 0x9c, 0xc8, 0x00, 0x01, 0x00, 0x00, "
			 "0x9e\n\t"
			 "movq %%mm3, %0\n\temms"
			 : "=m"(r.q)
			 : "m"(a.q), "a"((uintptr_t)&mem[1] - 0x100 - 3 * 8),
			   "c"(3)
			 : "mm3", "memory", X87);
	CHECK_F(r, 11.0f, 22.0f);

#ifdef __x86_64__
	/* REX.B: [r9]: 41, modrm 00 011 001 */
	__asm__ volatile("mov %2, %%r9\n\t"
			 "movq %1, %%mm3\n\t"
			 ".byte 0x41, 0x0f, 0x0f, 0x19, 0x9e\n\t"
			 "movq %%mm3, %0\n\temms"
			 : "=m"(r.q) : "m"(a.q), "r"(&mem[1])
			 : "r9", "mm3", "memory", X87);
	CHECK_F(r, 11.0f, 22.0f);

	/* RIP-relative, from the end of the instruction: modrm 00 011 101 */
	__asm__ volatile("jmp 2f\n\t"
			 ".balign 8\n"
			 "1:\t.float 100.0, 200.0\n"
			 "2:\tmovq %1, %%mm3\n\t"
			 ".byte 0x0f, 0x0f, 0x1d\n\t"
			 ".long 1b - (. + 5)\n\t"
			 ".byte 0x9e\n\t"
			 "movq %%mm3, %0\n\temms"
			 : "=m"(r.q) : "m"(a.q) : "mm3", "memory", X87);
	CHECK_F(r, 101.0f, 202.0f);

	/*
	 * The address-size override: [eax], with rax's high half ignored.
	 * The operand has to lie below 4 GB for that.
	 */
	mmx *low = mmap(NULL, 4096, PROT_READ | PROT_WRITE,
			MAP_PRIVATE | MAP_ANONYMOUS | MAP_32BIT, -1, 0);
	CHECK(low != MAP_FAILED);
	if (low != MAP_FAILED) {
		*low = F(10.0f, 20.0f);
		uint64_t rax = 0xdead000000000000ull | (uintptr_t)low;
		__asm__ volatile("movq %1, %%mm3\n\t"
				 ".byte 0x67, 0x0f, 0x0f, 0x18, 0x9e\n\t"
				 "movq %%mm3, %0\n\temms"
				 : "=m"(r.q) : "m"(a.q), "a"(rax)
				 : "mm3", "memory", X87);
		CHECK_F(r, 11.0f, 22.0f);
		munmap(low, 4096);
	}
#endif
}

/*
 * With the x87 stack top moved off 0 (by an fldz, which also writes
 * physical register 7, that is mm7), mm3 is still mm3; the other MMX
 * registers keep their values.
 */
static void test_stack_top(void)
{
	mmx a = F(1.0f, 2.0f), b = F(3.0f, 4.0f), keep = D(0x12345678, 0x9abcdef0);
	mmx r, k2, k6;

	__asm__ volatile("movq %3, %%mm2\n\t"
			 "movq %3, %%mm6\n\t"
			 "movq %4, %%mm3\n\t"
			 "movq %5, %%mm5\n\t"
			 "fldz\n\t"
			 ".byte 0x0f, 0x0f, 0xdd, 0x9e\n\t"
			 "movq %%mm3, %0\n\t"
			 "movq %%mm2, %1\n\t"
			 "movq %%mm6, %2\n\t"
			 "emms"
			 : "=m"(r.q), "=m"(k2.q), "=m"(k6.q)
			 : "m"(keep.q), "m"(a.q), "m"(b.q)
			 : "mm2", "mm3", "mm5", "mm6", "memory", X87);
	CHECK_F(r, 4.0f, 6.0f);
	CHECK_EQ(k2.q, keep.q);
	CHECK_EQ(k6.q, keep.q);
}

/*
 * FEMMS empties the registers, so the x87 can push again: without it,
 * fld onto a full stack overflows into NaN.
 */
static void test_femms(void)
{
	double two = 0;
	mmx one = D(1, 0);

	__asm__ volatile("movq %1, %%mm0\n\tmovq %1, %%mm1\n\t"
			 "movq %1, %%mm2\n\tmovq %1, %%mm3\n\t"
			 "movq %1, %%mm4\n\tmovq %1, %%mm5\n\t"
			 "movq %1, %%mm6\n\tmovq %1, %%mm7\n\t"
			 ".byte 0x0f, 0x0e\n\t"
			 "fld1\n\tfld1\n\tfaddp\n\t"
			 "fstpl %0"
			 : "=m"(two) : "m"(one.q)
			 : "mm0", "mm1", "mm2", "mm3", "mm4", "mm5", "mm6",
			   "mm7", "memory", X87);
	CHECK(two == 2.0);

	/* prefetchw [eax] (0F 0D /1), which is only a hint. */
	static char line[64];
	__asm__ volatile(".byte 0x0f, 0x0d, 0x08" : : "a"(line) : "memory", X87);
}

/* Runs fn in a child; its wait status. */
static int in_child(void (*fn)(void))
{
	fflush(NULL);
	pid_t pid = fork();
	if (pid < 0)
		return -1;
	if (!pid) {
		int null = open("/dev/null", O_WRONLY);
		if (null >= 0)
			dup2(null, STDERR_FILENO);
		fn();
		_exit(0);
	}
	int status = 0;
	waitpid(pid, &status, 0);
	return status;
}

static void bad_operand(void)
{
	/* pfadd mm3, [eax] with eax 0. */
	__asm__ volatile(".byte 0x0f, 0x0f, 0x18, 0x9e" : : "a"(0) : "mm3", X87);
}

static void bad_suffix(void)
{
	/* 0F 0F with a suffix that is no operation. */
	__asm__ volatile(".byte 0x0f, 0x0f, 0xc0, 0x00" : : : "mm0", X87);
}

static int died_of_sigill(int status)
{
	return (WIFEXITED(status) && WEXITSTATUS(status) == 128 + SIGILL) ||
	       (WIFSIGNALED(status) && WTERMSIG(status) == SIGILL);
}

static int cpu_3dnow(void)
{
	unsigned a, b, c, d;
	return __get_cpuid(0x80000001, &a, &b, &c, &d) && (d >> 31);
}

int main(void)
{
	CHECK_EQ(crash_install(), 0);
	/*
	 * A CPU with 3DNow! runs these itself, and its estimates and
	 * refining steps aren't the emulator's: then only what both agree
	 * on is checked.
	 */
	if (cpu_3dnow()) {
		fprintf(stderr, "test_3dnow: this CPU has 3DNow!, so the "
			"emulator isn't exercised\n");
		test_stack_top();
		test_femms();
		return test_summary("test_3dnow");
	}
	unsigned long before = amd3dnow_count();
	test_operations();
	test_addressing();
	test_stack_top();
	test_femms();
	/* The CPU did none of it itself. */
	CHECK(amd3dnow_count() - before >= 39);

	/* What it can't do is still the illegal instruction it was. */
	CHECK(died_of_sigill(in_child(bad_operand)));
	CHECK(died_of_sigill(in_child(bad_suffix)));
	return test_summary("test_3dnow");
}
