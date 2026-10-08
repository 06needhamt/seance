/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * A COM transform of video or audio, a DirectShow filter or a DMO, behind one
 * interface, so decode, encode and probe drive either the same way: open
 * the one decoder (or encoder) a DLL has, connect it with an input and an
 * output type, push frames, and take what comes out through a callback.
 */
#ifndef VFW_XFORM_H
#define VFW_XFORM_H

#include <stddef.h>

#include "win32.h"

/* A class in a server: which one, what it is, what an encoder makes. */
struct xform_class {
	CLSID clsid;
	int dmo;		/* a DMO, else a DirectShow filter */
	/* An encoder's output: a FOURCC for video, a format tag for audio. */
	DWORD compression;
};

/* One output, from a filter's sample or a DMO's buffer. */
struct xform_output {
	const BYTE *data;
	size_t size;
	int sync, preroll, timed;
	REFERENCE_TIME start, stop;
};

/* Called for each output, with the new type when it has changed. */
typedef HRESULT (*xform_output_fn)(void *opaque, const struct xform_output *o,
				   const AM_MEDIA_TYPE *new_type);

struct xform;

/*
 * The decoder (or encoder) of video (or audio) among the filters and DMOs
 * the server at path registers, or the one with CLSID selected: S_OK and
 * *found, or CLASS_E_CLASSNOTAVAILABLE for none, VFW_E_DUPLICATE_NAME for
 * several. An encoder counts only if it names its output: a FOURCC for
 * video, a format tag for audio.
 */
HRESULT xform_find(const char *path, const CLSID *selected, int audio,
		   int encoder, struct xform_class *found);
/* Creates the class (found, or known) and holds it. */
HRESULT xform_create(const char *path, const struct xform_class *c,
		     struct xform **x);
/* xform_find, then xform_create. */
HRESULT xform_open(const char *path, const CLSID *selected, int audio,
		   int encoder, struct xform **x, struct xform_class *found);
void xform_close(struct xform *x);

/* "DirectShow filter" or "DMO". */
const char *xform_kind_name(const struct xform *x);
/* The COM object (IBaseFilter or IMediaObject), for the dshow_* controls. */
void *xform_object(const struct xform *x);

/*
 * Connects with an input and an output type (or fails, the transform then
 * left unconnected for another try). The callback may be NULL to only see
 * whether the types are taken. A filter may be given a partial output type
 * (major type and subtype only) to complete; xform_output_type then says
 * what it chose.
 */
HRESULT xform_connect(struct xform *x, const AM_MEDIA_TYPE *in,
		      const AM_MEDIA_TYPE *out, xform_output_fn callback,
		      void *opaque);
/* The output type connected, with any codec data a DMO encoder added. */
const AM_MEDIA_TYPE *xform_output_type(const struct xform *x);
void xform_disconnect(struct xform *x);

/* AM_SAMPLE_SPLICEPOINT, _PREROLL and _DATADISCONTINUITY flags. */
HRESULT xform_push(struct xform *x, const void *data, size_t size,
		   REFERENCE_TIME start, REFERENCE_TIME stop, DWORD flags);
/* Before going back to start (the frames there on are stop at most). */
HRESULT xform_seek(struct xform *x, REFERENCE_TIME start,
		   REFERENCE_TIME stop);
/* End of stream: everything still held comes out. */
HRESULT xform_end(struct xform *x);

#endif /* VFW_XFORM_H */
