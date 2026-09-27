/*
 * ole32.dll, the part codecs use: apartments (CoInitialize and friends,
 * counted per thread but otherwise without effect), task memory, GUID
 * strings, and in-process class objects: CoCreateInstance finds a class
 * registered with CoRegisterClassObject, or loads the DLL named by
 * HKCR\CLSID\{...}\InprocServer32 in the harness's registry and asks its
 * DllGetClassObject. There are no out-of-process servers, marshalling or
 * OLE documents.
 */
#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/random.h>
#include <unistd.h>

#include "common/error.h"
#include "loader/modules.h"
#include "win32/codepage.h"
#include "win32/internal.h"
#include "win32/k32.h"
#include "win32/path.h"
#include "win32/reg.h"

/* "{xxxxxxxx-xxxx-xxxx-xxxx-xxxxxxxxxxxx}" and its NUL. */
#define GUID_CHARS 39

#define MEMCTX_TASK 1

static const IID iid_iunknown = { 0x00000000, 0x0000, 0x0000,
	{ 0xc0, 0, 0, 0, 0, 0, 0, 0x46 } };
static const IID iid_iclassfactory = { 0x00000001, 0x0000, 0x0000,
	{ 0xc0, 0, 0, 0, 0, 0, 0, 0x46 } };
static const IID iid_imalloc = { 0x00000002, 0x0000, 0x0000,
	{ 0xc0, 0, 0, 0, 0, 0, 0, 0x46 } };

static int guid_eq(const GUID *a, const GUID *b)
{
	return !memcmp(a, b, sizeof(GUID));
}

/* Apartments. */

/* The calling thread's CoInitialize count and its apartment kind. */
static _Thread_local int apt_count;
static _Thread_local int apt_sta;
/* Threads in the multithreaded apartment, which others join implicitly. */
static int mta_threads;
static pthread_mutex_t apt_lock = PTHREAD_MUTEX_INITIALIZER;

static int initialized(void)
{
	pthread_mutex_lock(&apt_lock);
	int ok = apt_count > 0 || mta_threads > 0;
	pthread_mutex_unlock(&apt_lock);
	return ok;
}

WINFN(HRESULT) ole_CoInitializeEx(LPVOID reserved, DWORD flags)
{
	int sta = !!(flags & COINIT_APARTMENTTHREADED);

	if (reserved)
		return E_INVALIDARG;
	if (apt_count && sta != apt_sta)
		return RPC_E_CHANGED_MODE;
	if (apt_count++)
		return S_FALSE;
	apt_sta = sta;
	if (!sta) {
		pthread_mutex_lock(&apt_lock);
		mta_threads++;
		pthread_mutex_unlock(&apt_lock);
	}
	TRACE("CoInitializeEx: thread %lu joins the %s", (unsigned long)
	      thread_current_id(), sta ? "STA" : "MTA");
	return S_OK;
}

WINFN(HRESULT) ole_CoInitialize(LPVOID reserved)
{
	return ole_CoInitializeEx(NULL, COINIT_APARTMENTTHREADED);
}

WINFN(void) ole_CoUninitialize(void)
{
	if (!apt_count || --apt_count)
		return;
	if (!apt_sta) {
		pthread_mutex_lock(&apt_lock);
		mta_threads--;
		pthread_mutex_unlock(&apt_lock);
	}
}

WINFN(HRESULT) ole_OleInitialize(LPVOID reserved)
{
	return ole_CoInitializeEx(NULL, COINIT_APARTMENTTHREADED);
}

WINFN(void) ole_OleUninitialize(void)
{
	ole_CoUninitialize();
}

/* Task memory: the process heap, as on Windows. */

WINFN(LPVOID) ole_CoTaskMemAlloc(SIZE_T size)
{
	return heap_alloc(heap_process(), 0, size);
}

WINFN(LPVOID) ole_CoTaskMemRealloc(LPVOID p, SIZE_T size)
{
	if (!p)
		return heap_alloc(heap_process(), 0, size);
	if (!size) {
		heap_free(heap_process(), 0, p);
		return NULL;
	}
	return heap_realloc(heap_process(), 0, p, size);
}

