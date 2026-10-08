/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * Small CRT-less DirectShow transforms used to exercise the Linux host: a
 * TST0 video decoder and encoder, and a TSTA audio decoder and encoder
 * (tsta.h).
 */
#include "fixture.h"
#include <strmif.h>
#include <amvideo.h>
#include <vfwmsgs.h>
#include <mmreg.h>
#include "tsta.h"

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
	BOOL async, no_eos, require_segment, segment, peer_info, dynamic, dynamic_extra, no_preroll, codec_api_only, notify_error, strict, own_allocator;
	LONG workers;
	HRESULT async_error;
	CRITICAL_SECTION transform_lock;
	/*
	 * Audio (TSTA). The encoder gathers PCM in pcm until a packet is
	 * full; the decoder in hold mode keeps a decoded packet there until
	 * the next one comes (or the end of the stream).
	 */
	BOOL audio;
	BYTE *pcm;
	DWORD pcm_frames;
	BOOL held, held_timed;
	REFERENCE_TIME held_start, base;
	DWORD held_packet;
	DWORDLONG frames_done;
	DWORD packets;
	BOOL split, hold, float_switch, rate_switch, switched;
};
struct enum_pins { IEnumPins iface; LONG refs; struct filter *filter; ULONG pos; };

static LONG objects;
static const GUID iid_unknown={0,0,0,{0xc0,0,0,0,0,0,0,0x46}};
static const GUID iid_filter={0x56a86895,0x0ad4,0x11ce,{0xb0,0x3a,0,0x20,0xaf,0x0b,0xa7,0x70}};
static const GUID iid_pin={0x56a86891,0x0ad4,0x11ce,{0xb0,0x3a,0,0x20,0xaf,0x0b,0xa7,0x70}};
static const GUID iid_enum_pins={0x56a86892,0x0ad4,0x11ce,{0xb0,0x3a,0,0x20,0xaf,0x0b,0xa7,0x70}};
static const GUID iid_mem_input={0x56a8689d,0x0ad4,0x11ce,{0xb0,0x3a,0,0x20,0xaf,0x0b,0xa7,0x70}};
static const GUID iid_event_sink={0x56a868a2,0x0ad4,0x11ce,{0xb0,0x3a,0,0x20,0xaf,0x0b,0xa7,0x70}};
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

/*
 * TESTFILTER_STRICT: every pointer the host hands this filter must be below
 * 2 GB, as Windows gives a program that isn't large-address aware (and some
 * filters then keep it in a signed int). Like testcodec's strict mode.
 */
static BOOL high(const struct filter*f,const void*p){return f->strict&&(ULONG_PTR)p>=0x80000000UL;}
#define LOWPTR(f,p) do{if(high((f),(p)))return E_POINTER;}while(0)
static HRESULT WINAPI filter_qi(IBaseFilter*p,REFIID iid,void**out){if(!out)return E_POINTER;struct filter*f=(void*)p;if(eq(iid,&iid_unknown)||eq(iid,&iid_filter))*out=p;else if(f->encoder&&!f->audio&&!f->codec_api_only&&eq(iid,&iid_compression))*out=&f->compression;else if(f->encoder&&(f->codec_api_only||f->audio)&&eq(iid,&iid_codec_api))*out=&f->codec_api;else if(f->encoder&&eq(iid,&iid_persist_stream))*out=&f->persist;else{*out=NULL;return E_NOINTERFACE;}p->lpVtbl->AddRef(p);return S_OK;}
static ULONG WINAPI filter_addref(IBaseFilter*p){return InterlockedIncrement(&((struct filter*)p)->refs);}
static ULONG WINAPI filter_release(IBaseFilter*p){struct filter*f=(void*)p;LONG n=InterlockedDecrement(&f->refs);if(!n){release(f->picture);release(f->pcm);if(f->clock)f->clock->lpVtbl->Release(f->clock);if(f->graph)f->graph->lpVtbl->Release(f->graph);DeleteCriticalSection(&f->transform_lock);release(f);InterlockedDecrement(&objects);}return n;}
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

/*
 * Audio: TSTA in and PCM out (the decoder), or the reverse (the encoder).
 *
 * Modes, from the environment as the video ones are:
 *   TESTFILTER_AUDIO_SPLIT  the decoder gives its PCM in pieces of 300
 *                           sample frames, not a sample per packet
 *   TESTFILTER_AUDIO_HOLD   the decoder keeps each packet's PCM until the
 *                           next packet comes, so the last comes out only
 *                           at the end of the stream
 *   TESTFILTER_AUDIO_FLOAT  from the second packet on, the decoder gives
 *                           32-bit float, announcing the new type on the
 *                           first sample that has it
 *   TESTFILTER_AUDIO_RATE   from the second packet on, the decoder claims
 *                           twice the sample rate: a change no file can
 *                           follow, which the host must refuse cleanly
 * The encoder always gathers PCM into whole packets and gives the last,
 * short one at the end of the stream.
 */
static const GUID mt_audio={0x73647561,0x0000,0x0010,{0x80,0x00,0x00,0xaa,0x00,0x38,0x9b,0x71}};
static const GUID st_pcm={0x00000001,0x0000,0x0010,{0x80,0x00,0x00,0xaa,0x00,0x38,0x9b,0x71}};
static const GUID st_float={0x00000003,0x0000,0x0010,{0x80,0x00,0x00,0xaa,0x00,0x38,0x9b,0x71}};
static const GUID st_tsta={WAVE_FORMAT_TSTA,0x0000,0x0010,{0x80,0x00,0x00,0xaa,0x00,0x38,0x9b,0x71}};
static const GUID fmt_wave={0x05589f81,0xc356,0x11ce,{0xbf,0x01,0x00,0xaa,0x00,0x55,0x59,0x5a}};

static REFERENCE_TIME frames_time(DWORDLONG frames,DWORD rate){return (REFERENCE_TIME)tsta_ticks(frames,rate);}

