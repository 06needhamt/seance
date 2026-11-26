/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * oleaut32's numeric parser API and VARIANT arithmetic, with Visual
 * Basic's rules for the result's type:
 *
 * - VarParseNumFromStr splits a string into digits and a power of ten (or
 *   hex/octal digits) as NUMPARSE describes, in US English whatever the
 *   LCID, as the conversions read strings; VarNumFromParseNum makes the
 *   smallest of the types it is allowed that holds the number.
 * - VarAdd/Sub/Mul/Div/Idiv/Mod/Pow, VarNeg/Abs/Fix/Int/Round, the
 *   logical operators (And, Or, Xor, Eqv, Imp, Not), VarCmp and VarCat.
 *   Null propagates (but Null And False is False, Null Or True is True);
 *   Empty is 0 or "". Integers widen when the result overflows (Byte to
 *   Integer to Long, then Double), Single meets Long as Double, Currency
 *   outranks Single and Decimal everything; dates add as dates. Operands
 *   by reference are read through, and objects through their value
 *   property.
 */
#include <errno.h>
#include <float.h>
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "win32/internal.h"
#include "win32/k32.h"
#include "win32/oleaut.h"

#define US_LCID 0x0409

static int is_blank(WCHAR c)
{
	return c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == 0xa0;
}

/* ---- VarParseNumFromStr ---- */

WINFN(HRESULT) oa_VarParseNumFromStr(const OLECHAR *str, LCID lcid,
				     ULONG flags, NUMPARSE *np, BYTE *dig)
{
	const WCHAR *p = str;
	ULONG in, out = 0;
	int max, n = 0, pwr = 0, neg = 0, any = 0, frac = 0, closed = 0;

	if (!str || !np || !dig)
		return E_INVALIDARG;
	in = np->dwInFlags;
	max = np->cDig;
	np->nBaseShift = 0;

	/* What comes before the number. */
	for (;; p++) {
		if (is_blank(*p) && (in & NUMPRS_LEADING_WHITE))
			out |= NUMPRS_LEADING_WHITE;
		else if (*p == '+' && (in & NUMPRS_LEADING_PLUS) &&
			 !(out & (NUMPRS_LEADING_PLUS | NUMPRS_LEADING_MINUS)))
			out |= NUMPRS_LEADING_PLUS;
		else if (*p == '-' && (in & NUMPRS_LEADING_MINUS) &&
			 !(out & (NUMPRS_LEADING_PLUS | NUMPRS_LEADING_MINUS))) {
			out |= NUMPRS_LEADING_MINUS;
			neg = 1;
		} else if (*p == '(' && (in & NUMPRS_PARENS) &&
			   !(out & NUMPRS_PARENS))
			out |= NUMPRS_PARENS;
		else if (*p == '$' && (in & NUMPRS_CURRENCY) &&
			 !(out & NUMPRS_CURRENCY))
			out |= NUMPRS_CURRENCY;
		else
			break;
	}

	if (*p == '&' && (in & NUMPRS_HEX_OCT) &&
	    (p[1] == 'H' || p[1] == 'h' || p[1] == 'O' || p[1] == 'o')) {
		int base = p[1] == 'H' || p[1] == 'h' ? 16 : 8;
		out |= NUMPRS_HEX_OCT;
		np->nBaseShift = base == 16 ? 4 : 3;
		for (p += 2;; p++) {
			int x = *p >= '0' && *p <= '9' ? *p - '0' :
				*p >= 'a' && *p <= 'f' ? *p - 'a' + 10 :
				*p >= 'A' && *p <= 'F' ? *p - 'A' + 10 : 99;
			if (x >= base)
				break;
			any = 1;
			if (!n && !x)
				continue;
			if (n >= max)
				return DISP_E_OVERFLOW;
			dig[n++] = (BYTE)x;
		}
	} else {
		for (;; p++) {
			if (*p >= '0' && *p <= '9') {
				int x = *p - '0';
				any = 1;
				if (!n && !x) {		/* a leading zero */
					pwr -= frac;
					continue;
				}
				if (n < max) {
					dig[n++] = (BYTE)x;
					pwr -= frac;
				} else {
					pwr += !frac;
					if (x)
						out |= NUMPRS_INEXACT;
				}
			} else if (*p == ',' && (in & NUMPRS_THOUSANDS) && any &&
				   !frac) {
				out |= NUMPRS_THOUSANDS;
			} else if (*p == '.' && (in & NUMPRS_DECIMAL) && !frac) {
				out |= NUMPRS_DECIMAL;
				frac = 1;
			} else {
				break;
			}
		}
		/* An exponent, if digits follow it. */
		if (any && (in & NUMPRS_EXPONENT) &&
		    (*p == 'e' || *p == 'E' || *p == 'd' || *p == 'D')) {
			const WCHAR *q = p + 1;
			int eneg = 0;
			if (*q == '+' || *q == '-')
				eneg = *q++ == '-';
			if (*q >= '0' && *q <= '9') {
				long e = 0;
				for (; *q >= '0' && *q <= '9'; q++)
					if (e < 100000)
						e = e * 10 + (*q - '0');
				pwr += eneg ? -(int)e : (int)e;
				out |= NUMPRS_EXPONENT;
				p = q;
			}
		}
	}
	if (!any)
		return DISP_E_TYPEMISMATCH;

	/* What comes after it. */
	for (;; p++) {
		if (is_blank(*p) && (in & NUMPRS_TRAILING_WHITE))
			out |= NUMPRS_TRAILING_WHITE;
		else if (*p == '+' && (in & NUMPRS_TRAILING_PLUS) &&
			 !(out & (NUMPRS_LEADING_PLUS | NUMPRS_LEADING_MINUS |
				  NUMPRS_TRAILING_PLUS | NUMPRS_TRAILING_MINUS)))
			out |= NUMPRS_TRAILING_PLUS;
		else if (*p == '-' && (in & NUMPRS_TRAILING_MINUS) &&
			 !(out & (NUMPRS_LEADING_PLUS | NUMPRS_LEADING_MINUS |
				  NUMPRS_TRAILING_PLUS | NUMPRS_TRAILING_MINUS))) {
			out |= NUMPRS_TRAILING_MINUS;
			neg = 1;
		} else if (*p == ')' && (out & NUMPRS_PARENS) && !closed) {
			closed = 1;
		} else if (*p == '$' && (in & NUMPRS_CURRENCY) &&
			   !(out & NUMPRS_CURRENCY))
			out |= NUMPRS_CURRENCY;
		else
			break;
	}
	if (out & NUMPRS_PARENS) {
		if (!closed || neg)
			return DISP_E_TYPEMISMATCH;
		neg = 1;
	}
	if ((in & NUMPRS_USE_ALL) && *p)
		return DISP_E_TYPEMISMATCH;

	if (!n) {			/* zero */
		if (max < 1)
			return DISP_E_OVERFLOW;
		dig[n++] = 0;
		pwr = 0;
	} else if (!np->nBaseShift) {
		while (n > 1 && !dig[n - 1]) {
			n--;
			pwr++;
		}
	}
	if (neg)
		out |= NUMPRS_NEG;
	np->cDig = n;
	np->dwOutFlags = out;
	np->cchUsed = (INT)(p - str);
	np->nPwr10 = np->nBaseShift ? 0 : pwr;
	return S_OK;
}

