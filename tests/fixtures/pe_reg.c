/*
 * Round-trips codec settings through the registry from the Win32 side:
 * reg_write stores them, and reg_read (run after the harness has reloaded
 * the registry from its file) checks them. Each returns 0, or the number
 * of the first check that failed.
 */
#include "fixture.h"

#define KEY "Software\\VfW Fixture\\pe_reg"

EXPORT int reg_write(void)
{
	HKEY k;
	DWORD disp = 0, quality = 75;
	static const BYTE state[5] = { 1, 2, 3, 4, 5 };

	if (RegCreateKeyExA(HKEY_CURRENT_USER, KEY, 0, NULL, 0, KEY_ALL_ACCESS,
			    NULL, &k, &disp) || disp != REG_CREATED_NEW_KEY)
		return 1;
	if (RegSetValueExA(k, "Quality", 0, REG_DWORD, (const BYTE *)&quality,
			   sizeof(quality)))
		return 2;
	if (RegSetValueExW(k, L"Mode", 0, REG_SZ, (const BYTE *)L"fast",
			   5 * sizeof(WCHAR)))
		return 3;
	if (RegSetValueExA(k, "State", 0, REG_BINARY, state, sizeof(state)))
		return 4;
	if (RegCloseKey(k))
		return 5;
	return 0;
}

EXPORT int reg_read(void)
{
	HKEY k;
	DWORD type, size, quality = 0, n = 0;
	char mode[16];
	BYTE state[8];
	FILETIME ft;
	int i;

	if (RegOpenKeyExA(HKEY_CURRENT_USER, KEY, 0, KEY_READ, &k))
		return 1;
	size = sizeof(quality);
	if (RegQueryValueExA(k, "Quality", NULL, &type, (BYTE *)&quality, &size) ||
	    type != REG_DWORD || quality != 75)
		return 2;
	/* Written wide, read narrow. */
	size = sizeof(mode);
	if (RegQueryValueExA(k, "Mode", NULL, &type, (BYTE *)mode, &size) ||
	    type != REG_SZ || size != 5 || mode[0] != 'f' || mode[4])
		return 3;
	size = sizeof(state);
	if (RegQueryValueExA(k, "State", NULL, &type, state, &size) ||
	    type != REG_BINARY || size != 5)
		return 4;
	for (i = 0; i < 5; i++)
		if (state[i] != i + 1)
			return 4;
	if (RegQueryInfoKeyA(k, NULL, NULL, NULL, NULL, NULL, NULL, &n, NULL,
			     NULL, NULL, &ft) || n != 3)
		return 5;
	if (RegCloseKey(k))
		return 6;
	if (RegDeleteKeyA(HKEY_CURRENT_USER, KEY))
		return 7;
	if (RegOpenKeyExA(HKEY_CURRENT_USER, KEY, 0, KEY_READ, &k) !=
	    ERROR_FILE_NOT_FOUND)
		return 8;
	return 0;
}

BOOL WINAPI DllMain(HINSTANCE inst, DWORD reason, LPVOID reserved)
{
	return TRUE;
}
