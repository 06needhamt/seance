/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * ole32, called through its export table: apartments, task memory, GUID
 * strings, and class objects, both registered in-process and loaded from
 * an InprocServer32 DLL (the pe_com fixture) found in the registry; streams,
 * marshalling and structured storage (compound files).
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

	int (CDECL *run)(void) = mod_get_proc(m, "com_run");
	int (CDECL *objects)(void) = mod_get_proc(m, "com_objects");
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

	/*
	 * A server registered by bare name, as Matrox's swc*vfw.dll register
	 * themselves for digivcap.dll: not on the search path here, but next
	 * to the DLL asking for it, which finds it (and that it is no COM
	 * server) through either call.
	 */
	HRESULT (CDECL *get)(const GUID *, int) = mod_get_proc(m, "com_get");
	CHECK(get != NULL);
	set_class_value(key, "pe_fourccs.dll");
	unsetenv("VFW_PATH");
	CHECK_EQ(CoGetClassObject(&clsid_test, CLSCTX_INPROC_SERVER, NULL,
				  &iid_factory, &p), CO_E_DLLNOTFOUND);
	if (get) {
		CHECK_EQ(get(&clsid_test, 0), CO_E_ERRORINDLL);
		CHECK_EQ(get(&clsid_test, 1), CO_E_ERRORINDLL);
	}
	CoUninitialize();
	mod_free(m);
}

static const CLSID clsid_doc = { 0x11223344, 0x5566, 0x7788,
	{ 0x99, 0xaa, 0xbb, 0xcc, 0xdd, 0xee, 0xff, 0x00 } };

#define EXCL (STGM_SHARE_EXCLUSIVE)
#define RW (STGM_READWRITE | STGM_SHARE_EXCLUSIVE)

/* Writes n bytes of a pattern seeded by seed into a new stream. */
static void put_stream(IStorage *stg, const char *name, ULONG n, int seed)
{
	WCHAR *w = wide(name);
	IStream *s = NULL;
	CHECK_EQ(stg->lpVtbl->CreateStream(stg, w, RW | STGM_CREATE, 0, 0, &s),
		 S_OK);
	free(w);
	if (!s)
		return;
	unsigned char *b = malloc(n ? n : 1);
	for (ULONG i = 0; i < n; i++)
		b[i] = (unsigned char)(i * 7 + seed);
	ULONG put = 0;
	CHECK_EQ(s->lpVtbl->Write(s, b, n, &put), S_OK);
	CHECK_EQ(put, n);
	free(b);
	s->lpVtbl->Release(s);
}

/* Checks a stream put_stream() wrote. */
static int got_stream(IStorage *stg, const char *name, ULONG n, int seed)
{
	WCHAR *w = wide(name);
	IStream *s = NULL;
	HRESULT hr = stg->lpVtbl->OpenStream(stg, w, NULL, STGM_READ | EXCL, 0,
					     &s);
	free(w);
	if (hr != S_OK)
		return 0;
	STATSTG st;
	int ok = s->lpVtbl->Stat(s, &st, STATFLAG_NONAME) == S_OK &&
		 st.cbSize.QuadPart == n && st.type == STGTY_STREAM;
	unsigned char *b = malloc(n + 16);
	ULONG got = 0;
	ok = ok && s->lpVtbl->Read(s, b, n + 16, &got) == S_OK && got == n;
	for (ULONG i = 0; ok && i < n; i++)
		ok = b[i] == (unsigned char)(i * 7 + seed);
	free(b);
	s->lpVtbl->Release(s);
	return ok;
}

