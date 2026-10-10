/* SPDX-License-Identifier: GPL-2.0-only */
#include <stdio.h>

#include "acm/acm.h"
#include "common/log.h"
#include "dshow/dshow.h"
#include "runtime/crash.h"
#include "win32/msacm32.h"

const char *dshow_hr_name(HRESULT hr, char buf[24])
{
	switch (hr) {
	case S_OK: return "S_OK";
	case S_FALSE: return "S_FALSE";
	case E_FAIL: return "E_FAIL";
	case E_NOINTERFACE: return "E_NOINTERFACE";
	case E_NOTIMPL: return "E_NOTIMPL";
	case VFW_E_INVALIDMEDIATYPE: return "VFW_E_INVALIDMEDIATYPE";
	case VFW_E_TYPE_NOT_ACCEPTED: return "VFW_E_TYPE_NOT_ACCEPTED";
	case VFW_E_NOT_CONNECTED: return "VFW_E_NOT_CONNECTED";
	case VFW_E_TIMEOUT: return "VFW_E_TIMEOUT";
	case VFW_E_INVALID_FILE_FORMAT: return "VFW_E_INVALID_FILE_FORMAT";
	case VFW_E_SAMPLE_REJECTED: return "VFW_E_SAMPLE_REJECTED";
	case VFW_E_BUFFER_OVERFLOW: return "VFW_E_BUFFER_OVERFLOW";
	case VFW_E_NO_ACCEPTABLE_TYPES: return "VFW_E_NO_ACCEPTABLE_TYPES";
	case VFW_E_DUPLICATE_NAME: return "VFW_E_DUPLICATE_NAME";
	case VFW_E_WRONG_STATE: return "VFW_E_WRONG_STATE";
	case VFW_E_NOT_COMMITTED: return "VFW_E_NOT_COMMITTED";
	case VFW_E_UNSUPPORTED_AUDIO: return "VFW_E_UNSUPPORTED_AUDIO";
	case E_POINTER: return "E_POINTER";
	case E_INVALIDARG: return "E_INVALIDARG";
	case E_OUTOFMEMORY: return "E_OUTOFMEMORY";
	case CLASS_E_CLASSNOTAVAILABLE: return "CLASS_E_CLASSNOTAVAILABLE";
	case DMO_E_INVALIDSTREAMINDEX: return "DMO_E_INVALIDSTREAMINDEX";
	case DMO_E_INVALIDTYPE: return "DMO_E_INVALIDTYPE";
	case DMO_E_TYPE_NOT_SET: return "DMO_E_TYPE_NOT_SET";
	case DMO_E_NOTACCEPTING: return "DMO_E_NOTACCEPTING";
	case DMO_E_TYPE_NOT_ACCEPTED: return "DMO_E_TYPE_NOT_ACCEPTED";
	case DMO_E_NO_MORE_ITEMS: return "DMO_E_NO_MORE_ITEMS";
	default:
		if (ACM_IS_HRESULT(hr))	/* an ACM driver's MMRESULT */
			return msacm_error_name(ACM_MMRESULT(hr), buf);
		snprintf(buf, 24, "0x%08lx", (unsigned long)hr);
		return buf;
	}
}

static void trace_result(const char *method, HRESULT hr)
{
	char name[24];
	TRACE("dshow: %s = %s", method, dshow_hr_name(hr, name));
}

#define CALL(method, fn, expression) do { \
	struct crash_frame saved; \
	TRACE("dshow: %s", method); \
	crash_enter(method, (const void *)(fn), 0, NULL, &saved); \
	HRESULT hr_ = (expression); \
	crash_leave(&saved); \
	trace_result(method, hr_); \
	return hr_; \
} while (0)

HRESULT dshow_call_filter_stop(IBaseFilter *filter)
{
	if (!filter)
		return E_POINTER;
	CALL("IBaseFilter::Stop", filter->lpVtbl->Stop,
	     filter->lpVtbl->Stop(filter));
}

HRESULT dshow_call_filter_pause(IBaseFilter *filter)
{
	if (!filter)
		return E_POINTER;
	CALL("IBaseFilter::Pause", filter->lpVtbl->Pause,
	     filter->lpVtbl->Pause(filter));
}

HRESULT dshow_call_filter_run(IBaseFilter *filter, REFERENCE_TIME start)
{
	if (!filter)
		return E_POINTER;
	CALL("IBaseFilter::Run", filter->lpVtbl->Run,
	     filter->lpVtbl->Run(filter, start));
}

HRESULT dshow_call_filter_get_state(IBaseFilter *filter, DWORD timeout,
				    FILTER_STATE *state)
{
	if (!filter || !state)
		return E_POINTER;
	CALL("IBaseFilter::GetState", filter->lpVtbl->GetState,
	     filter->lpVtbl->GetState(filter, timeout, state));
}

HRESULT dshow_call_filter_set_sync_source(IBaseFilter *filter,
					  IReferenceClock *clock)
{
	if (!filter)
		return E_POINTER;
	CALL("IBaseFilter::SetSyncSource", filter->lpVtbl->SetSyncSource,
	     filter->lpVtbl->SetSyncSource(filter, clock));
}

HRESULT dshow_call_filter_join_graph(IBaseFilter *filter, IFilterGraph *graph,
				     LPCWSTR name)
{
	if (!filter)
		return E_POINTER;
	CALL("IBaseFilter::JoinFilterGraph", filter->lpVtbl->JoinFilterGraph,
	     filter->lpVtbl->JoinFilterGraph(filter, graph, name));
}