/* ---- VarNumFromParseNum ---- */

static const VARTYPE int_order[] = {
	VT_I1, VT_UI1, VT_I2, VT_UI2, VT_I4, VT_UI4, VT_I8, VT_UI8,
};

static int width_of(VARTYPE vt)
{
	switch (vt) {
	case VT_I1: case VT_UI1: return 8;
	case VT_I2: case VT_UI2: return 16;
	case VT_I4: case VT_UI4: return 32;
	}
	return 64;
}

static void set_bits(VARIANT *v, VARTYPE vt, uint64_t x)
{
	v->vt = vt;
	switch (vt) {
	case VT_I1:  v->cVal = (CHAR)x; break;
	case VT_UI1: v->bVal = (BYTE)x; break;
	case VT_I2:  v->iVal = (SHORT)x; break;
	case VT_UI2: v->uiVal = (USHORT)x; break;
	case VT_I4:  v->lVal = (LONG)x; break;
	case VT_UI4: v->ulVal = (ULONG)x; break;
	case VT_I8:  v->llVal = (LONGLONG)x; break;
	default:     v->ullVal = x; break;
	}
}

/* The number as text conversions read: no exponent if it's short. */
static BSTR number_text(const NUMPARSE *np, const BYTE *dig)
{
	int n = np->cDig, pwr = np->nPwr10;
	size_t cap = (size_t)n + 64 + (pwr > 0 && pwr < 40 ? pwr : 0) +
		     (pwr < 0 && pwr > -40 ? -pwr : 0);
	char *s = malloc(cap), *o = s;
	BSTR b;

	if (!s)
		return NULL;
	if (np->dwOutFlags & NUMPRS_NEG)
		*o++ = '-';
	if (pwr >= 0 && pwr < 40) {
		for (int i = 0; i < n; i++)
			*o++ = (char)('0' + dig[i]);
		for (int i = 0; i < pwr; i++)
			*o++ = '0';
	} else if (pwr < 0 && pwr > -40) {
		int point = n + pwr;	/* digits before the point */
		if (point <= 0) {
			*o++ = '0';
			*o++ = '.';
			for (int i = point; i < 0; i++)
				*o++ = '0';
			point = 0;
		}
		for (int i = 0; i < n; i++) {
			if (i == point && point)
				*o++ = '.';
			*o++ = (char)('0' + dig[i]);
		}
	} else {
		for (int i = 0; i < n; i++)
			*o++ = (char)('0' + dig[i]);
		o += sprintf(o, "e%d", pwr);
	}
	*o = '\0';
	b = oa_bstr_utf8(s);
	free(s);
	return b;
}

static HRESULT text_to(VARIANT *out, BSTR text, VARTYPE vt)
{
	VARIANT src, dst;

	src.vt = VT_BSTR;
	src.bstrVal = text;
	oa_VariantInit(&dst);
	HRESULT hr = oa_VariantChangeTypeEx(&dst, &src, US_LCID, 0, vt);
	if (hr == S_OK)
		*out = dst;
	return hr;
}

