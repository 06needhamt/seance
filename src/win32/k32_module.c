/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * kernel32: LoadLibrary and friends, module handles and names, resources.
 */
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#include "common/error.h"
#include "loader/modules.h"
#include "runtime/builtin.h"
#include "win32/codepage.h"
#include "win32/internal.h"
#include "win32/k32.h"
#include "win32/path.h"

#define SYSTEM_DIR "C:\\WINDOWS\\system32\\"

/*
 * Loads name (UTF-8) for a caller at ret. A Windows path is mapped to Unix,
 * except that system DLLs named by path still come from the built-ins. A
 * bare name is searched for first next to the calling module, where codecs
 * expect their helper DLLs to be.
 */
static HMODULE load(const char *name, const void *ret)
{
	struct module *m = NULL;

	if (!name || !*name) {
		win_set_error(ERROR_INVALID_PARAMETER);
		return NULL;
	}
	const char *base = name;
	for (const char *p = name; *p; p++)
		if (*p == '\\' || *p == '/')
			base = p + 1;

	char want[256];
	snprintf(want, sizeof(want), "%s%s", base, strchr(base, '.') ? "" : ".dll");
	if (base != name && !builtin_find(want)) {
		char *u = path_to_unix(name);
		if (u)
			m = mod_load(u, NULL);
		free(u);
	} else {
		m = mod_load(want, win_caller_module(ret));
	}

	if (!m) {
		TRACE("LoadLibrary(%s) failed: %s", name, vfw_error());
		win_set_error(ERROR_MOD_NOT_FOUND);
		return NULL;
	}
	TRACE("LoadLibrary(%s) = %p", name, (void *)m->base);
	return (HMODULE)m->base;
}

WINFN(HMODULE) k32_LoadLibraryA(LPCSTR name)
{
	char *u = acp_to_utf8(name);
	HMODULE h = load(u, __builtin_return_address(0));
	free(u);
	return h;
}

WINFN(HMODULE) k32_LoadLibraryW(LPCWSTR name)
{
	char *u = utf16_to_utf8_dup(name);
	HMODULE h = load(u, __builtin_return_address(0));
	free(u);
	return h;
}

/* LOAD_LIBRARY_AS_DATAFILE and DONT_RESOLVE_DLL_REFERENCES load normally. */
WINFN(HMODULE) k32_LoadLibraryExA(LPCSTR name, HANDLE file, DWORD flags)
{
	char *u = acp_to_utf8(name);
	HMODULE h = load(u, __builtin_return_address(0));
	free(u);
	return h;
}

WINFN(HMODULE) k32_LoadLibraryExW(LPCWSTR name, HANDLE file, DWORD flags)
{
	char *u = utf16_to_utf8_dup(name);
	HMODULE h = load(u, __builtin_return_address(0));
	free(u);
	return h;
}

static struct module *module_of(HMODULE h)
{
	struct module *m = h ? mod_from_base(h) : mod_main();

	if (!m)
		win_set_error(ERROR_MOD_NOT_FOUND);
	return m;
}

WINFN(BOOL) k32_FreeLibrary(HMODULE h)
{
	struct module *m = h ? mod_from_base(h) : NULL;

	if (!m) {
		win_set_error(ERROR_MOD_NOT_FOUND);
		return FALSE;
	}
	mod_free(m);
	return TRUE;
}

WINFN(void) k32_FreeLibraryAndExitThread(HMODULE h, DWORD code)
{
	struct module *m = h ? mod_from_base(h) : NULL;

	if (m)
		mod_free(m);
	thread_exit(code);
}

WINFN(FARPROC) k32_GetProcAddress(HMODULE h, LPCSTR name)
{
	struct module *m = module_of(h);
	void *p;

	if (!m)
		return NULL;
	if ((uintptr_t)name <= 0xffff)
		p = mod_get_ordinal(m, (WORD)(uintptr_t)name);
	else
		p = mod_get_proc(m, name);
	if (!p) {
		if ((uintptr_t)name <= 0xffff)
			TRACE("GetProcAddress(%s, #%u) = NULL", m->name,
			      (unsigned)(uintptr_t)name);
		else
			TRACE("GetProcAddress(%s, %s) = NULL", m->name, name);
		win_set_error(ERROR_PROC_NOT_FOUND);
	}
	return (FARPROC)p;
}

/* The loaded module a GetModuleHandle name refers to (a path works too). */
static struct module *find_by_name(const char *name)
{
	const char *base = name;

	for (const char *p = name; *p; p++)
		if (*p == '\\' || *p == '/')
			base = p + 1;
	if (!strcasecmp(base, "seance.exe") || !strcasecmp(base, "seance"))
		return mod_main();
	return mod_find(base);
}

