/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * oleaut32's type library registration: RegisterTypeLib(ForUser),
 * UnRegisterTypeLib(ForUser) and QueryPathOfRegTypeLib, over seance's
 * registry (reg.h). The keys are the ones Windows writes:
 *
 *   TypeLib\{libid}\{major.minor}       (default) the help string, or name
 *       FLAGS                           (default) the LIBFLAGs, in decimal
 *       HELPDIR                         (default) the help directory
 *       {lcid}\win32 (win64, win16)     (default) the library's path
 *   Interface\{iid}                     (default) the interface's name
 *       ProxyStubClsid, ProxyStubClsid32 (default) the proxy's class
 *       TypeLib                         (default) {libid}, Version maj.min
 *
 * Versions and LCIDs are in hex, as Windows writes them. The machine's
 * classes are HKLM\Software\Classes (HKEY_CLASSES_ROOT here); the user's,
 * for the ForUser functions, HKCU\Software\Classes, which lookups try
 * first. An interface is registered if it is a dispinterface (dual ones
 * included) or an [oleautomation] interface, with the universal
 * marshaller's proxy (PSOAInterface) or, for a plain dispinterface,
 * IDispatch's (PSDispatch). seance runs everything in one process, so the
 * proxies are never used: the keys are there for code that reads them.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#include "common/log.h"
#include "win32/codepage.h"
#include "win32/internal.h"
#include "win32/ole.h"
#include "win32/oleaut.h"
#include "win32/reg.h"

#define MACHINE_ROOT "HKEY_LOCAL_MACHINE\\Software\\Classes"
#define USER_ROOT "HKEY_CURRENT_USER\\Software\\Classes"

static const GUID ps_oainterface = { 0x00020424, 0, 0,
				     { 0xc0, 0, 0, 0, 0, 0, 0, 0x46 } };
static const GUID ps_dispatch = { 0x00020420, 0, 0,
				  { 0xc0, 0, 0, 0, 0, 0, 0, 0x46 } };

static const char *syskind_name(SYSKIND k)
{
	switch (k) {
	case SYS_WIN16:
		return "win16";
	case SYS_WIN32:
		return "win32";
	case SYS_WIN64:
		return "win64";
	default:
		return NULL;
	}
}

static LONG set_wstr(struct reg_key *k, const char *name, const WCHAR *s)
{
	return reg_set(k, name, REG_SZ, s,
		       (DWORD)((wstrlen(s) + 1) * sizeof(WCHAR)));
}

static LONG set_str(struct reg_key *k, const char *name, const char *s)
{
	WCHAR *w = utf8_to_utf16_dup(s);
	LONG err = w ? set_wstr(k, name, w) : ERROR_OUTOFMEMORY;

	free(w);
	return err;
}

/* Creates root\sub and sets its default value; a Win32 error code. */
static LONG put_key(const char *root, const char *sub, const char *name,
		    const char *value)
{
	char path[512];
	struct reg_key *k;
	LONG err;

	snprintf(path, sizeof(path), "%s\\%s", root, sub);
	if (!(k = reg_create(path, 0, NULL, &err)))
		return err;
	return value ? set_str(k, name, value) : ERROR_SUCCESS;
}

/* A string value as UTF-8 (malloc()ed), or NULL. */
static char *get_str(struct reg_key *k, const char *name)
{
	struct reg_value *v = k ? reg_get(k, name) : NULL;
	WCHAR *w;
	char *s;
	size_t n;

	if (!v || (v->type != REG_SZ && v->type != REG_EXPAND_SZ))
		return NULL;
	n = v->size / sizeof(WCHAR);
	if (!(w = malloc((n + 1) * sizeof(WCHAR))))
		return NULL;
	memcpy(w, v->data, n * sizeof(WCHAR));
	w[n] = 0;
	s = utf16_to_utf8_dup(w);
	free(w);
	return s;
}

/* An interface the registry gets a key for. */
struct reg_iface {
	GUID iid;
	char *name;
	const GUID *proxy;
};

/*
 * The interfaces of a library that are registered, read before the
 * registry is locked (reading a type can load an imported library, which
 * reads the registry).
 */
