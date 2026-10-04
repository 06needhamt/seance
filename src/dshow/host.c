/* SPDX-License-Identifier: GPL-2.0-only */
/* Minimal DirectShow graph: harness source -> transform -> harness sink. */
#include <errno.h>
#include <pthread.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "dshow/dshow.h"
#include "win32/ole.h"

struct host_pin {
  IPin pin;
  IMemInputPin mem;
  PIN_DIRECTION direction;
  struct dshow_host *host;
  IPin *peer;
  AM_MEDIA_TYPE type;
  int connected;
};

struct host_graph {
  IFilterGraph graph;
  IMediaEventSink events;
  struct dshow_host *host;
};

struct dshow_host {
  IBaseFilter *filter;
  IPin *input, *output;
  IMemInputPin *mem;
  IMemAllocator *allocator;
  struct host_pin source, sink;
  struct host_graph graph;
  dshow_output_fn callback;
  void *opaque;
  pthread_mutex_t mutex;
  pthread_cond_t changed;
  LONG event_code;
  LONG_PTR event_param1, event_param2;
  int have_event, eos, flushing, running;
};

static int guid_eq(REFIID a, REFIID b) { return !memcmp(a, b, sizeof(*a)); }
static struct host_pin *from_mem(IMemInputPin *p) {
  return (void *)((char *)p - offsetof(struct host_pin, mem));
}
static struct host_graph *from_events(IMediaEventSink *p) {
  return (void *)((char *)p - offsetof(struct host_graph, events));
}

static HRESULT find_pin(IBaseFilter *f, PIN_DIRECTION want, IPin **out) {
  IEnumPins *e = NULL;
  *out = NULL;
  HRESULT hr = f->lpVtbl->EnumPins(f, &e);
  if (FAILED(hr))
    return hr;
  for (;;) {
    IPin *p = NULL;
    ULONG got = 0;
    PIN_DIRECTION direction;
    if (e->lpVtbl->Next(e, 1, &p, &got) != S_OK || !got)
      break;
    if (SUCCEEDED(p->lpVtbl->QueryDirection(p, &direction)) &&
        direction == want) {
      *out = p;
      break;
    }
    p->lpVtbl->Release(p);
  }
  e->lpVtbl->Release(e);
  return *out ? S_OK : VFW_E_NOT_FOUND;
}

