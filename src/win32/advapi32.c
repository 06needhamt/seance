/*
 * advapi32.dll: the registry functions (on src/win32/reg.c) and
 * GetUserName. Reg* return their Win32 error instead of setting the
 * last error, as on Windows.
 *
 * An open HKEY is a kernel-object handle holding the key's path, so a key
 * deleted while open reads as ERROR_KEY_DELETED rather than dangling.
 * HKEY_CLASSES_ROOT is HKEY_LOCAL_MACHINE\Software\Classes; there is no
 * per-user merge.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "win32/codepage.h"
#include "win32/handle.h"
#include "win32/internal.h"
#include "win32/reg.h"

struct hkey_obj {
	struct kobject obj;
	char *path;
};

static const struct {
	HKEY key;
	const char *path;
} predefined[] = {
	{ HKEY_CLASSES_ROOT, "HKEY_LOCAL_MACHINE\\Software\\Classes" },
	{ HKEY_CURRENT_USER, "HKEY_CURRENT_USER" },
	{ HKEY_LOCAL_MACHINE, "HKEY_LOCAL_MACHINE" },
	{ HKEY_USERS, "HKEY_USERS" },
	{ HKEY_CURRENT_CONFIG, "HKEY_CURRENT_CONFIG" },
};

static int is_predefined(HKEY h)
{
	return (ULONG_PTR)h >= 0x80000000 && (ULONG_PTR)h <= 0x80000006;
}

/* The path of an open key (malloc()ed), or NULL with *err. */
static char *key_path(HKEY h, LONG *err)
{
	for (size_t i = 0; i < sizeof(predefined) / sizeof(predefined[0]); i++)
		if (predefined[i].key == h)
			return strdup(predefined[i].path);

	struct kobject *o = is_predefined(h) ? NULL : handle_ref(h, KOBJ_KEY);
	if (!o) {
		*err = ERROR_INVALID_HANDLE;
		return NULL;
	}
	char *p = strdup(((struct hkey_obj *)o)->path);
	kobj_unref(o);
	if (!p)
		*err = ERROR_NOT_ENOUGH_MEMORY;
	return p;
}

/*
 * The path of subkey sub of h (malloc()ed), or NULL with *err. A leading
 * backslash is a bad path, as on Windows.
 */
static char *sub_path(HKEY h, const char *sub, LONG *err)
{
	char *base = key_path(h, err);

	if (!base || !sub || !*sub)
		return base;
	if (*sub == '\\') {
		free(base);
		*err = ERROR_BAD_PATHNAME;
		return NULL;
	}
	char *p = malloc(strlen(base) + strlen(sub) + 2);
	if (p)
		sprintf(p, "%s\\%s", base, sub);
	else
		*err = ERROR_NOT_ENOUGH_MEMORY;
	free(base);
	return p;
}

/* The key behind h, with the registry locked; unlock with reg_unlock(). */
static struct reg_key *lock_key(HKEY h, LONG *err)
{
	char *path = key_path(h, err);

	if (!path)
		return NULL;
	reg_lock();
	struct reg_key *k = reg_open(path);
	free(path);
	if (!k) {
		reg_unlock();
		*err = ERROR_KEY_DELETED;
	}
	return k;
}

static void key_destroy(struct kobject *obj)
{
	struct hkey_obj *k = (struct hkey_obj *)obj;

	free(k->path);
	free(k);
}

static HKEY new_handle(const char *path)
{
	struct hkey_obj *k = malloc(sizeof(*k));

	if (!k || !(k->path = strdup(path))) {
		free(k);
		return NULL;
	}
	kobj_init(&k->obj, KOBJ_KEY, key_destroy);
	HKEY h = handle_new(&k->obj);
	if (!h)
		kobj_unref(&k->obj);
	return h;
}

/* Opening, creating, closing and deleting keys. */

