/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * urlmon.dll as a machine that is offline (the LEAD filters, rmoc3260):
 * downloads fail with INET_E_DOWNLOAD_FAILURE and monikers and bind
 * contexts aren't made, so callers give up on the network. No object is
 * ever returned.
 */
#include "win32/internal.h"

#define INET_E_DOWNLOAD_FAILURE ((HRESULT)0x800C0008)

static HRESULT none(void **out, HRESULT hr)
{
	if (out)
		*out = NULL;
	return hr;
}

WINFN(HRESULT) um_CreateURLMoniker(void *ctx, LPCWSTR url, void **out)
{
	return none(out, E_NOTIMPL);
}

WINFN(HRESULT) um_CreateURLMonikerEx(void *ctx, LPCWSTR url, void **out, DWORD flags)
{
	return none(out, E_NOTIMPL);
}

WINFN(HRESULT) um_CreateAsyncBindCtx(DWORD reserved, void *cb, void *fmt, void **out)
{
	return none(out, E_NOTIMPL);
}

WINFN(HRESULT) um_CreateAsyncBindCtxEx(void *bc, DWORD opts, void *cb, void *fmt,
				       void **out, DWORD reserved)
{
	return none(out, E_NOTIMPL);
}

WINFN(HRESULT) um_RegisterBindStatusCallback(void *bc, void *cb, void **prev,
					     DWORD reserved)
{
	return none(prev, E_NOTIMPL);
}

WINFN(HRESULT) um_RevokeBindStatusCallback(void *bc, void *cb) { return E_NOTIMPL; }

/* No moniker is ever made, so none is asynchronous. */
WINFN(HRESULT) um_IsAsyncMoniker(void *mk) { return S_FALSE; }

WINFN(HRESULT) um_CopyStgMedium(const void *src, void *dst) { return E_NOTIMPL; }

WINFN(HRESULT) um_FindMimeFromData(void *bc, LPCWSTR url, void *buf, DWORD size,
				   LPCWSTR proposed, DWORD flags, LPWSTR *out,
				   DWORD reserved)
{
	return none((void **)out, E_FAIL);
}

WINFN(HRESULT) um_HlinkSimpleNavigateToString(LPCWSTR target, LPCWSTR location,
					      LPCWSTR frame, void *unk, void *bc,
					      void *cb, DWORD flags, DWORD reserved)
{
	return E_NOTIMPL;
}

WINFN(HRESULT) um_URLDownloadToFileA(void *caller, LPCSTR url, LPCSTR file,
				     DWORD reserved, void *cb)
{
	return INET_E_DOWNLOAD_FAILURE;
}

WINFN(HRESULT) um_URLDownloadToFileW(void *caller, LPCWSTR url, LPCWSTR file,
				     DWORD reserved, void *cb)
{
	return INET_E_DOWNLOAD_FAILURE;
}

WINFN(HRESULT) um_URLDownloadToCacheFileA(void *caller, LPCSTR url, LPSTR file,
					  DWORD size, DWORD reserved, void *cb)
{
	if (file && size)
		file[0] = 0;
	return INET_E_DOWNLOAD_FAILURE;
}

WINFN(HRESULT) um_URLDownloadToCacheFileW(void *caller, LPCWSTR url, LPWSTR file,
					  DWORD size, DWORD reserved, void *cb)
{
	if (file && size)
		file[0] = 0;
	return INET_E_DOWNLOAD_FAILURE;
}

WINFN(HRESULT) um_URLOpenBlockingStreamA(void *caller, LPCSTR url, void **out,
					 DWORD reserved, void *cb)
{
	return none(out, INET_E_DOWNLOAD_FAILURE);
}

WINFN(HRESULT) um_URLOpenBlockingStreamW(void *caller, LPCWSTR url, void **out,
					 DWORD reserved, void *cb)
{
	return none(out, INET_E_DOWNLOAD_FAILURE);
}

WINFN(HRESULT) um_URLOpenPullStreamW(void *caller, LPCWSTR url, DWORD reserved,
				     void *cb)
{
	return INET_E_DOWNLOAD_FAILURE;
}

WINFN(HRESULT) um_UrlMkSetSessionOption(DWORD opt, void *buf, DWORD len,
					DWORD reserved)
{
	return S_OK;
}

#define E(name) { #name, 0, (void *)um_##name }

static const struct builtin_export exports[] = {
	E(CopyStgMedium),
	E(CreateAsyncBindCtx),
	E(CreateAsyncBindCtxEx),
	E(CreateURLMoniker),
	E(CreateURLMonikerEx),
	E(FindMimeFromData),
	E(HlinkSimpleNavigateToString),
	E(IsAsyncMoniker),
	E(RegisterBindStatusCallback),
	E(RevokeBindStatusCallback),
	E(URLDownloadToCacheFileA),
	E(URLDownloadToCacheFileW),
	E(URLDownloadToFileA),
	E(URLDownloadToFileW),
	E(URLOpenBlockingStreamA),
	E(URLOpenBlockingStreamW),
	E(URLOpenPullStreamW),
	E(UrlMkSetSessionOption),
	BUILTIN_END
};

static const struct builtin_export *const tables[] = { exports, NULL };

const struct builtin_dll builtin_urlmon = { "urlmon.dll", NULL, tables, 0 };
