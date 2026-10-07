/* SPDX-License-Identifier: GPL-2.0-only */
/* Small CRT-less DirectShow transform used to exercise the Linux host. */
#include "fixture.h"
#include <strmif.h>
#include <amvideo.h>
#include <vfwmsgs.h>

struct filter;
struct pin {
	IPin iface;
	IMemInputPin mem;
	struct filter *filter;
	PIN_DIRECTION direction;
	IPin *peer;
	IMemInputPin *peer_mem;
	IMemAllocator *allocator;
	AM_MEDIA_TYPE type;
	BOOL connected, flushing;
};
struct filter {
	IBaseFilter iface;
	IAMVideoCompression compression;
	ICodecAPI codec_api;
	IPersistStream persist;
	LONG refs;
	BOOL encoder;
	FILTER_STATE state;
	IFilterGraph *graph;
	IReferenceClock *clock;
	struct pin input, output;
	BYTE *picture;
	DWORD picture_size, frame;
	BOOL valid;
	LONG key_interval, pframes;
	double quality;
	DWORDLONG window;
	BOOL async, no_eos, require_segment, segment, peer_info, dynamic, dynamic_extra, no_preroll, codec_api_only;
	LONG workers;
	HRESULT async_error;
	CRITICAL_SECTION transform_lock;
};
struct enum_pins { IEnumPins iface; LONG refs; struct filter *filter; ULONG pos; };

static LONG objects;
static const GUID iid_unknown={0,0,0,{0xc0,0,0,0,0,0,0,0x46}};
static const GUID iid_filter={0x56a86895,0x0ad4,0x11ce,{0xb0,0x3a,0,0x20,0xaf,0x0b,0xa7,0x70}};
static const GUID iid_pin={0x56a86891,0x0ad4,0x11ce,{0xb0,0x3a,0,0x20,0xaf,0x0b,0xa7,0x70}};
static const GUID iid_enum_pins={0x56a86892,0x0ad4,0x11ce,{0xb0,0x3a,0,0x20,0xaf,0x0b,0xa7,0x70}};
static const GUID iid_mem_input={0x56a8689d,0x0ad4,0x11ce,{0xb0,0x3a,0,0x20,0xaf,0x0b,0xa7,0x70}};
static const GUID iid_codec_api={0x901db4c7,0x31ce,0x41a2,{0x85,0xdc,0x8f,0xa0,0xbf,0x41,0xb8,0xda}};
static const GUID api_gop_size={0x95f31b26,0x95a4,0x41aa,{0x93,0x03,0x24,0x6a,0x7f,0xc6,0xee,0xf1}};
static const GUID api_quality={0xfcbf57a3,0x7ea5,0x4b0c,{0x96,0x44,0x69,0xb4,0x0c,0x39,0xc3,0x91}};
static const GUID api_mean_bitrate={0xf7222374,0x2144,0x4815,{0xb5,0x50,0xa3,0x7f,0x8e,0x12,0xee,0x52}};
static const GUID iid_compression={0xc6e13343,0x30ac,0x11d0,{0xa1,0x8c,0,0xa0,0xc9,0x11,0x89,0x56}};
static const GUID iid_persist_stream={0x00000109,0x0000,0x0000,{0xc0,0,0,0,0,0,0,0x46}};

static int eq(const GUID*a,const GUID*b){const BYTE*x=(const BYTE*)a,*y=(const BYTE*)b;for(int i=0;i<16;i++)if(x[i]!=y[i])return 0;return 1;}
static void *alloc(DWORD n){return HeapAlloc(GetProcessHeap(),HEAP_ZERO_MEMORY,n);}
static void release(void*p){if(p)HeapFree(GetProcessHeap(),0,p);}
static struct pin *pin_from_mem(IMemInputPin*p){return(void*)((BYTE*)p-FIELD_OFFSET(struct pin,mem));}

static void free_type(AM_MEDIA_TYPE*m){if(m->pUnk)m->pUnk->lpVtbl->Release(m->pUnk);CoTaskMemFree(m->pbFormat);for(DWORD i=0;i<sizeof(*m);i++)((BYTE*)m)[i]=0;}
static HRESULT copy_type(AM_MEDIA_TYPE*d,const AM_MEDIA_TYPE*s){*d=*s;d->pUnk=NULL;d->pbFormat=NULL;if(s->cbFormat){d->pbFormat=CoTaskMemAlloc(s->cbFormat);if(!d->pbFormat)return E_OUTOFMEMORY;for(ULONG i=0;i<s->cbFormat;i++)d->pbFormat[i]=s->pbFormat[i];}if(s->pUnk){d->pUnk=s->pUnk;s->pUnk->lpVtbl->AddRef(s->pUnk);}return S_OK;}

