/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * The built-in quartz.dll's internals, for the DirectShow host: filter
 * registrations as data, the FilterData blob IFilterMapper2 keeps them in,
 * and a capture mode that records what a DLL's DllRegisterServer registers.
 */
#ifndef VFW_WIN32_QUARTZ_H
#define VFW_WIN32_QUARTZ_H

#include <stddef.h>

#include "win32.h"

struct qz_type {
	GUID major;
	GUID minor;		/* GUID_NULL (MEDIASUBTYPE_NULL) for any */
};

struct qz_pin {
	WCHAR *name;		/* only legacy IFilterMapper pins have one */
	DWORD flags;		/* REG_PINFLAG_B_* */
	UINT instances;
	int has_category;
	GUID category;
	size_t ntypes;
	struct qz_type *types;
	size_t nmediums;
	REGPINMEDIUM *mediums;
};

struct qz_filter {
	CLSID clsid;
	CLSID category;		/* CLSID_LegacyAmFilterCategory by default */
	WCHAR *name;
	DWORD merit;
	size_t npins;
	struct qz_pin *pins;
};

/* Frees what f holds (not f itself). */
void qz_filter_clear(struct qz_filter *f);
/* Frees an array of n filters and the array. */
void qz_filters_free(struct qz_filter *f, size_t n);

/*
 * The FilterData value IFilterMapper2::RegisterFilter writes, laid out as
 * Windows lays it out (version 2): a header, then each pin ("0pi3") with
 * its media types ("0ty3"), mediums and category as offsets into a table
 * of GUIDs and mediums at the end. malloc()ed, its size in *size; NULL if
 * out of memory.
 */
BYTE *qz_filterdata_write(DWORD merit, const struct qz_pin *pins,
			  size_t npins, DWORD *size);

/*
 * Reads a FilterData value back into f's merit and pins (names NULL),
 * checking every count and offset. 0, or -1 if it is malformed (f is then
 * left empty).
 */
int qz_filterdata_parse(const BYTE *data, DWORD size, struct qz_filter *f);

/*
 * Capture mode: from quartz_capture_begin() every filter registered
 * through either filter mapper, by any thread, is also recorded, until
 * quartz_capture_end() hands the records over (malloc()ed, n of them;
 * free with qz_filters_free). Registrations still go to the registry as
 * usual; reg_sandbox_begin() keeps them out of the real one.
 */
void quartz_capture_begin(void);
size_t quartz_capture_end(struct qz_filter **out);

#endif /* VFW_WIN32_QUARTZ_H */