WINFN(void) ole_CoTaskMemFree(LPVOID p)
{
	heap_free(heap_process(), 0, p);
}

WINFN(HRESULT) malloc_QueryInterface(IMalloc *self, REFIID riid, void **ppv)
{
	if (!ppv)
		return E_POINTER;
	if (riid && (guid_eq(riid, &iid_iunknown) ||
		     guid_eq(riid, &iid_imalloc))) {
		*ppv = self;
		return S_OK;
	}
	*ppv = NULL;
	return E_NOINTERFACE;
}

/* The task allocator is never freed. */
WINFN(ULONG) malloc_AddRef(IMalloc *self) { return 1; }
WINFN(ULONG) malloc_Release(IMalloc *self) { return 1; }

WINFN(void *) malloc_Alloc(IMalloc *self, SIZE_T size)
{
	return ole_CoTaskMemAlloc(size);
}

WINFN(void *) malloc_Realloc(IMalloc *self, void *p, SIZE_T size)
{
	return ole_CoTaskMemRealloc(p, size);
}

WINFN(void) malloc_Free(IMalloc *self, void *p)
{
	ole_CoTaskMemFree(p);
}

/* A task-memory block's size; -1 for NULL or a foreign pointer. */
WINFN(SIZE_T) malloc_GetSize(IMalloc *self, void *p)
{
	return p ? heap_size(heap_process(), 0, p) : (SIZE_T)-1;
}

WINFN(int) malloc_DidAlloc(IMalloc *self, void *p)
{
	if (!p)
		return -1;
	return heap_size(heap_process(), 0, p) != (SIZE_T)-1;
}

WINFN(void) malloc_HeapMinimize(IMalloc *self) { }

static const IMallocVtbl malloc_vtbl = {
	malloc_QueryInterface, malloc_AddRef, malloc_Release, malloc_Alloc,
	malloc_Realloc, malloc_Free, malloc_GetSize, malloc_DidAlloc,
	malloc_HeapMinimize,
};
static IMalloc task_malloc = { &malloc_vtbl };

WINFN(HRESULT) ole_CoGetMalloc(DWORD context, LPMALLOC *out)
{
	if (!out)
		return E_INVALIDARG;
	if (context != MEMCTX_TASK) {
		*out = NULL;
		return E_INVALIDARG;
	}
	*out = &task_malloc;
	return S_OK;
}

/* GUIDs and their strings. */

static void guid_format(const GUID *g, char out[GUID_CHARS])
{
	snprintf(out, GUID_CHARS,
		 "{%08X-%04X-%04X-%02X%02X-%02X%02X%02X%02X%02X%02X}",
		 (unsigned)g->Data1, g->Data2, g->Data3, g->Data4[0],
		 g->Data4[1], g->Data4[2], g->Data4[3], g->Data4[4],
		 g->Data4[5], g->Data4[6], g->Data4[7]);
}

static int hex_digit(WCHAR c)
{
	if (c >= '0' && c <= '9')
		return c - '0';
	if (c >= 'a' && c <= 'f')
		return c - 'a' + 10;
	if (c >= 'A' && c <= 'F')
		return c - 'A' + 10;
	return -1;
}

/* Parses exactly "{xxxxxxxx-xxxx-xxxx-xxxx-xxxxxxxxxxxx}". */
static int guid_parse(const WCHAR *s, GUID *out)
{
	static const char shape[] = "{xxxxxxxx-xxxx-xxxx-xxxx-xxxxxxxxxxxx}";
	BYTE b[16];
	int n = 0;

	for (int i = 0; shape[i]; i++) {
		if (shape[i] != 'x') {
			if (s[i] != (WCHAR)shape[i])
				return -1;
			continue;
		}
		int hi = hex_digit(s[i]), lo = hi < 0 ? -1 : hex_digit(s[i + 1]);
		if (lo < 0)
			return -1;
		b[n++] = (BYTE)(hi << 4 | lo);
		i++;
	}
	if (s[sizeof(shape) - 1])
		return -1;
	out->Data1 = (DWORD)b[0] << 24 | (DWORD)b[1] << 16 | (DWORD)b[2] << 8 |
		     b[3];
	out->Data2 = (WORD)(b[4] << 8 | b[5]);
	out->Data3 = (WORD)(b[6] << 8 | b[7]);
	memcpy(out->Data4, b + 8, 8);
	return 0;
}

