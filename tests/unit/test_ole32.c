/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * ole32, called through its export table: apartments, task memory, GUID
 * strings, and class objects, both registered in-process and loaded from
 * an InprocServer32 DLL (the pe_com fixture) found in the registry.
 */
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "loader/modules.h"
#include "runtime/lowmem.h"
#include "runtime/runtime.h"
#include "win32/codepage.h"
#include "win32/internal.h"
#include "win32/path.h"
#include "win32/reg.h"
#include "test.h"

static struct module *ole, *k32;

static void *get_from(struct module *m, const char *name)
{
	void *p = mod_get_proc(m, name);
	if (!p) {
		fprintf(stderr, "%s has no %s\n", m->name, name);
		exit(1);
	}
	return p;
}

#define FN(ret, name, ...) \
	ret (WINAPI *name)(__VA_ARGS__) = \
		(ret (WINAPI *)(__VA_ARGS__))get_from(ole, #name)
#define K32(ret, name, ...) \
	ret (WINAPI *name)(__VA_ARGS__) = \
		(ret (WINAPI *)(__VA_ARGS__))get_from(k32, #name)

/* pe_com.dll's class, and an interface nobody implements. */
static const CLSID clsid_test = { 0x5e6c9b21, 0x3a7d, 0x4f0e,
	{ 0x9c, 0x11, 0x0d, 0x2b, 0x7a, 0x6e, 0x4f, 0x80 } };
#define CLSID_TEST "{5E6C9B21-3A7D-4F0E-9C11-0D2B7A6E4F80}"
static const IID iid_other = { 0x12345678, 0x9abc, 0xdef0,
	{ 1, 2, 3, 4, 5, 6, 7, 8 } };
static const IID iid_unknown = { 0, 0, 0,
	{ 0xc0, 0, 0, 0, 0, 0, 0, 0x46 } };
static const IID iid_factory = { 1, 0, 0,
	{ 0xc0, 0, 0, 0, 0, 0, 0, 0x46 } };

/* Something that isn't a stream on global memory. */
static const void *task_ole_dummy[1];

static WCHAR *wide(const char *s)
{
	return utf8_to_utf16_dup(s);
}

static char *narrow(const WCHAR *w)
{
	return utf16_to_utf8_dup(w);
}

static void set_class_value(const char *key, const char *value)
{
	WCHAR *w = wide(value);
	LONG err = 0;

	reg_lock();
	struct reg_key *k = reg_create(key, 0, NULL, &err);
	CHECK(k && w);
	if (k && w)
		CHECK_EQ(reg_set(k, "", REG_SZ, w,
				 (DWORD)((wstrlen(w) + 1) * sizeof(WCHAR))), 0);
	reg_unlock();
	free(w);
}

static void test_memory(void)
{
	FN(LPVOID, CoTaskMemAlloc, SIZE_T);
	FN(LPVOID, CoTaskMemRealloc, LPVOID, SIZE_T);
	FN(void, CoTaskMemFree, LPVOID);
	FN(HRESULT, CoGetMalloc, DWORD, LPMALLOC *);

	BYTE *p = CoTaskMemAlloc(100);
	CHECK(p != NULL);
	if (lowmem_active())
		CHECK((uintptr_t)p < 0x80000000u);
	memset(p, 0xab, 100);
	p = CoTaskMemRealloc(p, 5000);
	CHECK(p && p[99] == 0xab);
	CHECK(CoTaskMemRealloc(p, 0) == NULL);
	p = CoTaskMemRealloc(NULL, 10);
	CHECK(p != NULL);
	CoTaskMemFree(p);
	CoTaskMemFree(NULL);

	IMalloc *m = NULL;
	CHECK_EQ(CoGetMalloc(0, &m), E_INVALIDARG);
	CHECK(m == NULL);
	CHECK_EQ(CoGetMalloc(1, &m), S_OK);
	CHECK(m != NULL);
	if (!m)
		return;
	IMalloc *m2 = NULL;
	CHECK_EQ(CoGetMalloc(1, &m2), S_OK);
	CHECK(m2 == m);
	void *q = NULL;
	CHECK_EQ(m->lpVtbl->QueryInterface(m, &iid_unknown, &q), S_OK);
	CHECK(q == m);
	CHECK_EQ(m->lpVtbl->QueryInterface(m, &iid_other, &q), E_NOINTERFACE);
	CHECK(q == NULL);
	p = m->lpVtbl->Alloc(m, 64);
	CHECK(p != NULL);
	CHECK_EQ(m->lpVtbl->GetSize(m, p), 64);
	CHECK_EQ(m->lpVtbl->DidAlloc(m, p), 1);
	int local;
	CHECK_EQ(m->lpVtbl->DidAlloc(m, &local), 0);
	CHECK_EQ(m->lpVtbl->DidAlloc(m, NULL), -1);
	CHECK_EQ(m->lpVtbl->GetSize(m, NULL), (SIZE_T)-1);
	p = m->lpVtbl->Realloc(m, p, 128);
	CHECK_EQ(m->lpVtbl->GetSize(m, p), 128);
	m->lpVtbl->Free(m, p);
	m->lpVtbl->HeapMinimize(m);
	m->lpVtbl->Release(m);
	m->lpVtbl->Release(m2);
}

static void test_guids(void)
{
	FN(INT, StringFromGUID2, REFGUID, LPOLESTR, INT);
	FN(HRESULT, StringFromCLSID, REFCLSID, LPOLESTR *);
	FN(HRESULT, StringFromIID, REFIID, LPOLESTR *);
	FN(HRESULT, CLSIDFromString, LPCOLESTR, LPCLSID);
	FN(HRESULT, IIDFromString, LPCOLESTR, LPIID);
	FN(HRESULT, CLSIDFromProgID, LPCOLESTR, LPCLSID);
	FN(HRESULT, ProgIDFromCLSID, REFCLSID, LPOLESTR *);
	FN(HRESULT, CoCreateGuid, GUID *);
	FN(BOOL, IsEqualGUID, REFGUID, REFGUID);
	FN(void, CoTaskMemFree, LPVOID);

	WCHAR buf[40];
	CHECK_EQ(StringFromGUID2(&iid_factory, buf, 38), 0);
	CHECK_EQ(StringFromGUID2(&iid_factory, buf, 40), 39);
	char *s = narrow(buf);
	CHECK_STR(s, "{00000001-0000-0000-C000-000000000046}");
	free(s);

	LPOLESTR str = NULL;
	CHECK_EQ(StringFromCLSID(&clsid_test, &str), S_OK);
	s = narrow(str);
	CHECK_STR(s, CLSID_TEST);
	free(s);
	CoTaskMemFree(str);
	CHECK_EQ(StringFromIID(&iid_other, &str), S_OK);
	s = narrow(str);
	CHECK_STR(s, "{12345678-9ABC-DEF0-0102-030405060708}");
	free(s);

	GUID g;
	CHECK_EQ(CLSIDFromString(str, &g), S_OK);
	CHECK(IsEqualGUID(&g, &iid_other));
	CHECK(!IsEqualGUID(&g, &clsid_test));
	CHECK_EQ(IIDFromString(str, &g), S_OK);
	CHECK(IsEqualGUID(&g, &iid_other));
	CoTaskMemFree(str);

	WCHAR *w = wide("{5e6c9b21-3a7d-4f0e-9c11-0d2b7a6e4f80}");
	CHECK_EQ(CLSIDFromString(w, &g), S_OK);
	CHECK(IsEqualGUID(&g, &clsid_test));
	free(w);
	static const char *const bad[] = {
		"{5E6C9B21-3A7D-4F0E-9C11-0D2B7A6E4F8}",
		"{5E6C9B21-3A7D-4F0E-9C11-0D2B7A6E4F800}",
		"{5E6C9B21-3A7D-4F0E-9C11-0D2B7A6E4F8G}",
		"5E6C9B21-3A7D-4F0E-9C11-0D2B7A6E4F80",
		"{5E6C9B21:3A7D-4F0E-9C11-0D2B7A6E4F80}",
	};
	for (size_t i = 0; i < sizeof(bad) / sizeof(*bad); i++) {
		w = wide(bad[i]);
		memset(&g, 0xff, sizeof(g));
		CHECK_EQ(CLSIDFromString(w, &g), CO_E_CLASSSTRING);
		CHECK_EQ(g.Data1, 0);
		CHECK_EQ(IIDFromString(w, &g), E_INVALIDARG);
		free(w);
	}
	memset(&g, 0xff, sizeof(g));
	CHECK_EQ(CLSIDFromString(NULL, &g), S_OK);
	CHECK(g.Data1 == 0 && g.Data4[7] == 0);

	/* ProgIDs, per-user classes winning over the machine's. */
	set_class_value("HKEY_LOCAL_MACHINE\\Software\\Classes\\Seance.Test\\CLSID",
			"{00000000-0000-0000-0000-000000000001}");
	set_class_value("HKEY_CURRENT_USER\\Software\\Classes\\Seance.Test\\CLSID",
			CLSID_TEST);
	set_class_value("HKEY_LOCAL_MACHINE\\Software\\Classes\\CLSID\\" CLSID_TEST
			"\\ProgID", "Seance.Test");
	w = wide("Seance.Test");
	CHECK_EQ(CLSIDFromProgID(w, &g), S_OK);
	CHECK(IsEqualGUID(&g, &clsid_test));
	memset(&g, 0, sizeof(g));
	CHECK_EQ(CLSIDFromString(w, &g), S_OK);
	CHECK(IsEqualGUID(&g, &clsid_test));
	free(w);
	w = wide("Seance.Nothing");
	CHECK_EQ(CLSIDFromProgID(w, &g), CO_E_CLASSSTRING);
	CHECK_EQ(CLSIDFromString(w, &g), CO_E_CLASSSTRING);
	free(w);
	CHECK_EQ(ProgIDFromCLSID(&clsid_test, &str), S_OK);
	s = narrow(str);
	CHECK_STR(s, "Seance.Test");
	free(s);
	CoTaskMemFree(str);
	CHECK_EQ(ProgIDFromCLSID(&iid_other, &str), REGDB_E_CLASSNOTREG);
	CHECK(str == NULL);

	GUID a, b;
	CHECK_EQ(CoCreateGuid(&a), S_OK);
	CHECK_EQ(CoCreateGuid(&b), S_OK);
	CHECK(!IsEqualGUID(&a, &b));
	CHECK_EQ(a.Data3 >> 12, 4);
	CHECK_EQ(a.Data4[0] >> 6, 2);
}

/* A class factory on the harness side, for CoRegisterClassObject. */
static int factory_refs, factory_creates;

static HRESULT WINAPI cf_qi(IClassFactory *cf, REFIID riid, void **ppv)
{
	if (!memcmp(riid, &iid_unknown, sizeof(IID)) ||
	    !memcmp(riid, &iid_factory, sizeof(IID))) {
		factory_refs++;
		*ppv = cf;
		return S_OK;
	}
	*ppv = NULL;
	return E_NOINTERFACE;
}

static ULONG WINAPI cf_addref(IClassFactory *cf)
{
	return ++factory_refs;
}

static ULONG WINAPI cf_release(IClassFactory *cf)
{
	return --factory_refs;
}

/* Hands out the factory itself as the object. */
static HRESULT WINAPI cf_create(IClassFactory *cf, IUnknown *outer,
				REFIID riid, void **ppv)
{
	factory_creates++;
	return cf_qi(cf, riid, ppv);
}

static HRESULT WINAPI cf_lock(IClassFactory *cf, BOOL lock)
{
	return S_OK;
}

static const IClassFactoryVtbl cf_vtbl = {
	cf_qi, cf_addref, cf_release, cf_create, cf_lock
};
static IClassFactory factory = { &cf_vtbl };

static HRESULT thread_hr;

static DWORD WINAPI implicit_mta(LPVOID arg)
{
	HRESULT (WINAPI *get_class)(REFCLSID, DWORD, LPVOID, REFIID, LPVOID *) =
		get_from(ole, "CoGetClassObject");
	void *p;

	thread_hr = get_class(&iid_other, CLSCTX_INPROC_SERVER, NULL,
			      &iid_factory, &p);
	return 0;
}

static void test_apartments(void)
{
	FN(HRESULT, CoInitialize, LPVOID);
	FN(HRESULT, CoInitializeEx, LPVOID, DWORD);
	FN(void, CoUninitialize, void);
	FN(HRESULT, OleInitialize, LPVOID);
	FN(void, OleUninitialize, void);
	FN(HRESULT, CoCreateInstance, REFCLSID, LPUNKNOWN, DWORD, REFIID, LPVOID *);
	K32(HANDLE, CreateThread, LPSECURITY_ATTRIBUTES, SIZE_T,
	    LPTHREAD_START_ROUTINE, LPVOID, DWORD, LPDWORD);
	K32(DWORD, WaitForSingleObject, HANDLE, DWORD);
	K32(BOOL, CloseHandle, HANDLE);
	void *p = &p;

	CHECK_EQ(CoCreateInstance(&iid_other, NULL, CLSCTX_ALL, &iid_unknown, &p),
		 CO_E_NOTINITIALIZED);
	CHECK(p == NULL);
	CHECK_EQ(CoInitializeEx(&p, COINIT_MULTITHREADED), E_INVALIDARG);
	CHECK_EQ(CoInitializeEx(NULL, COINIT_MULTITHREADED), S_OK);
	CHECK_EQ(CoInitializeEx(NULL, COINIT_MULTITHREADED), S_FALSE);
	CHECK_EQ(CoInitialize(NULL), RPC_E_CHANGED_MODE);

	/* Another thread is in the multithreaded apartment implicitly. */
	HANDLE t = CreateThread(NULL, 0, implicit_mta, NULL, 0, NULL);
	CHECK(t && WaitForSingleObject(t, 10000) == WAIT_OBJECT_0);
	CloseHandle(t);
	CHECK_EQ(thread_hr, REGDB_E_CLASSNOTREG);

	CoUninitialize();
	CoUninitialize();
	CoUninitialize();	/* one too many: ignored */
	t = CreateThread(NULL, 0, implicit_mta, NULL, 0, NULL);
	CHECK(t && WaitForSingleObject(t, 10000) == WAIT_OBJECT_0);
	CloseHandle(t);
	CHECK_EQ(thread_hr, CO_E_NOTINITIALIZED);

	CHECK_EQ(OleInitialize(NULL), S_OK);
	CHECK_EQ(CoInitialize(NULL), S_FALSE);
	CHECK_EQ(CoInitializeEx(NULL, COINIT_MULTITHREADED), RPC_E_CHANGED_MODE);
	CoUninitialize();
	OleUninitialize();
	CHECK_EQ(CoCreateInstance(&iid_other, NULL, CLSCTX_ALL, &iid_unknown, &p),
		 CO_E_NOTINITIALIZED);
}

static void test_registered(void)
{
	FN(HRESULT, CoInitialize, LPVOID);
	FN(void, CoUninitialize, void);
	FN(HRESULT, CoRegisterClassObject, REFCLSID, LPUNKNOWN, DWORD, DWORD,
	   LPDWORD);
	FN(HRESULT, CoRevokeClassObject, DWORD);
	FN(HRESULT, CoResumeClassObjects, void);
	FN(HRESULT, CoGetClassObject, REFCLSID, DWORD, LPVOID, REFIID, LPVOID *);
	FN(HRESULT, CoCreateInstance, REFCLSID, LPUNKNOWN, DWORD, REFIID, LPVOID *);
	DWORD cookie = 5;
	void *p;

	CHECK_EQ(CoRegisterClassObject(&iid_other, (IUnknown *)&factory,
				       CLSCTX_INPROC_SERVER, REGCLS_MULTIPLEUSE,
				       &cookie), CO_E_NOTINITIALIZED);
	CHECK_EQ(cookie, 0);
	CHECK_EQ(CoInitialize(NULL), S_OK);
	CHECK_EQ(CoRegisterClassObject(&iid_other, (IUnknown *)&factory,
				       CLSCTX_INPROC_SERVER, REGCLS_MULTIPLEUSE,
				       &cookie), S_OK);
	CHECK(cookie != 0);
	CHECK_EQ(factory_refs, 1);

	CHECK_EQ(CoGetClassObject(&iid_other, CLSCTX_INPROC_SERVER, NULL,
				  &iid_factory, &p), S_OK);
	CHECK(p == &factory);
	CHECK_EQ(factory_refs, 2);
	cf_release(&factory);
	/* A context it wasn't registered for. */
	CHECK_EQ(CoGetClassObject(&iid_other, CLSCTX_LOCAL_SERVER, NULL,
				  &iid_factory, &p), REGDB_E_CLASSNOTREG);

	CHECK_EQ(CoCreateInstance(&iid_other, NULL, CLSCTX_ALL, &iid_unknown, &p),
		 S_OK);
	CHECK(p == &factory);
	CHECK_EQ(factory_creates, 1);
	CHECK_EQ(factory_refs, 2);	/* the registration and p */
	cf_release(&factory);
	CHECK_EQ(CoCreateInstance(&iid_other, NULL, CLSCTX_ALL, &iid_other, &p),
		 E_NOINTERFACE);
	CHECK(p == NULL);
	CHECK_EQ(CoCreateInstance(&iid_other, NULL, CLSCTX_ALL, &iid_unknown, NULL),
		 E_POINTER);

	CHECK_EQ(CoRevokeClassObject(cookie), S_OK);
	CHECK_EQ(factory_refs, 0);
	CHECK_EQ(CoRevokeClassObject(cookie), E_INVALIDARG);
	CHECK_EQ(CoCreateInstance(&iid_other, NULL, CLSCTX_ALL, &iid_unknown, &p),
		 REGDB_E_CLASSNOTREG);

	/* Suspended until CoResumeClassObjects. */
	CHECK_EQ(CoRegisterClassObject(&iid_other, (IUnknown *)&factory,
				       CLSCTX_INPROC_SERVER,
				       REGCLS_MULTIPLEUSE | REGCLS_SUSPENDED,
				       &cookie), S_OK);
	CHECK_EQ(CoGetClassObject(&iid_other, CLSCTX_INPROC_SERVER, NULL,
				  &iid_factory, &p), REGDB_E_CLASSNOTREG);
	CHECK_EQ(CoResumeClassObjects(), S_OK);
	CHECK_EQ(CoGetClassObject(&iid_other, CLSCTX_INPROC_SERVER, NULL,
				  &iid_factory, &p), S_OK);
	cf_release(&factory);
	CHECK_EQ(CoRevokeClassObject(cookie), S_OK);

	/* Single use: handed out once, then hidden (but still revocable). */
	CHECK_EQ(CoRegisterClassObject(&iid_other, (IUnknown *)&factory,
				       CLSCTX_INPROC_SERVER, REGCLS_SINGLEUSE,
				       &cookie), S_OK);
	CHECK_EQ(CoGetClassObject(&iid_other, CLSCTX_INPROC_SERVER, NULL,
				  &iid_factory, &p), S_OK);
	cf_release(&factory);
	CHECK_EQ(CoGetClassObject(&iid_other, CLSCTX_INPROC_SERVER, NULL,
				  &iid_factory, &p), REGDB_E_CLASSNOTREG);
	CHECK_EQ(CoRevokeClassObject(cookie), S_OK);

	/* A multiple-use local server is in-process to its own process. */
	CHECK_EQ(CoRegisterClassObject(&iid_other, (IUnknown *)&factory,
				       CLSCTX_LOCAL_SERVER, REGCLS_MULTIPLEUSE,
				       &cookie), S_OK);
	CHECK_EQ(CoGetClassObject(&iid_other, CLSCTX_INPROC_SERVER, NULL,
				  &iid_factory, &p), S_OK);
	cf_release(&factory);
	CHECK_EQ(CoRevokeClassObject(cookie), S_OK);
	CHECK_EQ(CoRegisterClassObject(&iid_other, (IUnknown *)&factory,
				       CLSCTX_LOCAL_SERVER, REGCLS_MULTI_SEPARATE,
				       &cookie), S_OK);
	CHECK_EQ(CoGetClassObject(&iid_other, CLSCTX_INPROC_SERVER, NULL,
				  &iid_factory, &p), REGDB_E_CLASSNOTREG);
	CHECK_EQ(CoRevokeClassObject(cookie), S_OK);
	CHECK_EQ(factory_refs, 0);
	CoUninitialize();
}

/* Streams on global memory, as filters keep their state in. */
static void test_streams(void)
{
	FN(HRESULT, CreateStreamOnHGlobal, HGLOBAL, BOOL, LPSTREAM *);
	FN(HRESULT, GetHGlobalFromStream, IStream *, HGLOBAL *);
	K32(HGLOBAL, GlobalAlloc, UINT, SIZE_T);
	K32(LPVOID, GlobalLock, HGLOBAL);
	K32(BOOL, GlobalUnlock, HGLOBAL);
	K32(SIZE_T, GlobalSize, HGLOBAL);
	K32(HGLOBAL, GlobalFree, HGLOBAL);
	IStream *s = NULL, *c = NULL;
	ULONG n = 0;
	ULARGE_INTEGER pos, size;
	LARGE_INTEGER off;
	char buf[64];

	CHECK_EQ(CreateStreamOnHGlobal(NULL, TRUE, NULL), E_INVALIDARG);
	CHECK_EQ(CreateStreamOnHGlobal(NULL, TRUE, &s), S_OK);
	if (!s)
		return;
	void *q = NULL;
	CHECK_EQ(s->lpVtbl->QueryInterface(s, &IID_ISequentialStream, &q), S_OK);
	CHECK(q == s);
	s->lpVtbl->Release(s);
	CHECK_EQ(s->lpVtbl->QueryInterface(s, &iid_other, &q), E_NOINTERFACE);

	CHECK_EQ(s->lpVtbl->Write(s, "hello, world", 12, &n), S_OK);
	CHECK_EQ(n, 12);
	off.QuadPart = 7;
	CHECK_EQ(s->lpVtbl->Seek(s, off, STREAM_SEEK_SET, &pos), S_OK);
	CHECK_EQ(pos.QuadPart, 7);
	memset(buf, 0, sizeof(buf));
	CHECK_EQ(s->lpVtbl->Read(s, buf, sizeof(buf), &n), S_OK);
	CHECK_EQ(n, 5);
	CHECK_STR(buf, "world");
	CHECK_EQ(s->lpVtbl->Read(s, buf, sizeof(buf), &n), S_OK);
	CHECK_EQ(n, 0);

	off.QuadPart = -5;
	CHECK_EQ(s->lpVtbl->Seek(s, off, STREAM_SEEK_END, &pos), S_OK);
	CHECK_EQ(pos.QuadPart, 7);
	off.QuadPart = -8;
	CHECK_EQ(s->lpVtbl->Seek(s, off, STREAM_SEEK_CUR, NULL),
		 STG_E_SEEKERROR);
	CHECK_EQ(s->lpVtbl->Seek(s, off, 7, NULL), STG_E_SEEKERROR);

	/* Writing past the end grows it, with a gap. */
	off.QuadPart = 20;
	s->lpVtbl->Seek(s, off, STREAM_SEEK_SET, NULL);
	CHECK_EQ(s->lpVtbl->Write(s, "!", 1, NULL), S_OK);
	STATSTG st;
	CHECK_EQ(s->lpVtbl->Stat(s, &st, STATFLAG_NONAME), S_OK);
	CHECK_EQ(st.type, STGTY_STREAM);
	CHECK_EQ(st.cbSize.QuadPart, 21);
	CHECK(st.pwcsName == NULL);
	size.QuadPart = 5;
	CHECK_EQ(s->lpVtbl->SetSize(s, size), S_OK);
	s->lpVtbl->Stat(s, &st, STATFLAG_NONAME);
	CHECK_EQ(st.cbSize.QuadPart, 5);

	/* A clone shares the data and starts at the same place. */
	off.QuadPart = 1;
	s->lpVtbl->Seek(s, off, STREAM_SEEK_SET, NULL);
	CHECK_EQ(s->lpVtbl->Clone(s, &c), S_OK);
	if (c) {
		memset(buf, 0, sizeof(buf));
		c->lpVtbl->Read(c, buf, sizeof(buf), &n);
		CHECK_STR(buf, "ello");
		off.QuadPart = 0;
		c->lpVtbl->Seek(c, off, STREAM_SEEK_SET, NULL);
		c->lpVtbl->Write(c, "J", 1, NULL);
	}
	off.QuadPart = 0;
	s->lpVtbl->Seek(s, off, STREAM_SEEK_SET, NULL);
	memset(buf, 0, sizeof(buf));
	s->lpVtbl->Read(s, buf, sizeof(buf), &n);
	CHECK_STR(buf, "Jello");

	/* CopyTo, into a stream on a block of our own. */
	HGLOBAL mine = GlobalAlloc(GMEM_MOVEABLE, 3);
	char *p = GlobalLock(mine);
	memcpy(p, "abc", 3);
	GlobalUnlock(mine);
	IStream *d = NULL;
	CHECK_EQ(CreateStreamOnHGlobal(mine, FALSE, &d), S_OK);
	if (d) {
		ULARGE_INTEGER cb = { .QuadPart = 3 }, r, w;
		d->lpVtbl->Stat(d, &st, STATFLAG_NONAME);
		CHECK_EQ(st.cbSize.QuadPart, 3);	/* the block's size */
		off.QuadPart = 1;
		s->lpVtbl->Seek(s, off, STREAM_SEEK_SET, NULL);
		off.QuadPart = 0;
		d->lpVtbl->Seek(d, off, STREAM_SEEK_END, NULL);
		CHECK_EQ(s->lpVtbl->CopyTo(s, d, cb, &r, &w), S_OK);
		CHECK(r.QuadPart == 3 && w.QuadPart == 3);
		HGLOBAL h = NULL;
		CHECK_EQ(GetHGlobalFromStream(d, &h), S_OK);
		CHECK(h == mine);
		CHECK_EQ(GlobalSize(mine), 6);
		p = GlobalLock(mine);
		CHECK(p && !memcmp(p, "abcell", 6));
		GlobalUnlock(mine);
		d->lpVtbl->Release(d);
	}
	/* Not deleted on release: still ours. */
	CHECK_EQ(GlobalSize(mine), 6);
	GlobalFree(mine);

	HGLOBAL h = (HGLOBAL)1;
	CHECK_EQ(GetHGlobalFromStream((IStream *)&task_ole_dummy, &h),
		 E_INVALIDARG);
	CHECK(h == NULL);
	CHECK_EQ(s->lpVtbl->LockRegion(s, pos, pos, 0), STG_E_INVALIDFUNCTION);
	if (c)
		CHECK_EQ(c->lpVtbl->Release(c), 0);
	CHECK_EQ(s->lpVtbl->Release(s), 0);
}

/* "Marshalling" within the process: the same pointer comes back. */
static void test_marshal(void)
{
	FN(HRESULT, CoMarshalInterThreadInterfaceInStream, REFIID, LPUNKNOWN,
	   LPSTREAM *);
	FN(HRESULT, CoGetInterfaceAndReleaseStream, LPSTREAM, REFIID, LPVOID *);
	FN(HRESULT, CoCreateFreeThreadedMarshaler, LPUNKNOWN, LPUNKNOWN *);
	FN(HRESULT, CreateStreamOnHGlobal, HGLOBAL, BOOL, LPSTREAM *);
	FN(HRESULT, CoMarshalInterface, IStream *, REFIID, IUnknown *, DWORD,
	   void *, DWORD);
	FN(HRESULT, CoReleaseMarshalData, IStream *);
	IUnknown *ftm = NULL, *got = NULL;
	IStream *s = NULL;

	CHECK_EQ(CoCreateFreeThreadedMarshaler(NULL, &ftm), S_OK);
	if (!ftm)
		return;
	void *q = NULL;
	CHECK_EQ(ftm->lpVtbl->QueryInterface(ftm, &iid_other, &q),
		 E_NOINTERFACE);

	CHECK_EQ(CoMarshalInterThreadInterfaceInStream(&IID_IUnknown, ftm, &s),
		 S_OK);
	CHECK_EQ(ftm->lpVtbl->AddRef(ftm), 3);	/* the stream holds one */
	ftm->lpVtbl->Release(ftm);
	CHECK_EQ(CoGetInterfaceAndReleaseStream(s, &IID_IUnknown,
						(void **)&got), S_OK);
	CHECK(got == ftm);
	if (got)
		got->lpVtbl->Release(got);

	/* Released without being unmarshalled. */
	CHECK_EQ(CreateStreamOnHGlobal(NULL, TRUE, &s), S_OK);
	CHECK_EQ(CoMarshalInterface(s, &IID_IUnknown, ftm, 0, NULL, 0), S_OK);
	CHECK_EQ(CoMarshalInterface(s, &iid_other, ftm, 0, NULL, 0),
		 E_NOINTERFACE);
	LARGE_INTEGER zero = { .QuadPart = 0 };
	s->lpVtbl->Seek(s, zero, STREAM_SEEK_SET, NULL);
	CHECK_EQ(CoReleaseMarshalData(s), S_OK);
	CHECK_EQ(CoReleaseMarshalData(s), STG_E_READFAULT);
	s->lpVtbl->Release(s);
	CHECK_EQ(ftm->lpVtbl->Release(ftm), 0);
}

/* pe_com.dll, found through HKCR\CLSID\{...}\InprocServer32. */
static void test_inproc_server(void)
{
	FN(HRESULT, CoInitialize, LPVOID);
	FN(void, CoUninitialize, void);
	FN(HRESULT, CoGetClassObject, REFCLSID, DWORD, LPVOID, REFIID, LPVOID *);
	const char *key = "HKEY_LOCAL_MACHINE\\Software\\Classes\\CLSID\\"
		CLSID_TEST "\\InprocServer32";
	void *p;

	struct module *m = mod_load(fixture("pe_com.dll"), NULL);
	CHECK(m != NULL);
	if (!m)
		return;
	char *path = path_to_windows(m->path);
	set_class_value(key, path);
	free(path);

	int (*run)(void) = mod_get_proc(m, "com_run");
	int (*objects)(void) = mod_get_proc(m, "com_objects");
	CHECK(run && objects);
	if (run && objects) {
		CHECK_EQ(run(), 0);
		CHECK_EQ(objects(), 0);
	}

	/* A server that isn't there, or isn't a COM server. */
	CHECK_EQ(CoInitialize(NULL), S_OK);
	set_class_value(key, "Z:\\no\\such\\server.dll");
	CHECK_EQ(CoGetClassObject(&clsid_test, CLSCTX_INPROC_SERVER, NULL,
				  &iid_factory, &p), CO_E_DLLNOTFOUND);
	path = path_to_windows(fixture("pe_basic.dll"));
	set_class_value(key, path);
	free(path);
	CHECK_EQ(CoGetClassObject(&clsid_test, CLSCTX_INPROC_SERVER, NULL,
				  &iid_factory, &p), CO_E_ERRORINDLL);
	CHECK_EQ(CoGetClassObject(&clsid_test, CLSCTX_LOCAL_SERVER, NULL,
				  &iid_factory, &p), REGDB_E_CLASSNOTREG);
	CoUninitialize();
	mod_free(m);
}

int main(void)
{
	char dir[] = "/tmp/vfw-ole-XXXXXX";
	if (!mkdtemp(dir)) {
		perror("mkdtemp");
		return 1;
	}
	setenv("VFW_ROOT", dir, 1);
	setenv("VFW_REGISTRY", "", 1);
	CHECK_EQ(lowmem_reserve(), 0);

	CHECK_EQ(runtime_enter(), 0);
	reg_reload();
	ole = mod_load("ole32.dll", NULL);
	k32 = mod_load("kernel32.dll", NULL);
	CHECK(ole && ole->builtin && !ole->builtin->fallback && k32);
	if (!ole || !k32)
		return test_summary("test_ole32");

	test_memory();
	test_guids();
	test_apartments();
	test_registered();
	test_inproc_server();
	test_streams();
	test_marshal();

	mod_free(k32);
	mod_free(ole);
	char cmd[64];
	snprintf(cmd, sizeof(cmd), "rm -rf '%s'", dir);
	if (system(cmd))
		fprintf(stderr, "could not remove %s\n", dir);
	return test_summary("test_ole32");
}