WINFN(HRESULT) oa_VarNumFromParseNum(NUMPARSE *np, BYTE *dig, ULONG bits,
				     VARIANT *out)
{
	static const VARTYPE reals[] = { VT_R4, VT_R8, VT_CY, VT_DECIMAL };
	HRESULT hr = DISP_E_OVERFLOW;

	if (!np || !dig || !out || np->cDig < 0)
		return E_INVALIDARG;

	if (np->nBaseShift != 0 && np->nBaseShift != 3 && np->nBaseShift != 4)
		return E_INVALIDARG;	/* decimal, octal or hex only */
	if (np->nBaseShift) {
		/* Hex and octal fill the type: &HFFFF is -1 as an I2. */
		uint64_t v = 0;
		int used = 0;
		for (int i = 0; i < np->cDig; i++) {
			if (dig[i] >> np->nBaseShift)
				return E_INVALIDARG;
			if (v >> (64 - np->nBaseShift))
				return DISP_E_OVERFLOW;
			v = v << np->nBaseShift | dig[i];
		}
		while (used < 64 && v >> used)
			used++;
		if (np->dwOutFlags & NUMPRS_NEG)
			v = -v;
		for (size_t i = 0; i < sizeof(int_order) / sizeof(*int_order);
		     i++) {
			VARTYPE vt = int_order[i];
			if ((bits & (1u << vt)) && used <= width_of(vt) &&
			    !(np->dwOutFlags & NUMPRS_NEG)) {
				set_bits(out, vt, v);
				return S_OK;
			}
		}
		if (!(np->dwOutFlags & NUMPRS_NEG) && (bits & VTBIT_R8)) {
			out->vt = VT_R8;
			out->dblVal = (double)v;
			return S_OK;
		}
		if (!(np->dwOutFlags & NUMPRS_NEG)) {
			/* Then the other reals, from its decimal digits. */
			char t[32];
			snprintf(t, sizeof(t), "%llu", (unsigned long long)v);
			BSTR b = oa_bstr_utf8(t);
			if (!b)
				return E_OUTOFMEMORY;
			for (size_t i = 0; i < 4 && hr != S_OK &&
					   hr != E_OUTOFMEMORY; i++)
				if (bits & (1u << reals[i]))
					hr = text_to(out, b, reals[i]);
			oa_SysFreeString(b);
			return hr == S_OK || hr == E_OUTOFMEMORY ?
				hr : DISP_E_OVERFLOW;
		}
		/* A negative one reads as a decimal number. */
		char t[32];
		snprintf(t, sizeof(t), "%lld", (long long)(int64_t)v);
		BSTR b = oa_bstr_utf8(t);
		if (!b)
			return E_OUTOFMEMORY;
		for (size_t i = 0; i < sizeof(int_order) / sizeof(*int_order) &&
				   hr != S_OK && hr != E_OUTOFMEMORY; i++)
			if (bits & (1u << int_order[i]))
				hr = text_to(out, b, int_order[i]);
		for (size_t i = 0; i < 4 && hr != S_OK && hr != E_OUTOFMEMORY;
		     i++)
			if (bits & (1u << reals[i]))
				hr = text_to(out, b, reals[i]);
		oa_SysFreeString(b);
		return hr == S_OK || hr == E_OUTOFMEMORY ? hr : DISP_E_OVERFLOW;
	}

	BSTR text = number_text(np, dig);
	if (!text)
		return E_OUTOFMEMORY;
	int n = np->cDig, pwr = np->nPwr10;

	/* The smallest integer that holds it. */
	if (pwr >= 0)
		for (size_t i = 0; i < sizeof(int_order) / sizeof(*int_order) &&
				   hr != S_OK && hr != E_OUTOFMEMORY; i++)
			if (bits & (1u << int_order[i]))
				hr = text_to(out, text, int_order[i]);
	/* Then a real that holds it exactly, then one that holds it. */
	for (int pass = 0; pass < 2 && hr != S_OK && hr != E_OUTOFMEMORY;
	     pass++)
		for (int i = 0; i < 4 && hr != S_OK && hr != E_OUTOFMEMORY;
		     i++) {
			VARTYPE vt = reals[i];
			int exact = vt == VT_R4 ? n <= 7 :
				    vt == VT_R8 ? n <= 15 :
				    vt == VT_CY ? pwr >= -4 :
				    n <= 29 && pwr >= -28;
			if ((bits & (1u << vt)) && (exact || pass))
				hr = text_to(out, text, vt);
		}
	/* With only integers allowed, a fraction is rounded into one. */
	if (pwr < 0)
		for (size_t i = 0; i < sizeof(int_order) / sizeof(*int_order) &&
				   hr != S_OK && hr != E_OUTOFMEMORY; i++)
			if (bits & (1u << int_order[i]))
				hr = text_to(out, text, int_order[i]);
	oa_SysFreeString(text);
	return hr == S_OK || hr == E_OUTOFMEMORY ? hr : DISP_E_OVERFLOW;
}

/* ---- operands ---- */

/* The operand by value: references read through, objects' values read. */
static HRESULT operand(const VARIANT *src, VARIANT *out)
{
	HRESULT hr;

	oa_VariantInit(out);
	if (!src)
		return E_INVALIDARG;
	if ((hr = oa_VariantCopyInd(out, src)) != S_OK)
		return hr;
	if (out->vt == VT_DISPATCH || out->vt == VT_UNKNOWN) {
		IDispatch *d = out->vt == VT_DISPATCH ? out->pdispVal : NULL;
		DISPPARAMS none = { NULL, NULL, 0, 0 };
		VARIANT r;
		oa_VariantInit(&r);
		hr = d ? d->lpVtbl->Invoke(d, DISPID_VALUE, &oa_IID_NULL,
					   US_LCID, DISPATCH_PROPERTYGET, &none,
					   &r, NULL, NULL) :
			 DISP_E_TYPEMISMATCH;
		oa_VariantClear(out);
		if (FAILED(hr))
			return hr;
		hr = oa_VariantCopyInd(out, &r);
		oa_VariantClear(&r);
		if (hr != S_OK)
			return hr;
		if (out->vt == VT_DISPATCH || out->vt == VT_UNKNOWN) {
			oa_VariantClear(out);
			return DISP_E_TYPEMISMATCH;
		}
	}
	if (out->vt & (VT_ARRAY | VT_VECTOR)) {
		oa_VariantClear(out);
		return DISP_E_TYPEMISMATCH;
	}
	return S_OK;
}

static int is_int_vt(VARTYPE vt)
{
	switch (vt) {
	case VT_EMPTY: case VT_BOOL: case VT_I1: case VT_UI1: case VT_I2:
	case VT_UI2: case VT_I4: case VT_UI4: case VT_INT: case VT_UINT:
	case VT_I8: case VT_UI8:
		return 1;
	}
	return 0;
}

static int is_num_vt(VARTYPE vt)
{
	return is_int_vt(vt) || vt == VT_R4 || vt == VT_R8 ||
	       vt == VT_DATE || vt == VT_CY || vt == VT_DECIMAL ||
	       vt == VT_BSTR;
}

/* Byte 1, Integer 2, Long 3, LongLong 4, for the integer types. */
static int int_rank(VARTYPE vt)
{
	switch (vt) {
	case VT_UI1:
		return 1;
	case VT_EMPTY: case VT_BOOL: case VT_I1: case VT_I2:
		return 2;
	case VT_UI2: case VT_I4: case VT_INT:
		return 3;
	}
	return 4;
}

static const VARTYPE rank_vt[] = { VT_EMPTY, VT_UI1, VT_I2, VT_I4, VT_I8 };

static HRESULT change(VARIANT *dst, const VARIANT *src, VARTYPE vt)
{
	oa_VariantInit(dst);
	return oa_VariantChangeTypeEx(dst, (VARIANTARG *)src, US_LCID, 0, vt);
}

static HRESULT as_r8(const VARIANT *v, double *out)
{
	VARIANT t;
	HRESULT hr = change(&t, v, VT_R8);
	if (hr == S_OK)
		*out = t.dblVal;
	return hr;
}

static HRESULT as_i8(const VARIANT *v, int64_t *out)
{
	VARIANT t;
	HRESULT hr = change(&t, v, VT_I8);
	if (hr == S_OK)
		*out = t.llVal;
	return hr;
}

