/*
 * msvcrt: the printf family's formatting, as msvcrt.dll does it, which
 * glibc's differs from in ways a codec's log or file names would show:
 *
 * - size prefixes I64 (and ll) for 64-bit integers, I32 and I (32-bit
 *   here), h, l, w; L on a floating conversion is a plain double, since
 *   MSVC's long double is a double.
 * - %s and %c take the function's own kind of string, %S and %C the other
 *   kind, and h/l/w pick narrow or wide explicitly. Wide means UTF-16,
 *   converted through the ANSI code page where the output is narrow.
 * - %p is eight uppercase hex digits with no 0x; a NULL %s is "(null)".
 * - %Z is a counted string: an ANSI_STRING, or with l or w a
 *   UNICODE_STRING.
 * - %e, %E, and %g in exponent form have three exponent digits.
 * - infinities and NaNs are 1.#INF, 1.#QNAN and 1.#IND (see put_special).
 * - an unknown conversion character is printed as itself.
 *
 * Integers and floats are still formatted by the host's snprintf, one
 * conversion at a time; strings, characters and padding are done here.
 */
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "runtime/builtin.h"
#include "win32/codepage.h"
#include "win32/crt.h"
#include "win32/internal.h"

#define CRTFN(ret) static ret STUB_CDECL

/* Output of the current kind: bytes, or WCHARs when wide. */
struct sink {
	int wide;
	BYTE *buf;
	size_t len, cap;	/* in characters */
	int failed;
};

static size_t unit(const struct sink *s)
{
	return s->wide ? sizeof(WCHAR) : 1;
}

static void *reserve(struct sink *s, size_t n)
{
	if (s->failed)
		return NULL;
	if (s->len + n + 1 > s->cap) {
		size_t cap = s->cap ? s->cap : 64;
		while (cap < s->len + n + 1)
			cap *= 2;
		BYTE *buf = realloc(s->buf, cap * unit(s));
		if (!buf) {
			s->failed = 1;
			return NULL;
		}
		s->buf = buf;
		s->cap = cap;
	}
	void *p = s->buf + s->len * unit(s);
	s->len += n;
	return p;
}

/* n characters of the sink's own kind. */
static void put(struct sink *s, const void *p, size_t n)
{
	void *d = reserve(s, n);
	if (d && n)
		memcpy(d, p, n * unit(s));
}

static void put_ascii(struct sink *s, const char *a, size_t n)
{
	if (!s->wide) {
		put(s, a, n);
		return;
	}
	WCHAR *d = reserve(s, n);
	for (size_t i = 0; d && i < n; i++)
		d[i] = (BYTE)a[i];
}

static void put_fill(struct sink *s, char c, size_t n)
{
	if (!s->wide) {
		BYTE *d = reserve(s, n);
		if (d)
			memset(d, c, n);
		return;
	}
	WCHAR *d = reserve(s, n);
	for (size_t i = 0; d && i < n; i++)
		d[i] = (BYTE)c;
}

/* n characters of text, narrow (ANSI) or wide, converted to the sink's. */
static void put_text(struct sink *s, const void *text, size_t n, int wide)
{
	if (wide == s->wide) {
		put(s, text, n);
		return;
	}
	if (!n)
		return;
	if (wide) {
		int an = cp_from_utf16(CP_ACP, text, (int)n, NULL, 0, '?', NULL);
		char *d = an > 0 ? reserve(s, (size_t)an) : NULL;
		if (d)
			cp_from_utf16(CP_ACP, text, (int)n, d, an, '?', NULL);
	} else {
		int wn = cp_to_utf16(CP_ACP, text, (int)n, NULL, 0, 0);
		WCHAR *d = wn > 0 ? reserve(s, (size_t)wn) : NULL;
		if (d)
			cp_to_utf16(CP_ACP, text, (int)n, d, wn, 0);
	}
}

/* How long the converted text will be, for padding. */
static size_t text_len(const struct sink *s, const void *text, size_t n,
		       int wide)
{
	if (wide == s->wide || !n)
		return n;
	int r = wide ? cp_from_utf16(CP_ACP, text, (int)n, NULL, 0, '?', NULL) :
		       cp_to_utf16(CP_ACP, text, (int)n, NULL, 0, 0);
	return r > 0 ? (size_t)r : 0;
}

struct spec {
	int left, plus, space, alt, zero;
	int width;		/* -1: none */
	int prec;		/* -1: none */
	enum { SZ_NONE, SZ_H, SZ_HH, SZ_L, SZ_64, SZ_32, SZ_W, SZ_LD } size;
	unsigned conv;
};

