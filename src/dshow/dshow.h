/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef VFW_DSHOW_H
#define VFW_DSHOW_H

#include <stddef.h>
#include "win32.h"

/* Media-type helpers.  Objects returned by these routines use COM task memory. */
void dshow_free_media_type(AM_MEDIA_TYPE *mt);
HRESULT dshow_copy_media_type(AM_MEDIA_TYPE *dst, const AM_MEDIA_TYPE *src);
HRESULT dshow_bih_to_mt(const BITMAPINFOHEADER *bih, size_t size,
			 DWORD rate, DWORD scale, int video_info2,
			 AM_MEDIA_TYPE *mt);
HRESULT dshow_mt_to_bih(const AM_MEDIA_TYPE *mt, BITMAPINFOHEADER **bih,
			 size_t *size);

/* A small allocator suitable for either side of a transform connection. */
HRESULT dshow_allocator_create(IMemAllocator **out);

/* Encoder controls.  Negative members are left at the filter's defaults. */
struct dshow_encoder_options {
	LONG key_interval;
	double quality; /* DirectShow's 0.0 .. 1.0 scale. */
	LONGLONG bitrate;
};

HRESULT dshow_encoder_configure(IBaseFilter *filter,
				const struct dshow_encoder_options *options);
HRESULT dshow_filter_load_state(IBaseFilter *filter, const void *data,
				size_t size);
HRESULT dshow_filter_save_state(IBaseFilter *filter, void **data,
				size_t *size);

/* Human-readable HRESULT used by tracing and diagnostics. */
const char *dshow_hr_name(HRESULT hr, char buf[24]);

/* All host-to-filter calls go through these crash-framed trace wrappers. */
HRESULT dshow_call_filter_stop(IBaseFilter *filter);
HRESULT dshow_call_filter_pause(IBaseFilter *filter);
HRESULT dshow_call_filter_run(IBaseFilter *filter, REFERENCE_TIME start);
HRESULT dshow_call_pin_connect(IPin *pin, IPin *receiver,
			       const AM_MEDIA_TYPE *mt);
HRESULT dshow_call_pin_receive_connection(IPin *pin, IPin *connector,
					  const AM_MEDIA_TYPE *mt);
HRESULT dshow_call_pin_disconnect(IPin *pin);
HRESULT dshow_call_pin_end_of_stream(IPin *pin);
HRESULT dshow_call_mem_receive(IMemInputPin *pin, IMediaSample *sample);

#endif