static HRESULT as_dec(const VARIANT *v, DECIMAL *out)
{
	VARIANT t;
	HRESULT hr = change(&t, v, VT_DECIMAL);
	if (hr == S_OK)
		*out = t.decVal;
	return hr;
}

static void set_dec(VARIANT *out, const DECIMAL *d)
{
	out->decVal = *d;
	out->vt = VT_DECIMAL;
}

static int range_ok(int rank, int64_t v)
{
	switch (rank) {
	case 1: return v >= 0 && v <= 255;
	case 2: return v >= INT16_MIN && v <= INT16_MAX;
	case 3: return v >= INT32_MIN && v <= INT32_MAX;
	}
	return 1;
}

/*
 * An integer result of at least the given rank, widened until it fits;
 * a Long that doesn't fit becomes a Double, as in Visual Basic.
 */
static HRESULT set_int(VARIANT *out, int rank, int64_t v, int overflow,
		       double approx)
{
	while (!overflow && rank < 4 && !range_ok(rank, v)) {
		if (rank == 3)
			overflow = 1;
		else
			rank++;
	}
	if (overflow) {
		if (!isfinite(approx))
			return DISP_E_OVERFLOW;
		out->vt = VT_R8;
		out->dblVal = approx;
		return S_OK;
	}
	set_bits(out, rank_vt[rank], (uint64_t)v);
	return S_OK;
}

static HRESULT set_r8(VARIANT *out, double d)
{
	if (isnan(d))
		return DISP_E_TYPEMISMATCH;
	if (!isfinite(d))
		return DISP_E_OVERFLOW;
	out->vt = VT_R8;
	out->dblVal = d;
	return S_OK;
}

static HRESULT set_r4(VARIANT *out, double d)
{
	if (isnan(d))
		return DISP_E_TYPEMISMATCH;
	if (fabs(d) > FLT_MAX)
		return set_r8(out, d);		/* Single widens to Double */
	out->vt = VT_R4;
	out->fltVal = (float)d;
	return S_OK;
}

static HRESULT set_date(VARIANT *out, double d)
{
	if (!(d >= -657434.0 && d < 2958466.0))
		return DISP_E_OVERFLOW;
	out->vt = VT_DATE;
	out->date = d;
	return S_OK;
}

/* ---- concatenation ---- */

static HRESULT text_of(const VARIANT *v, BSTR *out)
{
	VARIANT t;
	HRESULT hr;

	if (v->vt == VT_NULL || v->vt == VT_EMPTY) {
		*out = NULL;
		return S_OK;
	}
	if (v->vt == VT_BSTR) {
		*out = v->bstrVal;
		return S_OK;
	}
	oa_VariantInit(&t);
	hr = oa_VariantChangeTypeEx(&t, (VARIANTARG *)v, US_LCID,
				    v->vt == VT_BOOL ? VARIANT_ALPHABOOL : 0,
				    VT_BSTR);
	if (hr != S_OK)
		return hr;
	*out = t.bstrVal;
	return S_OK;
}

static HRESULT cat(const VARIANT *a, const VARIANT *b, VARIANT *out)
{
	BSTR x = NULL, y = NULL, r;
	HRESULT hr;

	if (a->vt == VT_NULL && b->vt == VT_NULL) {
		out->vt = VT_NULL;
		return S_OK;
	}
	if (!is_num_vt(a->vt) && a->vt != VT_NULL && a->vt != VT_ERROR)
		return DISP_E_TYPEMISMATCH;
	if (!is_num_vt(b->vt) && b->vt != VT_NULL && b->vt != VT_ERROR)
		return DISP_E_TYPEMISMATCH;
	if ((hr = text_of(a, &x)) != S_OK)
		return hr;
	if ((hr = text_of(b, &y)) != S_OK) {
		if (x != a->bstrVal || a->vt != VT_BSTR)
			oa_SysFreeString(x);
		return hr;
	}
	UINT nx = oa_SysStringLen(x), ny = oa_SysStringLen(y);
	r = oa_SysAllocStringLen(NULL, nx + ny);
	if (r) {
		if (nx)
			memcpy(r, x, nx * sizeof(WCHAR));
		if (ny)
			memcpy(r + nx, y, ny * sizeof(WCHAR));
	}
	if (a->vt != VT_BSTR)
		oa_SysFreeString(x);
	if (b->vt != VT_BSTR)
		oa_SysFreeString(y);
	if (!r)
		return E_OUTOFMEMORY;
	out->vt = VT_BSTR;
	out->bstrVal = r;
	return S_OK;
}

/* ---- the binary arithmetic operators ---- */

enum op { OP_ADD, OP_SUB, OP_MUL, OP_DIV, OP_IDIV, OP_MOD, OP_POW };

static int is_zero_r8(const VARIANT *v)
{
	double d;
	return as_r8(v, &d) == S_OK && d == 0.0;
}

