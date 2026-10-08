/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * A DMO server, the way a codec's DLL is one: a TST0 decoder registered
 * through msdmo's DMORegister, a TST0 encoder whose
 * HKCR\DirectShow\MediaObjects keys DllRegisterServer writes itself (as
 * some DMOs do), and a TSTA audio decoder and encoder in the audio
 * categories, each with its HKCR\CLSID\{...}\InprocServer32 key;
 * DllUnregisterServer takes it all away again.
 *
 * The DMOs themselves live in testdmo_dmo.c (video) and testdmo_audio.c.
 */
#include "fixture.h"
#include <dmoreg.h>

/* {5E6C9B41-3A7D-4F0E-9C11-0D2B7A6E4F80}: the decoder */
static const GUID clsid_decoder = { 0x5e6c9b41, 0x3a7d, 0x4f0e,
	{ 0x9c, 0x11, 0x0d, 0x2b, 0x7a, 0x6e, 0x4f, 0x80 } };
/* {5E6C9B42-3A7D-4F0E-9C11-0D2B7A6E4F80}: the encoder */
static const GUID clsid_encoder = { 0x5e6c9b42, 0x3a7d, 0x4f0e,
	{ 0x9c, 0x11, 0x0d, 0x2b, 0x7a, 0x6e, 0x4f, 0x80 } };
/* {5E6C9B43-3A7D-4F0E-9C11-0D2B7A6E4F80}: the audio decoder */
static const GUID clsid_audio_decoder = { 0x5e6c9b43, 0x3a7d, 0x4f0e,
	{ 0x9c, 0x11, 0x0d, 0x2b, 0x7a, 0x6e, 0x4f, 0x80 } };
/* {5E6C9B44-3A7D-4F0E-9C11-0D2B7A6E4F80}: the audio encoder */
static const GUID clsid_audio_encoder = { 0x5e6c9b44, 0x3a7d, 0x4f0e,
	{ 0x9c, 0x11, 0x0d, 0x2b, 0x7a, 0x6e, 0x4f, 0x80 } };

/* Windows' GUIDs, defined here: the fixture links no uuid library. */
static const GUID video_decoder = { 0x4a69b442, 0x28be, 0x4991,
	{ 0x96, 0x9c, 0xb5, 0x00, 0xad, 0xf5, 0xd8, 0xa8 } };
static const WCHAR video_encoder[] =
	L"{33D9A760-90C8-11D0-BD43-00A0C911CE86}";
static const GUID audio_decoder = { 0x57f2db8b, 0xe6bb, 0x4513,
	{ 0x9d, 0x43, 0xdc, 0xd2, 0xa6, 0x59, 0x31, 0x25 } };
static const GUID audio_encoder = { 0x33d9a761, 0x90c8, 0x11d0,
	{ 0xbd, 0x43, 0x00, 0xa0, 0xc9, 0x11, 0xce, 0x86 } };

/* Media types: 'vids', TST0, YUY2 (FOURCC-shaped) and RGB24. */
#define FOURCC_GUID(a, b, c, d) { \
	(DWORD)(a) | (DWORD)(b) << 8 | (DWORD)(c) << 16 | (DWORD)(d) << 24, \
	0x0000, 0x0010, { 0x80, 0x00, 0x00, 0xaa, 0x00, 0x38, 0x9b, 0x71 } }
#define RGB24_GUID { 0xe436eb7d, 0x524f, 0x11ce, \
	{ 0x9f, 0x53, 0x00, 0x20, 0xaf, 0x0b, 0xa7, 0x70 } }
#define RGB32_GUID { 0xe436eb7e, 0x524f, 0x11ce, \
	{ 0x9f, 0x53, 0x00, 0x20, 0xaf, 0x0b, 0xa7, 0x70 } }
#define VIDS FOURCC_GUID('v', 'i', 'd', 's')

static const DMO_PARTIAL_MEDIATYPE tst0[] = {
	{ VIDS, FOURCC_GUID('T', 'S', 'T', '0') },
};
/* The raw formats testdmo_dmo.c handles. */
static const DMO_PARTIAL_MEDIATYPE raw[] = {
	{ VIDS, FOURCC_GUID('I', '4', '2', '0') },
	{ VIDS, FOURCC_GUID('Y', 'V', '1', '2') },
	{ VIDS, FOURCC_GUID('Y', 'U', 'Y', '2') },
	{ VIDS, FOURCC_GUID('U', 'Y', 'V', 'Y') },
	{ VIDS, RGB24_GUID }, { VIDS, RGB32_GUID },
};

/* Audio: 'auds' with PCM, and TSTA (format tag subtypes). */
#define AUDS FOURCC_GUID('a', 'u', 'd', 's')
#define TAG_GUID(t) { (t), 0x0000, 0x0010, \
	{ 0x80, 0x00, 0x00, 0xaa, 0x00, 0x38, 0x9b, 0x71 } }
static const DMO_PARTIAL_MEDIATYPE tsta[] = { { AUDS, TAG_GUID(0x7354) } };
static const DMO_PARTIAL_MEDIATYPE pcm[] = { { AUDS, TAG_GUID(1) } };

