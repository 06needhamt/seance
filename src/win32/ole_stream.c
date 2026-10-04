/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * ole32's streams on global memory (CreateStreamOnHGlobal, after Wine's
 * hglobalstream.c), and marshalling as it comes out with one process and
 * no apartments enforced: an interface "marshalled" into a stream is its
 * pointer, with a reference, and unmarshalling hands the same pointer back.
 * The free-threaded marshaler is an object that marshals nothing.
 */
#include <stdlib.h>
#include <string.h>

#include "common/log.h"
#include "win32/internal.h"
#include "win32/k32.h"
#include "win32/ole.h"

static int guid_eq(const GUID *a, const GUID *b)
{
	return !memcmp(a, b, sizeof(GUID));
}

/*
 * The memory behind a stream, shared by its clones: the handle, and the
 * stream's size, which may be less than the block's.
 */
struct hglobal_data {
	LONG refs;
	HGLOBAL h;
	BOOL delete_on_release;
	ULONG size;
};

struct hstream {
	IStream iface;
	LONG refs;
	struct hglobal_data *data;
	ULONG pos;
};

static struct hstream *impl(IStream *s)
{
	return (struct hstream *)s;
}

static void data_release(struct hglobal_data *d)
{
	if (__atomic_sub_fetch(&d->refs, 1, __ATOMIC_SEQ_CST))
		return;
	if (d->delete_on_release)
		global_free(d->h);
	free(d);
}

static const IStreamVtbl hstream_vtbl;

static HRESULT hstream_new(struct hglobal_data *d, IStream **out)
{
	struct hstream *s = calloc(1, sizeof(*s));

	if (!s)
		return E_OUTOFMEMORY;
	s->iface.lpVtbl = &hstream_vtbl;
	s->refs = 1;
	s->data = d;
	__atomic_add_fetch(&d->refs, 1, __ATOMIC_SEQ_CST);
	*out = &s->iface;
	return S_OK;
}

WINFN(HRESULT) hs_QueryInterface(IStream *self, REFIID riid, void **ppv)
{
	if (!ppv)
		return E_POINTER;
	if (riid && (guid_eq(riid, &IID_IUnknown) ||
		     guid_eq(riid, &IID_ISequentialStream) ||
		     guid_eq(riid, &IID_IStream))) {
		self->lpVtbl->AddRef(self);
		*ppv = self;
		return S_OK;
	}
	*ppv = NULL;
	return E_NOINTERFACE;
}

WINFN(ULONG) hs_AddRef(IStream *self)
{
	return __atomic_add_fetch(&impl(self)->refs, 1, __ATOMIC_SEQ_CST);
}

WINFN(ULONG) hs_Release(IStream *self)
{
	struct hstream *s = impl(self);
	ULONG r = __atomic_sub_fetch(&s->refs, 1, __ATOMIC_SEQ_CST);

	if (!r) {
		data_release(s->data);
		free(s);
	}
	return r;
}

/* Reads what there is, short of cb at the end; S_OK either way. */
WINFN(HRESULT) hs_Read(IStream *self, void *pv, ULONG cb, ULONG *read)
{
	struct hstream *s = impl(self);
	ULONG n = 0;

	if (!pv)
		return STG_E_INVALIDPOINTER;
	if (s->pos < s->data->size) {
		n = s->data->size - s->pos;
		if (n > cb)
			n = cb;
		BYTE *p = global_lock(s->data->h);
		if (!p)
			return E_OUTOFMEMORY;
		memcpy(pv, p + s->pos, n);
		global_unlock(s->data->h);
		s->pos += n;
	}
	if (read)
		*read = n;
	return S_OK;
}

WINFN(HRESULT) hs_SetSize(IStream *self, ULARGE_INTEGER size)
{
	struct hglobal_data *d = impl(self)->data;

	if (size.u.HighPart)
		return STG_E_MEDIUMFULL;
	if (size.u.LowPart == d->size)
		return S_OK;
	/*
	 * The block grows as needed, but never shrinks below its size. A
	 * fixed block (Windows wants a moveable one) may move as it grows.
	 */
	if (size.u.LowPart > global_size(d->h)) {
		HGLOBAL h = global_realloc(d->h, size.u.LowPart, GMEM_MOVEABLE);
		if (!h)
			return STG_E_MEDIUMFULL;
		d->h = h;
	}
	d->size = size.u.LowPart;
	return S_OK;
}

