/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * advapi32's service control manager and event log.
 *
 * Services: the database is the registry's, as on Windows: a service is a
 * key under HKLM\SYSTEM\CurrentControlSet\Services with a Type value, its
 * configuration in Start, ErrorControl, ImagePath, DisplayName, Group,
 * DependOnService and ObjectName. Win32 services can be created, read,
 * changed, enumerated and deleted (marked first and gone when the last
 * handle to them closes). None is ever run: the harness starts no other
 * programs, so StartService reports the program as not answering
 * (ERROR_SERVICE_REQUEST_TIMEOUT) and every service stays stopped, and a
 * program asking to run as one (StartServiceCtrlDispatcher) is told that
 * no service controller started it. Drivers (a codec's protection driver)
 * may not be installed or started.
 *
 * The event log keeps what this process reports, in memory: each source
 * writes to the log the registry places it in
 * (Services\EventLog\<log>\<source>), else Application, and the logs
 * read back as EVENTLOGRECORDs, forwards or backwards, sequentially or
 * from a record number. VFW_TRACE shows each event as it is reported.
 */
#include <pthread.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#include "common/log.h"
#include "win32/codepage.h"
#include "win32/handle.h"
#include "win32/internal.h"
#include "win32/k32.h"
#include "win32/reg.h"
#include "win32/security.h"
#include "runtime/builtin.h"

extern const struct builtin_dll builtin_kernel32;

#define SERVICES "HKEY_LOCAL_MACHINE\\SYSTEM\\CurrentControlSet\\Services"
#define EVENTLOG_KEY SERVICES "\\EventLog"

/* Strings in and out: UTF-8 inside, the caller's A or W outside. */

static char *in_str(const void *s, int wide)
{
	if (!s)
		return NULL;
	return wide ? utf16_to_utf8_dup(s) : acp_to_utf8(s);
}

/* A MULTI_SZ list in, as UTF-8 strings each NUL-ended, then a NUL. */
static char *in_multi(const void *s, int wide, size_t *len)
{
	size_t n = 0, cap = 1;
	char *out = malloc(1);

	*len = 0;
	if (!out)
		return NULL;
	while (s) {
		char *one = in_str(s, wide);
		if (!one || !*one) {
			free(one);
			break;
		}
		size_t k = strlen(one) + 1;
		char *p = realloc(out, cap + k);
		if (!p) {
			free(one);
			free(out);
			return NULL;
		}
		out = p;
		memcpy(out + n, one, k);
		n += k;
		cap += k;
		s = wide ? (const void *)((const WCHAR *)s + wstrlen(s) + 1) :
			   (const void *)((const char *)s + strlen(s) + 1);
		free(one);
	}
	out[n] = 0;
	*len = n + 1;
	return out;
}

/* Strings packed after a structure in the caller's buffer. */
struct pack {
	BYTE *buf;
	size_t size, at;
	int wide;
};

/* s (UTF-8, n bytes with its NULs) at the end of the pack, or NULL. */
static void *pack_put(struct pack *p, const char *s, size_t n)
{
	size_t chars;
	void *r = NULL;

	if (!s)
		return NULL;
	if (p->wide) {
		WCHAR *w = NULL;
		int k = cp_to_utf16(CP_UTF8, s, (int)n, NULL, 0, 0);
		chars = k > 0 ? (size_t)k : 0;
		p->at = (p->at + 1) & ~(size_t)1;
		if (p->at + chars * sizeof(WCHAR) <= p->size) {
			w = (WCHAR *)(p->buf + p->at);
			cp_to_utf16(CP_UTF8, s, (int)n, w, (int)chars, 0);
			r = w;
		}
		p->at += chars * sizeof(WCHAR);
	} else {
		WCHAR *w = malloc((n + 1) * sizeof(WCHAR));
		int k = w ? cp_to_utf16(CP_UTF8, s, (int)n, w, (int)n + 1, 0) : -1;
		int m = k > 0 ? cp_from_utf16(CP_ACP, w, k, NULL, 0, '?', NULL) : 0;
		chars = m > 0 ? (size_t)m : 0;
		if (p->at + chars <= p->size && w) {
			r = p->buf + p->at;
			cp_from_utf16(CP_ACP, w, k, r, (int)chars, '?', NULL);
		}
		p->at += chars;
		free(w);
	}
	return r;
}

static void *pack_str(struct pack *p, const char *s)
{
	return s ? pack_put(p, s, strlen(s) + 1) : NULL;
}

/* A UTF-8 copy of s into the caller's A or W buffer of *size chars. */
static BOOL out_str(const char *s, void *buf, LPDWORD size, int wide)
{
	WCHAR *w = utf8_to_utf16_dup(s);
	char *a = wide ? NULL : utf8_to_acp_dup(s);
	size_t n = wide ? (w ? wstrlen(w) : 0) : (a ? strlen(a) : 0);
	BOOL ok = TRUE;

	if (!size) {
		ok = FALSE;
		win_set_error(ERROR_INVALID_PARAMETER);
	} else if (!buf || *size <= n) {
		*size = (DWORD)n;
		ok = FALSE;
		win_set_error(ERROR_INSUFFICIENT_BUFFER);
	} else {
		if (wide)
			memcpy(buf, w, (n + 1) * sizeof(WCHAR));
		else
			memcpy(buf, a, n + 1);
		*size = (DWORD)n;
	}
	free(w);
	free(a);
	return ok;
}

/* Registry helpers, with the registry lock held. */

static char *svc_path(const char *name)
{
	size_t n = strlen(SERVICES) + strlen(name) + 2;
	char *p = malloc(n);
	if (p)
		snprintf(p, n, "%s\\%s", SERVICES, name);
	return p;
}

static int reg_dword(struct reg_key *k, const char *name, DWORD *v)
{
	struct reg_value *r = reg_get(k, name);
	if (!r || r->type != REG_DWORD || r->size < 4)
		return 0;
	memcpy(v, r->data, 4);
	return 1;
}

/* A string value as UTF-8 (malloc()ed), or NULL. */
static char *reg_text(struct reg_key *k, const char *name)
{
	struct reg_value *r = reg_get(k, name);
	if (!r || (r->type != REG_SZ && r->type != REG_EXPAND_SZ))
		return NULL;
	WCHAR *w = calloc(r->size / 2 + 1, sizeof(WCHAR));
	if (!w)
		return NULL;
	memcpy(w, r->data, r->size & ~1u);
	char *s = utf16_to_utf8_dup(w);
	free(w);
	return s;
}

/* A MULTI_SZ as UTF-8 strings and a final NUL; its length in *len. */
static char *reg_multi(struct reg_key *k, const char *name, size_t *len)
{
	struct reg_value *r = reg_get(k, name);
	*len = 0;
	if (!r || r->type != REG_MULTI_SZ)
		return NULL;
	WCHAR *w = calloc(r->size / 2 + 2, sizeof(WCHAR));
	if (!w)
		return NULL;
	memcpy(w, r->data, r->size & ~1u);
	char *s = in_multi(w, 1, len);
	free(w);
	return s;
}

static LONG set_dword(struct reg_key *k, const char *name, DWORD v)
{
	return reg_set(k, name, REG_DWORD, &v, 4);
}

static LONG set_text(struct reg_key *k, const char *name, DWORD type,
		     const char *s)
{
	WCHAR *w = utf8_to_utf16_dup(s);
	if (!w)
		return ERROR_NOT_ENOUGH_MEMORY;
	LONG r = reg_set(k, name, type, w,
			 (DWORD)((wstrlen(w) + 1) * sizeof(WCHAR)));
	free(w);
	return r;
}

static LONG set_multi(struct reg_key *k, const char *name, const char *s,
		      size_t len)
{
	WCHAR *w = malloc((len + 1) * sizeof(WCHAR));
	if (!w)
		return ERROR_NOT_ENOUGH_MEMORY;
	int n = cp_to_utf16(CP_UTF8, s, (int)len, w, (int)len + 1, 0);
	LONG r = reg_set(k, name, REG_MULTI_SZ, w,
			 (DWORD)((n > 0 ? n : 0) * sizeof(WCHAR)));
	free(w);
	return r;
}

/* The key of service name, if it is one (has a Type). */
static struct reg_key *svc_key(const char *name)
{
	char *path = svc_path(name);
	struct reg_key *k = path ? reg_open(path) : NULL;
	DWORD type;

	free(path);
	return k && reg_dword(k, "Type", &type) ? k : NULL;
}

/* Services and the manager. */

struct scm {
	struct kobject obj;
	DWORD access;
};

struct service {
	struct kobject obj;
	char *name;		/* the key's own name */
	DWORD access;
};

/* Services marked for delete, and how many handles each has open. */
static pthread_mutex_t svc_lock = PTHREAD_MUTEX_INITIALIZER;
static struct svc_open {
	char *name;
	int opens, deleted;
} *opens;
static size_t nopens;
static int db_locked;

static struct svc_open *find_open(const char *name)
{
	for (size_t i = 0; i < nopens; i++)
		if (!strcasecmp(opens[i].name, name))
			return &opens[i];
	return NULL;
}

static int is_deleted(const char *name)
{
	pthread_mutex_lock(&svc_lock);
	struct svc_open *o = find_open(name);
	int r = o && o->deleted;
	pthread_mutex_unlock(&svc_lock);
	return r;
}

static void scm_destroy(struct kobject *o)
{
	free(o);
}

static void service_destroy(struct kobject *obj)
{
	struct service *s = (struct service *)obj;
	char *gone = NULL;

	pthread_mutex_lock(&svc_lock);
	struct svc_open *o = find_open(s->name);
	if (o && --o->opens == 0) {
		if (o->deleted)
			gone = o->name;
		else
			free(o->name);
		*o = opens[--nopens];
	}
	pthread_mutex_unlock(&svc_lock);
	if (gone) {
		reg_lock();
		char *path = svc_path(gone);
		struct reg_key *k = path ? reg_open(path) : NULL;
		if (k)
			reg_delete_tree(k, 0);
		reg_save();
		reg_unlock();
		free(path);
		free(gone);
	}
	free(s->name);
	free(s);
}

static SC_HANDLE open_scm(const void *machine, const void *db, DWORD access,
			  int wide)
{
	char *m = in_str(machine, wide), *d = in_str(db, wide);
	int remote = m && *m && strcmp(m, ".") &&
		     strcmp(m + (m[0] == '\\' && m[1] == '\\' ? 2 : 0),
			    "localhost");
	int bad_db = d && *d && strcasecmp(d, "ServicesActive");

	free(m);
	free(d);
	if (remote) {
		win_set_error(ERROR_ACCESS_DENIED);
		return NULL;
	}
	if (bad_db) {
		win_set_error(ERROR_INVALID_NAME);
		return NULL;
	}
	struct scm *s = calloc(1, sizeof(*s));
	if (!s) {
		win_set_error(ERROR_NOT_ENOUGH_MEMORY);
		return NULL;
	}
	kobj_init(&s->obj, KOBJ_SCM, scm_destroy);
	s->access = access;
	HANDLE h = handle_new(&s->obj);
	if (!h)
		win_set_error(ERROR_NOT_ENOUGH_MEMORY);
	return h;
}

WINFN(SC_HANDLE) adv_OpenSCManagerA(LPCSTR machine, LPCSTR db, DWORD access)
{
	return open_scm(machine, db, access, 0);
}

WINFN(SC_HANDLE) adv_OpenSCManagerW(LPCWSTR machine, LPCWSTR db, DWORD access)
{
	return open_scm(machine, db, access, 1);
}

static struct scm *scm_ref(SC_HANDLE h)
{
	return (struct scm *)handle_ref(h, KOBJ_SCM);
}

static struct service *service_ref(SC_HANDLE h)
{
	return (struct service *)handle_ref(h, KOBJ_SERVICE);
}

/* A handle to service name (the key's own spelling). */
static SC_HANDLE new_service(const char *name, DWORD access)
{
	struct service *s = calloc(1, sizeof(*s));
	char *n = strdup(name);
	char *n2 = strdup(name);

	if (!s || !n || !n2) {
		free(s);
		free(n);
		free(n2);
		win_set_error(ERROR_NOT_ENOUGH_MEMORY);
		return NULL;
	}
	pthread_mutex_lock(&svc_lock);
	struct svc_open *o = find_open(name);
	if (!o) {
		struct svc_open *p = realloc(opens, (nopens + 1) * sizeof(*p));
		if (!p) {
			pthread_mutex_unlock(&svc_lock);
			free(s);
			free(n);
			free(n2);
			win_set_error(ERROR_NOT_ENOUGH_MEMORY);
			return NULL;
		}
		opens = p;
		o = &opens[nopens++];
		o->name = n2;
		o->opens = 0;
		o->deleted = 0;
		n2 = NULL;
	}
	o->opens++;
	pthread_mutex_unlock(&svc_lock);
	free(n2);
	kobj_init(&s->obj, KOBJ_SERVICE, service_destroy);
	s->name = n;
	s->access = access;
	HANDLE h = handle_new(&s->obj);
	if (!h)
		win_set_error(ERROR_NOT_ENOUGH_MEMORY);
	return h;
}

static SC_HANDLE open_service(SC_HANDLE scm, const void *name, DWORD access,
			      int wide)
{
	struct scm *m = scm_ref(scm);
	if (!m)
		return NULL;
	kobj_unref(&m->obj);
	if (!name) {
		win_set_error(ERROR_INVALID_ADDRESS);
		return NULL;
	}
	char *n = in_str(name, wide);
	if (!n || !*n || strchr(n, '\\') || strchr(n, '/')) {
		free(n);
		win_set_error(ERROR_INVALID_NAME);
		return NULL;
	}
	reg_lock();
	struct reg_key *k = svc_key(n);
	char *own = k ? strdup(k->name) : NULL;
	reg_unlock();
	free(n);
	if (!k) {
		win_set_error(ERROR_SERVICE_DOES_NOT_EXIST);
		return NULL;
	}
	if (!own) {
		win_set_error(ERROR_NOT_ENOUGH_MEMORY);
		return NULL;
	}
	SC_HANDLE h = new_service(own, access);
	free(own);
	return h;
}

WINFN(SC_HANDLE) adv_OpenServiceA(SC_HANDLE scm, LPCSTR name, DWORD access)
{
	return open_service(scm, name, access, 0);
}

WINFN(SC_HANDLE) adv_OpenServiceW(SC_HANDLE scm, LPCWSTR name, DWORD access)
{
	return open_service(scm, name, access, 1);
}

WINFN(BOOL) adv_CloseServiceHandle(SC_HANDLE h)
{
	struct kobject *o = handle_ref(h, KOBJ_SCM);
	if (!o)
		o = handle_ref(h, KOBJ_SERVICE);
	if (!o) {
		win_set_error(ERROR_INVALID_HANDLE);
		return FALSE;
	}
	kobj_unref(o);
	return handle_close(h);
}

/* A display name already used by a service other than name. */
static int display_taken(const char *display, const char *name)
{
	struct reg_key *root = reg_open(SERVICES);
	for (size_t i = 0; root && i < root->nsub; i++) {
		struct reg_key *k = root->sub[i];
		DWORD t;
		if (!reg_dword(k, "Type", &t) || !strcasecmp(k->name, name))
			continue;
		char *d = reg_text(k, "DisplayName");
		int same = d && !strcasecmp(d, display);
		free(d);
		if (same || !strcasecmp(k->name, display))
			return 1;
	}
	return 0;
}

static int valid_config(DWORD type, DWORD start, DWORD error)
{
	DWORD base = type & ~SERVICE_INTERACTIVE_PROCESS;
	if (base != SERVICE_WIN32_OWN_PROCESS &&
	    base != SERVICE_WIN32_SHARE_PROCESS &&
	    base != SERVICE_KERNEL_DRIVER && base != SERVICE_FILE_SYSTEM_DRIVER)
		return 0;
	if ((type & SERVICE_INTERACTIVE_PROCESS) && !(type & SERVICE_WIN32))
		return 0;
	if (start > SERVICE_DISABLED ||
	    (start < SERVICE_AUTO_START && (type & SERVICE_WIN32)))
		return 0;
	return error <= SERVICE_ERROR_CRITICAL;
}

static SC_HANDLE create_service(SC_HANDLE scm, const void *name,
				const void *display, DWORD access, DWORD type,
				DWORD start, DWORD error, const void *path,
				const void *group, LPDWORD tag, const void *deps,
				const void *user, int wide)
{
	struct scm *m = scm_ref(scm);
	if (!m)
		return NULL;
	DWORD granted = m->access;
	kobj_unref(&m->obj);
	if (!(granted & SC_MANAGER_CREATE_SERVICE)) {
		win_set_error(ERROR_ACCESS_DENIED);
		return NULL;
	}
	if (!name || !path) {
		win_set_error(ERROR_INVALID_ADDRESS);
		return NULL;
	}
	if (!valid_config(type, start, error) || tag) {
		win_set_error(ERROR_INVALID_PARAMETER);
		return NULL;
	}
	if (type & SERVICE_DRIVER) {
		/* No drivers here: a protection driver may not be installed. */
		win_set_error(ERROR_ACCESS_DENIED);
		return NULL;
	}
	char *n = in_str(name, wide), *d = in_str(display, wide);
	char *p = in_str(path, wide), *g = in_str(group, wide);
	char *u = in_str(user, wide);
	size_t dlen = 0;
	char *dep = deps ? in_multi(deps, wide, &dlen) : NULL;
	SC_HANDLE h = NULL;

	if (!n || !*n || strlen(n) > 256 || strchr(n, '\\') || strchr(n, '/')) {
		win_set_error(ERROR_INVALID_NAME);
		goto out;
	}
	if (!p || !*p) {
		win_set_error(ERROR_INVALID_PARAMETER);
		goto out;
	}
	if (is_deleted(n)) {
		win_set_error(ERROR_SERVICE_MARKED_FOR_DELETE);
		goto out;
	}
	reg_lock();
	if (svc_key(n)) {
		reg_unlock();
		win_set_error(ERROR_SERVICE_EXISTS);
		goto out;
	}
	if (display_taken(d && *d ? d : n, n)) {
		reg_unlock();
		win_set_error(ERROR_DUPLICATE_SERVICE_NAME);
		goto out;
	}
	char *kp = svc_path(n);
	LONG err = ERROR_NOT_ENOUGH_MEMORY;
	int created;
	struct reg_key *k = kp ? reg_create(kp, 0, &created, &err) : NULL;
	free(kp);
	if (k) {
		err = set_dword(k, "Type", type);
		if (!err) err = set_dword(k, "Start", start);
		if (!err) err = set_dword(k, "ErrorControl", error);
		if (!err) err = set_text(k, "ImagePath", REG_EXPAND_SZ, p);
		if (!err && d && *d) err = set_text(k, "DisplayName", REG_SZ, d);
		if (!err && g && *g) err = set_text(k, "Group", REG_SZ, g);
		if (!err && dep && dlen > 1)
			err = set_multi(k, "DependOnService", dep, dlen);
		if (!err) err = set_text(k, "ObjectName", REG_SZ,
					 u && *u ? u : "LocalSystem");
		if (err)
			reg_delete_tree(k, 0);
		reg_save();
	}
	reg_unlock();
	if (err) {
		win_set_error(err);
		goto out;
	}
	h = new_service(n, access);
out:
	free(n);
	free(d);
	free(p);
	free(g);
	free(u);
	free(dep);
	return h;
}

WINFN(SC_HANDLE) adv_CreateServiceA(SC_HANDLE scm, LPCSTR name, LPCSTR display,
				    DWORD access, DWORD type, DWORD start,
				    DWORD error, LPCSTR path, LPCSTR group,
				    LPDWORD tag, LPCSTR deps, LPCSTR user,
				    LPCSTR password)
{
	return create_service(scm, name, display, access, type, start, error,
			      path, group, tag, deps, user, 0);
}

WINFN(SC_HANDLE) adv_CreateServiceW(SC_HANDLE scm, LPCWSTR name, LPCWSTR display,
				    DWORD access, DWORD type, DWORD start,
				    DWORD error, LPCWSTR path, LPCWSTR group,
				    LPDWORD tag, LPCWSTR deps, LPCWSTR user,
				    LPCWSTR password)
{
	return create_service(scm, name, display, access, type, start, error,
			      path, group, tag, deps, user, 1);
}

/* The service behind h if it was opened for need; NULL with the error. */
static struct service *service_for(SC_HANDLE h, DWORD need)
{
	struct service *s = service_ref(h);
	if (!s)
		return NULL;
	if ((s->access & need) != need) {
		kobj_unref(&s->obj);
		win_set_error(ERROR_ACCESS_DENIED);
		return NULL;
	}
	return s;
}

WINFN(BOOL) adv_DeleteService(SC_HANDLE h)
{
	struct service *s = service_for(h, DELETE);
	if (!s)
		return FALSE;
	BOOL ok = TRUE;
	pthread_mutex_lock(&svc_lock);
	struct svc_open *o = find_open(s->name);
	if (o && o->deleted) {
		win_set_error(ERROR_SERVICE_MARKED_FOR_DELETE);
		ok = FALSE;
	} else if (o) {
		o->deleted = 1;
	}
	pthread_mutex_unlock(&svc_lock);
	if (ok) {
		/* Windows marks the key too. */
		reg_lock();
		struct reg_key *k = svc_key(s->name);
		if (k) {
			set_dword(k, "DeleteFlag", 1);
			reg_save();
		}
		reg_unlock();
	}
	kobj_unref(&s->obj);
	return ok;
}

/* Its type, start type and whether it still exists. */
static int service_type(const char *name, DWORD *type, DWORD *start)
{
	reg_lock();
	struct reg_key *k = svc_key(name);
	int ok = k && reg_dword(k, "Type", type);
	if (ok && !reg_dword(k, "Start", start))
		*start = SERVICE_DEMAND_START;
	reg_unlock();
	return ok;
}

/* Every service is stopped. */
static void stopped(DWORD type, SERVICE_STATUS *st)
{
	memset(st, 0, sizeof(*st));
	st->dwServiceType = type;
	st->dwCurrentState = SERVICE_STOPPED;
	st->dwWin32ExitCode = ERROR_SERVICE_NEVER_STARTED;
}

static BOOL start_service(SC_HANDLE h)
{
	struct service *s = service_for(h, SERVICE_START);
	if (!s)
		return FALSE;
	DWORD type, start;
	int ok = service_type(s->name, &type, &start);
	int deleted = is_deleted(s->name);
	kobj_unref(&s->obj);
	if (!ok)
		win_set_error(ERROR_SERVICE_DOES_NOT_EXIST);
	else if (deleted)
		win_set_error(ERROR_SERVICE_MARKED_FOR_DELETE);
	else if (start == SERVICE_DISABLED)
		win_set_error(ERROR_SERVICE_DISABLED);
	else if (type & SERVICE_DRIVER)
		win_set_error(ERROR_ACCESS_DENIED);
	else
		win_set_error(ERROR_SERVICE_REQUEST_TIMEOUT);
	return FALSE;
}

WINFN(BOOL) adv_StartServiceA(SC_HANDLE svc, DWORD argc, LPCSTR *argv)
{
	return start_service(svc);
}

WINFN(BOOL) adv_StartServiceW(SC_HANDLE svc, DWORD argc, LPCWSTR *argv)
{
	return start_service(svc);
}

WINFN(BOOL) adv_ControlService(SC_HANDLE h, DWORD control,
			       LPSERVICE_STATUS status)
{
	DWORD need = control == SERVICE_CONTROL_STOP ? SERVICE_STOP :
		     control == SERVICE_CONTROL_PAUSE ||
		     control == SERVICE_CONTROL_CONTINUE ? SERVICE_PAUSE_CONTINUE :
		     control == SERVICE_CONTROL_INTERROGATE ? SERVICE_INTERROGATE :
		     control >= 128 && control <= 255 ?
		     SERVICE_USER_DEFINED_CONTROL : 0;
	if (!need) {
		struct service *s = service_ref(h);
		if (s) {
			kobj_unref(&s->obj);
			win_set_error(ERROR_INVALID_PARAMETER);
		}
		return FALSE;
	}
	struct service *s = service_for(h, need);
	if (!s)
		return FALSE;
	DWORD type, start;
	int ok = service_type(s->name, &type, &start);
	kobj_unref(&s->obj);
	if (!ok) {
		win_set_error(ERROR_SERVICE_DOES_NOT_EXIST);
		return FALSE;
	}
	if (status)
		stopped(type, status);
	win_set_error(ERROR_SERVICE_NOT_ACTIVE);
	return FALSE;
}

WINFN(BOOL) adv_QueryServiceStatus(SC_HANDLE h, LPSERVICE_STATUS status)
{
	if (!status) {
		win_set_error(ERROR_INVALID_ADDRESS);
		return FALSE;
	}
	struct service *s = service_for(h, SERVICE_QUERY_STATUS);
	if (!s)
		return FALSE;
	DWORD type, start;
	int ok = service_type(s->name, &type, &start);
	kobj_unref(&s->obj);
	if (!ok) {
		win_set_error(ERROR_SERVICE_DOES_NOT_EXIST);
		return FALSE;
	}
	stopped(type, status);
	return TRUE;
}

WINFN(BOOL) adv_QueryServiceStatusEx(SC_HANDLE h, DWORD level, LPBYTE buf,
				     DWORD size, LPDWORD needed)
{
	if (level != SC_STATUS_PROCESS_INFO) {
		win_set_error(ERROR_INVALID_LEVEL);
		return FALSE;
	}
	if (!needed) {
		win_set_error(ERROR_INVALID_ADDRESS);
		return FALSE;
	}
	*needed = sizeof(SERVICE_STATUS_PROCESS);
	if (!buf || size < sizeof(SERVICE_STATUS_PROCESS)) {
		struct service *s = service_ref(h);
		if (s) {
			kobj_unref(&s->obj);
			win_set_error(ERROR_INSUFFICIENT_BUFFER);
		}
		return FALSE;
	}
	SERVICE_STATUS st;
	if (!adv_QueryServiceStatus(h, &st))
		return FALSE;
	SERVICE_STATUS_PROCESS *p = (SERVICE_STATUS_PROCESS *)buf;
	memset(p, 0, sizeof(*p));
	memcpy(p, &st, sizeof(st));
	return TRUE;
}

/* The configuration, packed as QUERY_SERVICE_CONFIG (A and W alike). */
static BOOL query_config(SC_HANDLE h, void *buf, DWORD size, LPDWORD needed,
			 int wide)
{
	if (!needed) {
		win_set_error(ERROR_INVALID_ADDRESS);
		return FALSE;
	}
	struct service *s = service_for(h, SERVICE_QUERY_CONFIG);
	if (!s)
		return FALSE;
	reg_lock();
	struct reg_key *k = svc_key(s->name);
	DWORD type = 0, start = SERVICE_DEMAND_START, error = 0, tag = 0;
	char *path = NULL, *group = NULL, *user = NULL, *display = NULL,
	     *deps = NULL;
	size_t dlen = 0;
	if (k) {
		reg_dword(k, "Type", &type);
		reg_dword(k, "Start", &start);
		reg_dword(k, "ErrorControl", &error);
		reg_dword(k, "Tag", &tag);
		path = reg_text(k, "ImagePath");
		group = reg_text(k, "Group");
		user = reg_text(k, "ObjectName");
		display = reg_text(k, "DisplayName");
		deps = reg_multi(k, "DependOnService", &dlen);
	}
	reg_unlock();
	char *name = strdup(s->name);
	kobj_unref(&s->obj);
	if (!k) {
		free(name);
		win_set_error(ERROR_SERVICE_DOES_NOT_EXIST);
		return FALSE;
	}

	/* Same layout either way: the A and W structures match. */
	QUERY_SERVICE_CONFIGW *c = buf;
	struct pack p = { buf, buf ? size : 0, sizeof(*c), wide };
	void *pp = pack_str(&p, path ? path : "");
	void *pg = pack_str(&p, group ? group : "");
	void *pd = pack_put(&p, deps && dlen > 1 ? deps : "\0", deps && dlen > 1 ? dlen : 2);
	void *pu = pack_str(&p, user ? user : "");
	void *pn = pack_str(&p, display ? display : name ? name : "");
	*needed = (DWORD)p.at;
	free(path);
	free(group);
	free(user);
	free(display);
	free(deps);
	free(name);
	if (!buf || p.at > size) {
		win_set_error(ERROR_INSUFFICIENT_BUFFER);
		return FALSE;
	}
	c->dwServiceType = type;
	c->dwStartType = start;
	c->dwErrorControl = error;
	c->lpBinaryPathName = pp;
	c->lpLoadOrderGroup = pg;
	c->dwTagId = tag;
	c->lpDependencies = pd;
	c->lpServiceStartName = pu;
	c->lpDisplayName = pn;
	return TRUE;
}

WINFN(BOOL) adv_QueryServiceConfigA(SC_HANDLE h, LPQUERY_SERVICE_CONFIGA buf,
				    DWORD size, LPDWORD needed)
{
	return query_config(h, buf, size, needed, 0);
}

WINFN(BOOL) adv_QueryServiceConfigW(SC_HANDLE h, LPQUERY_SERVICE_CONFIGW buf,
				    DWORD size, LPDWORD needed)
{
	return query_config(h, buf, size, needed, 1);
}

static BOOL change_config(SC_HANDLE h, DWORD type, DWORD start, DWORD error,
			  const void *path, const void *group, LPDWORD tag,
			  const void *deps, const void *user,
			  const void *display, int wide)
{
	struct service *s = service_for(h, SERVICE_CHANGE_CONFIG);
	if (!s)
		return FALSE;
	char *name = strdup(s->name);
	kobj_unref(&s->obj);
	if (!name) {
		win_set_error(ERROR_NOT_ENOUGH_MEMORY);
		return FALSE;
	}
	if (is_deleted(name)) {
		free(name);
		win_set_error(ERROR_SERVICE_MARKED_FOR_DELETE);
		return FALSE;
	}
	char *p = in_str(path, wide), *g = in_str(group, wide);
	char *u = in_str(user, wide), *d = in_str(display, wide);
	size_t dlen = 0;
	char *dep = deps ? in_multi(deps, wide, &dlen) : NULL;
	LONG err = 0;

	reg_lock();
	struct reg_key *k = svc_key(name);
	DWORD ot = 0, os = 0, oe = 0;
	if (!k) {
		err = ERROR_SERVICE_DOES_NOT_EXIST;
	} else {
		reg_dword(k, "Type", &ot);
		if (!reg_dword(k, "Start", &os))
			os = SERVICE_DEMAND_START;
		reg_dword(k, "ErrorControl", &oe);
		DWORD nt = type == SERVICE_NO_CHANGE ? ot : type;
		DWORD ns = start == SERVICE_NO_CHANGE ? os : start;
		DWORD ne = error == SERVICE_NO_CHANGE ? oe : error;
		if (!valid_config(nt, ns, ne) || tag)
			err = ERROR_INVALID_PARAMETER;
		else if ((nt & SERVICE_DRIVER) && !(ot & SERVICE_DRIVER))
			err = ERROR_ACCESS_DENIED;
		else if (d && *d && display_taken(d, name))
			err = ERROR_DUPLICATE_SERVICE_NAME;
		if (!err) {
			set_dword(k, "Type", nt);
			set_dword(k, "Start", ns);
			set_dword(k, "ErrorControl", ne);
			if (p && *p)
				set_text(k, "ImagePath", REG_EXPAND_SZ, p);
			if (g)
				set_text(k, "Group", REG_SZ, g);
			if (dep)
				set_multi(k, "DependOnService", dep, dlen);
			if (u && *u)
				set_text(k, "ObjectName", REG_SZ, u);
			if (d && *d)
				set_text(k, "DisplayName", REG_SZ, d);
			reg_save();
		}
	}
	reg_unlock();
	free(name);
	free(p);
	free(g);
	free(u);
	free(d);
	free(dep);
	if (err) {
		win_set_error(err);
		return FALSE;
	}
	return TRUE;
}

WINFN(BOOL) adv_ChangeServiceConfigA(SC_HANDLE h, DWORD type, DWORD start,
				     DWORD error, LPCSTR path, LPCSTR group,
				     LPDWORD tag, LPCSTR deps, LPCSTR user,
				     LPCSTR password, LPCSTR display)
{
	return change_config(h, type, start, error, path, group, tag, deps,
			     user, display, 0);
}

WINFN(BOOL) adv_ChangeServiceConfigW(SC_HANDLE h, DWORD type, DWORD start,
				     DWORD error, LPCWSTR path, LPCWSTR group,
				     LPDWORD tag, LPCWSTR deps, LPCWSTR user,
				     LPCWSTR password, LPCWSTR display)
{
	return change_config(h, type, start, error, path, group, tag, deps,
			     user, display, 1);
}

/* The display name of key name, or the key name of display name. */
static BOOL service_name(SC_HANDLE scm, const void *in, void *buf,
			 LPDWORD size, int wide, int to_display)
{
	struct scm *m = scm_ref(scm);
	if (!m)
		return FALSE;
	kobj_unref(&m->obj);
	char *n = in_str(in, wide), *out = NULL;
	if (!n) {
		win_set_error(ERROR_INVALID_ADDRESS);
		return FALSE;
	}
	reg_lock();
	if (to_display) {
		struct reg_key *k = svc_key(n);
		if (k && !(out = reg_text(k, "DisplayName")))
			out = strdup(k->name);
	} else {
		struct reg_key *root = reg_open(SERVICES);
		for (size_t i = 0; root && !out && i < root->nsub; i++) {
			DWORD t;
			if (!reg_dword(root->sub[i], "Type", &t))
				continue;
			char *d = reg_text(root->sub[i], "DisplayName");
			if (d && !strcasecmp(d, n))
				out = strdup(root->sub[i]->name);
			free(d);
		}
	}
	reg_unlock();
	free(n);
	if (!out) {
		if (size && buf && *size)
			wide ? (void)(*(WCHAR *)buf = 0) : (void)(*(char *)buf = 0);
		if (size)
			*size = 1;
		win_set_error(ERROR_SERVICE_DOES_NOT_EXIST);
		return FALSE;
	}
	BOOL r = out_str(out, buf, size, wide);
	free(out);
	return r;
}

WINFN(BOOL) adv_GetServiceDisplayNameA(SC_HANDLE scm, LPCSTR name, LPSTR buf,
				       LPDWORD size)
{
	return service_name(scm, name, buf, size, 0, 1);
}

WINFN(BOOL) adv_GetServiceDisplayNameW(SC_HANDLE scm, LPCWSTR name, LPWSTR buf,
				       LPDWORD size)
{
	return service_name(scm, name, buf, size, 1, 1);
}

WINFN(BOOL) adv_GetServiceKeyNameA(SC_HANDLE scm, LPCSTR display, LPSTR buf,
				   LPDWORD size)
{
	return service_name(scm, display, buf, size, 0, 0);
}

WINFN(BOOL) adv_GetServiceKeyNameW(SC_HANDLE scm, LPCWSTR display, LPWSTR buf,
				   LPDWORD size)
{
	return service_name(scm, display, buf, size, 1, 0);
}

/*
 * EnumServicesStatus(Ex): every service of the types asked for, all
 * stopped, so SERVICE_ACTIVE alone finds none. *resume counts the ones
 * already given.
 */
static BOOL enum_services(SC_HANDLE scm, DWORD types, DWORD state, BYTE *buf,
			  DWORD size, LPDWORD needed, LPDWORD count,
			  LPDWORD resume, int wide, int ex, const char *group)
{
	struct scm *m = scm_ref(scm);
	if (!m)
		return FALSE;
	DWORD access = m->access;
	kobj_unref(&m->obj);
	if (!(access & SC_MANAGER_ENUMERATE_SERVICE)) {
		win_set_error(ERROR_ACCESS_DENIED);
		return FALSE;
	}
	if (!needed || !count || !types || (types & ~SERVICE_TYPE_ALL) ||
	    !state || (state & ~SERVICE_STATE_ALL)) {
		win_set_error(ERROR_INVALID_PARAMETER);
		return FALSE;
	}
	size_t entry = ex ? sizeof(ENUM_SERVICE_STATUS_PROCESSW) :
			    sizeof(ENUM_SERVICE_STATUSW);
	DWORD skip = resume ? *resume : 0, given = 0, rest = 0, more = 0;

	*needed = 0;
	*count = 0;
	/* First the names, then pack those that fit. */
	struct item { char *name, *display; DWORD type; } *items = NULL;
	size_t n = 0;
	reg_lock();
	struct reg_key *root = (state & SERVICE_INACTIVE) ? reg_open(SERVICES) : NULL;
	for (size_t i = 0; root && i < root->nsub; i++) {
		struct reg_key *k = root->sub[i];
		DWORD t;
		if (!reg_dword(k, "Type", &t) || !(t & types & ~SERVICE_INTERACTIVE_PROCESS))
			continue;
		if (group) {
			char *g = reg_text(k, "Group");
			int match = *group ? g && !strcasecmp(g, group) : !g || !*g;
			free(g);
			if (!match)
				continue;
		}
		struct item *p = realloc(items, (n + 1) * sizeof(*p));
		if (!p)
			break;
		items = p;
		items[n].name = strdup(k->name);
		items[n].display = reg_text(k, "DisplayName");
		items[n].type = t;
		n++;
	}
	reg_unlock();

	struct pack p = { buf, buf ? size : 0, 0, wide };
	for (size_t i = skip; i < n; i++) {
		/* Size this entry alone, then pack it if it fits. */
		struct pack probe = { NULL, 0, 0, wide };
		pack_str(&probe, items[i].name ? items[i].name : "");
		pack_str(&probe, items[i].display ? items[i].display :
				 items[i].name ? items[i].name : "");
		size_t strings = probe.at + 1;
		if (more || (given + 1) * entry + p.at + strings > p.size) {
			more = 1;
			rest += (DWORD)(entry + strings);
			continue;
		}
		p.at += strings;
		given++;
	}
	/* Pack the ones that fit: structures first, strings after them. */
	p.at = given * entry;
	for (DWORD i = 0; i < given; i++) {
		struct item *it = &items[skip + i];
		BYTE *e = buf + i * entry;
		void *pn = pack_str(&p, it->name ? it->name : "");
		void *pd = pack_str(&p, it->display ? it->display :
				   it->name ? it->name : "");
		SERVICE_STATUS st;
		stopped(it->type, &st);
		if (ex) {
			ENUM_SERVICE_STATUS_PROCESSW *x = (void *)e;
			x->lpServiceName = pn;
			x->lpDisplayName = pd;
			memset(&x->ServiceStatusProcess, 0,
			       sizeof(x->ServiceStatusProcess));
			memcpy(&x->ServiceStatusProcess, &st, sizeof(st));
		} else {
			ENUM_SERVICE_STATUSW *x = (void *)e;
			x->lpServiceName = pn;
			x->lpDisplayName = pd;
			x->ServiceStatus = st;
		}
	}
	for (size_t i = 0; i < n; i++) {
		free(items[i].name);
		free(items[i].display);
	}
	free(items);
	*count = given;
	if (more) {
		*needed = rest;
		if (resume)
			*resume = skip + given;
		win_set_error(ERROR_MORE_DATA);
		return FALSE;
	}
	if (resume)
		*resume = 0;
	return TRUE;
}

WINFN(BOOL) adv_EnumServicesStatusA(SC_HANDLE scm, DWORD types, DWORD state,
				    LPENUM_SERVICE_STATUSA buf, DWORD size,
				    LPDWORD needed, LPDWORD count,
				    LPDWORD resume)
{
	return enum_services(scm, types, state, (BYTE *)buf, size, needed,
			     count, resume, 0, 0, NULL);
}

WINFN(BOOL) adv_EnumServicesStatusW(SC_HANDLE scm, DWORD types, DWORD state,
				    LPENUM_SERVICE_STATUSW buf, DWORD size,
				    LPDWORD needed, LPDWORD count,
				    LPDWORD resume)
{
	return enum_services(scm, types, state, (BYTE *)buf, size, needed,
			     count, resume, 1, 0, NULL);
}

static BOOL enum_ex(SC_HANDLE scm, DWORD level, DWORD types, DWORD state,
		    BYTE *buf, DWORD size, LPDWORD needed, LPDWORD count,
		    LPDWORD resume, const void *group, int wide)
{
	if (level != SC_ENUM_PROCESS_INFO) {
		win_set_error(ERROR_INVALID_LEVEL);
		return FALSE;
	}
	char *g = in_str(group, wide);
	BOOL r = enum_services(scm, types, state, buf, size, needed, count,
			       resume, wide, 1, g);
	free(g);
	return r;
}

WINFN(BOOL) adv_EnumServicesStatusExA(SC_HANDLE scm, DWORD level, DWORD types,
				      DWORD state, LPBYTE buf, DWORD size,
				      LPDWORD needed, LPDWORD count,
				      LPDWORD resume, LPCSTR group)
{
	return enum_ex(scm, level, types, state, buf, size, needed, count,
		       resume, group, 0);
}

WINFN(BOOL) adv_EnumServicesStatusExW(SC_HANDLE scm, DWORD level, DWORD types,
				      DWORD state, LPBYTE buf, DWORD size,
				      LPDWORD needed, LPDWORD count,
				      LPDWORD resume, LPCWSTR group)
{
	return enum_ex(scm, level, types, state, buf, size, needed, count,
		       resume, group, 1);
}

/* The database lock: one holder at a time, a token to give back. */
#define DB_LOCK ((SC_LOCK)(ULONG_PTR)0x5c10c)

WINFN(SC_LOCK) adv_LockServiceDatabase(SC_HANDLE scm)
{
	struct scm *m = scm_ref(scm);
	if (!m)
		return NULL;
	DWORD access = m->access;
	kobj_unref(&m->obj);
	if (!(access & SC_MANAGER_LOCK)) {
		win_set_error(ERROR_ACCESS_DENIED);
		return NULL;
	}
	pthread_mutex_lock(&svc_lock);
	int was = db_locked;
	db_locked = 1;
	pthread_mutex_unlock(&svc_lock);
	if (was) {
		win_set_error(ERROR_SERVICE_DATABASE_LOCKED);
		return NULL;
	}
	return DB_LOCK;
}

WINFN(BOOL) adv_UnlockServiceDatabase(SC_LOCK lock)
{
	pthread_mutex_lock(&svc_lock);
	int ok = lock == DB_LOCK && db_locked;
	if (ok)
		db_locked = 0;
	pthread_mutex_unlock(&svc_lock);
	if (!ok)
		win_set_error(ERROR_INVALID_SERVICE_LOCK);
	return ok;
}

/* The service side: this process was not started as a service. */

WINFN(BOOL) adv_StartServiceCtrlDispatcherA(const SERVICE_TABLE_ENTRYA *t)
{
	win_set_error(ERROR_FAILED_SERVICE_CONTROLLER_CONNECT);
	return FALSE;
}

WINFN(BOOL) adv_StartServiceCtrlDispatcherW(const SERVICE_TABLE_ENTRYW *t)
{
	win_set_error(ERROR_FAILED_SERVICE_CONTROLLER_CONNECT);
	return FALSE;
}

static SERVICE_STATUS_HANDLE no_handler(void)
{
	win_set_error(ERROR_SERVICE_DOES_NOT_EXIST);
	return NULL;
}

WINFN(SERVICE_STATUS_HANDLE) adv_RegisterServiceCtrlHandlerA(LPCSTR n, void *f)
{
	return no_handler();
}

WINFN(SERVICE_STATUS_HANDLE) adv_RegisterServiceCtrlHandlerW(LPCWSTR n, void *f)
{
	return no_handler();
}

WINFN(SERVICE_STATUS_HANDLE) adv_RegisterServiceCtrlHandlerExA(LPCSTR n,
							       void *f, void *c)
{
	return no_handler();
}

WINFN(SERVICE_STATUS_HANDLE) adv_RegisterServiceCtrlHandlerExW(LPCWSTR n,
							       void *f, void *c)
{
	return no_handler();
}

WINFN(BOOL) adv_SetServiceStatus(SERVICE_STATUS_HANDLE h, LPSERVICE_STATUS st)
{
	win_set_error(ERROR_INVALID_HANDLE);
	return FALSE;
}

/* The event log. */

struct log_record {
	BYTE *rec;		/* an EVENTLOGRECORD, W form */
	DWORD size;
};

struct event_log {
	char *name;
	struct log_record *recs;
	size_t n, cap;
	DWORD first;		/* the oldest record's number */
};

static pthread_mutex_t log_lock = PTHREAD_MUTEX_INITIALIZER;
static struct event_log *logs;
static size_t nlogs;

/* The log called name, made on first use (with log_lock held). */
static struct event_log *get_log(const char *name)
{
	for (size_t i = 0; i < nlogs; i++)
		if (!strcasecmp(logs[i].name, name))
			return &logs[i];
	struct event_log *p = realloc(logs, (nlogs + 1) * sizeof(*p));
	char *n = strdup(name);
	if (!p || !n) {
		if (p)
			logs = p;
		free(n);
		return NULL;
	}
	logs = p;
	memset(&logs[nlogs], 0, sizeof(logs[nlogs]));
	logs[nlogs].name = n;
	logs[nlogs].first = 1;
	return &logs[nlogs++];
}

struct log_handle {
	struct kobject obj;
	char *log;		/* the log's name */
	char *source;		/* a source's name, NULL for a reader */
	DWORD next;		/* the next record to read sequentially */
	int dir;		/* 1 forwards, -1 backwards, 0 not yet read */
};

static void log_destroy(struct kobject *o)
{
	struct log_handle *l = (struct log_handle *)o;
	free(l->log);
	free(l->source);
	free(l);
}

/* The log the registry places source in, else Application. */
static char *source_log(const char *source)
{
	char *found = NULL;

	reg_lock();
	struct reg_key *root = reg_open(EVENTLOG_KEY);
	for (size_t i = 0; root && !found && i < root->nsub; i++) {
		struct reg_key *l = root->sub[i];
		for (size_t j = 0; j < l->nsub; j++)
			if (!strcasecmp(l->sub[j]->name, source)) {
				found = strdup(l->name);
				break;
			}
	}
	reg_unlock();
	return found ? found : strdup("Application");
}

/* A known log (Application, System, Security, or one in the registry). */
static char *log_named(const char *name)
{
	static const char *const builtin[] = { "Application", "System",
					       "Security" };
	for (size_t i = 0; i < 3; i++)
		if (!strcasecmp(name, builtin[i]))
			return strdup(builtin[i]);
	char *found = NULL;
	reg_lock();
	struct reg_key *root = reg_open(EVENTLOG_KEY);
	for (size_t i = 0; root && i < root->nsub; i++)
		if (!strcasecmp(root->sub[i]->name, name))
			found = strdup(root->sub[i]->name);
	reg_unlock();
	return found ? found : strdup("Application");
}

static HANDLE open_log(const void *server, const void *name, int wide,
		       int source)
{
	char *srv = in_str(server, wide), *n = in_str(name, wide);
	int remote = srv && *srv;

	free(srv);
	if (remote) {
		free(n);
		win_set_error(ERROR_ACCESS_DENIED);
		return NULL;
	}
	if (!n || !*n) {
		free(n);
		win_set_error(ERROR_INVALID_PARAMETER);
		return NULL;
	}
	struct log_handle *l = calloc(1, sizeof(*l));
	if (!l) {
		free(n);
		win_set_error(ERROR_NOT_ENOUGH_MEMORY);
		return NULL;
	}
	kobj_init(&l->obj, KOBJ_EVENTLOG, log_destroy);
	l->log = source ? source_log(n) : log_named(n);
	if (source)
		l->source = n;
	else
		free(n);
	if (!l->log) {
		kobj_unref(&l->obj);
		win_set_error(ERROR_NOT_ENOUGH_MEMORY);
		return NULL;
	}
	HANDLE h = handle_new(&l->obj);
	if (!h)
		win_set_error(ERROR_NOT_ENOUGH_MEMORY);
	return h;
}

WINFN(HANDLE) adv_RegisterEventSourceA(LPCSTR server, LPCSTR source)
{
	return open_log(server, source, 0, 1);
}

WINFN(HANDLE) adv_RegisterEventSourceW(LPCWSTR server, LPCWSTR source)
{
	return open_log(server, source, 1, 1);
}

WINFN(HANDLE) adv_OpenEventLogA(LPCSTR server, LPCSTR name)
{
	return open_log(server, name, 0, 0);
}

WINFN(HANDLE) adv_OpenEventLogW(LPCWSTR server, LPCWSTR name)
{
	return open_log(server, name, 1, 0);
}

static struct log_handle *log_ref(HANDLE h)
{
	return (struct log_handle *)handle_ref(h, KOBJ_EVENTLOG);
}

static BOOL close_log(HANDLE h)
{
	struct log_handle *l = log_ref(h);
	if (!l)
		return FALSE;
	kobj_unref(&l->obj);
	return handle_close(h);
}

WINFN(BOOL) adv_DeregisterEventSource(HANDLE h)
{
	return close_log(h);
}

WINFN(BOOL) adv_CloseEventLog(HANDLE h)
{
	return close_log(h);
}

static void put_w(BYTE *rec, DWORD *at, const char *s)
{
	WCHAR *w = utf8_to_utf16_dup(s ? s : "");
	size_t n = w ? wstrlen(w) + 1 : 1;
	if (w)
		memcpy(rec + *at, w, n * sizeof(WCHAR));
	else
		memset(rec + *at, 0, sizeof(WCHAR));
	*at += (DWORD)(n * sizeof(WCHAR));
	free(w);
}

static size_t w_size(const char *s)
{
	WCHAR *w = utf8_to_utf16_dup(s ? s : "");
	size_t n = w ? wstrlen(w) + 1 : 1;
	free(w);
	return n * sizeof(WCHAR);
}

/* The computer's name, as GetComputerName gives it. */
static const char *machine_name(void)
{
	static char name[64];
	if (!name[0]) {
		BOOL (WINAPI *get)(LPSTR, LPDWORD) =
			builtin_export(&builtin_kernel32, "GetComputerNameA");
		DWORD n = sizeof(name);
		if (!get || !get(name, &n))
			strcpy(name, "SEANCE");
	}
	return name;
}

static BOOL report(HANDLE h, WORD type, WORD cat, DWORD id, PSID sid,
		   WORD nstrings, DWORD size, const void *const *strings,
		   const void *data, int wide)
{
	struct log_handle *l = log_ref(h);
	if (!l)
		return FALSE;
	if ((nstrings && !strings) || (size && !data)) {
		kobj_unref(&l->obj);
		win_set_error(ERROR_INVALID_PARAMETER);
		return FALSE;
	}
	char **str = calloc(nstrings + 1, sizeof(*str));
	if (!str) {
		kobj_unref(&l->obj);
		win_set_error(ERROR_NOT_ENOUGH_MEMORY);
		return FALSE;
	}
	for (WORD i = 0; i < nstrings; i++)
		str[i] = in_str(strings[i] ? strings[i] : (wide ? (const void *)L"" : ""), wide);

	const char *source = l->source ? l->source : l->log;
	DWORD sidlen = sid ? 8 + 4 * ((SID *)sid)->SubAuthorityCount : 0;
	size_t len = sizeof(EVENTLOGRECORD) + w_size(source) +
		     w_size(machine_name());
	len = (len + 3) & ~(size_t)3;
	size_t sid_at = len;
	len += sidlen;
	size_t str_at = len;
	for (WORD i = 0; i < nstrings; i++)
		len += w_size(str[i]);
	size_t data_at = len;
	len += size;
	len = (len + 3) & ~(size_t)3;
	len += 4;

	BYTE *rec = calloc(1, len);
	BOOL ok = FALSE;
	if (rec) {
		EVENTLOGRECORD *r = (EVENTLOGRECORD *)rec;
		DWORD now = (DWORD)win_unix_now();
		DWORD at = sizeof(*r);
		r->Length = (DWORD)len;
		r->Reserved = 0x654c664c;	/* "LfLe" */
		r->TimeGenerated = r->TimeWritten = now;
		r->EventID = id;
		r->EventType = type;
		r->NumStrings = nstrings;
		r->EventCategory = cat;
		put_w(rec, &at, source);
		put_w(rec, &at, machine_name());
		r->UserSidLength = sidlen;
		r->UserSidOffset = (DWORD)sid_at;
		if (sidlen)
			memcpy(rec + sid_at, sid, sidlen);
		r->StringOffset = (DWORD)str_at;
		at = (DWORD)str_at;
		for (WORD i = 0; i < nstrings; i++)
			put_w(rec, &at, str[i]);
		r->DataLength = size;
		r->DataOffset = (DWORD)data_at;
		if (size)
			memcpy(rec + data_at, data, size);
		memcpy(rec + len - 4, &r->Length, 4);

		pthread_mutex_lock(&log_lock);
		struct event_log *lg = get_log(l->log);
		if (lg && lg->n == lg->cap) {
			size_t cap = lg->cap ? lg->cap * 2 : 16;
			struct log_record *p = realloc(lg->recs, cap * sizeof(*p));
			if (p) {
				lg->recs = p;
				lg->cap = cap;
			}
		}
		if (lg && lg->n < lg->cap) {
			r->RecordNumber = lg->first + (DWORD)lg->n;
			lg->recs[lg->n].rec = rec;
			lg->recs[lg->n].size = (DWORD)len;
			lg->n++;
			ok = TRUE;
		}
		pthread_mutex_unlock(&log_lock);
	}
	if (ok) {
		vfw_trace("event log %s: %s type %u category %u id 0x%lx%s%s",
			  l->log, source, type, cat, (unsigned long)id,
			  nstrings ? ": " : "", nstrings ? str[0] : "");
	} else {
		free(rec);
		win_set_error(ERROR_NOT_ENOUGH_MEMORY);
	}
	for (WORD i = 0; i < nstrings; i++)
		free(str[i]);
	free(str);
	kobj_unref(&l->obj);
	return ok;
}

WINFN(BOOL) adv_ReportEventA(HANDLE h, WORD type, WORD cat, DWORD id, PSID sid,
			     WORD nstrings, DWORD size, LPCSTR *strings,
			     LPVOID data)
{
	return report(h, type, cat, id, sid, nstrings, size,
		      (const void *const *)strings, data, 0);
}

WINFN(BOOL) adv_ReportEventW(HANDLE h, WORD type, WORD cat, DWORD id, PSID sid,
			     WORD nstrings, DWORD size, LPCWSTR *strings,
			     LPVOID data)
{
	return report(h, type, cat, id, sid, nstrings, size,
		      (const void *const *)strings, data, 1);
}

/* The A form of a W record, malloc()ed, its length in *len. */
static BYTE *record_a(const BYTE *w, DWORD *len)
{
	const EVENTLOGRECORD *r = (const EVENTLOGRECORD *)w;
	const WCHAR *src = (const WCHAR *)(w + sizeof(*r));
	const WCHAR *comp = src + wstrlen(src) + 1;
	char *s = utf16_to_utf8_dup(src), *c = utf16_to_utf8_dup(comp);
	char *as = utf8_to_acp_dup(s ? s : ""), *ac = utf8_to_acp_dup(c ? c : "");
	char **str = calloc(r->NumStrings + 1, sizeof(*str));
	const WCHAR *p = (const WCHAR *)(w + r->StringOffset);
	size_t n = sizeof(*r) + (as ? strlen(as) : 0) + 1 +
		   (ac ? strlen(ac) : 0) + 1;
	BYTE *out = NULL;

	free(s);
	free(c);
	if (!str || !as || !ac)
		goto done;
	for (WORD i = 0; i < r->NumStrings; i++) {
		char *u = utf16_to_utf8_dup(p);
		str[i] = utf8_to_acp_dup(u ? u : "");
		free(u);
		p += wstrlen(p) + 1;
	}
	n = (n + 3) & ~(size_t)3;
	size_t sid_at = n;
	n += r->UserSidLength;
	size_t str_at = n;
	for (WORD i = 0; i < r->NumStrings; i++)
		n += (str[i] ? strlen(str[i]) : 0) + 1;
	size_t data_at = n;
	n += r->DataLength;
	n = (n + 3) & ~(size_t)3;
	n += 4;
	out = calloc(1, n);
	if (!out)
		goto done;
	EVENTLOGRECORD *o = (EVENTLOGRECORD *)out;
	*o = *r;
	o->Length = (DWORD)n;
	size_t at = sizeof(*o);
	strcpy((char *)out + at, as);
	at += strlen(as) + 1;
	strcpy((char *)out + at, ac);
	o->UserSidOffset = (DWORD)sid_at;
	memcpy(out + sid_at, w + r->UserSidOffset, r->UserSidLength);
	o->StringOffset = (DWORD)str_at;
	at = str_at;
	for (WORD i = 0; i < r->NumStrings; i++) {
		const char *t = str[i] ? str[i] : "";
		strcpy((char *)out + at, t);
		at += strlen(t) + 1;
	}
	o->DataOffset = (DWORD)data_at;
	memcpy(out + data_at, w + r->DataOffset, r->DataLength);
	memcpy(out + n - 4, &o->Length, 4);
	*len = (DWORD)n;
done:
	for (WORD i = 0; str && i < r->NumStrings; i++)
		free(str[i]);
	free(str);
	free(as);
	free(ac);
	return out;
}

static BOOL read_log(HANDLE h, DWORD flags, DWORD offset, void *buf,
		     DWORD size, DWORD *read, DWORD *needed, int wide)
{
	struct log_handle *l = log_ref(h);
	if (!l)
		return FALSE;
	int seq = flags & EVENTLOG_SEQUENTIAL_READ, seek = flags & EVENTLOG_SEEK_READ;
	int fwd = flags & EVENTLOG_FORWARDS_READ, back = flags & EVENTLOG_BACKWARDS_READ;
	if (!read || !needed || !buf || seq == !!seek || !fwd == !back) {
		kobj_unref(&l->obj);
		win_set_error(ERROR_INVALID_PARAMETER);
		return FALSE;
	}
	*read = *needed = 0;
	int dir = fwd ? 1 : -1;
	BOOL ok = FALSE;

	pthread_mutex_lock(&log_lock);
	struct event_log *lg = get_log(l->log);
	DWORD first = lg ? lg->first : 1, last = lg ? lg->first + (DWORD)lg->n : 1;
	DWORD at = seek ? offset : l->dir ? l->next : dir > 0 ? first : last - 1;
	if (!lg || !lg->n || at < first || at >= last) {
		win_set_error(seek && lg && lg->n ? ERROR_INVALID_PARAMETER :
			      ERROR_HANDLE_EOF);
		goto out;
	}
	/* As many whole records as fit, at least the first or nothing. */
	DWORD used = 0;
	for (DWORD k = at; k >= first && k < last; k += dir) {
		struct log_record *rc = &lg->recs[k - first];
		DWORD len = rc->size;
		BYTE *a = NULL;
		if (!wide && !(a = record_a(rc->rec, &len))) {
			win_set_error(ERROR_NOT_ENOUGH_MEMORY);
			break;
		}
		if (used + len > size) {
			free(a);
			if (!used) {
				*needed = len;
				win_set_error(ERROR_INSUFFICIENT_BUFFER);
			}
			break;
		}
		memcpy((BYTE *)buf + used, a ? a : rc->rec, len);
		free(a);
		used += len;
		l->next = k + dir;
		l->dir = dir;
		ok = TRUE;
		if (k == first && dir < 0)
			break;
	}
	*read = used;
out:
	pthread_mutex_unlock(&log_lock);
	kobj_unref(&l->obj);
	return ok;
}

WINFN(BOOL) adv_ReadEventLogA(HANDLE h, DWORD flags, DWORD offset, LPVOID buf,
			      DWORD size, DWORD *read, DWORD *needed)
{
	return read_log(h, flags, offset, buf, size, read, needed, 0);
}

WINFN(BOOL) adv_ReadEventLogW(HANDLE h, DWORD flags, DWORD offset, LPVOID buf,
			      DWORD size, DWORD *read, DWORD *needed)
{
	return read_log(h, flags, offset, buf, size, read, needed, 1);
}

static BOOL log_count(HANDLE h, PDWORD out, int oldest)
{
	struct log_handle *l = log_ref(h);
	if (!l)
		return FALSE;
	if (!out) {
		kobj_unref(&l->obj);
		win_set_error(ERROR_INVALID_PARAMETER);
		return FALSE;
	}
	pthread_mutex_lock(&log_lock);
	struct event_log *lg = get_log(l->log);
	*out = !lg ? 0 : oldest ? (lg->n ? lg->first : 0) : (DWORD)lg->n;
	pthread_mutex_unlock(&log_lock);
	kobj_unref(&l->obj);
	return TRUE;
}

WINFN(BOOL) adv_GetNumberOfEventLogRecords(HANDLE h, PDWORD n)
{
	return log_count(h, n, 0);
}

WINFN(BOOL) adv_GetOldestEventLogRecord(HANDLE h, PDWORD n)
{
	return log_count(h, n, 1);
}

/* Clearing keeps the numbering going; a backup can't be written here. */
static BOOL clear_log(HANDLE h, const void *backup, int wide)
{
	struct log_handle *l = log_ref(h);
	if (!l)
		return FALSE;
	char *b = in_str(backup, wide);
	int want_backup = b && *b;
	free(b);
	if (want_backup) {
		kobj_unref(&l->obj);
		win_set_error(ERROR_ACCESS_DENIED);
		return FALSE;
	}
	pthread_mutex_lock(&log_lock);
	struct event_log *lg = get_log(l->log);
	if (lg) {
		for (size_t i = 0; i < lg->n; i++)
			free(lg->recs[i].rec);
		lg->first += (DWORD)lg->n;
		lg->n = 0;
	}
	pthread_mutex_unlock(&log_lock);
	kobj_unref(&l->obj);
	return TRUE;
}

WINFN(BOOL) adv_ClearEventLogA(HANDLE h, LPCSTR backup)
{
	return clear_log(h, backup, 0);
}

WINFN(BOOL) adv_ClearEventLogW(HANDLE h, LPCWSTR backup)
{
	return clear_log(h, backup, 1);
}

#define E(name) { #name, 0, (void *)adv_##name }

const struct builtin_export adv_service_exports[] = {
	E(ChangeServiceConfigA),
	E(ChangeServiceConfigW),
	E(ClearEventLogA),
	E(ClearEventLogW),
	E(CloseEventLog),
	E(CloseServiceHandle),
	E(ControlService),
	E(CreateServiceA),
	E(CreateServiceW),
	E(DeleteService),
	E(DeregisterEventSource),
	E(EnumServicesStatusA),
	E(EnumServicesStatusExA),
	E(EnumServicesStatusExW),
	E(EnumServicesStatusW),
	E(GetNumberOfEventLogRecords),
	E(GetOldestEventLogRecord),
	E(GetServiceDisplayNameA),
	E(GetServiceDisplayNameW),
	E(GetServiceKeyNameA),
	E(GetServiceKeyNameW),
	E(LockServiceDatabase),
	E(OpenEventLogA),
	E(OpenEventLogW),
	E(OpenSCManagerA),
	E(OpenSCManagerW),
	E(OpenServiceA),
	E(OpenServiceW),
	E(QueryServiceConfigA),
	E(QueryServiceConfigW),
	E(QueryServiceStatus),
	E(QueryServiceStatusEx),
	E(ReadEventLogA),
	E(ReadEventLogW),
	E(RegisterEventSourceA),
	E(RegisterEventSourceW),
	E(RegisterServiceCtrlHandlerA),
	E(RegisterServiceCtrlHandlerExA),
	E(RegisterServiceCtrlHandlerExW),
	E(RegisterServiceCtrlHandlerW),
	E(ReportEventA),
	E(ReportEventW),
	E(SetServiceStatus),
	E(StartServiceA),
	E(StartServiceCtrlDispatcherA),
	E(StartServiceCtrlDispatcherW),
	E(StartServiceW),
	E(UnlockServiceDatabase),
	BUILTIN_END
};
