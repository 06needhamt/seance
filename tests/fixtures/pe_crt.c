/*
 * Exercises msvcrt's math and string functions, imported the way a codec
 * built with a dynamic CRT (/MD) would.
 */
/* Plain msvcrt.dll printf entry points, not mingw's own __mingw_* wrappers
 * (its default; those come from libmingwex.a, which -nostdlib doesn't link). */
#define __USE_MINGW_ANSI_STDIO 0

#include "fixture.h"
#include <ctype.h>
#include <setjmp.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>

/*
 * msvcrt's import library only gives these as data (the __imp__* pointer
 * dlltool always makes), not a companion call-through thunk - unlike
 * kernel32's, which mingw's own <windows.h> always declares dllimport, so
 * the compiler already knows to call through the pointer. mingw's <math.h>
 * assumes a statically linked math library and declares them plainly, so
 * declaring them here instead, explicitly dllimport, is what makes the
 * compiler emit the indirect call the import library actually supports.
 */
#define CRTIMPORT __declspec(dllimport)
CRTIMPORT double sin(double);
CRTIMPORT double cos(double);
CRTIMPORT double sqrt(double);
CRTIMPORT double pow(double, double);
CRTIMPORT double floor(double);
CRTIMPORT double ceil(double);
CRTIMPORT double atan2(double, double);
CRTIMPORT double log(double);
CRTIMPORT double exp(double);
CRTIMPORT double fmod(double, double);
CRTIMPORT double fabs(double);
CRTIMPORT int atexit(void (*)(void));
CRTIMPORT void _lock(int);
CRTIMPORT void _unlock(int);

static int qsort_check(void);

EXPORT double crt_math_check(void)
{
	return sin(0.0) + cos(0.0) + sqrt(4.0) + pow(2.0, 10.0) +
	       fabs(-3.5) + floor(2.7) + ceil(2.1) + atan2(1.0, 1.0) +
	       log(1.0) + exp(0.0) + fmod(10.0, 3.0);
}

/* Returns 0, or the number of the first check that failed. */
EXPORT int crt_string_check(void)
{
	char buf[32], nbuf[16];

	strcpy(buf, "Hello");
	strcat(buf, ", World");
	if (strcmp(buf, "Hello, World") != 0)
		return 1;
	if (strlen(buf) != 12)
		return 2;
	if (_stricmp("ABC", "abc") != 0 || _strnicmp("ABCD", "abcx", 3) != 0)
		return 3;
	char *p = strchr(buf, ',');
	if (!p || *p != ',')
		return 4;
	if (atoi("42") != 42 || atol("-7") != -7)
		return 5;
	_itoa(255, nbuf, 16);
	if (strcmp(nbuf, "ff") != 0)
		return 6;
	if (memcmp(buf, "Hello", 5) != 0)
		return 7;
	char *dup = _strdup(buf);
	if (!dup || strcmp(dup, buf) != 0)
		return 8;
	free(dup);
	if (qsort_check() != 0)
		return 9;
	return 0;
}

/* qsort/bsearch take a plain cdecl comparator - the default calling
 * convention for a mingw-compiled function, unlike the Win32 APIs. */
static int int_cmp(const void *a, const void *b)
{
	return *(const int *)a - *(const int *)b;
}

static int qsort_check(void)
{
	int a[5] = { 5, 3, 1, 4, 2 };
	qsort(a, 5, sizeof(a[0]), int_cmp);
	for (int i = 0; i < 5; i++)
		if (a[i] != i + 1)
			return 1;
	int key = 4, *found = bsearch(&key, a, 5, sizeof(a[0]), int_cmp);
	return found && *found == 4 ? 0 : 1;
}

/*
 * fprintf/sprintf through _iob, a round trip through fopen/fwrite/fread/
 * fclose in the harness's sandboxed C:\, and _pctype (isalpha/isdigit are
 * table lookups through it in real msvcrt headers, not function calls).
 * Returns 0, or the number of the first check that failed.
 */
EXPORT int crt_stdio_check(void)
{
	char buf[64];
	int n = sprintf(buf, "n=%d s=%s", 42, "ok");
	if (n != 9 || strcmp(buf, "n=42 s=ok") != 0)
		return 1;

	if (fprintf(stdout, "%s", "") < 0 || fprintf(stderr, "%s", "") < 0)
		return 2;

	if (!isupper('A') || isupper('a') || !isdigit('5') || isdigit('x') ||
	    !isalpha('Q') || toupper('q') != 'Q' || tolower('Q') != 'q')
		return 3;

	FILE *f = fopen("C:\\WINDOWS\\TEMP\\pe_crt.tmp", "wb");
	if (!f)
		return 4;
	const char data[] = "hello crt";
	if (fwrite(data, 1, sizeof(data), f) != sizeof(data)) {
		fclose(f);
		return 5;
	}
	fclose(f);

	f = fopen("C:\\WINDOWS\\TEMP\\pe_crt.tmp", "rb");
	if (!f)
		return 6;
	char back[sizeof(data)];
	size_t got = fread(back, 1, sizeof(back), f);
	int eof_now = feof(f);
	fclose(f);
	remove("C:\\WINDOWS\\TEMP\\pe_crt.tmp");
	if (got != sizeof(data) || memcmp(back, data, sizeof(data)) != 0)
		return 7;
	(void)eof_now;
	return 0;
}

/* atexit/_onexit registration and _lock/_unlock. Returns 0, or the number
 * of the first check that failed. Whether a registered exit callback
 * actually runs at process exit isn't checked here - that happens after
 * this DLL, and the harness, are both gone. */
static void noop_atexit(void) {}

EXPORT int crt_startup_check(void)
{
	if (atexit(noop_atexit) != 0)
		return 1;
	if (!_onexit((_onexit_t)noop_atexit))
		return 2;

	_lock(0);
	_unlock(0);
	return 0;
}

/* setjmp and longjmp as plain C uses them (mingw's setjmp is _setjmp3). */
static jmp_buf jump;

static void __attribute__((noinline)) jump_back(int value)
{
	longjmp(jump, value);
}

EXPORT int crt_setjmp_check(void)
{
	volatile int stage = 0;
	int r = setjmp(jump);

	if (!r) {
		stage = 1;
		jump_back(7);
		return 1;
	}
	if (r != 7 || stage != 1)
		return 2;
	r = setjmp(jump);
	if (!r) {
		stage = 2;
		jump_back(0);	/* comes back as 1 */
		return 3;
	}
	if (r != 1 || stage != 2)
		return 4;
	return 0;
}

BOOL WINAPI DllMain(HINSTANCE inst, DWORD reason, LPVOID reserved)
{
	return TRUE;
}
