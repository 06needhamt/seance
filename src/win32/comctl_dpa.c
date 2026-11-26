/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * comctl32: dynamic pointer and structure arrays (DPA, DSA), the Alloc
 * family and Str_GetPtr/Str_SetPtr, after Wine's dpa.c and dsa.c.
 *
 * The first fields of both arrays are public: commctrl.h's macros read
 * the item count at offset 0, and DPA_GetPtrPtr the pointer array next
 * to it. Memory comes from the process heap (or the heap DPA_CreateEx is
 * given), so Win32 code can use it.
 */
#include <stdlib.h>
#include <string.h>

#include "win32/comctl.h"
#include "win32/internal.h"
#include "win32/k32.h"
#include "win32/objbase.h"

/* Alloc, ReAlloc, Free, GetSize. */

void *comctl_alloc(size_t size)
{
	return heap_alloc(heap_process(), HEAP_ZERO_MEMORY, size);
}

void *comctl_realloc(void *p, size_t size)
{
	return p ? heap_realloc(heap_process(), HEAP_ZERO_MEMORY, p, size) :
		   comctl_alloc(size);
}

void comctl_free(void *p)
{
	if (p)
		heap_free(heap_process(), 0, p);
}

WINFN(LPVOID) cc_Alloc(DWORD size) { return comctl_alloc(size); }
WINFN(LPVOID) cc_ReAlloc(LPVOID p, DWORD size) { return comctl_realloc(p, size); }

WINFN(BOOL) cc_Free(LPVOID p)
{
	return !p || heap_free(heap_process(), 0, p);
}

WINFN(DWORD) cc_GetSize(LPVOID p)
{
	return p ? (DWORD)heap_size(heap_process(), 0, p) : 0;
}

/*
 * Str_GetPtr copies at most max - 1 characters and a NUL, returning how
 * many it copied (or, with no buffer, the size needed); Str_SetPtr
 * replaces an Alloc'ed string with a copy of src (NULL frees it).
 */
WINFN(INT) cc_Str_GetPtrA(LPCSTR src, LPSTR dst, INT max)
{
	if (!dst && src)
		return (INT)strlen(src) + 1;
	if (max <= 0 || !dst)
		return 0;
	if (!src) {
		*dst = '\0';
		return 0;
	}
	INT len = (INT)strlen(src);
	if (len >= max)
		len = max - 1;
	memmove(dst, src, len);
	dst[len] = '\0';
	return len;
}

WINFN(INT) cc_Str_GetPtrW(LPCWSTR src, LPWSTR dst, INT max)
{
	if (!dst && src)
		return (INT)wstrlen(src) + 1;
	if (max <= 0 || !dst)
		return 0;
	if (!src) {
		*dst = 0;
		return 0;
	}
	INT len = (INT)wstrlen(src);
	if (len >= max)
		len = max - 1;
	memmove(dst, src, len * sizeof(WCHAR));
	dst[len] = 0;
	return len;
}

static BOOL set_ptr(void **dst, const void *src, size_t bytes)
{
	if (!dst)
		return FALSE;
	if (!src) {
		comctl_free(*dst);
		*dst = NULL;
		return TRUE;
	}
	void *p = comctl_realloc(*dst, bytes);
	if (!p)
		return FALSE;
	memmove(p, src, bytes);
	*dst = p;
	return TRUE;
}

WINFN(BOOL) cc_Str_SetPtrA(LPSTR *dst, LPCSTR src)
{
	return set_ptr((void **)dst, src, src ? strlen(src) + 1 : 0);
}

WINFN(BOOL) cc_Str_SetPtrW(LPWSTR *dst, LPCWSTR src)
{
	return set_ptr((void **)dst, src,
		       src ? (wstrlen(src) + 1) * sizeof(WCHAR) : 0);
}

/* DSA: an array of fixed-size items. */

struct _DSA {
	INT nItemCount;		/* public: DSA_GetItemCount */
	BYTE *pData;
	INT nMaxCount;
	INT nItemSize;
	INT nGrow;
};