WINFN(HRESULT) hs_Write(IStream *self, const void *pv, ULONG cb,
			ULONG *written)
{
	struct hstream *s = impl(self);

	if (written)
		*written = 0;
	if (!cb)
		return S_OK;
	if (!pv)
		return STG_E_INVALIDPOINTER;
	if (s->pos + cb < s->pos)
		return STG_E_MEDIUMFULL;
	if (s->pos + cb > s->data->size) {
		ULARGE_INTEGER size;
		size.QuadPart = s->pos + cb;
		HRESULT hr = hs_SetSize(self, size);
		if (FAILED(hr))
			return hr;
	}
	BYTE *p = global_lock(s->data->h);
	if (!p)
		return STG_E_MEDIUMFULL;
	memcpy(p + s->pos, pv, cb);
	global_unlock(s->data->h);
	s->pos += cb;
	if (written)
		*written = cb;
	return S_OK;
}

WINFN(HRESULT) hs_Seek(IStream *self, LARGE_INTEGER move, DWORD origin,
		       ULARGE_INTEGER *pos)
{
	struct hstream *s = impl(self);
	LONGLONG base;

	switch (origin) {
	case STREAM_SEEK_SET: base = 0; break;
	case STREAM_SEEK_CUR: base = s->pos; break;
	case STREAM_SEEK_END: base = s->data->size; break;
	default: return STG_E_SEEKERROR;
	}
	LONGLONG to = base + move.QuadPart;
	if (to < 0 || to > 0xffffffffLL)
		return STG_E_SEEKERROR;
	s->pos = (ULONG)to;
	if (pos)
		pos->QuadPart = s->pos;
	return S_OK;
}

WINFN(HRESULT) hs_CopyTo(IStream *self, IStream *to, ULARGE_INTEGER cb,
			 ULARGE_INTEGER *read, ULARGE_INTEGER *written)
{
	ULONGLONG total_read = 0, total_written = 0;
	HRESULT hr = S_OK;
	BYTE buf[4096];

	if (!to)
		return STG_E_INVALIDPOINTER;
	while (total_read < cb.QuadPart) {
		ULONG want = sizeof(buf), got = 0, put = 0;
		if (cb.QuadPart - total_read < want)
			want = (ULONG)(cb.QuadPart - total_read);
		hr = hs_Read(self, buf, want, &got);
		if (FAILED(hr) || !got)
			break;
		total_read += got;
		hr = to->lpVtbl->Write(to, buf, got, &put);
		total_written += put;
		if (FAILED(hr) || put != got)
			break;
	}
	if (read)
		read->QuadPart = total_read;
	if (written)
		written->QuadPart = total_written;
	return hr;
}

WINFN(HRESULT) hs_Commit(IStream *self, DWORD flags) { return S_OK; }
WINFN(HRESULT) hs_Revert(IStream *self) { return S_OK; }

WINFN(HRESULT) hs_LockRegion(IStream *self, ULARGE_INTEGER offset,
			     ULARGE_INTEGER cb, DWORD type)
{
	return STG_E_INVALIDFUNCTION;
}

WINFN(HRESULT) hs_UnlockRegion(IStream *self, ULARGE_INTEGER offset,
			       ULARGE_INTEGER cb, DWORD type)
{
	return S_OK;
}

WINFN(HRESULT) hs_Stat(IStream *self, STATSTG *stat, DWORD flags)
{
	if (!stat)
		return STG_E_INVALIDPOINTER;
	memset(stat, 0, sizeof(*stat));
	stat->type = STGTY_STREAM;
	stat->cbSize.QuadPart = impl(self)->data->size;
	stat->grfMode = STGM_READWRITE;
	return S_OK;
}

