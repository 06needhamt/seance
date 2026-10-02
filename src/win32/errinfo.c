/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * Error information: CreateErrorInfo, SetErrorInfo and GetErrorInfo,
 * which ole32 exports and oleaut32 forwards to it. One object is both the
 * ICreateErrorInfo that fills it and the IErrorInfo that reads it; each
 * thread holds at most one, which GetErrorInfo hands over and clears.
 */
#include <stddef.h>
#include <stdlib.h>
#include <string.h>

#include "win32/internal.h"
#include "win32/oleaut.h"

static const IID IID_IErrorInfo = { 0x1cf2b120, 0x547d, 0x101b,
	{ 0x8e, 0x65, 0x08, 0x00, 0x2b, 0x2b, 0xd1, 0x19 } };
static const IID IID_ICreateErrorInfo = { 0x22f03340, 0x547d, 0x101b,
	{ 0x8e, 0x65, 0x08, 0x00, 0x2b, 0x2b, 0xd1, 0x19 } };

struct errinfo {
	IErrorInfo info;
	ICreateErrorInfo create;
	LONG refs;
	GUID guid;
	BSTR source, description, help_file;
	DWORD help_context;
};

static _Thread_local IErrorInfo *current;

static struct errinfo *from_info(IErrorInfo *i)
{
	return (struct errinfo *)((BYTE *)i - offsetof(struct errinfo, info));
}

static struct errinfo *from_create(ICreateErrorInfo *c)
{
	return (struct errinfo *)((BYTE *)c - offsetof(struct errinfo, create));
}

static HRESULT query(struct errinfo *e, REFIID riid, void **out)
{
	if (!out)
		return E_POINTER;
	if (!memcmp(riid, &oa_IID_IUnknown, sizeof(IID)) ||
	    !memcmp(riid, &IID_IErrorInfo, sizeof(IID)))
		*out = &e->info;
	else if (!memcmp(riid, &IID_ICreateErrorInfo, sizeof(IID)))
		*out = &e->create;
	else {
		*out = NULL;
		return E_NOINTERFACE;
	}
	__atomic_add_fetch(&e->refs, 1, __ATOMIC_RELAXED);
	return S_OK;
}

static ULONG addref(struct errinfo *e)
{
	return (ULONG)__atomic_add_fetch(&e->refs, 1, __ATOMIC_RELAXED);
}

static ULONG release(struct errinfo *e)
{
	LONG n = __atomic_sub_fetch(&e->refs, 1, __ATOMIC_ACQ_REL);

	if (!n) {
		oa_SysFreeString(e->source);
		oa_SysFreeString(e->description);
		oa_SysFreeString(e->help_file);
		free(e);
	}
	return (ULONG)n;
}

/* A copy of s for the caller, or NULL for none. */
static HRESULT give(BSTR s, BSTR *out)
{
	if (!out)
		return E_INVALIDARG;
	*out = s ? oa_SysAllocString(s) : NULL;
	return s && !*out ? E_OUTOFMEMORY : S_OK;
}

static HRESULT keep(BSTR *field, LPOLESTR s)
{
	BSTR b = s ? oa_SysAllocString(s) : NULL;

	if (s && !b)
		return E_OUTOFMEMORY;
	oa_SysFreeString(*field);
	*field = b;
	return S_OK;
}

/* IErrorInfo. */

static HRESULT WINAPI ei_QueryInterface(IErrorInfo *i, REFIID riid, void **out)
{
	return query(from_info(i), riid, out);
}

static ULONG WINAPI ei_AddRef(IErrorInfo *i)
{
	return addref(from_info(i));
}

static ULONG WINAPI ei_Release(IErrorInfo *i)
{
	return release(from_info(i));
}

static HRESULT WINAPI ei_GetGUID(IErrorInfo *i, GUID *guid)
{
	if (!guid)
		return E_INVALIDARG;
	*guid = from_info(i)->guid;
	return S_OK;
}

static HRESULT WINAPI ei_GetSource(IErrorInfo *i, BSTR *out)
{
	return give(from_info(i)->source, out);
}

