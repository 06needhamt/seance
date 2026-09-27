/*
 * Keeping the memory DLL code sees below 2 GB, as Windows does for programs
 * that are not large-address aware. See lowmem.c.
 */
#ifndef VFW_RUNTIME_LOWMEM_H
#define VFW_RUNTIME_LOWMEM_H

/*
 * Reserves the free address space from 2 GB up, so later mappings land
 * below it. Call it early, before other threads start. VFW_LOW_MEMORY=0
 * turns it off. Returns 0 (also when off), or -1 if the address map can't
 * be read.
 */
int lowmem_reserve(void);

/* Whether a reservation is in place. */
int lowmem_active(void);

/*
 * Reserves, then runs fn on a thread whose stack and arguments are below
 * 2 GB, and returns its status. Without a reservation, calls fn directly.
 */
int lowmem_run(int (*fn)(int argc, char **argv), int argc, char **argv);

#endif /* VFW_RUNTIME_LOWMEM_H */