WINFN(INT) ole_StringFromGUID2(REFGUID g, LPOLESTR buf, INT cch)
{
	char s[GUID_CHARS];

	if (!g || !buf || cch < GUID_CHARS)
		return 0;
	guid_format(g, s);
	for (int i = 0; i < GUID_CHARS; i++)
		buf[i] = (BYTE)s[i];
	return GUID_CHARS;
}

static HRESULT guid_string(REFGUID g, LPOLESTR *out)
{
	if (!out)
		return E_INVALIDARG;
	if (!g) {
		*out = NULL;
		return E_INVALIDARG;
	}
	if (!(*out = ole_CoTaskMemAlloc(GUID_CHARS * sizeof(WCHAR))))
		return E_OUTOFMEMORY;
	ole_StringFromGUID2(g, *out, GUID_CHARS);
	return S_OK;
}

WINFN(HRESULT) ole_StringFromCLSID(REFCLSID clsid, LPOLESTR *out)
{
	return guid_string(clsid, out);
}

WINFN(HRESULT) ole_StringFromIID(REFIID iid, LPOLESTR *out)
{
	return guid_string(iid, out);
}

WINFN(BOOL) ole_IsEqualGUID(REFGUID a, REFGUID b)
{
	return guid_eq(a, b);
}

/* A random (version 4) GUID. */
WINFN(HRESULT) ole_CoCreateGuid(GUID *out)
{
	BYTE b[16];
	size_t got = 0;

	if (!out)
		return E_INVALIDARG;
	while (got < sizeof(b)) {
		ssize_t n = getrandom(b + got, sizeof(b) - got, 0);
		if (n < 0 && errno != EINTR)
			return E_FAIL;
		if (n > 0)
			got += (size_t)n;
	}
	memcpy(out, b, sizeof(*out));
	out->Data3 = (WORD)((out->Data3 & 0x0fff) | 0x4000);
	out->Data4[0] = (BYTE)((out->Data4[0] & 0x3f) | 0x80);
	return S_OK;
}

/*
 * A string value under HKEY_CLASSES_ROOT (sub is e.g. "CLSID\{...}\ProgID",
 * the value "" for the default), as UTF-8, malloc()ed; the per-user
 * classes win over the machine's, as on Windows. NULL if there is none.
 */
static char *classes_value(const char *sub, const char *name)
{
	static const char *const roots[] = {
		"HKEY_CURRENT_USER\\Software\\Classes",
		"HKEY_LOCAL_MACHINE\\Software\\Classes",
	};
	char *out = NULL;

	reg_lock();
	for (size_t i = 0; i < sizeof(roots) / sizeof(*roots) && !out; i++) {
		char path[512];
		snprintf(path, sizeof(path), "%s\\%s", roots[i], sub);
		struct reg_key *k = reg_open(path);
		struct reg_value *v = k ? reg_get(k, name) : NULL;
		if (!v || (v->type != REG_SZ && v->type != REG_EXPAND_SZ))
			continue;
		size_t n = v->size / sizeof(WCHAR);
		WCHAR *w = malloc((n + 1) * sizeof(WCHAR));
		if (!w)
			break;
		memcpy(w, v->data, n * sizeof(WCHAR));
		w[n] = 0;
		out = utf16_to_utf8_dup(w);
		free(w);
	}
	reg_unlock();
	return out;
}

