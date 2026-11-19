/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * 3DNow! in software: see amd3dnow.h. The operations follow AMD's 3DNow!
 * Technology Manual (21928) and its Extensions (22466); the decoding is
 * the ordinary ModRM/SIB form, with REX and RIP-relative operands on
 * x86-64.
 *
 * The MMX registers are the x87 ones: MMi is physical register i, which
 * the signal frame stores as ST((i - TOP) & 7), in two places on i386
 * (the FSAVE image, which the kernel folds back in, and the FXSAVE image
 * after it) and in one on x86-64. An MMX instruction leaves TOP at 0 and
 * every register valid, so the registers are written back that way.
 */
#include <errno.h>
#include <float.h>
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/uio.h>
#include <ucontext.h>
#include <unistd.h>

#include "common/log.h"
#include "runtime/amd3dnow.h"

static unsigned long emulated;

unsigned long amd3dnow_count(void)
{
	return __atomic_load_n(&emulated, __ATOMIC_RELAXED);
}

typedef union {
	uint64_t q;
	uint32_t d[2];
	int32_t i[2];
	float f[2];
	uint16_t w[4];
	int16_t s[4];
	uint8_t b[8];
} mmx;

/*
 * Copies n bytes from addr in this process, without faulting: the
 * kernel checks the address. Returns how many it copied.
 */
static size_t peek(void *to, uintptr_t addr, size_t n)
{
	struct iovec l = { to, n }, r = { (void *)addr, n };
	ssize_t got;

	/*
	 * Only through the kernel, which fails rather than faults. Without
	 * process_vm_readv (ENOSYS), nothing is read, not even the
	 * instruction: its page may be execute-only, or gone by now. The
	 * instruction is then declined and reported as the SIGILL it is.
	 */
	do
		got = process_vm_readv(getpid(), &l, 1, &r, 1, 0);
	while (got < 0 && errno == EINTR);
	return got < 0 ? 0 : (size_t)got;
}

/* The general registers in ModRM order (with r8-r15 on x86-64). */
#ifdef __x86_64__
#define REG_PC REG_RIP
static const int gpr[16] = {
	REG_RAX, REG_RCX, REG_RDX, REG_RBX, REG_RSP, REG_RBP, REG_RSI, REG_RDI,
	REG_R8, REG_R9, REG_R10, REG_R11, REG_R12, REG_R13, REG_R14, REG_R15,
};
#else
#define REG_PC REG_EIP
static const int gpr[8] = {
	REG_EAX, REG_ECX, REG_EDX, REG_EBX, REG_UESP, REG_EBP, REG_ESI, REG_EDI,
};
#endif

/* A decoded instruction. */
struct insn {
	int len;		/* its length in bytes */
	int reg;		/* ModRM.reg: the destination MMX register */
	int rm;			/* ModRM.rm when mod is 3: the source one */
	int mem;		/* whether the source is memory, at addr */
	uintptr_t addr;
	enum { OP, FEMMS, PREFETCH } kind;
	uint8_t op;		/* for OP, 0F 0F's suffix */
};

/*
 * Decodes the instruction in code (n bytes of it) at pc, taking register
 * values from g. Returns 0, or -1 for anything this doesn't emulate.
 */