/* A clone shares the memory but keeps its own position. */
WINFN(HRESULT) hs_Clone(IStream *self, IStream **clone)
{
	if (!clone)
		return STG_E_INVALIDPOINTER;
	HRESULT hr = hstream_new(impl(self)->data, clone);
	if (SUCCEEDED(hr))
		impl(*clone)->pos = impl(self)->pos;
	return hr;
}

static const IStreamVtbl hstream_vtbl = {
	hs_QueryInterface, hs_AddRef, hs_Release, hs_Read, hs_Write, hs_Seek,
	hs_SetSize, hs_CopyTo, hs_Commit, hs_Revert, hs_LockRegion,
	hs_UnlockRegion, hs_Stat, hs_Clone,
};

HRESULT ole_stream_on_hglobal(HGLOBAL h, BOOL delete_on_release,
			      IStream **out)
{
	if (!out)
		return E_INVALIDARG;
	*out = NULL;
	struct hglobal_data *d = calloc(1, sizeof(*d));
	if (!d)
		return E_OUTOFMEMORY;
	d->h = h ? h : global_alloc(GMEM_MOVEABLE | GMEM_NODISCARD, 0);
	if (!d->h) {
		free(d);
		return E_OUTOFMEMORY;
	}
	/* The stream starts as big as the block. */
	d->size = (ULONG)global_size(d->h);
	d->delete_on_release = delete_on_release;
	HRESULT hr = hstream_new(d, out);
	if (FAILED(hr)) {
		if (!h)
			global_free(d->h);
		free(d);
	}
	return hr;
}

HRESULT ole_hglobal_from_stream(IStream *stream, HGLOBAL *out)
{
	if (!out)
		return E_INVALIDARG;
	*out = NULL;
	if (!stream || stream->lpVtbl != &hstream_vtbl)
		return E_INVALIDARG;
	*out = impl(stream)->data->h;
	return S_OK;
}

WINFN(HRESULT) ole_CreateStreamOnHGlobal(HGLOBAL h, BOOL delete_on_release,
					 LPSTREAM *out)
{
	return ole_stream_on_hglobal(h, delete_on_release, out);
}

WINFN(HRESULT) ole_GetHGlobalFromStream(IStream *stream, HGLOBAL *out)
{
	return ole_hglobal_from_stream(stream, out);
}

/*
 * Marshalling: what CoMarshalInterface writes, and CoUnmarshalInterface
 * reads back.
 */
#define MARSHAL_MAGIC 0x4c48534d	/* "MSHL" */

struct marshal_data {
	DWORD magic;
	IID iid;
	IUnknown *obj;		/* holds a reference until unmarshalled */
};

static HRESULT marshal_read(IStream *stream, struct marshal_data *m)
{
	ULONG got = 0;

	if (!stream)
		return E_INVALIDARG;
	HRESULT hr = stream->lpVtbl->Read(stream, m, sizeof(*m), &got);
	if (FAILED(hr))
		return hr;
	if (got != sizeof(*m) || m->magic != MARSHAL_MAGIC || !m->obj)
		return STG_E_READFAULT;
	return S_OK;
}

WINFN(HRESULT) ole_CoMarshalInterface(IStream *stream, REFIID riid,
				      IUnknown *obj, DWORD dest, void *dest_ptr,
				      DWORD flags)
{
	struct marshal_data m = { .magic = MARSHAL_MAGIC };
	void *iface = NULL;

	if (!stream || !riid || !obj)
		return E_INVALIDARG;
	HRESULT hr = obj->lpVtbl->QueryInterface(obj, riid, &iface);
	if (FAILED(hr))
		return hr;
	m.iid = *riid;
	m.obj = iface;
	ULONG put = 0;
	hr = stream->lpVtbl->Write(stream, &m, sizeof(m), &put);
	if (FAILED(hr) || put != sizeof(m)) {
		m.obj->lpVtbl->Release(m.obj);
		return FAILED(hr) ? hr : STG_E_MEDIUMFULL;
	}
	return S_OK;
}