static BITMAPINFOHEADER *media_bih(AM_MEDIA_TYPE *mt)
{
	if (!mt->pbFormat || mt->cbFormat < sizeof(VIDEOINFOHEADER)) return NULL;
	return &((VIDEOINFOHEADER *)mt->pbFormat)->bmiHeader;
}
static LONG abs_long(LONG n){return n<0?-n:n;}
static DWORD stride(LONG w,WORD bits){return ((DWORD)w*bits+31)/32*4;}
static DWORD frame_bytes(const BITMAPINFOHEADER*b)
{
	LONG h=abs_long(b->biHeight);
	if(b->biBitCount==12)return (DWORD)(b->biWidth*h+2*((b->biWidth+1)/2)*((h+1)/2));
	return stride(b->biWidth,b->biBitCount)*h;
}
static int run_at(const BYTE*s,DWORD i,DWORD n){return i+2<n&&s[i]==s[i+1]&&s[i]==s[i+2];}
static DWORD rle_encode(const BYTE*s,DWORD n,BYTE*d)
{
	DWORD i=0,o=0;
	while(i<n){
		if(run_at(s,i,n)){DWORD run=3;while(i+run<n&&s[i+run]==s[i]&&run<129)run++;d[o++]=(BYTE)(run+126);d[o++]=s[i];i+=run;}
		else{DWORD begin=i,lit=0;while(i<n&&lit<128&&(lit==0||!run_at(s,i,n))){i++;lit++;}d[o++]=(BYTE)(lit-1);for(DWORD k=0;k<lit;k++)d[o++]=s[begin+k];}
	}
	return o;
}
static int rle_decode(const BYTE*s,DWORD len,BYTE*d,DWORD n,BOOL delta)
{
	DWORD i=0,o=0;
	while(o<n){if(i>=len)return -1;BYTE c=s[i++];if(c<128){DWORD lit=c+1;if(i+lit>len||o+lit>n)return -1;for(DWORD k=0;k<lit;k++,o++)d[o]=delta?(BYTE)(d[o]^s[i+k]):s[i+k];i+=lit;}else{DWORD run=c-126;if(i>=len||o+run>n)return -1;for(DWORD k=0;k<run;k++,o++)d[o]=delta?(BYTE)(d[o]^s[i]):s[i];i++;}}
	return 0;
}

static HRESULT WINAPI filter_qi(IBaseFilter*p,REFIID iid,void**out){if(!out)return E_POINTER;struct filter*f=(void*)p;if(eq(iid,&iid_unknown)||eq(iid,&iid_filter))*out=p;else if(f->encoder&&!f->codec_api_only&&eq(iid,&iid_compression))*out=&f->compression;else if(f->encoder&&f->codec_api_only&&eq(iid,&iid_codec_api))*out=&f->codec_api;else if(f->encoder&&eq(iid,&iid_persist_stream))*out=&f->persist;else{*out=NULL;return E_NOINTERFACE;}p->lpVtbl->AddRef(p);return S_OK;}
static ULONG WINAPI filter_addref(IBaseFilter*p){return InterlockedIncrement(&((struct filter*)p)->refs);}
static ULONG WINAPI filter_release(IBaseFilter*p){struct filter*f=(void*)p;LONG n=InterlockedDecrement(&f->refs);if(!n){release(f->picture);if(f->clock)f->clock->lpVtbl->Release(f->clock);if(f->graph)f->graph->lpVtbl->Release(f->graph);DeleteCriticalSection(&f->transform_lock);release(f);InterlockedDecrement(&objects);}return n;}
static HRESULT WINAPI filter_class(IBaseFilter*p,CLSID*c){if(!c)return E_POINTER;for(int i=0;i<16;i++)((BYTE*)c)[i]=0;return S_OK;}
static HRESULT WINAPI filter_stop(IBaseFilter*p){((struct filter*)p)->state=State_Stopped;return S_OK;}
static HRESULT WINAPI filter_pause(IBaseFilter*p){((struct filter*)p)->state=State_Paused;return S_OK;}
static HRESULT WINAPI filter_run(IBaseFilter*p,REFERENCE_TIME t){((struct filter*)p)->state=State_Running;return S_OK;}
static HRESULT WINAPI filter_state(IBaseFilter*p,DWORD timeout,FILTER_STATE*s){if(!s)return E_POINTER;*s=((struct filter*)p)->state;return S_OK;}
static HRESULT WINAPI filter_set_clock(IBaseFilter*p,IReferenceClock*c){struct filter*f=(void*)p;if(c)c->lpVtbl->AddRef(c);if(f->clock)f->clock->lpVtbl->Release(f->clock);f->clock=c;return S_OK;}
static HRESULT WINAPI filter_get_clock(IBaseFilter*p,IReferenceClock**c){if(!c)return E_POINTER;*c=((struct filter*)p)->clock;if(*c)(*c)->lpVtbl->AddRef(*c);return *c?S_OK:VFW_E_NO_CLOCK;}

