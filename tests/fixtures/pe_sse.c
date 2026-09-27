/*
 * Code built the way GCC-built and SIMD codecs are: SSE2, and a 16-byte
 * stack boundary assumed on entry, so aligned stack spills (movaps) fault
 * if the harness calls in with a misaligned stack. Also reports the x87
 * control word and MXCSR a thread starts with, which Windows code expects
 * at their Windows defaults (0x27f: 53-bit precision; 0x1f80: all SSE
 * exceptions masked, round to nearest).
 */
#include "fixture.h"

typedef float v4sf __attribute__((vector_size(16)));

/* Keeps the compiler from folding the vector work away. */
static v4sf opaque(v4sf v)
{
	__asm__ volatile("" : "+x"(v));
	return v;
}

/*
 * Sums a[i] * b[i] with 4-wide SSE, through a stack buffer the compiler
 * takes to be 16-byte aligned without realigning the stack.
 */
EXPORT float sse_dot(const float *a, const float *b, int n)
{
	v4sf acc = { 0, 0, 0, 0 };
	float lanes[4] __attribute__((aligned(16)));
	int i;

	for (i = 0; i + 4 <= n; i += 4) {
		v4sf x, y;
		__builtin_memcpy(&x, a + i, 16);
		__builtin_memcpy(&y, b + i, 16);
		acc = opaque(acc + x * y);
	}
	/* movaps, as compiled code spills with: faults if misaligned. */
	__asm__ volatile("movaps %1, %0" : "=m"(*(v4sf *)lanes) : "x"(acc));
	float sum = lanes[0] + lanes[1] + lanes[2] + lanes[3];
	for (; i < n; i++)
		sum += a[i] * b[i];
	return sum;
}

/* Whether the stack is 16-byte aligned in this function, as assumed. */
EXPORT int sse_stack_aligned(void)
{
	char probe __attribute__((aligned(16)));
	return ((DWORD)&probe & 15) == 0;
}

EXPORT void sse_fpu_state(DWORD *cw, DWORD *mxcsr)
{
	WORD c;
	DWORD m;

	__asm__ volatile("fnstcw %0" : "=m"(c));
	__asm__ volatile("stmxcsr %0" : "=m"(m));
	*cw = c;
	*mxcsr = m;
}

struct thread_result {
	DWORD cw, mxcsr;
	int aligned;
	float dot;
};

static DWORD WINAPI thread_main(LPVOID p)
{
	struct thread_result *r = p;
	static const float a[8] = { 1, 2, 3, 4, 5, 6, 7, 8 };

	sse_fpu_state(&r->cw, &r->mxcsr);
	r->aligned = sse_stack_aligned();
	r->dot = sse_dot(a, a, 8);
	return 0;
}

/*
 * The same checks on a thread made with CreateThread. Returns 0, or -1 if
 * the thread could not be run.
 */
EXPORT int sse_in_thread(DWORD *cw, DWORD *mxcsr, int *aligned, float *dot)
{
	struct thread_result r = { 0, 0, 0, 0 };
	HANDLE t = CreateThread(NULL, 0, thread_main, &r, 0, NULL);

	if (!t)
		return -1;
	if (WaitForSingleObject(t, 10000) != WAIT_OBJECT_0) {
		CloseHandle(t);
		return -1;
	}
	CloseHandle(t);
	*cw = r.cw;
	*mxcsr = r.mxcsr;
	*aligned = r.aligned;
	*dot = r.dot;
	return 0;
}

BOOL WINAPI DllMain(HINSTANCE inst, DWORD reason, LPVOID reserved)
{
	return TRUE;
}
