/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * msvcrt: <math.h>. Standard-name functions forward straight to libm; the
 * MSVC-only names (leading underscore, or a different spelling of the same
 * thing) wrap them or the CRT-specific bit they need beyond libm.
 */
#include <fenv.h>
#include <math.h>

#include "runtime/builtin.h"

#define CRTFN(ret) static ret STUB_CDECL

/* Standard C89 names: msvcrt exports them directly, same signature. */
#define FWD1(name) CRTFN(double) crt_##name(double x) { return name(x); }
#define FWD2(name) CRTFN(double) crt_##name(double x, double y) { return name(x, y); }

FWD1(sin) FWD1(cos) FWD1(tan)
FWD1(asin) FWD1(acos) FWD1(atan)
FWD1(sinh) FWD1(cosh) FWD1(tanh)
FWD1(exp) FWD1(log) FWD1(log10)
FWD1(sqrt) FWD1(fabs) FWD1(floor) FWD1(ceil)
FWD2(atan2) FWD2(pow) FWD2(fmod)

CRTFN(double) crt_frexp(double x, int *exp) { return frexp(x, exp); }
CRTFN(double) crt_ldexp(double x, int exp) { return ldexp(x, exp); }
CRTFN(double) crt_modf(double x, double *ip) { return modf(x, ip); }

/* MSVC's own names for functions ISO C only standardised later, or never
 * under this name; same semantics as their libm counterparts. */
CRTFN(double) crt__hypot(double x, double y) { return hypot(x, y); }
CRTFN(double) crt__copysign(double x, double y) { return copysign(x, y); }
CRTFN(double) crt__logb(double x) { return logb(x); }
CRTFN(double) crt__scalb(double x, long n) { return scalbn(x, (int)n); }
CRTFN(double) crt__nextafter(double x, double y) { return nextafter(x, y); }
CRTFN(int) crt__finite(double x) { return isfinite(x) != 0; }
CRTFN(int) crt__isnan(double x) { return isnan(x) != 0; }
/* _FPCLASS_* (float.h). We can't tell a signaling NaN from a quiet one
 * through libm, so both classify as QNAN. */
#define _FPCLASS_QNAN 0x0002
#define _FPCLASS_NINF 0x0004
#define _FPCLASS_ND   0x0008
#define _FPCLASS_NN   0x0010
#define _FPCLASS_NZ   0x0020
#define _FPCLASS_PZ   0x0040
#define _FPCLASS_PD   0x0080
#define _FPCLASS_PN   0x0100
#define _FPCLASS_PINF 0x0200

CRTFN(int) crt__fpclass(double x)
{
	switch (fpclassify(x)) {
	case FP_NAN:       return _FPCLASS_QNAN;
	case FP_INFINITE:  return signbit(x) ? _FPCLASS_NINF : _FPCLASS_PINF;
	case FP_ZERO:      return signbit(x) ? _FPCLASS_NZ : _FPCLASS_PZ;
	case FP_SUBNORMAL: return signbit(x) ? _FPCLASS_ND : _FPCLASS_PD;
	default:           return signbit(x) ? _FPCLASS_NN : _FPCLASS_PN;
	}
}

/* float (single-precision) counterparts MSVC also exports. */
CRTFN(float) crt_sinf(float x) { return sinf(x); }
CRTFN(float) crt_cosf(float x) { return cosf(x); }
CRTFN(float) crt_sqrtf(float x) { return sqrtf(x); }
CRTFN(float) crt_fabsf(float x) { return fabsf(x); }
CRTFN(float) crt_powf(float x, float y) { return powf(x, y); }
CRTFN(float) crt_floorf(float x) { return floorf(x); }
CRTFN(float) crt_ceilf(float x) { return ceilf(x); }

/* A user-supplied handler for domain/range errors; msvcrt calls it if set,
 * else applies the default (already-computed IEEE result). We never call
 * it ourselves - forwarding straight to libm never gives us the chance to -
 * so this is only here for a codec that sets one, expecting to be able to. */
static int (STUB_CDECL *matherr_handler)(void *exc);

CRTFN(int) crt__matherr(void *exc)
{
	return matherr_handler ? matherr_handler(exc) : 0;
}

/*
 * Rounding/exception control. MSVC's x87 control word (_control87) and the
 * newer, portable _controlfp share the same bit layout as the FPU's own
 * control word for the rounding and mask bits, so translate through
 * <fenv.h> for the rounding mode and leave exception masking alone (we
 * don't emulate unmasked FP exceptions).
 */
#define _RC_NEAR 0x0000
#define _RC_DOWN 0x0100
#define _RC_UP   0x0200
#define _RC_CHOP 0x0300
#define _RC_CHOP_UP_DOWN_NEAR_MASK 0x0300
#define _MCW_RC  0x0300

static int fe_round_to_mcw(int fe)
{
	switch (fe) {
	case FE_DOWNWARD:   return _RC_DOWN;
	case FE_UPWARD:     return _RC_UP;
	case FE_TOWARDZERO: return _RC_CHOP;
	default:            return _RC_NEAR;
	}
}

static int mcw_to_fe_round(unsigned int mcw)
{
	switch (mcw & _MCW_RC) {
	case _RC_DOWN: return FE_DOWNWARD;
	case _RC_UP:   return FE_UPWARD;
	case _RC_CHOP: return FE_TOWARDZERO;
	default:       return FE_TONEAREST;
	}
}

/*
 * The bits other than rounding (exception masks, precision, infinity
 * control) aren't applied, but are remembered per thread so the word reads
 * back as set. They start as MSVC's default: all exceptions masked, 53-bit
 * precision (0x9001f).
 */
static _Thread_local unsigned int mcw_other = 0x0009001f;