/* Host pins are embedded in the host, so their references are non-owning. */
static HRESULT WINAPI pin_qi(IPin *p, REFIID iid, void **out) {
  if (!out)
    return E_POINTER;
  struct host_pin *pin = (void *)p;
  if (guid_eq(iid, &IID_IUnknown) || guid_eq(iid, &IID_IPin))
    *out = p;
  else if (pin->direction == PINDIR_INPUT && guid_eq(iid, &IID_IMemInputPin))
    *out = &pin->mem;
  else {
    *out = NULL;
    return E_NOINTERFACE;
  }
  return S_OK;
}
static ULONG WINAPI pin_addref(IPin *p) { return 2; }
static ULONG WINAPI pin_release(IPin *p) { return 1; }
static HRESULT WINAPI pin_connect(IPin *p, IPin *receiver,
                                  const AM_MEDIA_TYPE *mt) {
  return E_NOTIMPL;
}
static HRESULT WINAPI pin_receive_connection(IPin *p, IPin *peer,
                                             const AM_MEDIA_TYPE *mt) {
  struct host_pin *pin = (void *)p;
  if (!peer || !mt)
    return E_POINTER;
  if (pin->direction != PINDIR_INPUT)
    return VFW_E_INVALID_DIRECTION;
  if (pin->connected)
    return VFW_E_ALREADY_CONNECTED;
  HRESULT hr = dshow_copy_media_type(&pin->type, mt);
  if (FAILED(hr))
    return hr;
  pin->peer = peer;
  peer->lpVtbl->AddRef(peer);
  pin->connected = 1;
  return S_OK;
}
static HRESULT WINAPI pin_disconnect(IPin *p) {
  struct host_pin *pin = (void *)p;
  if (!pin->connected)
    return S_FALSE;
  pin->peer->lpVtbl->Release(pin->peer);
  pin->peer = NULL;
  dshow_free_media_type(&pin->type);
  pin->connected = 0;
  return S_OK;
}
static HRESULT WINAPI pin_connected(IPin *p, IPin **out) {
  if (!out)
    return E_POINTER;
  struct host_pin *pin = (void *)p;
  if (!pin->connected) {
    *out = NULL;
    return VFW_E_NOT_CONNECTED;
  }
  *out = pin->peer;
  pin->peer->lpVtbl->AddRef(pin->peer);
  return S_OK;
}
static HRESULT WINAPI pin_type(IPin *p, AM_MEDIA_TYPE *out) {
  struct host_pin *x = (void *)p;
  return !out            ? E_POINTER
         : !x->connected ? VFW_E_NOT_CONNECTED
                         : dshow_copy_media_type(out, &x->type);
}
static HRESULT WINAPI pin_info(IPin *p, PIN_INFO *out) {
  if (!out)
    return E_POINTER;
  memset(out, 0, sizeof(*out));
  out->dir = ((struct host_pin *)p)->direction;
  return S_OK;
}
static HRESULT WINAPI pin_direction(IPin *p, PIN_DIRECTION *out) {
  if (!out)
    return E_POINTER;
  *out = ((struct host_pin *)p)->direction;
  return S_OK;
}
static HRESULT WINAPI pin_id(IPin *p, LPWSTR *out) { return E_NOTIMPL; }
static HRESULT WINAPI pin_accept(IPin *p, const AM_MEDIA_TYPE *mt) {
  return mt && guid_eq(&mt->majortype, &MEDIATYPE_Video) ? S_OK : S_FALSE;
}
static HRESULT WINAPI pin_enum(IPin *p, IEnumMediaTypes **out) {
  return E_NOTIMPL;
}
static HRESULT WINAPI pin_internal(IPin *p, IPin **out, ULONG *n) {
  return E_NOTIMPL;
}
static HRESULT WINAPI pin_eos(IPin *p) {
  struct dshow_host *h = ((struct host_pin *)p)->host;
  pthread_mutex_lock(&h->mutex);
  h->eos = 1;
  pthread_cond_broadcast(&h->changed);
  pthread_mutex_unlock(&h->mutex);
  return S_OK;
}
static HRESULT WINAPI pin_begin_flush(IPin *p) {
  ((struct host_pin *)p)->host->flushing = 1;
  return S_OK;
}
static HRESULT WINAPI pin_end_flush(IPin *p) {
  ((struct host_pin *)p)->host->flushing = 0;
  return S_OK;
}
static HRESULT WINAPI pin_segment(IPin *p, REFERENCE_TIME a, REFERENCE_TIME b,
                                  double r) {
  return S_OK;
}
static const IPinVtbl pin_vtbl = {pin_qi,
                                  pin_addref,
                                  pin_release,
                                  pin_connect,
                                  pin_receive_connection,
                                  pin_disconnect,
                                  pin_connected,
                                  pin_type,
                                  pin_info,
                                  pin_direction,
                                  pin_id,
                                  pin_accept,
                                  pin_enum,
                                  pin_internal,
                                  pin_eos,
                                  pin_begin_flush,
                                  pin_end_flush,
                                  pin_segment};

static HRESULT WINAPI mem_qi(IMemInputPin *p, REFIID iid, void **out) {
  return pin_qi(&from_mem(p)->pin, iid, out);
}
static ULONG WINAPI mem_addref(IMemInputPin *p) { return 2; }
static ULONG WINAPI mem_release(IMemInputPin *p) { return 1; }
static HRESULT WINAPI mem_allocator(IMemInputPin *p, IMemAllocator **out) {
  return dshow_allocator_create(out);
}
static HRESULT WINAPI mem_notify(IMemInputPin *p, IMemAllocator *a, BOOL ro) {
  return a ? S_OK : E_POINTER;
}
static HRESULT WINAPI mem_require(IMemInputPin *p, ALLOCATOR_PROPERTIES *a) {
  if (!a)
    return E_POINTER;
  memset(a, 0, sizeof(*a));
  a->cBuffers = 1;
  a->cbAlign = 1;
  return S_OK;
}
static HRESULT WINAPI mem_receive(IMemInputPin *p, IMediaSample *s) {
  if (!s)
    return E_POINTER;
  struct dshow_host *h = from_mem(p)->host;
  if (h->flushing)
    return S_FALSE;
  AM_MEDIA_TYPE *dynamic = NULL;
  s->lpVtbl->GetMediaType(s, &dynamic);
  HRESULT hr = h->callback ? h->callback(h->opaque, s, dynamic) : S_OK;
  if (dynamic) {
    dshow_free_media_type(dynamic);
    ole_taskmem_free(dynamic);
  }
  return hr;
}
static HRESULT WINAPI mem_multiple(IMemInputPin *p, IMediaSample **s, LONG n,
                                   LONG *done) {
  if (!s || !done)
    return E_POINTER;
  *done = 0;
  for (; *done < n; (*done)++) {
    HRESULT hr = mem_receive(p, s[*done]);
    if (FAILED(hr))
      return hr;
  }
  return S_OK;
}
static HRESULT WINAPI mem_can_block(IMemInputPin *p) { return S_FALSE; }
static const IMemInputPinVtbl mem_vtbl = {
    mem_qi,      mem_addref,  mem_release,  mem_allocator, mem_notify,
    mem_require, mem_receive, mem_multiple, mem_can_block};