WINFN(HDSA) cc_DSA_Create(INT size, INT grow)
{
	if (size <= 0)
		return NULL;
	HDSA d = comctl_alloc(sizeof(*d));
	if (d) {
		d->nItemSize = size;
		d->nGrow = grow > 0 ? grow : 1;
	}
	return d;
}

WINFN(BOOL) cc_DSA_DeleteAllItems(HDSA d)
{
	if (!d)
		return FALSE;
	comctl_free(d->pData);
	d->pData = NULL;
	d->nItemCount = d->nMaxCount = 0;
	return TRUE;
}

WINFN(BOOL) cc_DSA_Destroy(HDSA d)
{
	if (!d)
		return FALSE;
	comctl_free(d->pData);
	comctl_free(d);
	return TRUE;
}

/* Room for n items, growing by whole nGrow steps. */
static BOOL dsa_reserve(HDSA d, INT n)
{
	if (n <= d->nMaxCount)
		return TRUE;
	INT max = ((n + d->nGrow - 1) / d->nGrow) * d->nGrow;
	if (max < n || (size_t)max > ((size_t)-1 >> 1) / (size_t)d->nItemSize)
		return FALSE;
	BYTE *p = comctl_realloc(d->pData, (size_t)max * d->nItemSize);
	if (!p)
		return FALSE;
	d->pData = p;
	d->nMaxCount = max;
	return TRUE;
}

static BYTE *dsa_item(HDSA d, INT i)
{
	return d->pData + (size_t)i * d->nItemSize;
}

WINFN(BOOL) cc_DSA_GetItem(HDSA d, INT i, LPVOID out)
{
	if (!d || !out || i < 0 || i >= d->nItemCount)
		return FALSE;
	memmove(out, dsa_item(d, i), d->nItemSize);
	return TRUE;
}

WINFN(LPVOID) cc_DSA_GetItemPtr(HDSA d, INT i)
{
	return d && i >= 0 && i < d->nItemCount ? dsa_item(d, i) : NULL;
}

/* Beyond the end, the array grows with zeroed items up to i. */
WINFN(BOOL) cc_DSA_SetItem(HDSA d, INT i, LPVOID item)
{
	if (!d || !item || i < 0)
		return FALSE;
	if (i >= d->nItemCount) {
		if (i == 0x7fffffff || !dsa_reserve(d, i + 1))
			return FALSE;
		d->nItemCount = i + 1;
	}
	memmove(dsa_item(d, i), item, d->nItemSize);
	return TRUE;
}

/* At i (past the end: appended); the index it went to, or -1. */
WINFN(INT) cc_DSA_InsertItem(HDSA d, INT i, LPVOID item)
{
	if (!d || !item || i < 0)
		return -1;
	if (i > d->nItemCount)
		i = d->nItemCount;
	if (!dsa_reserve(d, d->nItemCount + 1))
		return -1;
	memmove(dsa_item(d, i + 1), dsa_item(d, i),
		(size_t)(d->nItemCount - i) * d->nItemSize);
	memmove(dsa_item(d, i), item, d->nItemSize);
	d->nItemCount++;
	return i;
}

WINFN(BOOL) cc_DSA_DeleteItem(HDSA d, INT i)
{
	if (!d || i < 0 || i >= d->nItemCount)
		return FALSE;
	memmove(dsa_item(d, i), dsa_item(d, i + 1),
		(size_t)(d->nItemCount - i - 1) * d->nItemSize);
	d->nItemCount--;
	/* Give back a whole grow step once it's unused. */
	if (d->nMaxCount - d->nItemCount > d->nGrow) {
		INT max = d->nMaxCount - d->nGrow;
		BYTE *p = max ? comctl_realloc(d->pData,
					       (size_t)max * d->nItemSize) : NULL;
		if (p || !max) {
			if (!max)
				comctl_free(d->pData);
			d->pData = p;
			d->nMaxCount = max;
		}
	}
	return TRUE;
}

/* Calls cb on each item until it returns 0. */
WINFN(void) cc_DSA_EnumCallback(HDSA d, PFNDAENUMCALLBACK cb, LPVOID data)
{
	if (!d || !cb)
		return;
	for (INT i = 0; i < d->nItemCount; i++)
		if (!cb(dsa_item(d, i), data))
			break;
}

