#include <asm/ldt.h>
#include <errno.h>
#include <pthread.h>
#include <stddef.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/syscall.h>
#include <unistd.h>

#include "common/error.h"
#include "runtime/teb.h"

_Static_assert(offsetof(struct win_teb, ExceptionList) == 0x00, "TEB");
_Static_assert(offsetof(struct win_teb, StackBase) == 0x04, "TEB");
_Static_assert(offsetof(struct win_teb, StackLimit) == 0x08, "TEB");
_Static_assert(offsetof(struct win_teb, Self) == 0x18, "TEB");
_Static_assert(offsetof(struct win_teb, UniqueProcess) == 0x20, "TEB");
_Static_assert(offsetof(struct win_teb, UniqueThread) == 0x24, "TEB");
_Static_assert(offsetof(struct win_teb, ThreadLocalStoragePointer) == 0x2c, "TEB");
_Static_assert(offsetof(struct win_teb, Peb) == 0x30, "TEB");
_Static_assert(offsetof(struct win_teb, LastErrorValue) == 0x34, "TEB");
_Static_assert(offsetof(struct win_teb, TlsSlots) == 0xe10, "TEB");
_Static_assert(offsetof(struct win_teb, TlsExpansionSlots) == 0xf94, "TEB");
_Static_assert(sizeof(struct win_teb) <= TEB_SIZE, "TEB size");
_Static_assert(offsetof(struct win_peb, BeingDebugged) == 0x02, "PEB");
_Static_assert(offsetof(struct win_peb, ImageBaseAddress) == 0x08, "PEB");
_Static_assert(offsetof(struct win_peb, ProcessHeap) == 0x18, "PEB");
_Static_assert(offsetof(struct win_peb, NumberOfProcessors) == 0x64, "PEB");
_Static_assert(offsetof(struct win_peb, OSMajorVersion) == 0xa4, "PEB");
_Static_assert(offsetof(struct win_peb, OSPlatformId) == 0xb0, "PEB");

#define LDT_ENTRIES 8192
#define EXPANSION_BYTES (TEB_TLS_EXPANSION_SLOTS * sizeof(void *))

static _Thread_local struct win_teb *current;
static pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;
static struct win_teb *all_tebs;
static unsigned char ldt_used[LDT_ENTRIES];
static unsigned char tls_used[TEB_TLS_MAX];

static struct win_peb peb;
static pthread_once_t peb_once = PTHREAD_ONCE_INIT;

static void peb_init(void)
{
	long cpus = sysconf(_SC_NPROCESSORS_ONLN);

	/* Report Windows XP SP3, the last version most VfW codecs knew. */
	peb.NumberOfProcessors = cpus > 0 ? cpus : 1;
	peb.OSMajorVersion = 5;
	peb.OSMinorVersion = 1;
	peb.OSBuildNumber = 2600;
	peb.OSCSDVersion = 0x0300;
	peb.OSPlatformId = 2;		/* VER_PLATFORM_WIN32_NT */
}

struct win_peb *peb_get(void)
{
	pthread_once(&peb_once, peb_init);
	return &peb;
}

static int set_ldt(int entry, void *base, unsigned limit, int present)
{
	struct user_desc d;

	memset(&d, 0, sizeof(d));
	d.entry_number = entry;
	d.base_addr = (unsigned long)base;
	d.limit = limit;
	d.seg_32bit = 1;
	d.useable = 1;
	if (!present) {
		/* The documented way to clear an entry. */
		d.base_addr = 0;
		d.limit = 0;
		d.read_exec_only = 1;
		d.seg_not_present = 1;
	}
	return syscall(SYS_modify_ldt, 1, &d, sizeof(d));
}

static void load_fs(unsigned short sel)
{
	__asm__ volatile("movw %0, %%fs" : : "r"(sel) : "memory");
}

static void set_fpu_control(unsigned short cw)
{
	__asm__ volatile("fldcw %0" : : "m"(cw));
}

struct win_teb *teb_init_thread(void)
{
	if (current)
		return current;

