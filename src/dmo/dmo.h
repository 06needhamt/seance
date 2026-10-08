/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * Driving a DMO: its IMediaObject created from its DLL, buffers to hand it,
 * and a host that sets its types and runs the ProcessInput/ProcessOutput
 * loop, giving each output to a callback. The DirectShow host's counterpart
 * (dshow/dshow.h), without a graph.
 */
#ifndef VFW_DMO_DMO_H
#define VFW_DMO_DMO_H

#include <stddef.h>

#include "win32.h"

/*
 * An IMediaBuffer of max_length bytes (aligned to at least 16), length 0,
 * or one holding a copy of data. Its memory is below 2 GB, as everything
 * the harness hands a DLL is.
 */
HRESULT dmo_buffer_create(DWORD max_length, DWORD alignment,
			  IMediaBuffer **out);
HRESULT dmo_buffer_from(const void *data, DWORD size, IMediaBuffer **out);

/* One output: data, the DMO_OUTPUT_DATA_BUFFERF_* flags, and its times. */
struct dmo_output {
	const BYTE *data;
	DWORD size;
	DWORD flags;
	REFERENCE_TIME time, length;	/* valid if flags say so */
	int preroll;		/* from input pushed with AM_SAMPLE_PREROLL */
};

/*
 * Called for each output, with the DMO's new output type when it has
 * changed since the last one (else NULL). A failure stops the stream.
 */
typedef HRESULT (*dmo_output_fn)(void *opaque, const struct dmo_output *out,
				 const DMO_MEDIA_TYPE *new_type);

struct dmo_host;

/*
 * Sets the DMO's input and output types (each tried with
 * DMO_SET_TYPEF_TEST_ONLY first) and makes it ready to stream. A partial
 * output type (a major type and perhaps a subtype, no format) becomes the
 * first the DMO offers (GetOutputType) that fits it. If the DMO
 * has IWMCodecPrivateData (encoders), the codec data it gives for the
 * output type is appended to that type's format, as for a stream header.
 */
HRESULT dmo_host_create(IMediaObject *mo, const DMO_MEDIA_TYPE *input,
			const DMO_MEDIA_TYPE *output, dmo_output_fn callback,
			void *opaque, struct dmo_host **host);
/* The output type set, with any codec data appended. */
const DMO_MEDIA_TYPE *dmo_host_output_type(const struct dmo_host *host);
/*
 * One input buffer: AM_SAMPLE_SPLICEPOINT makes it a sync point,
 * AM_SAMPLE_PREROLL marks what comes out of it as preroll; then every
 * output it gives. Times are in 100 ns units.
 */
HRESULT dmo_host_push(struct dmo_host *host, const void *data, size_t size,
		      REFERENCE_TIME start, REFERENCE_TIME stop, DWORD flags);
/* Drops what the DMO holds (a seek). */
HRESULT dmo_host_flush(struct dmo_host *host);
/* End of stream: Discontinuity, and the outputs the DMO still had. */
HRESULT dmo_host_end(struct dmo_host *host);
void dmo_host_destroy(struct dmo_host *host);

/* A DMO class created straight from its DLL (no registration needed). */
struct dmo_object;
HRESULT dmo_object_open(const char *path, const CLSID *clsid,
			struct dmo_object **object, IMediaObject **mo);
void dmo_object_close(struct dmo_object *object, IMediaObject *mo);

#endif /* VFW_DMO_DMO_H */