WINFN(void) cc_DSA_DestroyCallback(HDSA d, PFNDAENUMCALLBACK cb, LPVOID data)
{
	cc_DSA_EnumCallback(d, cb, data);
	cc_DSA_Destroy(d);
}

/* DPA: an array of pointers. */

struct _DPA {
	INT nItemCount;		/* public: DPA_GetPtrCount */
	LPVOID *ptrs;		/* public: DPA_GetPtrPtr */
	HANDLE hHeap;
	INT nGrow;
	INT nMaxCount;
};

static void *dpa_alloc(HDPA d, size_t size)
{
	return heap_alloc(d->hHeap, HEAP_ZERO_MEMORY, size);
}

WINFN(HDPA) cc_DPA_CreateEx(INT grow, HANDLE heap)
{
	if (!heap)
		heap = heap_process();
	HDPA d = heap_alloc(heap, HEAP_ZERO_MEMORY, sizeof(*d));
	if (!d)
		return NULL;
	d->hHeap = heap;
	d->nGrow = grow > 8 ? grow : 8;
	d->nMaxCount = d->nGrow * 2;
	if (!(d->ptrs = dpa_alloc(d, d->nMaxCount * sizeof(LPVOID)))) {
		heap_free(heap, 0, d);
		return NULL;
	}
	return d;
}

WINFN(HDPA) cc_DPA_Create(INT grow)
{
	return cc_DPA_CreateEx(grow, NULL);
}

WINFN(BOOL) cc_DPA_Destroy(HDPA d)
{
	if (!d)
		return FALSE;
	if (d->ptrs)
		heap_free(d->hHeap, 0, d->ptrs);
	return heap_free(d->hHeap, 0, d);
}

WINFN(BOOL) cc_DPA_Grow(HDPA d, INT grow)
{
	if (!d)
		return FALSE;
	d->nGrow = grow > 8 ? grow : 8;
	return TRUE;
}

static BOOL dpa_reserve(HDPA d, INT n)
{
	if (n <= d->nMaxCount && d->ptrs)
		return TRUE;
	INT max = ((n + d->nGrow - 1) / d->nGrow) * d->nGrow;
	if (max < n || (size_t)max > ((size_t)-1 >> 1) / sizeof(LPVOID))
		return FALSE;
	LPVOID *p = d->ptrs ?
		heap_realloc(d->hHeap, HEAP_ZERO_MEMORY, d->ptrs,
			     max * sizeof(LPVOID)) :
		dpa_alloc(d, max * sizeof(LPVOID));
	if (!p)
		return FALSE;
	d->ptrs = p;
	d->nMaxCount = max;
	return TRUE;
}

/* A copy of src in dst (which is emptied first), or in a new array. */
WINFN(HDPA) cc_DPA_Clone(const HDPA src, HDPA dst)
{
	if (!src)
		return NULL;
	HDPA d = dst ? dst : cc_DPA_CreateEx(src->nGrow, src->hHeap);
	if (!d)
		return NULL;
	d->nItemCount = 0;
	if (!dpa_reserve(d, src->nItemCount)) {
		if (!dst)
			cc_DPA_Destroy(d);
		return NULL;
	}
	memset(d->ptrs, 0, d->nMaxCount * sizeof(LPVOID));
	memcpy(d->ptrs, src->ptrs, src->nItemCount * sizeof(LPVOID));
	d->nItemCount = src->nItemCount;
	return d;
}

WINFN(LPVOID) cc_DPA_GetPtr(HDPA d, INT_PTR i)
{
	return d && d->ptrs && i >= 0 && i < d->nItemCount ? d->ptrs[i] : NULL;
}

WINFN(INT) cc_DPA_GetPtrIndex(HDPA d, LPCVOID p)
{
	for (INT i = 0; d && d->ptrs && i < d->nItemCount; i++)
		if (d->ptrs[i] == p)
			return i;
	return -1;
}