	struct win_teb *teb = mmap(NULL, TEB_SIZE, PROT_READ | PROT_WRITE,
				   MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
	if (teb == MAP_FAILED) {
		vfw_set_error("cannot allocate a TEB: %s", strerror(errno));
		return NULL;
	}

	teb->ExceptionList = SEH_CHAIN_END;
	teb->Self = teb;
	teb->UniqueProcess = getpid();
	teb->UniqueThread = syscall(SYS_gettid);
	teb->Peb = peb_get();
	teb->priv.thread = pthread_self();

	pthread_attr_t attr;
	void *stack;
	size_t size;
	if (!pthread_getattr_np(pthread_self(), &attr)) {
		if (!pthread_attr_getstack(&attr, &stack, &size)) {
			teb->StackLimit = stack;
			teb->StackBase = (BYTE *)stack + size;
		}
		pthread_attr_destroy(&attr);
	}

	pthread_mutex_lock(&lock);
	int entry = -1;
	for (int i = 0; i < LDT_ENTRIES; i++) {
		if (!ldt_used[i]) {
			entry = i;
			break;
		}
	}
	if (entry < 0 || set_ldt(entry, teb, TEB_SIZE - 1, 1)) {
		int err = errno;
		pthread_mutex_unlock(&lock);
		munmap(teb, TEB_SIZE);
		if (entry < 0)
			vfw_set_error("no free LDT entries");
		else
			vfw_set_error("modify_ldt: %s", strerror(err));
		return NULL;
	}
	ldt_used[entry] = 1;
	teb->priv.ldt_entry = entry;
	teb->priv.next = all_tebs;
	all_tebs = teb;
	pthread_mutex_unlock(&lock);

	load_fs((entry << 3) | 7);	/* LDT, ring 3 */
	set_fpu_control(0x27f);
	current = teb;
	return teb;
}

void teb_exit_thread(void)
{
	struct win_teb *teb = current;

	if (!teb)
		return;
	load_fs(0);
	current = NULL;

	pthread_mutex_lock(&lock);
	for (struct win_teb **p = &all_tebs; *p; p = &(*p)->priv.next) {
		if (*p == teb) {
			*p = teb->priv.next;
			break;
		}
	}
	set_ldt(teb->priv.ldt_entry, NULL, 0, 0);
	ldt_used[teb->priv.ldt_entry] = 0;
	pthread_mutex_unlock(&lock);

	if (teb->TlsExpansionSlots)
		munmap(teb->TlsExpansionSlots, EXPANSION_BYTES);
	munmap(teb, TEB_SIZE);
}

struct win_teb *teb_current(void)
{
	return current;
}

void teb_foreach(void (*fn)(struct win_teb *teb, void *ctx), void *ctx)
{
	pthread_mutex_lock(&lock);
	for (struct win_teb *t = all_tebs; t; t = t->priv.next)
		fn(t, ctx);
	pthread_mutex_unlock(&lock);
}

/* TLS slots. */

DWORD teb_tls_alloc(void)
{
	DWORD index = 0xffffffff;

	pthread_mutex_lock(&lock);
	for (DWORD i = 0; i < TEB_TLS_MAX; i++) {
		if (!tls_used[i]) {
			tls_used[i] = 1;
			index = i;
			break;
		}
	}
	pthread_mutex_unlock(&lock);
	return index;
}

static void **slot(struct win_teb *teb, DWORD index, int create)
{
	if (index < TEB_TLS_SLOTS)
		return &teb->TlsSlots[index];
	if (!teb->TlsExpansionSlots) {
		if (!create)
			return NULL;
		/* mmap, like the TEB that points to it. */
		void *p = mmap(NULL, EXPANSION_BYTES, PROT_READ | PROT_WRITE,
			       MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
		if (p == MAP_FAILED)
			return NULL;
		teb->TlsExpansionSlots = p;
	}
	return &teb->TlsExpansionSlots[index - TEB_TLS_SLOTS];
}

static void clear_slot(struct win_teb *teb, void *ctx)
{
	void **s = slot(teb, *(DWORD *)ctx, 0);
	if (s)
		*s = NULL;
}

int teb_tls_valid(DWORD index)
{
	int valid;

	if (index >= TEB_TLS_MAX)
		return 0;
	pthread_mutex_lock(&lock);
	valid = tls_used[index];
	pthread_mutex_unlock(&lock);
	return valid;
}

BOOL teb_tls_free(DWORD index)
{
	if (!teb_tls_valid(index))
		return FALSE;
	/* Like Windows, a freed slot reads as NULL in every thread. */
	teb_foreach(clear_slot, &index);
	pthread_mutex_lock(&lock);
	tls_used[index] = 0;
	pthread_mutex_unlock(&lock);
	return TRUE;
}

BOOL teb_tls_set(DWORD index, void *value)
{
	struct win_teb *teb = teb_init_thread();
	void **s;

	if (!teb || index >= TEB_TLS_MAX || !(s = slot(teb, index, 1)))
		return FALSE;
	*s = value;
	return TRUE;
}

void *teb_tls_get(DWORD index)
{
	struct win_teb *teb = teb_init_thread();
	void **s;

	if (!teb || index >= TEB_TLS_MAX || !(s = slot(teb, index, 0)))
		return NULL;
	return *s;
}
