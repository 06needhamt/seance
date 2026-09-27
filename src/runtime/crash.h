/*
 * Crash and hang reports. When DLL code faults, the harness says where
 * (module+offset), what it touched, and which call into the DLL was in
 * progress, then dies of the signal as it would have. With
 * VFW_TIMEOUT=<seconds>, a call into a DLL that takes longer than that
 * is reported the same way and the process exits with status 124.
 */
#ifndef VFW_RUNTIME_CRASH_H
#define VFW_RUNTIME_CRASH_H

#include "win32.h"

/*
 * Installs the fault handlers and, if VFW_TIMEOUT is set, the watchdog.
 * Returns 0, or -1 if VFW_TIMEOUT is not a number of seconds.
 */
int crash_install(void);

/* Sets up the calling thread (its signal stack); runtime_enter() calls it. */
void crash_thread_init(void);
/* And takes it down again; teb_exit_thread() calls it. */
void crash_thread_exit(void);

/* Names a call's code, e.g. drv_msg_name for driver messages. */
typedef const char *(*crash_namer)(UINT code, char buf[24]);

/*
 * Brackets a call into DLL code: what (e.g. "DriverProc"), the function
 * called, and a code (a message) named by namer (may be NULL). Calls nest;
 * crash_leave() restores what crash_enter() saved.
 */
struct crash_frame {
	const char *what;
	const void *fn;
	UINT code;
	crash_namer namer;
};

void crash_enter(const char *what, const void *fn, UINT code,
		 crash_namer namer, struct crash_frame *saved);
void crash_leave(const struct crash_frame *saved);

/*
 * Prints the same report a fault reaching on_fault would, for pc/fault_addr
 * on the calling thread, then dies of sig as it would without the handler.
 * seh.c calls this once its dispatch of a hardware fault finds no handler
 * in the fs:[0] chain that takes it.
 */
void crash_report_and_die(int sig, const void *pc, const void *fault_addr)
	__attribute__((noreturn));

#endif /* VFW_RUNTIME_CRASH_H */
