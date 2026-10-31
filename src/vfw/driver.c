/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * Installable drivers.
 */
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "common/error.h"
#include "common/log.h"
#include "loader/modules.h"
#include "loader/pe.h"
#include "runtime/runtime.h"
#include "runtime/crash.h"
#include "vfw/driver.h"

#define MAX_DRIVERS 256
#define HANDLE_TAG  0x5d000000u

/* A driver (DLL or procedure) with at least one open instance. */
struct drv_module {
	struct drv_module *next;
	struct module *mod;	/* NULL for a bare procedure */
	DRIVERPROC proc;
	unsigned opens;
};

struct drv_instance {
	struct drv_module *dm;	/* NULL when free */
	DWORD_PTR id;		/* what DRV_OPEN returned */
	int opening;		/* reserved while DRV_OPEN runs */
};

/* Guards the tables; never held while a driver runs. */
static pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;
/* Serialises opens and closes, which call into drivers; recursive. */
static pthread_mutex_t open_lock = PTHREAD_RECURSIVE_MUTEX_INITIALIZER_NP;
static struct drv_instance instances[MAX_DRIVERS];
static struct drv_module *modules;

static const struct {
	UINT msg;
	const char *name;
} msg_names[] = {
	{ DRV_LOAD, "DRV_LOAD" }, { DRV_ENABLE, "DRV_ENABLE" },
	{ DRV_OPEN, "DRV_OPEN" }, { DRV_CLOSE, "DRV_CLOSE" },
	{ DRV_DISABLE, "DRV_DISABLE" }, { DRV_FREE, "DRV_FREE" },
	{ DRV_CONFIGURE, "DRV_CONFIGURE" },
	{ DRV_QUERYCONFIGURE, "DRV_QUERYCONFIGURE" },
	{ DRV_INSTALL, "DRV_INSTALL" }, { DRV_REMOVE, "DRV_REMOVE" },
	{ DRV_EXITSESSION, "DRV_EXITSESSION" }, { DRV_POWER, "DRV_POWER" },
	{ ICM_GETSTATE, "ICM_GETSTATE" }, { ICM_SETSTATE, "ICM_SETSTATE" },
	{ ICM_GETINFO, "ICM_GETINFO" }, { ICM_CONFIGURE, "ICM_CONFIGURE" },
	{ ICM_ABOUT, "ICM_ABOUT" }, { ICM_GETERRORTEXT, "ICM_GETERRORTEXT" },
	{ ICM_GETFORMATNAME, "ICM_GETFORMATNAME" },
	{ ICM_ENUMFORMATS, "ICM_ENUMFORMATS" },
	{ ICM_GETDEFAULTQUALITY, "ICM_GETDEFAULTQUALITY" },
	{ ICM_GETQUALITY, "ICM_GETQUALITY" }, { ICM_SETQUALITY, "ICM_SETQUALITY" },
	{ ICM_SET, "ICM_SET" }, { ICM_GET, "ICM_GET" },
	{ ICM_COMPRESS_GET_FORMAT, "ICM_COMPRESS_GET_FORMAT" },
	{ ICM_COMPRESS_GET_SIZE, "ICM_COMPRESS_GET_SIZE" },
	{ ICM_COMPRESS_QUERY, "ICM_COMPRESS_QUERY" },
	{ ICM_COMPRESS_BEGIN, "ICM_COMPRESS_BEGIN" },
	{ ICM_COMPRESS, "ICM_COMPRESS" }, { ICM_COMPRESS_END, "ICM_COMPRESS_END" },
	{ ICM_DECOMPRESS_GET_FORMAT, "ICM_DECOMPRESS_GET_FORMAT" },
	{ ICM_DECOMPRESS_QUERY, "ICM_DECOMPRESS_QUERY" },
	{ ICM_DECOMPRESS_BEGIN, "ICM_DECOMPRESS_BEGIN" },
	{ ICM_DECOMPRESS, "ICM_DECOMPRESS" },
	{ ICM_DECOMPRESS_END, "ICM_DECOMPRESS_END" },
	{ ICM_DECOMPRESS_SET_PALETTE, "ICM_DECOMPRESS_SET_PALETTE" },
	{ ICM_DECOMPRESS_GET_PALETTE, "ICM_DECOMPRESS_GET_PALETTE" },
	{ ICM_DRAW_QUERY, "ICM_DRAW_QUERY" }, { ICM_DRAW_BEGIN, "ICM_DRAW_BEGIN" },
	{ ICM_DRAW_END, "ICM_DRAW_END" }, { ICM_DRAW, "ICM_DRAW" },
	{ ICM_DRAW_SUGGESTFORMAT, "ICM_DRAW_SUGGESTFORMAT" },
	{ ICM_GETBUFFERSWANTED, "ICM_GETBUFFERSWANTED" },
	{ ICM_GETDEFAULTKEYFRAMERATE, "ICM_GETDEFAULTKEYFRAMERATE" },
	{ ICM_DECOMPRESSEX_BEGIN, "ICM_DECOMPRESSEX_BEGIN" },
	{ ICM_DECOMPRESSEX_QUERY, "ICM_DECOMPRESSEX_QUERY" },
	{ ICM_DECOMPRESSEX, "ICM_DECOMPRESSEX" },
	{ ICM_DECOMPRESSEX_END, "ICM_DECOMPRESSEX_END" },
	{ ICM_COMPRESS_FRAMES_INFO, "ICM_COMPRESS_FRAMES_INFO" },
	{ ICM_COMPRESS_FRAMES, "ICM_COMPRESS_FRAMES" },
	{ ICM_SET_STATUS_PROC, "ICM_SET_STATUS_PROC" },
	{ ACMDM_DRIVER_NOTIFY, "ACMDM_DRIVER_NOTIFY" },
	{ ACMDM_DRIVER_DETAILS, "ACMDM_DRIVER_DETAILS" },
	{ ACMDM_DRIVER_ABOUT, "ACMDM_DRIVER_ABOUT" },
	{ ACMDM_HARDWARE_WAVE_CAPS_INPUT, "ACMDM_HARDWARE_WAVE_CAPS_INPUT" },
	{ ACMDM_HARDWARE_WAVE_CAPS_OUTPUT, "ACMDM_HARDWARE_WAVE_CAPS_OUTPUT" },
	{ ACMDM_FORMATTAG_DETAILS, "ACMDM_FORMATTAG_DETAILS" },
	{ ACMDM_FORMAT_DETAILS, "ACMDM_FORMAT_DETAILS" },
	{ ACMDM_FORMAT_SUGGEST, "ACMDM_FORMAT_SUGGEST" },
	{ ACMDM_FILTERTAG_DETAILS, "ACMDM_FILTERTAG_DETAILS" },
	{ ACMDM_FILTER_DETAILS, "ACMDM_FILTER_DETAILS" },
	{ ACMDM_STREAM_OPEN, "ACMDM_STREAM_OPEN" },
	{ ACMDM_STREAM_CLOSE, "ACMDM_STREAM_CLOSE" },
	{ ACMDM_STREAM_SIZE, "ACMDM_STREAM_SIZE" },
	{ ACMDM_STREAM_CONVERT, "ACMDM_STREAM_CONVERT" },
	{ ACMDM_STREAM_RESET, "ACMDM_STREAM_RESET" },
	{ ACMDM_STREAM_PREPARE, "ACMDM_STREAM_PREPARE" },
	{ ACMDM_STREAM_UNPREPARE, "ACMDM_STREAM_UNPREPARE" },
	{ ACMDM_STREAM_UPDATE, "ACMDM_STREAM_UPDATE" },
};