static HRESULT lib_interfaces(ITypeLib *lib, struct reg_iface **ifaces,
			      UINT *count)
{
	UINT n = lib->lpVtbl->GetTypeInfoCount(lib), k = 0;
	struct reg_iface *out = calloc(n ? n : 1, sizeof(*out));
	HRESULT hr = S_OK;

	*ifaces = NULL;
	*count = 0;
	if (!out)
		return E_OUTOFMEMORY;
	for (UINT i = 0; i < n && SUCCEEDED(hr); i++) {
		ITypeInfo *ti;
		TYPEATTR *a;
		BSTR name = NULL;

		if (FAILED(hr = lib->lpVtbl->GetTypeInfo(lib, i, &ti)))
			break;
		if (SUCCEEDED(hr = ti->lpVtbl->GetTypeAttr(ti, &a))) {
			int dispatch = a->typekind == TKIND_DISPATCH;
			int dual = a->wTypeFlags & TYPEFLAG_FDUAL;

			if (dispatch ||
			    (a->typekind == TKIND_INTERFACE &&
			     (a->wTypeFlags & (TYPEFLAG_FOLEAUTOMATION |
					       TYPEFLAG_FDUAL)))) {
				ti->lpVtbl->GetDocumentation(ti, MEMBERID_NIL,
							     &name, NULL, NULL,
							     NULL);
				out[k].iid = a->guid;
				out[k].name = name ? utf16_to_utf8_dup(name)
						   : NULL;
				out[k].proxy = dispatch && !dual
						       ? &ps_dispatch
						       : &ps_oainterface;
				k++;
				oa_SysFreeString(name);
			}
			ti->lpVtbl->ReleaseTypeAttr(ti, a);
		}
		ti->lpVtbl->Release(ti);
	}
	if (FAILED(hr)) {
		/* Not some of them: the registry would be left half done. */
		for (UINT i = 0; i < k; i++)
			free(out[i].name);
		free(out);
		return hr;
	}
	*ifaces = out;
	*count = k;
	return S_OK;
}

static void free_interfaces(struct reg_iface *r, UINT n)
{
	for (UINT i = 0; r && i < n; i++)
		free(r[i].name);
	free(r);
}

/* The directory part of a library's path (dropping "\N" for a resource). */
static char *dir_of(const char *path)
{
	char *d = strdup(path), *slash;

	if (!d)
		return NULL;
	for (int pass = 0; pass < 2; pass++) {
		const char *p;

		if (!(slash = strrchr(d, '\\'))) {
			d[0] = 0;
			return d;
		}
		for (p = slash + 1; *p >= '0' && *p <= '9'; p++)
			;
		*slash = 0;
		/* "file.dll\2": the file's directory, not the file's. */
		if (pass || *p || p == slash + 1)
			break;
	}
	return d;
}