HRESULT dshow_call_pin_connect(IPin *pin, IPin *receiver,
			       const AM_MEDIA_TYPE *mt)
{
	if (!pin)
		return E_POINTER;
	CALL("IPin::Connect", pin->lpVtbl->Connect,
	     pin->lpVtbl->Connect(pin, receiver, mt));
}

HRESULT dshow_call_pin_receive_connection(IPin *pin, IPin *connector,
					  const AM_MEDIA_TYPE *mt)
{
	if (!pin)
		return E_POINTER;
	CALL("IPin::ReceiveConnection", pin->lpVtbl->ReceiveConnection,
	     pin->lpVtbl->ReceiveConnection(pin, connector, mt));
}

HRESULT dshow_call_pin_enum_media_types(IPin *pin, IEnumMediaTypes **out)
{
	if (!pin || !out)
		return E_POINTER;
	*out = NULL;
	CALL("IPin::EnumMediaTypes", pin->lpVtbl->EnumMediaTypes,
	     pin->lpVtbl->EnumMediaTypes(pin, out));
}

/* One type; S_FALSE at the end. */
HRESULT dshow_call_enum_media_types_next(IEnumMediaTypes *e, AM_MEDIA_TYPE **mt)
{
	ULONG got = 0;

	if (!e || !mt)
		return E_POINTER;
	*mt = NULL;
	CALL("IEnumMediaTypes::Next", e->lpVtbl->Next,
	     e->lpVtbl->Next(e, 1, mt, &got) == S_OK && got && *mt ? S_OK :
	     S_FALSE);
}

HRESULT dshow_call_pin_disconnect(IPin *pin)
{
	if (!pin)
		return E_POINTER;
	CALL("IPin::Disconnect", pin->lpVtbl->Disconnect,
	     pin->lpVtbl->Disconnect(pin));
}

HRESULT dshow_call_pin_end_of_stream(IPin *pin)
{
	if (!pin)
		return E_POINTER;
	CALL("IPin::EndOfStream", pin->lpVtbl->EndOfStream,
	     pin->lpVtbl->EndOfStream(pin));
}

HRESULT dshow_call_pin_begin_flush(IPin *pin)
{
	if (!pin)
		return E_POINTER;
	CALL("IPin::BeginFlush", pin->lpVtbl->BeginFlush,
	     pin->lpVtbl->BeginFlush(pin));
}

HRESULT dshow_call_pin_end_flush(IPin *pin)
{
	if (!pin)
		return E_POINTER;
	CALL("IPin::EndFlush", pin->lpVtbl->EndFlush,
	     pin->lpVtbl->EndFlush(pin));
}

HRESULT dshow_call_pin_new_segment(IPin *pin, REFERENCE_TIME start,
				   REFERENCE_TIME stop, double rate)
{
	if (!pin)
		return E_POINTER;
	CALL("IPin::NewSegment", pin->lpVtbl->NewSegment,
	     pin->lpVtbl->NewSegment(pin, start, stop, rate));
}

HRESULT dshow_call_pin_query_accept(IPin *pin, const AM_MEDIA_TYPE *mt)
{
	if (!pin || !mt)
		return E_POINTER;
	CALL("IPin::QueryAccept", pin->lpVtbl->QueryAccept,
	     pin->lpVtbl->QueryAccept(pin, mt));
}

HRESULT dshow_call_mem_receive(IMemInputPin *pin, IMediaSample *sample)
{
	if (!pin)
		return E_POINTER;
	CALL("IMemInputPin::Receive", pin->lpVtbl->Receive,
	     pin->lpVtbl->Receive(pin, sample));
}

HRESULT dshow_call_mem_receive_multiple(IMemInputPin *pin,
					IMediaSample **samples, LONG count,
					LONG *processed)
{
	if (!pin || !samples || !processed)
		return E_POINTER;
	if (count < 0)
		return E_INVALIDARG;
	CALL("IMemInputPin::ReceiveMultiple", pin->lpVtbl->ReceiveMultiple,
	     pin->lpVtbl->ReceiveMultiple(pin, samples, count, processed));
}

HRESULT dshow_call_mem_get_allocator(IMemInputPin *pin,
				     IMemAllocator **allocator)
{
	if (!pin || !allocator)
		return E_POINTER;
	*allocator = NULL;
	CALL("IMemInputPin::GetAllocator", pin->lpVtbl->GetAllocator,
	     pin->lpVtbl->GetAllocator(pin, allocator));
}

HRESULT dshow_call_mem_notify_allocator(IMemInputPin *pin,
					IMemAllocator *allocator, BOOL read_only)
{
	if (!pin || !allocator)
		return E_POINTER;
	CALL("IMemInputPin::NotifyAllocator", pin->lpVtbl->NotifyAllocator,
	     pin->lpVtbl->NotifyAllocator(pin, allocator, read_only));
}

HRESULT dshow_call_mem_allocator_requirements(IMemInputPin *pin,
					      ALLOCATOR_PROPERTIES *props)
{
	if (!pin || !props)
		return E_POINTER;
	CALL("IMemInputPin::GetAllocatorRequirements",
	     pin->lpVtbl->GetAllocatorRequirements,
	     pin->lpVtbl->GetAllocatorRequirements(pin, props));
}

HRESULT dshow_call_mem_receive_can_block(IMemInputPin *pin)
{
	if (!pin)
		return E_POINTER;
	CALL("IMemInputPin::ReceiveCanBlock", pin->lpVtbl->ReceiveCanBlock,
	     pin->lpVtbl->ReceiveCanBlock(pin));
}