static int decode(const uint8_t *code, size_t n, uintptr_t pc,
		  const greg_t *g, struct insn *in)
{
	size_t at = 0;
	int rex = 0, addr32 = 0;

	/*
	 * Prefixes: segment overrides with a base of 0 (all, on x86-64),
	 * and on x86-64 the address-size override, which makes the address
	 * 32 bits. On i386 that would mean 16-bit addressing, which flat
	 * code doesn't use; it isn't decoded.
	 */
	for (; at < n; at++) {
		uint8_t p = code[at];
		if (p == 0x26 || p == 0x2e || p == 0x36 || p == 0x3e)
			continue;
#ifdef __x86_64__
		if (p == 0x67) {
			addr32 = 1;
			continue;
		}
#endif
		break;
	}
#ifdef __x86_64__
	if (at < n && (code[at] & 0xf0) == 0x40)
		rex = code[at++];
#endif
	if (at + 2 > n || code[at] != 0x0f)
		return -1;
	uint8_t opc = code[at + 1];
	at += 2;
	if (opc == 0x0e) {			/* femms */
		in->kind = FEMMS;
		in->len = (int)at;
		return 0;
	}
	if (opc != 0x0f && opc != 0x0d)
		return -1;

	if (at >= n)
		return -1;
	uint8_t modrm = code[at++];
	int mod = modrm >> 6;
	in->reg = (modrm >> 3) & 7;		/* MMX registers ignore REX.R */
	in->rm = modrm & 7;
	in->mem = mod != 3;
	in->addr = 0;
	if (in->mem) {
		int base = modrm & 7, index = -1, scale = 0;
		int rip = 0;
		if (base == 4) {		/* a SIB byte */
			if (at >= n)
				return -1;
			uint8_t sib = code[at++];
			scale = sib >> 6;
			index = ((sib >> 3) & 7) | ((rex & 2) ? 8 : 0);
			if (index == 4)
				index = -1;	/* no index */
			base = sib & 7;
			if (base == 5 && mod == 0)
				base = -1;	/* disp32, no base */
		} else if (base == 5 && mod == 0) {
			base = -1;
#ifdef __x86_64__
			rip = 1;
#endif
		}
		if (base >= 0)
			base |= (rex & 1) ? 8 : 0;

		intptr_t disp = 0;
		if (mod == 1) {
			if (at >= n)
				return -1;
			disp = (int8_t)code[at++];
		} else if (mod == 2 || base < 0) {
			int32_t d;
			if (at + 4 > n)
				return -1;
			memcpy(&d, code + at, 4);
			at += 4;
			disp = d;
		}
		uintptr_t a = (uintptr_t)disp;
		if (base >= 0)
			a += (uintptr_t)g[gpr[base]];
		if (index >= 0)
			a += (uintptr_t)g[gpr[index]] << scale;
		in->addr = a;
		in->mem = 1 + rip;	/* RIP-relative: fixed up below */
	}
	if (opc == 0x0d) {			/* prefetch, prefetchw */
		if (!in->mem)
			return -1;
		in->kind = PREFETCH;
		in->len = (int)at;
		return 0;
	}
	if (at >= n)
		return -1;
	in->kind = OP;
	in->op = code[at++];
	in->len = (int)at;
	if (in->mem == 2)		/* from the end of the instruction */
		in->addr += pc + at;
	if (in->mem)
		in->mem = 1;
	if (addr32)
		in->addr = (uint32_t)in->addr;
	return 0;
}

/* 3DNow! has no denormals: they read, and are written, as zeros. */
static float ftz(float x)
{
	return fpclassify(x) == FP_SUBNORMAL ? copysignf(0.0f, x) : x;
}

/* int32 to float, toward zero when it doesn't fit, as 3DNow! converts. */
static float from_i32(int32_t v)
{
	volatile float rounded = (float)v;	/* to float, not x87's width */
	float f = rounded;
	if (fabs((double)f) > fabs((double)v))
		f = nextafterf(f, 0.0f);
	return f;
}

/* float to int32, toward zero, saturating (NaN gives the low end). */
static int32_t to_i32(float x)
{
	if (x >= 2147483648.0f)
		return INT32_MAX;
	if (!(x > -2147483648.0f))
		return INT32_MIN;
	return (int32_t)x;
}

static int32_t to_i16(float x)
{
	if (x >= 32767.0f)
		return 32767;
	if (!(x > -32768.0f))
		return -32768;
	return (int32_t)x;
}

/* The reciprocal estimates, at full precision; a zero gives the
 * largest number of its sign, as AMD has it. */
static float recip(float x)
{
	x = ftz(x);
	return x == 0.0f ? copysignf(FLT_MAX, x) : 1.0f / x;
}

static float rsqrt(float x)
{
	x = ftz(x);
	if (x == 0.0f)
		return copysignf(FLT_MAX, x);
	return copysignf(1.0f / sqrtf(fabsf(x)), x);
}

/*
 * dst = op(dst, src) for the 0F 0F suffix op. Returns -1 for a suffix
 * that is no 3DNow! operation (#UD on AMD's parts too).
 */