static HRESULT arith(enum op op, const VARIANT *a, const VARIANT *b,
		     VARIANT *out)
{
	VARTYPE ta = a->vt, tb = b->vt;
	HRESULT hr;
	double x, y;

	if (!is_num_vt(ta) && ta != VT_NULL)
		return DISP_E_TYPEMISMATCH;
	if (!is_num_vt(tb) && tb != VT_NULL)
		return DISP_E_TYPEMISMATCH;
	if (ta == VT_NULL || tb == VT_NULL) {
		out->vt = VT_NULL;
		return S_OK;
	}
	if (op == OP_ADD && ((ta == VT_BSTR && (tb == VT_BSTR ||
						tb == VT_EMPTY)) ||
			     (tb == VT_BSTR && ta == VT_EMPTY)))
		return cat(a, b, out);

	if (op == OP_POW) {
		if ((hr = as_r8(a, &x)) != S_OK || (hr = as_r8(b, &y)) != S_OK)
			return hr;
		double r = pow(x, y);
		return isnan(r) ? E_INVALIDARG : set_r8(out, r);
	}

	if (op == OP_IDIV || op == OP_MOD) {
		/* Both rounded to integers; the result is at least a Long
		 * for anything that isn't an integer already. */
		int ra = is_int_vt(ta) ? int_rank(ta) : 3;
		int rb = is_int_vt(tb) ? int_rank(tb) : 3;
		int rank = ra > rb ? ra : rb;
		int64_t i, j;
		if ((hr = as_i8(a, &i)) != S_OK || (hr = as_i8(b, &j)) != S_OK)
			return hr;
		if ((!is_int_vt(ta) && !range_ok(3, i)) ||
		    (!is_int_vt(tb) && !range_ok(3, j)))
			return DISP_E_OVERFLOW;
		if (!j)
			return DISP_E_DIVBYZERO;
		if (i == INT64_MIN && j == -1) {
			if (op == OP_MOD)
				return set_int(out, rank, 0, 0, 0);
			return DISP_E_OVERFLOW;
		}
		int64_t r = op == OP_IDIV ? i / j : i % j;
		if (rank == 1 && !range_ok(1, r))
			rank = 2;
		while (rank < 4 && !range_ok(rank, r))
			rank++;
		set_bits(out, rank_vt[rank], (uint64_t)r);
		return S_OK;
	}

	if (op == OP_DIV) {
		if (is_zero_r8(b))
			return is_zero_r8(a) ? DISP_E_OVERFLOW :
			       DISP_E_DIVBYZERO;
		if (ta == VT_DECIMAL || tb == VT_DECIMAL) {
			DECIMAL p, q, r;
			if ((hr = as_dec(a, &p)) != S_OK ||
			    (hr = as_dec(b, &q)) != S_OK ||
			    (hr = oa_VarDecDiv(&p, &q, &r)) != S_OK)
				return hr;
			set_dec(out, &r);
			return S_OK;
		}
		if ((hr = as_r8(a, &x)) != S_OK || (hr = as_r8(b, &y)) != S_OK)
			return hr;
		int small_a = ta == VT_R4 || (is_int_vt(ta) && int_rank(ta) <= 2);
		int small_b = tb == VT_R4 || (is_int_vt(tb) && int_rank(tb) <= 2);
		if (small_a && small_b && (ta == VT_R4 || tb == VT_R4 ||
					   (ta != VT_EMPTY && tb != VT_EMPTY)))
			return set_r4(out, x / y);
		return set_r8(out, x / y);
	}

	/* Add, Sub, Mul. */
	if (ta == VT_EMPTY && tb == VT_EMPTY) {
		out->vt = VT_I2;
		out->iVal = 0;
		return S_OK;
	}
	if ((ta == VT_DATE || tb == VT_DATE) && op != OP_MUL &&
	    ta != VT_DECIMAL && tb != VT_DECIMAL) {
		if ((hr = as_r8(a, &x)) != S_OK || (hr = as_r8(b, &y)) != S_OK)
			return hr;
		if (op == OP_ADD)
			return set_date(out, x + y);
		if (ta == VT_DATE && tb == VT_DATE)
			return set_r8(out, x - y);	/* days between */
		return set_date(out, x - y);
	}
	if (ta == VT_DECIMAL || tb == VT_DECIMAL ||
	    ((ta == VT_CY || tb == VT_CY) && ta != VT_R8 && tb != VT_R8 &&
	     ta != VT_BSTR && tb != VT_BSTR && ta != VT_DATE && tb != VT_DATE)) {
		DECIMAL p, q, r;
		int to_cy = ta != VT_DECIMAL && tb != VT_DECIMAL;
		if ((hr = as_dec(a, &p)) != S_OK || (hr = as_dec(b, &q)) != S_OK)
			return hr;
		hr = op == OP_ADD ? oa_VarDecAdd(&p, &q, &r) :
		     op == OP_SUB ? oa_VarDecSub(&p, &q, &r) :
				    oa_VarDecMul(&p, &q, &r);
		if (hr != S_OK)
			return hr;
		if (!to_cy) {
			set_dec(out, &r);
			return S_OK;
		}
		VARIANT d, c;
		set_dec(&d, &r);
		if ((hr = change(&c, &d, VT_CY)) != S_OK)
			return hr;
		*out = c;
		return S_OK;
	}
	if ((hr = as_r8(a, &x)) != S_OK || (hr = as_r8(b, &y)) != S_OK)
		return hr;
	double approx = op == OP_ADD ? x + y : op == OP_SUB ? x - y : x * y;
	if (ta == VT_R8 || tb == VT_R8 || ta == VT_BSTR || tb == VT_BSTR ||
	    ta == VT_DATE || tb == VT_DATE)
		return set_r8(out, approx);
	if (ta == VT_R4 || tb == VT_R4) {
		VARTYPE other = ta == VT_R4 ? tb : ta;
		if (other != VT_R4 && is_int_vt(other) && int_rank(other) >= 3)
			return set_r8(out, approx);	/* Single and Long */
		return set_r4(out, approx);
	}

	/* Two integers. */
	int64_t i, j, r = 0;
	int over = 0;
	int ra = int_rank(ta), rb = int_rank(tb);
	if (ta == VT_EMPTY)
		ra = rb;
	if (tb == VT_EMPTY)
		rb = ra;
	if ((hr = as_i8(a, &i)) != S_OK || (hr = as_i8(b, &j)) != S_OK)
		return hr;
	over = op == OP_ADD ? __builtin_add_overflow(i, j, &r) :
	       op == OP_SUB ? __builtin_sub_overflow(i, j, &r) :
			      __builtin_mul_overflow(i, j, &r);
	return set_int(out, ra > rb ? ra : rb, r, over, approx);
}

static HRESULT binary(enum op op, LPVARIANT a, LPVARIANT b, LPVARIANT out)
{
	VARIANT x, y, r;
	HRESULT hr;

	if (!out)
		return E_INVALIDARG;
	if ((hr = operand(a, &x)) != S_OK)
		return hr;
	if ((hr = operand(b, &y)) != S_OK) {
		oa_VariantClear(&x);
		return hr;
	}
	oa_VariantInit(&r);
	hr = arith(op, &x, &y, &r);
	oa_VariantClear(&x);
	oa_VariantClear(&y);
	if (hr == S_OK)
		*out = r;
	return hr;
}

WINFN(HRESULT) oa_VarAdd(LPVARIANT a, LPVARIANT b, LPVARIANT out)
{
	return binary(OP_ADD, a, b, out);
}