static struct filter *filter_from_compression(IAMVideoCompression*p){return(void*)((BYTE*)p-FIELD_OFFSET(struct filter,compression));}
static HRESULT WINAPI compression_qi(IAMVideoCompression*p,REFIID i,void**o){return filter_qi(&filter_from_compression(p)->iface,i,o);}
static ULONG WINAPI compression_addref(IAMVideoCompression*p){return filter_addref(&filter_from_compression(p)->iface);}
static ULONG WINAPI compression_release(IAMVideoCompression*p){return filter_release(&filter_from_compression(p)->iface);}
static HRESULT WINAPI compression_put_key(IAMVideoCompression*p,LONG n){filter_from_compression(p)->key_interval=n;return S_OK;}
static HRESULT WINAPI compression_get_key(IAMVideoCompression*p,LONG*n){if(!n)return E_POINTER;*n=filter_from_compression(p)->key_interval;return S_OK;}
static HRESULT WINAPI compression_put_p(IAMVideoCompression*p,LONG n){filter_from_compression(p)->pframes=n;return S_OK;}
static HRESULT WINAPI compression_get_p(IAMVideoCompression*p,LONG*n){if(!n)return E_POINTER;*n=filter_from_compression(p)->pframes;return S_OK;}
static HRESULT WINAPI compression_put_quality(IAMVideoCompression*p,double n){filter_from_compression(p)->quality=n;return S_OK;}
static HRESULT WINAPI compression_get_quality(IAMVideoCompression*p,double*n){if(!n)return E_POINTER;*n=filter_from_compression(p)->quality;return S_OK;}
static HRESULT WINAPI compression_put_window(IAMVideoCompression*p,DWORDLONG n){filter_from_compression(p)->window=n;return S_OK;}
static HRESULT WINAPI compression_get_window(IAMVideoCompression*p,DWORDLONG*n){if(!n)return E_POINTER;*n=filter_from_compression(p)->window;return S_OK;}
static HRESULT WINAPI compression_info(IAMVideoCompression*p,LPWSTR a,int*b,LPWSTR c,int*d,LONG*e,LONG*f,double*g,LONG*h){return E_NOTIMPL;}
static HRESULT WINAPI compression_override_key(IAMVideoCompression*p,LONG n){return E_NOTIMPL;}
static HRESULT WINAPI compression_override_size(IAMVideoCompression*p,LONG n,LONG s){return E_NOTIMPL;}
static IAMVideoCompressionVtbl compression_vtbl={compression_qi,compression_addref,compression_release,compression_put_key,compression_get_key,compression_put_p,compression_get_p,compression_put_quality,compression_get_quality,compression_put_window,compression_get_window,compression_info,compression_override_key,compression_override_size};

/*
 * ICodecAPI (TESTFILTER_CODECAPI=1 gives the encoder this in place of
 * IAMVideoCompression): the GOP size, quality (a percentage) and mean bit
 * rate, as unsigned 32-bit values.
 */
static struct filter *filter_from_api(ICodecAPI*p){return(void*)((BYTE*)p-FIELD_OFFSET(struct filter,codec_api));}
static HRESULT WINAPI api_qi(ICodecAPI*p,REFIID i,void**o){return filter_qi(&filter_from_api(p)->iface,i,o);}
static ULONG WINAPI api_addref(ICodecAPI*p){return filter_addref(&filter_from_api(p)->iface);}
static ULONG WINAPI api_release(ICodecAPI*p){return filter_release(&filter_from_api(p)->iface);}
static BOOL api_known(const GUID*a){return eq(a,&api_gop_size)||eq(a,&api_quality)||eq(a,&api_mean_bitrate);}
static HRESULT WINAPI api_supported(ICodecAPI*p,const GUID*a){return a&&api_known(a)?S_OK:S_FALSE;}
static HRESULT WINAPI api_modifiable(ICodecAPI*p,const GUID*a){return a&&api_known(a)?S_OK:S_FALSE;}
static HRESULT WINAPI api_get(ICodecAPI*p,const GUID*a,VARIANT*v){
	struct filter*f=filter_from_api(p);
	if(!a||!v)return E_POINTER;
	v->vt=VT_UI4;
	if(eq(a,&api_gop_size))v->ulVal=(ULONG)f->key_interval;
	else if(eq(a,&api_quality))v->ulVal=(ULONG)(f->quality*100.0+0.5);
	else if(eq(a,&api_mean_bitrate))v->ulVal=(ULONG)f->window;
	else return E_NOTIMPL;
	return S_OK;
}
static HRESULT WINAPI api_set(ICodecAPI*p,const GUID*a,VARIANT*v){
	struct filter*f=filter_from_api(p);
	if(!a||!v)return E_POINTER;
	if(v->vt!=VT_UI4)return E_INVALIDARG;
	if(eq(a,&api_gop_size))f->key_interval=(LONG)v->ulVal;
	else if(eq(a,&api_quality)){if(v->ulVal>100)return E_INVALIDARG;f->quality=v->ulVal/100.0;}
	else if(eq(a,&api_mean_bitrate))f->window=v->ulVal;
	else return E_NOTIMPL;
	return S_OK;
}
static HRESULT WINAPI api_range(ICodecAPI*p,const GUID*a,VARIANT*x,VARIANT*y,VARIANT*z){return E_NOTIMPL;}
static HRESULT WINAPI api_values(ICodecAPI*p,const GUID*a,VARIANT**v,ULONG*n){return E_NOTIMPL;}
static HRESULT WINAPI api_event(ICodecAPI*p,const GUID*a,LONG_PTR d){return E_NOTIMPL;}
static HRESULT WINAPI api_unevent(ICodecAPI*p,const GUID*a){return E_NOTIMPL;}
static HRESULT WINAPI api_defaults(ICodecAPI*p){return E_NOTIMPL;}
static HRESULT WINAPI api_set_notify(ICodecAPI*p,const GUID*a,VARIANT*v,GUID**c,ULONG*n){return E_NOTIMPL;}
static HRESULT WINAPI api_defaults_notify(ICodecAPI*p,GUID**c,ULONG*n){return E_NOTIMPL;}
static HRESULT WINAPI api_all(ICodecAPI*p,IStream*s){return E_NOTIMPL;}
static HRESULT WINAPI api_all_notify(ICodecAPI*p,IStream*s,GUID**c,ULONG*n){return E_NOTIMPL;}
static ICodecAPIVtbl codec_api_vtbl={api_qi,api_addref,api_release,api_supported,api_modifiable,api_range,api_values,api_get,api_get,api_set,api_event,api_unevent,api_defaults,api_set_notify,api_defaults_notify,api_all,api_all,api_all_notify};