static int operate(uint8_t op, mmx *dst, const mmx *src)
{
	mmx a = *dst, b = *src, r;

	for (int k = 0; k < 2; k++) {
		a.f[k] = ftz(a.f[k]);
		b.f[k] = ftz(b.f[k]);
	}
	switch (op) {
	case 0x0c:			/* pi2fw */
		for (int k = 0; k < 2; k++)
			r.f[k] = (float)(int16_t)src->d[k];
		break;
	case 0x0d:			/* pi2fd */
		for (int k = 0; k < 2; k++)
			r.f[k] = from_i32(src->i[k]);
		break;
	case 0x1c:			/* pf2iw */
		for (int k = 0; k < 2; k++)
			r.i[k] = to_i16(b.f[k]);
		break;
	case 0x1d:			/* pf2id */
		for (int k = 0; k < 2; k++)
			r.i[k] = to_i32(b.f[k]);
		break;
	case 0x8a:			/* pfnacc */
		r.f[0] = a.f[0] - a.f[1];
		r.f[1] = b.f[0] - b.f[1];
		break;
	case 0x8e:			/* pfpnacc */
		r.f[0] = a.f[0] - a.f[1];
		r.f[1] = b.f[0] + b.f[1];
		break;
	case 0x90:			/* pfcmpge */
		for (int k = 0; k < 2; k++)
			r.d[k] = a.f[k] >= b.f[k] ? 0xffffffffu : 0;
		return *dst = r, 0;
	case 0xa0:			/* pfcmpgt */
		for (int k = 0; k < 2; k++)
			r.d[k] = a.f[k] > b.f[k] ? 0xffffffffu : 0;
		return *dst = r, 0;
	case 0xb0:			/* pfcmpeq */
		for (int k = 0; k < 2; k++)
			r.d[k] = a.f[k] == b.f[k] ? 0xffffffffu : 0;
		return *dst = r, 0;
	case 0x94:			/* pfmin */
		for (int k = 0; k < 2; k++)
			r.f[k] = b.f[k] < a.f[k] ? b.f[k] : a.f[k];
		break;
	case 0xa4:			/* pfmax */
		for (int k = 0; k < 2; k++)
			r.f[k] = b.f[k] > a.f[k] ? b.f[k] : a.f[k];
		break;
	case 0x96:			/* pfrcp */
		r.f[0] = r.f[1] = recip(b.f[0]);
		break;
	case 0x97:			/* pfrsqrt */
		r.f[0] = r.f[1] = rsqrt(b.f[0]);
		break;
	case 0xa6:			/* pfrcpit1 */
	case 0xa7:			/* pfrsqit1 */
	case 0xb6:			/* pfrcpit2 */
		r = b;
		break;
	case 0x9a:			/* pfsub */
		for (int k = 0; k < 2; k++)
			r.f[k] = a.f[k] - b.f[k];
		break;
	case 0xaa:			/* pfsubr */
		for (int k = 0; k < 2; k++)
			r.f[k] = b.f[k] - a.f[k];
		break;
	case 0x9e:			/* pfadd */
		for (int k = 0; k < 2; k++)
			r.f[k] = a.f[k] + b.f[k];
		break;
	case 0xb4:			/* pfmul */
		for (int k = 0; k < 2; k++)
			r.f[k] = a.f[k] * b.f[k];
		break;
	case 0xae:			/* pfacc */
		r.f[0] = a.f[0] + a.f[1];
		r.f[1] = b.f[0] + b.f[1];
		break;
	case 0xb7:			/* pmulhrw */
		for (int k = 0; k < 4; k++)
			r.w[k] = (uint16_t)(((int32_t)dst->s[k] * src->s[k] +
					     0x8000) >> 16);
		return *dst = r, 0;
	case 0xbb:			/* pswapd */
		r.d[0] = src->d[1];
		r.d[1] = src->d[0];
		return *dst = r, 0;
	case 0xbf:			/* pavgusb */
		for (int k = 0; k < 8; k++)
			r.b[k] = (uint8_t)((dst->b[k] + src->b[k] + 1) >> 1);
		return *dst = r, 0;
	default:
		return -1;
	}
	/*
	 * The float results (not the conversions to integers). 3DNow! has
	 * no infinities: from finite operands, an overflow gives the largest
	 * number of its sign. A zero PFMIN or PFMAX gives is +0, whichever
	 * zero it came from.
	 */
	if (op != 0x1c && op != 0x1d) {
		int finite = isfinite(a.f[0]) && isfinite(a.f[1]) &&
			     isfinite(b.f[0]) && isfinite(b.f[1]);
		for (int k = 0; k < 2; k++) {
			r.f[k] = ftz(r.f[k]);
			if (finite && isinf(r.f[k]))
				r.f[k] = copysignf(FLT_MAX, r.f[k]);
			if ((op == 0x94 || op == 0xa4) && r.f[k] == 0.0f)
				r.f[k] = 0.0f;
		}
	}
	*dst = r;
	return 0;
}

