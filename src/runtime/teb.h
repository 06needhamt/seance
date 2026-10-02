/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * Windows thread and process environment blocks.
 *
 * Win32 code finds its per-thread state through %fs: fs:[0] is the SEH
 * chain, fs:[0x18] the TEB itself, fs:[0x2c] the implicit TLS array,
 * fs:[0x30] the PEB and fs:[0x34] GetLastError(). Each thread that runs DLL
 * code gets a TEB laid out like the 32-bit Windows one, reachable through an
 * LDT segment loaded into %fs. glibc on i386 uses %gs for its own TLS, so
 * %fs is free.
 */
#ifndef VFW_RUNTIME_TEB_H
#define VFW_RUNTIME_TEB_H

#include <pthread.h>

#include "win32.h"

#define TEB_TLS_SLOTS           64
#define TEB_TLS_EXPANSION_SLOTS 1024
#define TEB_TLS_MAX             (TEB_TLS_SLOTS + TEB_TLS_EXPANSION_SLOTS)
#define TEB_SIZE                0x2000
#define TEB_STATIC_TLS_MAX      128	/* modules with implicit TLS */

/* The end of the SEH chain. */
#define SEH_CHAIN_END ((void *)0xffffffff)

struct win_peb {
	BYTE InheritedAddressSpace;		/* 0x00 */
	BYTE ReadImageFileExecOptions;		/* 0x01 */
	BYTE BeingDebugged;			/* 0x02 */
	BYTE SpareBool;				/* 0x03 */
	void *Mutant;				/* 0x04 */
	void *ImageBaseAddress;			/* 0x08 */
	void *Ldr;				/* 0x0c */
	void *ProcessParameters;		/* 0x10 */
	void *SubSystemData;			/* 0x14 */
	void *ProcessHeap;			/* 0x18 */
	void *FastPebLock;			/* 0x1c */
	BYTE reserved1[0x64 - 0x20];
	DWORD NumberOfProcessors;		/* 0x64 */
	DWORD NtGlobalFlag;			/* 0x68 */
	BYTE reserved2[0xa4 - 0x6c];
	DWORD OSMajorVersion;			/* 0xa4 */
	DWORD OSMinorVersion;			/* 0xa8 */
	WORD OSBuildNumber;			/* 0xac */
	WORD OSCSDVersion;			/* 0xae */
	DWORD OSPlatformId;			/* 0xb0 */
	BYTE reserved3[0x210 - 0xb4];
};

/* Harness-private state, kept after the Windows part of the TEB. */
struct teb_private {
	struct win_teb *next;		/* all TEBs, for TLS and notifications */
	int ldt_entry;
	pthread_t thread;
	int in_exception;		/* SEH dispatch in progress */
	/* The implicit TLS array fs:[0x2c] points to: a block per module. */
	void *static_tls[TEB_STATIC_TLS_MAX];
};

struct win_teb {
	/* NT_TIB */
	void *ExceptionList;			/* 0x00 */
	void *StackBase;			/* 0x04 */
	void *StackLimit;			/* 0x08 */
	void *SubSystemTib;			/* 0x0c */
	void *FiberData;			/* 0x10 */
	void *ArbitraryUserPointer;		/* 0x14 */
	struct win_teb *Self;			/* 0x18 */

	void *EnvironmentPointer;		/* 0x1c */
	DWORD UniqueProcess;			/* 0x20 */
	DWORD UniqueThread;			/* 0x24 */
	void *ActiveRpcHandle;			/* 0x28 */
	void **ThreadLocalStoragePointer;	/* 0x2c */
	struct win_peb *Peb;			/* 0x30 */
	DWORD LastErrorValue;			/* 0x34 */
	BYTE reserved1[0xe10 - 0x38];
	void *TlsSlots[TEB_TLS_SLOTS];		/* 0xe10 */
	BYTE reserved2[0xf94 - 0xf10];
	void **TlsExpansionSlots;		/* 0xf94 */
	BYTE reserved3[0x1000 - 0xf98];

	struct teb_private priv;		/* 0x1000 */
};

/*
 * Sets up the calling thread's TEB (once) and loads it into %fs. Also sets
 * the x87 control word to the Windows default (53-bit precision). Returns
 * NULL, with vfw_error(), if the LDT cannot be set up.
 */
struct win_teb *teb_init_thread(void);

/* Tears down the calling thread's TEB, if it has one. */
void teb_exit_thread(void);

/* The calling thread's TEB, or NULL if it has none. */
struct win_teb *teb_current(void);

struct win_peb *peb_get(void);

/* Calls fn for every thread's TEB, with the TEB list locked. */
void teb_foreach(void (*fn)(struct win_teb *teb, void *ctx), void *ctx);

/*
 * TLS slots (TlsAlloc and friends). Slots below TEB_TLS_SLOTS live in the
 * TEB itself; the rest in TlsExpansionSlots, allocated on first use.
 */
DWORD teb_tls_alloc(void);		/* 0xffffffff when none are left */
BOOL teb_tls_free(DWORD index);
BOOL teb_tls_set(DWORD index, void *value);
void *teb_tls_get(DWORD index);
int teb_tls_valid(DWORD index);

/*
 * Implicit (static) TLS, for modules with a TLS directory: MSVC's
 * __declspec(thread) variables live in a per-thread copy of the module's
 * TLS template, found as fs:[0x2c][_tls_index]. teb_static_tls_add()
 * registers a template of size bytes followed by zero_fill zero bytes and
 * gives every existing thread its block, as Windows Vista and later do for
 * DLLs loaded at run time; threads created afterwards get theirs when their
 * TEB is set up. Returns the index, or 0xffffffff with vfw_error().
 * teb_static_tls_remove() frees every thread's block for that index.
 */
DWORD teb_static_tls_add(const void *tmpl, size_t size, size_t zero_fill);
void teb_static_tls_remove(DWORD index);

#endif /* VFW_RUNTIME_TEB_H */
