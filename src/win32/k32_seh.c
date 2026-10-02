/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * kernel32: RaiseException and the top-level (Unhandled)ExceptionFilter
 * pair. RtlUnwind, mainly an ntdll export, is also given here since some
 * codecs import it from kernel32 (as real Windows forwards it there).
 */
#include "runtime/seh.h"
#include "win32/internal.h"

#define E(name) { #name, 0, (void *)seh_##name }

const struct builtin_export k32_seh_exports[] = {
	E(RaiseException),
	E(RtlUnwind),
	E(SetUnhandledExceptionFilter),
	E(UnhandledExceptionFilter),
	BUILTIN_END
};