static LONG open_key(HKEY h, const char *sub, int create, DWORD options,
		     HKEY *out, DWORD *disposition)
{
	LONG err = ERROR_SUCCESS;
	int created = 0;

	if (!out)
		return ERROR_INVALID_PARAMETER;
	*out = NULL;
	char *base = key_path(h, &err), *path = base ? sub_path(h, sub, &err) : NULL;
	if (!path) {
		free(base);
		return err;
	}
	reg_lock();
	struct reg_key *k = NULL;
	if (!reg_open(base))
		err = ERROR_KEY_DELETED;
	else if (create)
		k = reg_create(path, options & REG_OPTION_VOLATILE, &created, &err);
	else if (!(k = reg_open(path)))
		err = ERROR_FILE_NOT_FOUND;
	if (created)
		reg_save();
	reg_unlock();
	if (!err && !(*out = new_handle(path)))
		err = ERROR_NOT_ENOUGH_MEMORY;
	if (!err && disposition)
		*disposition = created ? REG_CREATED_NEW_KEY : REG_OPENED_EXISTING_KEY;
	TRACE("Reg%sKey(%s) = %ld", create ? "Create" : "Open", path, (long)err);
	free(base);
	free(path);
	return err;
}

WINFN(LONG) adv_RegOpenKeyExA(HKEY h, LPCSTR sub, DWORD options, REGSAM sam,
			      HKEY *out)
{
	char *s = acp_to_utf8(sub);
	LONG r = open_key(h, s, 0, 0, out, NULL);
	free(s);
	return r;
}

WINFN(LONG) adv_RegOpenKeyExW(HKEY h, LPCWSTR sub, DWORD options, REGSAM sam,
			      HKEY *out)
{
	char *s = utf16_to_utf8_dup(sub);
	LONG r = open_key(h, s, 0, 0, out, NULL);
	free(s);
	return r;
}

/* The old RegOpenKey hands back h itself for an empty subkey. */
WINFN(LONG) adv_RegOpenKeyA(HKEY h, LPCSTR sub, HKEY *out)
{
	if (out && (!sub || !*sub)) {
		*out = h;
		return ERROR_SUCCESS;
	}
	return adv_RegOpenKeyExA(h, sub, 0, KEY_ALL_ACCESS, out);
}

WINFN(LONG) adv_RegOpenKeyW(HKEY h, LPCWSTR sub, HKEY *out)
{
	if (out && (!sub || !*sub)) {
		*out = h;
		return ERROR_SUCCESS;
	}
	return adv_RegOpenKeyExW(h, sub, 0, KEY_ALL_ACCESS, out);
}

WINFN(LONG) adv_RegCreateKeyExA(HKEY h, LPCSTR sub, DWORD reserved,
				LPSTR cls, DWORD options, REGSAM sam,
				LPSECURITY_ATTRIBUTES sa, HKEY *out,
				LPDWORD disposition)
{
	char *s = acp_to_utf8(sub);
	LONG r = open_key(h, s, 1, options, out, disposition);
	free(s);
	return r;
}

WINFN(LONG) adv_RegCreateKeyExW(HKEY h, LPCWSTR sub, DWORD reserved,
				LPWSTR cls, DWORD options, REGSAM sam,
				LPSECURITY_ATTRIBUTES sa, HKEY *out,
				LPDWORD disposition)
{
	char *s = utf16_to_utf8_dup(sub);
	LONG r = open_key(h, s, 1, options, out, disposition);
	free(s);
	return r;
}

WINFN(LONG) adv_RegCreateKeyA(HKEY h, LPCSTR sub, HKEY *out)
{
	return adv_RegCreateKeyExA(h, sub, 0, NULL, 0, KEY_ALL_ACCESS, NULL,
				   out, NULL);
}

WINFN(LONG) adv_RegCreateKeyW(HKEY h, LPCWSTR sub, HKEY *out)
{
	return adv_RegCreateKeyExW(h, sub, 0, NULL, 0, KEY_ALL_ACCESS, NULL,
				   out, NULL);
}

WINFN(LONG) adv_RegCloseKey(HKEY h)
{
	if (is_predefined(h))
		return ERROR_SUCCESS;
	struct kobject *o = handle_ref(h, KOBJ_KEY);
	if (!o)
		return ERROR_INVALID_HANDLE;
	kobj_unref(o);
	handle_close(h);
	return ERROR_SUCCESS;
}

