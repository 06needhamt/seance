/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * d3d9.dll, dxva2.dll, d3d11.dll and dxgi.dll: Direct3D that says it isn't
 * there. Video decoders ask for Direct3D to find out whether they can use
 * the graphics hardware (DXVA, Quick Sync), and fall back to decoding in
 * software when it won't be created, which is the only way the harness can
 * run them. Each creating call fails as it does on a machine with no usable
 * adapter, and returns no object.
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

/* dxva2.dll: the DXVA 2 device manager and services need a Direct3D device. */

WINFN(HRESULT) dxva2_DXVA2CreateDirect3DDeviceManager9(UINT *token, void **manager)
{
	if (token)
		*token = 0;
	if (manager)
		*manager = NULL;
	return D3DERR_NOTAVAILABLE;
}

WINFN(HRESULT) dxva2_DXVA2CreateVideoService(void *device, const GUID *riid, void **out)
{
	if (out)
		*out = NULL;
	return D3DERR_NOTAVAILABLE;
}

#undef E
#define E(name) { #name, 0, (void *)dxva2_##name }

static const struct builtin_export dxva2_exports[] = {
	E(DXVA2CreateDirect3DDeviceManager9),
	E(DXVA2CreateVideoService),
	BUILTIN_END
};

static const struct builtin_export *const dxva2_tables[] = { dxva2_exports, NULL };

const struct builtin_dll builtin_dxva2 = { "dxva2.dll", NULL, dxva2_tables, 0 };

/* d3d11.dll and dxgi.dll: no hardware to make a device or enumerate. */

#define DXGI_ERROR_UNSUPPORTED ((HRESULT)0x887A0004)

WINFN(HRESULT) d3d11_D3D11CreateDevice(void *adapter, UINT driver, HMODULE software,
				       UINT flags, const UINT *levels, UINT n_levels,
				       UINT sdk, void **device, UINT *level, void **context)
{
	if (device)
		*device = NULL;
	if (level)
		*level = 0;
	if (context)
		*context = NULL;
	return DXGI_ERROR_UNSUPPORTED;
}

WINFN(HRESULT) d3d11_D3D11CreateDeviceAndSwapChain(void *adapter, UINT driver,
						   HMODULE software, UINT flags,
						   const UINT *levels, UINT n_levels, UINT sdk,
						   const void *desc, void **swap, void **device,
						   UINT *level, void **context)
{
	if (swap)
		*swap = NULL;
	return d3d11_D3D11CreateDevice(adapter, driver, software, flags, levels,
				       n_levels, sdk, device, level, context);
}

#undef E
#define E(name) { #name, 0, (void *)d3d11_##name }

static const struct builtin_export d3d11_exports[] = {
	E(D3D11CreateDevice),
	E(D3D11CreateDeviceAndSwapChain),
	BUILTIN_END
};

static const struct builtin_export *const d3d11_tables[] = { d3d11_exports, NULL };

const struct builtin_dll builtin_d3d11 = { "d3d11.dll", NULL, d3d11_tables, 0 };

WINFN(HRESULT) dxgi_CreateDXGIFactory(const GUID *riid, void **factory)
{
	if (factory)
		*factory = NULL;
	return DXGI_ERROR_UNSUPPORTED;
}

WINFN(HRESULT) dxgi_CreateDXGIFactory1(const GUID *riid, void **factory)
{
	return dxgi_CreateDXGIFactory(riid, factory);
}

WINFN(HRESULT) dxgi_CreateDXGIFactory2(UINT flags, const GUID *riid, void **factory)
{
	return dxgi_CreateDXGIFactory(riid, factory);
}

#undef E
#define E(name) { #name, 0, (void *)dxgi_##name }

static const struct builtin_export dxgi_exports[] = {
	E(CreateDXGIFactory),
	E(CreateDXGIFactory1),
	E(CreateDXGIFactory2),
	BUILTIN_END
};

static const struct builtin_export *const dxgi_tables[] = { dxgi_exports, NULL };

const struct builtin_dll builtin_dxgi = { "dxgi.dll", NULL, dxgi_tables, 0 };
