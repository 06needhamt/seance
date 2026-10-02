/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * winmm.dll: the installable-driver functions codecs use (DefDriverProc
 * above all), on top of src/vfw/driver.c, and the multimedia timer.
 */
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <time.h>

#include "common/unicode.h"
#include "loader/modules.h"
#include "vfw/driver.h"
#include "vfw/registry.h"
#include "win32/internal.h"
#include "win32/path.h"

#define TIMERR_NOERROR 0
#define TIMERR_NOCANDO 97

/* Wine's DefDriverProc: what a driver gets for messages it doesn't handle. */
WINFN(LRESULT) mm_DefDriverProc(DWORD_PTR id, HDRVR h, UINT msg, LPARAM l1,
				LPARAM l2)
{
	switch (msg) {
	case DRV_LOAD:
	case DRV_FREE:
	case DRV_ENABLE:
	case DRV_DISABLE:
	case DRV_OPEN:
	case DRV_CLOSE:
		return 1;
	case DRV_QUERYCONFIGURE:
		return 0;
	case DRV_CONFIGURE:
		TRACE("DefDriverProc: the driver isn't configurable");
		return DRVCNF_OK;
	case DRV_INSTALL:
	case DRV_REMOVE:
		return DRVCNF_RESTART;
	default:
		return 0;
	}
}

/*
 * The DLL for a driver name: an alias such as "vidc.TST0" registered in
 * [drivers32] (the only section there is), or a DLL name or path.
 */
static HDRVR open_named(const char *name, const WCHAR *alias, LPARAM lp2)
{
	FOURCC type, handler;
	char *unix_path = NULL;
	HDRVR h;

	if (strchr(name, '.') && !vfw_parse_fourcc(name, &type, &handler)) {
		const struct vfw_codec *c = vfw_find(type, handler);
		if (c && c->proc)
			return drv_open_proc(c->proc, alias, lp2);
		if (c)
			return drv_open(c->path, alias, lp2);
	}
	if (strchr(name, ':') || strchr(name, '\\'))
		unix_path = path_to_unix(name);
	h = drv_open(unix_path ? unix_path : name, alias, lp2);
	free(unix_path);
	return h;
}

WINFN(HDRVR) mm_OpenDriver(LPCWSTR name, LPCWSTR section, LPARAM lp2)
{
	char utf8[1024];

	if (!name)
		return NULL;
	utf16_to_utf8(name, (size_t)-1, utf8, sizeof(utf8));
	HDRVR h = open_named(utf8, name, lp2);
	TRACE("OpenDriver(%s) -> %p", utf8, (void *)h);
	return h;
}

WINFN(HDRVR) mm_OpenDriverA(LPCSTR name, LPCSTR section, LPARAM lp2)
{
	WCHAR alias[260];
	size_t i = 0;

	if (!name)
		return NULL;
	for (; name[i] && i < 259; i++)
		alias[i] = (BYTE)name[i];
	alias[i] = 0;
	return open_named(name, alias, lp2);
}

WINFN(LRESULT) mm_CloseDriver(HDRVR h, LPARAM l1, LPARAM l2)
{
	return drv_close(h, l1, l2);
}

WINFN(LRESULT) mm_SendDriverMessage(HDRVR h, UINT msg, LPARAM l1, LPARAM l2)
{
	return drv_send(h, msg, l1, l2);
}

WINFN(HMODULE) mm_GetDriverModuleHandle(HDRVR h)
{
	struct module *m = drv_module(h);
	return m ? (HMODULE)m->base : NULL;
}

WINFN(DWORD) mm_timeGetTime(void)
{
	struct timespec ts;

	clock_gettime(CLOCK_MONOTONIC, &ts);
	return (DWORD)((uint64_t)ts.tv_sec * 1000 + ts.tv_nsec / 1000000);
}

WINFN(UINT) mm_timeBeginPeriod(UINT period)
{
	return period ? TIMERR_NOERROR : TIMERR_NOCANDO;
}

WINFN(UINT) mm_timeEndPeriod(UINT period)
{
	return period ? TIMERR_NOERROR : TIMERR_NOCANDO;
}

WINFN(UINT) mm_timeGetDevCaps(UINT *caps, UINT size)
{
	if (!caps || size < 2 * sizeof(UINT))
		return TIMERR_NOCANDO;
	caps[0] = 1;		/* wPeriodMin */
	caps[1] = 1000000;	/* wPeriodMax */
	return TIMERR_NOERROR;
}

#define E(name) { #name, 0, (void *)mm_##name }

static const struct builtin_export exports[] = {
	E(CloseDriver),
	E(DefDriverProc),
	{ "DrvClose", 0, (void *)mm_CloseDriver },
	{ "DrvDefDriverProc", 0, (void *)mm_DefDriverProc },
	{ "DrvGetModuleHandle", 0, (void *)mm_GetDriverModuleHandle },
	{ "DrvOpen", 0, (void *)mm_OpenDriver },
	{ "DrvSendMessage", 0, (void *)mm_SendDriverMessage },
	E(GetDriverModuleHandle),
	E(OpenDriver),
	E(OpenDriverA),
	E(SendDriverMessage),
	E(timeBeginPeriod),
	E(timeEndPeriod),
	E(timeGetDevCaps),
	E(timeGetTime),
	BUILTIN_END
};

static const struct builtin_export *const tables[] = { exports, NULL };

const struct builtin_dll builtin_winmm = { "winmm.dll", NULL, tables, 0 };