/* Deletes subkey sub of h (h's own key if sub is empty). */
static LONG delete_key(HKEY h, const char *sub)
{
	LONG err = ERROR_SUCCESS;
	char *path = sub_path(h, sub, &err);

	if (!path)
		return err;
	reg_lock();
	struct reg_key *k = reg_open(path);
	err = k ? reg_delete(k) : ERROR_FILE_NOT_FOUND;
	if (!err)
		reg_save();
	reg_unlock();
	TRACE("RegDeleteKey(%s) = %ld", path, (long)err);
	free(path);
	return err;
}

WINFN(LONG) adv_RegDeleteKeyA(HKEY h, LPCSTR sub)
{
	char *s = acp_to_utf8(sub);
	LONG r = delete_key(h, s);
	free(s);
	return r;
}

WINFN(LONG) adv_RegDeleteKeyW(HKEY h, LPCWSTR sub)
{
	char *s = utf16_to_utf8_dup(sub);
	LONG r = delete_key(h, s);
	free(s);
	return r;
}

WINFN(LONG) adv_RegDeleteKeyExA(HKEY h, LPCSTR sub, REGSAM sam, DWORD reserved)
{
	return adv_RegDeleteKeyA(h, sub);
}

WINFN(LONG) adv_RegDeleteKeyExW(HKEY h, LPCWSTR sub, REGSAM sam, DWORD reserved)
{
	return adv_RegDeleteKeyW(h, sub);
}

WINFN(LONG) adv_RegFlushKey(HKEY h)
{
	LONG err = ERROR_SUCCESS;
	struct reg_key *k = lock_key(h, &err);

	if (!k)
		return err;
	err = reg_save();
	reg_unlock();
	return err;
}

/* Values. */

static int is_string(DWORD type)
{
	return type == REG_SZ || type == REG_EXPAND_SZ || type == REG_MULTI_SZ;
}

/* UTF-16 string data as ANSI (malloc()ed), for the A functions. */
static BYTE *to_ansi(const BYTE *data, DWORD size, DWORD *out)
{
	int n = cp_from_utf16(CP_ACP, (const WCHAR *)data, size / 2, NULL, 0,
			      '?', NULL);
	BYTE *a = malloc(n > 0 ? n : 1);

	if (a)
		cp_from_utf16(CP_ACP, (const WCHAR *)data, size / 2, (char *)a,
			      n, '?', NULL);
	*out = n > 0 ? n : 0;
	return a;
}

/* ANSI string data from an A function as UTF-16 (malloc()ed). */
static BYTE *to_wide(const BYTE *data, DWORD size, DWORD *out)
{
	int n = cp_to_utf16(CP_ACP, (const char *)data, size, NULL, 0, 0);
	WCHAR *w = malloc((n > 0 ? n : 1) * sizeof(WCHAR));

	if (w)
		cp_to_utf16(CP_ACP, (const char *)data, size, w, n, 0);
	*out = n > 0 ? n * sizeof(WCHAR) : 0;
	return (BYTE *)w;
}

/*
 * Copies value data out with RegQueryValueEx's contract: *cb in is the
 * buffer size, out the data size; ERROR_MORE_DATA if it doesn't fit.
 */
static LONG copy_data(const BYTE *src, DWORD size, BYTE *data, DWORD *cb)
{
	if (!cb)
		return ERROR_SUCCESS;
	if (data && *cb < size) {
		*cb = size;
		return ERROR_MORE_DATA;
	}
	if (data && size)
		memcpy(data, src, size);
	*cb = size;
	return ERROR_SUCCESS;
}

/* A value's type and data (converted for the A functions), malloc()ed. */
static LONG get_value(HKEY h, const char *name, int ansi, DWORD *type,
		      BYTE **data, DWORD *size)
{
	LONG err = ERROR_SUCCESS;
	struct reg_key *k = lock_key(h, &err);

	if (!k)
		return err;
	struct reg_value *v = reg_get(k, name);
	if (!v) {
		err = ERROR_FILE_NOT_FOUND;
	} else {
		*type = v->type;
		if (ansi && is_string(v->type)) {
			*data = to_ansi(v->data, v->size, size);
		} else {
			*size = v->size;
			if ((*data = malloc(v->size ? v->size : 1)))
				memcpy(*data, v->data, v->size);
		}
		if (!*data)
			err = ERROR_NOT_ENOUGH_MEMORY;
	}
	reg_unlock();
	return err;
}