/*
 * The x87 register file in a signal frame: the FXSAVE image (fx), and on
 * i386 the FSAVE one in front of it (fs, which the kernel restores the
 * registers and control words from).
 */
struct x87 {
	uint8_t *fx;		/* 512 bytes, or NULL (i386 without FXSR) */
	uint8_t *fs;		/* i386: 108 bytes */
};

#define FX_FSW   2
#define FX_FTW   4		/* abridged: a bit per register, 1 = valid */
#define FX_ST    32		/* 16 bytes each */
#define FX_SW    464		/* the kernel's software-reserved bytes */
#define FX_XHDR  512		/* the XSAVE header, if there is one */
#define FS_SW    4
#define FS_TAG   8		/* two bits per register, 3 = empty */
#define FS_ST    28		/* 10 bytes each */
#ifndef FP_XSTATE_MAGIC1
#define FP_XSTATE_MAGIC1 0x46505853u
#endif

static int frame_x87(ucontext_t *uc, struct x87 *x)
{
	uint8_t *p = (uint8_t *)uc->uc_mcontext.fpregs;

	if (!p)
		return -1;
#ifdef __x86_64__
	x->fs = NULL;
	x->fx = p;
#else
	/*
	 * struct _fpstate_32: the FSAVE image, then a status word and a
	 * magic number that is 0 when the FXSAVE image follows (at 112).
	 */
	uint16_t magic;
	memcpy(&magic, p + 110, 2);
	x->fs = p;
	x->fx = magic == 0 ? p + 112 : NULL;
#endif
	return 0;
}

static unsigned top_of(const struct x87 *x)
{
	uint16_t sw;
	memcpy(&sw, (x->fx ? x->fx + FX_FSW : x->fs + FS_SW), 2);
	return (sw >> 11) & 7;
}

/* The 80-bit physical registers R0-R7. */
static void read_regs(const struct x87 *x, uint8_t r[8][10])
{
	unsigned top = top_of(x);
	for (unsigned i = 0; i < 8; i++) {
		const uint8_t *st = x->fx ? x->fx + FX_ST + 16 * i :
					    x->fs + FS_ST + 10 * i;
		memcpy(r[(top + i) & 7], st, 10);
	}
}

/* Writes R0-R7 back with TOP 0 and, for an MMX instruction, every
 * register valid. */
static void write_regs(const struct x87 *x, uint8_t r[8][10])
{
	uint16_t sw;

	if (x->fx) {
		for (int i = 0; i < 8; i++)
			memcpy(x->fx + FX_ST + 16 * i, r[i], 10);
		memcpy(&sw, x->fx + FX_FSW, 2);
		sw &= (uint16_t)~(7u << 11);
		memcpy(x->fx + FX_FSW, &sw, 2);
		x->fx[FX_FTW] = 0xff;
		/*
		 * In an XSAVE frame the x87 state is restored from here only
		 * if its bit is set in the header; it isn't if the thread
		 * had left the x87 untouched.
		 */
		uint32_t magic1;
		memcpy(&magic1, x->fx + FX_SW, 4);
		if (magic1 == FP_XSTATE_MAGIC1)
			x->fx[FX_XHDR] |= 1;
	}
	if (x->fs) {
		for (int i = 0; i < 8; i++)
			memcpy(x->fs + FS_ST + 10 * i, r[i], 10);
		memcpy(&sw, x->fs + FS_SW, 2);
		sw &= (uint16_t)~(7u << 11);
		memcpy(x->fs + FS_SW, &sw, 2);
		memset(x->fs + FS_TAG, 0, 2);
	}
}

