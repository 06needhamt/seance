/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * d3d9.dll: Direct3D that says it isn't there. Video decoders ask for
 * Direct3D to find out whether they can use the graphics hardware (DXVA),
 * and fall back to decoding in software when it won't be created, which
 * is the only way the harness can run them.
 */
#include "win32/internal.h"

#define D3DERR_NOTAVAILABLE ((HRESULT)0x8876086A)

/* No Direct3D 9 object: the NULL a machine without a usable adapter gives. */
WINFN(void *) d3d9_Direct3DCreate9(UINT sdk_version)
{
	return NULL;
}

WINFN(HRESULT) d3d9_Direct3DCreate9Ex(UINT sdk_version, void **out)
{
	if (out)
		*out = NULL;
	return D3DERR_NOTAVAILABLE;
}

#define E(name) { #name, 0, (void *)d3d9_##name }

static const struct builtin_export exports[] = {
	E(Direct3DCreate9),
	E(Direct3DCreate9Ex),
	BUILTIN_END
};

static const struct builtin_export *const tables[] = { exports, NULL };

const struct builtin_dll builtin_d3d9 = { "d3d9.dll", NULL, tables, 0 };
