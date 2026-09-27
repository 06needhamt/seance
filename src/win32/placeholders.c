/*
 * System DLLs the harness has no implementation of yet. They are fallback
 * built-ins (see struct builtin_dll): a codec importing them loads, `info`
 * lists each import as not implemented, and calling one stops at a trap
 * naming it. A native DLL of the same name on the search path wins.
 */
#include "runtime/builtin.h"

static const struct builtin_export none[] = { BUILTIN_END };
static const struct builtin_export *const tables[] = { none, NULL };

#define PLACEHOLDER(var, name, aliases) \
	const struct builtin_dll var = { name, aliases, tables, 1 }

static const char *const comctl_aliases[] = { "comctl32.ocx", NULL };

PLACEHOLDER(builtin_user32, "user32.dll", NULL);
PLACEHOLDER(builtin_gdi32, "gdi32.dll", NULL);
PLACEHOLDER(builtin_advapi32, "advapi32.dll", NULL);
PLACEHOLDER(builtin_version, "version.dll", NULL);
PLACEHOLDER(builtin_ole32, "ole32.dll", NULL);
PLACEHOLDER(builtin_oleaut32, "oleaut32.dll", NULL);
PLACEHOLDER(builtin_shell32, "shell32.dll", NULL);
PLACEHOLDER(builtin_comctl32, "comctl32.dll", comctl_aliases);
PLACEHOLDER(builtin_comdlg32, "comdlg32.dll", NULL);