static LONG query_value(HKEY h, const char *name, int ansi, LPDWORD type,
			LPBYTE data, LPDWORD cb)
{
	DWORD t, size;
	BYTE *buf = NULL;

	if (data && !cb)
		return ERROR_INVALID_PARAMETER;
	LONG err = get_value(h, name, ansi, &t, &buf, &size);
	if (!err) {
		if (type)
			*type = t;
		err = copy_data(buf, size, data, cb);
	}
	TRACE("RegQueryValueEx(%s) = %ld", name ? name : "", (long)err);
	free(buf);
	return err;
}

WINFN(LONG) adv_RegQueryValueExA(HKEY h, LPCSTR name, LPDWORD reserved,
				 LPDWORD type, LPBYTE data, LPDWORD cb)
{
	char *n = acp_to_utf8(name);
	LONG r = query_value(h, n, 1, type, data, cb);
	free(n);
	return r;
}

WINFN(LONG) adv_RegQueryValueExW(HKEY h, LPCWSTR name, LPDWORD reserved,
				 LPDWORD type, LPBYTE data, LPDWORD cb)
{
	char *n = utf16_to_utf8_dup(name);
	LONG r = query_value(h, n, 0, type, data, cb);
	free(n);
	return r;
}

static LONG set_value(HKEY h, const char *name, int ansi, DWORD type,
		      const BYTE *data, DWORD cb)
{
	LONG err = ERROR_SUCCESS;
	BYTE *conv = NULL;

	if (!data && cb)
		return ERROR_NOACCESS;
	if (ansi && is_string(type) && cb) {
		if (!(conv = to_wide(data, cb, &cb)))
			return ERROR_NOT_ENOUGH_MEMORY;
		data = conv;
	}
	struct reg_key *k = lock_key(h, &err);
	if (k) {
		err = reg_set(k, name, type, data, cb);
		if (!err)
			reg_save();
		reg_unlock();
	}
	TRACE("RegSetValueEx(%s, type %u, %u bytes) = %ld", name ? name : "",
	      type, cb, (long)err);
	free(conv);
	return err;
}

WINFN(LONG) adv_RegSetValueExA(HKEY h, LPCSTR name, DWORD reserved, DWORD type,
			       const BYTE *data, DWORD cb)
{
	char *n = acp_to_utf8(name);
	LONG r = set_value(h, n, 1, type, data, cb);
	free(n);
	return r;
}

WINFN(LONG) adv_RegSetValueExW(HKEY h, LPCWSTR name, DWORD reserved, DWORD type,
			       const BYTE *data, DWORD cb)
{
	char *n = utf16_to_utf8_dup(name);
	LONG r = set_value(h, n, 0, type, data, cb);
	free(n);
	return r;
}

static LONG delete_value(HKEY h, const char *name)
{
	LONG err = ERROR_SUCCESS;
	struct reg_key *k = lock_key(h, &err);

	if (!k)
		return err;
	err = reg_unset(k, name);
	if (!err)
		reg_save();
	reg_unlock();
	return err;
}

WINFN(LONG) adv_RegDeleteValueA(HKEY h, LPCSTR name)
{
	char *n = acp_to_utf8(name);
	LONG r = delete_value(h, n);
	free(n);
	return r;
}

WINFN(LONG) adv_RegDeleteValueW(HKEY h, LPCWSTR name)
{
	char *n = utf16_to_utf8_dup(name);
	LONG r = delete_value(h, n);
	free(n);
	return r;
}

/*
 * The old RegSetValue/RegQueryValue: the default value of a subkey, which
 * RegSetValue creates. Always REG_SZ; its length comes from the string.
 */
static LONG set_default(HKEY h, const char *sub, DWORD type, const char *s)
{
	HKEY k;
	LONG err;

	if (type != REG_SZ || !s)
		return ERROR_INVALID_PARAMETER;
	if ((err = open_key(h, sub, 1, 0, &k, NULL)))
		return err;
	WCHAR *w = utf8_to_utf16_dup(s);
	err = w ? set_value(k, "", 0, REG_SZ, (const BYTE *)w,
			    (wstrlen(w) + 1) * sizeof(WCHAR)) :
		  ERROR_NOT_ENOUGH_MEMORY;
	free(w);
	adv_RegCloseKey(k);
	return err;
}