static void test_storage_memory(void)
{
	FN(HRESULT, CreateILockBytesOnHGlobal, HGLOBAL, BOOL, ILockBytes **);
	FN(HRESULT, GetHGlobalFromILockBytes, ILockBytes *, HGLOBAL *);
	FN(HRESULT, StgCreateDocfileOnILockBytes, ILockBytes *, DWORD, DWORD,
	   IStorage **);
	FN(HRESULT, StgOpenStorageOnILockBytes, ILockBytes *, IStorage *, DWORD,
	   SNB, DWORD, IStorage **);
	FN(HRESULT, StgIsStorageILockBytes, ILockBytes *);
	FN(void, CoTaskMemFree, void *);
	K32(LPVOID, GlobalLock, HGLOBAL);
	K32(BOOL, GlobalUnlock, HGLOBAL);
	K32(SIZE_T, GlobalSize, HGLOBAL);
	ILockBytes *lb = NULL;
	IStorage *root = NULL, *sub = NULL;
	char name[16];

	CHECK_EQ(CreateILockBytesOnHGlobal(NULL, TRUE, &lb), S_OK);
	if (!lb)
		return;
	CHECK_EQ(StgIsStorageILockBytes(lb), S_FALSE);
	CHECK_EQ(StgCreateDocfileOnILockBytes(lb, RW | STGM_CREATE, 0, &root),
		 S_OK);
	CHECK_EQ(StgIsStorageILockBytes(lb), S_OK);
	if (!root)
		return;
	put_stream(root, "Small", 100, 1);
	put_stream(root, "Big", 10000, 2);
	put_stream(root, "Empty", 0, 0);
	for (int i = 0; i < 20; i++) {
		snprintf(name, sizeof(name), "s%02d", i);
		put_stream(root, name, (ULONG)(i * 300), i);
	}
	WCHAR *w = wide("Sub");
	CHECK_EQ(root->lpVtbl->CreateStorage(root, w, RW, 0, 0, &sub), S_OK);
	IStorage *dup = NULL;
	CHECK_EQ(root->lpVtbl->CreateStorage(root, w, RW, 0, 0, &dup),
		 STG_E_FILEALREADYEXISTS);
	free(w);
	if (sub) {
		put_stream(sub, "Inner", 5000, 3);
		CHECK_EQ(sub->lpVtbl->SetClass(sub, &clsid_doc), S_OK);
		CHECK_EQ(sub->lpVtbl->SetStateBits(sub, 0xf0, 0x30), S_OK);
		sub->lpVtbl->Release(sub);
	}
	/* Names: too long, or with a separator. */
	IStream *s = NULL;
	w = wide("0123456789012345678901234567890123");
	CHECK_EQ(root->lpVtbl->CreateStream(root, w, RW, 0, 0, &s),
		 STG_E_INVALIDNAME);
	free(w);
	w = wide("a\\b");
	CHECK_EQ(root->lpVtbl->CreateStream(root, w, RW, 0, 0, &s),
		 STG_E_INVALIDNAME);
	free(w);
	/* Elements are opened exclusively. */
	w = wide("Small");
	CHECK_EQ(root->lpVtbl->OpenStream(root, w, NULL, STGM_READ |
					  STGM_SHARE_DENY_NONE, 0, &s),
		 STG_E_INVALIDFLAG);
	CHECK_EQ(root->lpVtbl->CreateStream(root, w, RW, 0, 0, &s),
		 STG_E_FILEALREADYEXISTS);
	free(w);
	CHECK_EQ(root->lpVtbl->Commit(root, STGC_DEFAULT), S_OK);
	root->lpVtbl->Release(root);

	/* The bytes: a version 3 header. */
	HGLOBAL h = NULL;
	CHECK_EQ(GetHGlobalFromILockBytes(lb, &h), S_OK);
	const unsigned char *p = h ? GlobalLock(h) : NULL;
	CHECK(p && GlobalSize(h) >= 1536 && GlobalSize(h) % 512 == 0);
	CHECK(p && p[0] == 0xd0 && p[1] == 0xcf && p[0x1a] == 3 && p[0x1e] == 9 &&
	      p[0x38] == 0 && p[0x39] == 0x10);
	if (h)
		GlobalUnlock(h);

	/* Read it back, read-only. */
	CHECK_EQ(StgOpenStorageOnILockBytes(lb, NULL, STGM_READ | EXCL, NULL, 0,
					    &root), S_OK);
	if (!root)
		return;
	CHECK(got_stream(root, "Small", 100, 1));
	CHECK(got_stream(root, "BIG", 10000, 2));	/* any case */
	CHECK(got_stream(root, "Empty", 0, 0));
	for (int i = 0; i < 20; i++) {
		snprintf(name, sizeof(name), "s%02d", i);
		CHECK(got_stream(root, name, (ULONG)(i * 300), i));
	}
	w = wide("Sub");
	CHECK_EQ(root->lpVtbl->OpenStorage(root, w, NULL, STGM_READ | EXCL, NULL,
					   0, &sub), S_OK);
	IStorage *again = NULL;
	CHECK_EQ(root->lpVtbl->OpenStorage(root, w, NULL, STGM_READ | EXCL, NULL,
					   0, &again), STG_E_ACCESSDENIED);
	CHECK_EQ(root->lpVtbl->OpenStream(root, w, NULL, STGM_READ | EXCL, 0, &s),
		 STG_E_FILENOTFOUND);
	free(w);
	if (sub) {
		STATSTG st;
		CHECK(got_stream(sub, "Inner", 5000, 3));
		CHECK_EQ(sub->lpVtbl->Stat(sub, &st, STATFLAG_DEFAULT), S_OK);
		CHECK(!memcmp(&st.clsid, &clsid_doc, sizeof(CLSID)));
		CHECK_EQ(st.grfStateBits, 0x30);
		CHECK_EQ(st.type, STGTY_STORAGE);
		char *n = narrow(st.pwcsName);
		CHECK_STR(n, "Sub");
		free(n);
		CoTaskMemFree(st.pwcsName);
		sub->lpVtbl->Release(sub);
	}
	/* Read-only. */
	w = wide("New");
	CHECK_EQ(root->lpVtbl->CreateStream(root, w, RW, 0, 0, &s),
		 STG_E_ACCESSDENIED);
	free(w);
	w = wide("Small");
	CHECK_EQ(root->lpVtbl->OpenStream(root, w, NULL, RW, 0, &s),
		 STG_E_ACCESSDENIED);
	CHECK_EQ(root->lpVtbl->OpenStream(root, w, NULL, STGM_READ | EXCL, 0, &s),
		 S_OK);
	if (s) {
		IStream *s2 = NULL;
		CHECK_EQ(root->lpVtbl->OpenStream(root, w, NULL, STGM_READ | EXCL,
						  0, &s2), STG_E_ACCESSDENIED);
		CHECK_EQ(s->lpVtbl->Write(s, "x", 1, NULL), STG_E_ACCESSDENIED);
		CHECK_EQ(root->lpVtbl->DestroyElement(root, w), STG_E_ACCESSDENIED);
		s->lpVtbl->Release(s);
	}
	free(w);
	w = wide("Nothing");
	CHECK_EQ(root->lpVtbl->OpenStream(root, w, NULL, STGM_READ | EXCL, 0, &s),
		 STG_E_FILENOTFOUND);
	free(w);

	/* Enumeration: every element, once. */
	IEnumSTATSTG *en = NULL;
	CHECK_EQ(root->lpVtbl->EnumElements(root, 0, NULL, 0, &en), S_OK);
	if (en) {
		STATSTG st[32];
		ULONG got = 0;
		int streams = 0, storages = 0, big = 0;
		CHECK_EQ(en->lpVtbl->Next(en, 32, st, &got), S_FALSE);
		CHECK_EQ(got, 24);
		for (ULONG i = 0; i < got; i++) {
			char *n = narrow(st[i].pwcsName);
			streams += st[i].type == STGTY_STREAM;
			storages += st[i].type == STGTY_STORAGE;
			if (n && !strcmp(n, "Big"))
				big = st[i].cbSize.QuadPart == 10000;
			free(n);
			CoTaskMemFree(st[i].pwcsName);
		}
		CHECK(streams == 23 && storages == 1 && big);
		IEnumSTATSTG *c = NULL;
		CHECK_EQ(en->lpVtbl->Reset(en), S_OK);
		CHECK_EQ(en->lpVtbl->Skip(en, 23), S_OK);
		CHECK_EQ(en->lpVtbl->Clone(en, &c), S_OK);
		if (c) {
			CHECK_EQ(c->lpVtbl->Next(c, 1, st, NULL), S_OK);
			CoTaskMemFree(st[0].pwcsName);
			CHECK_EQ(c->lpVtbl->Next(c, 1, st, NULL), S_FALSE);
			c->lpVtbl->Release(c);
		}
		en->lpVtbl->Release(en);
	}
	root->lpVtbl->Release(root);

	/* Not a storage. */
	ILockBytes *junk = NULL;
	CHECK_EQ(CreateILockBytesOnHGlobal(NULL, TRUE, &junk), S_OK);
	ULARGE_INTEGER zero = { 0 };
	char text[600];
	memset(text, 'x', sizeof(text));
	CHECK_EQ(junk->lpVtbl->WriteAt(junk, zero, text, sizeof(text), NULL),
		 S_OK);
	CHECK_EQ(StgOpenStorageOnILockBytes(junk, NULL, STGM_READ | EXCL, NULL, 0,
					    &root), STG_E_FILEALREADYEXISTS);
	CHECK_EQ(StgCreateDocfileOnILockBytes(lb, RW, 0, &root),
		 STG_E_FILEALREADYEXISTS);
	junk->lpVtbl->Release(junk);
	lb->lpVtbl->Release(lb);
}

