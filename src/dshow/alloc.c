/* SPDX-License-Identifier: GPL-2.0-only */
#include <pthread.h>
#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "dshow/dshow.h"
#include "win32/ole.h"

struct allocator;
struct sample {
	IMediaSample2 iface;
	struct allocator *allocator;
	ULONG refs;
	BYTE *allocation, *data;
	LONG size, actual;
	REFERENCE_TIME start, stop;
	LONGLONG media_start, media_stop;
	DWORD flags, type_flags, stream_id;
	AM_MEDIA_TYPE mt;
	int have_time, have_media_time, have_mt, busy;
};

struct allocator {
	IMemAllocator iface;
	ULONG refs;
	pthread_mutex_t lock;
	pthread_cond_t available;
	ALLOCATOR_PROPERTIES props;
	struct sample *samples;
	int committed;
};

static int guid_eq(REFIID a, REFIID b) { return !memcmp(a, b, sizeof(*a)); }
static struct sample *sample_from(IMediaSample2 *p) { return (void *)p; }
static struct allocator *allocator_from(IMemAllocator *p) { return (void *)p; }
static ULONG WINAPI alloc_release(IMemAllocator *p);

static HRESULT WINAPI sample_qi(IMediaSample2 *p, REFIID iid, void **out)
{
	if (!out) return E_POINTER;
	if (guid_eq(iid, &IID_IUnknown) || guid_eq(iid, &IID_IMediaSample) ||
	    guid_eq(iid, &IID_IMediaSample2)) {
		*out = p; p->lpVtbl->AddRef(p); return S_OK;
	}
	*out = NULL; return E_NOINTERFACE;
}
static ULONG WINAPI sample_addref(IMediaSample2 *p)
{
	struct sample *s = sample_from(p);
	return __atomic_add_fetch(&s->refs, 1, __ATOMIC_RELAXED);
}
static ULONG WINAPI sample_release(IMediaSample2 *p)
{
	struct sample *s = sample_from(p);
	ULONG n = __atomic_sub_fetch(&s->refs, 1, __ATOMIC_ACQ_REL);
	if (!n) {
		struct allocator *a = s->allocator;
		/* As strmbase's CMediaSample: the data length stays as it was. */
		dshow_free_media_type(&s->mt); s->have_mt = 0;
		s->flags = s->type_flags = s->stream_id = 0;
		s->have_time = s->have_media_time = 0;
		pthread_mutex_lock(&a->lock);
		s->busy = 0;
		pthread_cond_signal(&a->available);
		pthread_mutex_unlock(&a->lock);
		alloc_release(&a->iface);
	}
	return n;
}
static HRESULT WINAPI sample_pointer(IMediaSample2 *p, BYTE **out)
{ if (!out) return E_POINTER; *out = sample_from(p)->data; return S_OK; }
static LONG WINAPI sample_size(IMediaSample2 *p) { return sample_from(p)->size; }
static HRESULT WINAPI sample_get_time(IMediaSample2 *p, REFERENCE_TIME *a, REFERENCE_TIME *b)
{ struct sample *s=sample_from(p); if (!s->have_time) return VFW_E_SAMPLE_TIME_NOT_SET; if(a)memcpy(a,&s->start,sizeof(*a));if(b)memcpy(b,&s->stop,sizeof(*b));return S_OK; }
static HRESULT WINAPI sample_set_time(IMediaSample2 *p, REFERENCE_TIME *a, REFERENCE_TIME *b)
{ struct sample*s=sample_from(p);s->have_time=a!=NULL;if(a)memcpy(&s->start,a,sizeof(*a));if(b)memcpy(&s->stop,b,sizeof(*b));else s->stop=s->start;return S_OK; }
static HRESULT flag_get(struct sample*s,DWORD f){return s->flags&f?S_OK:S_FALSE;}
static HRESULT flag_set(struct sample*s,DWORD f,BOOL v){if(v)s->flags|=f;else s->flags&=~f;return S_OK;}
static HRESULT WINAPI sample_sync(IMediaSample2*p){return flag_get(sample_from(p),AM_SAMPLE_SPLICEPOINT);}
static HRESULT WINAPI sample_set_sync(IMediaSample2*p,BOOL v){return flag_set(sample_from(p),AM_SAMPLE_SPLICEPOINT,v);}
static HRESULT WINAPI sample_preroll(IMediaSample2*p){return flag_get(sample_from(p),AM_SAMPLE_PREROLL);}
static HRESULT WINAPI sample_set_preroll(IMediaSample2*p,BOOL v){return flag_set(sample_from(p),AM_SAMPLE_PREROLL,v);}
static LONG WINAPI sample_actual(IMediaSample2*p){return sample_from(p)->actual;}
static HRESULT WINAPI sample_set_actual(IMediaSample2*p,LONG n){struct sample*s=sample_from(p);if(n<0||n>s->size)return VFW_E_BUFFER_OVERFLOW;s->actual=n;return S_OK;}
static HRESULT WINAPI sample_get_mt(IMediaSample2*p,AM_MEDIA_TYPE **out)
{
	struct sample*s=sample_from(p); if(!out)return E_POINTER; *out=NULL;
	if(!s->have_mt)return S_FALSE;
	*out=ole_taskmem_alloc(sizeof(**out));
	if(!*out)return E_OUTOFMEMORY;
	HRESULT hr=dshow_copy_media_type(*out,&s->mt); if(FAILED(hr)){ole_taskmem_free(*out);*out=NULL;} return hr;
}
static HRESULT WINAPI sample_set_mt(IMediaSample2*p,AM_MEDIA_TYPE *mt)
{
	struct sample*s=sample_from(p);
	/* SetProperties hands back the type GetProperties pointed at: ours. */
	if(mt&&mt==&s->mt)return S_OK;
	AM_MEDIA_TYPE copy={0};
	if(mt){HRESULT hr=dshow_copy_media_type(&copy,mt);if(FAILED(hr)){dshow_free_media_type(&copy);return hr;}}
	dshow_free_media_type(&s->mt);
	s->mt=copy;s->have_mt=mt!=NULL;
	return S_OK;
}
static HRESULT WINAPI sample_disc(IMediaSample2*p){return flag_get(sample_from(p),AM_SAMPLE_DATADISCONTINUITY);}
static HRESULT WINAPI sample_set_disc(IMediaSample2*p,BOOL v){return flag_set(sample_from(p),AM_SAMPLE_DATADISCONTINUITY,v);}
static HRESULT WINAPI sample_get_media(IMediaSample2*p,LONGLONG*a,LONGLONG*b){struct sample*s=sample_from(p);if(!s->have_media_time)return VFW_E_MEDIA_TIME_NOT_SET;if(a)memcpy(a,&s->media_start,sizeof(*a));if(b)memcpy(b,&s->media_stop,sizeof(*b));return S_OK;}
static HRESULT WINAPI sample_set_media(IMediaSample2*p,LONGLONG*a,LONGLONG*b){struct sample*s=sample_from(p);s->have_media_time=a!=NULL;if(a)memcpy(&s->media_start,a,sizeof(*a));if(b)memcpy(&s->media_stop,b,sizeof(*b));return S_OK;}
/*
 * Whether the first n bytes of an AM_SAMPLE2_PROPERTIES hold field f. As
 * strmbase's CMediaSample, the properties may be any leading part of the
 * structure: CTransformFilter::InitializeOutputSample reads them up to
 * tStart and copies them up to dwStreamId, which is how a transform's
 * output gets its input's sync point, times and flags.
 */