/* A media type's WAVEFORMATEX, or NULL. */
static WAVEFORMATEX *media_wfx(const AM_MEDIA_TYPE *mt)
{
	if(!mt||!eq(&mt->majortype,&mt_audio)||!eq(&mt->formattype,&fmt_wave)||
	   !mt->pbFormat||mt->cbFormat<sizeof(WAVEFORMATEX))
		return NULL;
	return (WAVEFORMATEX*)mt->pbFormat;
}

/* Integer PCM, plain or extensible: its bits per sample, else 0. */
static DWORD pcm_bits(const AM_MEDIA_TYPE *mt)
{
	WAVEFORMATEX *wf=media_wfx(mt);
	if(!wf||!eq(&mt->subtype,&st_pcm))return 0;
	if(wf->wFormatTag==WAVE_FORMAT_EXTENSIBLE){
		WAVEFORMATEXTENSIBLE *x=(void*)wf;
		if(mt->cbFormat<sizeof(*x)||wf->cbSize<22||!eq(&x->SubFormat,&st_pcm)||
		   x->Samples.wValidBitsPerSample!=wf->wBitsPerSample)return 0;
	}else if(wf->wFormatTag!=WAVE_FORMAT_PCM)return 0;
	if(!tsta_pcm_block(wf->wBitsPerSample,wf->nChannels)||
	   wf->nBlockAlign!=tsta_pcm_block(wf->wBitsPerSample,wf->nChannels))return 0;
	return wf->wBitsPerSample;
}

static WAVEFORMATEX *tsta_wfx(const AM_MEDIA_TYPE *mt)
{
	WAVEFORMATEX *wf=media_wfx(mt);
	if(!wf||!eq(&mt->subtype,&st_tsta)||wf->wFormatTag!=WAVE_FORMAT_TSTA||
	   !tsta_pcm_block(wf->wBitsPerSample,wf->nChannels)||!wf->nSamplesPerSec)return NULL;
	return wf;
}

/* The input pin's type: TSTA for the decoder, integer PCM for the encoder. */
static HRESULT audio_check_input(struct filter*f,const AM_MEDIA_TYPE*mt)
{
	if(f->encoder)return pcm_bits(mt)&&media_wfx(mt)->nSamplesPerSec?S_OK:VFW_E_TYPE_NOT_ACCEPTED;
	return tsta_wfx(mt)?S_OK:VFW_E_TYPE_NOT_ACCEPTED;
}

/*
 * The output pin's type. The decoder takes integer PCM of the stream's
 * bits, channels and rate. The encoder makes TSTA from its input; given
 * only a major type and subtype it fills in the rest, as filters do when
 * left to choose. *full gets the type to connect with.
 */
static HRESULT audio_output_type(struct filter*f,const AM_MEDIA_TYPE*mt,AM_MEDIA_TYPE*full)
{
	WAVEFORMATEX *in=media_wfx(&f->input.type);
	if(!f->input.connected||!in)return VFW_E_NOT_CONNECTED;
	if(!f->encoder){
		WAVEFORMATEX *out=media_wfx(mt);
		if(pcm_bits(mt)!=in->wBitsPerSample||out->nChannels!=in->nChannels||
		   out->nSamplesPerSec!=in->nSamplesPerSec)return VFW_E_TYPE_NOT_ACCEPTED;
		return copy_type(full,mt);
	}
	if(!eq(&mt->majortype,&mt_audio)||!eq(&mt->subtype,&st_tsta))return VFW_E_TYPE_NOT_ACCEPTED;
	if(mt->pbFormat){
		WAVEFORMATEX *out=tsta_wfx(mt);
		if(!out||out->nChannels!=in->nChannels||out->nSamplesPerSec!=in->nSamplesPerSec||
		   out->wBitsPerSample!=in->wBitsPerSample)return VFW_E_TYPE_NOT_ACCEPTED;
		return copy_type(full,mt);
	}
	for(DWORD i=0;i<sizeof(*full);i++)((BYTE*)full)[i]=0;
	full->majortype=mt_audio;
	full->subtype=st_tsta;
	full->formattype=fmt_wave;
	full->cbFormat=sizeof(WAVEFORMATEX)+2;
	if(!(full->pbFormat=CoTaskMemAlloc(full->cbFormat)))return E_OUTOFMEMORY;
	tsta_format((WAVEFORMATEX*)full->pbFormat,in->wBitsPerSample,in->nChannels,
		    in->nSamplesPerSec,(DWORD)f->window);
	return S_OK;
}

/* The output buffers: a whole packet, or a packet's PCM as float. */
static LONG audio_buffer(struct filter*f)
{
	WAVEFORMATEX *in=media_wfx(&f->input.type);
	if(f->encoder)return TSTA_HEADER+TSTA_FRAMES*tsta_pcm_block(in->wBitsPerSample,in->nChannels);
	return TSTA_FRAMES*4*in->nChannels;
}

/* Starts again (a flush, or a new connection). */
static void audio_reset(struct filter*f){f->pcm_frames=0;f->held=FALSE;f->frames_done=0;f->packets=0;f->switched=FALSE;}

/* The encoder: a packet of the PCM gathered. */
static HRESULT audio_emit_packet(struct filter*f)
{
	struct pin*out=&f->output;
	WAVEFORMATEX *in=media_wfx(&f->input.type);
	IMediaSample*sample=NULL;
	BYTE*dst=NULL;
	REFERENCE_TIME start=f->base+frames_time(f->frames_done,in->nSamplesPerSec);
	REFERENCE_TIME stop=f->base+frames_time(f->frames_done+f->pcm_frames,in->nSamplesPerSec);
	HRESULT hr=out->allocator->lpVtbl->GetBuffer(out->allocator,&sample,&start,&stop,0);
	if(FAILED(hr))return hr;
	hr=sample->lpVtbl->GetPointer(sample,&dst);
	if(SUCCEEDED(hr)&&sample->lpVtbl->GetSize(sample)<audio_buffer(f))hr=VFW_E_BUFFER_OVERFLOW;
	if(SUCCEEDED(hr)){
		DWORD n=tsta_encode(f->pcm,f->pcm_frames,in->wBitsPerSample,in->nChannels,dst);
		sample->lpVtbl->SetActualDataLength(sample,(LONG)n);
		sample->lpVtbl->SetTime(sample,&start,&stop);
		sample->lpVtbl->SetSyncPoint(sample,TRUE);
		sample->lpVtbl->SetDiscontinuity(sample,f->frames_done==0);
		hr=out->peer_mem->lpVtbl->Receive(out->peer_mem,sample);
	}
	sample->lpVtbl->Release(sample);
	f->frames_done+=f->pcm_frames;
	f->pcm_frames=0;
	return hr;
}

