/*
 * msvcrt.dll and the whole family of names real Windows CRTs ship under
 * (msvcr70-msvcr140, msvcp*, vcruntime140, ucrtbase, the api-ms-win-crt-*
 * forwarders):
 * one built-in DLL answering to all of them, since a codec cares about the
 * functions, not which particular CRT DLL it asked for by name.
 *
 * A codec with a statically linked CRT (/MT) never imports any of this at
 * all - it carries its own copy - so this exists only for one built with a
 * dynamic CRT (/MD). fallback = 1: a real msvcrt.dll a codec ships
 * alongside itself, found on the search path, still wins over this.
 */
#include "runtime/builtin.h"

extern const struct builtin_export crt_math_exports[];
extern const struct builtin_export crt_string_exports[];
extern const struct builtin_export crt_stdio_exports[];
extern const struct builtin_export crt_startup_exports[];
extern const struct builtin_export crt_helpers_exports[];
extern const struct builtin_export crt_seh_exports[];
extern const struct builtin_export crt_printf_exports[];
extern const struct builtin_export crt_cxx_exports[];

static const struct builtin_export *const tables[] = {
	crt_math_exports,
	crt_string_exports,
	crt_stdio_exports,
	crt_startup_exports,
	crt_helpers_exports,
	crt_seh_exports,
	crt_printf_exports,
	crt_cxx_exports,
	NULL
};

static const char *const msvcrt_aliases[] = {
	"msvcrt[0-9]*.dll", "msvcr[0-9]*.dll", "msvcp[0-9]*.dll",
	"vcruntime[0-9]*.dll", "ucrtbase.dll", "api-ms-win-crt-*.dll", NULL
};

const struct builtin_dll builtin_msvcrt = { "msvcrt.dll", msvcrt_aliases, tables, 1 };