/* Big enough that the FAT needs more than the header's 109 sectors. */
static void test_storage_big(void)
{
	FN(HRESULT, StgCreateDocfile, LPCOLESTR, DWORD, DWORD, IStorage **);
	FN(HRESULT, StgOpenStorage, LPCOLESTR, IStorage *, DWORD, SNB, DWORD,
	   IStorage **);
	IStorage *root = NULL;
	WCHAR *file = wide("C:\\big.stg");

	CHECK_EQ(StgCreateDocfile(file, RW | STGM_CREATE, 0, &root), S_OK);
	if (root) {
		put_stream(root, "Huge", 8u << 20, 5);
		put_stream(root, "Tiny", 10, 6);
		root->lpVtbl->Release(root);
	}
	CHECK_EQ(StgOpenStorage(file, NULL, STGM_READ | EXCL, NULL, 0, &root),
		 S_OK);
	if (root) {
		CHECK(got_stream(root, "Huge", 8u << 20, 5));
		CHECK(got_stream(root, "Tiny", 10, 6));
		root->lpVtbl->Release(root);
	}
	free(file);
}

static void test_storage_file(void)
{
	FN(HRESULT, StgCreateDocfile, LPCOLESTR, DWORD, DWORD, IStorage **);
	FN(HRESULT, StgOpenStorage, LPCOLESTR, IStorage *, DWORD, SNB, DWORD,
	   IStorage **);
	FN(HRESULT, StgIsStorageFile, LPCOLESTR);
	FN(HRESULT, StgCreateStorageEx, LPCOLESTR, DWORD, DWORD, DWORD, void *,
	   void *, REFIID, void **);
	FN(HRESULT, WriteClassStg, IStorage *, REFCLSID);
	FN(HRESULT, ReadClassStg, IStorage *, CLSID *);
	FN(HRESULT, WriteFmtUserTypeStg, IStorage *, CLIPFORMAT, LPOLESTR);
	FN(HRESULT, ReadFmtUserTypeStg, IStorage *, CLIPFORMAT *, LPOLESTR *);
	FN(HRESULT, SetConvertStg, IStorage *, BOOL);
	FN(HRESULT, GetConvertStg, IStorage *);
	FN(void, CoTaskMemFree, void *);
	IStorage *root = NULL;
	WCHAR *file = wide("C:\\doc.stg"), *plain = wide("C:\\plain.txt");
	WCHAR *missing = wide("C:\\missing.stg");
	char *unix_plain = path_to_unix("C:\\plain.txt");

	CHECK_EQ(StgCreateDocfile(file, STGM_READ | EXCL | STGM_CREATE, 0, &root),
		 STG_E_INVALIDFLAG);
	CHECK_EQ(StgCreateDocfile(file, RW | STGM_CREATE, 0, &root), S_OK);
	if (!root)
		return;
	/* Without STGM_CREATE, an existing file isn't replaced. */
	IStorage *other = NULL;
	CHECK_EQ(StgCreateDocfile(file, RW, 0, &other), STG_E_FILEALREADYEXISTS);
	CHECK_EQ(WriteClassStg(root, &clsid_doc), S_OK);
	WCHAR *type = wide("Test Document");
	CHECK_EQ(WriteFmtUserTypeStg(root, 1 /* CF_TEXT */, type), S_OK);
	free(type);
	CHECK_EQ(GetConvertStg(root), S_FALSE);
	CHECK_EQ(SetConvertStg(root, TRUE), S_OK);
	CHECK_EQ(GetConvertStg(root), S_OK);
	put_stream(root, "Data", 3000, 9);
	put_stream(root, "Old", 10, 4);
	STATSTG st;
	CHECK_EQ(root->lpVtbl->Stat(root, &st, STATFLAG_DEFAULT), S_OK);
	char *n = narrow(st.pwcsName);
	CHECK_STR(n, "C:\\doc.stg");
	free(n);
	CoTaskMemFree(st.pwcsName);
	root->lpVtbl->Release(root);

	FILE *f = fopen(unix_plain, "w");
	if (f) {
		fputs("this is not a compound file, it is text and some more of"
		      " it to pass the length of a header .....................",
		      f);
		for (int i = 0; i < 20; i++)
			fputs("................................", f);
		fclose(f);
	}
	CHECK_EQ(StgIsStorageFile(file), S_OK);
	CHECK_EQ(StgIsStorageFile(plain), S_FALSE);
	CHECK_EQ(StgIsStorageFile(missing), STG_E_FILENOTFOUND);
	CHECK_EQ(StgOpenStorage(plain, NULL, STGM_READ | EXCL, NULL, 0, &root),
		 STG_E_FILEALREADYEXISTS);
	CHECK_EQ(StgOpenStorage(missing, NULL, STGM_READ | EXCL, NULL, 0, &root),
		 STG_E_FILENOTFOUND);

	/* Back again: the class, the user type, then changes. */
	CHECK_EQ(StgOpenStorage(file, NULL, RW, NULL, 0, &root), S_OK);
	if (!root)
		return;
	CLSID c;
	CHECK_EQ(ReadClassStg(root, &c), S_OK);
	CHECK(!memcmp(&c, &clsid_doc, sizeof(c)));
	CLIPFORMAT cf = 0;
	LPOLESTR ut = NULL;
	CHECK_EQ(ReadFmtUserTypeStg(root, &cf, &ut), S_OK);
	CHECK_EQ(cf, 1);
	n = ut ? narrow(ut) : NULL;
	CHECK_STR(n, "Test Document");
	free(n);
	CoTaskMemFree(ut);
	CHECK(got_stream(root, "Data", 3000, 9));
	WCHAR *old = wide("Old"), *renamed = wide("Renamed"), *data = wide("Data");
	CHECK_EQ(root->lpVtbl->RenameElement(root, old, data),
		 STG_E_FILEALREADYEXISTS);
	CHECK_EQ(root->lpVtbl->RenameElement(root, old, renamed), S_OK);
	CHECK_EQ(root->lpVtbl->DestroyElement(root, data), S_OK);
	CHECK_EQ(root->lpVtbl->DestroyElement(root, data), STG_E_FILENOTFOUND);
	root->lpVtbl->Release(root);	/* direct: written on release */
	CHECK_EQ(StgOpenStorage(file, NULL, STGM_READ | EXCL, NULL, 0, &root),
		 S_OK);
	if (root) {
		CHECK(got_stream(root, "Renamed", 10, 4));
		CHECK(!got_stream(root, "Data", 3000, 9));
		CHECK(!got_stream(root, "Old", 10, 4));
		root->lpVtbl->Release(root);
	}

	/* Transacted: nothing is written until Commit; Revert undoes. */
	CHECK_EQ(StgOpenStorage(file, NULL, RW | STGM_TRANSACTED, NULL, 0, &root),
		 S_OK);
	if (root) {
		IStream *s = NULL;
		put_stream(root, "Later", 50, 7);
		CHECK_EQ(root->lpVtbl->OpenStream(root, renamed, NULL, RW, 0, &s),
			 S_OK);
		CHECK_EQ(root->lpVtbl->Revert(root), S_OK);
		CHECK(!got_stream(root, "Later", 50, 7));
		CHECK(got_stream(root, "Renamed", 10, 4));
		if (s) {
			CHECK_EQ(s->lpVtbl->Write(s, "x", 1, NULL), STG_E_REVERTED);
			s->lpVtbl->Release(s);
		}
		put_stream(root, "Later", 50, 7);
		CHECK_EQ(root->lpVtbl->Commit(root, STGC_DEFAULT), S_OK);
		put_stream(root, "Uncommitted", 50, 8);
		root->lpVtbl->Release(root);
	}
	CHECK_EQ(StgOpenStorage(file, NULL, STGM_READ | EXCL, NULL, 0, &root),
		 S_OK);
	if (root) {
		CHECK(got_stream(root, "Later", 50, 7));
		CHECK(!got_stream(root, "Uncommitted", 50, 8));
		root->lpVtbl->Release(root);
	}

	/* STGM_CONVERT keeps what was there in a CONTENTS stream. */
	CHECK_EQ(StgCreateDocfile(plain, RW | STGM_CONVERT, 0, &root),
		 STG_S_CONVERTED);
	if (root) {
		static const WCHAR contents[] = { 3, 'C', 'O', 'N', 'T', 'E', 'N',
						  'T', 'S', 0 };
		IStream *s = NULL;
		CHECK_EQ(root->lpVtbl->OpenStream(root, contents, NULL,
						  STGM_READ | EXCL, 0, &s), S_OK);
		char buf[8] = { 0 };
		if (s) {
			s->lpVtbl->Read(s, buf, 7, NULL);
			s->lpVtbl->Release(s);
		}
		CHECK_STR(buf, "this is");
		root->lpVtbl->Release(root);
	}
	CHECK_EQ(StgIsStorageFile(plain), S_OK);

	IStorage *ex = NULL;
	CHECK_EQ(StgCreateStorageEx(missing, RW | STGM_CREATE, STGFMT_DOCFILE, 0,
				    NULL, NULL, &IID_IStorage, (void **)&ex),
		 S_OK);
	if (ex)
		ex->lpVtbl->Release(ex);
	CHECK_EQ(StgIsStorageFile(missing), S_OK);
	free(old);
	free(renamed);
	free(data);
	free(file);
	free(plain);
	free(missing);
	free(unix_plain);
}

