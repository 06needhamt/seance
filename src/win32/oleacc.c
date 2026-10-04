/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * oleacc.dll, headless Microsoft Active Accessibility.  Seance has no
 * visible controls for an accessibility client to inspect, so it cannot
 * create a standard IAccessible proxy or marshal one from WM_GETOBJECT.
 */
#include "win32/internal.h"

WINFN(HRESULT) acc_CreateStdAccessibleObject(HWND window, LONG object_id,
					      const GUID *iid, void **object)
{
	if (object)
		*object = NULL;
	return E_NOTIMPL;
}

WINFN(LRESULT) acc_LresultFromObject(const GUID *iid, WPARAM wparam,
				     void *object)
{
	return (LRESULT)E_NOTIMPL;
}

#define E(name) { #name, 0, (void *)acc_##name }

static const struct builtin_export exports[] = {
	E(CreateStdAccessibleObject),
	E(LresultFromObject),
	BUILTIN_END
};

static const struct builtin_export *const tables[] = { exports, NULL };

const struct builtin_dll builtin_oleacc = { "oleacc.dll", NULL, tables, 0 };