/* Applies new_val under mask and returns the control word as it now is. */
CRTFN(unsigned int) crt__control87(unsigned int new_val, unsigned int mask)
{
	if (mask & _MCW_RC)
		fesetround(mcw_to_fe_round(new_val));
	mcw_other = (mcw_other & ~(mask & ~_MCW_RC)) | (new_val & mask & ~_MCW_RC);
	return mcw_other | fe_round_to_mcw(fegetround());
}

CRTFN(int) crt__controlfp_s(unsigned int *cur, unsigned int new_val, unsigned int mask)
{
	unsigned int now = crt__control87(new_val, mask);
	if (cur)
		*cur = now;
	return 0;
}

CRTFN(unsigned int) crt__controlfp(unsigned int new_val, unsigned int mask)
{
	return crt__control87(new_val, mask);
}

/*
 * The _CI* intrinsics cl calls for math functions under /Oi or /Og: the
 * arguments on the x87 stack (one in ST0, or for two, the first in ST1
 * and the second in ST0), popped, and the result left in ST0. The caller
 * may keep other values live on the x87 stack across the call, but libm
 * expects it empty (and may use all eight registers), so each trampoline
 * saves the rest of the stack with fnsave, calls libm with the caller's
 * control word, and restores it under the result. The arguments are
 * rounded to double on the way, as /Op code stores them anyway.
 */
#define CI1(name) \
	double vfw_ci_##name(double x) \
		__attribute__((used, visibility("hidden"))); \
	double vfw_ci_##name(double x) { return name(x); }
#define CI2(name) \
	double vfw_ci_##name(double x, double y) \
		__attribute__((used, visibility("hidden"))); \
	double vfw_ci_##name(double x, double y) { return name(x, y); }

CI1(acos) CI1(asin) CI1(atan) CI1(cos) CI1(cosh) CI1(exp) CI1(log)
CI1(log10) CI1(sin) CI1(sinh) CI1(sqrt) CI1(tan) CI1(tanh)
CI2(atan2) CI2(fmod) CI2(pow)

__asm__(
	/* 0(%esp): arguments, 16: result, 32: the saved x87 state. */
	".macro CI_ENTER\n\t"
	"pushl %ebp\n\t"
	"movl %esp, %ebp\n\t"
	"subl $160, %esp\n\t"
	"andl $-16, %esp\n"
	".endm\n"
	".macro CI_CALL fn\n\t"
	"fnsave 32(%esp)\n\t"
	"fldcw 32(%esp)\n\t"
	"call \\fn\n\t"
	"fstpl 16(%esp)\n\t"
	"frstor 32(%esp)\n\t"
	"fldl 16(%esp)\n\t"
	"movl %ebp, %esp\n\t"
	"popl %ebp\n\t"
	"ret\n"
	".endm\n"
	".macro CI1 name\n"
	".globl _CI\\name\n"
	".type _CI\\name, @function\n"
	"_CI\\name:\n\t"
	"CI_ENTER\n\t"
	"fstpl (%esp)\n\t"
	"CI_CALL vfw_ci_\\name\n"
	".size _CI\\name, .-_CI\\name\n"
	".endm\n"
	".macro CI2 name\n"
	".globl _CI\\name\n"
	".type _CI\\name, @function\n"
	"_CI\\name:\n\t"
	"CI_ENTER\n\t"
	"fstpl 8(%esp)\n\t"
	"fstpl (%esp)\n\t"
	"CI_CALL vfw_ci_\\name\n"
	".size _CI\\name, .-_CI\\name\n"
	".endm\n"
	".text\n"
	"CI1 acos\n" "CI1 asin\n" "CI1 atan\n" "CI1 cos\n" "CI1 cosh\n"
	"CI1 exp\n" "CI1 log\n" "CI1 log10\n" "CI1 sin\n" "CI1 sinh\n"
	"CI1 sqrt\n" "CI1 tan\n" "CI1 tanh\n"
	"CI2 atan2\n" "CI2 fmod\n" "CI2 pow\n"
	".purgem CI_ENTER\n" ".purgem CI_CALL\n"
	".purgem CI1\n" ".purgem CI2\n"
);

extern void _CIacos(void), _CIasin(void), _CIatan(void), _CIcos(void),
	_CIcosh(void), _CIexp(void), _CIlog(void), _CIlog10(void),
	_CIsin(void), _CIsinh(void), _CIsqrt(void), _CItan(void),
	_CItanh(void), _CIatan2(void), _CIfmod(void), _CIpow(void);

#define EASM(name) { #name, 0, (void *)name }

#define E(name) { #name, 0, (void *)crt_##name }

const struct builtin_export crt_math_exports[] = {
	E(sin), E(cos), E(tan), E(asin), E(acos), E(atan), E(atan2),
	E(sinh), E(cosh), E(tanh), E(exp), E(log), E(log10), E(pow),
	E(sqrt), E(fabs), E(floor), E(ceil), E(fmod), E(frexp), E(ldexp),
	E(modf),
	E(_hypot), E(_copysign), E(_logb), E(_scalb), E(_nextafter),
	E(_finite), E(_isnan), E(_fpclass), E(_matherr),
	E(_control87), E(_controlfp), E(_controlfp_s),
	E(sinf), E(cosf), E(sqrtf), E(fabsf), E(powf), E(floorf), E(ceilf),
	EASM(_CIacos), EASM(_CIasin), EASM(_CIatan), EASM(_CIatan2),
	EASM(_CIcos), EASM(_CIcosh), EASM(_CIexp), EASM(_CIfmod),
	EASM(_CIlog), EASM(_CIlog10), EASM(_CIpow), EASM(_CIsin),
	EASM(_CIsinh), EASM(_CIsqrt), EASM(_CItan), EASM(_CItanh),
	BUILTIN_END
};