WINFN(LONG) adv_RegSetValueA(HKEY h, LPCSTR sub, DWORD type, LPCSTR data,
			     DWORD cb)
{
	char *s = acp_to_utf8(sub), *d = acp_to_utf8(data);
	LONG r = set_default(h, s, type, d);
	free(s);
	free(d);
	return r;
}

WINFN(LONG) adv_RegSetValueW(HKEY h, LPCWSTR sub, DWORD type, LPCWSTR data,
			     DWORD cb)
{
	char *s = utf16_to_utf8_dup(sub), *d = utf16_to_utf8_dup(data);
	LONG r = set_default(h, s, type, d);
	free(s);
	free(d);
	return r;
}

/* An unset default value reads as an empty string. */
static LONG query_default(HKEY h, const char *sub, int ansi, BYTE *data,
			  LONG *cb)
{
	static const WCHAR empty[1];
	HKEY k;
	LONG err;
	DWORD type, size;
	BYTE *buf = NULL;

	if (data && !cb)
		return ERROR_INVALID_PARAMETER;
	if ((err = open_key(h, sub, 0, 0, &k, NULL)))
		return err;
	err = get_value(k, "", ansi, &type, &buf, &size);
	if (err == ERROR_FILE_NOT_FOUND) {
		free(buf);
		buf = ansi ? to_ansi((const BYTE *)empty, sizeof(empty), &size) :
			     malloc(sizeof(empty));
		if (buf && !ansi) {
			memcpy(buf, empty, sizeof(empty));
			size = sizeof(empty);
		}
		err = buf ? ERROR_SUCCESS : ERROR_NOT_ENOUGH_MEMORY;
	}
	if (!err && cb) {
		DWORD c = *cb;
		err = copy_data(buf, size, data, &c);
		*cb = c;
	}
	free(buf);
	adv_RegCloseKey(k);
	return err;
}

WINFN(LONG) adv_RegQueryValueA(HKEY h, LPCSTR sub, LPSTR data, PLONG cb)
{
	char *s = acp_to_utf8(sub);
	LONG r = query_default(h, s, 1, (BYTE *)data, cb);
	free(s);
	return r;
}

WINFN(LONG) adv_RegQueryValueW(HKEY h, LPCWSTR sub, LPWSTR data, PLONG cb)
{
	char *s = utf16_to_utf8_dup(sub);
	LONG r = query_default(h, s, 0, (BYTE *)data, cb);
	free(s);
	return r;
}

/* Enumeration and information. */

/*
 * Copies a name out: *cch in is the buffer size in characters, out the
 * length without the NUL; ERROR_MORE_DATA if it doesn't fit.
 */
static LONG copy_name(const char *utf8, int wide, void *buf, LPDWORD cch)
{
	void *s = wide ? (void *)utf8_to_utf16_dup(utf8) : utf8_to_acp_dup(utf8);
	LONG err = ERROR_SUCCESS;

	if (!s)
		return ERROR_NOT_ENOUGH_MEMORY;
	size_t len = wide ? wstrlen(s) : strlen(s);
	if (!buf || !cch || len + 1 > *cch) {
		err = ERROR_MORE_DATA;
	} else {
		memcpy(buf, s, (len + 1) * (wide ? sizeof(WCHAR) : 1));
		*cch = len;
	}
	free(s);
	return err;
}

static void set_empty(void *buf, LPDWORD cch, int wide)
{
	if (buf && cch && *cch) {
		if (wide)
			*(WCHAR *)buf = 0;
		else
			*(char *)buf = '\0';
	}
	if (cch)
		*cch = 0;
}

static LONG enum_key(HKEY h, DWORD index, int wide, void *name, LPDWORD cch,
		     void *cls, LPDWORD cchcls, PFILETIME ft)
{
	LONG err = ERROR_SUCCESS;
	struct reg_key *k = lock_key(h, &err);
	char *n = NULL;

	if (!k)
		return err;
	if (index >= k->nsub) {
		err = ERROR_NO_MORE_ITEMS;
	} else {
		n = strdup(k->sub[index]->name);
		if (ft)
			*ft = k->sub[index]->modified;
	}
	reg_unlock();
	if (!err)
		err = n ? copy_name(n, wide, name, cch) : ERROR_NOT_ENOUGH_MEMORY;
	if (!err)
		set_empty(cls, cchcls, wide);
	free(n);
	return err;
}

