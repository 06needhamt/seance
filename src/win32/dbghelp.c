/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * dbghelp.dll, for codecs that link it to report their own crashes
 * (vp7vfw.dll). There are no symbols here: names are given back as they
 * are, decorated, which is what dbghelp does with a name it can't
 * undecorate, and the symbol and stack functions fail as they do for a
 * module without debug information.
 */
#include <string.h>

#include "win32/internal.h"

WINFN(DWORD) dbg_UnDecorateSymbolName(PCSTR name, PSTR out, DWORD size,
				      DWORD flags)
{
	if (!name || !out || !size) {
		win_set_error(ERROR_INVALID_PARAMETER);
		return 0;
	}
	size_t n = strlen(name);
	if (n >= size)
		n = size - 1;
	memcpy(out, name, n);
	out[n] = 0;
	return (DWORD)n;
}

WINFN(DWORD) dbg_UnDecorateSymbolNameW(PCWSTR name, PWSTR out, DWORD size,
				       DWORD flags)
{
	if (!name || !out || !size) {
		win_set_error(ERROR_INVALID_PARAMETER);
		return 0;
	}
	size_t n = wstrlen(name);
	if (n >= size)
		n = size - 1;
	memcpy(out, name, n * sizeof(WCHAR));
	out[n] = 0;
	return (DWORD)n;
}

static DWORD sym_options;

WINFN(BOOL) dbg_SymInitialize(HANDLE process, PCSTR path, BOOL invade)
{
	win_set_error(ERROR_NOT_SUPPORTED);
	return FALSE;
}

WINFN(BOOL) dbg_SymCleanup(HANDLE process)
{
	return TRUE;
}

WINFN(DWORD) dbg_SymGetOptions(void)
{
	return sym_options;
}

WINFN(DWORD) dbg_SymSetOptions(DWORD options)
{
	sym_options = options;
	return options;
}

WINFN(DWORD) dbg_SymLoadModule(HANDLE process, HANDLE file, PCSTR image,
			       PCSTR module, DWORD base, DWORD size)
{
	win_set_error(ERROR_NOT_SUPPORTED);
	return 0;
}

WINFN(BOOL) dbg_SymGetModuleInfo(HANDLE process, DWORD addr, void *info)
{
	win_set_error(ERROR_NOT_SUPPORTED);
	return FALSE;
}

WINFN(BOOL) dbg_SymGetSymFromAddr(HANDLE process, DWORD addr, DWORD *disp,
				  void *symbol)
{
	win_set_error(ERROR_NOT_SUPPORTED);
	return FALSE;
}

WINFN(BOOL) dbg_SymGetLineFromAddr(HANDLE process, DWORD addr, DWORD *disp,
				   void *line)
{
	win_set_error(ERROR_NOT_SUPPORTED);
	return FALSE;
}

WINFN(BOOL) dbg_SymGetLineFromAddr64(HANDLE process, ULONGLONG addr,
				     DWORD *disp, void *line)
{
	win_set_error(ERROR_NOT_SUPPORTED);
	return FALSE;
}

WINFN(BOOL) dbg_SymUnDName(void *symbol, PSTR out, DWORD size)
{
	win_set_error(ERROR_NOT_SUPPORTED);
	return FALSE;
}

WINFN(void *) dbg_SymFunctionTableAccess(HANDLE process, DWORD base)
{
	return NULL;
}

/* No frame to give: the walk ends before it starts. */
WINFN(BOOL) dbg_StackWalk(DWORD machine, HANDLE process, HANDLE thread,
			  void *frame, void *context, void *read_memory,
			  void *table_access, void *module_base,
			  void *translate)
{
	return FALSE;
}

#define E(name) { #name, 0, (void *)dbg_##name }

static const struct builtin_export exports[] = {
	E(StackWalk),
	E(SymCleanup),
	E(SymFunctionTableAccess),
	E(SymGetLineFromAddr),
	E(SymGetLineFromAddr64),
	E(SymGetModuleInfo),
	E(SymGetOptions),
	E(SymGetSymFromAddr),
	E(SymInitialize),
	E(SymLoadModule),
	E(SymSetOptions),
	E(SymUnDName),
	E(UnDecorateSymbolName),
	E(UnDecorateSymbolNameW),
	BUILTIN_END
};

static const struct builtin_export *const tables[] = { exports, NULL };

const struct builtin_dll builtin_dbghelp = { "dbghelp.dll", NULL, tables, 0 };
