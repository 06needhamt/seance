/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * DMOs as the commands see them: roles judged from the category a DMO is
 * registered in (or, without one, from its types), the DMOs registered on
 * this machine with the DLL serving each, and printing.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "dmo/discover.h"
#include "dshow/discover.h"
#include "win32/codepage.h"
#include "win32/ole.h"

static const GUID guid_null;

static int guid_eq(const GUID *a, const GUID *b)
{
	return !memcmp(a, b, sizeof(GUID));
}

/* Roles. */

static const struct {
	const GUID *category;
	enum dmo_role role;
} category_roles[] = {
	{ &DMOCATEGORY_VIDEO_DECODER, DMO_ROLE_VIDEO_DECODER },
	{ &DMOCATEGORY_VIDEO_ENCODER, DMO_ROLE_VIDEO_ENCODER },
	{ &DMOCATEGORY_AUDIO_DECODER, DMO_ROLE_AUDIO_DECODER },
	{ &DMOCATEGORY_AUDIO_ENCODER, DMO_ROLE_AUDIO_ENCODER },
	{ &DMOCATEGORY_AUDIO_EFFECT, DMO_ROLE_EFFECT },
	{ &DMOCATEGORY_VIDEO_EFFECT, DMO_ROLE_EFFECT },
	{ &DMOCATEGORY_AUDIO_CAPTURE_EFFECT, DMO_ROLE_EFFECT },
	{ &DMOCATEGORY_ACOUSTIC_ECHO_CANCEL, DMO_ROLE_EFFECT },
	{ &DMOCATEGORY_AUDIO_NOISE_SUPPRESS, DMO_ROLE_EFFECT },
	{ &DMOCATEGORY_AGC, DMO_ROLE_EFFECT },
};

static int uncompressed_audio(const GUID *subtype)
{
	return guid_eq(subtype, &MEDIASUBTYPE_PCM) ||
	       guid_eq(subtype, &MEDIASUBTYPE_IEEE_FLOAT);
}

/*
 * Whether one of the types is of major type major (or any) and compressed
 * (want 1: a named subtype that isn't raw) or raw (want 0: raw, or any).
 */
static int has_type(const DMO_PARTIAL_MEDIATYPE *t, size_t n,
		    const GUID *major, int compressed)
{
	int video = guid_eq(major, &MEDIATYPE_Video);

	for (size_t i = 0; i < n; i++) {
		if (!guid_eq(&t[i].type, major) &&
		    !guid_eq(&t[i].type, &guid_null))
			continue;
		int any = guid_eq(&t[i].subtype, &guid_null);
		int raw = any || (video ? dshow_uncompressed_video(&t[i].subtype) :
				  uncompressed_audio(&t[i].subtype));
		if (compressed ? !raw : raw)
			return 1;
	}
	return 0;
}

enum dmo_role dmo_role(const struct dmo_reg *d)
{
	for (size_t i = 0; i < sizeof(category_roles) / sizeof(*category_roles);
	     i++)
		if (guid_eq(&d->category, category_roles[i].category))
			return category_roles[i].role;
	if (has_type(d->in, d->nin, &MEDIATYPE_Video, 1) &&
	    has_type(d->out, d->nout, &MEDIATYPE_Video, 0))
		return DMO_ROLE_VIDEO_DECODER;
	if (has_type(d->in, d->nin, &MEDIATYPE_Video, 0) &&
	    has_type(d->out, d->nout, &MEDIATYPE_Video, 1))
		return DMO_ROLE_VIDEO_ENCODER;
	if (has_type(d->in, d->nin, &MEDIATYPE_Audio, 1) &&
	    has_type(d->out, d->nout, &MEDIATYPE_Audio, 0))
		return DMO_ROLE_AUDIO_DECODER;
	if (has_type(d->in, d->nin, &MEDIATYPE_Audio, 0) &&
	    has_type(d->out, d->nout, &MEDIATYPE_Audio, 1))
		return DMO_ROLE_AUDIO_ENCODER;
	return DMO_ROLE_OTHER;
}