static HRESULT audio_encode(struct filter*f,IMediaSample*input)
{
	WAVEFORMATEX *in=media_wfx(&f->input.type);
	DWORD block=tsta_pcm_block(in->wBitsPerSample,in->nChannels);
	BYTE*src=NULL;
	LONG len=input->lpVtbl->GetActualDataLength(input);
	REFERENCE_TIME start,stop;
	HRESULT hr=S_OK;
	if(FAILED(input->lpVtbl->GetPointer(input,&src)))return E_FAIL;
	LOWPTR(f,src);
	if(len<0||(DWORD)len%block)return VFW_E_INVALID_MEDIA_TYPE;
	if(!f->pcm&&!(f->pcm=alloc(TSTA_FRAMES*4*in->nChannels)))return E_OUTOFMEMORY;
	/* Times count from the first sample's. */
	if(!f->frames_done&&!f->pcm_frames)
		f->base=SUCCEEDED(input->lpVtbl->GetTime(input,&start,&stop))?start:0;
	for(DWORD frames=(DWORD)len/block,i=0;i<frames&&SUCCEEDED(hr);i++){
		BYTE*d=f->pcm+f->pcm_frames*block;
		for(DWORD k=0;k<block;k++)d[k]=src[i*block+k];
		if(++f->pcm_frames==TSTA_FRAMES)hr=audio_emit_packet(f);
	}
	return hr;
}

/* The float the decoder switches to, for a stream of these channels and rate. */
static void float_type(const AM_MEDIA_TYPE*pcm,AM_MEDIA_TYPE*mt)
{
	WAVEFORMATEXTENSIBLE *x=(void*)mt->pbFormat;
	WAVEFORMATEX *wf=media_wfx(pcm);
	mt->subtype=st_float;
	x->Format.wFormatTag=WAVE_FORMAT_EXTENSIBLE;
	x->Format.nChannels=wf->nChannels;
	x->Format.nSamplesPerSec=wf->nSamplesPerSec;
	x->Format.wBitsPerSample=32;
	x->Format.nBlockAlign=(WORD)(4*wf->nChannels);
	x->Format.nAvgBytesPerSec=wf->nSamplesPerSec*x->Format.nBlockAlign;
	x->Format.cbSize=22;
	x->Samples.wValidBitsPerSample=32;
	x->dwChannelMask=wf->nChannels==2?3:0;
	x->SubFormat=st_float;
	mt->lSampleSize=x->Format.nBlockAlign;
}

/* PCM samples as floats in [-1, 1). */
static void to_float(const BYTE*pcm,DWORD samples,DWORD bits,float*out)
{
	for(DWORD i=0;i<samples;i++,pcm+=bits/8){
		LONG v;
		if(bits==8)v=(LONG)pcm[0]-128;
		else if(bits==16)v=(SHORT)(pcm[0]|pcm[1]<<8);
		else if(bits==24)v=(LONG)((DWORD)pcm[0]<<8|(DWORD)pcm[1]<<16|(DWORD)pcm[2]<<24)>>8;
		else v=(LONG)((DWORD)pcm[0]|(DWORD)pcm[1]<<8|(DWORD)pcm[2]<<16|(DWORD)pcm[3]<<24);
		out[i]=(float)v/(float)(1u<<(bits-1));
	}
}

/*
 * The decoder: frames of PCM from packet number packet, starting at start,
 * as one sample or (split) several.
 */
static HRESULT audio_deliver(struct filter*f,const BYTE*pcm,DWORD frames,BOOL timed,REFERENCE_TIME start,DWORD packet)
{
	struct pin*out=&f->output;
	WAVEFORMATEX *wf=media_wfx(&out->type);
	DWORD block=wf->nBlockAlign,piece=f->split?300:frames;
	HRESULT hr=S_OK;
	for(DWORD off=0;off<frames&&SUCCEEDED(hr);off+=piece){
		DWORD n=frames-off<piece?frames-off:piece;
		REFERENCE_TIME a=start+frames_time(off,wf->nSamplesPerSec),b=start+frames_time(off+n,wf->nSamplesPerSec);
		IMediaSample*sample=NULL;
		BYTE*dst=NULL;
		hr=out->allocator->lpVtbl->GetBuffer(out->allocator,&sample,timed?&a:NULL,timed?&b:NULL,0);
		if(FAILED(hr))return hr;
		hr=sample->lpVtbl->GetPointer(sample,&dst);
		BOOL to_float_now=f->float_switch&&packet>=1;
		DWORD bytes=to_float_now?n*4*wf->nChannels:n*block;
		if(SUCCEEDED(hr)&&(DWORD)sample->lpVtbl->GetSize(sample)<bytes)hr=VFW_E_BUFFER_OVERFLOW;
		/* The change, on the first sample that has it. */
		if(SUCCEEDED(hr)&&(f->float_switch||f->rate_switch)&&packet>=1&&!f->switched){
			AM_MEDIA_TYPE mt;
			hr=copy_type(&mt,&out->type);
			if(SUCCEEDED(hr)&&f->float_switch){
				BYTE*bigger=CoTaskMemAlloc(sizeof(WAVEFORMATEXTENSIBLE));
				if(!bigger)hr=E_OUTOFMEMORY;
				else{CoTaskMemFree(mt.pbFormat);mt.pbFormat=bigger;mt.cbFormat=sizeof(WAVEFORMATEXTENSIBLE);float_type(&out->type,&mt);}
			}else if(SUCCEEDED(hr)){
				WAVEFORMATEX*x=(WAVEFORMATEX*)mt.pbFormat;
				x->nSamplesPerSec*=2;
				x->nAvgBytesPerSec*=2;
			}
			if(SUCCEEDED(hr)){
				hr=out->peer->lpVtbl->QueryAccept(out->peer,&mt);
				if(hr==S_OK)hr=sample->lpVtbl->SetMediaType(sample,&mt);
				else if(SUCCEEDED(hr))hr=VFW_E_TYPE_NOT_ACCEPTED;
			}
			free_type(&mt);
			f->switched=TRUE;
		}
		if(SUCCEEDED(hr)){
			if(to_float_now)to_float(pcm+off*block,n*wf->nChannels,wf->wBitsPerSample,(float*)dst);
			else for(DWORD i=0;i<n*block;i++)dst[i]=pcm[off*block+i];
			sample->lpVtbl->SetActualDataLength(sample,(LONG)bytes);
			sample->lpVtbl->SetSyncPoint(sample,TRUE);
			hr=out->peer_mem->lpVtbl->Receive(out->peer_mem,sample);
		}
		sample->lpVtbl->Release(sample);
	}
	return hr;
}