WINFN(HRESULT) oa_VarSub(LPVARIANT a, LPVARIANT b, LPVARIANT out)
{
	return binary(OP_SUB, a, b, out);
}

WINFN(HRESULT) oa_VarMul(LPVARIANT a, LPVARIANT b, LPVARIANT out)
{
	return binary(OP_MUL, a, b, out);
}

WINFN(HRESULT) oa_VarDiv(LPVARIANT a, LPVARIANT b, LPVARIANT out)
{
	return binary(OP_DIV, a, b, out);
}

WINFN(HRESULT) oa_VarIdiv(LPVARIANT a, LPVARIANT b, LPVARIANT out)
{
	return binary(OP_IDIV, a, b, out);
}

WINFN(HRESULT) oa_VarMod(LPVARIANT a, LPVARIANT b, LPVARIANT out)
{
	return binary(OP_MOD, a, b, out);
}

WINFN(HRESULT) oa_VarPow(LPVARIANT a, LPVARIANT b, LPVARIANT out)
{
	return binary(OP_POW, a, b, out);
}

WINFN(HRESULT) oa_VarCat(LPVARIANT a, LPVARIANT b, LPVARIANT out)
{
	VARIANT x, y, r;
	HRESULT hr;

	if (!out)
		return E_INVALIDARG;
	if ((hr = operand(a, &x)) != S_OK)
		return hr;
	if ((hr = operand(b, &y)) != S_OK) {
		oa_VariantClear(&x);
		return hr;
	}
	oa_VariantInit(&r);
	hr = cat(&x, &y, &r);
	oa_VariantClear(&x);
	oa_VariantClear(&y);
	if (hr == S_OK)
		*out = r;
	return hr;
}

/* ---- the unary ones ---- */

enum un { UN_NEG, UN_ABS, UN_FIX, UN_INT, UN_ROUND };

static double round_places(double d, int places)
{
	if (places > 15 || !isfinite(d))
		return d;
	double scale = pow(10.0, places);
	double r = nearbyint(d * scale) / scale;	/* to even */
	return isfinite(r) ? r : d;
}

/* v (a CY's scaled value) cut to places decimals; DISP_E_OVERFLOW if
 * the result leaves the CY range. */
static HRESULT cy_round_to(int64_t *v, int places, int how)
{
	if (places >= 4)	/* nothing to take off */
		return S_OK;
	int64_t unit = 1;
	for (int i = places; i < 4; i++)
		unit *= 10;
	int64_t q = *v / unit, rem = *v % unit;
	if (how == UN_INT) {
		if (rem < 0)
			q--;
	} else if (how == UN_ROUND) {
		int64_t half = unit / 2;
		if (rem > half || (rem == half && (q & 1)))
			q++;
		else if (rem < -half || (rem == -half && (q & 1)))
			q--;
	}
	if (q > INT64_MAX / unit || q < INT64_MIN / unit)
		return DISP_E_OVERFLOW;
	*v = q * unit;
	return S_OK;
}

static HRESULT unary(enum un how, int places, const VARIANT *v, VARIANT *out)
{
	HRESULT hr;
	double d;
	int64_t i;

	switch (v->vt) {
	case VT_NULL:
		out->vt = VT_NULL;
		return S_OK;
	case VT_EMPTY:
		out->vt = VT_I2;
		out->iVal = 0;
		return S_OK;
	case VT_R4:
	case VT_R8:
	case VT_DATE:
	case VT_BSTR:
		if ((hr = as_r8(v, &d)) != S_OK)
			return hr;
		d = how == UN_NEG ? -d : how == UN_ABS ? fabs(d) :
		    how == UN_FIX ? trunc(d) : how == UN_INT ? floor(d) :
		    round_places(d, places);
		if (v->vt == VT_R4) {
			out->vt = VT_R4;
			out->fltVal = (float)d;
			return S_OK;
		}
		if (v->vt == VT_DATE && how != UN_ABS)
			return set_date(out, d);
		return set_r8(out, d);
	case VT_CY:
		i = v->cyVal.int64;
		if (how == UN_NEG || how == UN_ABS) {
			if (how == UN_ABS && i >= 0) {
				*out = *v;
				return S_OK;
			}
			if (i == INT64_MIN)
				return DISP_E_OVERFLOW;
			i = -i;
		} else {
			hr = cy_round_to(&i, how == UN_ROUND ? places : 0, how);
			if (hr != S_OK)
				return hr;
		}
		out->vt = VT_CY;
		out->cyVal.int64 = i;
		return S_OK;
	case VT_DECIMAL: {
		DECIMAL r;
		hr = how == UN_NEG ? oa_VarDecNeg(&v->decVal, &r) :
		     how == UN_ABS ? oa_VarDecAbs(&v->decVal, &r) :
		     how == UN_FIX ? oa_VarDecFix(&v->decVal, &r) :
		     how == UN_INT ? oa_VarDecInt(&v->decVal, &r) :
				     oa_VarDecRound(&v->decVal, places, &r);
		if (hr == S_OK)
			set_dec(out, &r);
		return hr;
	}
	default:
		break;
	}
	if (!is_int_vt(v->vt))
		return DISP_E_TYPEMISMATCH;
	if (v->vt == VT_UI8 && v->ullVal > INT64_MAX) {
		if (how != UN_NEG) {
			*out = *v;
			return S_OK;
		}
		DECIMAL p, r;
		if ((hr = as_dec(v, &p)) != S_OK ||
		    (hr = oa_VarDecNeg(&p, &r)) != S_OK)
			return hr;
		set_dec(out, &r);
		return S_OK;
	}
	if ((hr = as_i8(v, &i)) != S_OK)
		return hr;
	if (how != UN_NEG && how != UN_ABS) {
		if (v->vt == VT_BOOL) {
			out->vt = VT_I2;
			out->iVal = v->boolVal;
		} else {
			*out = *v;
		}
		return S_OK;
	}
	/* Negating: unsigned and Boolean values turn signed, and the most
	 * negative value of a type widens. */
	VARTYPE vt = v->vt;
	switch (vt) {
	case VT_BOOL: case VT_UI1: vt = VT_I2; break;
	case VT_UI2: case VT_INT: vt = VT_I4; break;
	case VT_UI4: case VT_UINT: case VT_UI8: vt = VT_I8; break;
	}
	if (how == UN_ABS && i >= 0 && vt == v->vt) {
		*out = *v;
		return S_OK;
	}
	if (how == UN_ABS && i >= 0)
		;
	else if (i == INT64_MIN)
		return set_r8(out, 9223372036854775808.0);
	else
		i = -i;
	int rank = vt == VT_I1 ? 0 : int_rank(vt);
	if (rank == 0) {
		if (i >= INT8_MIN && i <= INT8_MAX) {
			set_bits(out, VT_I1, (uint64_t)i);
			return S_OK;
		}
		rank = 2;
	}
	return set_int(out, rank, i, 0, (double)i);
}

