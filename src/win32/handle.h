/*
 * Kernel objects behind Win32 HANDLEs, and waiting on them.
 *
 * Every object has a type and a reference count; each open handle holds
 * one reference. Waitable objects (events, mutexes, semaphores, threads)
 * keep their signal state under one global wait lock, and waiters sleep on
 * one condition variable that is broadcast whenever any state changes.
 * That is not the fastest design, but it makes WaitForMultipleObjects easy
 * to get right, and codecs wait rarely.
 */
#ifndef VFW_WIN32_HANDLE_H
#define VFW_WIN32_HANDLE_H

#include "win32.h"

enum kobj_type {
	KOBJ_ANY = 0,
	KOBJ_FILE,
	KOBJ_EVENT,
	KOBJ_MUTEX,
	KOBJ_SEMAPHORE,
	KOBJ_THREAD,
	KOBJ_MAPPING,
	KOBJ_FIND,
	KOBJ_KEY,
};

struct kobject {
	enum kobj_type type;
	int refcount;
	char *name;		/* for named objects (CreateEvent("x") etc.) */
	void (*destroy)(struct kobject *obj);

	/* Waitable objects; both are called with the wait lock held. */
	int (*signaled)(struct kobject *obj, DWORD tid);
	void (*acquire)(struct kobject *obj, DWORD tid);
};

/* Initialises the common fields; refcount starts at 1. */
void kobj_init(struct kobject *obj, enum kobj_type type,
	       void (*destroy)(struct kobject *obj));

/* Adds a handle for obj, taking over one reference. NULL if full. */
HANDLE handle_new(struct kobject *obj);

/*
 * The object behind h with a new reference, or NULL (and
 * ERROR_INVALID_HANDLE) if h is not an open handle of that type.
 */
struct kobject *handle_ref(HANDLE h, enum kobj_type type);

void kobj_ref(struct kobject *obj);
void kobj_unref(struct kobject *obj);

BOOL handle_close(HANDLE h);

/*
 * Named objects share one namespace, like Windows' per-session one. Returns
 * a new reference to the object called name, or NULL.
 */
struct kobject *kobj_find_named(const char *name);
/* Names obj; the name is dropped when the object is destroyed. */
void kobj_set_name(struct kobject *obj, const char *name);

/* The wait lock, held while changing any waitable object's state. */
void wait_lock(void);
void wait_unlock(void);
/* Wakes waiters after a state change (call with the lock held). */
void wait_broadcast(void);

/*
 * Waits for one (all == FALSE) or all of objs, like WaitForMultipleObjects.
 * Returns WAIT_OBJECT_0 + i, WAIT_TIMEOUT or WAIT_FAILED.
 */
DWORD wait_objects(DWORD n, struct kobject *const *objs, BOOL all,
		   DWORD timeout_ms);

#endif /* VFW_WIN32_HANDLE_H */