HRESULT oa_register_typelib(ITypeLib *lib, const WCHAR *path,
			    const WCHAR *help_dir, int for_user)
{
	const char *root = for_user ? USER_ROOT : MACHINE_ROOT;
	TLIBATTR *la;
	BSTR name = NULL, doc = NULL;
	char g[OLE_GUID_CHARS], base[256], sub[320], buf[32];
	char *path8 = NULL, *help8 = NULL, *desc = NULL;
	const char *sys;
	struct reg_iface *ifaces = NULL;
	UINT nifaces = 0;
	LONG err;
	HRESULT hr;

	if (!lib || !path)
		return E_INVALIDARG;
	if (FAILED(hr = lib->lpVtbl->GetLibAttr(lib, &la)))
		return hr;
	if (!(sys = syskind_name(la->syskind))) {
		lib->lpVtbl->ReleaseTLibAttr(lib, la);
		return TYPE_E_BADMODULEKIND;
	}
	lib->lpVtbl->GetDocumentation(lib, -1, &name, &doc, NULL, NULL);
	ole_guid_format(&la->guid, g);
	snprintf(base, sizeof(base), "TypeLib\\%s\\%x.%x", g,
		 la->wMajorVerNum, la->wMinorVerNum);
	path8 = utf16_to_utf8_dup(path);
	help8 = help_dir ? utf16_to_utf8_dup(help_dir)
			 : path8 ? dir_of(path8) : NULL;
	desc = doc && *doc ? utf16_to_utf8_dup(doc)
			   : name ? utf16_to_utf8_dup(name) : strdup("");
	if (!path8 || !help8 || !desc) {
		hr = E_OUTOFMEMORY;
		goto out;
	}
	if (FAILED(hr = lib_interfaces(lib, &ifaces, &nifaces)))
		goto out;

	reg_lock();
	err = put_key(root, base, "", desc);
	snprintf(sub, sizeof(sub), "%s\\FLAGS", base);
	snprintf(buf, sizeof(buf), "%u", la->wLibFlags);
	if (!err)
		err = put_key(root, sub, "", buf);
	snprintf(sub, sizeof(sub), "%s\\HELPDIR", base);
	if (!err)
		err = put_key(root, sub, "", help8);
	snprintf(sub, sizeof(sub), "%s\\%lx\\%s", base,
		 (unsigned long)la->lcid, sys);
	if (!err)
		err = put_key(root, sub, "", path8);
	for (UINT i = 0; i < nifaces && !err; i++) {
		char iid[OLE_GUID_CHARS], ps[OLE_GUID_CHARS], key[128];
		char path_ps[160];

		ole_guid_format(&ifaces[i].iid, iid);
		ole_guid_format(ifaces[i].proxy, ps);
		snprintf(key, sizeof(key), "Interface\\%s", iid);
		err = put_key(root, key, "",
			      ifaces[i].name ? ifaces[i].name : "");
		snprintf(path_ps, sizeof(path_ps), "%s\\ProxyStubClsid", key);
		if (!err)
			err = put_key(root, path_ps, "", ps);
		snprintf(path_ps, sizeof(path_ps), "%s\\ProxyStubClsid32",
			 key);
		if (!err)
			err = put_key(root, path_ps, "", ps);
		snprintf(path_ps, sizeof(path_ps), "%s\\TypeLib", key);
		snprintf(buf, sizeof(buf), "%x.%x", la->wMajorVerNum,
			 la->wMinorVerNum);
		if (!err)
			err = put_key(root, path_ps, "", g);
		if (!err)
			err = put_key(root, path_ps, "Version", buf);
	}
	int saving = !err;

	if (!err)
		err = reg_save();
	reg_unlock();
	/* The file wasn't written: back to what it holds. */
	if (err && saving)
		reg_reload();
	TRACE("RegisterTypeLib%s(%s %s, %s): %u interfaces, error %ld",
	      for_user ? "ForUser" : "", g, base + 8 + OLE_GUID_CHARS, path8,
	      nifaces, (long)err);
	hr = err ? TYPE_E_REGISTRYACCESS : S_OK;
out:
	free_interfaces(ifaces, nifaces);
	free(path8);
	free(help8);
	free(desc);
	oa_SysFreeString(name);
	oa_SysFreeString(doc);
	lib->lpVtbl->ReleaseTLibAttr(lib, la);
	return hr;
}

WINFN(HRESULT) oa_RegisterTypeLib(ITypeLib *lib, LPCOLESTR path,
				  LPCOLESTR help)
{
	return oa_register_typelib(lib, path, help, 0);
}

WINFN(HRESULT) oa_RegisterTypeLibForUser(ITypeLib *lib, LPCOLESTR path,
					 LPCOLESTR help)
{
	return oa_register_typelib(lib, path, help, 1);
}

/*
 * The version key under a library's GUID key for major.minor: that one,
 * else the highest minor version above it, or for 0xffff.0xffff the
 * highest of all.
 */