static void put_padded_text(struct sink *s, const struct spec *sp,
			    const void *text, size_t n, int wide)
{
	size_t len = text_len(s, text, n, wide);
	size_t pad = sp->width > 0 && (size_t)sp->width > len ?
		     (size_t)sp->width - len : 0;

	if (!sp->left)
		put_fill(s, sp->zero ? '0' : ' ', pad);
	put_text(s, text, n, wide);
	if (sp->left)
		put_fill(s, ' ', pad);
}

/* ASCII from the host's formatting, padded to the width here. */
static void put_padded_ascii(struct sink *s, const struct spec *sp,
			     const char *a, size_t n, int zero_ok)
{
	size_t pad = sp->width > 0 && (size_t)sp->width > n ?
		     (size_t)sp->width - n : 0;

	if (sp->left) {
		put_ascii(s, a, n);
		put_fill(s, ' ', pad);
	} else if (sp->zero && zero_ok) {
		/* Zeros go after a sign and a 0x prefix. */
		size_t lead = n && strchr("+- ", a[0]) ? 1 : 0;
		if (n >= lead + 2 && a[lead] == '0' &&
		    (a[lead + 1] == 'x' || a[lead + 1] == 'X'))
			lead += 2;
		put_ascii(s, a, lead);
		put_fill(s, '0', pad);
		put_ascii(s, a + lead, n - lead);
	} else {
		put_fill(s, ' ', pad);
		put_ascii(s, a, n);
	}
}

/* Runs the host's snprintf on one conversion, without its width. */
static char *host_format(const struct spec *sp, const char *size, char conv,
			 ...)
{
	char f[32], *p = f;
	va_list ap;

	*p++ = '%';
	if (sp->plus)
		*p++ = '+';
	if (sp->space)
		*p++ = ' ';
	if (sp->alt)
		*p++ = '#';
	if (sp->prec >= 0)
		p += sprintf(p, ".%d", sp->prec);
	p += sprintf(p, "%s%c", size, conv);

	va_start(ap, conv);
	int n = vsnprintf(NULL, 0, f, ap);
	va_end(ap);
	char *out = n >= 0 ? malloc((size_t)n + 2) : NULL;
	if (out) {
		va_start(ap, conv);
		vsnprintf(out, (size_t)n + 1, f, ap);
		va_end(ap);
	}
	return out;
}

/* e+05 -> e+005, as msvcrt.dll writes exponents. */
static void three_digit_exponent(char *a)
{
	char *e = strpbrk(a, "eE");

	if (!e || (e[1] != '+' && e[1] != '-'))
		return;
	char *d = e + 2;
	size_t digits = strspn(d, "0123456789");
	if (digits == 2 && !d[2]) {
		memmove(d + 1, d, 3);
		d[0] = '0';
	}
}

/*
 * A field as msvcrt pads its pieces: the sign (if any) and fill before
 * the text (zeros after the sign, spaces before it), or spaces after it
 * when left-aligned. The width left after the sign is also what the
 * right-hand fill counts against.
 */
static void put_field(struct sink *s, char sign, int width, int prec,
		      int left, int zero, const char *text)
{
	int len = (int)strlen(text);

	if (prec >= 0 && prec < len)
		len = prec;
	if (sign) {
		width--;
		if (zero)
			put_ascii(s, &sign, 1);
	}
	if (!left && width > len)
		put_fill(s, zero ? '0' : ' ', (size_t)(width - len));
	if (sign && !zero)
		put_ascii(s, &sign, 1);
	put_ascii(s, text, (size_t)len);
	if (left && width > len)
		put_fill(s, ' ', (size_t)(width - len));
}

/*
 * msvcrt's infinities and NaNs, after Wine's: 1.#INF, 1.#QNAN, or 1.#IND
 * for the negative NaN the FPU makes, the part after the point treated
 * as digits. So a precision cuts it, rounding the last one kept up (the
 * next is always 5 or more, as a character): "%.2f" gives 1.#J and
 * "%.1f" 1.$. Then come the zeros a number would have to reach its
 * precision, and %e's exponent: "%f" gives 1.#INF00, "%e" 1.#INF00e+000.
 * %g counts its precision one less (as msvcrt does) and adds no zeros.
 */
