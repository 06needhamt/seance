/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * iphlpapi.dll and msimg32.dll, as far as codecs call them.
 *
 * iphlpapi reports no network adapters. Codecs (WaveT2, VgmSplt) read the
 * adapter list for a MAC address, which some use to tie a licence to the
 * machine, so none is made up: they get the answer a machine without a
 * network card gives.
 *
 * msimg32's blending calls fail: there is no display to blend onto, and
 * drawing without the transparency or alpha asked for would be wrong
 * output passed off as right.
 */
#include "win32/internal.h"

#define ERROR_NO_DATA 232

WINFN(DWORD) ip_GetAdaptersInfo(void *info, ULONG *size)
{
	if (!size)
		return ERROR_INVALID_PARAMETER;
	return ERROR_NO_DATA;
}

WINFN(ULONG) ip_GetAdaptersAddresses(ULONG family, ULONG flags, void *reserved,
				     void *addrs, ULONG *size)
{
	if (!size)
		return ERROR_INVALID_PARAMETER;
	return ERROR_NO_DATA;
}

WINFN(DWORD) ip_GetNumberOfInterfaces(DWORD *n)
{
	if (!n)
		return ERROR_INVALID_PARAMETER;
	*n = 0;
	return ERROR_SUCCESS;
}

#define E(name) { #name, 0, (void *)ip_##name }

static const struct builtin_export ip_exports[] = {
	E(GetAdaptersAddresses),
	E(GetAdaptersInfo),
	E(GetNumberOfInterfaces),
	BUILTIN_END
};

static const struct builtin_export *const ip_tables[] = { ip_exports, NULL };

const struct builtin_dll builtin_iphlpapi = { "iphlpapi.dll", NULL, ip_tables, 0 };

/* msimg32.dll */

WINFN(BOOL) mi_AlphaBlend(HDC dst, int dx, int dy, int dw, int dh, HDC src,
			  int sx, int sy, int sw, int sh, DWORD blend)
{
	win_set_error(ERROR_CALL_NOT_IMPLEMENTED);
	return FALSE;
}

WINFN(BOOL) mi_TransparentBlt(HDC dst, int dx, int dy, int dw, int dh, HDC src,
			      int sx, int sy, int sw, int sh, UINT key)
{
	win_set_error(ERROR_CALL_NOT_IMPLEMENTED);
	return FALSE;
}

WINFN(BOOL) mi_GradientFill(HDC dc, void *vertices, ULONG nv, void *mesh,
			    ULONG nm, ULONG mode)
{
	win_set_error(ERROR_CALL_NOT_IMPLEMENTED);
	return FALSE;
}

#undef E
#define E(name) { #name, 0, (void *)mi_##name }

static const struct builtin_export mi_exports[] = {
	E(AlphaBlend),
	E(GradientFill),
	E(TransparentBlt),
	BUILTIN_END
};

static const struct builtin_export *const mi_tables[] = { mi_exports, NULL };

const struct builtin_dll builtin_msimg32 = { "msimg32.dll", NULL, mi_tables, 0 };