struct saved_state { DWORD magic; LONG key,pframes; double quality; DWORDLONG window; };
static struct filter *filter_from_persist(IPersistStream*p){return(void*)((BYTE*)p-FIELD_OFFSET(struct filter,persist));}
static HRESULT WINAPI persist_qi(IPersistStream*p,REFIID i,void**o){return filter_qi(&filter_from_persist(p)->iface,i,o);}
static ULONG WINAPI persist_addref(IPersistStream*p){return filter_addref(&filter_from_persist(p)->iface);}
static ULONG WINAPI persist_release(IPersistStream*p){return filter_release(&filter_from_persist(p)->iface);}
static HRESULT WINAPI persist_class(IPersistStream*p,CLSID*c){if(!c)return E_POINTER;for(int i=0;i<16;i++)((BYTE*)c)[i]=0;return S_OK;}
static HRESULT WINAPI persist_dirty(IPersistStream*p){return S_FALSE;}
static HRESULT WINAPI persist_load(IPersistStream*p,IStream*s){if(!s)return E_POINTER;struct saved_state x;ULONG got=0;HRESULT hr=s->lpVtbl->Read(s,&x,sizeof(x),&got);if(FAILED(hr))return hr;if(got!=sizeof(x)||x.magic!=mmioFOURCC('T','S','T','0'))return E_FAIL;struct filter*f=filter_from_persist(p);f->key_interval=x.key;f->pframes=x.pframes;f->quality=x.quality;f->window=x.window;return S_OK;}
static HRESULT WINAPI persist_save(IPersistStream*p,IStream*s,BOOL clear){if(!s)return E_POINTER;struct filter*f=filter_from_persist(p);struct saved_state x={mmioFOURCC('T','S','T','0'),f->key_interval,f->pframes,f->quality,f->window};ULONG wrote=0;HRESULT hr=s->lpVtbl->Write(s,&x,sizeof(x),&wrote);return SUCCEEDED(hr)&&wrote!=sizeof(x)?E_FAIL:hr;}
static HRESULT WINAPI persist_size(IPersistStream*p,ULARGE_INTEGER*n){if(!n)return E_POINTER;n->QuadPart=sizeof(struct saved_state);return S_OK;}
static IPersistStreamVtbl persist_vtbl={persist_qi,persist_addref,persist_release,persist_class,persist_dirty,persist_load,persist_save,persist_size};

static HRESULT WINAPI enum_qi(IEnumPins*p,REFIID iid,void**out){if(!out)return E_POINTER;if(eq(iid,&iid_unknown)||eq(iid,&iid_enum_pins))*out=p;else{*out=NULL;return E_NOINTERFACE;}p->lpVtbl->AddRef(p);return S_OK;}
static ULONG WINAPI enum_addref(IEnumPins*p){return InterlockedIncrement(&((struct enum_pins*)p)->refs);}
static ULONG WINAPI enum_release(IEnumPins*p){struct enum_pins*e=(void*)p;LONG n=InterlockedDecrement(&e->refs);if(!n){e->filter->iface.lpVtbl->Release(&e->filter->iface);release(e);}return n;}
static HRESULT WINAPI enum_next(IEnumPins*p,ULONG n,IPin**pins,ULONG*got){struct enum_pins*e=(void*)p;if(!pins||(!got&&n!=1))return E_POINTER;ULONG count=0;while(count<n&&e->pos<2){pins[count]=e->pos++?&e->filter->output.iface:&e->filter->input.iface;pins[count]->lpVtbl->AddRef(pins[count]);count++;}if(got)*got=count;return count==n?S_OK:S_FALSE;}
static HRESULT WINAPI enum_skip(IEnumPins*p,ULONG n){struct enum_pins*e=(void*)p;e->pos+=n;if(e->pos>2){e->pos=2;return S_FALSE;}return S_OK;}
static HRESULT WINAPI enum_reset(IEnumPins*p){((struct enum_pins*)p)->pos=0;return S_OK;}
static HRESULT WINAPI enum_clone(IEnumPins*p,IEnumPins**out);
static IEnumPinsVtbl enum_vtbl={enum_qi,enum_addref,enum_release,enum_next,enum_skip,enum_reset,enum_clone};
static HRESULT make_enum(struct filter*f,ULONG pos,IEnumPins**out){struct enum_pins*e=alloc(sizeof(*e));if(!e)return E_OUTOFMEMORY;e->iface.lpVtbl=&enum_vtbl;e->refs=1;e->filter=f;e->pos=pos;f->iface.lpVtbl->AddRef(&f->iface);*out=&e->iface;return S_OK;}
static HRESULT WINAPI enum_clone(IEnumPins*p,IEnumPins**out){if(!out)return E_POINTER;struct enum_pins*e=(void*)p;return make_enum(e->filter,e->pos,out);}
static HRESULT WINAPI filter_enum(IBaseFilter*p,IEnumPins**out){if(!out)return E_POINTER;return make_enum((void*)p,0,out);}

