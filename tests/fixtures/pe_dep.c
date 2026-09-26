/*
 * Imports pe_basic.dll by name and by ordinal, and forwards one export to it.
 */
#include "fixture.h"

int basic_add(int a, int b);
int basic_mul(int a, int b);
int basic_secret(void);
__declspec(dllimport) extern int basic_data;

int dep_compute(void)
{
	/* 2 + 3 = 5, 5 * 7 = 35, plus the data export */
	return basic_mul(basic_add(2, 3), 7) + basic_data;
}

int dep_secret(void)
{
	return basic_secret();
}

BOOL WINAPI DllMain(HINSTANCE inst, DWORD reason, LPVOID reserved)
{
	return TRUE;
}
