/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * ole32 internals shared between its source files, and with the other
 * built-in DLLs.
 */
#ifndef VFW_WIN32_OLE_H
#define VFW_WIN32_OLE_H

#include "runtime/builtin.h"
#include "win32.h"

/* "{xxxxxxxx-xxxx-xxxx-xxxx-xxxxxxxxxxxx}" and its NUL. */
#define OLE_GUID_CHARS 39

/* A GUID in registry form, upper case, as StringFromGUID2 writes it. */
void ole_guid_format(const GUID *g, char out[OLE_GUID_CHARS]);

/* Parses exactly that form (either case): 0, or -1 if s isn't one. */
int ole_guid_parse(const WCHAR *s, GUID *out);

/*
 * CreateStreamOnHGlobal: a stream over h (a new moveable block if NULL),
 * which is freed with the last reference if delete_on_release.
 */
HRESULT ole_stream_on_hglobal(HGLOBAL h, BOOL delete_on_release,
			      IStream **out);

/* The streams and in-process marshalling (ole_stream.c). */
extern const struct builtin_export ole_stream_exports[];

#endif /* VFW_WIN32_OLE_H */
