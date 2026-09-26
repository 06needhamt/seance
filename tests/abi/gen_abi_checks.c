/*
 * Prints one _Static_assert per entry in abi_list.h, using the sizes,
 * offsets and values from our own include/win32 headers. check_abi.c then
 * compiles those assertions against the mingw-w64 Windows headers, so any
 * layout difference fails the build.
 */
#include <stdio.h>

#include "win32.h"
#include "abi_list.h"

#define S(t) \
	printf("_Static_assert(sizeof(%s) == %zu, \"sizeof(%s)\");\n", \
	       #t, sizeof(t), #t);
#define O(t, m) \
	printf("_Static_assert(offsetof(%s, %s) == %zu, \"offsetof(%s, %s)\");\n", \
	       #t, #m, offsetof(t, m), #t, #m);
#define C(c) \
	printf("_Static_assert((DWORD)(%s) == 0x%08xu, \"%s\");\n", \
	       #c, (unsigned)(DWORD)(c), #c);

int main(void)
{
	VFW_ABI_LIST(S, O, C)
	return 0;
}
