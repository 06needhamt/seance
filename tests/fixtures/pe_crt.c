/*
 * Exercises msvcrt's math and string functions, imported the way a codec
 * built with a dynamic CRT (/MD) would.
 */
#include "fixture.h"
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

BOOL WINAPI DllMain(HINSTANCE inst, DWORD reason, LPVOID reserved)
{
	return TRUE;
}
