/*
 * Exports by name, by ordinal only, and a data export; counts DllMain calls.
 */
#include "fixture.h"

int basic_data = 0x1234;
static int attach_count;
static int detach_count;
static int *detach_flag;

int basic_add(int a, int b)
{
	return a + b;
}

int basic_mul(int a, int b)
{
	return a * b;
}

/* Exported by ordinal only (NONAME in pe_basic.def). */
int basic_secret(void)
{
	return 0x5ec7e7;
}

int basic_attach_count(void)
{
	return attach_count;
}

int basic_detach_count(void)
{
	return detach_count;
}

/* The test can't read our memory after unload, so we write to its memory. */
void basic_on_detach(int *flag)
{
	detach_flag = flag;
}

BOOL WINAPI DllMain(HINSTANCE inst, DWORD reason, LPVOID reserved)
{
	if (reason == DLL_PROCESS_ATTACH)
		attach_count++;
	else if (reason == DLL_PROCESS_DETACH) {
		detach_count++;
		if (detach_flag)
			*detach_flag = 1;
	}
	return TRUE;
}
