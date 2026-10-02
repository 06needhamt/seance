/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * msvcrt: CRT startup support a dynamically linked codec's compiler-
 * generated init code calls before/after its own DllMain, plus the small
 * odds and ends (_lock/_unlock, pointer "encoding", _fmode/_commode) nothing
 * else here needs but a codec's own CRT-linked code may still reference.
 */
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>

#include "loader/modules.h"
#include "runtime/builtin.h"
#include "runtime/seh.h"
#include "win32/internal.h"

#define CRTFN(ret) static ret STUB_CDECL

typedef void (STUB_CDECL *pfv)(void);
typedef int (STUB_CDECL *pfv_e)(void);

/* _initterm/_initterm_e: call each non-NULL entry of a compiler-emitted
 * table of static-initialiser/atexit-registration function pointers. */
CRTFN(void) crt__initterm(pfv *first, pfv *last)
{
	for (; first < last; first++)
		if (*first)
			(*first)();
}

CRTFN(int) crt__initterm_e(pfv_e *first, pfv_e *last)
{
	for (; first < last; first++)
		if (*first) {
			int r = (*first)();
			if (r)
				return r;
		}
	return 0;
}

/*
 * atexit/_onexit: registered functions run when the harness process itself
 * exits (there is no per-DLL "CRT exit" moment separate from that here),
 * through one real atexit() call of our own that walks the table.
 */
#define MAX_ONEXIT 64
static pfv onexit_funcs[MAX_ONEXIT];
static int onexit_count;
static pthread_mutex_t onexit_lock = PTHREAD_MUTEX_INITIALIZER;

static void run_onexit(void)
{
	pthread_mutex_lock(&onexit_lock);
	for (int i = onexit_count - 1; i >= 0; i--)
		/*
		 * The module that registered this may have been mod_free()d
		 * since (a codec unloaded mid-run, say by `probe` moving on
		 * to the next one) - calling into memory that's no longer
		 * mapped would crash the whole harness at process exit, so
		 * skip anything whose address isn't inside a module that's
		 * still loaded. This is the same hazard FreeLibrary-before-
		 * exit is on real Windows; there just being no unregistering
		 * it back out.
		 */
		if (onexit_funcs[i] && mod_from_address((void *)onexit_funcs[i]))
			onexit_funcs[i]();
	pthread_mutex_unlock(&onexit_lock);
}

static int register_onexit(pfv f)
{
	static int installed;
	int ok = 0;

	pthread_mutex_lock(&onexit_lock);
	if (!installed) {
		atexit(run_onexit);
		installed = 1;
	}
	if (onexit_count < MAX_ONEXIT) {
		onexit_funcs[onexit_count++] = f;
		ok = 1;
	}
	pthread_mutex_unlock(&onexit_lock);
	return ok;
}

CRTFN(int) crt_atexit(void (STUB_CDECL *func)(void))
{
	return register_onexit(func) ? 0 : -1;
}

/* _onexit_t is int(__cdecl*)(void); atexit's table only calls void(void)
 * functions, so each registration gets its own forwarding is unnecessary -
 * the return value a real _onexit callback gives back is defined to be
 * ignored by the CRT's own exit-time walk anyway, so calling it as void
 * through the same pointer type is safe. */
CRTFN(void *) crt__onexit(void *func)
{
	return func && register_onexit((pfv)func) ? func : NULL;
}

/* __dllonexit: a DLL-specific variant taking the caller's own exit-table
 * bounds; those exist for a CRT that keeps its own per-DLL table walked at
 * unload, which this harness doesn't model, so it's folded into the same
 * one process-exit table as _onexit. */
CRTFN(void *) crt___dllonexit(void *func, void *start, void *end)
{
	(void)start;
	(void)end;
	return crt__onexit(func);
}

/*
 * _encode_pointer/_decode_pointer exist to make a stored function pointer
 * (an exception handler, mostly) harder to forge from outside the process;
 * since encode and decode always run in the same process here, the
 * identity function satisfies every real caller without implementing the
 * obfuscation itself.
 */
CRTFN(void *) crt__encode_pointer(void *p) { return p; }
CRTFN(void *) crt__decode_pointer(void *p) { return p; }
CRTFN(void *) crt__encoded_null(void) { return NULL; }

/* The CRT heap, under the names its own startup/cleanup code uses. */
CRTFN(void *) crt__malloc_crt(size_t n) { return malloc(n); }
CRTFN(void *) crt__calloc_crt(size_t n, size_t size) { return calloc(n, size); }
CRTFN(void *) crt__realloc_crt(void *p, size_t n) { return realloc(p, n); }
CRTFN(void) crt__free_crt(void *p) { free(p); }

/* _lock/_unlock: the CRT's own internal locks (heap, stdio, ...), by
 * number; a fixed table lazily initialised covers every locknum a codec's
 * CRT actually uses (0-15 covers every _*_LOCK constant mingw-w64 and MSVC
 * define). */
#define MAX_LOCKS 16
static pthread_mutex_t crt_locks[MAX_LOCKS];
static pthread_once_t crt_locks_once = PTHREAD_ONCE_INIT;

static void crt_locks_init(void)
{
	for (int i = 0; i < MAX_LOCKS; i++)
		pthread_mutex_init(&crt_locks[i], NULL);
}

CRTFN(void) crt__lock(int locknum)
{
	pthread_once(&crt_locks_once, crt_locks_init);
	if (locknum >= 0 && locknum < MAX_LOCKS)
		pthread_mutex_lock(&crt_locks[locknum]);
}

CRTFN(void) crt__unlock(int locknum)
{
	pthread_once(&crt_locks_once, crt_locks_init);
	if (locknum >= 0 && locknum < MAX_LOCKS)
		pthread_mutex_unlock(&crt_locks[locknum]);
}

/* Console-vs-GUI subsystem bookkeeping: nothing here cares. */
CRTFN(void) crt___set_app_type(int type) { (void)type; }
CRTFN(int) crt__set_error_mode(int mode) { (void)mode; return 0; }

static int crt_fmode, crt_commode = 1;	/* _O_TEXT, and full buffering */

/* A fatal CRT-detected error ("out of memory", historically "R6002" and
 * friends): report it and stop, as msvcrt's own _amsg_exit does. */
CRTFN(void) crt__amsg_exit(int code)
{
	fprintf(stderr, "seance: msvcrt runtime error %d\n", code);
	fflush(stderr);
	_exit(255);
}

/* Forwards a first-chance SEH exception to the process's own top-level
 * filter, the way the real _XcptFilter (called from a __except filter
 * expression) does; xcptnum, the exception's own already-known code, adds
 * nothing seh_UnhandledExceptionFilter doesn't already have from ep. */
CRTFN(int) crt__XcptFilter(unsigned int xcptnum, EXCEPTION_POINTERS *ep)
{
	(void)xcptnum;
	return (int)seh_UnhandledExceptionFilter(ep);
}

#define E(name) { #name, 0, (void *)crt_##name }

const struct builtin_export crt_startup_exports[] = {
	E(_initterm), E(_initterm_e),
	E(atexit), E(_onexit), E(__dllonexit),
	E(_encode_pointer), E(_decode_pointer), E(_encoded_null),
	E(_malloc_crt), E(_calloc_crt), E(_realloc_crt), E(_free_crt),
	E(_lock), E(_unlock),
	E(__set_app_type), E(_set_error_mode),
	E(_amsg_exit), E(_XcptFilter),
	{ "_fmode", 0, &crt_fmode }, { "_commode", 0, &crt_commode },
	BUILTIN_END
};