static struct reg_key *find_version(struct reg_key *lib, WORD major,
				    WORD minor)
{
	struct reg_key *best = NULL;
	unsigned long best_major = 0, best_minor = 0;
	int latest = major == 0xffff && minor == 0xffff;
	char exact[16];

	/* The version asked for, as it is written, first. */
	snprintf(exact, sizeof(exact), "%x.%x", major, minor);
	for (size_t i = 0; !latest && i < lib->nsub; i++)
		if (!strcasecmp(lib->sub[i]->name, exact))
			return lib->sub[i];
	for (size_t i = 0; i < lib->nsub; i++) {
		char *end;
		unsigned long maj = strtoul(lib->sub[i]->name, &end, 16), min;

		if (*end != '.')
			continue;
		min = strtoul(end + 1, &end, 16);
		if (*end)
			continue;
		if (latest) {
			if (!best || maj > best_major ||
			    (maj == best_major && min > best_minor))
				goto take;
			continue;
		}
		if (maj != major || min < minor ||
		    (best && min <= best_minor))
			continue;
take:
		best = lib->sub[i];
		best_major = maj;
		best_minor = min;
	}
	return best;
}

static struct reg_key *subkey(struct reg_key *k, const char *name)
{
	for (size_t i = 0; k && i < k->nsub; i++)
		if (!strcasecmp(k->sub[i]->name, name))
			return k->sub[i];
	return NULL;
}

/* This build's syskind first, then the other's. */
static const char *const sys_order[] = {
#if defined(__x86_64__)
	"win64", "win32",
#else
	"win32", "win64",
#endif
};

/* A library's path in one root, or NULL; malloc()ed UTF-8. */
static char *query_path(const char *root, const GUID *guid, WORD major,
			WORD minor, LCID lcid)
{
	char g[OLE_GUID_CHARS], path[256];
	struct reg_key *lib, *ver;
	unsigned long lcids[] = { lcid, lcid & 0x3ff, 0 };

	ole_guid_format(guid, g);
	snprintf(path, sizeof(path), "%s\\TypeLib\\%s", root, g);
	if (!(lib = reg_open(path)) || !(ver = find_version(lib, major, minor)))
		return NULL;
	for (int i = 0; i < 3; i++) {
		char name[16];
		struct reg_key *l;

		snprintf(name, sizeof(name), "%lx", lcids[i]);
		if (!(l = subkey(ver, name)))
			continue;
		for (int j = 0; j < 2; j++) {
			char *s = get_str(subkey(l, sys_order[j]), "");

			if (s)
				return s;
		}
	}
	return NULL;
}

HRESULT STUB_WINAPI oa_QueryPathOfRegTypeLib(REFGUID guid, WORD major,
					     WORD minor, LCID lcid,
					     BSTR *out)
{
	char *path = NULL;

	if (!guid || !out)
		return E_INVALIDARG;
	*out = NULL;
	reg_lock();
	path = query_path(USER_ROOT, guid, major, minor, lcid);
	if (!path)
		path = query_path(MACHINE_ROOT, guid, major, minor, lcid);
	reg_unlock();
	if (!path)
		return TYPE_E_LIBNOTREGISTERED;
	*out = oa_bstr_utf8(path);
	free(path);
	return *out ? S_OK : E_OUTOFMEMORY;
}

/* Deletes root\sub and everything under it, if it is there. */
static void drop_tree(const char *root, const char *sub)
{
	char path[512];
	struct reg_key *k;

	snprintf(path, sizeof(path), "%s\\%s", root, sub);
	if ((k = reg_open(path)))
		reg_delete_tree(k, 0);
}

/* Deletes root\sub if it has no subkeys but these. */
static void drop_if_empty(const char *root, const char *sub,
			  const char *const *except)
{
	char path[512];
	struct reg_key *k;

	snprintf(path, sizeof(path), "%s\\%s", root, sub);
	if (!(k = reg_open(path)))
		return;
	for (size_t i = 0; i < k->nsub; i++) {
		int ok = 0;

		for (const char *const *e = except; e && *e; e++)
			if (!strcasecmp(k->sub[i]->name, *e))
				ok = 1;
		if (!ok)
			return;
	}
	reg_delete_tree(k, 0);
}