WINFN(HRESULT) ole_CLSIDFromProgID(LPCOLESTR progid, LPCLSID clsid)
{
	if (!progid || !clsid)
		return E_INVALIDARG;

	char *name = utf16_to_utf8_dup(progid), sub[300];
	snprintf(sub, sizeof(sub), "%s\\CLSID", name ? name : "");
	free(name);
	char *value = classes_value(sub, "");
	WCHAR *w = utf8_to_utf16_dup(value);
	free(value);
	int bad = !w || guid_parse(w, clsid);
	free(w);
	if (bad) {
		memset(clsid, 0, sizeof(*clsid));
		return CO_E_CLASSSTRING;
	}
	return S_OK;
}

WINFN(HRESULT) ole_CLSIDFromString(LPCOLESTR s, LPCLSID clsid)
{
	if (!clsid)
		return E_INVALIDARG;
	if (!s || !*s) {
		memset(clsid, 0, sizeof(*clsid));
		return S_OK;
	}
	if (!guid_parse(s, clsid))
		return S_OK;
	if (*s != '{' && ole_CLSIDFromProgID(s, clsid) == S_OK)
		return S_OK;
	memset(clsid, 0, sizeof(*clsid));
	return CO_E_CLASSSTRING;
}

WINFN(HRESULT) ole_IIDFromString(LPCOLESTR s, LPIID iid)
{
	if (!iid)
		return E_INVALIDARG;
	if (!s) {
		memset(iid, 0, sizeof(*iid));
		return S_OK;
	}
	if (guid_parse(s, iid)) {
		memset(iid, 0, sizeof(*iid));
		return E_INVALIDARG;
	}
	return S_OK;
}

WINFN(HRESULT) ole_ProgIDFromCLSID(REFCLSID clsid, LPOLESTR *out)
{
	char g[GUID_CHARS], sub[64];

	if (!out)
		return E_INVALIDARG;
	*out = NULL;
	if (!clsid)
		return E_INVALIDARG;
	guid_format(clsid, g);
	snprintf(sub, sizeof(sub), "CLSID\\%s\\ProgID", g);
	char *value = classes_value(sub, "");
	if (!value)
		return REGDB_E_CLASSNOTREG;
	WCHAR *w = utf8_to_utf16_dup(value);
	free(value);
	size_t n = w ? wstrlen(w) + 1 : 0;
	if (w && (*out = ole_CoTaskMemAlloc(n * sizeof(WCHAR))))
		memcpy(*out, w, n * sizeof(WCHAR));
	free(w);
	return *out ? S_OK : E_OUTOFMEMORY;
}

/* Class objects. */

struct class_reg {
	struct class_reg *next;
	DWORD cookie;
	CLSID clsid;
	DWORD context;
	IUnknown *obj;
	int suspended;		/* REGCLS_SUSPENDED, until CoResumeClassObjects */
	int single_use;		/* REGCLS_SINGLEUSE: hidden once handed out */
	int used;
};
static struct class_reg *classes;
static DWORD next_cookie = 1;
/* In-process servers loaded for class objects, kept until exit. */
static struct module **servers;
static size_t nservers;
static pthread_mutex_t class_lock = PTHREAD_MUTEX_INITIALIZER;

WINFN(HRESULT) ole_CoRegisterClassObject(REFCLSID clsid, LPUNKNOWN obj,
					 DWORD context, DWORD flags,
					 LPDWORD cookie)
{
	struct class_reg *r;

	if (!clsid || !obj || !cookie)
		return E_INVALIDARG;
	*cookie = 0;
	if (!initialized())
		return CO_E_NOTINITIALIZED;
	if (!(r = calloc(1, sizeof(*r))))
		return E_OUTOFMEMORY;
	r->clsid = *clsid;
	r->context = context;
	/* A multiple-use local server serves this process in-process too. */
	if ((flags & REGCLS_MULTIPLEUSE) && (context & CLSCTX_LOCAL_SERVER))
		r->context |= CLSCTX_INPROC_SERVER;
	r->suspended = !!(flags & REGCLS_SUSPENDED);
	r->single_use = !(flags & (REGCLS_MULTIPLEUSE | REGCLS_MULTI_SEPARATE));
	r->obj = obj;
	obj->lpVtbl->AddRef(obj);

	pthread_mutex_lock(&class_lock);
	r->cookie = next_cookie++;
	r->next = classes;
	classes = r;
	*cookie = r->cookie;
	pthread_mutex_unlock(&class_lock);
	return S_OK;
}