static void test_storage_copy(void)
{
	FN(HRESULT, StgCreateDocfile, LPCOLESTR, DWORD, DWORD, IStorage **);
	FN(HRESULT, CreateStreamOnHGlobal, HGLOBAL, BOOL, LPSTREAM *);
	FN(HRESULT, WriteClassStm, IStream *, REFCLSID);
	FN(HRESULT, ReadClassStm, IStream *, CLSID *);
	FN(HRESULT, OleSaveToStream, IPersistStream *, IStream *);
	IStorage *a = NULL, *b = NULL, *sub = NULL;

	/* NULL names: temporary storages. */
	CHECK_EQ(StgCreateDocfile(NULL, RW | STGM_CREATE | STGM_DELETEONRELEASE,
				  0, &a), S_OK);
	CHECK_EQ(StgCreateDocfile(NULL, RW | STGM_CREATE | STGM_DELETEONRELEASE,
				  0, &b), S_OK);
	if (!a || !b)
		return;
	put_stream(a, "One", 20, 1);
	put_stream(a, "Two", 5000, 2);
	WCHAR *w = wide("Dir");
	CHECK_EQ(a->lpVtbl->CreateStorage(a, w, RW, 0, 0, &sub), S_OK);
	free(w);
	if (sub) {
		put_stream(sub, "Three", 30, 3);
		sub->lpVtbl->Release(sub);
	}
	CHECK_EQ(a->lpVtbl->SetClass(a, &clsid_doc), S_OK);
	WCHAR *two = wide("Two");
	LPOLESTR excl[] = { two, NULL };
	CHECK_EQ(a->lpVtbl->CopyTo(a, 0, NULL, excl, b), S_OK);
	CHECK(got_stream(b, "One", 20, 1));
	CHECK(!got_stream(b, "Two", 5000, 2));
	w = wide("Dir");
	CHECK_EQ(b->lpVtbl->OpenStorage(b, w, NULL, STGM_READ | EXCL, NULL, 0,
					&sub), S_OK);
	free(w);
	if (sub) {
		CHECK(got_stream(sub, "Three", 30, 3));
		sub->lpVtbl->Release(sub);
	}
	STATSTG st;
	CHECK_EQ(b->lpVtbl->Stat(b, &st, STATFLAG_NONAME), S_OK);
	CHECK(!memcmp(&st.clsid, &clsid_doc, sizeof(CLSID)));
	/* Moving takes it away. */
	WCHAR *moved = wide("Moved");
	CHECK_EQ(a->lpVtbl->MoveElementTo(a, two, b, moved, STGMOVE_MOVE), S_OK);
	CHECK(got_stream(b, "Moved", 5000, 2));
	CHECK(!got_stream(a, "Two", 5000, 2));
	free(moved);
	free(two);
	a->lpVtbl->Release(a);
	b->lpVtbl->Release(b);

	/* Class IDs in streams. */
	IStream *s = NULL;
	CLSID c;
	CHECK_EQ(CreateStreamOnHGlobal(NULL, TRUE, &s), S_OK);
	if (!s)
		return;
	CHECK_EQ(WriteClassStm(s, &clsid_doc), S_OK);
	CHECK_EQ(OleSaveToStream(NULL, s), S_OK);
	LARGE_INTEGER zero = { 0 };
	s->lpVtbl->Seek(s, zero, STREAM_SEEK_SET, NULL);
	CHECK_EQ(ReadClassStm(s, &c), S_OK);
	CHECK(!memcmp(&c, &clsid_doc, sizeof(c)));
	CHECK_EQ(ReadClassStm(s, &c), S_OK);
	CHECK(!memcmp(&c, &(CLSID){ 0 }, sizeof(c)));
	CHECK_EQ(ReadClassStm(s, &c), STG_E_READFAULT);
	s->lpVtbl->Release(s);
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
	test_storage_memory();
	test_storage_file();
	test_storage_big();
	test_storage_copy();

	mod_free(k32);
	mod_free(ole);
	char cmd[64];
	snprintf(cmd, sizeof(cmd), "rm -rf '%s'", dir);
	if (system(cmd))
		fprintf(stderr, "could not remove %s\n", dir);
	return test_summary("test_ole32");
}