static HRESULT unregister(const GUID *guid, WORD major, WORD minor,
			  LCID lcid, SYSKIND kind, int for_user)
{
	static const char *const ver_keep[] = { "FLAGS", "HELPDIR", NULL };
	const char *root = for_user ? USER_ROOT : MACHINE_ROOT;
	const char *sys = syskind_name(kind);
	char g[OLE_GUID_CHARS], ver[320], key[384], vnum[16], *path;
	struct reg_iface *ifaces = NULL;
	UINT nifaces = 0;
	ITypeLib *lib = NULL;

	if (!guid)
		return E_INVALIDARG;
	if (!sys)
		return TYPE_E_BADMODULEKIND;
	ole_guid_format(guid, g);
	snprintf(ver, sizeof(ver), "TypeLib\\%s\\%x.%x", g, major, minor);
	snprintf(vnum, sizeof(vnum), "%x.%x", major, minor);
	snprintf(key, sizeof(key), "%s\\%s\\%lx\\%s", root, ver,
		 (unsigned long)lcid, sys);
	reg_lock();
	path = get_str(reg_open(key), "");
	reg_unlock();
	if (!path) {
		TRACE("UnRegisterTypeLib(%s %x.%x): not registered", g, major,
		      minor);
		return TYPE_E_REGISTRYACCESS;
	}

	/* Its interfaces, from the library itself, if it still loads. */
	WCHAR *w = utf8_to_utf16_dup(path);
	HRESULT hr = S_OK;

	if (w && SUCCEEDED(oa_LoadTypeLibEx(w, REGKIND_NONE, &lib))) {
		hr = lib_interfaces(lib, &ifaces, &nifaces);
		lib->lpVtbl->Release(lib);
	}
	free(w);
	if (FAILED(hr)) {
		free(path);
		return hr;
	}

	reg_lock();
	for (UINT i = 0; i < nifaces; i++) {
		char iid[OLE_GUID_CHARS], sub[160], *owner, *version;
		struct reg_key *tk;

		ole_guid_format(&ifaces[i].iid, iid);
		snprintf(sub, sizeof(sub), "%s\\Interface\\%s\\TypeLib", root,
			 iid);
		tk = reg_open(sub);
		owner = get_str(tk, "");
		version = get_str(tk, "Version");
		/* Only if it is still this version of this library's. */
		if (owner && !strcasecmp(owner, g) &&
		    (!version || !strcasecmp(version, vnum))) {
			snprintf(sub, sizeof(sub), "Interface\\%s", iid);
			drop_tree(root, sub);
		}
		free(owner);
		free(version);
	}
	snprintf(key, sizeof(key), "%s\\%lx\\%s", ver, (unsigned long)lcid,
		 sys);
	drop_tree(root, key);
	snprintf(key, sizeof(key), "%s\\%lx", ver, (unsigned long)lcid);
	drop_if_empty(root, key, NULL);
	drop_if_empty(root, ver, ver_keep);
	snprintf(key, sizeof(key), "TypeLib\\%s", g);
	drop_if_empty(root, key, NULL);
	LONG err = reg_save();
	reg_unlock();
	if (err)
		reg_reload();	/* the file wasn't written: back to it */
	TRACE("UnRegisterTypeLib%s(%s %x.%x): %u interfaces, error %ld",
	      for_user ? "ForUser" : "", g, major, minor, nifaces, (long)err);
	free_interfaces(ifaces, nifaces);
	free(path);
	return err ? TYPE_E_REGISTRYACCESS : S_OK;
}

WINFN(HRESULT) oa_UnRegisterTypeLib(REFGUID guid, WORD major, WORD minor,
				    LCID lcid, SYSKIND kind)
{
	return unregister(guid, major, minor, lcid, kind, 0);
}

WINFN(HRESULT) oa_UnRegisterTypeLibForUser(REFGUID guid, WORD major,
					   WORD minor, LCID lcid,
					   SYSKIND kind)
{
	return unregister(guid, major, minor, lcid, kind, 1);
}

#define E(name, ord) { #name, ord, (void *)oa_##name }

const struct builtin_export oa_typereg_exports[] = {
	E(RegisterTypeLib, 163),
	E(QueryPathOfRegTypeLib, 164),
	E(UnRegisterTypeLib, 186),
	E(RegisterTypeLibForUser, 442),
	E(UnRegisterTypeLibForUser, 443),
	BUILTIN_END
};
