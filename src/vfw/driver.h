/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * Installable drivers: what winmm's OpenDriver / SendDriverMessage /
 * CloseDriver do. A driver is a DLL exporting DriverProc (or, for
 * ICOpenFunction, a bare procedure). Its first open sends DRV_LOAD and
 * DRV_ENABLE, each open DRV_OPEN; each close DRV_CLOSE, and the last one
 * DRV_DISABLE and DRV_FREE, following Wine's dlls/winmm/driver.c.
 *
 * With VFW_TRACE=1 every message and its result is logged.
 */
#ifndef VFW_VFW_DRIVER_H
#define VFW_VFW_DRIVER_H

#include "win32.h"

struct module;

/*
 * Opens the driver in dll (a path, or a bare name searched for like
 * LoadLibrary). alias is the lParam1 of DRV_OPEN (the driver's name, e.g.
 * L"vidc.TST0"; may be NULL) and lp2 its lParam2. NULL with vfw_error() on
 * failure, including DRV_OPEN returning 0.
 */
HDRVR drv_open(const char *dll, const WCHAR *alias, LPARAM lp2);

/* The same for a driver procedure with no DLL behind it. */
HDRVR drv_open_proc(DRIVERPROC proc, const WCHAR *alias, LPARAM lp2);

/* Sends a message to an open driver. 0 for a bad handle. */
LRESULT drv_send(HDRVR h, UINT msg, LPARAM l1, LPARAM l2);

/* Closes an open driver. Returns DRV_CLOSE's result, or 0. */
LRESULT drv_close(HDRVR h, LPARAM l1, LPARAM l2);

/* Whether h is an open driver. */
int drv_valid(HDRVR h);

/* The DLL behind a driver (NULL for a procedure or a bad handle). */
struct module *drv_module(HDRVR h);

/* The number of open drivers (for tests). */
unsigned drv_open_count(void);

/*
 * Calls a driver procedure directly, tracing the message. Used for the
 * messages sent outside an open instance.
 */
LRESULT drv_call(DRIVERPROC proc, DWORD_PTR id, HDRVR h, UINT msg,
		 LPARAM l1, LPARAM l2);

/* A message's name ("DRV_OPEN", "ICM_DECOMPRESS", ...) or its number. */
const char *drv_msg_name(UINT msg, char buf[24]);

#endif /* VFW_VFW_DRIVER_H */