static void put_special(struct sink *s, struct spec sp, double v)
{
	char pfx[16], sfx[8] = "";
	char sign = signbit(v) ? '-' : sp.plus ? '+' : sp.space ? ' ' : 0;
	int g = sp.conv == 'g' || sp.conv == 'G';
	int prec = sp.prec, width = sp.width > 0 ? sp.width : 0;

	if (g)
		prec--;
	strcpy(pfx, isinf(v) ? "1.#INF" : sign == '-' ? "1.#IND" : "1.#QNAN");
	int pfx_len = (int)strlen(pfx), frac_len = pfx_len - 2;
	if (prec >= 0 && prec < frac_len)
		pfx[1 + prec]++;
	if (sp.conv == 'e' || sp.conv == 'E')
		strcpy(sfx, sp.conv == 'e' ? "e+000" : "E+000");

	int sfx_len = !sp.alt && g ? frac_len : prec;
	if (sfx_len < 0 && !g)
		sfx_len = 6;
	sfx_len += (int)strlen(sfx) - frac_len;
	if (sfx_len > 0)
		width -= sfx_len;
	if (prec >= 0)
		prec = (prec ? prec : -1) + pfx_len - frac_len;

	put_field(s, sign, width, prec, sp.left, sp.zero, pfx);
	put_field(s, 0, sfx_len, -1, sp.left, 1, sfx);
}

static unsigned fmt_at(const void *fmt, int wide, size_t i)
{
	return wide ? ((const WCHAR *)fmt)[i] : ((const BYTE *)fmt)[i];
}

static size_t wide_len(const WCHAR *w, int max)
{
	size_t n = 0;

	while ((max < 0 || n < (size_t)max) && w[n])
		n++;
	return n;
}

