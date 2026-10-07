/* SPDX-License-Identifier: GPL-2.0-only */
/* The harness's IMediaBuffer: a block of memory and how much of it is data. */
#include <stdlib.h>
#include <string.h>

#include "dmo/dmo.h"

struct buffer {
	IMediaBuffer iface;
	LONG refs;
	DWORD max, length;
	BYTE *data;
};

static struct buffer *from(IMediaBuffer *b)
{
	return (struct buffer *)b;
}

static int guid_eq(const GUID *a, const GUID *b)
{
	return !memcmp(a, b, sizeof(GUID));
}

static HRESULT WINAPI buf_QueryInterface(IMediaBuffer *self, REFIID riid,
					 void **ppv)
{
	if (!ppv)
		return E_POINTER;
	if (riid && (guid_eq(riid, &IID_IUnknown) ||
		     guid_eq(riid, &IID_IMediaBuffer))) {
		*ppv = self;
		self->lpVtbl->AddRef(self);
		return S_OK;
	}
	*ppv = NULL;
	return E_NOINTERFACE;
}

static ULONG WINAPI buf_AddRef(IMediaBuffer *self)
{
	return (ULONG)__atomic_add_fetch(&from(self)->refs, 1,
					 __ATOMIC_SEQ_CST);
}

static ULONG WINAPI buf_Release(IMediaBuffer *self)
{
	struct buffer *b = from(self);
	LONG refs = __atomic_sub_fetch(&b->refs, 1, __ATOMIC_SEQ_CST);

	if (!refs) {
		free(b->data);
		free(b);
	}
	return (ULONG)refs;
}

static HRESULT WINAPI buf_SetLength(IMediaBuffer *self, DWORD length)
{
	struct buffer *b = from(self);

	if (length > b->max)
		return E_INVALIDARG;
	b->length = length;
	return S_OK;
}

static HRESULT WINAPI buf_GetMaxLength(IMediaBuffer *self, DWORD *max)
{
	if (!max)
		return E_POINTER;
	*max = from(self)->max;
	return S_OK;
}

/* Either pointer may be NULL, but not both. */
static HRESULT WINAPI buf_GetBufferAndLength(IMediaBuffer *self, BYTE **data,
					     DWORD *length)
{
	struct buffer *b = from(self);

	if (!data && !length)
		return E_POINTER;
	if (data)
		*data = b->data;
	if (length)
		*length = b->length;
	return S_OK;
}

static const IMediaBufferVtbl buffer_vtbl = {
	buf_QueryInterface, buf_AddRef, buf_Release, buf_SetLength,
	buf_GetMaxLength, buf_GetBufferAndLength,
};

HRESULT dmo_buffer_create(DWORD max_length, DWORD alignment,
			  IMediaBuffer **out)
{
	struct buffer *b;
	size_t align = 16;

	if (!out)
		return E_POINTER;
	*out = NULL;
	while (align < alignment && align < 4096)
		align *= 2;
	if (!(b = calloc(1, sizeof(*b))))
		return E_OUTOFMEMORY;
	size_t size = max_length ? max_length : 1;
	size = (size + align - 1) / align * align;
	if (posix_memalign((void **)&b->data, align, size)) {
		free(b);
		return E_OUTOFMEMORY;
	}
	memset(b->data, 0, size);
	b->iface.lpVtbl = &buffer_vtbl;
	b->refs = 1;
	b->max = max_length;
	*out = &b->iface;
	return S_OK;
}

HRESULT dmo_buffer_from(const void *data, DWORD size, IMediaBuffer **out)
{
	HRESULT hr = dmo_buffer_create(size, 0, out);

	if (SUCCEEDED(hr) && size) {
		memcpy(from(*out)->data, data, size);
		from(*out)->length = size;
	}
	return hr;
}
