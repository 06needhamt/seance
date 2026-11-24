/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * oleaut32's numeric parser API (VarParseNumFromStr, VarNumFromParseNum)
 * VARIANT arithmetic and formatting, through its export table.
 */
#include <math.h>
#include <stdlib.h>
#include <string.h>

#include "loader/modules.h"
#include "runtime/runtime.h"
#include "test.h"
#include "win32/codepage.h"
#include "win32/oleauto.h"

static struct module *oa;

static void *get(const char *name)
{
	void *p = mod_get_proc(oa, name);
	if (!p) {
		fprintf(stderr, "oleaut32 has no %s\n", name);
		exit(1);
	}
	return p;
}

#define FN(ret, name, ...) \
	ret (WINAPI *name)(__VA_ARGS__) = (ret (WINAPI *)(__VA_ARGS__))get(#name)

typedef HRESULT (WINAPI *binop)(VARIANT *, VARIANT *, VARIANT *);
typedef HRESULT (WINAPI *unop)(VARIANT *, VARIANT *);

static BSTR (WINAPI *SysAllocString)(const OLECHAR *);
static HRESULT (WINAPI *VariantClear)(VARIANT *);
static HRESULT (WINAPI *VariantChangeType)(VARIANT *, VARIANT *, USHORT,
					   VARTYPE);

static VARIANT I2(SHORT v) { VARIANT x; x.vt = VT_I2; x.iVal = v; return x; }
static VARIANT I4(LONG v) { VARIANT x; x.vt = VT_I4; x.lVal = v; return x; }
static VARIANT I8(LONGLONG v) { VARIANT x; x.vt = VT_I8; x.llVal = v; return x; }
static VARIANT UI1(BYTE v) { VARIANT x; x.vt = VT_UI1; x.bVal = v; return x; }
static VARIANT R4(float v) { VARIANT x; x.vt = VT_R4; x.fltVal = v; return x; }
static VARIANT R8(double v) { VARIANT x; x.vt = VT_R8; x.dblVal = v; return x; }
static VARIANT DT(double v) { VARIANT x; x.vt = VT_DATE; x.date = v; return x; }
static VARIANT CYV(LONGLONG v) { VARIANT x; x.vt = VT_CY; x.cyVal.int64 = v; return x; }
static VARIANT BOOLV(int v)
{
	VARIANT x;
	x.vt = VT_BOOL;
	x.boolVal = v ? VARIANT_TRUE : VARIANT_FALSE;
	return x;
}
static VARIANT NUL(void) { VARIANT x; x.vt = VT_NULL; return x; }
static VARIANT EMP(void) { VARIANT x; x.vt = VT_EMPTY; return x; }

static VARIANT STR(const char *s)
{
	VARIANT x;
	WCHAR *w = utf8_to_utf16_dup(s);
	x.vt = VT_BSTR;
	x.bstrVal = SysAllocString(w);
	free(w);
	return x;
}

static VARIANT DEC(const char *s)
{
	VARIANT t = STR(s), d;
	d.vt = VT_EMPTY;
	CHECK_EQ(VariantChangeType(&d, &t, 0, VT_DECIMAL), S_OK);
	VariantClear(&t);
	return d;
}

/* The value as text, through VT_BSTR. */
static char *text(VARIANT *v)
{
	static char buf[128];
	VARIANT t;
	t.vt = VT_EMPTY;
	buf[0] = 0;
	if (v->vt == VT_NULL)
		return strcpy(buf, "Null");
	if (VariantChangeType(&t, v, v->vt == VT_BOOL ? 2 /* ALPHABOOL */ : 0,
			      VT_BSTR) == S_OK) {
		char *u = utf16_to_utf8_dup(t.bstrVal);
		snprintf(buf, sizeof(buf), "%s", u ? u : "");
		free(u);
		VariantClear(&t);
	}
	return buf;
}

static int failed;

static void expect(const char *what, HRESULT hr, VARIANT *r, VARTYPE vt,
		   const char *want)
{
	int ok = hr == S_OK && r->vt == vt && !strcmp(text(r), want);
	CHECK(ok);
	if (!ok) {
		fprintf(stderr, "  %s: hr %08x vt %d \"%s\", want vt %d \"%s\"\n",
			what, (unsigned)hr, r->vt, hr == S_OK ? text(r) : "",
			vt, want);
		failed++;
	}
	if (hr == S_OK)
		VariantClear(r);
}

static void bin(const char *what, binop f, VARIANT a, VARIANT b, VARTYPE vt,
		const char *want)
{
	VARIANT r;
	r.vt = VT_EMPTY;
	HRESULT hr = f(&a, &b, &r);
	expect(what, hr, &r, vt, want);
	VariantClear(&a);
	VariantClear(&b);
}

static void bin_fails(const char *what, binop f, VARIANT a, VARIANT b,
		      HRESULT want)
{
	VARIANT r;
	r.vt = VT_EMPTY;
	HRESULT hr = f(&a, &b, &r);
	CHECK_EQ(hr, want);
	if (hr != want)
		fprintf(stderr, "  %s\n", what);
	VariantClear(&a);
	VariantClear(&b);
}

static void un(const char *what, unop f, VARIANT a, VARTYPE vt,
	       const char *want)
{
	VARIANT r;
	r.vt = VT_EMPTY;
	HRESULT hr = f(&a, &r);
	expect(what, hr, &r, vt, want);
	VariantClear(&a);
}

static void test_parse(void)
{
	FN(HRESULT, VarParseNumFromStr, const OLECHAR *, LCID, ULONG,
	   NUMPARSE *, BYTE *);
	FN(HRESULT, VarNumFromParseNum, NUMPARSE *, BYTE *, ULONG, VARIANT *);
	NUMPARSE np;
	BYTE dig[32];
	WCHAR *w;
	VARIANT v;

#define PARSE(s, flags) \
	(w = utf8_to_utf16_dup(s), np.cDig = 32, np.dwInFlags = (flags), \
	 VarParseNumFromStr(w, 0x409, 0, &np, dig))

	CHECK_EQ(PARSE("  -1,234.50  ", NUMPRS_STD), S_OK);
	free(w);
	CHECK_EQ(np.cDig, 5);	/* 12345, the trailing zero dropped */
	CHECK(!memcmp(dig, "\1\2\3\4\5", 5));
	CHECK_EQ(np.nPwr10, -1);
	CHECK_EQ(np.cchUsed, 13);
	CHECK_EQ(np.dwOutFlags, NUMPRS_LEADING_WHITE | NUMPRS_LEADING_MINUS |
			       NUMPRS_THOUSANDS | NUMPRS_DECIMAL |
			       NUMPRS_TRAILING_WHITE | NUMPRS_NEG);
	CHECK_EQ(np.nBaseShift, 0);

	CHECK_EQ(PARSE("100", NUMPRS_STD), S_OK);
	free(w);
	CHECK(np.cDig == 1 && dig[0] == 1 && np.nPwr10 == 2);
	CHECK_EQ(PARSE("0.005", NUMPRS_STD), S_OK);
	free(w);
	CHECK(np.cDig == 1 && dig[0] == 5 && np.nPwr10 == -3);
	CHECK_EQ(PARSE("000", NUMPRS_STD), S_OK);
	free(w);
	CHECK(np.cDig == 1 && dig[0] == 0 && np.nPwr10 == 0);
	CHECK_EQ(PARSE("1.5E+3", NUMPRS_STD), S_OK);
	free(w);
	CHECK(np.cDig == 2 && np.nPwr10 == 2 &&
	      (np.dwOutFlags & NUMPRS_EXPONENT));
	CHECK_EQ(PARSE("($12)", NUMPRS_STD), S_OK);
	free(w);
	CHECK(np.dwOutFlags & NUMPRS_NEG);
	CHECK(np.dwOutFlags & NUMPRS_CURRENCY);
	CHECK_EQ(PARSE("(-1)", NUMPRS_STD), DISP_E_TYPEMISMATCH);
	free(w);
	CHECK_EQ(PARSE("(1", NUMPRS_STD), DISP_E_TYPEMISMATCH);
	free(w);
	CHECK_EQ(PARSE("12-", NUMPRS_STD), S_OK);
	free(w);
	CHECK(np.dwOutFlags & NUMPRS_TRAILING_MINUS);
	CHECK(np.dwOutFlags & NUMPRS_NEG);

	/* Flags not given: the parse stops there, unless all is wanted. */
	CHECK_EQ(PARSE("12.5", NUMPRS_STD & ~NUMPRS_DECIMAL & ~NUMPRS_USE_ALL),
		 S_OK);
	free(w);
	CHECK(np.cDig == 2 && np.nPwr10 == 0 && np.cchUsed == 2);
	CHECK_EQ(PARSE("12.5", NUMPRS_STD & ~NUMPRS_DECIMAL),
		 DISP_E_TYPEMISMATCH);
	free(w);
	CHECK_EQ(PARSE(" 1", 0), DISP_E_TYPEMISMATCH);
	free(w);
	CHECK_EQ(PARSE("abc", NUMPRS_STD), DISP_E_TYPEMISMATCH);
	free(w);
	CHECK_EQ(PARSE("1e", NUMPRS_STD), DISP_E_TYPEMISMATCH);
	free(w);
	CHECK_EQ(PARSE("1e", NUMPRS_STD & ~NUMPRS_USE_ALL), S_OK);
	free(w);
	CHECK_EQ(np.cchUsed, 1);

	/* More digits than room: the power grows, and it's inexact. */
	np.cDig = 3;
	np.dwInFlags = NUMPRS_STD;
	w = utf8_to_utf16_dup("123456");
	CHECK_EQ(VarParseNumFromStr(w, 0x409, 0, &np, dig), S_OK);
	free(w);
	CHECK(np.cDig == 3 && np.nPwr10 == 3 &&
	      (np.dwOutFlags & NUMPRS_INEXACT));

	/* Hex and octal. */
	CHECK_EQ(PARSE("&HFFff", NUMPRS_STD), S_OK);
	free(w);
	CHECK(np.nBaseShift == 4 && np.cDig == 4 && dig[0] == 15);
	CHECK_EQ(VarNumFromParseNum(&np, dig, VTBIT_I2 | VTBIT_I4, &v), S_OK);
	CHECK(v.vt == VT_I2 && v.iVal == -1);
	CHECK_EQ(VarNumFromParseNum(&np, dig, VTBIT_UI1 | VTBIT_I4, &v), S_OK);
	CHECK(v.vt == VT_I4 && v.lVal == 0xffff);
	CHECK_EQ(PARSE("&o17", NUMPRS_STD), S_OK);
	free(w);
	CHECK(np.nBaseShift == 3);
	CHECK_EQ(VarNumFromParseNum(&np, dig, VTBIT_I4, &v), S_OK);
	CHECK(v.vt == VT_I4 && v.lVal == 15);
	CHECK_EQ(PARSE("&H1", NUMPRS_STD & ~NUMPRS_HEX_OCT),
		 DISP_E_TYPEMISMATCH);
	free(w);

	/* The smallest type allowed. */
	CHECK_EQ(PARSE("200", NUMPRS_STD), S_OK);
	free(w);
	CHECK_EQ(VarNumFromParseNum(&np, dig, VTBIT_I1 | VTBIT_UI1 | VTBIT_I2,
				    &v), S_OK);
	CHECK(v.vt == VT_UI1 && v.bVal == 200);
	CHECK_EQ(VarNumFromParseNum(&np, dig, VTBIT_I1 | VTBIT_I2, &v), S_OK);
	CHECK(v.vt == VT_I2 && v.iVal == 200);
	CHECK_EQ(VarNumFromParseNum(&np, dig, VTBIT_I1, &v), DISP_E_OVERFLOW);
	CHECK_EQ(PARSE("-40000", NUMPRS_STD), S_OK);
	free(w);
	CHECK_EQ(VarNumFromParseNum(&np, dig, VTBIT_I2 | VTBIT_I4 | VTBIT_R8,
				    &v), S_OK);
	CHECK(v.vt == VT_I4 && v.lVal == -40000);
	CHECK_EQ(PARSE("1.25", NUMPRS_STD), S_OK);
	free(w);
	CHECK_EQ(VarNumFromParseNum(&np, dig, VTBIT_I4 | VTBIT_R8 | VTBIT_CY,
				    &v), S_OK);
	CHECK(v.vt == VT_R8 && v.dblVal == 1.25);
	CHECK_EQ(VarNumFromParseNum(&np, dig, VTBIT_I4 | VTBIT_CY, &v), S_OK);
	CHECK(v.vt == VT_CY && v.cyVal.int64 == 12500);
	CHECK_EQ(VarNumFromParseNum(&np, dig, VTBIT_I4 | VTBIT_DECIMAL, &v),
		 S_OK);
	CHECK(v.vt == VT_DECIMAL && v.decVal.scale == 2 &&
	      v.decVal.Lo64 == 125);
	CHECK_EQ(VarNumFromParseNum(&np, dig, VTBIT_I4, &v), DISP_E_OVERFLOW);
	/* 20 digits: past a double's exactness, so a decimal. */
	np.cDig = 32;
	np.dwInFlags = NUMPRS_STD;
	w = utf8_to_utf16_dup("12345678901234567890.5");
	CHECK_EQ(VarParseNumFromStr(w, 0x409, 0, &np, dig), S_OK);
	free(w);
	CHECK_EQ(VarNumFromParseNum(&np, dig, VTBIT_R8 | VTBIT_DECIMAL, &v),
		 S_OK);
	CHECK_EQ(v.vt, VT_DECIMAL);
	CHECK(!strcmp(text(&v), "12345678901234567890.5"));
	CHECK_EQ(VarNumFromParseNum(&np, dig, VTBIT_R8, &v), S_OK);
	CHECK_EQ(v.vt, VT_R8);
	CHECK_EQ(PARSE("1e400", NUMPRS_STD), S_OK);
	free(w);
	CHECK_EQ(VarNumFromParseNum(&np, dig, VTBIT_R8 | VTBIT_DECIMAL, &v),
		 DISP_E_OVERFLOW);
#undef PARSE
}

static void test_arith(void)
{
	FN(HRESULT, VarAdd, VARIANT *, VARIANT *, VARIANT *);
	FN(HRESULT, VarSub, VARIANT *, VARIANT *, VARIANT *);
	FN(HRESULT, VarMul, VARIANT *, VARIANT *, VARIANT *);
	FN(HRESULT, VarDiv, VARIANT *, VARIANT *, VARIANT *);
	FN(HRESULT, VarIdiv, VARIANT *, VARIANT *, VARIANT *);
	FN(HRESULT, VarMod, VARIANT *, VARIANT *, VARIANT *);
	FN(HRESULT, VarPow, VARIANT *, VARIANT *, VARIANT *);
	FN(HRESULT, VarCat, VARIANT *, VARIANT *, VARIANT *);

	bin("i2+i2", VarAdd, I2(2), I2(3), VT_I2, "5");
	bin("ui1+ui1", VarAdd, UI1(200), UI1(100), VT_I2, "300");
	bin("ui1-ui1", VarSub, UI1(1), UI1(2), VT_I2, "-1");
	bin("i2 overflow", VarAdd, I2(32767), I2(1), VT_I4, "32768");
	bin("i4 overflow", VarMul, I4(0x40000000), I4(4), VT_R8, "4294967296");
	bin("i8", VarAdd, I8(1LL << 40), I2(1), VT_I8, "1099511627777");
	bin("bool", VarAdd, BOOLV(1), BOOLV(1), VT_I2, "-2");
	bin("empty+empty", VarAdd, EMP(), EMP(), VT_I2, "0");
	bin("empty+ui1", VarAdd, EMP(), UI1(7), VT_UI1, "7");
	bin("null", VarAdd, NUL(), I2(1), VT_NULL, "Null");
	bin("r4+i2", VarAdd, R4(1.5f), I2(1), VT_R4, "2.5");
	bin("r4+i4", VarAdd, R4(1.5f), I4(1), VT_R8, "2.5");
	bin("r8", VarMul, R8(1.5), I2(4), VT_R8, "6");
	bin("cy+r4", VarAdd, CYV(15000), R4(1), VT_CY, "2.5");
	bin("cy*cy", VarMul, CYV(15000), CYV(15000), VT_CY, "2.25");
	bin("cy+r8", VarAdd, CYV(15000), R8(1), VT_R8, "2.5");
	bin("dec", VarAdd, DEC("0.1"), DEC("0.2"), VT_DECIMAL, "0.3");
	bin("dec*i4", VarMul, DEC("1.5"), I4(3), VT_DECIMAL, "4.5");
	bin("date+1", VarAdd, DT(2.0), I2(1), VT_DATE, "1/2/1900");
	bin("date-date", VarSub, DT(5.5), DT(2.0), VT_R8, "3.5");
	bin("date-num", VarSub, DT(5.0), R8(1.0), VT_DATE, "1/3/1900");
	bin("str+str", VarAdd, STR("ab"), STR("cd"), VT_BSTR, "abcd");
	bin("str+num", VarAdd, STR("1.5"), I2(2), VT_R8, "3.5");
	bin("str+empty", VarAdd, STR("x"), EMP(), VT_BSTR, "x");
	bin_fails("str+word", VarAdd, STR("x"), I2(1), DISP_E_TYPEMISMATCH);
	bin_fails("dispatch-less", VarAdd, I2(1), STR("1.2.3"),
		  DISP_E_TYPEMISMATCH);

	bin("i2/i2", VarDiv, I2(7), I2(2), VT_R4, "3.5");
	bin("i4/i2", VarDiv, I4(7), I2(2), VT_R8, "3.5");
	bin("dec/", VarDiv, DEC("1"), I2(4), VT_DECIMAL, "0.25");
	bin_fails("1/0", VarDiv, I2(1), I2(0), DISP_E_DIVBYZERO);
	bin_fails("0/0", VarDiv, I2(0), I2(0), DISP_E_OVERFLOW);
	bin("null/0", VarDiv, NUL(), I2(0), VT_NULL, "Null");

	bin("idiv", VarIdiv, I2(7), I2(2), VT_I2, "3");
	bin("idiv neg", VarIdiv, I2(-7), I2(2), VT_I2, "-3");
	bin("idiv r8", VarIdiv, R8(7.6), R8(2.5), VT_I4, "4");	/* 8 \ 2 */
	bin("idiv ui1", VarIdiv, UI1(200), UI1(3), VT_UI1, "66");
	bin("mod", VarMod, I4(-7), I2(3), VT_I4, "-1");
	bin("mod r8", VarMod, R8(10.5), I2(4), VT_I4, "2");	/* 10 Mod 4 */
	bin_fails("mod 0", VarMod, I4(1), I4(0), DISP_E_DIVBYZERO);
	bin("idiv widen", VarIdiv, I2(-32768), I2(-1), VT_I4, "32768");

	bin("pow", VarPow, I2(2), I2(10), VT_R8, "1024");
	bin("pow frac", VarPow, R8(4), R8(0.5), VT_R8, "2");
	bin_fails("pow neg", VarPow, R8(-8), R8(0.5), E_INVALIDARG);
	bin("pow null", VarPow, I2(2), NUL(), VT_NULL, "Null");

	bin("cat", VarCat, STR("a"), I2(1), VT_BSTR, "a1");
	bin("cat bool", VarCat, BOOLV(1), STR("!"), VT_BSTR, "True!");
	bin("cat null", VarCat, NUL(), STR("b"), VT_BSTR, "b");
	bin("cat nulls", VarCat, NUL(), NUL(), VT_NULL, "Null");

	/* By reference. */
	VARIANT x = I2(5), ref, y = I2(1), r;
	ref.vt = VT_BYREF | VT_I2;
	ref.piVal = &x.iVal;
	r.vt = VT_EMPTY;
	CHECK_EQ(VarAdd(&ref, &y, &r), S_OK);
	CHECK(r.vt == VT_I2 && r.iVal == 6);
}

static void test_unary(void)
{
	FN(HRESULT, VarNeg, VARIANT *, VARIANT *);
	FN(HRESULT, VarAbs, VARIANT *, VARIANT *);
	FN(HRESULT, VarFix, VARIANT *, VARIANT *);
	FN(HRESULT, VarInt, VARIANT *, VARIANT *);
	FN(HRESULT, VarNot, VARIANT *, VARIANT *);
	FN(HRESULT, VarRound, VARIANT *, int, VARIANT *);
	VARIANT r, a;

	un("neg i2", VarNeg, I2(5), VT_I2, "-5");
	un("neg min", VarNeg, I2(-32768), VT_I4, "32768");
	un("neg ui1", VarNeg, UI1(5), VT_I2, "-5");
	un("neg bool", VarNeg, BOOLV(1), VT_I2, "1");
	un("neg r8", VarNeg, R8(1.5), VT_R8, "-1.5");
	un("neg cy", VarNeg, CYV(10000), VT_CY, "-1");
	un("neg dec", VarNeg, DEC("2.5"), VT_DECIMAL, "-2.5");
	un("neg str", VarNeg, STR("3"), VT_R8, "-3");
	un("neg empty", VarNeg, EMP(), VT_I2, "0");
	un("neg null", VarNeg, NUL(), VT_NULL, "Null");
	un("abs", VarAbs, I4(-9), VT_I4, "9");
	un("abs bool", VarAbs, BOOLV(1), VT_I2, "1");
	un("abs r4", VarAbs, R4(-2.5f), VT_R4, "2.5");
	un("fix", VarFix, R8(-2.7), VT_R8, "-2");
	un("int", VarInt, R8(-2.7), VT_R8, "-3");
	un("int cy", VarInt, CYV(-25000), VT_CY, "-3");
	un("fix cy", VarFix, CYV(-25000), VT_CY, "-2");
	un("int dec", VarInt, DEC("-2.5"), VT_DECIMAL, "-3");
	un("fix i2", VarFix, I2(4), VT_I2, "4");
	un("not i2", VarNot, I2(0), VT_I2, "-1");
	un("not ui1", VarNot, UI1(0xf0), VT_UI1, "15");
	un("not bool", VarNot, BOOLV(1), VT_BOOL, "False");
	un("not r8", VarNot, R8(1.6), VT_I4, "-3");	/* Not 2 */
	un("not str", VarNot, STR("True"), VT_BOOL, "False");
	un("not empty", VarNot, EMP(), VT_I2, "-1");

	struct { double v; int places; const char *want; } rounds[] = {
		{ 2.5, 0, "2" }, { 3.5, 0, "4" }, { -2.5, 0, "-2" },
		{ 1.2345, 2, "1.23" }, { 0.125, 2, "0.12" }, { 1.5, 5, "1.5" },
	};
	for (size_t i = 0; i < sizeof(rounds) / sizeof(*rounds); i++) {
		a = R8(rounds[i].v);
		r.vt = VT_EMPTY;
		expect("round", VarRound(&a, rounds[i].places, &r), &r, VT_R8,
		       rounds[i].want);
	}
	a = CYV(12345);		/* 1.2345 */
	expect("round cy", VarRound(&a, 2, &r), &r, VT_CY, "1.23");
	a = CYV(12350);
	expect("round cy even", VarRound(&a, 2, &r), &r, VT_CY, "1.24");
	a = DEC("2.345");
	expect("round dec", VarRound(&a, 2, &r), &r, VT_DECIMAL, "2.34");
	a = R8(1);
	CHECK_EQ(VarRound(&a, -1, &r), E_INVALIDARG);
}

static void test_logic(void)
{
	FN(HRESULT, VarAnd, VARIANT *, VARIANT *, VARIANT *);
	FN(HRESULT, VarOr, VARIANT *, VARIANT *, VARIANT *);
	FN(HRESULT, VarXor, VARIANT *, VARIANT *, VARIANT *);
	FN(HRESULT, VarEqv, VARIANT *, VARIANT *, VARIANT *);
	FN(HRESULT, VarImp, VARIANT *, VARIANT *, VARIANT *);

	bin("and bool", VarAnd, BOOLV(1), BOOLV(0), VT_BOOL, "False");
	bin("or bool", VarOr, BOOLV(1), BOOLV(0), VT_BOOL, "True");
	bin("and i2", VarAnd, I2(6), I2(3), VT_I2, "2");
	bin("or ui1", VarOr, UI1(1), UI1(2), VT_UI1, "3");
	bin("xor i4", VarXor, I4(6), I2(3), VT_I4, "5");
	bin("and bool ui1", VarAnd, BOOLV(1), UI1(5), VT_I2, "5");
	bin("and r8", VarAnd, R8(7.0), I2(5), VT_I4, "5");
	bin("eqv", VarEqv, BOOLV(1), BOOLV(1), VT_BOOL, "True");
	bin("eqv i2", VarEqv, I2(0), I2(0), VT_I2, "-1");
	bin("imp", VarImp, BOOLV(1), BOOLV(0), VT_BOOL, "False");
	bin("imp f", VarImp, BOOLV(0), BOOLV(0), VT_BOOL, "True");
	bin("and str", VarAnd, STR("True"), BOOLV(1), VT_BOOL, "True");
	bin("and null false", VarAnd, NUL(), BOOLV(0), VT_BOOL, "False");
	bin("and null true", VarAnd, NUL(), BOOLV(1), VT_NULL, "Null");
	bin("and null 0", VarAnd, I4(0), NUL(), VT_I4, "0");
	bin("or null true", VarOr, BOOLV(1), NUL(), VT_BOOL, "True");
	bin("or null false", VarOr, NUL(), BOOLV(0), VT_NULL, "Null");
	bin("xor null", VarXor, NUL(), BOOLV(1), VT_NULL, "Null");
	bin("imp null true", VarImp, NUL(), BOOLV(1), VT_BOOL, "True");
	bin("false imp null", VarImp, BOOLV(0), NUL(), VT_BOOL, "True");
	bin("true imp null", VarImp, BOOLV(1), NUL(), VT_NULL, "Null");
	bin("and empty", VarAnd, EMP(), EMP(), VT_I2, "0");
	bin_fails("and word", VarAnd, STR("x"), I2(1), DISP_E_TYPEMISMATCH);
}

static void test_cmp(void)
{
	FN(HRESULT, VarCmp, VARIANT *, VARIANT *, LCID, ULONG);
	struct {
		VARIANT a, b;
		ULONG flags;
		HRESULT want;
	} c[] = {
		{ I2(1), I4(2), 0, VARCMP_LT },
		{ I8(INT64_MAX), I8(INT64_MAX - 1), 0, VARCMP_GT },
		{ R8(1.5), CYV(15000), 0, VARCMP_EQ },
		{ DEC("0.1"), R8(0.2), 0, VARCMP_LT },
		{ BOOLV(1), I2(-1), 0, VARCMP_EQ },
		{ EMP(), I2(0), 0, VARCMP_EQ },
		{ NUL(), I2(0), 0, VARCMP_NULL },
		{ STR("abc"), STR("ABD"), 0, VARCMP_GT },
		{ STR("abc"), STR("ABC"), 1 /* NORM_IGNORECASE */, VARCMP_EQ },
		{ STR("ab"), STR("abc"), 0, VARCMP_LT },
		{ STR("1"), I2(5), 0, VARCMP_GT },
		{ I2(5), STR("1"), 0, VARCMP_LT },
		{ EMP(), STR(""), 0, VARCMP_EQ },
		{ EMP(), STR("a"), 0, VARCMP_LT },
		{ DT(2.0), R8(2.0), 0, VARCMP_EQ },
		{ R4(0.1f), R8(0.1), 0, VARCMP_EQ },	/* as Singles */
	};
	for (size_t i = 0; i < sizeof(c) / sizeof(*c); i++) {
		HRESULT hr = VarCmp(&c[i].a, &c[i].b, 0x409, c[i].flags);
		CHECK_EQ(hr, c[i].want);
		if (hr != c[i].want)
			fprintf(stderr, "  compare %zu\n", i);
		VariantClear(&c[i].a);
		VariantClear(&c[i].b);
	}
	VARIANT e;
	e.vt = VT_ERROR;
	e.scode = 0;
	VARIANT one = I2(1);
	CHECK_EQ(VarCmp(&e, &one, 0x409, 0), DISP_E_TYPEMISMATCH);
}

static int bstr_is(BSTR b, const char *want)
{
	char *u = b ? utf16_to_utf8_dup(b) : NULL;
	int ok = u && !strcmp(u, want);
	if (!ok)
		fprintf(stderr, "  got \"%s\", want \"%s\"\n", u ? u : "(null)",
			want);
	free(u);
	return ok;
}

static void test_format(void)
{
	FN(HRESULT, VarFormat, VARIANT *, LPOLESTR, int, int, ULONG, BSTR *);
	FN(HRESULT, VarFormatNumber, VARIANT *, int, int, int, int, ULONG,
	   BSTR *);
	FN(HRESULT, VarFormatPercent, VARIANT *, int, int, int, int, ULONG,
	   BSTR *);
	FN(HRESULT, VarFormatCurrency, VARIANT *, int, int, int, int, ULONG,
	   BSTR *);
	FN(HRESULT, VarFormatDateTime, VARIANT *, int, ULONG, BSTR *);
	FN(HRESULT, VarWeekdayName, int, int, int, ULONG, BSTR *);
	FN(HRESULT, VarMonthName, int, int, ULONG, BSTR *);
	FN(HRESULT, VarTokenizeFormatString, LPOLESTR, BYTE *, int, int, int,
	   LCID, int *);
	FN(HRESULT, VarFormatFromTokens, VARIANT *, LPOLESTR, BYTE *, ULONG,
	   BSTR *, LCID);
	FN(void, SysFreeString, BSTR);
	BSTR b;

	VARIANT when = STR("11/24/2026 1:05:09 PM"), date;
	date.vt = VT_EMPTY;
	CHECK_EQ(VariantChangeType(&date, &when, 0, VT_DATE), S_OK);
	VariantClear(&when);

	struct {
		VARIANT v;
		const char *fmt;
		int first_day, first_week;
		const char *want;
	} f[] = {
		{ R8(1234.567), "#,##0.00", 0, 0, "1,234.57" },
		{ R8(0.5), "#.##", 0, 0, ".5" },
		{ I2(5), "#.##", 0, 0, "5." },
		{ I2(5), "000", 0, 0, "005" },
		{ I4(1234567), "#,##0", 0, 0, "1,234,567" },
		{ I4(1234567), "#,##0,", 0, 0, "1,235" },
		{ I4(1234567), "0,,", 0, 0, "1" },
		{ R8(0.256), "0.0%", 0, 0, "25.6%" },
		{ I2(-5), "0;(0)", 0, 0, "(5)" },
		{ I2(-5), "0", 0, 0, "-5" },
		{ I2(0), "0;(0);\"zero\"", 0, 0, "zero" },
		{ I2(-5), "0;;\"zero\"", 0, 0, "-5" },
		{ R8(1234.5), "0.00E+00", 0, 0, "1.23E+03" },
		{ R8(0.00012), "0.0E-0", 0, 0, "1.2E-4" },
		{ R8(9.996), "0.00E+00", 0, 0, "1.00E+01" },
		{ R8(2.5), "0", 0, 0, "3" },
		{ R8(-2.5), "0", 0, 0, "-3" },
		{ R8(-0.001), "0", 0, 0, "0" },
		{ R8(5551234567.0), "(000) 000-0000", 0, 0, "(555) 123-4567" },
		{ I4(42), "\\#0 \"items\"", 0, 0, "#42 items" },
		{ CYV(12345678), "0.000", 0, 0, "1234.568" },
		{ DEC("12345678901234567890.5"), "#,##0", 0, 0,
		  "12,345,678,901,234,567,891" },
		{ R8(1234.567), "Currency", 0, 0, "$1,234.57" },
		{ I2(-1), "Currency", 0, 0, "($1.00)" },
		{ R8(1234.567), "Fixed", 0, 0, "1234.57" },
		{ R8(1234.567), "Standard", 0, 0, "1,234.57" },
		{ R8(0.5), "Percent", 0, 0, "50.00%" },
		{ R8(1234.5), "Scientific", 0, 0, "1.23E+03" },
		{ R8(1234.5), "General Number", 0, 0, "1234.5" },
		{ I2(0), "Yes/No", 0, 0, "No" },
		{ I2(5), "yes/no", 0, 0, "Yes" },
		{ BOOLV(1), "True/False", 0, 0, "True" },
		{ I2(0), "On/Off", 0, 0, "Off" },
		{ NUL(), "0;0;0;\"nul\"", 0, 0, "nul" },
		{ NUL(), "0", 0, 0, "" },
		{ BOOLV(1), "0", 0, 0, "-1" },
		{ date, "yyyy-mm-dd hh:nn:ss", 0, 0, "2026-11-24 13:05:09" },
		{ date, "dddd, mmmm d, yyyy", 0, 0, "Tuesday, November 24, 2026" },
		{ date, "ddd d mmm yy", 0, 0, "Tue 24 Nov 26" },
		{ date, "h:mm AM/PM", 0, 0, "1:05 PM" },
		{ date, "hh:mm a/p", 0, 0, "01:05 p" },
		{ date, "q y w", 0, 0, "4 328 3" },
		{ date, "w", 2, 0, "2" },
		{ date, "ww", 0, 1, "48" },
		{ date, "ww", 2, 2, "48" },
		{ date, "c", 0, 0, "11/24/2026 1:05:09 PM" },
		{ date, "ddddd", 0, 0, "11/24/2026" },
		{ date, "dddddd", 0, 0, "Tuesday, November 24, 2026" },
		{ date, "ttttt", 0, 0, "1:05:09 PM" },
		{ date, "Long Date", 0, 0, "Tuesday, November 24, 2026" },
		{ date, "Medium Date", 0, 0, "24-Nov-26" },
		{ date, "Short Date", 0, 0, "11/24/2026" },
		{ date, "Long Time", 0, 0, "1:05:09 PM" },
		{ date, "Medium Time", 0, 0, "01:05 PM" },
		{ date, "Short Time", 0, 0, "13:05" },
		{ date, "\\Y yyyy", 0, 0, "Y 2026" },
		{ date, "", 0, 0, "11/24/2026 1:05:09 PM" },
		{ I2(2), "m/d/yyyy", 0, 0, "1/1/1900" },
		{ STR("abc"), ">", 0, 0, "ABC" },
		{ STR("ABC"), "<", 0, 0, "abc" },
		{ STR("hello"), "@@@@@@@", 0, 0, "  hello" },
		{ STR("hello"), "!@@@@@@@", 0, 0, "hello  " },
		{ STR("abcd"), "&&-&&", 0, 0, "ab-cd" },
		{ STR("abcdef"), "&&-&&", 0, 0, "abcd-ef" },
		{ STR("123"), "0.00", 0, 0, "123.00" },
		{ STR("abc"), "0.00", 0, 0, "abc" },
		{ I2(12), "@@@@", 0, 0, "  12" },
		{ BOOLV(1), "", 0, 0, "True" },
		{ R8(1.5), "", 0, 0, "1.5" },
	};
	for (size_t i = 0; i < sizeof(f) / sizeof(*f); i++) {
		WCHAR *w = utf8_to_utf16_dup(f[i].fmt);
		b = NULL;
		HRESULT hr = VarFormat(&f[i].v, w, f[i].first_day,
				       f[i].first_week, 0, &b);
		int ok = hr == S_OK && bstr_is(b, f[i].want);
		CHECK(ok);
		if (!ok)
			fprintf(stderr, "  format %zu \"%s\": %08x\n", i, f[i].fmt,
				(unsigned)hr);
		SysFreeString(b);
		free(w);
		if (f[i].v.vt == VT_BSTR)
			VariantClear(&f[i].v);
	}
	VARIANT one = I2(1);
	CHECK_EQ(VarFormat(&one, NULL, 8, 0, 0, &b), E_INVALIDARG);

	struct {
		VARIANT v;
		int digits, lead, parens, group, which;
		const char *want;
	} n[] = {
		{ R8(1234.567), -1, -2, -2, -2, 0, "1,234.57" },
		{ R8(-0.5), 1, 0, -2, -2, 0, "-.5" },
		{ R8(-0.5), -1, -1, -1, -2, 0, "(0.50)" },
		{ R8(1234.567), 2, -2, -2, 0, 0, "1234.57" },
		{ R8(2.5), 0, -2, -2, -2, 0, "3" },
		{ R8(0.1234), 1, -2, -2, -2, 1, "12.3%" },
		{ R8(-1234.5), -1, -2, -2, -2, 2, "($1,234.50)" },
		{ R8(-1234.5), -1, -2, 0, -2, 2, "-$1,234.50" },
		{ STR("7"), 0, -2, -2, -2, 2, "$7" },
	};
	for (size_t i = 0; i < sizeof(n) / sizeof(*n); i++) {
		HRESULT hr;
		b = NULL;
		if (n[i].which == 0)
			hr = VarFormatNumber(&n[i].v, n[i].digits, n[i].lead,
					     n[i].parens, n[i].group, 0, &b);
		else if (n[i].which == 1)
			hr = VarFormatPercent(&n[i].v, n[i].digits, n[i].lead,
					      n[i].parens, n[i].group, 0, &b);
		else
			hr = VarFormatCurrency(&n[i].v, n[i].digits, n[i].lead,
					       n[i].parens, n[i].group, 0, &b);
		int ok = hr == S_OK && bstr_is(b, n[i].want);
		CHECK(ok);
		if (!ok)
			fprintf(stderr, "  fixed %zu: %08x\n", i, (unsigned)hr);
		SysFreeString(b);
		VariantClear(&n[i].v);
	}
	CHECK_EQ(VarFormatNumber(&one, 100, -2, -2, -2, 0, &b), E_INVALIDARG);

	static const char *const dt[] = {
		"11/24/2026 1:05:09 PM", "Tuesday, November 24, 2026",
		"11/24/2026", "1:05:09 PM", "13:05",
	};
	for (int i = 0; i < 5; i++) {
		b = NULL;
		CHECK_EQ(VarFormatDateTime(&date, i, 0, &b), S_OK);
		CHECK(bstr_is(b, dt[i]));
		SysFreeString(b);
	}
	CHECK_EQ(VarFormatDateTime(&date, 5, 0, &b), E_INVALIDARG);

	CHECK_EQ(VarWeekdayName(1, 0, 0, 0, &b), S_OK);
	CHECK(bstr_is(b, "Sunday"));
	SysFreeString(b);
	CHECK_EQ(VarWeekdayName(1, 1, 2, 0, &b), S_OK);
	CHECK(bstr_is(b, "Mon"));
	SysFreeString(b);
	CHECK_EQ(VarWeekdayName(7, 0, 2, 0, &b), S_OK);
	CHECK(bstr_is(b, "Sunday"));
	SysFreeString(b);
	CHECK_EQ(VarWeekdayName(8, 0, 0, 0, &b), E_INVALIDARG);
	CHECK_EQ(VarMonthName(12, 1, 0, &b), S_OK);
	CHECK(bstr_is(b, "Dec"));
	SysFreeString(b);
	CHECK_EQ(VarMonthName(0, 0, 0, &b), E_INVALIDARG);

	BYTE tok[256];
	int used = 0;
	WCHAR *w = utf8_to_utf16_dup("0.0");
	CHECK_EQ(VarTokenizeFormatString(w, tok, 4, 0, 0, 0x409, &used),
		 DISP_E_BUFFERTOOSMALL);
	CHECK_EQ(VarTokenizeFormatString(w, tok, sizeof(tok), 0, 0, 0x409,
					 &used), S_OK);
	CHECK(used > 0 && used <= (int)sizeof(tok));
	VARIANT q = R8(1.25);
	CHECK_EQ(VarFormatFromTokens(&q, w, tok, 0, &b, 0x409), S_OK);
	CHECK(bstr_is(b, "1.3"));
	SysFreeString(b);
	free(w);
}

static void test_ordinals(void)
{
	static const struct { const char *name; WORD ord; } o[] = {
		{ "VarParseNumFromStr", 46 }, { "VarNumFromParseNum", 47 },
		{ "VarAdd", 141 }, { "VarAnd", 142 }, { "VarDiv", 143 },
		{ "VarEqv", 152 }, { "VarIdiv", 153 }, { "VarImp", 154 },
		{ "VarMod", 155 }, { "VarMul", 156 }, { "VarOr", 157 },
		{ "VarPow", 158 }, { "VarSub", 159 }, { "VarXor", 167 },
		{ "VarAbs", 168 }, { "VarFix", 169 }, { "VarInt", 172 },
		{ "VarNeg", 173 }, { "VarNot", 174 }, { "VarRound", 175 },
		{ "VarCmp", 176 }, { "VarCat", 318 },
		{ "VarFormat", 87 }, { "VarFormatDateTime", 97 },
		{ "VarFormatNumber", 107 }, { "VarFormatPercent", 117 },
		{ "VarFormatCurrency", 127 }, { "VarWeekdayName", 128 },
		{ "VarMonthName", 129 }, { "VarFormatFromTokens", 139 },
		{ "VarTokenizeFormatString", 140 },
	};
	for (size_t i = 0; i < sizeof(o) / sizeof(*o); i++)
		CHECK(mod_get_proc(oa, o[i].name) &&
		      mod_get_proc(oa, o[i].name) ==
		      mod_get_ordinal(oa, o[i].ord));
}

int main(void)
{
	CHECK_EQ(runtime_enter(), 0);
	oa = mod_load("oleaut32.dll", NULL);
	CHECK(oa != NULL);
	if (!oa)
		return test_summary("test_oleaut_var");
	SysAllocString = get("SysAllocString");
	VariantClear = get("VariantClear");
	VariantChangeType = get("VariantChangeType");

	test_parse();
	test_arith();
	test_unary();
	test_logic();
	test_cmp();
	test_format();
	test_ordinals();
	mod_free(oa);
	return test_summary("test_oleaut_var");
}