WINFN(HRESULT) ole_CoRevokeClassObject(DWORD cookie)
{
	struct class_reg *r = NULL;

	pthread_mutex_lock(&class_lock);
	for (struct class_reg **p = &classes; *p; p = &(*p)->next) {
		if ((*p)->cookie == cookie) {
			r = *p;
			*p = r->next;
			break;
		}
	}
	pthread_mutex_unlock(&class_lock);
	if (!r)
		return E_INVALIDARG;
	r->obj->lpVtbl->Release(r->obj);
	free(r);
	return S_OK;
}

WINFN(HRESULT) ole_CoResumeClassObjects(void)
{
	pthread_mutex_lock(&class_lock);
	for (struct class_reg *r = classes; r; r = r->next)
		r->suspended = 0;
	pthread_mutex_unlock(&class_lock);
	return S_OK;
}

/*
 * A registered class object for clsid in context, with a reference.
 * Suspended registrations aren't visible yet, and a single-use one only
 * until it has been handed out once.
 */
static IUnknown *registered(REFCLSID clsid, DWORD context)
{
	IUnknown *obj = NULL;

	pthread_mutex_lock(&class_lock);
	for (struct class_reg *r = classes; r && !obj; r = r->next) {
		if (!(r->context & context) || !guid_eq(&r->clsid, clsid) ||
		    r->suspended || (r->single_use && r->used))
			continue;
		obj = r->obj;
		r->used = 1;
	}
	if (obj)
		obj->lpVtbl->AddRef(obj);
	pthread_mutex_unlock(&class_lock);
	return obj;
}

/*
 * Loads an InprocServer32 DLL (UTF-8) as LoadLibrary would, and keeps it
 * loaded: a Windows path maps to Unix, except that a system DLL named by
 * path is still the built-in one.
 */
static struct module *load_server(const char *name)
{
	const char *base = name;
	struct module *m;

	for (const char *p = name; *p; p++)
		if (*p == '\\' || *p == '/')
			base = p + 1;
	if (base != name && !builtin_find(base)) {
		char *u = path_to_unix(name);
		m = u ? mod_load(u, NULL) : NULL;
		free(u);
	} else {
		m = mod_load(base, NULL);
	}
	if (!m)
		return NULL;

	pthread_mutex_lock(&class_lock);
	int known = 0;
	for (size_t i = 0; i < nservers && !known; i++)
		known = servers[i] == m;
	if (!known) {
		struct module **s = realloc(servers,
					    (nservers + 1) * sizeof(*s));
		if (s) {
			servers = s;
			servers[nservers++] = m;
			known = -1;	/* keeps the reference */
		}
	}
	pthread_mutex_unlock(&class_lock);
	if (known > 0)
		mod_free(m);	/* already held once */
	return m;
}

static HRESULT inproc_class_object(REFCLSID clsid, REFIID riid, void **ppv)
{
	char g[GUID_CHARS], sub[64];

	guid_format(clsid, g);
	snprintf(sub, sizeof(sub), "CLSID\\%s\\InprocServer32", g);
	char *dll = classes_value(sub, "");
	if (!dll || !*dll) {
		free(dll);
		return REGDB_E_CLASSNOTREG;
	}

	struct module *m = load_server(dll);
	if (!m) {
		TRACE("CoGetClassObject(%s): %s: %s", g, dll, vfw_error());
		free(dll);
		return CO_E_DLLNOTFOUND;
	}
	LPFNGETCLASSOBJECT get = (LPFNGETCLASSOBJECT)mod_get_proc(
		m, "DllGetClassObject");
	if (!get) {
		TRACE("CoGetClassObject(%s): %s has no DllGetClassObject", g,
		      dll);
		free(dll);
		return CO_E_ERRORINDLL;
	}
	HRESULT hr = get(clsid, riid, ppv);
	TRACE("CoGetClassObject(%s): %s DllGetClassObject = 0x%08lx", g, dll,
	      (unsigned long)hr);
	free(dll);
	return hr;
}

