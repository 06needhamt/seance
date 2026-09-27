/*
 * The Win32 runtime: per-process and per-thread setup that must happen
 * before any DLL code runs.
 */
#ifndef VFW_RUNTIME_RUNTIME_H
#define VFW_RUNTIME_RUNTIME_H

/*
 * Initialises the process state once (logging, PEB, heap, exception
 * handling) and the calling thread's TEB. Every entry point that may run DLL
 * code on a thread calls this first. Returns 0, or -1 with vfw_error().
 */
int runtime_enter(void);

#endif /* VFW_RUNTIME_RUNTIME_H */
