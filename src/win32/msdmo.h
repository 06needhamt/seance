/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * The built-in msdmo.dll's internals, for DMO discovery: DMO registrations
 * as data, read from the registry the way DMORegister writes them, and a
 * capture mode that records what a DLL's DllRegisterServer registers.
 */
#ifndef VFW_WIN32_MSDMO_H
#define VFW_WIN32_MSDMO_H

#include <stddef.h>

#include "win32.h"

/* One DMO in one category. */
struct dmo_reg {
	CLSID clsid;
	GUID category;		/* GUID_NULL if it is in none */
	WCHAR *name;		/* NULL if it has none */
	DWORD flags;		/* DMO_REGISTERF_IS_KEYED */
	size_t nin, nout;
	DMO_PARTIAL_MEDIATYPE *in, *out;
};

/* Frees what d holds (not d itself). */
void dmo_reg_clear(struct dmo_reg *d);
/* Frees an array of n registrations and the array. */
void dmo_regs_free(struct dmo_reg *d, size_t n);

/*
 * The DMOs registered (HKCR\DirectShow\MediaObjects), one entry for each
 * category a DMO is in, or one with GUID_NULL if it is in none, the user's
 * classes before the machine's; malloc()ed, to free with dmo_regs_free.
 * Takes the registry lock. Returns the count.
 */
size_t msdmo_registered(struct dmo_reg **out);

/*
 * Capture mode: from msdmo_capture_begin() every DMO registered through
 * DMORegister, by any thread, is also recorded, until msdmo_capture_end()
 * hands the records over (malloc()ed, n of them; free with dmo_regs_free).
 * Registrations still go to the registry as usual; reg_sandbox_begin()
 * keeps them out of the real one.
 */
void msdmo_capture_begin(void);
size_t msdmo_capture_end(struct dmo_reg **out);

#endif /* VFW_WIN32_MSDMO_H */