static HRESULT audio_decode(struct filter*f,IMediaSample*input)
{
	WAVEFORMATEX *in=media_wfx(&f->input.type);
	DWORD block=tsta_pcm_block(in->wBitsPerSample,in->nChannels);
	BYTE*src=NULL;
	LONG len=input->lpVtbl->GetActualDataLength(input);
	REFERENCE_TIME start=0,stop=0;
	BOOL timed=SUCCEEDED(input->lpVtbl->GetTime(input,&start,&stop));
	HRESULT hr=S_OK;
	if(FAILED(input->lpVtbl->GetPointer(input,&src)))return E_FAIL;
	LOWPTR(f,src);
	/* Two packets' room: the one held back, and the new one. */
	if(!f->pcm&&!(f->pcm=alloc(2*TSTA_FRAMES*block)))return E_OUTOFMEMORY;
	BYTE*decoded=f->pcm+TSTA_FRAMES*block;
	LONG frames=len<0?-1:tsta_decode(src,(DWORD)len,in->wBitsPerSample,in->nChannels,decoded,TSTA_FRAMES);
	if(frames<0)return VFW_E_INVALID_FILE_FORMAT;
	DWORD packet=f->packets++;
	if(!f->hold)return audio_deliver(f,decoded,(DWORD)frames,timed,start,packet);
	if(f->held)hr=audio_deliver(f,f->pcm,f->pcm_frames,f->held_timed,f->held_start,f->held_packet);
	for(DWORD i=0;i<(DWORD)frames*block;i++)f->pcm[i]=decoded[i];
	f->pcm_frames=(DWORD)frames;
	f->held=TRUE;
	f->held_timed=timed;
	f->held_start=start;
	f->held_packet=packet;
	return hr;
}