static HRESULT unary_entry(enum un how, int places, LPVARIANT in,
			   LPVARIANT out)
{
	VARIANT x, r;
	HRESULT hr;

	if (!out)
		return E_INVALIDARG;
	if ((hr = operand(in, &x)) != S_OK)
		return hr;
	oa_VariantInit(&r);
	hr = unary(how, places, &x, &r);
	oa_VariantClear(&x);
	if (hr == S_OK)
		*out = r;
	return hr;
}

WINFN(HRESULT) oa_VarNeg(LPVARIANT in, LPVARIANT out)
{
	return unary_entry(UN_NEG, 0, in, out);
}

WINFN(HRESULT) oa_VarAbs(LPVARIANT in, LPVARIANT out)
{
	return unary_entry(UN_ABS, 0, in, out);
}

WINFN(HRESULT) oa_VarFix(LPVARIANT in, LPVARIANT out)
{
	return unary_entry(UN_FIX, 0, in, out);
}

WINFN(HRESULT) oa_VarInt(LPVARIANT in, LPVARIANT out)
{
	return unary_entry(UN_INT, 0, in, out);
}

WINFN(HRESULT) oa_VarRound(LPVARIANT in, int places, LPVARIANT out)
{
	if (places < 0)
		return E_INVALIDARG;
	return unary_entry(UN_ROUND, places, in, out);
}

/* ---- the logical operators ---- */

enum logic { L_AND, L_OR, L_XOR, L_EQV, L_IMP };

/*
 * The integer a logical operand stands for, and its rank (0 for a
 * Boolean): reals, currency and decimals are Longs, strings Longs or,
 * failing that, Booleans ("True").
 */
static HRESULT logic_operand(const VARIANT *v, int64_t *val, int *rank)
{
	HRESULT hr;

	if (v->vt == VT_BOOL) {
		*val = v->boolVal ? -1 : 0;
		*rank = 0;
		return S_OK;
	}
	if (is_int_vt(v->vt)) {
		*rank = int_rank(v->vt);
		if (v->vt == VT_UI8 && v->ullVal > INT64_MAX)
			return DISP_E_OVERFLOW;
		return as_i8(v, val);
	}
	if (!is_num_vt(v->vt))
		return DISP_E_TYPEMISMATCH;
	VARIANT t;
	hr = change(&t, v, VT_I4);
	if (hr == S_OK) {
		*val = t.lVal;
		*rank = 3;
		return S_OK;
	}
	if (v->vt == VT_BSTR && hr == DISP_E_TYPEMISMATCH &&
	    change(&t, v, VT_BOOL) == S_OK) {
		*val = t.boolVal ? -1 : 0;
		*rank = 0;
		return S_OK;
	}
	return hr;
}

static void set_logic(VARIANT *out, int rank, int64_t v)
{
	if (rank == 0) {
		out->vt = VT_BOOL;
		out->boolVal = v ? VARIANT_TRUE : VARIANT_FALSE;
		return;
	}
	set_bits(out, rank_vt[rank], (uint64_t)v);
}

static HRESULT logic(enum logic op, const VARIANT *a, const VARIANT *b,
		     VARIANT *out)
{
	int64_t x = 0, y = 0, r;
	int ra = 0, rb = 0;
	HRESULT hr;
	int na = a->vt == VT_NULL, nb = b->vt == VT_NULL;

	if (!na && (hr = logic_operand(a, &x, &ra)) != S_OK)
		return hr;
	if (!nb && (hr = logic_operand(b, &y, &rb)) != S_OK)
		return hr;
	if (a->vt == VT_EMPTY && b->vt == VT_EMPTY)
		ra = rb = 2;
	if (na || nb) {
		/* What is known whatever Null stands for. */
		const VARIANT *o = na ? b : a;
		int64_t ov = na ? y : x;
		int orank = na ? rb : ra;
		out->vt = VT_NULL;
		if (na && nb)
			return S_OK;
		if (o->vt == VT_EMPTY)
			orank = 2;
		if (op == L_AND && !ov)
			set_logic(out, orank, 0);
		else if (op == L_OR && orank == 0 && ov)
			set_logic(out, 0, -1);
		else if (op == L_IMP && ((na && orank == 0 && ov) ||
					 (nb && !ov)))
			set_logic(out, orank, -1);
		return S_OK;
	}
	switch (op) {
	case L_AND: r = x & y; break;
	case L_OR:  r = x | y; break;
	case L_XOR: r = x ^ y; break;
	case L_EQV: r = ~(x ^ y); break;
	default:    r = ~x | y; break;
	}
	int rank = ra == 0 && rb == 0 ? 0 :
		   (ra ? ra : 2) > (rb ? rb : 2) ? (ra ? ra : 2) : (rb ? rb : 2);
	if (rank == 1)
		r &= 0xff;
	set_logic(out, rank, r);
	return S_OK;
}

static HRESULT logic_entry(enum logic op, LPVARIANT a, LPVARIANT b,
			   LPVARIANT out)
{
	VARIANT x, y, r;
	HRESULT hr;

	if (!out)
		return E_INVALIDARG;
	if ((hr = operand(a, &x)) != S_OK)
		return hr;
	if ((hr = operand(b, &y)) != S_OK) {
		oa_VariantClear(&x);
		return hr;
	}
	oa_VariantInit(&r);
	hr = logic(op, &x, &y, &r);
	oa_VariantClear(&x);
	oa_VariantClear(&y);
	if (hr == S_OK)
		*out = r;
	return hr;
}

WINFN(HRESULT) oa_VarAnd(LPVARIANT a, LPVARIANT b, LPVARIANT out)
{
	return logic_entry(L_AND, a, b, out);
}

