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
/*
 * The ways a stream's format can be offered as an input type, one per
 * variant, in the order to try them: VideoInfo, VideoInfo2, and for MPEG-1
 * and MPEG-2 video the subtypes decoders know them by. S_OK with the type,
 * or S_FALSE when there are no more variants.
 */
HRESULT dshow_input_variant(const BITMAPINFOHEADER *bih, size_t size,
			    DWORD rate, DWORD scale, unsigned variant,
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

/*
 * These take any COM object, a filter (IBaseFilter) or a DMO (IMediaObject),
 * and ask it for what they need: IAMVideoCompression or ICodecAPI, and
 * IPersistStream for the state.
 */
HRESULT dshow_encoder_configure(void *object,
				const struct dshow_encoder_options *options);
/*
 * Which of the optional interfaces an encoder or decoder has
 * (IAMVideoCompression, ICodecAPI, IPersistStream, ISpecifyPropertyPages,
 * IWMCodecPrivateData, IPropertyBag), names separated by commas in buf.
 * Returns how many.
 */
int dshow_filter_interfaces(void *object, char *buf, size_t size);
HRESULT dshow_filter_load_state(void *object, const void *data, size_t size);
HRESULT dshow_filter_save_state(void *object, void **data, size_t *size);

/* Human-readable HRESULT used by tracing and diagnostics. */
const char *dshow_hr_name(HRESULT hr, char buf[24]);

/* All host-to-filter calls go through these crash-framed trace wrappers. */
HRESULT dshow_call_filter_stop(IBaseFilter *filter);
HRESULT dshow_call_filter_pause(IBaseFilter *filter);
HRESULT dshow_call_filter_run(IBaseFilter *filter, REFERENCE_TIME start);
HRESULT dshow_call_filter_get_state(IBaseFilter *filter, DWORD timeout,
				    FILTER_STATE *state);
HRESULT dshow_call_filter_set_sync_source(IBaseFilter *filter,
					  IReferenceClock *clock);
HRESULT dshow_call_filter_join_graph(IBaseFilter *filter, IFilterGraph *graph,
				     LPCWSTR name);
HRESULT dshow_call_pin_connect(IPin *pin, IPin *receiver,
			       const AM_MEDIA_TYPE *mt);
HRESULT dshow_call_pin_receive_connection(IPin *pin, IPin *connector,
					  const AM_MEDIA_TYPE *mt);
HRESULT dshow_call_pin_disconnect(IPin *pin);
HRESULT dshow_call_pin_end_of_stream(IPin *pin);
HRESULT dshow_call_pin_begin_flush(IPin *pin);
HRESULT dshow_call_pin_end_flush(IPin *pin);
HRESULT dshow_call_pin_new_segment(IPin *pin, REFERENCE_TIME start,
				   REFERENCE_TIME stop, double rate);
HRESULT dshow_call_pin_query_accept(IPin *pin, const AM_MEDIA_TYPE *mt);
HRESULT dshow_call_pin_enum_media_types(IPin *pin, IEnumMediaTypes **out);
HRESULT dshow_call_enum_media_types_next(IEnumMediaTypes *e,
					 AM_MEDIA_TYPE **mt);
HRESULT dshow_call_mem_receive(IMemInputPin *pin, IMediaSample *sample);
HRESULT dshow_call_mem_receive_multiple(IMemInputPin *pin,
					IMediaSample **samples, LONG count,
					LONG *processed);
HRESULT dshow_call_mem_get_allocator(IMemInputPin *pin,
				     IMemAllocator **allocator);
HRESULT dshow_call_mem_notify_allocator(IMemInputPin *pin,
					IMemAllocator *allocator,
					BOOL read_only);
HRESULT dshow_call_mem_allocator_requirements(IMemInputPin *pin,
					      ALLOCATOR_PROPERTIES *props);
HRESULT dshow_call_mem_receive_can_block(IMemInputPin *pin);

/* Clockless one-filter graph used by both decoding and encoding. */
struct dshow_host;
struct dshow_filter;
typedef HRESULT (*dshow_output_fn)(void *opaque, IMediaSample *sample,
				   const AM_MEDIA_TYPE *dynamic_type);

/*
 * Connects filter between the host's source and sink, input in and output
 * out, and runs it. out may be partial (a major type and subtype, with no
 * format): the first of the output pin's own types it stands for is
 * connected, as in a graph builder's connection, else the filter is left
 * to complete it. dshow_host_output_type gives the type connected.
 */
HRESULT dshow_host_create(IBaseFilter *filter, const AM_MEDIA_TYPE *input,
			  const AM_MEDIA_TYPE *output, dshow_output_fn callback,
			  void *opaque, struct dshow_host **host);
/* The output connection's type, as changed by any dynamic change since. */
const AM_MEDIA_TYPE *dshow_host_output_type(const struct dshow_host *host);
HRESULT dshow_host_push(struct dshow_host *host, const void *data, size_t size,
			REFERENCE_TIME start, REFERENCE_TIME stop, DWORD flags);
HRESULT dshow_host_seek(struct dshow_host *host, REFERENCE_TIME start,
			REFERENCE_TIME stop);
HRESULT dshow_host_end(struct dshow_host *host, DWORD timeout_ms);
HRESULT dshow_host_event(struct dshow_host *host, LONG *code,
			 LONG_PTR *param1, LONG_PTR *param2);
void dshow_host_destroy(struct dshow_host *host);

/* Load one transform class directly from a DirectShow server. */
HRESULT dshow_filter_open(const char *path, const CLSID *selected,
			  int encoder, struct dshow_filter **server,
			  IBaseFilter **filter, CLSID *clsid);
/* One class known to be a filter, created without looking at the others. */
HRESULT dshow_filter_create(const char *path, const CLSID *clsid,
			    struct dshow_filter **server, IBaseFilter **filter);
void dshow_filter_close(struct dshow_filter *server, IBaseFilter *filter);

#endif