static void format(struct sink *s, const void *fmt, va_list *ap)
{
	int wide = s->wide;
	size_t i = 0;

	for (;;) {
		size_t start = i;
		while (fmt_at(fmt, wide, i) && fmt_at(fmt, wide, i) != '%')
			i++;
		put(s, (const BYTE *)fmt + start * unit(s), i - start);
		if (!fmt_at(fmt, wide, i))
			return;
		i++;	/* the % */

		struct spec sp = { .width = -1, .prec = -1 };
		unsigned c;
		for (;; i++) {
			c = fmt_at(fmt, wide, i);
			if (c == '-')
				sp.left = 1;
			else if (c == '+')
				sp.plus = 1;
			else if (c == ' ')
				sp.space = 1;
			else if (c == '#')
				sp.alt = 1;
			else if (c == '0')
				sp.zero = 1;
			else
				break;
		}
		if (c == '*') {
			sp.width = va_arg(*ap, int);
			if (sp.width < 0) {
				sp.left = 1;
				sp.width = -sp.width;
			}
			c = fmt_at(fmt, wide, ++i);
		} else {
			for (; c >= '0' && c <= '9'; c = fmt_at(fmt, wide, ++i))
				sp.width = (sp.width < 0 ? 0 : sp.width) * 10 +
					   (int)(c - '0');
		}
		if (c == '.') {
			c = fmt_at(fmt, wide, ++i);
			sp.prec = 0;
			if (c == '*') {
				sp.prec = va_arg(*ap, int);
				if (sp.prec < 0)
					sp.prec = -1;
				c = fmt_at(fmt, wide, ++i);
			} else {
				for (; c >= '0' && c <= '9';
				     c = fmt_at(fmt, wide, ++i))
					sp.prec = sp.prec * 10 + (int)(c - '0');
			}
		}
		if (c == 'I' && fmt_at(fmt, wide, i + 1) == '6' &&
		    fmt_at(fmt, wide, i + 2) == '4') {
			sp.size = SZ_64;
			i += 3;
		} else if (c == 'I' && fmt_at(fmt, wide, i + 1) == '3' &&
			   fmt_at(fmt, wide, i + 2) == '2') {
			sp.size = SZ_32;
			i += 3;
		} else if (c == 'I') {
			sp.size = SZ_32;
			i++;
		} else if (c == 'l' && fmt_at(fmt, wide, i + 1) == 'l') {
			sp.size = SZ_64;
			i += 2;
		} else if (c == 'h' && fmt_at(fmt, wide, i + 1) == 'h') {
			sp.size = SZ_HH;
			i += 2;
		} else if (c == 'h' || c == 'l' || c == 'w' || c == 'L') {
			sp.size = c == 'h' ? SZ_H : c == 'l' ? SZ_L :
				  c == 'w' ? SZ_W : SZ_LD;
			i++;
		}
		sp.conv = c = fmt_at(fmt, wide, i);
		if (!c)
			return;		/* a lone % at the end prints nothing */
		i++;

		switch (c) {
		case 'd': case 'i': case 'o': case 'u': case 'x': case 'X': {
			char conv = (char)c, *a;
			if (sp.size == SZ_64) {
				a = host_format(&sp, "ll", conv,
						va_arg(*ap, long long));
			} else {
				int v = va_arg(*ap, int);
				int is_signed = c == 'd' || c == 'i';
				if (sp.size == SZ_H)
					v = is_signed ? (short)v : (unsigned short)v;
				else if (sp.size == SZ_HH)
					v = is_signed ? (signed char)v : (unsigned char)v;
				a = host_format(&sp, "", conv, v);
			}
			if (!a) {
				s->failed = 1;
				return;
			}
			/* A precision turns zero padding off, as in C. */
			put_padded_ascii(s, &sp, a, strlen(a), sp.prec < 0);
			free(a);
			break;
		}
		case 'e': case 'E': case 'f': case 'g': case 'G':
		case 'a': case 'A': {
			double v = va_arg(*ap, double);
			if (!isfinite(v) && c != 'a' && c != 'A') {
				put_special(s, sp, v);
				break;
			}
			char *a = host_format(&sp, "", (char)c, v);
			if (!a) {
				s->failed = 1;
				return;
			}
			if (c != 'f' && c != 'a' && c != 'A')
				three_digit_exponent(a);
			int finite = !strpbrk(a, "iInN");
			put_padded_ascii(s, &sp, a, strlen(a), finite);
			free(a);
			break;
		}
		case 'p': {
			char a[16];
			snprintf(a, sizeof(a), "%08X",
				 (unsigned)(uintptr_t)va_arg(*ap, void *));
			sp.zero = 0;
			put_padded_ascii(s, &sp, a, strlen(a), 0);
			break;
		}
		case 'c': case 'C': {
			int w = sp.size == SZ_L || sp.size == SZ_W ? 1 :
				sp.size == SZ_H ? 0 : (c == 'C') != wide;
			int v = va_arg(*ap, int);
			BYTE b = (BYTE)v;
			WCHAR u = (WCHAR)v;
			sp.zero = 0;
			put_padded_text(s, &sp, w ? (const void *)&u : &b, 1, w);
			break;
		}
		case 's': case 'S': {
			int w = sp.size == SZ_L || sp.size == SZ_W ? 1 :
				sp.size == SZ_H ? 0 : (c == 'S') != wide;
			const void *str = va_arg(*ap, const void *);
			static const WCHAR null_w[] = {
				'(', 'n', 'u', 'l', 'l', ')', 0 };
			if (!str)
				str = w ? (const void *)null_w : "(null)";
			size_t n = w ? wide_len(str, sp.prec) :
				   sp.prec < 0 ? strlen(str) :
				   strnlen(str, (size_t)sp.prec);
			sp.zero = 0;
			put_padded_text(s, &sp, str, n, w);
			break;
		}
		case 'Z': {
			/* UNICODE_STRING and ANSI_STRING: a length in bytes. */
			const struct {
				USHORT len, max;
				const void *buf;
			} *str = va_arg(*ap, const void *);
			int w = sp.size == SZ_L || sp.size == SZ_W;
			sp.zero = 0;
			if (!str || !str->buf) {
				put_padded_text(s, &sp, "(null)", 6, 0);
				break;
			}
			size_t n = w ? str->len / sizeof(WCHAR) : str->len;
			if (sp.prec >= 0 && (size_t)sp.prec < n)
				n = (size_t)sp.prec;
			put_padded_text(s, &sp, str->buf, n, w);
			break;
		}
		case 'n': {
			/* Through a pointer as wide as the size prefix says. */
			void *p = va_arg(*ap, void *);
			if (sp.size == SZ_HH)
				*(signed char *)p = (signed char)s->len;
			else if (sp.size == SZ_H)
				*(short *)p = (short)s->len;
			else if (sp.size == SZ_64)
				*(long long *)p = (long long)s->len;
			else
				*(int *)p = (int)s->len;
			break;
		}
		default: {
			/* %% and anything unknown: the character itself. */
			if (wide) {
				WCHAR u = (WCHAR)c;
				put(s, &u, 1);
			} else {
				BYTE b = (BYTE)c;
				put(s, &b, 1);
			}
			break;
		}
		}
	}
}