WINFN(INT) cc_DPA_InsertPtr(HDPA d, INT i, LPVOID p)
{
	if (!d || i < 0)
		return -1;
	if (i > d->nItemCount)
		i = d->nItemCount;
	if (!dpa_reserve(d, d->nItemCount + 1))
		return -1;
	memmove(d->ptrs + i + 1, d->ptrs + i,
		(d->nItemCount - i) * sizeof(LPVOID));
	d->ptrs[i] = p;
	d->nItemCount++;
	return i;
}

/* Beyond the end, the array grows with NULLs up to i. */
WINFN(BOOL) cc_DPA_SetPtr(HDPA d, INT i, LPVOID p)
{
	if (!d || i < 0)
		return FALSE;
	if (i >= d->nItemCount) {
		if (i == 0x7fffffff || !dpa_reserve(d, i + 1))
			return FALSE;
		d->nItemCount = i + 1;
	}
	d->ptrs[i] = p;
	return TRUE;
}

WINFN(LPVOID) cc_DPA_DeletePtr(HDPA d, INT i)
{
	if (!d || !d->ptrs || i < 0 || i >= d->nItemCount)
		return NULL;
	LPVOID p = d->ptrs[i];
	memmove(d->ptrs + i, d->ptrs + i + 1,
		(d->nItemCount - i - 1) * sizeof(LPVOID));
	d->ptrs[--d->nItemCount] = NULL;
	return p;
}

WINFN(BOOL) cc_DPA_DeleteAllPtrs(HDPA d)
{
	if (!d)
		return FALSE;
	if (d->ptrs)
		memset(d->ptrs, 0, d->nMaxCount * sizeof(LPVOID));
	d->nItemCount = 0;
	return TRUE;
}

/* A stable merge sort of p[0..n), with scratch space tmp. */
static void merge_sort(LPVOID *p, LPVOID *tmp, INT n, PFNDACOMPARE cmp,
		       LPARAM lp)
{
	if (n < 2)
		return;
	INT h = n / 2;
	merge_sort(p, tmp, h, cmp, lp);
	merge_sort(p + h, tmp, n - h, cmp, lp);
	INT i = 0, j = h, k = 0;
	while (i < h && j < n)
		tmp[k++] = cmp(p[i], p[j], lp) <= 0 ? p[i++] : p[j++];
	while (i < h)
		tmp[k++] = p[i++];
	while (j < n)
		tmp[k++] = p[j++];
	memcpy(p, tmp, n * sizeof(LPVOID));
}

WINFN(BOOL) cc_DPA_Sort(HDPA d, PFNDACOMPARE cmp, LPARAM lp)
{
	if (!d || !cmp)
		return FALSE;
	if (d->nItemCount < 2)
		return TRUE;
	LPVOID *tmp = malloc(d->nItemCount * sizeof(LPVOID));
	if (!tmp)
		return FALSE;
	merge_sort(d->ptrs, tmp, d->nItemCount, cmp, lp);
	free(tmp);
	return TRUE;
}

/*
 * The index of an item comparing equal to find, from start on, or -1.
 * Sorted: a binary search, which with DPAS_INSERTBEFORE/AFTER answers
 * where find would go when nothing matches.
 */
WINFN(INT) cc_DPA_Search(HDPA d, LPVOID find, INT start, PFNDACOMPARE cmp,
			 LPARAM lp, UINT options)
{
	if (!d || !cmp || !d->ptrs)
		return -1;
	if (options & DPAS_SORTED) {
		INT l = 0, r = d->nItemCount - 1;
		while (l <= r) {
			INT m = l + (r - l) / 2;
			int c = cmp(find, d->ptrs[m], lp);
			if (!c)
				return m;
			if (c < 0)
				r = m - 1;
			else
				l = m + 1;
		}
		return options & (DPAS_INSERTBEFORE | DPAS_INSERTAFTER) ? l : -1;
	}
	for (INT i = start < 0 ? 0 : start; i < d->nItemCount; i++)
		if (!cmp(find, d->ptrs[i], lp))
			return i;
	return -1;
}

