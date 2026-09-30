/*
 * Uses oleaut32 from Win32 code, importing it by ordinal only (see
 * oleaut_ord.def): BSTRs, a VARIANT converted in place, a SAFEARRAY of
 * BSTRs, a date, and error information. oleaut_run returns 0, or the
 * number of the first check that failed.
 */
#include "fixture.h"
#include <oleauto.h>

static const IID iid_errinfo = { 0x1cf2b120, 0x547d, 0x101b,
	{ 0x8e, 0x65, 0x08, 0x00, 0x2b, 0x2b, 0xd1, 0x19 } };

static int wstr_eq(const WCHAR *a, const char *b)
{
	while (*b && *a == (BYTE)*b)
		a++, b++;
	return !*b && !*a;
}

EXPORT int oleaut_run(void)
{
	static const WCHAR hello[] = { 'h', 'e', 'l', 'l', 'o', 0 };
	static const WCHAR num[] = { ' ', '4', '2', ' ', 0 };
	BSTR b, got = NULL;
	VARIANT v;
	LONG l = 0, i0 = 0;
	SYSTEMTIME st = { 2000, 1, 0, 1, 12, 0, 0, 0 };
	double d = 0;

	b = SysAllocString(hello);
	if (!b || SysStringLen(b) != 5 || !wstr_eq(b, "hello"))
		return 1;

	/* A BSTR converted to a number in place, and back. */
	VariantInit(&v);
	v.vt = VT_BSTR;
	v.bstrVal = SysAllocString(num);
	if (VariantChangeType(&v, &v, 0, VT_I4) != S_OK || v.vt != VT_I4 ||
	    v.lVal != 42)
		return 2;
	if (VariantChangeType(&v, &v, 0, VT_BSTR) != S_OK ||
	    !wstr_eq(v.bstrVal, "42"))
		return 3;
	VariantClear(&v);
	if (v.vt != VT_EMPTY)
		return 4;
	if (VarI4FromStr((OLECHAR *)num, 0, 0, &l) != S_OK || l != 42)
		return 5;
	if (VarBstrFromR8(0.25, 0, 0, &got) != S_OK || !wstr_eq(got, "0.25"))
		return 6;
	SysFreeString(got);

	/* A vector of BSTRs: copied in and out. */
	SAFEARRAY *psa = SafeArrayCreateVector(VT_BSTR, 0, 1);
	if (!psa || SafeArrayPutElement(psa, &i0, b) != S_OK)
		return 7;
	got = NULL;
	if (SafeArrayGetElement(psa, &i0, &got) != S_OK || got == b ||
	    !wstr_eq(got, "hello"))
		return 8;
	SysFreeString(got);
	if (SafeArrayDestroy(psa) != S_OK)
		return 9;
	SysFreeString(b);

	/* A date there and back. */
	if (!SystemTimeToVariantTime(&st, &d) || d != 36526.5)
		return 10;
	st.wYear = 0;
	if (!VariantTimeToSystemTime(d, &st) || st.wYear != 2000 ||
	    st.wHour != 12 || st.wDayOfWeek != 6)
		return 11;

	/* Error information for this thread. */
	ICreateErrorInfo *ce = NULL;
	IErrorInfo *ei = NULL;
	if (CreateErrorInfo(&ce) != S_OK ||
	    ce->lpVtbl->SetDescription(ce, (LPOLESTR)hello) != S_OK ||
	    ce->lpVtbl->QueryInterface(ce, &iid_errinfo, (void **)&ei) != S_OK)
		return 12;
	ce->lpVtbl->Release(ce);
	if (SetErrorInfo(0, ei) != S_OK)
		return 13;
	ei->lpVtbl->Release(ei);
	ei = NULL;
	if (GetErrorInfo(0, &ei) != S_OK || !ei)
		return 14;
	got = NULL;
	if (ei->lpVtbl->GetDescription(ei, &got) != S_OK || !wstr_eq(got, "hello"))
		return 15;
	SysFreeString(got);
	ei->lpVtbl->Release(ei);
	if (GetErrorInfo(0, &ei) != S_FALSE)
		return 16;
	return 0;
}

BOOL WINAPI DllMain(HINSTANCE inst, DWORD reason, LPVOID reserved)
{
	return TRUE;
}