WINFN(HRESULT) ole_CoUnmarshalInterface(IStream *stream, REFIID riid,
					void **ppv)
{
	struct marshal_data m;

	if (!ppv)
		return E_INVALIDARG;
	*ppv = NULL;
	HRESULT hr = marshal_read(stream, &m);
	if (FAILED(hr))
		return hr;
	hr = m.obj->lpVtbl->QueryInterface(m.obj, riid ? riid : &m.iid, ppv);
	m.obj->lpVtbl->Release(m.obj);
	return hr;
}

WINFN(HRESULT) ole_CoReleaseMarshalData(IStream *stream)
{
	struct marshal_data m;
	HRESULT hr = marshal_read(stream, &m);

	if (SUCCEEDED(hr))
		m.obj->lpVtbl->Release(m.obj);
	return hr;
}

WINFN(HRESULT) ole_CoMarshalInterThreadInterfaceInStream(REFIID riid,
							 LPUNKNOWN obj,
							 LPSTREAM *out)
{
	LARGE_INTEGER zero = { .QuadPart = 0 };

	if (!out)
		return E_INVALIDARG;
	*out = NULL;
	HRESULT hr = ole_stream_on_hglobal(NULL, TRUE, out);
	if (FAILED(hr))
		return hr;
	hr = ole_CoMarshalInterface(*out, riid, obj, 0, NULL, 0);
	if (FAILED(hr)) {
		(*out)->lpVtbl->Release(*out);
		*out = NULL;
		return hr;
	}
	(*out)->lpVtbl->Seek(*out, zero, STREAM_SEEK_SET, NULL);
	return S_OK;
}

WINFN(HRESULT) ole_CoGetInterfaceAndReleaseStream(LPSTREAM stream, REFIID riid,
						  LPVOID *ppv)
{
	if (!stream)
		return E_INVALIDARG;
	HRESULT hr = ole_CoUnmarshalInterface(stream, riid, ppv);
	stream->lpVtbl->Release(stream);
	return hr;
}

/*
 * The free-threaded marshaler: an inner object for its outer to aggregate,
 * which answers IUnknown only; nothing here needs marshalling.
 */
struct ftm {
	IUnknown iface;
	LONG refs;
};

WINFN(HRESULT) ftm_QueryInterface(IUnknown *self, REFIID riid, void **ppv)
{
	if (!ppv)
		return E_POINTER;
	if (riid && guid_eq(riid, &IID_IUnknown)) {
		self->lpVtbl->AddRef(self);
		*ppv = self;
		return S_OK;
	}
	*ppv = NULL;
	return E_NOINTERFACE;
}

WINFN(ULONG) ftm_AddRef(IUnknown *self)
{
	return __atomic_add_fetch(&((struct ftm *)self)->refs, 1,
				  __ATOMIC_SEQ_CST);
}

WINFN(ULONG) ftm_Release(IUnknown *self)
{
	ULONG r = __atomic_sub_fetch(&((struct ftm *)self)->refs, 1,
				     __ATOMIC_SEQ_CST);
	if (!r)
		free(self);
	return r;
}

static const IUnknownVtbl ftm_vtbl = {
	ftm_QueryInterface, ftm_AddRef, ftm_Release,
};

WINFN(HRESULT) ole_CoCreateFreeThreadedMarshaler(LPUNKNOWN outer,
						 LPUNKNOWN *out)
{
	if (!out)
		return E_INVALIDARG;
	struct ftm *f = calloc(1, sizeof(*f));
	if (!(*out = (IUnknown *)f))
		return E_OUTOFMEMORY;
	f->iface.lpVtbl = &ftm_vtbl;
	f->refs = 1;
	TRACE("CoCreateFreeThreadedMarshaler(%p) = %p", (void *)outer,
	      (void *)f);
	return S_OK;
}

#define E(name) { #name, 0, (void *)ole_##name }

const struct builtin_export ole_stream_exports[] = {
	E(CoCreateFreeThreadedMarshaler),
	E(CoGetInterfaceAndReleaseStream),
	E(CoMarshalInterThreadInterfaceInStream),
	E(CoMarshalInterface),
	E(CoReleaseMarshalData),
	E(CoUnmarshalInterface),
	E(CreateStreamOnHGlobal),
	E(GetHGlobalFromStream),
	BUILTIN_END
};