static const WCHAR decoder_name[] = L"seance test DMO decoder";
static const WCHAR encoder_name[] = L"seance test DMO encoder";
static const WCHAR audio_decoder_name[] = L"seance test DMO audio decoder";
static const WCHAR audio_encoder_name[] = L"seance test DMO audio encoder";

static HINSTANCE self;
HRESULT testdmo_create(BOOL encoder, REFIID riid, void **out);
LONG testdmo_objects(void);
HRESULT testdmo_audio_create(BOOL encoder, REFIID riid, void **out);
LONG testdmo_audio_objects(void);

static const GUID iid_unknown = { 0, 0, 0, { 0xc0, 0, 0, 0, 0, 0, 0, 0x46 } };
static const GUID iid_factory = { 1, 0, 0, { 0xc0, 0, 0, 0, 0, 0, 0, 0x46 } };

static int guid_eq(const GUID *a, const GUID *b)
{
	const BYTE *x = (const BYTE *)a, *y = (const BYTE *)b;
	for (int i = 0; i < 16; i++)
		if (x[i] != y[i])
			return 0;
	return 1;
}

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
	    SUCCEEDED(hr = register_class(&clsid_audio_decoder,
					  audio_decoder_name)) &&
	    SUCCEEDED(hr = register_class(&clsid_audio_encoder,
					  audio_encoder_name)) &&
	    SUCCEEDED(hr = DMORegister(decoder_name, &clsid_decoder,
				       &video_decoder, 0, 1, tst0,
				       sizeof(raw) / sizeof(*raw), raw)) &&
	    SUCCEEDED(hr = register_encoder()) &&
	    SUCCEEDED(hr = DMORegister(audio_decoder_name,
				       &clsid_audio_decoder, &audio_decoder,
				       0, 1, tsta, 1, pcm)))
		hr = DMORegister(audio_encoder_name, &clsid_audio_encoder,
				 &audio_encoder, 0, 1, pcm, 1, tsta);
	return hr;
}

HRESULT WINAPI DllUnregisterServer(void)
{
	DMOUnregister(&clsid_decoder, &video_decoder);
	DMOUnregister(&clsid_audio_decoder, &audio_decoder);
	DMOUnregister(&clsid_audio_encoder, &audio_encoder);
	unregister_encoder();
	unregister_class(&clsid_decoder);
	unregister_class(&clsid_encoder);
	unregister_class(&clsid_audio_decoder);
	unregister_class(&clsid_audio_encoder);
	return S_OK;
}

/* Class factories: one per DMO. */
struct factory { IClassFactory iface; BOOL encoder, audio; };

static HRESULT WINAPI cf_qi(IClassFactory *cf, REFIID riid, void **ppv)
{
	if (guid_eq(riid, &iid_unknown) || guid_eq(riid, &iid_factory)) {
		*ppv = cf;
		return S_OK;
	}
	*ppv = NULL;
	return E_NOINTERFACE;
}

static ULONG WINAPI cf_addref(IClassFactory *cf) { return 2; }
static ULONG WINAPI cf_release(IClassFactory *cf) { return 1; }

static HRESULT WINAPI cf_create(IClassFactory *cf, IUnknown *outer,
				REFIID riid, void **ppv)
{
	*ppv = NULL;
	return outer ? CLASS_E_NOAGGREGATION :
		((struct factory *)cf)->audio ?
		testdmo_audio_create(((struct factory *)cf)->encoder, riid, ppv) :
		testdmo_create(((struct factory *)cf)->encoder, riid, ppv);
}

static HRESULT WINAPI cf_lock(IClassFactory *cf, BOOL lock) { return S_OK; }

static const IClassFactoryVtbl cf_vtbl = {
	cf_qi, cf_addref, cf_release, cf_create, cf_lock
};
static struct factory decoder_factory = {
	{ (IClassFactoryVtbl *)&cf_vtbl }, FALSE
};
static struct factory encoder_factory = {
	{ (IClassFactoryVtbl *)&cf_vtbl }, TRUE
};
static struct factory audio_decoder_factory = {
	{ (IClassFactoryVtbl *)&cf_vtbl }, FALSE, TRUE
};
static struct factory audio_encoder_factory = {
	{ (IClassFactoryVtbl *)&cf_vtbl }, TRUE, TRUE
};

HRESULT WINAPI DllGetClassObject(REFCLSID clsid, REFIID riid, void **ppv)
{
	if (guid_eq(clsid, &clsid_decoder))
		return cf_qi(&decoder_factory.iface, riid, ppv);
	if (guid_eq(clsid, &clsid_encoder))
		return cf_qi(&encoder_factory.iface, riid, ppv);
	if (guid_eq(clsid, &clsid_audio_decoder))
		return cf_qi(&audio_decoder_factory.iface, riid, ppv);
	if (guid_eq(clsid, &clsid_audio_encoder))
		return cf_qi(&audio_encoder_factory.iface, riid, ppv);
	*ppv = NULL;
	return CLASS_E_CLASSNOTAVAILABLE;
}

HRESULT WINAPI DllCanUnloadNow(void)
{
	return testdmo_objects() || testdmo_audio_objects() ? S_FALSE : S_OK;
}

BOOL WINAPI DllMain(HINSTANCE inst, DWORD reason, LPVOID reserved)
{
	if (reason == DLL_PROCESS_ATTACH)
		self = inst;
	return TRUE;
}