WINFN(HRESULT) oa_VarOr(LPVARIANT a, LPVARIANT b, LPVARIANT out)
{
	return logic_entry(L_OR, a, b, out);
}

WINFN(HRESULT) oa_VarXor(LPVARIANT a, LPVARIANT b, LPVARIANT out)
{
	return logic_entry(L_XOR, a, b, out);
}

WINFN(HRESULT) oa_VarEqv(LPVARIANT a, LPVARIANT b, LPVARIANT out)
{
	return logic_entry(L_EQV, a, b, out);
}

WINFN(HRESULT) oa_VarImp(LPVARIANT a, LPVARIANT b, LPVARIANT out)
{
	return logic_entry(L_IMP, a, b, out);
}

WINFN(HRESULT) oa_VarNot(LPVARIANT in, LPVARIANT out)
{
	VARIANT x;
	HRESULT hr;
	int64_t v;
	int rank;

	if (!out)
		return E_INVALIDARG;
	if ((hr = operand(in, &x)) != S_OK)
		return hr;
	if (x.vt == VT_NULL) {
		out->vt = VT_NULL;
		return S_OK;
	}
	hr = logic_operand(&x, &v, &rank);
	VARTYPE vt = x.vt;
	oa_VariantClear(&x);
	if (hr != S_OK)
		return hr;
	if (rank == 0) {
		set_logic(out, 0, !v);
		return S_OK;
	}
	if (vt == VT_EMPTY)
		vt = VT_I2;
	else if (!is_int_vt(vt))
		vt = VT_I4;
	set_bits(out, vt, (uint64_t)~v);
	return S_OK;
}

/* ---- comparing ---- */

static HRESULT compare(const VARIANT *a, const VARIANT *b, LCID lcid,
		       ULONG flags)
{
	VARTYPE ta = a->vt, tb = b->vt;
	HRESULT hr;

	if (ta == VT_NULL || tb == VT_NULL)
		return VARCMP_NULL;
	if (!is_num_vt(ta) || !is_num_vt(tb))
		return DISP_E_TYPEMISMATCH;
	if (ta == VT_BSTR || tb == VT_BSTR) {
		if (ta == VT_BSTR && tb == VT_BSTR) {
			UINT na = oa_SysStringLen(a->bstrVal);
			UINT nb = oa_SysStringLen(b->bstrVal);
			for (UINT i = 0; i < na && i < nb; i++) {
				WCHAR x = a->bstrVal[i], y = b->bstrVal[i];
				if (flags & NORM_IGNORECASE) {
					x = k32_toupper_w(x);
					y = k32_toupper_w(y);
				}
				if (x != y)
					return x < y ? VARCMP_LT : VARCMP_GT;
			}
			return na == nb ? VARCMP_EQ :
			       na < nb ? VARCMP_LT : VARCMP_GT;
		}
		/* Empty is ""; a number sorts before any string. */
		if (ta == VT_EMPTY)
			return oa_SysStringLen(b->bstrVal) ? VARCMP_LT : VARCMP_EQ;
		if (tb == VT_EMPTY)
			return oa_SysStringLen(a->bstrVal) ? VARCMP_GT : VARCMP_EQ;
		return ta == VT_BSTR ? VARCMP_GT : VARCMP_LT;
	}
	if (is_int_vt(ta) && is_int_vt(tb)) {
		int ua = ta == VT_UI8 && a->ullVal > INT64_MAX;
		int ub = tb == VT_UI8 && b->ullVal > INT64_MAX;
		int64_t x = 0, y = 0;
		if (ua || ub) {
			if (ua && ub)
				return a->ullVal == b->ullVal ? VARCMP_EQ :
				       a->ullVal < b->ullVal ? VARCMP_LT :
				       VARCMP_GT;
			return ua ? VARCMP_GT : VARCMP_LT;
		}
		if ((hr = as_i8(a, &x)) != S_OK || (hr = as_i8(b, &y)) != S_OK)
			return hr;
		return x == y ? VARCMP_EQ : x < y ? VARCMP_LT : VARCMP_GT;
	}
	if (ta == VT_DECIMAL || tb == VT_DECIMAL || ta == VT_CY ||
	    tb == VT_CY || (is_int_vt(ta) && is_int_vt(tb))) {
		DECIMAL p, q;
		if (as_dec(a, &p) == S_OK && as_dec(b, &q) == S_OK)
			return oa_VarDecCmp(&p, &q);
	}
	double x, y;
	if ((hr = as_r8(a, &x)) != S_OK || (hr = as_r8(b, &y)) != S_OK)
		return hr;
	if (ta == VT_R4 || tb == VT_R4) {	/* as Singles */
		x = (float)x;
		y = (float)y;
	}
	return x == y ? VARCMP_EQ : x < y ? VARCMP_LT : VARCMP_GT;
}

WINFN(HRESULT) oa_VarCmp(LPVARIANT a, LPVARIANT b, LCID lcid, ULONG flags)
{
	VARIANT x, y;
	HRESULT hr;

	if ((hr = operand(a, &x)) != S_OK)
		return hr;
	if ((hr = operand(b, &y)) != S_OK) {
		oa_VariantClear(&x);
		return hr;
	}
	hr = compare(&x, &y, lcid, flags);
	oa_VariantClear(&x);
	oa_VariantClear(&y);
	return hr;
}

#define E(name, ord) { #name, ord, (void *)oa_##name }

const struct builtin_export oa_var_exports[] = {
	E(VarParseNumFromStr, 46),
	E(VarNumFromParseNum, 47),
	E(VarAdd, 141),
	E(VarAnd, 142),
	E(VarDiv, 143),
	E(VarEqv, 152),
	E(VarIdiv, 153),
	E(VarImp, 154),
	E(VarMod, 155),
	E(VarMul, 156),
	E(VarOr, 157),
	E(VarPow, 158),
	E(VarSub, 159),
	E(VarXor, 167),
	E(VarAbs, 168),
	E(VarFix, 169),
	E(VarInt, 172),
	E(VarNeg, 173),
	E(VarNot, 174),
	E(VarRound, 175),
	E(VarCmp, 176),
	E(VarCat, 318),
	BUILTIN_END
};