const char *drv_msg_name(UINT msg, char buf[24])
{
	for (size_t i = 0; i < sizeof(msg_names) / sizeof(msg_names[0]); i++)
		if (msg_names[i].msg == msg)
			return msg_names[i].name;
	if (msg >= DRV_USER)
		snprintf(buf, 24, "DRV_USER+0x%x", msg - DRV_USER);
	else
		snprintf(buf, 24, "0x%x", msg);
	return buf;
}

LRESULT drv_call(DRIVERPROC proc, DWORD_PTR id, HDRVR h, UINT msg,
		 LPARAM l1, LPARAM l2)
{
	char buf[24];

	TRACE("driver %p: %s(id 0x%lx, 0x%lx, 0x%lx)", (void *)h,
	      drv_msg_name(msg, buf), (unsigned long)id, (unsigned long)l1,
	      (unsigned long)l2);
	struct crash_frame saved;
	crash_enter("DriverProc", (const void *)proc, msg, drv_msg_name, &saved);
	LRESULT r = proc(id, h, msg, l1, l2);
	crash_leave(&saved);
	TRACE("driver %p: %s -> %ld (0x%lx)", (void *)h, drv_msg_name(msg, buf),
	      (long)r, (unsigned long)r);
	return r;
}

static HDRVR handle_of(int slot)
{
	return (HDRVR)(UINT_PTR)(HANDLE_TAG | (unsigned)(slot + 1));
}

/* The slot of an open (or, with opening, a reserved) handle, or -1. */
static int slot_of(HDRVR h, int opening)
{
	UINT_PTR v = (UINT_PTR)h;
	unsigned i = (unsigned)(v & 0xffff) - 1;

	if ((v & 0xffff0000u) != HANDLE_TAG || i >= MAX_DRIVERS ||
	    !instances[i].dm || (instances[i].opening && !opening))
		return -1;
	return (int)i;
}

