#include <errno.h>
#include <pthread.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "win32/handle.h"
#include "win32/internal.h"

#define MAX_HANDLES 65536

/* Handle values are 4, 8, 12, ...: slot i is handle (i + 1) * 4. */
static struct kobject **table;
static size_t table_size;
static pthread_mutex_t table_lock = PTHREAD_MUTEX_INITIALIZER;

/* Named objects. */
static struct named {
	struct named *next;
	struct kobject *obj;
} *names;

static pthread_mutex_t wait_mutex = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t wait_cond;
static pthread_once_t wait_once = PTHREAD_ONCE_INIT;

static void wait_init(void)
{
	pthread_condattr_t attr;

	pthread_condattr_init(&attr);
	pthread_condattr_setclock(&attr, CLOCK_MONOTONIC);
	pthread_cond_init(&wait_cond, &attr);
	pthread_condattr_destroy(&attr);
}

void kobj_init(struct kobject *obj, enum kobj_type type,
	       void (*destroy)(struct kobject *obj))
{
	memset(obj, 0, sizeof(*obj));
	obj->type = type;
	obj->refcount = 1;
	obj->destroy = destroy;
}

void kobj_ref(struct kobject *obj)
{
	__atomic_add_fetch(&obj->refcount, 1, __ATOMIC_SEQ_CST);
}

void kobj_unref(struct kobject *obj)
{
	if (!obj || __atomic_sub_fetch(&obj->refcount, 1, __ATOMIC_SEQ_CST))
		return;

	pthread_mutex_lock(&table_lock);
	for (struct named **p = &names; *p; p = &(*p)->next) {
		if ((*p)->obj == obj) {
			struct named *n = *p;
			*p = n->next;
			free(n);
			break;
		}
	}
	pthread_mutex_unlock(&table_lock);

	free(obj->name);
	if (obj->destroy)
		obj->destroy(obj);
	else
		free(obj);
}

HANDLE handle_new(struct kobject *obj)
{
	HANDLE h = NULL;

	pthread_mutex_lock(&table_lock);
	size_t i;
	for (i = 0; i < table_size && table[i]; i++)
		;
	if (i == table_size && table_size < MAX_HANDLES) {
		size_t n = table_size ? table_size * 2 : 64;
		struct kobject **t = realloc(table, n * sizeof(*t));
		if (t) {
			memset(t + table_size, 0, (n - table_size) * sizeof(*t));
			table = t;
			table_size = n;
		}
	}
	if (i < table_size) {
		table[i] = obj;
		h = (HANDLE)(uintptr_t)((i + 1) * 4);
	}
	pthread_mutex_unlock(&table_lock);

	if (!h) {
		win_set_error(ERROR_TOO_MANY_OPEN_FILES);
		kobj_unref(obj);
	}
	return h;
}

/* The slot for h, or -1. Call with table_lock held. */
static long slot_of(HANDLE h)
{
	uintptr_t v = (uintptr_t)h;

	if (!v || v % 4 || v / 4 > table_size)
		return -1;
	return table[v / 4 - 1] ? (long)(v / 4 - 1) : -1;
}

struct kobject *handle_ref(HANDLE h, enum kobj_type type)
{
	struct kobject *obj = NULL;

	pthread_mutex_lock(&table_lock);
	long i = slot_of(h);
	if (i >= 0 && (type == KOBJ_ANY || table[i]->type == type)) {
		obj = table[i];
		kobj_ref(obj);
	}
	pthread_mutex_unlock(&table_lock);

	if (!obj)
		win_set_error(ERROR_INVALID_HANDLE);
	return obj;
}

BOOL handle_close(HANDLE h)
{
	struct kobject *obj = NULL;

	pthread_mutex_lock(&table_lock);
	long i = slot_of(h);
	if (i >= 0) {
		obj = table[i];
		table[i] = NULL;
	}
	pthread_mutex_unlock(&table_lock);

	if (!obj) {
		win_set_error(ERROR_INVALID_HANDLE);
		return FALSE;
	}
	kobj_unref(obj);
	return TRUE;
}

struct kobject *kobj_find_named(const char *name)
{
	struct kobject *found = NULL;

	pthread_mutex_lock(&table_lock);
	for (struct named *n = names; n && !found; n = n->next) {
		if (strcmp(n->obj->name, name))
			continue;
		/* Only while it is alive: it may be mid-destruction. */
		int c = __atomic_load_n(&n->obj->refcount, __ATOMIC_SEQ_CST);
		while (c > 0 &&
		       !__atomic_compare_exchange_n(&n->obj->refcount, &c, c + 1, 0,
						    __ATOMIC_SEQ_CST,
						    __ATOMIC_SEQ_CST))
			;
		if (c > 0)
			found = n->obj;
	}
	pthread_mutex_unlock(&table_lock);
	return found;
}

void kobj_set_name(struct kobject *obj, const char *name)
{
	struct named *n = malloc(sizeof(*n));

	if (!n || !(obj->name = strdup(name))) {
		free(n);
		return;
	}
	n->obj = obj;
	pthread_mutex_lock(&table_lock);
	n->next = names;
	names = n;
	pthread_mutex_unlock(&table_lock);
}

void wait_lock(void)
{
	pthread_once(&wait_once, wait_init);
	pthread_mutex_lock(&wait_mutex);
}

void wait_unlock(void)
{
	pthread_mutex_unlock(&wait_mutex);
}

void wait_broadcast(void)
{
	pthread_cond_broadcast(&wait_cond);
}

DWORD wait_objects(DWORD n, struct kobject *const *objs, BOOL all,
		   DWORD timeout_ms)
{
	DWORD tid = teb_current() ? teb_current()->UniqueThread : 0;
	struct timespec deadline;
	DWORD result = WAIT_TIMEOUT;

	for (DWORD i = 0; i < n; i++) {
		if (!objs[i]->signaled) {
			win_set_error(ERROR_INVALID_HANDLE);
			return WAIT_FAILED;
		}
	}
	if (timeout_ms != INFINITE) {
		clock_gettime(CLOCK_MONOTONIC, &deadline);
		deadline.tv_sec += timeout_ms / 1000;
		deadline.tv_nsec += (timeout_ms % 1000) * 1000000L;
		if (deadline.tv_nsec >= 1000000000L) {
			deadline.tv_sec++;
			deadline.tv_nsec -= 1000000000L;
		}
	}

	wait_lock();
	for (;;) {
		if (all) {
			DWORD ready = 0;
			for (DWORD i = 0; i < n; i++)
				ready += objs[i]->signaled(objs[i], tid) != 0;
			if (ready == n) {
				for (DWORD i = 0; i < n; i++)
					if (objs[i]->acquire)
						objs[i]->acquire(objs[i], tid);
				result = WAIT_OBJECT_0;
				break;
			}
		} else {
			DWORD i;
			for (i = 0; i < n; i++)
				if (objs[i]->signaled(objs[i], tid))
					break;
			if (i < n) {
				if (objs[i]->acquire)
					objs[i]->acquire(objs[i], tid);
				result = WAIT_OBJECT_0 + i;
				break;
			}
		}
		if (timeout_ms == 0)
			break;
		int r = timeout_ms == INFINITE ?
			pthread_cond_wait(&wait_cond, &wait_mutex) :
			pthread_cond_timedwait(&wait_cond, &wait_mutex, &deadline);
		if (r == ETIMEDOUT) {
			result = WAIT_TIMEOUT;
			break;
		}
	}
	wait_unlock();
	return result;
}
