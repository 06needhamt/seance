/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * A DMO server, the way a codec's DLL is one: a TST0 decoder registered
 * through msdmo's DMORegister, and a TST0 encoder whose
 * HKCR\DirectShow\MediaObjects keys DllRegisterServer writes itself (as
 * some DMOs do), each with its HKCR\CLSID\{...}\InprocServer32 key;
 * DllUnregisterServer takes it all away again.
 *
 * The DMO objects themselves come with the DMO host (Phase 2 of
 * docs/DMO_AUDIO_PLAN.md); until then no class can be created.
 */
#include "fixture.h"
#include <dmoreg.h>

/* {5E6C9B41-3A7D-4F0E-9C11-0D2B7A6E4F80}: the decoder */
static const GUID clsid_decoder = { 0x5e6c9b41, 0x3a7d, 0x4f0e,
	{ 0x9c, 0x11, 0x0d, 0x2b, 0x7a, 0x6e, 0x4f, 0x80 } };
/* {5E6C9B42-3A7D-4F0E-9C11-0D2B7A6E4F80}: the encoder */
static const GUID clsid_encoder = { 0x5e6c9b42, 0x3a7d, 0x4f0e,
	{ 0x9c, 0x11, 0x0d, 0x2b, 0x7a, 0x6e, 0x4f, 0x80 } };

/* Windows' GUIDs, defined here: the fixture links no uuid library. */
static const GUID video_decoder = { 0x4a69b442, 0x28be, 0x4991,
	{ 0x96, 0x9c, 0xb5, 0x00, 0xad, 0xf5, 0xd8, 0xa8 } };
static const WCHAR video_encoder[] =
	L"{33D9A760-90C8-11D0-BD43-00A0C911CE86}";

/* Media types: 'vids', TST0, YUY2 (FOURCC-shaped) and RGB24. */
#define FOURCC_GUID(a, b, c, d) { \
	(DWORD)(a) | (DWORD)(b) << 8 | (DWORD)(c) << 16 | (DWORD)(d) << 24, \
	0x0000, 0x0010, { 0x80, 0x00, 0x00, 0xaa, 0x00, 0x38, 0x9b, 0x71 } }
#define RGB24_GUID { 0xe436eb7d, 0x524f, 0x11ce, \
	{ 0x9f, 0x53, 0x00, 0x20, 0xaf, 0x0b, 0xa7, 0x70 } }
#define VIDS FOURCC_GUID('v', 'i', 'd', 's')

static const DMO_PARTIAL_MEDIATYPE tst0[] = {
	{ VIDS, FOURCC_GUID('T', 'S', 'T', '0') },
};
static const DMO_PARTIAL_MEDIATYPE raw[] = {
	{ VIDS, FOURCC_GUID('Y', 'U', 'Y', '2') }, { VIDS, RGB24_GUID },
};

static const WCHAR decoder_name[] = L"seance test DMO decoder";
static const WCHAR encoder_name[] = L"seance test DMO encoder";

static HINSTANCE self;

static int wlen(const WCHAR *s)
{
	int n = 0;
	while (s[n])
		n++;
	return n;
}

static void wcat(WCHAR *d, const WCHAR *s)
{
	d += wlen(d);
	while ((*d++ = *s++))
		;
}

static LONG set_string(HKEY k, const WCHAR *name, const WCHAR *s)
{
	return RegSetValueExW(k, name, 0, REG_SZ, (const BYTE *)s,
			      (DWORD)(wlen(s) + 1) * sizeof(WCHAR));
}

/* "prefix{clsid}suffix", in out (at least 120 WCHARs). */
static void guid_key(WCHAR *out, const WCHAR *prefix, const GUID *clsid,
		     const WCHAR *suffix)
{
	WCHAR g[40];

	StringFromGUID2(clsid, g, 40);
	out[0] = 0;
	wcat(out, prefix);
	wcat(out, g);
	wcat(out, suffix);
}

