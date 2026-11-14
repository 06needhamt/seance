/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * ddraw.dll, dsound.dll, dinput.dll and dinput8.dll: DirectX that says the
 * hardware isn't there. Codecs ask for DirectDraw to draw an overlay
 * (clvsd, lmpgvd, sonydv, the miro codecs), DirectSound to play what they
 * decode (dvdaudio, reclock) or DirectInput for hot keys (ffdshow); each
 * creating call fails as on a machine with no display, sound or input
 * driver, and returns no object, so the caller takes its fallback.
 * Enumerations list nothing.
 */
#include "win32/internal.h"

#define DDERR_NODIRECTDRAWHW ((HRESULT)0x88760208)
#define DSERR_NODRIVER       ((HRESULT)0x88780078)
#define DIERR_NOTINITIALIZED ((HRESULT)0x80070015)

static HRESULT none(void **out, HRESULT hr)
{
	if (out)
		*out = NULL;
	return hr;
}

/* ddraw.dll */

WINFN(HRESULT) dd_DirectDrawCreate(GUID *guid, void **out, IUnknown *outer)
{
	return none(out, DDERR_NODIRECTDRAWHW);
}

WINFN(HRESULT) dd_DirectDrawCreateEx(GUID *guid, void **out, REFIID iid,
				     IUnknown *outer)
{
	return none(out, DDERR_NODIRECTDRAWHW);
}

WINFN(HRESULT) dd_DirectDrawCreateClipper(DWORD flags, void **out,
					  IUnknown *outer)
{
	return none(out, DDERR_NODIRECTDRAWHW);
}

WINFN(HRESULT) dd_DirectDrawEnumerateA(void *cb, void *ctx) { return S_OK; }
WINFN(HRESULT) dd_DirectDrawEnumerateW(void *cb, void *ctx) { return S_OK; }
WINFN(HRESULT) dd_DirectDrawEnumerateExA(void *cb, void *ctx, DWORD flags) { return S_OK; }
WINFN(HRESULT) dd_DirectDrawEnumerateExW(void *cb, void *ctx, DWORD flags) { return S_OK; }

#define E(name) { #name, 0, (void *)dd_##name }

static const struct builtin_export dd_exports[] = {
	E(DirectDrawCreate),
	E(DirectDrawCreateClipper),
	E(DirectDrawCreateEx),
	E(DirectDrawEnumerateA),
	E(DirectDrawEnumerateExA),
	E(DirectDrawEnumerateExW),
	E(DirectDrawEnumerateW),
	BUILTIN_END
};

static const struct builtin_export *const dd_tables[] = { dd_exports, NULL };

const struct builtin_dll builtin_ddraw = { "ddraw.dll", NULL, dd_tables, 0 };

/* dsound.dll: imported by ordinal, as its import library does. */

WINFN(HRESULT) ds_DirectSoundCreate(const GUID *guid, void **out, IUnknown *outer)
{
	return none(out, DSERR_NODRIVER);
}

WINFN(HRESULT) ds_DirectSoundCreate8(const GUID *guid, void **out, IUnknown *outer)
{
	return none(out, DSERR_NODRIVER);
}

WINFN(HRESULT) ds_DirectSoundCaptureCreate(const GUID *guid, void **out,
					   IUnknown *outer)
{
	return none(out, DSERR_NODRIVER);
}

WINFN(HRESULT) ds_DirectSoundCaptureCreate8(const GUID *guid, void **out,
					    IUnknown *outer)
{
	return none(out, DSERR_NODRIVER);
}

WINFN(HRESULT) ds_DirectSoundEnumerateA(void *cb, void *ctx) { return S_OK; }
WINFN(HRESULT) ds_DirectSoundEnumerateW(void *cb, void *ctx) { return S_OK; }
WINFN(HRESULT) ds_DirectSoundCaptureEnumerateA(void *cb, void *ctx) { return S_OK; }
WINFN(HRESULT) ds_DirectSoundCaptureEnumerateW(void *cb, void *ctx) { return S_OK; }

#undef E
#define E(name, ord) { #name, ord, (void *)ds_##name }

static const struct builtin_export ds_exports[] = {
	E(DirectSoundCreate, 1),
	E(DirectSoundEnumerateA, 2),
	E(DirectSoundEnumerateW, 3),
	E(DirectSoundCaptureCreate, 6),
	E(DirectSoundCaptureEnumerateA, 7),
	E(DirectSoundCaptureEnumerateW, 8),
	E(DirectSoundCreate8, 11),
	E(DirectSoundCaptureCreate8, 12),
	BUILTIN_END
};

static const struct builtin_export *const ds_tables[] = { ds_exports, NULL };

const struct builtin_dll builtin_dsound = { "dsound.dll", NULL, ds_tables, 0 };

/* dinput.dll and dinput8.dll */

WINFN(HRESULT) di_DirectInputCreateA(HINSTANCE inst, DWORD version, void **out,
				     IUnknown *outer)
{
	return none(out, DIERR_NOTINITIALIZED);
}

WINFN(HRESULT) di_DirectInputCreateW(HINSTANCE inst, DWORD version, void **out,
				     IUnknown *outer)
{
	return none(out, DIERR_NOTINITIALIZED);
}

WINFN(HRESULT) di_DirectInputCreateEx(HINSTANCE inst, DWORD version, REFIID iid,
				      void **out, IUnknown *outer)
{
	return none(out, DIERR_NOTINITIALIZED);
}

WINFN(HRESULT) di_DirectInput8Create(HINSTANCE inst, DWORD version, REFIID iid,
				     void **out, IUnknown *outer)
{
	return none(out, DIERR_NOTINITIALIZED);
}

#undef E
#define E(name) { #name, 0, (void *)di_##name }

static const struct builtin_export di_exports[] = {
	E(DirectInputCreateA),
	E(DirectInputCreateEx),
	E(DirectInputCreateW),
	BUILTIN_END
};

static const struct builtin_export *const di_tables[] = { di_exports, NULL };

const struct builtin_dll builtin_dinput = { "dinput.dll", NULL, di_tables, 0 };

static const struct builtin_export di8_exports[] = {
	E(DirectInput8Create),
	BUILTIN_END
};

static const struct builtin_export *const di8_tables[] = { di8_exports, NULL };

const struct builtin_dll builtin_dinput8 = { "dinput8.dll", NULL, di8_tables, 0 };