static HMODULE module_handle(const char *name)
{
	struct module *m = name ? find_by_name(name) : mod_main();

	if (!m) {
		win_set_error(ERROR_MOD_NOT_FOUND);
		return NULL;
	}
	return (HMODULE)m->base;
}

WINFN(HMODULE) k32_GetModuleHandleA(LPCSTR name)
{
	char *u = acp_to_utf8(name);
	HMODULE h = (name && !u) ? NULL : module_handle(u);
	free(u);
	return h;
}

WINFN(HMODULE) k32_GetModuleHandleW(LPCWSTR name)
{
	char *u = utf16_to_utf8_dup(name);
	HMODULE h = (name && !u) ? NULL : module_handle(u);
	free(u);
	return h;
}

static BOOL module_handle_ex(DWORD flags, const void *name_or_addr, int wide,
			     HMODULE *out)
{
	struct module *m;

	if (!out) {
		win_set_error(ERROR_INVALID_PARAMETER);
		return FALSE;
	}
	*out = NULL;
	if (flags & GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS) {
		m = mod_from_address(name_or_addr);
	} else if (!name_or_addr) {
		m = mod_main();
	} else {
		char *u = wide ? utf16_to_utf8_dup(name_or_addr) :
			  acp_to_utf8(name_or_addr);
		m = u ? find_by_name(u) : NULL;
		free(u);
	}
	if (!m) {
		win_set_error(ERROR_MOD_NOT_FOUND);
		return FALSE;
	}
	if (flags & GET_MODULE_HANDLE_EX_FLAG_PIN)
		mod_pin(m);
	else if (!(flags & GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT))
		mod_addref(m);
	*out = (HMODULE)m->base;
	return TRUE;
}

WINFN(BOOL) k32_GetModuleHandleExA(DWORD flags, LPCSTR name, HMODULE *out)
{
	return module_handle_ex(flags, name, 0, out);
}

WINFN(BOOL) k32_GetModuleHandleExW(DWORD flags, LPCWSTR name, HMODULE *out)
{
	return module_handle_ex(flags, name, 1, out);
}

/* The Windows path of a module (malloc()ed, UTF-8). */
static char *module_path(struct module *m)
{
	if (!m->path) {
		char *p = malloc(sizeof(SYSTEM_DIR) + strlen(m->name));
		if (p)
			sprintf(p, "%s%s", SYSTEM_DIR, m->name);
		return p;
	}
	char abs[PATH_MAX];
	if (!realpath(m->path, abs))
		snprintf(abs, sizeof(abs), "%s", m->path);
	return path_to_windows(abs);
}

/* GetModuleFileName: a too-small buffer gets a truncated, terminated copy. */
static DWORD module_file_name(HMODULE h, void *buf, DWORD size, int wide)
{
	struct module *m = module_of(h);
	char *path;
	DWORD r;

	if (!m || !(path = module_path(m)))
		return 0;
	r = wide ? copy_out_w(path, buf, size) : copy_out_a(path, buf, size);
	if (r >= size) {
		if (size) {
			WCHAR *w = utf8_to_utf16_dup(path);
			if (wide && w) {
				memcpy(buf, w, (size - 1) * sizeof(WCHAR));
				((WCHAR *)buf)[size - 1] = 0;
			} else if (!wide) {
				char *a = utf8_to_acp_dup(path);
				if (a) {
					memcpy(buf, a, size - 1);
					((char *)buf)[size - 1] = '\0';
				}
				free(a);
			}
			free(w);
		}
		win_set_error(ERROR_INSUFFICIENT_BUFFER);
		r = size;
	}
	free(path);
	return r;
}

WINFN(DWORD) k32_GetModuleFileNameA(HMODULE h, LPSTR buf, DWORD size)
{
	return module_file_name(h, buf, size, 0);
}

WINFN(DWORD) k32_GetModuleFileNameW(HMODULE h, LPWSTR buf, DWORD size)
{
	return module_file_name(h, buf, size, 1);
}

WINFN(BOOL) k32_DisableThreadLibraryCalls(HMODULE h)
{
	struct module *m = h ? mod_from_base(h) : NULL;

	if (!m) {
		win_set_error(ERROR_INVALID_HANDLE);
		return FALSE;
	}
	m->no_thread_calls = 1;
	return TRUE;
}

/* Resources. HRSRC is the IMAGE_RESOURCE_DATA_ENTRY, as on Windows. */

/*
 * A resource name or type argument as pe_find_resource wants it: ids pass
 * through, "#123" means id 123, other strings are copied (ASCII) into buf.
 */