/* FEMMS: every register empty, as EMMS leaves them. */
static void empty_regs(const struct x87 *x)
{
	if (x->fx) {
		x->fx[FX_FTW] = 0;
		uint32_t magic1;
		memcpy(&magic1, x->fx + FX_SW, 4);
		if (magic1 == FP_XSTATE_MAGIC1)
			x->fx[FX_XHDR] |= 1;
	}
	if (x->fs)
		memset(x->fs + FS_TAG, 0xff, 2);
}

static const char *op_name(uint8_t op)
{
	switch (op) {
	case 0x0c: return "pi2fw";
	case 0x0d: return "pi2fd";
	case 0x1c: return "pf2iw";
	case 0x1d: return "pf2id";
	case 0x8a: return "pfnacc";
	case 0x8e: return "pfpnacc";
	case 0x90: return "pfcmpge";
	case 0x94: return "pfmin";
	case 0x96: return "pfrcp";
	case 0x97: return "pfrsqrt";
	case 0x9a: return "pfsub";
	case 0x9e: return "pfadd";
	case 0xa0: return "pfcmpgt";
	case 0xa4: return "pfmax";
	case 0xa6: return "pfrcpit1";
	case 0xa7: return "pfrsqit1";
	case 0xaa: return "pfsubr";
	case 0xae: return "pfacc";
	case 0xb0: return "pfcmpeq";
	case 0xb4: return "pfmul";
	case 0xb6: return "pfrcpit2";
	case 0xb7: return "pmulhrw";
	case 0xbb: return "pswapd";
	case 0xbf: return "pavgusb";
	}
	return "?";
}

/* Says once, under VFW_TRACE, that this is happening and where. */
static void trace_first(uintptr_t pc, const struct insn *in)
{
	static int said;

	if (!vfw_trace_enabled || __atomic_exchange_n(&said, 1, __ATOMIC_RELAXED))
		return;
	char line[160];
	const char *what = in->kind == FEMMS ? "femms" :
			   in->kind == PREFETCH ? "prefetch" : op_name(in->op);
	int n = snprintf(line, sizeof(line), "trace[%d] emulating 3DNow! "
			 "instructions (the first, %s, at %#lx): slow, a "
			 "signal each\n", (int)getpid(), what,
			 (unsigned long)pc);
	if (n > 0)
		(void)!write(STDERR_FILENO, line, (size_t)n);
}

int amd3dnow_emulate(void *ucontext)
{
	ucontext_t *uc = ucontext;
	greg_t *g = uc->uc_mcontext.gregs;
	uintptr_t pc = (uintptr_t)g[REG_PC];
	uint8_t code[15];
	struct insn in = { 0 };
	struct x87 x;
	int saved = errno;	/* the code resumed sees its own errno */

	size_t n = peek(code, pc, sizeof(code));
	if (n < 2 || decode(code, n, pc, g, &in) || frame_x87(uc, &x))
		goto decline;

	if (in.kind == FEMMS) {
		empty_regs(&x);
	} else if (in.kind == PREFETCH) {
		/* Nothing to do: it is only a hint. */
	} else {
		uint8_t r[8][10];
		mmx dst, src;
		read_regs(&x, r);
		memcpy(&dst, r[in.reg], 8);
		if (in.mem) {
			if (peek(&src, in.addr, 8) != 8)
				goto decline;
		} else {
			memcpy(&src, r[in.rm], 8);
		}
		if (operate(in.op, &dst, &src))
			goto decline;
		memcpy(r[in.reg], &dst, 8);
		r[in.reg][8] = r[in.reg][9] = 0xff;	/* as MMX writes */
		write_regs(&x, r);
	}
	trace_first(pc, &in);
	g[REG_PC] = (greg_t)(pc + (uintptr_t)in.len);
	__atomic_fetch_add(&emulated, 1, __ATOMIC_RELAXED);
	errno = saved;
	return 1;
decline:
	errno = saved;
	return 0;
}
