/* SPDX-License-Identifier: GPL-2.0-only */
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
#include <math.h>

#include "runtime/builtin.h"
#include "win32/crt.h"

extern const struct builtin_export crt_math_exports[];
extern const struct builtin_export crt_string_exports[];
extern const struct builtin_export crt_stdio_exports[];
extern const struct builtin_export crt_startup_exports[];
extern const struct builtin_export crt_helpers_exports[];
extern const struct builtin_export crt_seh_exports[];
extern const struct builtin_export crt_printf_exports[];
extern const struct builtin_export crt_scanf_exports[];
extern const struct builtin_export crt_cxx_exports[];
extern const struct builtin_export crt_lowio_exports[];
extern const struct builtin_export crt_legacy_exports[];

static const struct builtin_export *const tables[] = {
	crt_math_exports,
	crt_string_exports,
	crt_stdio_exports,
	crt_startup_exports,
	crt_helpers_exports,
	crt_seh_exports,
	crt_printf_exports,
	crt_scanf_exports,
	crt_cxx_exports,
	crt_lowio_exports,
	crt_legacy_exports,
	NULL
};

static const char *const msvcrt_aliases[] = {
	"msvcrt[0-9]*.dll", "msvcr[0-9]*.dll", "msvcp[0-9]*.dll",
	"vcruntime[0-9]*.dll", "ucrtbase.dll", "api-ms-win-crt-*.dll", NULL
};

const struct builtin_dll builtin_msvcrt = { "msvcrt.dll", msvcrt_aliases, tables, 1 };

/*
 * crtdll.dll, the C runtime of Windows NT 3.x and 4.0 (msvidc32.dll, the
 * Microsoft Video 1 codec, and mcmjpg32.dll import it): msvcrt's functions,
 * as Wine's crtdll forwards to msvcrt, and its own names for msvcrt's data,
 * ending in _dll. The versions are those GetVersion reports (5.1.2600).
 */
static const double huge_dll = HUGE_VAL;
static int argc_dll;
static char *no_strings[1];
static char **argv_dll = no_strings, **environ_dll = no_strings;
static unsigned osmajor_dll = 5, osminor_dll = 1, osver_dll = 2600;
static unsigned winmajor_dll = 5, winminor_dll = 1, winver_dll = 0x0501;
static unsigned osversion_dll = 0x0105;	/* minor << 8 | major */

static const struct builtin_export crtdll_exports[] = {
	{ "_HUGE_dll", 0, (void *)&huge_dll },
	{ "__argc_dll", 0, &argc_dll },
	{ "__argv_dll", 0, &argv_dll },
	{ "_environ_dll", 0, &environ_dll },
	{ "__mb_cur_max_dll", 0, &crt_mb_cur_max },
	{ "_pctype_dll", 0, (void *)&crt_pctype_ptr },
	{ "_fmode_dll", 0, &crt_fmode },
	{ "_commode_dll", 0, &crt_commode },
	{ "_osmajor_dll", 0, &osmajor_dll },
	{ "_osminor_dll", 0, &osminor_dll },
	{ "_osver_dll", 0, &osver_dll },
	{ "_osversion_dll", 0, &osversion_dll },
	{ "_winmajor_dll", 0, &winmajor_dll },
	{ "_winminor_dll", 0, &winminor_dll },
	{ "_winver_dll", 0, &winver_dll },
	BUILTIN_END
};

static const struct builtin_export *const crtdll_tables[] = {
	crtdll_exports,
	crt_math_exports,
	crt_string_exports,
	crt_stdio_exports,
	crt_startup_exports,
	crt_helpers_exports,
	crt_seh_exports,
	crt_printf_exports,
	crt_scanf_exports,
	crt_cxx_exports,
	crt_lowio_exports,
	crt_legacy_exports,
	NULL
};

/* A real crtdll.dll found on the search path wins, as for msvcrt. */
const struct builtin_dll builtin_crtdll = { "crtdll.dll", NULL, crtdll_tables, 1 };