/* The end of the stream: the short last packet, or the PCM held back. */
static HRESULT audio_end(struct filter*f)
{
	HRESULT hr=S_OK;
	if(!f->output.connected||!f->output.allocator)return S_OK;
	if(f->encoder&&f->pcm_frames)hr=audio_emit_packet(f);
	else if(!f->encoder&&f->held){
		f->held=FALSE;
		hr=audio_deliver(f,f->pcm,f->pcm_frames,f->held_timed,f->held_start,f->held_packet);
	}
	return hr;
}

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
static HRESULT WINAPI pin_receive_connection(IPin*p,IPin*peer,const AM_MEDIA_TYPE*mt){struct pin*x=(void*)p;if(!peer||!mt)return E_POINTER;LOWPTR(x->filter,peer);LOWPTR(x->filter,mt);LOWPTR(x->filter,mt->pbFormat);if(x->filter->peer_info&&!inspect_peer(peer))return E_UNEXPECTED;if(x->direction!=PINDIR_INPUT)return VFW_E_INVALID_DIRECTION;if(x->connected)return VFW_E_ALREADY_CONNECTED;if(x->filter->audio&&FAILED(audio_check_input(x->filter,mt)))return VFW_E_TYPE_NOT_ACCEPTED;HRESULT hr=copy_type(&x->type,mt);if(FAILED(hr))return hr;x->peer=peer;peer->lpVtbl->AddRef(peer);x->connected=TRUE;if(x->filter->audio)audio_reset(x->filter);return S_OK;}
static HRESULT WINAPI pin_connect(IPin*p,IPin*peer,const AM_MEDIA_TYPE*mt)
{
	struct pin*x=(void*)p;
	struct filter*f=x->filter;
	AM_MEDIA_TYPE full;
	if(!peer||!mt)return E_POINTER;
	LOWPTR(f,peer);LOWPTR(f,mt);
	if(f->peer_info&&!inspect_peer(peer))return E_UNEXPECTED;
	if(x->direction!=PINDIR_OUTPUT)return VFW_E_INVALID_DIRECTION;
	if(x->connected)return VFW_E_ALREADY_CONNECTED;
	HRESULT hr=f->audio?audio_output_type(f,mt,&full):copy_type(&full,mt);
	if(FAILED(hr))return hr;
	hr=peer->lpVtbl->ReceiveConnection(peer,p,&full);
	if(FAILED(hr)){free_type(&full);return hr;}
	hr=peer->lpVtbl->QueryInterface(peer,&iid_mem_input,(void**)&x->peer_mem);
	if(FAILED(hr)){peer->lpVtbl->Disconnect(peer);free_type(&full);return hr;}
	hr=x->peer_mem->lpVtbl->GetAllocator(x->peer_mem,&x->allocator);
	if(FAILED(hr)){free_type(&full);return hr;}
	ALLOCATOR_PROPERTIES want={2,f->audio?audio_buffer(f):(LONG)(full.lSampleSize?full.lSampleSize:1),1,0},actual;
	hr=x->allocator->lpVtbl->SetProperties(x->allocator,&want,&actual);
	if(SUCCEEDED(hr))hr=x->peer_mem->lpVtbl->NotifyAllocator(x->peer_mem,x->allocator,FALSE);
	if(SUCCEEDED(hr))hr=x->allocator->lpVtbl->Commit(x->allocator);
	if(FAILED(hr)){free_type(&full);return hr;}
	x->type=full;
	x->peer=peer;peer->lpVtbl->AddRef(peer);x->connected=TRUE;
	return S_OK;
}
static HRESULT WINAPI pin_disconnect(IPin*p){struct pin*x=(void*)p;if(!x->connected)return S_FALSE;if(x->allocator){x->allocator->lpVtbl->Decommit(x->allocator);x->allocator->lpVtbl->Release(x->allocator);}if(x->peer_mem)x->peer_mem->lpVtbl->Release(x->peer_mem);x->peer->lpVtbl->Release(x->peer);free_type(&x->type);x->allocator=NULL;x->peer_mem=NULL;x->peer=NULL;x->connected=FALSE;return S_OK;}
static HRESULT WINAPI pin_connected(IPin*p,IPin**out){if(!out)return E_POINTER;struct pin*x=(void*)p;if(!x->connected){*out=NULL;return VFW_E_NOT_CONNECTED;}*out=x->peer;x->peer->lpVtbl->AddRef(x->peer);return S_OK;}
static HRESULT WINAPI pin_type(IPin*p,AM_MEDIA_TYPE*out){if(!out)return E_POINTER;struct pin*x=(void*)p;return x->connected?copy_type(out,&x->type):VFW_E_NOT_CONNECTED;}
static HRESULT WINAPI pin_info(IPin*p,PIN_INFO*out){if(!out)return E_POINTER;struct pin*x=(void*)p;out->pFilter=&x->filter->iface;out->pFilter->lpVtbl->AddRef(out->pFilter);out->dir=x->direction;out->achName[0]=x->direction==PINDIR_INPUT?'I':'O';out->achName[1]=0;return S_OK;}
static HRESULT WINAPI pin_direction(IPin*p,PIN_DIRECTION*out){if(!out)return E_POINTER;*out=((struct pin*)p)->direction;return S_OK;}
static HRESULT WINAPI pin_id(IPin*p,LPWSTR*out){return E_NOTIMPL;}
static HRESULT WINAPI pin_accept(IPin*p,const AM_MEDIA_TYPE*mt){return mt?S_OK:E_POINTER;}
static HRESULT WINAPI pin_enum_types(IPin*p,IEnumMediaTypes**out){return E_NOTIMPL;}
static HRESULT WINAPI pin_internal(IPin*p,IPin**out,ULONG*n){return E_NOTIMPL;}
static HRESULT WINAPI pin_eos(IPin*p){struct pin*x=(void*)p;while(x->filter->workers)Sleep(1);if(FAILED(x->filter->async_error))return x->filter->async_error;if(x->filter->no_eos)return S_OK;if(x->filter->audio){HRESULT hr=audio_end(x->filter);if(FAILED(hr))return hr;}return x->filter->output.connected?x->filter->output.peer->lpVtbl->EndOfStream(x->filter->output.peer):S_OK;}
static HRESULT WINAPI pin_begin_flush(IPin*p){struct pin*x=(void*)p;x->flushing=TRUE;x->filter->valid=FALSE;if(x->filter->audio)audio_reset(x->filter);x->filter->segment=FALSE;x->filter->async_error=S_OK;return x->filter->output.connected?x->filter->output.peer->lpVtbl->BeginFlush(x->filter->output.peer):S_OK;}
static HRESULT WINAPI pin_end_flush(IPin*p){struct pin*x=(void*)p;x->flushing=FALSE;return x->filter->output.connected?x->filter->output.peer->lpVtbl->EndFlush(x->filter->output.peer):S_OK;}
static HRESULT WINAPI pin_segment(IPin*p,REFERENCE_TIME a,REFERENCE_TIME b,double r){struct pin*x=(void*)p;x->filter->segment=TRUE;return x->filter->output.connected?x->filter->output.peer->lpVtbl->NewSegment(x->filter->output.peer,a,b,r):S_OK;}
static IPinVtbl pin_vtbl={pin_qi,pin_addref,pin_release,pin_connect,pin_receive_connection,pin_disconnect,pin_connected,pin_type,pin_info,pin_direction,pin_id,pin_accept,pin_enum_types,pin_internal,pin_eos,pin_begin_flush,pin_end_flush,pin_segment};

static HRESULT WINAPI mem_qi(IMemInputPin*p,REFIID i,void**o){return pin_qi(&pin_from_mem(p)->iface,i,o);}
static ULONG WINAPI mem_addref(IMemInputPin*p){return pin_addref(&pin_from_mem(p)->iface);}
static ULONG WINAPI mem_release(IMemInputPin*p){return pin_release(&pin_from_mem(p)->iface);}
/*
 * TESTFILTER_OWN_ALLOCATOR: the input pin supplies its own allocator, as
 * filters whose decoder wants its buffers in particular memory do. At least
 * three buffers, each with a prefix before the data, which the host must fill
 * and pass back through Receive (which refuses any other sample).
 */
