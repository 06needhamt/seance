/*
 * An in-process COM server that also uses version.dll and ole32 from the
 * Win32 side, as a codec with a COM part would. com_run() creates its own
 * class through CoCreateInstance (which finds this DLL by the
 * InprocServer32 key the test registers) and reads its own version
 * resource. It returns 0, or the number of the first check that failed.
 */
#include "fixture.h"

/* {5E6C9B21-3A7D-4F0E-9C11-0D2B7A6E4F80} */
static const GUID clsid_test = { 0x5e6c9b21, 0x3a7d, 0x4f0e,
	{ 0x9c, 0x11, 0x0d, 0x2b, 0x7a, 0x6e, 0x4f, 0x80 } };
/* {5E6C9B22-3A7D-4F0E-9C11-0D2B7A6E4F80}: ITest */
static const GUID iid_test = { 0x5e6c9b22, 0x3a7d, 0x4f0e,
	{ 0x9c, 0x11, 0x0d, 0x2b, 0x7a, 0x6e, 0x4f, 0x80 } };
static const GUID iid_unknown = { 0x00000000, 0x0000, 0x0000,
	{ 0xc0, 0, 0, 0, 0, 0, 0, 0x46 } };
static const GUID iid_factory = { 0x00000001, 0x0000, 0x0000,
	{ 0xc0, 0, 0, 0, 0, 0, 0, 0x46 } };

static HINSTANCE self;
static LONG objects;

static int guid_eq(const GUID *a, const GUID *b)
{
	const BYTE *x = (const BYTE *)a, *y = (const BYTE *)b;
	for (int i = 0; i < 16; i++)
		if (x[i] != y[i])
			return 0;
	return 1;
}

static int str_eq(const char *a, const char *b)
{
	while (*a && *a == *b)
		a++, b++;
	return *a == *b;
}

/* ITest: IUnknown plus Value(). */
typedef struct test_obj test_obj;
struct test_vtbl {
	HRESULT (WINAPI *QueryInterface)(test_obj *, REFIID, void **);
	ULONG (WINAPI *AddRef)(test_obj *);
	ULONG (WINAPI *Release)(test_obj *);
	int (WINAPI *Value)(test_obj *);
};
struct test_obj {
	const struct test_vtbl *vtbl;
	LONG refs;
};

static HRESULT WINAPI obj_qi(test_obj *o, REFIID riid, void **ppv)
{
	if (guid_eq(riid, &iid_unknown) || guid_eq(riid, &iid_test)) {
		o->refs++;
		*ppv = o;
		return S_OK;
	}
	*ppv = NULL;
	return E_NOINTERFACE;
}

static ULONG WINAPI obj_addref(test_obj *o)
{
	return ++o->refs;
}

static ULONG WINAPI obj_release(test_obj *o)
{
	ULONG r = --o->refs;
	if (!r) {
		CoTaskMemFree(o);
		objects--;
	}
	return r;
}

static int WINAPI obj_value(test_obj *o)
{
	return 42;
}

static const struct test_vtbl obj_vtbl = {
	obj_qi, obj_addref, obj_release, obj_value
};

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
	if (outer)
		return CLASS_E_NOAGGREGATION;
	test_obj *o = CoTaskMemAlloc(sizeof(*o));
	if (!o)
		return E_OUTOFMEMORY;
	o->vtbl = &obj_vtbl;
	o->refs = 1;
	objects++;
	HRESULT hr = obj_qi(o, riid, ppv);
	obj_release(o);
	return hr;
}

static HRESULT WINAPI cf_lock(IClassFactory *cf, BOOL lock) { return S_OK; }

static const IClassFactoryVtbl cf_vtbl = {
	cf_qi, cf_addref, cf_release, cf_create, cf_lock
};
static IClassFactory factory = { (IClassFactoryVtbl *)&cf_vtbl };

HRESULT WINAPI DllGetClassObject(REFCLSID clsid, REFIID riid, void **ppv)
{
	if (!guid_eq(clsid, &clsid_test)) {
		*ppv = NULL;
		return CLASS_E_CLASSNOTAVAILABLE;
	}
	return cf_qi(&factory, riid, ppv);
}

/* Objects of the class still alive. */
int com_objects(void)
{
	return objects;
}

static int check_version(void)
{
	char path[MAX_PATH];
	DWORD handle = 1, size;
	void *block, *value;
	UINT len;
	VS_FIXEDFILEINFO *fixed;

	if (!GetModuleFileNameA(self, path, sizeof(path)))
		return 1;
	if (!(size = GetFileVersionInfoSizeA(path, &handle)) || handle)
		return 2;
	if (!(block = CoTaskMemAlloc(size)))
		return 3;
	int bad = 0;
	if (!GetFileVersionInfoA(path, 0, size, block))
		bad = 4;
	else if (!VerQueryValueA(block, "\\", (void **)&fixed, &len) ||
		 len != sizeof(*fixed) || fixed->dwFileVersionMS != 0x00020000 ||
		 fixed->dwFileVersionLS != 0x00000001)
		bad = 5;
	else if (!VerQueryValueA(block, "\\StringFileInfo\\040904B0\\FileDescription",
				 &value, &len) || !str_eq(value, "pe_com fixture"))
		bad = 6;
	else if (VerQueryValueA(block, "\\StringFileInfo\\040904b0\\Missing",
				&value, &len))
		bad = 7;
	CoTaskMemFree(block);
	return bad;
}

int com_run(void)
{
	test_obj *o;
	void *p;
	CLSID parsed;
	WCHAR s[40];

	if (CoCreateInstance(&clsid_test, NULL, CLSCTX_INPROC_SERVER, &iid_test,
			     &p) != CO_E_NOTINITIALIZED)
		return 10;
	if (CoInitialize(NULL) != S_OK)
		return 11;
	if (CoInitialize(NULL) != S_FALSE)
		return 12;
	if (CoInitializeEx(NULL, COINIT_MULTITHREADED) != RPC_E_CHANGED_MODE)
		return 13;

	if (CoCreateInstance(&clsid_test, NULL, CLSCTX_ALL, &iid_test,
			     (void **)&o) != S_OK || !o)
		return 20;
	if (o->vtbl->Value(o) != 42 || objects != 1)
		return 21;
	if (o->vtbl->Release(o) || objects)
		return 22;
	if (CoCreateInstance(&clsid_test, NULL, CLSCTX_INPROC_SERVER,
			     &iid_factory, &p) != E_NOINTERFACE || p || objects)
		return 23;
	if (CoCreateInstance(&iid_test, NULL, CLSCTX_INPROC_SERVER, &iid_test,
			     &p) != REGDB_E_CLASSNOTREG || p)
		return 24;

	if (StringFromGUID2(&clsid_test, s, 40) != 39 ||
	    CLSIDFromString(s, &parsed) != S_OK || !guid_eq(&parsed, &clsid_test))
		return 30;

	int v = check_version();
	if (v)
		return 40 + v;

	CoUninitialize();
	CoUninitialize();
	return 0;
}

BOOL WINAPI DllMain(HINSTANCE inst, DWORD reason, LPVOID reserved)
{
	if (reason == DLL_PROCESS_ATTACH)
		self = inst;
	return TRUE;
}