static HRESULT WINAPI graph_qi(IFilterGraph *p, REFIID iid, void **out) {
  if (!out)
    return E_POINTER;
  struct host_graph *g = (void *)p;
  if (guid_eq(iid, &IID_IUnknown) || guid_eq(iid, &IID_IFilterGraph))
    *out = p;
  else if (guid_eq(iid, &IID_IMediaEventSink))
    *out = &g->events;
  else {
    *out = NULL;
    return E_NOINTERFACE;
  }
  return S_OK;
}
static ULONG WINAPI graph_addref(IFilterGraph *p) { return 2; }
static ULONG WINAPI graph_release(IFilterGraph *p) { return 1; }
static HRESULT WINAPI graph_add(IFilterGraph *p, IBaseFilter *f, LPCWSTR n) {
  return E_NOTIMPL;
}
static HRESULT WINAPI graph_remove(IFilterGraph *p, IBaseFilter *f) {
  return E_NOTIMPL;
}
static HRESULT WINAPI graph_enum(IFilterGraph *p, IEnumFilters **e) {
  return E_NOTIMPL;
}
static HRESULT WINAPI graph_find(IFilterGraph *p, LPCWSTR n, IBaseFilter **f) {
  return E_NOTIMPL;
}
static HRESULT WINAPI graph_connect(IFilterGraph *p, IPin *a, IPin *b,
                                    const AM_MEDIA_TYPE *m) {
  return dshow_call_pin_connect(a, b, m);
}
static HRESULT WINAPI graph_reconnect(IFilterGraph *p, IPin *a) {
  return E_NOTIMPL;
}
static HRESULT WINAPI graph_disconnect(IFilterGraph *p, IPin *a) {
  return dshow_call_pin_disconnect(a);
}
static HRESULT WINAPI graph_clock(IFilterGraph *p) { return S_OK; }
static const IFilterGraphVtbl graph_vtbl = {
    graph_qi,        graph_addref,     graph_release, graph_add,
    graph_remove,    graph_enum,       graph_find,    graph_connect,
    graph_reconnect, graph_disconnect, graph_clock};
static HRESULT WINAPI event_qi(IMediaEventSink *p, REFIID iid, void **out) {
  return graph_qi(&from_events(p)->graph, iid, out);
}
static ULONG WINAPI event_addref(IMediaEventSink *p) { return 2; }
static ULONG WINAPI event_release(IMediaEventSink *p) { return 1; }
static HRESULT WINAPI event_notify(IMediaEventSink *p, LONG code, LONG_PTR a,
                                   LONG_PTR b) {
  struct dshow_host *h = from_events(p)->host;
  pthread_mutex_lock(&h->mutex);
  h->event_code = code;
  h->event_param1 = a;
  h->event_param2 = b;
  h->have_event = 1;
  if (code == EC_COMPLETE)
    h->eos = 1;
  pthread_cond_broadcast(&h->changed);
  pthread_mutex_unlock(&h->mutex);
  return S_OK;
}
static const IMediaEventSinkVtbl event_vtbl = {event_qi, event_addref,
                                               event_release, event_notify};