static pe_resid res_arg(const void *arg, int wide, char *buf, size_t size)
{
	if ((uintptr_t)arg <= 0xffff)
		return (pe_resid)arg;
	char *u = wide ? utf16_to_utf8_dup(arg) : acp_to_utf8(arg);
	if (!u)
		return 0;
	snprintf(buf, size, "%s", u);
	free(u);
	if (buf[0] == '#')
		return strtoul(buf + 1, NULL, 10);
	return (pe_resid)buf;
}

static HRSRC find_resource(HMODULE h, const void *type, const void *name,
			   WORD lang, int wide, int any_lang)
{
	struct module *m = module_of(h);
	char tbuf[256], nbuf[256];
	struct pe_resource res;

	if (!m)
		return NULL;
	pe_resid t = res_arg(type, wide, tbuf, sizeof(tbuf));
	pe_resid n = res_arg(name, wide, nbuf, sizeof(nbuf));
	if (!m->img || !t || !n ||
	    pe_find_resource(m->img, t, n, any_lang ? PE_RES_ANY_LANG : lang, &res)) {
		win_set_error(ERROR_RESOURCE_NAME_NOT_FOUND);
		return NULL;
	}
	return (HRSRC)(m->base + res.entry_rva);
}

WINFN(HRSRC) k32_FindResourceA(HMODULE h, LPCSTR name, LPCSTR type)
{
	return find_resource(h, type, name, 0, 0, 1);
}

WINFN(HRSRC) k32_FindResourceW(HMODULE h, LPCWSTR name, LPCWSTR type)
{
	return find_resource(h, type, name, 0, 1, 1);
}

WINFN(HRSRC) k32_FindResourceExA(HMODULE h, LPCSTR type, LPCSTR name, WORD lang)
{
	return find_resource(h, type, name, lang, 0, 0);
}

WINFN(HRSRC) k32_FindResourceExW(HMODULE h, LPCWSTR type, LPCWSTR name, WORD lang)
{
	return find_resource(h, type, name, lang, 1, 0);
}

/* The data entry behind hrsrc, if it lies inside module h's image. */
static const IMAGE_RESOURCE_DATA_ENTRY *resource_entry(HMODULE h, HRSRC hrsrc,
						       struct module **mod)
{
	struct module *m = module_of(h);

	if (!m || !m->img || !hrsrc ||
	    (BYTE *)hrsrc < m->base ||
	    (BYTE *)hrsrc + sizeof(IMAGE_RESOURCE_DATA_ENTRY) > m->base + m->img->map_len) {
		win_set_error(ERROR_RESOURCE_DATA_NOT_FOUND);
		return NULL;
	}
	*mod = m;
	return (const IMAGE_RESOURCE_DATA_ENTRY *)hrsrc;
}

WINFN(HGLOBAL) k32_LoadResource(HMODULE h, HRSRC hrsrc)
{
	struct module *m;
	const IMAGE_RESOURCE_DATA_ENTRY *e = resource_entry(h, hrsrc, &m);

	if (!e)
		return NULL;
	const void *data = pe_ptr(m->img, e->OffsetToData, e->Size);
	if (!data)
		win_set_error(ERROR_RESOURCE_DATA_NOT_FOUND);
	return (HGLOBAL)data;
}

WINFN(LPVOID) k32_LockResource(HGLOBAL h) { return h; }
WINFN(BOOL) k32_FreeResource(HGLOBAL h) { return FALSE; }

WINFN(DWORD) k32_SizeofResource(HMODULE h, HRSRC hrsrc)
{
	struct module *m;
	const IMAGE_RESOURCE_DATA_ENTRY *e = resource_entry(h, hrsrc, &m);
	return e ? e->Size : 0;
}

#define E(name) { #name, 0, (void *)k32_##name }

const struct builtin_export k32_module_exports[] = {
	E(DisableThreadLibraryCalls),
	E(FindResourceA),
	E(FindResourceExA),
	E(FindResourceExW),
	E(FindResourceW),
	E(FreeLibrary),
	E(FreeLibraryAndExitThread),
	E(FreeResource),
	E(GetModuleFileNameA),
	E(GetModuleFileNameW),
	E(GetModuleHandleA),
	E(GetModuleHandleExA),
	E(GetModuleHandleExW),
	E(GetModuleHandleW),
	E(GetProcAddress),
	E(LoadLibraryA),
	E(LoadLibraryExA),
	E(LoadLibraryExW),
	E(LoadLibraryW),
	E(LoadResource),
	E(LockResource),
	E(SizeofResource),
	BUILTIN_END
};