int drv_valid(HDRVR h)
{
	pthread_mutex_lock(&lock);
	int r = slot_of(h, 0) >= 0;
	pthread_mutex_unlock(&lock);
	return r;
}

struct module *drv_module(HDRVR h)
{
	struct module *m = NULL;

	pthread_mutex_lock(&lock);
	int s = slot_of(h, 1);
	if (s >= 0)
		m = instances[s].dm->mod;
	pthread_mutex_unlock(&lock);
	return m;
}

unsigned drv_open_count(void)
{
	unsigned n = 0;

	pthread_mutex_lock(&lock);
	for (int i = 0; i < MAX_DRIVERS; i++)
		n += instances[i].dm && !instances[i].opening;
	pthread_mutex_unlock(&lock);
	return n;
}

static void unlink_module(struct drv_module *dm)
{
	for (struct drv_module **p = &modules; *p; p = &(*p)->next)
		if (*p == dm) {
			*p = dm->next;
			break;
		}
}

/* Sends the messages that end a driver's last instance. */
static void unload(struct drv_module *dm, HDRVR h)
{
	drv_call(dm->proc, 0, h, DRV_DISABLE, 0, 0);
	drv_call(dm->proc, 0, h, DRV_FREE, 0, 0);
	pthread_mutex_lock(&lock);
	unlink_module(dm);
	pthread_mutex_unlock(&lock);
	if (dm->mod)
		mod_free(dm->mod);
	free(dm);
}

/*
 * Opens an instance of mod / proc. Takes over the module reference the
 * caller holds.
 */
/* ", it looked for ..., which aren't here", or "", for a refusal. */
static const char *looked_for(unsigned mark, char *buf, size_t size)
{
	char names[400];

	mod_missing_since(mark, names, sizeof(names));
	if (!names[0])
		return "";
	int one = !strchr(names, ',');
	snprintf(buf, size, "; it looked for %s, which %s here (add %s "
		 "directory to VFW_PATH)", names, one ? "isn't" : "aren't",
		 one ? "its" : "their");
	return buf;
}

static HDRVR open_instance(struct module *mod, DRIVERPROC proc,
			   const WCHAR *alias, LPARAM lp2, unsigned mark)
{
	char looked[512];
	struct drv_module *dm;
	int slot = -1, extra_ref = 0;

	pthread_mutex_lock(&open_lock);
	pthread_mutex_lock(&lock);
	for (dm = modules; dm; dm = dm->next)
		if (mod ? dm->mod == mod : !dm->mod && dm->proc == proc)
			break;
	extra_ref = dm && mod;	/* dm already holds a reference */
	if (!dm && (dm = calloc(1, sizeof(*dm)))) {
		dm->mod = mod;
		dm->proc = proc;
		dm->next = modules;
		modules = dm;
	}
	for (int i = 0; dm && i < MAX_DRIVERS && slot < 0; i++)
		if (!instances[i].dm) {
			slot = i;
			instances[i] = (struct drv_instance){ dm, 0, 1 };
		}
	pthread_mutex_unlock(&lock);
	if (extra_ref)
		mod_free(mod);

	if (!dm || slot < 0) {
		vfw_set_error(dm ? "too many open drivers" : "out of memory");
		if (dm && !dm->opens) {
			pthread_mutex_lock(&lock);
			unlink_module(dm);
			pthread_mutex_unlock(&lock);
			if (dm->mod)
				mod_free(dm->mod);
			free(dm);
		} else if (!dm && mod) {
			mod_free(mod);
		}
		pthread_mutex_unlock(&open_lock);
		return NULL;
	}

	HDRVR h = handle_of(slot);
	int first = dm->opens == 0, loaded = 0;
	if (first) {
		if (!drv_call(proc, 0, h, DRV_LOAD, 0, 0)) {
			vfw_set_error("%s: the driver refused DRV_LOAD%s",
				      mod ? mod->name : "driver procedure",
				      looked_for(mark, looked, sizeof(looked)));
			goto fail;
		}
		loaded = 1;
		drv_call(proc, 0, h, DRV_ENABLE, 0, 0);
	}
	DWORD_PTR id = drv_call(proc, 0, h, DRV_OPEN, (LPARAM)alias, lp2);
	if (!id) {
		vfw_set_error("%s: the driver refused DRV_OPEN%s",
			      mod ? mod->name : "driver procedure",
			      looked_for(mark, looked, sizeof(looked)));
		goto fail;
	}

	pthread_mutex_lock(&lock);
	instances[slot].id = id;
	instances[slot].opening = 0;
	dm->opens++;
	pthread_mutex_unlock(&lock);
	pthread_mutex_unlock(&open_lock);
	return h;

fail:
	pthread_mutex_lock(&lock);
	instances[slot].dm = NULL;
	pthread_mutex_unlock(&lock);
	if (first) {
		/* Undo the load for the instance that never opened. */
		if (loaded)
			unload(dm, h);
		else {
			pthread_mutex_lock(&lock);
			unlink_module(dm);
			pthread_mutex_unlock(&lock);
			if (dm->mod)
				mod_free(dm->mod);
			free(dm);
		}
	}
	pthread_mutex_unlock(&open_lock);
	return NULL;
}