WINFN(HRESULT) ole_CoGetClassObject(REFCLSID clsid, DWORD context,
				    LPVOID server_info, REFIID riid,
				    LPVOID *ppv)
{
	if (!ppv)
		return E_INVALIDARG;
	*ppv = NULL;
	if (!clsid || !riid)
		return E_INVALIDARG;
	if (!initialized())
		return CO_E_NOTINITIALIZED;

	IUnknown *obj = registered(clsid, context);
	if (obj) {
		HRESULT hr = obj->lpVtbl->QueryInterface(obj, riid, ppv);
		obj->lpVtbl->Release(obj);
		return hr;
	}
	if (context & CLSCTX_INPROC)
		return inproc_class_object(clsid, riid, ppv);
	return REGDB_E_CLASSNOTREG;
}

WINFN(HRESULT) ole_CoCreateInstance(REFCLSID clsid, LPUNKNOWN outer,
				    DWORD context, REFIID riid, LPVOID *ppv)
{
	IClassFactory *cf;

	if (!ppv)
		return E_POINTER;
	*ppv = NULL;
	if (!riid)
		return E_INVALIDARG;
	HRESULT hr = ole_CoGetClassObject(clsid, context, NULL,
					  &iid_iclassfactory, (void **)&cf);
	if (FAILED(hr))
		return hr;
	hr = cf->lpVtbl->CreateInstance(cf, outer, riid, ppv);
	cf->lpVtbl->Release(cf);
	return hr;
}

/* In-process servers stay loaded until exit. */
WINFN(void) ole_CoFreeUnusedLibraries(void) { }
WINFN(void) ole_CoFreeUnusedLibrariesEx(DWORD delay, DWORD reserved) { }

static const struct builtin_export exports[] = {
	{ "CLSIDFromProgID", 0, ole_CLSIDFromProgID },
	{ "CLSIDFromString", 0, ole_CLSIDFromString },
	{ "CoCreateGuid", 0, ole_CoCreateGuid },
	{ "CoCreateInstance", 0, ole_CoCreateInstance },
	{ "CoFreeUnusedLibraries", 0, ole_CoFreeUnusedLibraries },
	{ "CoFreeUnusedLibrariesEx", 0, ole_CoFreeUnusedLibrariesEx },
	{ "CoGetClassObject", 0, ole_CoGetClassObject },
	{ "CoGetMalloc", 0, ole_CoGetMalloc },
	{ "CoInitialize", 0, ole_CoInitialize },
	{ "CoInitializeEx", 0, ole_CoInitializeEx },
	{ "CoRegisterClassObject", 0, ole_CoRegisterClassObject },
	{ "CoResumeClassObjects", 0, ole_CoResumeClassObjects },
	{ "CoRevokeClassObject", 0, ole_CoRevokeClassObject },
	{ "CoTaskMemAlloc", 0, ole_CoTaskMemAlloc },
	{ "CoTaskMemFree", 0, ole_CoTaskMemFree },
	{ "CoTaskMemRealloc", 0, ole_CoTaskMemRealloc },
	{ "CoUninitialize", 0, ole_CoUninitialize },
	{ "IIDFromString", 0, ole_IIDFromString },
	{ "IsEqualGUID", 0, ole_IsEqualGUID },
	{ "OleInitialize", 0, ole_OleInitialize },
	{ "OleUninitialize", 0, ole_OleUninitialize },
	{ "ProgIDFromCLSID", 0, ole_ProgIDFromCLSID },
	{ "StringFromCLSID", 0, ole_StringFromCLSID },
	{ "StringFromGUID2", 0, ole_StringFromGUID2 },
	{ "StringFromIID", 0, ole_StringFromIID },
	BUILTIN_END
};

static const struct builtin_export *const tables[] = { exports, NULL };

const struct builtin_dll builtin_ole32 = { "ole32.dll", NULL, tables, 0 };
