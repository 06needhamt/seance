/* SPDX-License-Identifier: GPL-2.0-only */
/* Minimal DirectShow graph: harness source -> transform -> harness sink. */
#include <errno.h>
#include <pthread.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "dshow/dshow.h"
#include "win32/ole.h"

/* The harness's own filters, which the two host pins belong to. */
struct host_filter {
  IBaseFilter filter;
  struct dshow_host *host;
  struct host_pin *pin;		/* its one pin */
  const WCHAR *name;
};

struct host_pin {
  IPin pin;
  IMemInputPin mem;
  PIN_DIRECTION direction;
  struct dshow_host *host;
  struct host_filter *owner;
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
  struct host_filter source_filter, sink_filter;
  struct host_graph graph;
  dshow_output_fn callback;
  void *opaque;
  pthread_mutex_t mutex;
  pthread_cond_t changed;
  LONG event_code;
  LONG_PTR event_param1, event_param2;
  int have_event, eos, flushing, running;
  HRESULT error;	/* a fatal error a filter reported by event, until a seek */
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
  struct host_pin *pin = (void *)p;
  memset(out, 0, sizeof(*out));
  out->dir = pin->direction;
  out->pFilter = &pin->owner->filter;
  out->pFilter->lpVtbl->AddRef(out->pFilter);
  for (int i = 0; i < MAX_PIN_NAME - 1 && pin->owner->name[i]; i++)
    out->achName[i] = pin->owner->name[i];
  return S_OK;
}
static HRESULT WINAPI pin_direction(IPin *p, PIN_DIRECTION *out) {
  if (!out)
    return E_POINTER;
  *out = ((struct host_pin *)p)->direction;
  return S_OK;
}
static HRESULT WINAPI pin_id(IPin *p, LPWSTR *out) {
  static const WCHAR in[] = {'I', 'n', 0}, outp[] = {'O', 'u', 't', 0};
  if (!out)
    return E_POINTER;
  const WCHAR *id = ((struct host_pin *)p)->direction == PINDIR_INPUT ? in : outp;
  size_t n = 0;
  while (id[n])
    n++;
  *out = ole_taskmem_alloc((n + 1) * sizeof(WCHAR));
  if (!*out)
    return E_OUTOFMEMORY;
  memcpy(*out, id, (n + 1) * sizeof(WCHAR));
  return S_OK;
}
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

/*
 * The filters at the two ends. A filter looks at the filter on the other side
 * of a pin (QueryPinInfo, then GetClassID or QueryFilterInfo) to recognise
 * what it is connected to, so they have to answer; they are always running
 * and have no pins to list.
 */
static struct host_filter *filter_of(IBaseFilter *f) { return (void *)f; }
static HRESULT WINAPI filt_qi(IBaseFilter *f, REFIID iid, void **out) {
  if (!out)
    return E_POINTER;
  if (guid_eq(iid, &IID_IUnknown) || guid_eq(iid, &IID_IPersist) ||
      guid_eq(iid, &IID_IMediaFilter) || guid_eq(iid, &IID_IBaseFilter)) {
    *out = f;
    return S_OK;
  }
  *out = NULL;
  return E_NOINTERFACE;
}
static ULONG WINAPI filt_addref(IBaseFilter *f) { return 2; }
static ULONG WINAPI filt_release(IBaseFilter *f) { return 1; }
static HRESULT WINAPI filt_class(IBaseFilter *f, CLSID *clsid) {
  /* No class is registered for these: a private one names them. */
  static const CLSID id = {0x5ea4ce00, 0x5ea4, 0x4ce0, {0x5e, 0xa4, 0xce, 0x00, 0x5e, 0xa4, 0xce, 0x01}};
  if (!clsid)
    return E_POINTER;
  *clsid = id;
  return S_OK;
}
static HRESULT WINAPI filt_state_change(IBaseFilter *f) { return S_OK; }
static HRESULT WINAPI filt_run(IBaseFilter *f, REFERENCE_TIME start) { return S_OK; }
static HRESULT WINAPI filt_state(IBaseFilter *f, DWORD timeout, FILTER_STATE *st) {
  if (!st)
    return E_POINTER;
  *st = State_Running;
  return S_OK;
}
static HRESULT WINAPI filt_set_clock(IBaseFilter *f, IReferenceClock *c) { return S_OK; }
static HRESULT WINAPI filt_get_clock(IBaseFilter *f, IReferenceClock **c) {
  if (!c)
    return E_POINTER;
  *c = NULL;
  return S_OK;
}

