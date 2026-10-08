/* SPDX-License-Identifier: GPL-2.0-only */
#include <pthread.h>

#include "common/log.h"
#include "runtime/crash.h"
#include "runtime/runtime.h"
#include "runtime/teb.h"
#include "win32/k32.h"

static pthread_once_t once = PTHREAD_ONCE_INIT;

static void process_init(void)
{
	vfw_log_init();
	peb_get();
	/*
	 * The PEB names it from the start: DLLs read it there (Wine's
	 * GetProcessHeap is NtCurrentTeb()->Peb->ProcessHeap, inline).
	 */
	heap_process();
}

int runtime_enter(void)
{
	pthread_once(&once, process_init);
	if (!teb_init_thread())
		return -1;
	crash_thread_init();
	return 0;
}