static HRESULT WINAPI ei_GetDescription(IErrorInfo *i, BSTR *out)
{
	return give(from_info(i)->description, out);
}

static HRESULT WINAPI ei_GetHelpFile(IErrorInfo *i, BSTR *out)
{
	return give(from_info(i)->help_file, out);
}

static HRESULT WINAPI ei_GetHelpContext(IErrorInfo *i, DWORD *out)
{
	if (!out)
		return E_INVALIDARG;
	*out = from_info(i)->help_context;
	return S_OK;
}

static const IErrorInfoVtbl info_vtbl = {
	ei_QueryInterface, ei_AddRef, ei_Release, ei_GetGUID, ei_GetSource,
	ei_GetDescription, ei_GetHelpFile, ei_GetHelpContext,
};

/* ICreateErrorInfo. */

static HRESULT WINAPI ce_QueryInterface(ICreateErrorInfo *c, REFIID riid,
					void **out)
{
	return query(from_create(c), riid, out);
}

static ULONG WINAPI ce_AddRef(ICreateErrorInfo *c)
{
	return addref(from_create(c));
}

static ULONG WINAPI ce_Release(ICreateErrorInfo *c)
{
	return release(from_create(c));
}

static HRESULT WINAPI ce_SetGUID(ICreateErrorInfo *c, REFGUID guid)
{
	if (!guid)
		return E_INVALIDARG;
	from_create(c)->guid = *guid;
	return S_OK;
}

static HRESULT WINAPI ce_SetSource(ICreateErrorInfo *c, LPOLESTR s)
{
	return keep(&from_create(c)->source, s);
}

static HRESULT WINAPI ce_SetDescription(ICreateErrorInfo *c, LPOLESTR s)
{
	return keep(&from_create(c)->description, s);
}

static HRESULT WINAPI ce_SetHelpFile(ICreateErrorInfo *c, LPOLESTR s)
{
	return keep(&from_create(c)->help_file, s);
}

static HRESULT WINAPI ce_SetHelpContext(ICreateErrorInfo *c, DWORD context)
{
	from_create(c)->help_context = context;
	return S_OK;
}

static const ICreateErrorInfoVtbl create_vtbl = {
	ce_QueryInterface, ce_AddRef, ce_Release, ce_SetGUID, ce_SetSource,
	ce_SetDescription, ce_SetHelpFile, ce_SetHelpContext,
};

HRESULT STUB_WINAPI oa_CreateErrorInfo(ICreateErrorInfo **out)
{
	if (!out)
		return E_INVALIDARG;
	struct errinfo *e = calloc(1, sizeof(*e));
	if (!e) {
		*out = NULL;
		return E_OUTOFMEMORY;
	}
	e->info.lpVtbl = &info_vtbl;
	e->create.lpVtbl = &create_vtbl;
	e->refs = 1;
	*out = &e->create;
	return S_OK;
}

HRESULT STUB_WINAPI oa_SetErrorInfo(ULONG reserved, IErrorInfo *info)
{
	if (reserved)
		return E_INVALIDARG;
	if (info)
		info->lpVtbl->AddRef(info);
	IErrorInfo *old = current;
	current = info;
	if (old)
		old->lpVtbl->Release(old);
	return S_OK;
}

/* The thread's error information, handed over (and cleared); S_FALSE if none. */
HRESULT STUB_WINAPI oa_GetErrorInfo(ULONG reserved, IErrorInfo **out)
{
	if (!out)
		return E_INVALIDARG;
	if (reserved) {
		*out = NULL;
		return E_INVALIDARG;
	}
	*out = current;
	current = NULL;
	return *out ? S_OK : S_FALSE;
}

const struct builtin_export oa_errinfo_ole32_exports[] = {
	{ "CreateErrorInfo", 0, (void *)oa_CreateErrorInfo },
	{ "GetErrorInfo", 0, (void *)oa_GetErrorInfo },
	{ "SetErrorInfo", 0, (void *)oa_SetErrorInfo },
	BUILTIN_END
};