static HRESULT negotiate_allocator(struct dshow_host *h, ULONG bytes) {
  HRESULT hr = dshow_call_mem_get_allocator(h->mem, &h->allocator);
  if (FAILED(hr)) {
    hr = dshow_allocator_create(&h->allocator);
    if (FAILED(hr))
      return hr;
  }
  ALLOCATOR_PROPERTIES want = {1, (LONG)(bytes ? bytes : 1), 1, 0}, req, actual;
  if (SUCCEEDED(dshow_call_mem_allocator_requirements(h->mem, &req))) {
    if (req.cBuffers > want.cBuffers)
      want.cBuffers = req.cBuffers;
    if (req.cbBuffer > want.cbBuffer)
      want.cbBuffer = req.cbBuffer;
    if (req.cbAlign > want.cbAlign)
      want.cbAlign = req.cbAlign;
    if (req.cbPrefix > want.cbPrefix)
      want.cbPrefix = req.cbPrefix;
  }
  hr = h->allocator->lpVtbl->SetProperties(h->allocator, &want, &actual);
  if (SUCCEEDED(hr))
    hr = dshow_call_mem_notify_allocator(h->mem, h->allocator, FALSE);
  if (SUCCEEDED(hr))
    hr = h->allocator->lpVtbl->Commit(h->allocator);
  return hr;
}

HRESULT dshow_host_create(IBaseFilter *f, const AM_MEDIA_TYPE *in,
                          const AM_MEDIA_TYPE *out, dshow_output_fn callback,
                          void *opaque, struct dshow_host **result) {
  if (!f || !in || !out || !result)
    return E_POINTER;
  *result = NULL;
  struct dshow_host *h = calloc(1, sizeof(*h));
  if (!h)
    return E_OUTOFMEMORY;
  h->filter = f;
  f->lpVtbl->AddRef(f);
  h->callback = callback;
  h->opaque = opaque;
  pthread_mutex_init(&h->mutex, NULL);
  pthread_cond_init(&h->changed, NULL);
  h->source = (struct host_pin){.pin = {&pin_vtbl},
                                .mem = {&mem_vtbl},
                                .direction = PINDIR_OUTPUT,
                                .host = h};
  h->sink = (struct host_pin){.pin = {&pin_vtbl},
                              .mem = {&mem_vtbl},
                              .direction = PINDIR_INPUT,
                              .host = h};
  h->graph = (struct host_graph){
      .graph = {&graph_vtbl}, .events = {&event_vtbl}, .host = h};
  static const WCHAR name[] = {'s', 'e', 'a', 'n', 'c', 'e', 0};
  HRESULT hr = find_pin(f, PINDIR_INPUT, &h->input);
  if (SUCCEEDED(hr))
    hr = find_pin(f, PINDIR_OUTPUT, &h->output);
  if (SUCCEEDED(hr))
    hr = h->input->lpVtbl->QueryInterface(h->input, &IID_IMemInputPin,
                                          (void **)&h->mem);
  if (SUCCEEDED(hr))
    hr = dshow_call_filter_join_graph(f, &h->graph.graph, name);
  if (SUCCEEDED(hr))
    hr = dshow_call_filter_set_sync_source(f, NULL);
  if (SUCCEEDED(hr))
    hr = dshow_call_pin_receive_connection(h->input, &h->source.pin, in);
  if (SUCCEEDED(hr))
    hr = dshow_call_pin_connect(h->output, &h->sink.pin, out);
  if (SUCCEEDED(hr))
    hr = negotiate_allocator(h, in->lSampleSize);
  if (SUCCEEDED(hr))
    hr = dshow_call_filter_pause(f);
  if (SUCCEEDED(hr))
    hr = dshow_call_filter_run(f, 0);
  if (FAILED(hr)) {
    dshow_host_destroy(h);
    return hr;
  }
  h->running = 1;
  *result = h;
  return S_OK;
}