static HRESULT WINAPI pin_qi(IPin*p,REFIID iid,void**out){if(!out)return E_POINTER;struct pin*x=(void*)p;if(eq(iid,&iid_unknown)||eq(iid,&iid_pin))*out=p;else if(x->direction==PINDIR_INPUT&&eq(iid,&iid_mem_input))*out=&x->mem;else{*out=NULL;return E_NOINTERFACE;}x->filter->iface.lpVtbl->AddRef(&x->filter->iface);return S_OK;}
static ULONG WINAPI pin_addref(IPin*p){struct pin*x=(void*)p;return x->filter->iface.lpVtbl->AddRef(&x->filter->iface);}
static ULONG WINAPI pin_release(IPin*p){struct pin*x=(void*)p;return x->filter->iface.lpVtbl->Release(&x->filter->iface);}
/*
 * What real filters do with the pin they are connected to (TESTFILTER_PEER_INFO):
 * find its filter and look at the filter, to know what is on the other side.
 * The peer has to be a whole filter with a pin of its own, not just a pin.
 */
static BOOL inspect_peer(IPin*peer){
	PIN_INFO pi;
	CLSID id;
	FILTER_INFO fi;
	IEnumPins*e=NULL;
	IPin*first=NULL;
	ULONG got=0;
	LPWSTR pin_name=NULL;
	BOOL ok;
	if(FAILED(peer->lpVtbl->QueryPinInfo(peer,&pi))||!pi.pFilter)return FALSE;
	ok=SUCCEEDED(pi.pFilter->lpVtbl->GetClassID(pi.pFilter,&id));
	ok=ok&&SUCCEEDED(pi.pFilter->lpVtbl->EnumPins(pi.pFilter,&e))&&e;
	ok=ok&&e->lpVtbl->Next(e,1,&first,&got)==S_OK&&got==1&&first;
	if(first){ok=ok&&first==peer;first->lpVtbl->Release(first);}
	if(e)e->lpVtbl->Release(e);
	ok=ok&&SUCCEEDED(pi.pFilter->lpVtbl->QueryFilterInfo(pi.pFilter,&fi))&&fi.pGraph&&fi.achName[0];
	if(ok)fi.pGraph->lpVtbl->Release(fi.pGraph);
	pi.pFilter->lpVtbl->Release(pi.pFilter);
	ok=ok&&SUCCEEDED(peer->lpVtbl->QueryId(peer,&pin_name))&&pin_name&&pin_name[0];
	if(pin_name)CoTaskMemFree(pin_name);
	return ok;
}
static HRESULT WINAPI pin_receive_connection(IPin*p,IPin*peer,const AM_MEDIA_TYPE*mt){struct pin*x=(void*)p;if(!peer||!mt)return E_POINTER;if(x->filter->peer_info&&!inspect_peer(peer))return E_UNEXPECTED;if(x->direction!=PINDIR_INPUT)return VFW_E_INVALID_DIRECTION;if(x->connected)return VFW_E_ALREADY_CONNECTED;HRESULT hr=copy_type(&x->type,mt);if(FAILED(hr))return hr;x->peer=peer;peer->lpVtbl->AddRef(peer);x->connected=TRUE;return S_OK;}
static HRESULT WINAPI pin_connect(IPin*p,IPin*peer,const AM_MEDIA_TYPE*mt){struct pin*x=(void*)p;if(!peer||!mt)return E_POINTER;if(x->filter->peer_info&&!inspect_peer(peer))return E_UNEXPECTED;if(x->direction!=PINDIR_OUTPUT)return VFW_E_INVALID_DIRECTION;if(x->connected)return VFW_E_ALREADY_CONNECTED;HRESULT hr=peer->lpVtbl->ReceiveConnection(peer,p,mt);if(FAILED(hr))return hr;hr=peer->lpVtbl->QueryInterface(peer,&iid_mem_input,(void**)&x->peer_mem);if(FAILED(hr)){peer->lpVtbl->Disconnect(peer);return hr;}hr=x->peer_mem->lpVtbl->GetAllocator(x->peer_mem,&x->allocator);if(FAILED(hr))return hr;ALLOCATOR_PROPERTIES want={2,(LONG)(mt->lSampleSize?mt->lSampleSize:1),1,0},actual;hr=x->allocator->lpVtbl->SetProperties(x->allocator,&want,&actual);if(SUCCEEDED(hr))hr=x->peer_mem->lpVtbl->NotifyAllocator(x->peer_mem,x->allocator,FALSE);if(SUCCEEDED(hr))hr=x->allocator->lpVtbl->Commit(x->allocator);if(FAILED(hr))return hr;copy_type(&x->type,mt);x->peer=peer;peer->lpVtbl->AddRef(peer);x->connected=TRUE;return S_OK;}
static HRESULT WINAPI pin_disconnect(IPin*p){struct pin*x=(void*)p;if(!x->connected)return S_FALSE;if(x->allocator){x->allocator->lpVtbl->Decommit(x->allocator);x->allocator->lpVtbl->Release(x->allocator);}if(x->peer_mem)x->peer_mem->lpVtbl->Release(x->peer_mem);x->peer->lpVtbl->Release(x->peer);free_type(&x->type);x->allocator=NULL;x->peer_mem=NULL;x->peer=NULL;x->connected=FALSE;return S_OK;}
static HRESULT WINAPI pin_connected(IPin*p,IPin**out){if(!out)return E_POINTER;struct pin*x=(void*)p;if(!x->connected){*out=NULL;return VFW_E_NOT_CONNECTED;}*out=x->peer;x->peer->lpVtbl->AddRef(x->peer);return S_OK;}
static HRESULT WINAPI pin_type(IPin*p,AM_MEDIA_TYPE*out){if(!out)return E_POINTER;struct pin*x=(void*)p;return x->connected?copy_type(out,&x->type):VFW_E_NOT_CONNECTED;}
static HRESULT WINAPI pin_info(IPin*p,PIN_INFO*out){if(!out)return E_POINTER;struct pin*x=(void*)p;out->pFilter=&x->filter->iface;out->pFilter->lpVtbl->AddRef(out->pFilter);out->dir=x->direction;out->achName[0]=x->direction==PINDIR_INPUT?'I':'O';out->achName[1]=0;return S_OK;}
static HRESULT WINAPI pin_direction(IPin*p,PIN_DIRECTION*out){if(!out)return E_POINTER;*out=((struct pin*)p)->direction;return S_OK;}
static HRESULT WINAPI pin_id(IPin*p,LPWSTR*out){return E_NOTIMPL;}
static HRESULT WINAPI pin_accept(IPin*p,const AM_MEDIA_TYPE*mt){return mt?S_OK:E_POINTER;}
static HRESULT WINAPI pin_enum_types(IPin*p,IEnumMediaTypes**out){return E_NOTIMPL;}
static HRESULT WINAPI pin_internal(IPin*p,IPin**out,ULONG*n){return E_NOTIMPL;}
static HRESULT WINAPI pin_eos(IPin*p){struct pin*x=(void*)p;while(x->filter->workers)Sleep(1);if(FAILED(x->filter->async_error))return x->filter->async_error;if(x->filter->no_eos)return S_OK;return x->filter->output.connected?x->filter->output.peer->lpVtbl->EndOfStream(x->filter->output.peer):S_OK;}
static HRESULT WINAPI pin_begin_flush(IPin*p){struct pin*x=(void*)p;x->flushing=TRUE;x->filter->valid=FALSE;x->filter->segment=FALSE;x->filter->async_error=S_OK;return x->filter->output.connected?x->filter->output.peer->lpVtbl->BeginFlush(x->filter->output.peer):S_OK;}
static HRESULT WINAPI pin_end_flush(IPin*p){struct pin*x=(void*)p;x->flushing=FALSE;return x->filter->output.connected?x->filter->output.peer->lpVtbl->EndFlush(x->filter->output.peer):S_OK;}
static HRESULT WINAPI pin_segment(IPin*p,REFERENCE_TIME a,REFERENCE_TIME b,double r){struct pin*x=(void*)p;x->filter->segment=TRUE;return x->filter->output.connected?x->filter->output.peer->lpVtbl->NewSegment(x->filter->output.peer,a,b,r):S_OK;}
static IPinVtbl pin_vtbl={pin_qi,pin_addref,pin_release,pin_connect,pin_receive_connection,pin_disconnect,pin_connected,pin_type,pin_info,pin_direction,pin_id,pin_accept,pin_enum_types,pin_internal,pin_eos,pin_begin_flush,pin_end_flush,pin_segment};