WINFN(void) cc_DPA_EnumCallback(HDPA d, PFNDAENUMCALLBACK cb, LPVOID data)
{
	if (!d || !cb)
		return;
	for (INT i = 0; i < d->nItemCount; i++)
		if (!cb(d->ptrs[i], data))
			break;
}

WINFN(void) cc_DPA_DestroyCallback(HDPA d, PFNDAENUMCALLBACK cb, LPVOID data)
{
	cc_DPA_EnumCallback(d, cb, data);
	cc_DPA_Destroy(d);
}

/*
 * DPA_Merge: both arrays sorted by cmp (unless DPAM_SORTED says they
 * are); items in both are merged into dst by merge(DPAMM_MERGE), items
 * only in src are inserted (DPAMM_INSERT) for DPAM_UNION, and items only
 * in dst are dropped (DPAMM_DELETE) for DPAM_INTERSECT. dst keeps its
 * order.
 */
WINFN(BOOL) cc_DPA_Merge(HDPA dst, HDPA src, DWORD flags, PFNDACOMPARE cmp,
			 PFNDPAMERGE merge, LPARAM lp)
{
	if (!dst || !src || !cmp || !merge)
		return FALSE;
	if (!(flags & DPAM_SORTED)) {
		if (!cc_DPA_Sort(dst, cmp, lp) || !cc_DPA_Sort(src, cmp, lp))
			return FALSE;
	}
	INT nd = dst->nItemCount, ns = src->nItemCount;
	LPVOID *out = malloc(((size_t)nd + ns + 1) * sizeof(LPVOID));
	if (!out)
		return FALSE;
	INT i = 0, j = 0, n = 0;
	BOOL ok = TRUE;
	while (ok && (i < nd || j < ns)) {
		int c = i >= nd ? 1 : j >= ns ? -1 :
			cmp(dst->ptrs[i], src->ptrs[j], lp);
		if (!c) {
			void *m = merge(DPAMM_MERGE, dst->ptrs[i++],
					src->ptrs[j++], lp);
			if (!m)
				ok = FALSE;
			else
				out[n++] = m;
		} else if (c < 0) {
			/* only in dst */
			if (flags & DPAM_INTERSECT)
				merge(DPAMM_DELETE, dst->ptrs[i], NULL, lp);
			else
				out[n++] = dst->ptrs[i];
			i++;
		} else {
			/* only in src */
			if (flags & DPAM_UNION) {
				void *m = merge(DPAMM_INSERT, src->ptrs[j],
						NULL, lp);
				if (!m)
					ok = FALSE;
				else
					out[n++] = m;
			}
			j++;
		}
	}
	/* What wasn't reached (after a failure) stays as it was. */
	while (i < nd)
		out[n++] = dst->ptrs[i++];
	if (!dpa_reserve(dst, n)) {
		free(out);
		return FALSE;
	}
	memcpy(dst->ptrs, out, n * sizeof(LPVOID));
	if (n < dst->nItemCount)
		memset(dst->ptrs + n, 0,
		       (dst->nItemCount - n) * sizeof(LPVOID));
	dst->nItemCount = n;
	free(out);
	return ok;
}

/*
 * The stream form: a header (its whole size, a version of 1, the item
 * count), then whatever the callback writes for each item.
 */
struct dpa_stream_head {
	DWORD size;
	DWORD version;
	DWORD items;
};

static HRESULT stream_pos(IStream *s, ULONGLONG *pos)
{
	LARGE_INTEGER zero = { .QuadPart = 0 };
	ULARGE_INTEGER at;
	HRESULT hr = s->lpVtbl->Seek(s, zero, STREAM_SEEK_CUR, &at);
	*pos = at.QuadPart;
	return hr;
}

static HRESULT stream_seek(IStream *s, ULONGLONG pos)
{
	LARGE_INTEGER to = { .QuadPart = (LONGLONG)pos };
	return s->lpVtbl->Seek(s, to, STREAM_SEEK_SET, NULL);
}