int crt_vformat(int wide, const void *fmt, va_list ap, void **out,
		size_t *len)
{
	struct sink s = { .wide = wide };
	va_list copy;

	if (!fmt)
		return -1;
	va_copy(copy, ap);
	format(&s, fmt, &copy);
	va_end(copy);
	void *nul = reserve(&s, 1);
	if (s.failed || !nul) {
		free(s.buf);
		return -1;
	}
	s.len--;
	memset(nul, 0, unit(&s));
	*out = s.buf;
	*len = s.len;
	return 0;
}

/*
 * The string forms. _snprintf and _snwprintf: when the output is longer
 * than n they write n characters with no terminator and return -1; when it
 * is exactly n, no terminator either.
 */
static int to_buffer(int wide, void *buf, size_t n, int bounded,
		     const void *fmt, va_list ap)
{
	void *out;
	size_t len, u = wide ? sizeof(WCHAR) : 1;

	if (crt_vformat(wide, fmt, ap, &out, &len))
		return -1;
	if (!bounded) {
		memcpy(buf, out, (len + 1) * u);
	} else {
		size_t copy = len < n ? len + 1 : n;
		if (buf && copy)
			memcpy(buf, out, copy * u);
	}
	free(out);
	return !bounded || len <= n ? (int)len : -1;
}

static int count(int wide, const void *fmt, va_list ap)
{
	void *out;
	size_t len;

	if (crt_vformat(wide, fmt, ap, &out, &len))
		return -1;
	free(out);
	return (int)len;
}

CRTFN(int) crt_vsprintf(char *buf, const char *fmt, va_list ap)
{
	return to_buffer(0, buf, 0, 0, fmt, ap);
}

CRTFN(int) crt_sprintf(char *buf, const char *fmt, ...)
{
	va_list ap;
	va_start(ap, fmt);
	int r = to_buffer(0, buf, 0, 0, fmt, ap);
	va_end(ap);
	return r;
}

CRTFN(int) crt__vsnprintf(char *buf, size_t n, const char *fmt, va_list ap)
{
	return to_buffer(0, buf, n, 1, fmt, ap);
}

CRTFN(int) crt__snprintf(char *buf, size_t n, const char *fmt, ...)
{
	va_list ap;
	va_start(ap, fmt);
	int r = to_buffer(0, buf, n, 1, fmt, ap);
	va_end(ap);
	return r;
}

CRTFN(int) crt__vscprintf(const char *fmt, va_list ap)
{
	return count(0, fmt, ap);
}

CRTFN(int) crt__scprintf(const char *fmt, ...)
{
	va_list ap;
	va_start(ap, fmt);
	int r = count(0, fmt, ap);
	va_end(ap);
	return r;
}

/* msvcrt.dll's swprintf and vswprintf are the old forms, with no size. */
CRTFN(int) crt_vswprintf(WCHAR *buf, const WCHAR *fmt, va_list ap)
{
	return to_buffer(1, buf, 0, 0, fmt, ap);
}

CRTFN(int) crt_swprintf(WCHAR *buf, const WCHAR *fmt, ...)
{
	va_list ap;
	va_start(ap, fmt);
	int r = to_buffer(1, buf, 0, 0, fmt, ap);
	va_end(ap);
	return r;
}

CRTFN(int) crt__vsnwprintf(WCHAR *buf, size_t n, const WCHAR *fmt, va_list ap)
{
	return to_buffer(1, buf, n, 1, fmt, ap);
}

CRTFN(int) crt__snwprintf(WCHAR *buf, size_t n, const WCHAR *fmt, ...)
{
	va_list ap;
	va_start(ap, fmt);
	int r = to_buffer(1, buf, n, 1, fmt, ap);
	va_end(ap);
	return r;
}

CRTFN(int) crt__vscwprintf(const WCHAR *fmt, va_list ap)
{
	return count(1, fmt, ap);
}

CRTFN(int) crt__scwprintf(const WCHAR *fmt, ...)
{
	va_list ap;
	va_start(ap, fmt);
	int r = count(1, fmt, ap);
	va_end(ap);
	return r;
}

#define E(name) { #name, 0, (void *)crt_##name }

const struct builtin_export crt_printf_exports[] = {
	E(sprintf), E(vsprintf), E(_snprintf), E(_vsnprintf),
	E(_scprintf), E(_vscprintf),
	E(swprintf), E(vswprintf), E(_snwprintf), E(_vsnwprintf),
	E(_scwprintf), E(_vscwprintf),
	BUILTIN_END
};