#define HAS_FIELD(f, n) (offsetof(AM_SAMPLE2_PROPERTIES, f) + \
			 sizeof(((AM_SAMPLE2_PROPERTIES *)0)->f) <= (n))

static HRESULT WINAPI sample_get_props(IMediaSample2 *p, DWORD n, BYTE *out)
{
	if (!n)
		return S_OK;
	if (!out)
		return E_POINTER;
	struct sample *s = sample_from(p);
	AM_SAMPLE2_PROPERTIES q = { 0 };
	q.cbData = n < sizeof(q) ? n : sizeof(q);
	q.dwTypeSpecificFlags = s->type_flags;
	q.dwSampleFlags = s->flags |
		(s->have_time ? AM_SAMPLE_TIMEVALID | AM_SAMPLE_STOPVALID : 0) |
		(s->have_mt ? AM_SAMPLE_TYPECHANGED : 0);
	q.lActual = s->actual;
	q.tStart = s->start;
	q.tStop = s->stop;
	q.dwStreamId = s->stream_id;
	q.pMediaType = s->have_mt ? &s->mt : NULL;
	q.pbBuffer = s->data;
	q.cbBuffer = s->size;
	memcpy(out, &q, q.cbData);
	return S_OK;
}

static HRESULT WINAPI sample_set_props(IMediaSample2 *p, DWORD n, const BYTE *in)
{
	struct sample *s = sample_from(p);
	AM_SAMPLE2_PROPERTIES q = { 0 };

	if (!HAS_FIELD(cbData, n))
		return S_OK;
	if (!in)
		return E_POINTER;
	memcpy(&q, in, n < sizeof(q) ? n : sizeof(q));
	/* No more than is there. */
	if (q.cbData < n)
		n = q.cbData;
	if (n > sizeof(q))
		return E_INVALIDARG;
	/* Checks first, so a refusal changes nothing. */
	if (HAS_FIELD(pbBuffer, n) && q.pbBuffer && q.pbBuffer != s->data)
		return E_INVALIDARG;
	if (HAS_FIELD(cbBuffer, n) && q.cbBuffer && q.cbBuffer != s->size)
		return E_INVALIDARG;
	if (HAS_FIELD(lActual, n) && (q.lActual < 0 || q.lActual > s->size))
		return VFW_E_BUFFER_OVERFLOW;
	if (HAS_FIELD(pMediaType, n) &&
	    (q.dwSampleFlags & AM_SAMPLE_TYPECHANGED) && !q.pMediaType)
		return E_POINTER;
	if (HAS_FIELD(dwStreamId, n))
		s->stream_id = q.dwStreamId;
	if (HAS_FIELD(dwTypeSpecificFlags, n))
		s->type_flags = q.dwTypeSpecificFlags;
	if (HAS_FIELD(dwSampleFlags, n)) {
		s->flags = q.dwSampleFlags &
			   ~(AM_SAMPLE_TIMEVALID | AM_SAMPLE_STOPVALID |
			     AM_SAMPLE_TYPECHANGED);
		s->have_time = !!(q.dwSampleFlags & AM_SAMPLE_TIMEVALID);
	}
	if (HAS_FIELD(lActual, n))
		s->actual = q.lActual;
	if (HAS_FIELD(tStart, n))
		s->start = q.tStart;
	if (HAS_FIELD(tStop, n))
		s->stop = q.tStop;
	if (HAS_FIELD(pMediaType, n))
		return sample_set_mt(p, q.dwSampleFlags & AM_SAMPLE_TYPECHANGED ?
				     q.pMediaType : NULL);
	return S_OK;
}

