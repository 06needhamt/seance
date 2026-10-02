/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * Uses shell32 from Win32 code the way a codec might: its settings folder
 * under Application Data, a copy and delete through SHFileOperation, the
 * path back from an item ID list, a command line split, and a web link
 * that nothing can open. shell_run returns 0, or the number of the first
 * check that failed.
 */
#include "fixture.h"
#include <shellapi.h>
#include <shlobj.h>

static int str_eq(const char *a, const char *b)
{
	while (*a && *a == *b)
		a++, b++;
	return *a == *b;
}

static int ends_with(const char *s, const char *tail)
{
	const char *a = s, *b = tail;

	while (*a)
		a++;
	while (*b)
		b++;
	while (b > tail && a > s)
		if (*--a != *--b)
			return 0;
	return b == tail;
}

static int wstr_eq(const WCHAR *a, const char *b)
{
	while (*b && *a == (BYTE)*b)
		a++, b++;
	return !*b && !*a;
}

static void copy_str(char *d, const char *s)
{
	while ((*d++ = *s++))
		;
}

EXPORT int shell_run(void)
{
	char dir[MAX_PATH], file[MAX_PATH + 16], copy[MAX_PATH + 16];
	char from[MAX_PATH + 16], to[MAX_PATH + 16], back[MAX_PATH];
	LPITEMIDLIST pidl = NULL;
	IMalloc *m = NULL;
	DWORD n;
	int argc = 0;

	/* The codec's own folder under Application Data. */
	if (SHGetFolderPathA(NULL, CSIDL_APPDATA | CSIDL_FLAG_CREATE, NULL,
			     SHGFP_TYPE_CURRENT, dir) != S_OK ||
	    !ends_with(dir, "\\Application Data"))
		return 1;
	char *e = dir;
	while (*e)
		e++;
	copy_str(e, "\\PeShell");
	if (SHCreateDirectoryExA(NULL, dir, NULL) != ERROR_SUCCESS)
		return 2;

	/* A settings file, copied and the original deleted. */
	copy_str(file, dir);
	copy_str(file + (e - dir) + 8, "\\settings.ini");
	HANDLE h = CreateFileA(file, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS,
			       FILE_ATTRIBUTE_NORMAL, NULL);
	if (h == INVALID_HANDLE_VALUE)
		return 3;
	WriteFile(h, "[pe_shell]\r\n", 12, &n, NULL);
	CloseHandle(h);

	copy_str(from, file);
	from[(e - dir) + 8 + 13 + 1] = 0;	/* double NUL */
	copy_str(copy, dir);
	copy_str(copy + (e - dir) + 8, "\\backup.ini");
	copy_str(to, copy);
	to[(e - dir) + 8 + 11 + 1] = 0;
	SHFILEOPSTRUCTA op = { NULL, FO_COPY, from, to, FOF_NOCONFIRMATION |
			       FOF_SILENT, TRUE, NULL, NULL };
	if (SHFileOperationA(&op) || op.fAnyOperationsAborted)
		return 4;
	if (GetFileAttributesA(copy) == INVALID_FILE_ATTRIBUTES)
		return 5;
	op.wFunc = FO_DELETE;
	op.pTo = NULL;
	if (SHFileOperationA(&op) ||
	    GetFileAttributesA(file) != INVALID_FILE_ATTRIBUTES)
		return 6;

	/* The folder's item ID list, and its path back. */
	if (SHGetSpecialFolderLocation(NULL, CSIDL_APPDATA, &pidl) != S_OK ||
	    !SHGetPathFromIDListA(pidl, back))
		return 7;
	*e = 0;
	if (!str_eq(back, dir))
		return 8;
	if (SHGetMalloc(&m) != S_OK)
		return 9;
	m->lpVtbl->Free(m, pidl);

	/* A command line, split. */
	LPWSTR *argv = CommandLineToArgvW(L"codec.exe -q \"a b\"", &argc);
	if (!argv || argc != 3 || !wstr_eq(argv[0], "codec.exe") ||
	    !wstr_eq(argv[2], "a b"))
		return 10;
	LocalFree(argv);

	/* The about box link: no program to open it. */
	if ((UINT_PTR)ShellExecuteA(NULL, "open", "http://example.com/", NULL,
				    NULL, SW_SHOWNORMAL) > 32)
		return 11;
	return 0;
}

BOOL WINAPI DllMain(HINSTANCE inst, DWORD reason, LPVOID reserved)
{
	return TRUE;
}