/* IEnumPins over the filter's one pin; a copy of its position per Clone. */
struct host_enum_pins {
  IEnumPins e;
  LONG refs;
  struct host_filter *filter;
  int pos;
};
static HRESULT WINAPI enum_qi(IEnumPins *e, REFIID iid, void **out) {
  if (!out)
    return E_POINTER;
  if (guid_eq(iid, &IID_IUnknown) || guid_eq(iid, &IID_IEnumPins)) {
    *out = e;
    ((struct host_enum_pins *)e)->refs++;
    return S_OK;
  }
  *out = NULL;
  return E_NOINTERFACE;
}
static ULONG WINAPI enum_addref(IEnumPins *e) { return ++((struct host_enum_pins *)e)->refs; }
static ULONG WINAPI enum_release(IEnumPins *e) {
  struct host_enum_pins *x = (void *)e;
  ULONG n = --x->refs;
  if (!n)
    free(x);
  return n;
}
static HRESULT WINAPI enum_next(IEnumPins *e, ULONG count, IPin **pins, ULONG *fetched) {
  struct host_enum_pins *x = (void *)e;
  ULONG got = 0;
  if (!pins || (!fetched && count != 1))
    return E_POINTER;
  if (count && !x->pos) {
    pins[got++] = &x->filter->pin->pin;
    x->pos = 1;
  }
  if (fetched)
    *fetched = got;
  return got == count ? S_OK : S_FALSE;
}
static HRESULT WINAPI enum_skip(IEnumPins *e, ULONG count) {
  struct host_enum_pins *x = (void *)e;
  if (count && x->pos)
    return S_FALSE;
  if (count)
    x->pos = 1;
  return count > 1 ? S_FALSE : S_OK;
}
static HRESULT WINAPI enum_reset(IEnumPins *e) {
  ((struct host_enum_pins *)e)->pos = 0;
  return S_OK;
}
static HRESULT WINAPI enum_clone(IEnumPins *e, IEnumPins **out);
static const IEnumPinsVtbl enum_vtbl = {enum_qi,   enum_addref, enum_release,
                                        enum_next, enum_skip,   enum_reset,
                                        enum_clone};
static HRESULT enum_new(struct host_filter *f, int pos, IEnumPins **out) {
  struct host_enum_pins *x = calloc(1, sizeof(*x));
  if (!out)
    return E_POINTER;
  if (!x)
    return E_OUTOFMEMORY;
  x->e.lpVtbl = &enum_vtbl;
  x->refs = 1;
  x->filter = f;
  x->pos = pos;
  *out = &x->e;
  return S_OK;
}
static HRESULT WINAPI enum_clone(IEnumPins *e, IEnumPins **out) {
  struct host_enum_pins *x = (void *)e;
  return enum_new(x->filter, x->pos, out);
}
static HRESULT WINAPI filt_enum_pins(IBaseFilter *f, IEnumPins **e) {
  return enum_new(filter_of(f), 0, e);
}
static HRESULT WINAPI filt_find_pin(IBaseFilter *f, LPCWSTR id, IPin **pin) {
  struct host_filter *hf = filter_of(f);
  LPWSTR mine = NULL;
  if (!pin)
    return E_POINTER;
  *pin = NULL;
  if (!id || FAILED(pin_id(&hf->pin->pin, &mine)))
    return VFW_E_NOT_FOUND;
  int same = 1;
  for (size_t i = 0; same; i++) {
    same = id[i] == mine[i];
    if (!mine[i])
      break;
  }
  ole_taskmem_free(mine);
  if (!same)
    return VFW_E_NOT_FOUND;
  *pin = &hf->pin->pin;
  return S_OK;
}
static HRESULT WINAPI filt_info(IBaseFilter *f, FILTER_INFO *info) {
  struct host_filter *hf = filter_of(f);
  if (!info)
    return E_POINTER;
  memset(info, 0, sizeof(*info));
  for (int i = 0; i < MAX_FILTER_NAME - 1 && hf->name[i]; i++)
    info->achName[i] = hf->name[i];
  info->pGraph = &hf->host->graph.graph;
  info->pGraph->lpVtbl->AddRef(info->pGraph);
  return S_OK;
}
static HRESULT WINAPI filt_join(IBaseFilter *f, IFilterGraph *g, LPCWSTR name) { return S_OK; }
static HRESULT WINAPI filt_vendor(IBaseFilter *f, LPWSTR *info) { return E_NOTIMPL; }
static const IBaseFilterVtbl filter_vtbl = {
    filt_qi,        filt_addref,    filt_release,    filt_class,
    filt_state_change, filt_state_change, filt_run,  filt_state,
    filt_set_clock, filt_get_clock, filt_enum_pins,  filt_find_pin,
    filt_info,      filt_join,      filt_vendor};

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
  if (dynamic && SUCCEEDED(hr)) {
    /* From here on the connection has this type. */
    AM_MEDIA_TYPE next;
    if (SUCCEEDED(dshow_copy_media_type(&next, dynamic))) {
      dshow_free_media_type(&from_mem(p)->type);
      from_mem(p)->type = next;
    }
  }
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
  /*
   * A filter that fails a sample may report it here and still return S_OK
   * from Receive. These end the stream: the HRESULT is the first parameter.
   */
  if ((code == EC_ERRORABORT || code == EC_ERRORABORTEX ||
       code == EC_STREAM_ERROR_STOPPED) && !h->error)
    h->error = FAILED((HRESULT)a) ? (HRESULT)a : E_FAIL;
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
  static const WCHAR source_name[] = {'S', 'e', 'a', 'n', 'c', 'e', ' ', 'S', 'o', 'u', 'r', 'c', 'e', 0};
  static const WCHAR sink_name[] = {'S', 'e', 'a', 'n', 'c', 'e', ' ', 'S', 'i', 'n', 'k', 0};
  h->source_filter = (struct host_filter){
      .filter = {&filter_vtbl}, .host = h, .pin = &h->source,
      .name = source_name};
  h->sink_filter = (struct host_filter){
      .filter = {&filter_vtbl}, .host = h, .pin = &h->sink,
      .name = sink_name};
  h->source = (struct host_pin){.pin = {&pin_vtbl},
                                .mem = {&mem_vtbl},
                                .direction = PINDIR_OUTPUT,
                                .host = h,
                                .owner = &h->source_filter};
  h->sink = (struct host_pin){.pin = {&pin_vtbl},
                              .mem = {&mem_vtbl},
                              .direction = PINDIR_INPUT,
                              .host = h,
                              .owner = &h->sink_filter};
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