/* An entry point of RealPlayer's codecs (the *3260.dll), if mod has one. */
static int realmedia_export(void *ctx, const struct pe_export *e)
{
	static const char *const prefixes[] = {
		"RACreateDecoderInstance", "RAOpenCodec", "RAInitDecoder",
		"PNCodec_",
	};
	const char *n = e->name;

	if (!n)
		return 0;
	/* RV10toYUV420Init, RV20toYUV420Transform, RV40toYUV420Free, ... */
	if (n[0] == 'R' && n[1] == 'V' && strstr(n, "toYUV420")) {
		*(const char **)ctx = n;
		return 1;
	}
	for (size_t i = 0; i < sizeof(prefixes) / sizeof(prefixes[0]); i++)
		if (!strncmp(n, prefixes[i], strlen(prefixes[i]))) {
			*(const char **)ctx = n;
			return 1;
		}
	return 0;
}

static const char *realmedia_entry(const struct module *mod)
{
	const char *found = NULL;

	if (mod->img)
		pe_foreach_export(mod->img, realmedia_export, &found);
	return found;
}

HDRVR drv_open(const char *dll, const WCHAR *alias, LPARAM lp2)
{
	if (runtime_enter())
		return NULL;

	unsigned mark = mod_missing_mark();
	struct module *mod = mod_load(dll, NULL);
	if (!mod)
		return NULL;	/* vfw_error() names the DLL */
	DRIVERPROC proc = mod_get_proc(mod, "DriverProc");
	if (!proc)
		proc = mod_get_proc(mod, "_DriverProc@20");
	if (!proc)
		proc = mod_get_proc(mod, "DriverProc@20");
	if (!proc) {
		const char *real = realmedia_entry(mod);
		if (real)
			vfw_set_error("%s: a RealPlayer codec (it exports %s), "
				      "which works through RealPlayer's own "
				      "interface, not as a VfW driver, DirectShow "
				      "filter or DMO", mod->name, real);
		else
			vfw_set_error("%s: not a driver (no DriverProc export)",
				      mod->name);
		mod_free(mod);
		return NULL;
	}
	return open_instance(mod, proc, alias, lp2, mark);
}

HDRVR drv_open_proc(DRIVERPROC proc, const WCHAR *alias, LPARAM lp2)
{
	if (!proc) {
		vfw_set_error("no driver procedure");
		return NULL;
	}
	if (runtime_enter())
		return NULL;
	return open_instance(NULL, proc, alias, lp2, mod_missing_mark());
}

LRESULT drv_send(HDRVR h, UINT msg, LPARAM l1, LPARAM l2)
{
	pthread_mutex_lock(&lock);
	int s = slot_of(h, 0);
	DRIVERPROC proc = s >= 0 ? instances[s].dm->proc : NULL;
	DWORD_PTR id = s >= 0 ? instances[s].id : 0;
	pthread_mutex_unlock(&lock);

	if (!proc) {
		TRACE("driver %p: message 0x%x to a bad handle", (void *)h, msg);
		return 0;
	}
	return drv_call(proc, id, h, msg, l1, l2);
}

LRESULT drv_close(HDRVR h, LPARAM l1, LPARAM l2)
{
	pthread_mutex_lock(&open_lock);
	pthread_mutex_lock(&lock);
	int s = slot_of(h, 0);
	struct drv_module *dm = s >= 0 ? instances[s].dm : NULL;
	DWORD_PTR id = s >= 0 ? instances[s].id : 0;
	if (dm)
		instances[s].opening = 1;	/* no more messages to it */
	pthread_mutex_unlock(&lock);

	if (!dm) {
		pthread_mutex_unlock(&open_lock);
		return 0;
	}
	LRESULT r = drv_call(dm->proc, id, h, DRV_CLOSE, l1, l2);

	pthread_mutex_lock(&lock);
	instances[s].dm = NULL;
	unsigned left = --dm->opens;
	pthread_mutex_unlock(&lock);
	if (!left)
		unload(dm, h);
	pthread_mutex_unlock(&open_lock);
	return r;
}