/* HKCR\CLSID\{clsid}, named, with this DLL as its InprocServer32. */
static HRESULT register_class(const GUID *clsid, const WCHAR *name)
{
	WCHAR key[120], path[MAX_PATH];
	HKEY k;

	if (!GetModuleFileNameW(self, path, MAX_PATH))
		return E_FAIL;
	guid_key(key, L"CLSID\\", clsid, L"");
	if (RegCreateKeyExW(HKEY_CLASSES_ROOT, key, 0, NULL, 0, KEY_ALL_ACCESS,
			    NULL, &k, NULL))
		return E_FAIL;
	LONG err = set_string(k, NULL, name);
	RegCloseKey(k);
	guid_key(key, L"CLSID\\", clsid, L"\\InprocServer32");
	if (err || RegCreateKeyExW(HKEY_CLASSES_ROOT, key, 0, NULL, 0,
				   KEY_ALL_ACCESS, NULL, &k, NULL))
		return E_FAIL;
	err = set_string(k, NULL, path);
	if (!err)
		err = set_string(k, L"ThreadingModel", L"Both");
	RegCloseKey(k);
	return err ? E_FAIL : S_OK;
}

static void unregister_class(const GUID *clsid)
{
	WCHAR key[120];

	guid_key(key, L"CLSID\\", clsid, L"\\InprocServer32");
	RegDeleteKeyW(HKEY_CLASSES_ROOT, key);
	guid_key(key, L"CLSID\\", clsid, L"");
	RegDeleteKeyW(HKEY_CLASSES_ROOT, key);
}

/* The encoder's DMO keys, written as DMORegister would write them. */
static HRESULT register_encoder(void)
{
	WCHAR key[120];
	HKEY k;

	guid_key(key, L"DirectShow\\MediaObjects\\", &clsid_encoder, L"");
	if (RegCreateKeyExW(HKEY_CLASSES_ROOT, key, 0, NULL, 0, KEY_ALL_ACCESS,
			    NULL, &k, NULL))
		return E_FAIL;
	LONG err = set_string(k, NULL, encoder_name);
	if (!err)
		err = RegSetValueExW(k, L"InputTypes", 0, REG_BINARY,
				     (const BYTE *)raw, sizeof(raw));
	if (!err)
		err = RegSetValueExW(k, L"OutputTypes", 0, REG_BINARY,
				     (const BYTE *)tst0, sizeof(tst0));
	RegCloseKey(k);
	key[0] = 0;
	wcat(key, L"DirectShow\\MediaObjects\\Categories\\");
	wcat(key, video_encoder);
	guid_key(key + wlen(key), L"\\", &clsid_encoder, L"");
	if (err || RegCreateKeyExW(HKEY_CLASSES_ROOT, key, 0, NULL, 0,
				   KEY_ALL_ACCESS, NULL, &k, NULL))
		return E_FAIL;
	RegCloseKey(k);
	return S_OK;
}

static void unregister_encoder(void)
{
	WCHAR key[120];

	key[0] = 0;
	wcat(key, L"DirectShow\\MediaObjects\\Categories\\");
	wcat(key, video_encoder);
	guid_key(key + wlen(key), L"\\", &clsid_encoder, L"");
	RegDeleteKeyW(HKEY_CLASSES_ROOT, key);
	guid_key(key, L"DirectShow\\MediaObjects\\", &clsid_encoder, L"");
	RegDeleteKeyW(HKEY_CLASSES_ROOT, key);
}

HRESULT WINAPI DllRegisterServer(void)
{
	HRESULT hr;

	if (SUCCEEDED(hr = register_class(&clsid_decoder, decoder_name)) &&
	    SUCCEEDED(hr = register_class(&clsid_encoder, encoder_name)) &&
	    SUCCEEDED(hr = DMORegister(decoder_name, &clsid_decoder,
				       &video_decoder, 0, 1, tst0, 2, raw)))
		hr = register_encoder();
	return hr;
}

HRESULT WINAPI DllUnregisterServer(void)
{
	DMOUnregister(&clsid_decoder, &video_decoder);
	unregister_encoder();
	unregister_class(&clsid_decoder);
	unregister_class(&clsid_encoder);
	return S_OK;
}

HRESULT WINAPI DllGetClassObject(REFCLSID clsid, REFIID riid, void **ppv)
{
	*ppv = NULL;
	return CLASS_E_CLASSNOTAVAILABLE;
}

HRESULT WINAPI DllCanUnloadNow(void)
{
	return S_OK;
}

BOOL WINAPI DllMain(HINSTANCE inst, DWORD reason, LPVOID reserved)
{
	if (reason == DLL_PROCESS_ATTACH)
		self = inst;
	return TRUE;
}
