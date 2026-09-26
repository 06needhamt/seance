/*
 * A DLL whose DllMain refuses DLL_PROCESS_ATTACH.
 */
#include "fixture.h"

EXPORT int fail_never_called(void)
{
	return 0;
}

BOOL WINAPI DllMain(HINSTANCE inst, DWORD reason, LPVOID reserved)
{
	return reason != DLL_PROCESS_ATTACH;
}