static const IMediaSample2Vtbl sample_vtbl={sample_qi,sample_addref,sample_release,sample_pointer,sample_size,sample_get_time,sample_set_time,sample_sync,sample_set_sync,sample_preroll,sample_set_preroll,sample_actual,sample_set_actual,sample_get_mt,sample_set_mt,sample_disc,sample_set_disc,sample_get_media,sample_set_media,sample_get_props,sample_set_props};

static void allocator_clear(struct allocator*a){if(!a->samples)return;for(LONG i=0;i<a->props.cBuffers;i++)free(a->samples[i].allocation);free(a->samples);a->samples=NULL;}
static HRESULT WINAPI alloc_qi(IMemAllocator*p,REFIID iid,void**out){if(!out)return E_POINTER;if(guid_eq(iid,&IID_IUnknown)||guid_eq(iid,&IID_IMemAllocator)){*out=p;p->lpVtbl->AddRef(p);return S_OK;}*out=NULL;return E_NOINTERFACE;}
static ULONG WINAPI alloc_addref(IMemAllocator*p){return __atomic_add_fetch(&allocator_from(p)->refs,1,__ATOMIC_RELAXED);}
static ULONG WINAPI alloc_release(IMemAllocator*p){struct allocator*a=allocator_from(p);ULONG n=__atomic_sub_fetch(&a->refs,1,__ATOMIC_ACQ_REL);if(!n){allocator_clear(a);pthread_cond_destroy(&a->available);pthread_mutex_destroy(&a->lock);free(a);}return n;}
static HRESULT WINAPI alloc_set(IMemAllocator*p,ALLOCATOR_PROPERTIES*r,ALLOCATOR_PROPERTIES*actual)
{struct allocator*a=allocator_from(p);if(!r||!actual)return E_POINTER;if(a->committed)return VFW_E_ALREADY_COMMITTED;if(r->cBuffers<=0||r->cbBuffer<=0)return E_INVALIDARG;a->props=*r;if(a->props.cbAlign<=0)a->props.cbAlign=1;if((a->props.cbAlign&(a->props.cbAlign-1)))return VFW_E_BADALIGN;*actual=a->props;return S_OK;}
static HRESULT WINAPI alloc_get(IMemAllocator*p,ALLOCATOR_PROPERTIES*out){if(!out)return E_POINTER;*out=allocator_from(p)->props;return S_OK;}
static HRESULT WINAPI alloc_commit(IMemAllocator*p)
{
	struct allocator*a=allocator_from(p);if(a->committed)return S_OK;if(a->props.cBuffers<=0||a->props.cbBuffer<=0)return VFW_E_SIZENOTSET;
	a->samples=calloc(a->props.cBuffers,sizeof(*a->samples));if(!a->samples)return E_OUTOFMEMORY;
	for(LONG i=0;i<a->props.cBuffers;i++){struct sample*s=&a->samples[i];s->iface.lpVtbl=&sample_vtbl;s->allocator=a;s->size=a->props.cbBuffer;s->actual=s->size;/* a new CMediaSample is full */size_t n=(size_t)s->size+a->props.cbPrefix+a->props.cbAlign-1;s->allocation=malloc(n);if(!s->allocation){allocator_clear(a);return E_OUTOFMEMORY;}uintptr_t q=(uintptr_t)(s->allocation+a->props.cbPrefix);q=(q+a->props.cbAlign-1)&~(uintptr_t)(a->props.cbAlign-1);s->data=(BYTE*)q;}
	a->committed=1;return S_OK;
}
static HRESULT WINAPI alloc_decommit(IMemAllocator*p){struct allocator*a=allocator_from(p);pthread_mutex_lock(&a->lock);a->committed=0;int busy=0;for(LONG i=0;a->samples&&i<a->props.cBuffers;i++)busy|=a->samples[i].busy;if(!busy)allocator_clear(a);pthread_cond_broadcast(&a->available);pthread_mutex_unlock(&a->lock);return S_OK;}
static HRESULT WINAPI alloc_buffer(IMemAllocator*p,IMediaSample**out,REFERENCE_TIME*a,REFERENCE_TIME*b,DWORD flags)
{struct allocator*x=allocator_from(p);if(!out)return E_POINTER;*out=NULL;pthread_mutex_lock(&x->lock);for(;;){if(!x->committed){pthread_mutex_unlock(&x->lock);return VFW_E_NOT_COMMITTED;}for(LONG i=0;i<x->props.cBuffers;i++){struct sample*s=&x->samples[i];if(!s->busy){s->busy=1;s->refs=1;__atomic_add_fetch(&x->refs,1,__ATOMIC_RELAXED);sample_set_time(&s->iface,a,b);*out=(IMediaSample*)&s->iface;pthread_mutex_unlock(&x->lock);return S_OK;}}if(flags&AM_GBF_NOWAIT){pthread_mutex_unlock(&x->lock);return VFW_E_TIMEOUT;}pthread_cond_wait(&x->available,&x->lock);}}
static HRESULT WINAPI alloc_release_buffer(IMemAllocator*p,IMediaSample*s){if(!s)return E_POINTER;s->lpVtbl->Release(s);return S_OK;}
static const IMemAllocatorVtbl alloc_vtbl={alloc_qi,alloc_addref,alloc_release,alloc_set,alloc_get,alloc_commit,alloc_decommit,alloc_buffer,alloc_release_buffer};

HRESULT dshow_allocator_create(IMemAllocator **out)
{if(!out)return E_POINTER;struct allocator*a=calloc(1,sizeof(*a));if(!a)return E_OUTOFMEMORY;a->iface.lpVtbl=&alloc_vtbl;a->refs=1;a->props=(ALLOCATOR_PROPERTIES){1,1,1,0};pthread_mutex_init(&a->lock,NULL);pthread_cond_init(&a->available,NULL);*out=&a->iface;return S_OK;}