HRESULT dshow_host_push(struct dshow_host *h, const void *data, size_t size,
                        REFERENCE_TIME start, REFERENCE_TIME stop,
                        DWORD flags) {
  if (!h || (!data && size))
    return E_POINTER;
  if (size > INT32_MAX)
    return E_INVALIDARG;
  IMediaSample *s = NULL;
  HRESULT hr =
      h->allocator->lpVtbl->GetBuffer(h->allocator, &s, &start, &stop, 0);
  if (FAILED(hr))
    return hr;
  BYTE *p = NULL;
  if ((size_t)s->lpVtbl->GetSize(s) < size)
    hr = VFW_E_BUFFER_OVERFLOW;
  else if (SUCCEEDED(hr = s->lpVtbl->GetPointer(s, &p))) {
    memcpy(p, data, size);
    s->lpVtbl->SetActualDataLength(s, size);
    s->lpVtbl->SetSyncPoint(s, !!(flags & AM_SAMPLE_SPLICEPOINT));
    s->lpVtbl->SetPreroll(s, !!(flags & AM_SAMPLE_PREROLL));
    s->lpVtbl->SetDiscontinuity(s, !!(flags & AM_SAMPLE_DATADISCONTINUITY));
    hr = dshow_call_mem_receive(h->mem, s);
  }
  s->lpVtbl->Release(s);
  return hr;
}

HRESULT dshow_host_seek(struct dshow_host *h, REFERENCE_TIME start,
                        REFERENCE_TIME stop) {
  if (!h)
    return E_POINTER;
  HRESULT hr = dshow_call_pin_begin_flush(h->input);
  if (SUCCEEDED(hr))
    hr = dshow_call_pin_begin_flush(h->output);
  if (SUCCEEDED(hr))
    hr = dshow_call_pin_end_flush(h->input);
  if (SUCCEEDED(hr))
    hr = dshow_call_pin_end_flush(h->output);
  if (SUCCEEDED(hr))
    hr = dshow_call_pin_new_segment(h->input, start, stop, 1.0);
  pthread_mutex_lock(&h->mutex);
  h->eos = h->have_event = 0;
  pthread_mutex_unlock(&h->mutex);
  return hr;
}

HRESULT dshow_host_end(struct dshow_host *h, DWORD timeout) {
  if (!h)
    return E_POINTER;
  HRESULT hr = dshow_call_pin_end_of_stream(h->input);
  if (FAILED(hr))
    return hr;
  struct timespec ts;
  clock_gettime(CLOCK_REALTIME, &ts);
  ts.tv_sec += timeout / 1000;
  ts.tv_nsec += (timeout % 1000) * 1000000ull;
  if (ts.tv_nsec >= 1000000000) {
    ts.tv_sec++;
    ts.tv_nsec -= 1000000000;
  }
  pthread_mutex_lock(&h->mutex);
  while (!h->eos) {
    int e = pthread_cond_timedwait(&h->changed, &h->mutex, &ts);
    if (e == ETIMEDOUT) {
      hr = VFW_E_TIMEOUT;
      break;
    }
  }
  if (h->have_event && h->event_code == EC_ERRORABORT)
    hr = h->event_param1 ? (HRESULT)h->event_param1 : E_FAIL;
  pthread_mutex_unlock(&h->mutex);
  return hr;
}

HRESULT dshow_host_event(struct dshow_host *h, LONG *c, LONG_PTR *a,
                         LONG_PTR *b) {
  if (!h || !c || !a || !b)
    return E_POINTER;
  pthread_mutex_lock(&h->mutex);
  if (!h->have_event) {
    pthread_mutex_unlock(&h->mutex);
    return S_FALSE;
  }
  *c = h->event_code;
  *a = h->event_param1;
  *b = h->event_param2;
  h->have_event = 0;
  pthread_mutex_unlock(&h->mutex);
  return S_OK;
}

void dshow_host_destroy(struct dshow_host *h) {
  if (!h)
    return;
  if (h->running)
    dshow_call_filter_stop(h->filter);
  if (h->allocator) {
    h->allocator->lpVtbl->Decommit(h->allocator);
    h->allocator->lpVtbl->Release(h->allocator);
  }
  if (h->output)
    dshow_call_pin_disconnect(h->output);
  if (h->input)
    dshow_call_pin_disconnect(h->input);
  pin_disconnect(&h->sink.pin);
  pin_disconnect(&h->source.pin);
  if (h->mem)
    h->mem->lpVtbl->Release(h->mem);
  if (h->input)
    h->input->lpVtbl->Release(h->input);
  if (h->output)
    h->output->lpVtbl->Release(h->output);
  dshow_call_filter_join_graph(h->filter, NULL, NULL);
  h->filter->lpVtbl->Release(h->filter);
  pthread_cond_destroy(&h->changed);
  pthread_mutex_destroy(&h->mutex);
  free(h);
}
