/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * Finding DirectShow filters: the ones a filter server (an .ax DLL)
 * registers, found by running its DllRegisterServer in a registry sandbox,
 * and the ones registered on this machine, read from the registry as
 * Windows' filter mapper keeps them.
 */
#ifndef VFW_DSHOW_DISCOVER_H
#define VFW_DSHOW_DISCOVER_H

#include <stddef.h>

#include "loader/pe.h"
#include "win32.h"
#include "win32/quartz.h"

/* What a filter is for, judged from its pins and category. */
enum dshow_role {
	DSHOW_OTHER,
	DSHOW_DECODER,		/* compressed video in, uncompressed out */
	DSHOW_ENCODER,		/* the reverse, or a video compressor */
};

enum dshow_role dshow_role(const struct qz_filter *f);
const char *dshow_role_name(enum dshow_role role);

/*
 * Whether an image is a filter server rather than a VfW codec: it exports
 * DllGetClassObject and not DriverProc.
 */
int dshow_is_server(const struct pe_image *img);

/*
 * Runs the DllRegisterServer of the DLL at path, with the registry
 * sandboxed and quartz capturing, and gives the filters it registers whose
 * class is served by this DLL (its InprocServer32 names the file, or it
 * has none); malloc()ed, *n of them, to free with qz_filters_free. Nothing
 * reaches the real registry. *hr is what DllRegisterServer returned (any
 * filters registered before a failure are still given). Returns 0, or -1
 * with vfw_error() if the DLL doesn't load or has no DllRegisterServer.
 */
int dshow_discover(const char *path, struct qz_filter **out, size_t *n,
		   HRESULT *hr);

/*
 * DllRegisterServer (or DllUnregisterServer) for real, giving what it
 * registered as dshow_discover does (nothing, for unregistering).
 */
int dshow_register(const char *path, int unregister, struct qz_filter **out,
		   size_t *n, HRESULT *hr);

/* A filter registered on this machine. */
struct dshow_registered {
	struct qz_filter f;
	char *path;		/* its InprocServer32 (UTF-8), or NULL */
};

/*
 * The filters registered in every filter category (HKCR\CLSID\{category}
 * \Instance, the categories being those under ActiveMovieCategories plus
 * the legacy and video compressor ones) and the legacy way (HKCR\Filter);
 * malloc()ed, to free with dshow_registered_free. Returns the count.
 */
size_t dshow_registered_filters(struct dshow_registered **out);
void dshow_registered_free(struct dshow_registered *r, size_t n);

/*
 * A GUID's name for people: "MEDIATYPE_Video", a FOURCC subtype as
 * 'TST0', else its {...} form. buf needs DSHOW_GUID_NAME_SIZE bytes.
 */
#define DSHOW_GUID_NAME_SIZE 48
const char *dshow_guid_name(const GUID *g, char *buf);

/* A filter's pins and media types, one line each, indented by indent. */
void dshow_print_pins(const struct qz_filter *f, int indent);

/*
 * A filter: its CLSID and name, role, merit and category, its server's
 * path if given, and with pins, its pins.
 */
void dshow_print_filter(const struct qz_filter *f, const char *path,
			int pins);

#endif /* VFW_DSHOW_DISCOVER_H */
