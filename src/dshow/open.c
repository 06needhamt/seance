/* SPDX-License-Identifier: GPL-2.0-only */
/* Loading an unregistered DirectShow class directly from its server. */
#include <stdlib.h>
#include <string.h>

#include "dshow/discover.h"
#include "dshow/dshow.h"
#include "loader/modules.h"

struct dshow_filter {
	struct module *module;
};

HRESULT dshow_filter_open(const char *path, const CLSID *selected, int encoder,
			  struct dshow_filter **server, IBaseFilter **filter,
			  CLSID *chosen)
{
	struct qz_filter *found = NULL;
	size_t count = 0;
	HRESULT registration = E_FAIL, hr = CLASS_E_CLASSNOTAVAILABLE;
	CLSID clsid;

	if (!path || !server || !filter)
		return E_POINTER;
	*server = NULL;
	*filter = NULL;
	if (dshow_discover(path, &found, &count, &registration))
		return E_FAIL;
	int matches = 0;
	for (size_t i = 0; i < count; i++) {
		if (selected && memcmp(selected, &found[i].clsid, sizeof(*selected)))
			continue;
		if (dshow_role(&found[i]) !=
				(encoder ? DSHOW_VIDEO_ENCODER : DSHOW_VIDEO_DECODER))
			continue;
		clsid = found[i].clsid;
		matches++;
	}
	qz_filters_free(found, count);
	if (matches != 1)
		return matches ? VFW_E_DUPLICATE_NAME : CLASS_E_CLASSNOTAVAILABLE;
	hr = dshow_filter_create(path, &clsid, server, filter);
	if (SUCCEEDED(hr) && chosen)
		*chosen = clsid;
	return hr;
}

HRESULT dshow_filter_create(const char *path, const CLSID *clsid,
			    struct dshow_filter **server, IBaseFilter **filter)
{
	HRESULT hr = CLASS_E_CLASSNOTAVAILABLE;

	if (!path || !clsid || !server || !filter)
		return E_POINTER;
	*server = NULL;
	*filter = NULL;
	struct dshow_filter *s = calloc(1, sizeof(*s));
	if (!s)
		return E_OUTOFMEMORY;
	s->module = mod_load(path, NULL);
	if (!s->module) {
		free(s);
		return E_FAIL;
	}
	LPFNGETCLASSOBJECT get = (LPFNGETCLASSOBJECT)mod_get_proc(
		s->module, "DllGetClassObject");
	IClassFactory *factory = NULL;
	if (get)
		hr = get(clsid, &IID_IClassFactory, (void **)&factory);
	if (SUCCEEDED(hr))
		hr = factory->lpVtbl->CreateInstance(factory, NULL, &IID_IBaseFilter,
						      (void **)filter);
	if (factory)
		factory->lpVtbl->Release(factory);
	if (FAILED(hr)) {
		*filter = NULL;
		mod_free(s->module);
		free(s);
		return hr;
	}
	*server = s;
	return S_OK;
}

void dshow_filter_close(struct dshow_filter *server, IBaseFilter *filter)
{
	if (filter)
		filter->lpVtbl->Release(filter);
	if (server) {
		mod_free(server->module);
		free(server);
	}
}