WINFN(LONG) adv_RegEnumKeyExA(HKEY h, DWORD index, LPSTR name, LPDWORD cch,
			      LPDWORD reserved, LPSTR cls, LPDWORD cchcls,
			      PFILETIME ft)
{
	return enum_key(h, index, 0, name, cch, cls, cchcls, ft);
}

WINFN(LONG) adv_RegEnumKeyExW(HKEY h, DWORD index, LPWSTR name, LPDWORD cch,
			      LPDWORD reserved, LPWSTR cls, LPDWORD cchcls,
			      PFILETIME ft)
{
	return enum_key(h, index, 1, name, cch, cls, cchcls, ft);
}

WINFN(LONG) adv_RegEnumKeyA(HKEY h, DWORD index, LPSTR name, DWORD cch)
{
	return enum_key(h, index, 0, name, &cch, NULL, NULL, NULL);
}

WINFN(LONG) adv_RegEnumKeyW(HKEY h, DWORD index, LPWSTR name, DWORD cch)
{
	return enum_key(h, index, 1, name, &cch, NULL, NULL, NULL);
}

static LONG enum_value(HKEY h, DWORD index, int wide, void *name,
		       LPDWORD cch, LPDWORD type, LPBYTE data, LPDWORD cb)
{
	LONG err = ERROR_SUCCESS;
	struct reg_key *k = lock_key(h, &err);
	char *n = NULL;
	BYTE *buf = NULL;
	DWORD t = 0, size = 0;

	if (!k)
		return err;
	if (index >= k->nval) {
		err = ERROR_NO_MORE_ITEMS;
	} else {
		const struct reg_value *v = &k->val[index];
		n = strdup(v->name);
		t = v->type;
		if (!wide && is_string(t)) {
			buf = to_ansi(v->data, v->size, &size);
		} else if ((buf = malloc(v->size ? v->size : 1))) {
			memcpy(buf, v->data, v->size);
			size = v->size;
		}
	}
	reg_unlock();
	if (!err && (!n || !buf))
		err = ERROR_NOT_ENOUGH_MEMORY;
	if (!err)
		err = copy_name(n, wide, name, cch);
	if (!err) {
		if (type)
			*type = t;
		err = copy_data(buf, size, data, cb);
	}
	free(n);
	free(buf);
	return err;
}

WINFN(LONG) adv_RegEnumValueA(HKEY h, DWORD index, LPSTR name, LPDWORD cch,
			      LPDWORD reserved, LPDWORD type, LPBYTE data,
			      LPDWORD cb)
{
	return enum_value(h, index, 0, name, cch, type, data, cb);
}

WINFN(LONG) adv_RegEnumValueW(HKEY h, DWORD index, LPWSTR name, LPDWORD cch,
			      LPDWORD reserved, LPDWORD type, LPBYTE data,
			      LPDWORD cb)
{
	return enum_value(h, index, 1, name, cch, type, data, cb);
}

/* A name's length in UTF-16 units (also its ANSI length, for 1252). */
static DWORD name_len(const char *s)
{
	WCHAR *w = utf8_to_utf16_dup(s);
	DWORD n = w ? wstrlen(w) : strlen(s);

	free(w);
	return n;
}

static LONG query_info(HKEY h, int wide, void *cls, LPDWORD cchcls,
		       LPDWORD nsub, LPDWORD maxsub, LPDWORD maxcls,
		       LPDWORD nval, LPDWORD maxname, LPDWORD maxdata,
		       LPDWORD secdesc, PFILETIME ft)
{
	LONG err = ERROR_SUCCESS;
	struct reg_key *k = lock_key(h, &err);
	DWORD ms = 0, mn = 0, md = 0;

	if (!k)
		return err;
	for (size_t i = 0; i < k->nsub; i++) {
		DWORD n = name_len(k->sub[i]->name);
		ms = n > ms ? n : ms;
	}
	for (size_t i = 0; i < k->nval; i++) {
		DWORD n = name_len(k->val[i].name);
		mn = n > mn ? n : mn;
		md = k->val[i].size > md ? k->val[i].size : md;
	}
	if (nsub)
		*nsub = k->nsub;
	if (nval)
		*nval = k->nval;
	if (ft)
		*ft = k->modified;
	reg_unlock();
	if (maxsub)
		*maxsub = ms;
	if (maxname)
		*maxname = mn;
	if (maxdata)
		*maxdata = md;
	if (maxcls)
		*maxcls = 0;
	if (secdesc)
		*secdesc = 0;
	set_empty(cls, cchcls, wide);
	return ERROR_SUCCESS;
}