WINFN(HRESULT) cc_DPA_SaveStream(HDPA d, PFNDPASTREAM save, IStream *s,
				 LPVOID data)
{
	struct dpa_stream_head head = { 0, 1, 0 };
	ULONGLONG start, end;
	ULONG done = 0;
	HRESULT hr;

	if (!d || !save || !s)
		return E_INVALIDARG;
	if (FAILED(hr = stream_pos(s, &start)))
		return hr;
	head.items = (DWORD)d->nItemCount;
	hr = s->lpVtbl->Write(s, &head, sizeof(head), &done);
	if (FAILED(hr) || done != sizeof(head))
		return FAILED(hr) ? hr : STG_E_MEDIUMFULL;
	for (INT i = 0; i < d->nItemCount; i++) {
		DPASTREAMINFO info = { i, d->ptrs[i] };
		if ((hr = save(&info, s, data)) != S_OK)
			return FAILED(hr) ? hr : E_FAIL;
	}
	if (FAILED(hr = stream_pos(s, &end)))
		return hr;
	head.size = (DWORD)(end - start);
	if (FAILED(hr = stream_seek(s, start)) ||
	    FAILED(hr = s->lpVtbl->Write(s, &head, sizeof(head), NULL)))
		return hr;
	return stream_seek(s, end);
}

WINFN(HRESULT) cc_DPA_LoadStream(HDPA *out, PFNDPASTREAM load, IStream *s,
				 LPVOID data)
{
	struct dpa_stream_head head;
	ULONG got = 0;
	HRESULT hr;

	if (!out || !load || !s)
		return E_INVALIDARG;
	*out = NULL;
	hr = s->lpVtbl->Read(s, &head, sizeof(head), &got);
	if (FAILED(hr))
		return hr;
	if (got != sizeof(head) || head.size < sizeof(head) ||
	    head.version != 1 || head.items > 0x1000000)
		return E_FAIL;
	HDPA d = cc_DPA_Create(head.items > 8 ? (INT)head.items : 8);
	if (!d)
		return E_OUTOFMEMORY;
	hr = S_OK;
	for (DWORD i = 0; i < head.items; i++) {
		DPASTREAMINFO info = { (int)i, NULL };
		HRESULT r = load(&info, s, data);
		if (r != S_OK) {
			hr = S_FALSE;	/* (what loaded is kept) */
			break;
		}
		if (cc_DPA_InsertPtr(d, (INT)i, info.pvItem) < 0) {
			hr = E_OUTOFMEMORY;
			break;
		}
	}
	if (FAILED(hr)) {
		cc_DPA_Destroy(d);
		return hr;
	}
	*out = d;
	return hr;
}

/* Most of these are imported by ordinal; the names are Windows' too. */
#define E(name, ord) { #name, ord, (void *)cc_##name }

const struct builtin_export comctl_dpa_exports[] = {
	E(Alloc, 71),
	E(ReAlloc, 72),
	E(Free, 73),
	E(GetSize, 74),
	E(DPA_LoadStream, 9),
	E(DPA_SaveStream, 10),
	E(DPA_Merge, 11),
	E(Str_GetPtrA, 233),
	E(Str_SetPtrA, 234),
	E(Str_GetPtrW, 235),
	E(Str_SetPtrW, 236),
	E(DSA_Create, 320),
	E(DSA_Destroy, 321),
	E(DSA_GetItem, 322),
	E(DSA_GetItemPtr, 323),
	E(DSA_InsertItem, 324),
	E(DSA_SetItem, 325),
	E(DSA_DeleteItem, 326),
	E(DSA_DeleteAllItems, 327),
	E(DPA_Create, 328),
	E(DPA_Destroy, 329),
	E(DPA_Grow, 330),
	E(DPA_Clone, 331),
	E(DPA_GetPtr, 332),
	E(DPA_GetPtrIndex, 333),
	E(DPA_InsertPtr, 334),
	E(DPA_SetPtr, 335),
	E(DPA_DeletePtr, 336),
	E(DPA_DeleteAllPtrs, 337),
	E(DPA_Sort, 338),
	E(DPA_Search, 339),
	E(DPA_CreateEx, 340),
	E(DPA_EnumCallback, 385),
	E(DPA_DestroyCallback, 386),
	E(DSA_EnumCallback, 387),
	E(DSA_DestroyCallback, 388),
	BUILTIN_END
};