static HRESULT WINAPI mem_qi(IMemInputPin*p,REFIID i,void**o){return pin_qi(&pin_from_mem(p)->iface,i,o);}
static ULONG WINAPI mem_addref(IMemInputPin*p){return pin_addref(&pin_from_mem(p)->iface);}
static ULONG WINAPI mem_release(IMemInputPin*p){return pin_release(&pin_from_mem(p)->iface);}
static HRESULT WINAPI mem_get_allocator(IMemInputPin*p,IMemAllocator**out){return VFW_E_NO_ALLOCATOR;}
static HRESULT WINAPI mem_notify(IMemInputPin*p,IMemAllocator*a,BOOL ro){return a?S_OK:E_POINTER;}
static HRESULT WINAPI mem_require(IMemInputPin*p,ALLOCATOR_PROPERTIES*a){if(!a)return E_POINTER;a->cBuffers=1;a->cbBuffer=1;a->cbAlign=1;a->cbPrefix=0;return S_OK;}
static HRESULT transform(struct filter*f,IMediaSample*input)
{
	struct pin*out=&f->output;
	if(!out->connected||!out->allocator||!out->peer_mem)return VFW_E_NOT_CONNECTED;
	BITMAPINFOHEADER*ib=media_bih(&f->input.type),*ob=media_bih(&out->type);
	if(!ib||!ob)return VFW_E_INVALID_MEDIA_TYPE;
	DWORD raw=frame_bytes(f->encoder?ib:ob);
	if(!raw)return VFW_E_INVALID_MEDIA_TYPE;
	if(f->picture_size!=raw){release(f->picture);f->picture=alloc(raw);f->picture_size=raw;f->valid=FALSE;if(!f->picture)return E_OUTOFMEMORY;}
	BYTE*src=NULL;if(FAILED(input->lpVtbl->GetPointer(input,&src)))return E_FAIL;
	LONG input_len=input->lpVtbl->GetActualDataLength(input);
	REFERENCE_TIME start=0,stop=0;REFERENCE_TIME*sp=&start,*ep=&stop;
	if(FAILED(input->lpVtbl->GetTime(input,&start,&stop)))sp=ep=NULL;
	IMediaSample*sample=NULL;HRESULT hr=out->allocator->lpVtbl->GetBuffer(out->allocator,&sample,sp,ep,0);
	if(FAILED(hr))return hr;
	BYTE*dst=NULL;hr=sample->lpVtbl->GetPointer(sample,&dst);
	if(SUCCEEDED(hr)&&f->encoder){
		BOOL key=input->lpVtbl->IsSyncPoint(input)==S_OK||!f->valid||(f->key_interval>0&&f->frame%(DWORD)f->key_interval==0);
		LONG cap=sample->lpVtbl->GetSize(sample);if(cap<4)hr=VFW_E_BUFFER_OVERFLOW;
		else{dst[0]=key?'K':'D';dst[1]=(BYTE)ib->biBitCount;dst[2]=dst[3]=0;if(key){DWORD n=rle_encode(src,raw,dst+4);if(n+4>(DWORD)cap)hr=VFW_E_BUFFER_OVERFLOW;else sample->lpVtbl->SetActualDataLength(sample,n+4);}else{for(DWORD i=0;i<raw;i++)f->picture[i]^=src[i];DWORD n=rle_encode(f->picture,raw,dst+4);if(n+4>(DWORD)cap)hr=VFW_E_BUFFER_OVERFLOW;else sample->lpVtbl->SetActualDataLength(sample,n+4);}if(SUCCEEDED(hr)){for(DWORD i=0;i<raw;i++)f->picture[i]=src[i];f->valid=TRUE;sample->lpVtbl->SetSyncPoint(sample,key);}}
	}else if(SUCCEEDED(hr)){
		if(input_len<4||src[1]!=ib->biBitCount||(src[0]!='K'&&src[0]!='D'))hr=VFW_E_INVALID_FILE_FORMAT;
		else if(src[0]=='D'&&!f->valid)hr=VFW_E_SAMPLE_REJECTED;
		else if(rle_decode(src+4,input_len-4,f->picture,raw,src[0]=='D'))hr=VFW_E_INVALID_FILE_FORMAT;
		else if((DWORD)sample->lpVtbl->GetSize(sample)<raw)hr=VFW_E_BUFFER_OVERFLOW;
		else{for(DWORD i=0;i<raw;i++)dst[i]=f->picture[i];f->valid=TRUE;sample->lpVtbl->SetActualDataLength(sample,raw);sample->lpVtbl->SetSyncPoint(sample,input->lpVtbl->IsSyncPoint(input)==S_OK);}
	}
	/*
	 * TESTFILTER_DYNAMIC: from the second frame on, the decoder switches its
	 * output to YV12 (the chroma planes swapped), announcing it with the type
	 * on the first sample that has it, after the receiver accepts it.
	 */
	if(SUCCEEDED(hr)&&f->dynamic&&!f->encoder&&f->frame>=1&&ob->biCompression==mmioFOURCC('I','4','2','0')){
		DWORD luma=(DWORD)ob->biWidth*(DWORD)(ob->biHeight<0?-ob->biHeight:ob->biHeight),chroma=luma/4;
		for(DWORD i=0;i<chroma;i++){BYTE t=dst[luma+i];dst[luma+i]=dst[luma+chroma+i];dst[luma+chroma+i]=t;}
		if(f->frame==1){
			AM_MEDIA_TYPE mt;
			hr=copy_type(&mt,&out->type);
			if(SUCCEEDED(hr)){
				media_bih(&mt)->biCompression=mmioFOURCC('Y','V','1','2');
				mt.subtype.Data1=mmioFOURCC('Y','V','1','2');
				hr=out->peer->lpVtbl->QueryAccept(out->peer,&mt);
				if(hr==S_OK)hr=sample->lpVtbl->SetMediaType(sample,&mt);
				else if(SUCCEEDED(hr))hr=VFW_E_TYPE_NOT_ACCEPTED;
				free_type(&mt);
			}
		}
	}
	/* The encoder re-announces its output type on the first sample (TESTFILTER_DYNAMIC). */
	if(SUCCEEDED(hr)&&(f->dynamic||f->dynamic_extra)&&f->encoder&&f->frame==0){
		AM_MEDIA_TYPE mt;
		hr=copy_type(&mt,&out->type);
		if(SUCCEEDED(hr)&&f->dynamic_extra){
			/* TESTFILTER_DYNAMIC_EXTRA: format data (a sequence header, say) the file's stream header lacks. */
			BYTE*bigger=CoTaskMemAlloc(mt.cbFormat+4);
			if(!bigger)hr=E_OUTOFMEMORY;
			else{for(DWORD i=0;i<mt.cbFormat;i++)bigger[i]=mt.pbFormat[i];for(DWORD i=0;i<4;i++)bigger[mt.cbFormat+i]=0xA5;CoTaskMemFree(mt.pbFormat);mt.pbFormat=bigger;mt.cbFormat+=4;}
		}
		if(SUCCEEDED(hr)){
			hr=out->peer->lpVtbl->QueryAccept(out->peer,&mt);
			if(hr==S_OK)hr=sample->lpVtbl->SetMediaType(sample,&mt);
			else if(SUCCEEDED(hr))hr=VFW_E_TYPE_NOT_ACCEPTED;
			free_type(&mt);
		}
	}
	if(SUCCEEDED(hr)){sample->lpVtbl->SetPreroll(sample,!f->no_preroll&&input->lpVtbl->IsPreroll(input)==S_OK);sample->lpVtbl->SetDiscontinuity(sample,input->lpVtbl->IsDiscontinuity(input)==S_OK);hr=out->peer_mem->lpVtbl->Receive(out->peer_mem,sample);}
	sample->lpVtbl->Release(sample);f->frame++;return hr;
}
struct async_sample { struct filter *filter; IMediaSample *sample; HRESULT result; };
static DWORD WINAPI transform_thread(void *opaque){struct async_sample*w=opaque;EnterCriticalSection(&w->filter->transform_lock);w->result=transform(w->filter,w->sample);if(FAILED(w->result)&&SUCCEEDED(w->filter->async_error))w->filter->async_error=w->result;LeaveCriticalSection(&w->filter->transform_lock);w->sample->lpVtbl->Release(w->sample);InterlockedDecrement(&w->filter->workers);w->filter->iface.lpVtbl->Release(&w->filter->iface);return 0;}
static HRESULT WINAPI mem_receive(IMemInputPin*p,IMediaSample*s){struct pin*x=pin_from_mem(p);if(!s)return E_POINTER;if(x->flushing)return S_FALSE;if(x->filter->require_segment&&!x->filter->segment)return VFW_E_WRONG_STATE;if(!x->filter->async)return transform(x->filter,s);struct async_sample*w=alloc(sizeof(*w));if(!w)return E_OUTOFMEMORY;w->filter=x->filter;w->sample=s;s->lpVtbl->AddRef(s);x->filter->iface.lpVtbl->AddRef(&x->filter->iface);InterlockedIncrement(&x->filter->workers);HANDLE thread=CreateThread(NULL,0,transform_thread,w,0,NULL);if(!thread){InterlockedDecrement(&x->filter->workers);x->filter->iface.lpVtbl->Release(&x->filter->iface);s->lpVtbl->Release(s);release(w);return E_FAIL;}WaitForSingleObject(thread,INFINITE);CloseHandle(thread);HRESULT hr=w->result;release(w);return hr;}
static HRESULT WINAPI mem_multiple(IMemInputPin*p,IMediaSample**s,LONG n,LONG*done){if(!s||!done)return E_POINTER;*done=0;for(;*done<n;(*done)++){HRESULT hr=mem_receive(p,s[*done]);if(FAILED(hr))return hr;}return S_OK;}
static HRESULT WINAPI mem_can_block(IMemInputPin*p){return pin_from_mem(p)->filter->async?S_OK:S_FALSE;}
static IMemInputPinVtbl mem_vtbl={mem_qi,mem_addref,mem_release,mem_get_allocator,mem_notify,mem_require,mem_receive,mem_multiple,mem_can_block};