const char *dmo_role_name(enum dmo_role role)
{
	switch (role) {
	case DMO_ROLE_VIDEO_DECODER: return "video decoder";
	case DMO_ROLE_VIDEO_ENCODER: return "video encoder";
	case DMO_ROLE_AUDIO_DECODER: return "audio decoder";
	case DMO_ROLE_AUDIO_ENCODER: return "audio encoder";
	case DMO_ROLE_EFFECT: return "effect";
	default: return "other";
	}
}

int dmo_role_is_codec(enum dmo_role role)
{
	return role == DMO_ROLE_VIDEO_DECODER ||
	       role == DMO_ROLE_VIDEO_ENCODER ||
	       role == DMO_ROLE_AUDIO_DECODER || role == DMO_ROLE_AUDIO_ENCODER;
}

/* Registered DMOs. */

size_t dmo_registered(struct dmo_registered **out)
{
	struct dmo_reg *d = NULL;
	size_t n = msdmo_registered(&d);
	struct dmo_registered *r = n ? calloc(n, sizeof(*r)) : NULL;

	*out = NULL;
	if (!r) {
		dmo_regs_free(d, n);
		return 0;
	}
	for (size_t i = 0; i < n; i++) {
		r[i].d = d[i];
		r[i].path = dshow_server_path(&d[i].clsid);
	}
	free(d);
	*out = r;
	return n;
}

void dmo_registered_free(struct dmo_registered *r, size_t n)
{
	for (size_t i = 0; i < n; i++) {
		dmo_reg_clear(&r[i].d);
		free(r[i].path);
	}
	free(r);
}

/* Printing. */

#define NAME(n, l, w1, w2, b1, b2, b3, b4, b5, b6, b7, b8) { &n, #n },

static const struct {
	const GUID *guid;
	const char *name;
} category_names[] = {
	VFW_DMO_GUIDS(NAME)
};

const char *dmo_category_name(const GUID *category, char *buf)
{
	if (guid_eq(category, &guid_null)) {
		snprintf(buf, DSHOW_GUID_NAME_SIZE, "none");
		return buf;
	}
	for (size_t i = 0; i < sizeof(category_names) / sizeof(*category_names);
	     i++) {
		if (guid_eq(category, category_names[i].guid)) {
			snprintf(buf, DSHOW_GUID_NAME_SIZE, "%s",
				 category_names[i].name);
			return buf;
		}
	}
	return dshow_guid_name(category, buf);
}

static void print_types(const char *dir, const DMO_PARTIAL_MEDIATYPE *t,
			size_t n)
{
	char a[DSHOW_GUID_NAME_SIZE], b[DSHOW_GUID_NAME_SIZE];

	if (!n)
		printf("      %s types: none registered\n", dir);
	for (size_t i = 0; i < n; i++)
		printf("      %s %s / %s\n", dir,
		       dshow_guid_name(&t[i].type, a),
		       dshow_guid_name(&t[i].subtype, b));
}

void dmo_print(const struct dmo_reg *d, const char *path, int types)
{
	char g[OLE_GUID_CHARS], cat[DSHOW_GUID_NAME_SIZE];
	char *name = d->name ? utf16_to_utf8_dup(d->name) : NULL;

	ole_guid_format(&d->clsid, g);
	printf("  %s %s\n", g, name && *name ? name : "(no name)");
	printf("      DMO, %s, category %s%s\n", dmo_role_name(dmo_role(d)),
	       dmo_category_name(&d->category, cat),
	       (d->flags & DMO_REGISTERF_IS_KEYED) ? ", keyed" : "");
	if (path)
		printf("      server %s\n", path);
	if (types) {
		print_types("input", d->in, d->nin);
		print_types("output", d->out, d->nout);
	}
	free(name);
}
