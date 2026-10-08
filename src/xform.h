/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * A transform of video or audio, a DirectShow filter, a DMO or (for audio)
 * an ACM driver, behind one interface, so decode, encode and probe drive
 * each the same way: open the one decoder (or encoder) a DLL has, connect
 * it with an input and an output type, push frames, and take what comes
 * out through a callback.
 */
#ifndef VFW_XFORM_H
#define VFW_XFORM_H

#include <stddef.h>

#include "win32.h"

/* A class in a server: which one, what it is, what an encoder makes. */
struct xform_class {
	CLSID clsid;		/* GUID_NULL for an ACM driver */
	int dmo;		/* a DMO */
	int acm;		/* an ACM driver; neither: a DirectShow filter */
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
/*
 * The same, with an encoder's output tag (audio) or FOURCC (video) to
 * choose by when not 0. A DLL that exports DriverProc and no
 * DllGetClassObject is an ACM driver to this (audio only, no CLSIDs).
 */
HRESULT xform_find_output(const char *path, const CLSID *selected, int audio,
			  int encoder, DWORD output, struct xform_class *found);
/* Creates the class (found, or known) and holds it. */
HRESULT xform_create(const char *path, const struct xform_class *c,
		     struct xform **x);
/* xform_find, then xform_create. */
HRESULT xform_open(const char *path, const CLSID *selected, int audio,
		   int encoder, struct xform **x, struct xform_class *found);
void xform_close(struct xform *x);

/* "DirectShow filter", "DMO" or "ACM driver". */
const char *xform_kind_name(const struct xform *x);
/*
 * The COM object (IBaseFilter or IMediaObject), for the dshow_* controls;
 * NULL for an ACM driver, which has none of them.
 */
void *xform_object(const struct xform *x);

/*
 * An encoder's settings, before connecting: the dshow_* controls for a
 * filter or DMO. An ACM driver takes a bit rate (its format of the output
 * tag, for the input's rate and channels, with the nearest data rate)
 * and E_NOTIMPL for a quality or key frame interval.
 */
struct dshow_encoder_options;
HRESULT xform_encoder_options(struct xform *x,
			      const struct dshow_encoder_options *o);

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
