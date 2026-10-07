/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * Finding DMOs: what they are for, the ones registered on this machine
 * (HKCR\DirectShow\MediaObjects, read by msdmo), and printing them. The
 * ones a server registers come from dshow_server_discover.
 */
#ifndef VFW_DMO_DISCOVER_H
#define VFW_DMO_DISCOVER_H

#include <stddef.h>

#include "win32.h"
#include "win32/msdmo.h"

/* What a DMO is for, from its category, else from its types. */
enum dmo_role {
	DMO_ROLE_OTHER,
	DMO_ROLE_VIDEO_DECODER,
	DMO_ROLE_VIDEO_ENCODER,
	DMO_ROLE_AUDIO_DECODER,
	DMO_ROLE_AUDIO_ENCODER,
	DMO_ROLE_EFFECT,
};

enum dmo_role dmo_role(const struct dmo_reg *d);
const char *dmo_role_name(enum dmo_role role);
/* A decoder or an encoder, video or audio. */
int dmo_role_is_codec(enum dmo_role role);

/* A DMO registered on this machine, in one category. */
struct dmo_registered {
	struct dmo_reg d;
	char *path;		/* its InprocServer32 (UTF-8), or NULL */
};

/* The DMOs registered; malloc()ed, to free with dmo_registered_free. */
size_t dmo_registered(struct dmo_registered **out);
void dmo_registered_free(struct dmo_registered *r, size_t n);

/* A category's name: DMOCATEGORY_..., else as dshow_guid_name gives it. */
const char *dmo_category_name(const GUID *category, char *buf);

/*
 * A DMO: its CLSID and name, role and category, its server's path if
 * given, and with types, its input and output types.
 */
void dmo_print(const struct dmo_reg *d, const char *path, int types);

#endif /* VFW_DMO_DISCOVER_H */