static const GUID iid_allocator={0x56a8689c,0x0ad4,0x11ce,{0xb0,0x3a,0,0x20,0xaf,0x0b,0xa7,0x70}};
static const GUID iid_sample={0x56a8689a,0x0ad4,0x11ce,{0xb0,0x3a,0,0x20,0xaf,0x0b,0xa7,0x70}};
struct falloc;
struct fsample{IMediaSample iface;struct falloc*a;LONG refs;BYTE*mem,*data;LONG size,actual;REFERENCE_TIME start,stop;BOOL has_time,sync,preroll,disc,busy;};
struct falloc{IMemAllocator iface;LONG refs;ALLOCATOR_PROPERTIES props;struct fsample*s;BOOL committed;};
static IMediaSampleVtbl sample_vtbl;
static struct fsample*fs(IMediaSample*p){return(void*)p;}
static HRESULT WINAPI fs_qi(IMediaSample*p,REFIID i,void**o){if(!o)return E_POINTER;if(eq(i,&iid_unknown)||eq(i,&iid_sample)){*o=p;p->lpVtbl->AddRef(p);return S_OK;}*o=NULL;return E_NOINTERFACE;}
static ULONG WINAPI fs_addref(IMediaSample*p){return InterlockedIncrement(&fs(p)->refs);}
static ULONG WINAPI fs_release(IMediaSample*p){
	struct fsample*x=fs(p);LONG n=InterlockedDecrement(&x->refs);
	if(!n){struct falloc*a=x->a;x->busy=FALSE;a->iface.lpVtbl->Release(&a->iface);}
	return n;
}
static HRESULT WINAPI fs_pointer(IMediaSample*p,BYTE**b){if(!b)return E_POINTER;*b=fs(p)->data;return S_OK;}
static LONG WINAPI fs_size(IMediaSample*p){return fs(p)->size;}
static HRESULT WINAPI fs_get_time(IMediaSample*p,REFERENCE_TIME*a,REFERENCE_TIME*b){struct fsample*x=fs(p);if(!x->has_time)return VFW_E_SAMPLE_TIME_NOT_SET;*a=x->start;*b=x->stop;return S_OK;}
static HRESULT WINAPI fs_set_time(IMediaSample*p,REFERENCE_TIME*a,REFERENCE_TIME*b){struct fsample*x=fs(p);x->has_time=a!=NULL;if(a){x->start=*a;x->stop=b?*b:*a;}return S_OK;}
static HRESULT WINAPI fs_sync(IMediaSample*p){return fs(p)->sync?S_OK:S_FALSE;}
static HRESULT WINAPI fs_set_sync(IMediaSample*p,BOOL v){fs(p)->sync=v;return S_OK;}
static HRESULT WINAPI fs_preroll(IMediaSample*p){return fs(p)->preroll?S_OK:S_FALSE;}
static HRESULT WINAPI fs_set_preroll(IMediaSample*p,BOOL v){fs(p)->preroll=v;return S_OK;}
static LONG WINAPI fs_actual(IMediaSample*p){return fs(p)->actual;}
static HRESULT WINAPI fs_set_actual(IMediaSample*p,LONG n){struct fsample*x=fs(p);if(n<0||n>x->size)return VFW_E_BUFFER_OVERFLOW;x->actual=n;return S_OK;}
static HRESULT WINAPI fs_get_type(IMediaSample*p,AM_MEDIA_TYPE**t){if(!t)return E_POINTER;*t=NULL;return S_FALSE;}
static HRESULT WINAPI fs_set_type(IMediaSample*p,AM_MEDIA_TYPE*t){return t?E_NOTIMPL:S_OK;}
static HRESULT WINAPI fs_disc(IMediaSample*p){return fs(p)->disc?S_OK:S_FALSE;}
static HRESULT WINAPI fs_set_disc(IMediaSample*p,BOOL v){fs(p)->disc=v;return S_OK;}
static HRESULT WINAPI fs_get_media(IMediaSample*p,LONGLONG*a,LONGLONG*b){return VFW_E_MEDIA_TIME_NOT_SET;}
static HRESULT WINAPI fs_set_media(IMediaSample*p,LONGLONG*a,LONGLONG*b){return S_OK;}
static IMediaSampleVtbl sample_vtbl={fs_qi,fs_addref,fs_release,fs_pointer,fs_size,fs_get_time,fs_set_time,fs_sync,fs_set_sync,fs_preroll,fs_set_preroll,fs_actual,fs_set_actual,fs_get_type,fs_set_type,fs_disc,fs_set_disc,fs_get_media,fs_set_media};
static struct falloc*fa(IMemAllocator*p){return(void*)p;}
static HRESULT WINAPI fa_qi(IMemAllocator*p,REFIID i,void**o){if(!o)return E_POINTER;if(eq(i,&iid_unknown)||eq(i,&iid_allocator)){*o=p;p->lpVtbl->AddRef(p);return S_OK;}*o=NULL;return E_NOINTERFACE;}
static ULONG WINAPI fa_addref(IMemAllocator*p){return InterlockedIncrement(&fa(p)->refs);}
static void fa_free_samples(struct falloc*a){if(a->s){for(LONG i=0;i<a->props.cBuffers;i++)release(a->s[i].mem);release(a->s);a->s=NULL;}}
static ULONG WINAPI fa_release(IMemAllocator*p){struct falloc*a=fa(p);LONG n=InterlockedDecrement(&a->refs);if(!n){fa_free_samples(a);release(a);}return n;}
static HRESULT WINAPI fa_set_props(IMemAllocator*p,ALLOCATOR_PROPERTIES*req,ALLOCATOR_PROPERTIES*got){
	struct falloc*a=fa(p);
	if(!req||!got)return E_POINTER;
	if(a->committed)return VFW_E_ALREADY_COMMITTED;
	if(req->cbBuffer<=0)return E_INVALIDARG;
	a->props.cBuffers=req->cBuffers>3?req->cBuffers:3;
	a->props.cbBuffer=req->cbBuffer;
	a->props.cbAlign=req->cbAlign>16?req->cbAlign:16;
	a->props.cbPrefix=req->cbPrefix>8?req->cbPrefix:8;
	*got=a->props;
	return S_OK;
}
static HRESULT WINAPI fa_get_props(IMemAllocator*p,ALLOCATOR_PROPERTIES*got){if(!got)return E_POINTER;*got=fa(p)->props;return S_OK;}
static HRESULT WINAPI fa_commit(IMemAllocator*p){
	struct falloc*a=fa(p);
	if(a->committed)return S_OK;
	if(a->props.cBuffers<=0||a->props.cbBuffer<=0)return VFW_E_SIZENOTSET;
	a->s=alloc(sizeof(*a->s)*a->props.cBuffers);
	if(!a->s)return E_OUTOFMEMORY;
	for(LONG i=0;i<a->props.cBuffers;i++){
		struct fsample*x=&a->s[i];
		DWORD room=a->props.cbPrefix+a->props.cbBuffer+a->props.cbAlign;
		x->iface.lpVtbl=&sample_vtbl;x->a=a;x->size=a->props.cbBuffer;
		x->mem=alloc(room);
		if(!x->mem){fa_free_samples(a);return E_OUTOFMEMORY;}
		/* The data starts aligned, with the prefix before it. */
		x->data=(BYTE*)(((ULONG_PTR)x->mem+a->props.cbPrefix+a->props.cbAlign-1)&~(ULONG_PTR)(a->props.cbAlign-1));
	}
	a->committed=TRUE;
	return S_OK;
}
static HRESULT WINAPI fa_decommit(IMemAllocator*p){fa(p)->committed=FALSE;return S_OK;}
static HRESULT WINAPI fa_get_buffer(IMemAllocator*p,IMediaSample**out,REFERENCE_TIME*start,REFERENCE_TIME*stop,DWORD flags){
	struct falloc*a=fa(p);
	if(!out)return E_POINTER;
	*out=NULL;
	if(!a->committed)return VFW_E_NOT_COMMITTED;
	for(int waited=0;waited<5000;waited++){
		for(LONG i=0;i<a->props.cBuffers;i++){
			struct fsample*x=&a->s[i];
			if(x->busy)continue;
			x->busy=TRUE;x->refs=1;x->actual=0;x->sync=x->preroll=x->disc=FALSE;
			x->has_time=start!=NULL;if(start){x->start=*start;x->stop=stop?*stop:*start;}
			InterlockedIncrement(&a->refs);
			*out=&x->iface;
			return S_OK;
		}
		if(flags&AM_GBF_NOWAIT)return VFW_E_TIMEOUT;
		Sleep(1);
	}
	return VFW_E_TIMEOUT;
}
static HRESULT WINAPI fa_release_buffer(IMemAllocator*p,IMediaSample*s){return S_OK;}
static IMemAllocatorVtbl allocator_vtbl={fa_qi,fa_addref,fa_release,fa_set_props,fa_get_props,fa_commit,fa_decommit,fa_get_buffer,fa_release_buffer};
static BOOL is_own_sample(IMediaSample*s){return s->lpVtbl==&sample_vtbl;}
static HRESULT WINAPI mem_get_allocator(IMemInputPin*p,IMemAllocator**out){
	struct falloc*a;
	if(!out)return E_POINTER;
	*out=NULL;
	if(!pin_from_mem(p)->filter->own_allocator)return VFW_E_NO_ALLOCATOR;
	if(!(a=alloc(sizeof(*a))))return E_OUTOFMEMORY;
	a->iface.lpVtbl=&allocator_vtbl;a->refs=1;
	*out=&a->iface;
	return S_OK;
}
static HRESULT WINAPI mem_notify(IMemInputPin*p,IMemAllocator*a,BOOL ro){if(!a)return E_POINTER;LOWPTR(pin_from_mem(p)->filter,a);return S_OK;}
static HRESULT WINAPI mem_require(IMemInputPin*p,ALLOCATOR_PROPERTIES*a){if(!a)return E_POINTER;a->cBuffers=1;a->cbBuffer=1;a->cbAlign=1;a->cbPrefix=0;return S_OK;}
static HRESULT transform(struct filter*f,IMediaSample*input)
{
	struct pin*out=&f->output;
	if(!out->connected||!out->allocator||!out->peer_mem)return VFW_E_NOT_CONNECTED;
	if(f->audio)return f->encoder?audio_encode(f,input):audio_decode(f,input);
	BITMAPINFOHEADER*ib=media_bih(&f->input.type),*ob=media_bih(&out->type);
	if(!ib||!ob)return VFW_E_INVALID_MEDIA_TYPE;
	DWORD raw=frame_bytes(f->encoder?ib:ob);
	if(!raw)return VFW_E_INVALID_MEDIA_TYPE;
	if(f->picture_size!=raw){release(f->picture);f->picture=alloc(raw);f->picture_size=raw;f->valid=FALSE;if(!f->picture)return E_OUTOFMEMORY;}
	BYTE*src=NULL;if(FAILED(input->lpVtbl->GetPointer(input,&src)))return E_FAIL;LOWPTR(f,src);
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
	 * TESTFILTER_NOTIFY_ERROR: a frame that can't be decoded is reported to the
	 * graph as an event (EC_ERRORABORT, the HRESULT as the first parameter) and
	 * Receive returns S_OK, as filters that report errors asynchronously do.
	 */
	BOOL reported=FALSE;
	if(FAILED(hr)&&f->notify_error&&f->graph){
		IMediaEventSink*ev=NULL;
		if(SUCCEEDED(f->graph->lpVtbl->QueryInterface(f->graph,&iid_event_sink,(void**)&ev))&&ev){
			ev->lpVtbl->Notify(ev,3/*EC_ERRORABORT*/,(LONG_PTR)hr,0);
			ev->lpVtbl->Release(ev);
			hr=S_OK;reported=TRUE;
		}
	}
	/*
	 * TESTFILTER_DYNAMIC: from the second frame on, the decoder switches its
	 * output to YV12 (the chroma planes swapped), announcing it with the type
	 * on the first sample that has it, after the receiver accepts it.
	 */
	if(SUCCEEDED(hr)&&!reported&&f->dynamic&&!f->encoder&&f->frame>=1&&ob->biCompression==mmioFOURCC('I','4','2','0')){
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
	if(SUCCEEDED(hr)&&!reported){sample->lpVtbl->SetPreroll(sample,!f->no_preroll&&input->lpVtbl->IsPreroll(input)==S_OK);sample->lpVtbl->SetDiscontinuity(sample,input->lpVtbl->IsDiscontinuity(input)==S_OK);hr=out->peer_mem->lpVtbl->Receive(out->peer_mem,sample);}
	sample->lpVtbl->Release(sample);f->frame++;return hr;
}
struct async_sample { struct filter *filter; IMediaSample *sample; HRESULT result; };
static DWORD WINAPI transform_thread(void *opaque){struct async_sample*w=opaque;EnterCriticalSection(&w->filter->transform_lock);w->result=transform(w->filter,w->sample);if(FAILED(w->result)&&SUCCEEDED(w->filter->async_error))w->filter->async_error=w->result;LeaveCriticalSection(&w->filter->transform_lock);w->sample->lpVtbl->Release(w->sample);InterlockedDecrement(&w->filter->workers);w->filter->iface.lpVtbl->Release(&w->filter->iface);return 0;}
static HRESULT WINAPI mem_receive(IMemInputPin*p,IMediaSample*s){struct pin*x=pin_from_mem(p);if(!s)return E_POINTER;LOWPTR(x->filter,s);if(x->filter->own_allocator&&!is_own_sample(s))return E_UNEXPECTED;if(x->flushing)return S_FALSE;if(x->filter->require_segment&&!x->filter->segment)return VFW_E_WRONG_STATE;if(!x->filter->async)return transform(x->filter,s);struct async_sample*w=alloc(sizeof(*w));if(!w)return E_OUTOFMEMORY;w->filter=x->filter;w->sample=s;s->lpVtbl->AddRef(s);x->filter->iface.lpVtbl->AddRef(&x->filter->iface);InterlockedIncrement(&x->filter->workers);HANDLE thread=CreateThread(NULL,0,transform_thread,w,0,NULL);if(!thread){InterlockedDecrement(&x->filter->workers);x->filter->iface.lpVtbl->Release(&x->filter->iface);s->lpVtbl->Release(s);release(w);return E_FAIL;}WaitForSingleObject(thread,INFINITE);CloseHandle(thread);HRESULT hr=w->result;release(w);return hr;}
static HRESULT WINAPI mem_multiple(IMemInputPin*p,IMediaSample**s,LONG n,LONG*done){if(!s||!done)return E_POINTER;*done=0;for(;*done<n;(*done)++){HRESULT hr=mem_receive(p,s[*done]);if(FAILED(hr))return hr;}return S_OK;}
static HRESULT WINAPI mem_can_block(IMemInputPin*p){return pin_from_mem(p)->filter->async?S_OK:S_FALSE;}
static IMemInputPinVtbl mem_vtbl={mem_qi,mem_addref,mem_release,mem_get_allocator,mem_notify,mem_require,mem_receive,mem_multiple,mem_can_block};

static HRESULT WINAPI filter_find(IBaseFilter*p,LPCWSTR id,IPin**out){return E_NOTIMPL;}
static HRESULT WINAPI filter_info(IBaseFilter*p,FILTER_INFO*out){if(!out)return E_POINTER;struct filter*f=(void*)p;out->achName[0]='T';out->achName[1]=0;out->pGraph=f->graph;if(f->graph)f->graph->lpVtbl->AddRef(f->graph);return S_OK;}
static HRESULT WINAPI filter_join(IBaseFilter*p,IFilterGraph*g,LPCWSTR name){struct filter*f=(void*)p;if(g)g->lpVtbl->AddRef(g);if(f->graph)f->graph->lpVtbl->Release(f->graph);f->graph=g;return S_OK;}
static HRESULT WINAPI filter_vendor(IBaseFilter*p,LPWSTR*out){return E_NOTIMPL;}
static IBaseFilterVtbl filter_vtbl={filter_qi,filter_addref,filter_release,filter_class,filter_stop,filter_pause,filter_run,filter_state,filter_set_clock,filter_get_clock,filter_enum,filter_find,filter_info,filter_join,filter_vendor};

static BOOL env_on(const char*name){char value[2];return GetEnvironmentVariableA(name,value,sizeof(value))==1&&value[0]=='1';}
HRESULT testfilter_create(BOOL encoder,BOOL audio,REFIID iid,void**out){if(!out)return E_POINTER;*out=NULL;struct filter*f=alloc(sizeof(*f));if(!f)return E_OUTOFMEMORY;f->audio=audio;f->split=env_on("TESTFILTER_AUDIO_SPLIT");f->hold=env_on("TESTFILTER_AUDIO_HOLD");f->float_switch=env_on("TESTFILTER_AUDIO_FLOAT");f->rate_switch=env_on("TESTFILTER_AUDIO_RATE");f->iface.lpVtbl=&filter_vtbl;f->compression.lpVtbl=&compression_vtbl;f->codec_api.lpVtbl=&codec_api_vtbl;f->codec_api_only=env_on("TESTFILTER_CODECAPI");f->persist.lpVtbl=&persist_vtbl;f->refs=1;f->encoder=encoder;f->quality=0.75;f->async=env_on("TESTFILTER_ASYNC");f->no_eos=env_on("TESTFILTER_NO_EOS");f->require_segment=env_on("TESTFILTER_REQUIRE_SEGMENT");f->peer_info=env_on("TESTFILTER_PEER_INFO");f->dynamic=env_on("TESTFILTER_DYNAMIC");f->dynamic_extra=env_on("TESTFILTER_DYNAMIC_EXTRA");f->no_preroll=env_on("TESTFILTER_NO_PREROLL");f->notify_error=env_on("TESTFILTER_NOTIFY_ERROR");f->strict=env_on("TESTFILTER_STRICT");f->own_allocator=env_on("TESTFILTER_OWN_ALLOCATOR");InitializeCriticalSection(&f->transform_lock);f->input=(struct pin){.iface={&pin_vtbl},.mem={&mem_vtbl},.filter=f,.direction=PINDIR_INPUT};f->output=(struct pin){.iface={&pin_vtbl},.mem={&mem_vtbl},.filter=f,.direction=PINDIR_OUTPUT};InterlockedIncrement(&objects);HRESULT hr=filter_qi(&f->iface,iid,out);filter_release(&f->iface);return hr;}
LONG testfilter_objects(void){return objects;}