static HRESULT WINAPI filter_find(IBaseFilter*p,LPCWSTR id,IPin**out){return E_NOTIMPL;}
static HRESULT WINAPI filter_info(IBaseFilter*p,FILTER_INFO*out){if(!out)return E_POINTER;struct filter*f=(void*)p;out->achName[0]='T';out->achName[1]=0;out->pGraph=f->graph;if(f->graph)f->graph->lpVtbl->AddRef(f->graph);return S_OK;}
static HRESULT WINAPI filter_join(IBaseFilter*p,IFilterGraph*g,LPCWSTR name){struct filter*f=(void*)p;if(g)g->lpVtbl->AddRef(g);if(f->graph)f->graph->lpVtbl->Release(f->graph);f->graph=g;return S_OK;}
static HRESULT WINAPI filter_vendor(IBaseFilter*p,LPWSTR*out){return E_NOTIMPL;}
static IBaseFilterVtbl filter_vtbl={filter_qi,filter_addref,filter_release,filter_class,filter_stop,filter_pause,filter_run,filter_state,filter_set_clock,filter_get_clock,filter_enum,filter_find,filter_info,filter_join,filter_vendor};

static BOOL env_on(const char*name){char value[2];return GetEnvironmentVariableA(name,value,sizeof(value))==1&&value[0]=='1';}
HRESULT testfilter_create(BOOL encoder,REFIID iid,void**out){if(!out)return E_POINTER;*out=NULL;struct filter*f=alloc(sizeof(*f));if(!f)return E_OUTOFMEMORY;f->iface.lpVtbl=&filter_vtbl;f->compression.lpVtbl=&compression_vtbl;f->codec_api.lpVtbl=&codec_api_vtbl;f->codec_api_only=env_on("TESTFILTER_CODECAPI");f->persist.lpVtbl=&persist_vtbl;f->refs=1;f->encoder=encoder;f->quality=0.75;f->async=env_on("TESTFILTER_ASYNC");f->no_eos=env_on("TESTFILTER_NO_EOS");f->require_segment=env_on("TESTFILTER_REQUIRE_SEGMENT");f->peer_info=env_on("TESTFILTER_PEER_INFO");f->dynamic=env_on("TESTFILTER_DYNAMIC");f->dynamic_extra=env_on("TESTFILTER_DYNAMIC_EXTRA");f->no_preroll=env_on("TESTFILTER_NO_PREROLL");InitializeCriticalSection(&f->transform_lock);f->input=(struct pin){.iface={&pin_vtbl},.mem={&mem_vtbl},.filter=f,.direction=PINDIR_INPUT};f->output=(struct pin){.iface={&pin_vtbl},.mem={&mem_vtbl},.filter=f,.direction=PINDIR_OUTPUT};InterlockedIncrement(&objects);HRESULT hr=filter_qi(&f->iface,iid,out);filter_release(&f->iface);return hr;}
LONG testfilter_objects(void){return objects;}