WINFN(LONG) adv_RegQueryInfoKeyA(HKEY h, LPSTR cls, LPDWORD cchcls,
				 LPDWORD reserved, LPDWORD nsub, LPDWORD maxsub,
				 LPDWORD maxcls, LPDWORD nval, LPDWORD maxname,
				 LPDWORD maxdata, LPDWORD secdesc, PFILETIME ft)
{
	return query_info(h, 0, cls, cchcls, nsub, maxsub, maxcls, nval,
			  maxname, maxdata, secdesc, ft);
}

WINFN(LONG) adv_RegQueryInfoKeyW(HKEY h, LPWSTR cls, LPDWORD cchcls,
				 LPDWORD reserved, LPDWORD nsub, LPDWORD maxsub,
				 LPDWORD maxcls, LPDWORD nval, LPDWORD maxname,
				 LPDWORD maxdata, LPDWORD secdesc, PFILETIME ft)
{
	return query_info(h, 1, cls, cchcls, nsub, maxsub, maxcls, nval,
			  maxname, maxdata, secdesc, ft);
}

/* GetUserName: $USER, else "user"; *size counts the NUL. */
static BOOL user_name(void *buf, LPDWORD size, int wide)
{
	const char *u = getenv("USER");
	void *s;

	if (!size) {
		win_set_error(ERROR_INVALID_PARAMETER);
		return FALSE;
	}
	if (!u || !*u)
		u = "user";
	s = wide ? (void *)utf8_to_utf16_dup(u) : utf8_to_acp_dup(u);
	if (!s) {
		win_set_error(ERROR_NOT_ENOUGH_MEMORY);
		return FALSE;
	}
	DWORD len = wide ? wstrlen(s) : strlen(s);
	BOOL ok = buf && *size > len;
	if (ok)
		memcpy(buf, s, (len + 1) * (wide ? sizeof(WCHAR) : 1));
	else
		win_set_error(ERROR_INSUFFICIENT_BUFFER);
	*size = len + 1;
	free(s);
	return ok;
}

WINFN(BOOL) adv_GetUserNameA(LPSTR buf, LPDWORD size)
{
	return user_name(buf, size, 0);
}

WINFN(BOOL) adv_GetUserNameW(LPWSTR buf, LPDWORD size)
{
	return user_name(buf, size, 1);
}

#define E(name) { #name, 0, (void *)adv_##name }

static const struct builtin_export exports[] = {
	E(GetUserNameA),
	E(GetUserNameW),
	E(RegCloseKey),
	E(RegCreateKeyA),
	E(RegCreateKeyExA),
	E(RegCreateKeyExW),
	E(RegCreateKeyW),
	E(RegDeleteKeyA),
	E(RegDeleteKeyExA),
	E(RegDeleteKeyExW),
	E(RegDeleteKeyW),
	E(RegDeleteValueA),
	E(RegDeleteValueW),
	E(RegEnumKeyA),
	E(RegEnumKeyExA),
	E(RegEnumKeyExW),
	E(RegEnumKeyW),
	E(RegEnumValueA),
	E(RegEnumValueW),
	E(RegFlushKey),
	E(RegOpenKeyA),
	E(RegOpenKeyExA),
	E(RegOpenKeyExW),
	E(RegOpenKeyW),
	E(RegQueryInfoKeyA),
	E(RegQueryInfoKeyW),
	E(RegQueryValueA),
	E(RegQueryValueExA),
	E(RegQueryValueExW),
	E(RegQueryValueW),
	E(RegSetValueA),
	E(RegSetValueExA),
	E(RegSetValueExW),
	E(RegSetValueW),
	BUILTIN_END
};

static const struct builtin_export *const tables[] = { exports, NULL };

const struct builtin_dll builtin_advapi32 = { "advapi32.dll", NULL, tables, 0 };