/* The fatal error a filter has reported by event, or S_OK. */
static HRESULT host_error(struct dshow_host *h) {
  pthread_mutex_lock(&h->mutex);
  HRESULT hr = h->error;
  pthread_mutex_unlock(&h->mutex);
  return hr;
}

HRESULT dshow_host_push(struct dshow_host *h, const void *data, size_t size,
                        REFERENCE_TIME start, REFERENCE_TIME stop,
                        DWORD flags) {
  if (!h || (!data && size))
    return E_POINTER;
  if (size > INT32_MAX)
    return E_INVALIDARG;
  IMediaSample *s = NULL;
  HRESULT hr = host_error(h);
  if (FAILED(hr))
    return hr;
  hr = h->allocator->lpVtbl->GetBuffer(h->allocator, &s, &start, &stop, 0);
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
  /* An error the filter reported by event, alongside a Receive that succeeded. */
  if (SUCCEEDED(hr))
    hr = host_error(h);
  return hr;
}

HRESULT dshow_host_seek(struct dshow_host *h, REFERENCE_TIME start,
                        REFERENCE_TIME stop) {
  if (!h)
    return E_POINTER;
  pthread_mutex_lock(&h->mutex);
  h->eos = h->have_event = 0;
  h->error = S_OK;
  pthread_mutex_unlock(&h->mutex);
  HRESULT hr = dshow_call_pin_begin_flush(h->input);
  if (SUCCEEDED(hr))
    hr = dshow_call_pin_end_flush(h->input);
  if (SUCCEEDED(hr))
    hr = dshow_call_pin_new_segment(h->input, start, stop, 1.0);
  return hr;
}

HRESULT dshow_host_end(struct dshow_host *h, DWORD timeout) {
  if (!h)
    return E_POINTER;
  HRESULT hr = dshow_call_pin_end_of_stream(h->input);
  if (FAILED(hr))
    return hr;
  if (!timeout) {
    timeout = 5000;
    const char *setting = getenv("VFW_TIMEOUT");
    if (setting && *setting) {
      char *end;
      unsigned long seconds = strtoul(setting, &end, 10);
      if (!*end && seconds && seconds <= UINT32_MAX / 1000)
        timeout = (DWORD)seconds * 1000;
    }
  }
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
  if (h->error)
    hr = h->error;
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
