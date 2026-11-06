/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * msvcrt: the names of types as type_info::name() gives them, from the
 * decorated ones of type descriptors (".?AVfoo@@" is class foo, ".H" int).
 * Shared by the i386 and x86-64 C++ runtimes.
 */
#include <stdlib.h>
#include <string.h>

struct out {
	char buf[256];
	size_t n;
	int bad;
};

static void put(struct out *o, const char *s, size_t len)
{
	if (o->n + len >= sizeof(o->buf)) {
		o->bad = 1;
		return;
	}
	memcpy(o->buf + o->n, s, len);
	o->n += len;
	o->buf[o->n] = '\0';
}

static void puts_(struct out *o, const char *s)
{
	put(o, s, strlen(s));
}

/* A qualified name, "B@A@@" -> "A::B"; advances *p past it. */
static void qualified(struct out *o, const char **p)
{
	const char *parts[8];
	size_t lens[8];
	int n = 0;

	while (**p && **p != '@') {
		const char *s = *p;
		if (*s == '?' || (*s >= '0' && *s <= '9')) {
			o->bad = 1;	/* templates, back references */
			return;
		}
		while (**p && **p != '@')
			(*p)++;
		if (!**p || n == 8) {
			o->bad = 1;
			return;
		}
		parts[n] = s;
		lens[n++] = (size_t)(*p - s);
		(*p)++;
	}
	if (**p != '@' || !n) {
		o->bad = 1;
		return;
	}
	(*p)++;
	for (int i = n - 1; i >= 0; i--) {
		put(o, parts[i], lens[i]);
		if (i)
			puts_(o, "::");
	}
}

/* A type in MSVC's mangling, the common forms; o->bad if not one. */
static void type_name(struct out *o, const char **p)
{
	static const struct { char c; const char *name; } basic[] = {
		{ 'C', "signed char" }, { 'D', "char" }, { 'E', "unsigned char" },
		{ 'F', "short" }, { 'G', "unsigned short" }, { 'H', "int" },
		{ 'I', "unsigned int" }, { 'J', "long" }, { 'K', "unsigned long" },
		{ 'M', "float" }, { 'N', "double" }, { 'O', "long double" },
		{ 'X', "void" },
	};
	static const struct { char c; const char *name; } ext[] = {
		{ 'N', "bool" }, { 'J', "__int64" }, { 'K', "unsigned __int64" },
		{ 'W', "wchar_t" },
	};
	char c = *(*p)++;

	switch (c) {
	case 'V': puts_(o, "class "); qualified(o, p); return;
	case 'U': puts_(o, "struct "); qualified(o, p); return;
	case 'T': puts_(o, "union "); qualified(o, p); return;
	case 'W':
		if (*(*p)++ != '4') {
			o->bad = 1;
			return;
		}
		puts_(o, "enum ");
		qualified(o, p);
		return;
	case 'P':	/* pointer: A plain, B const, C volatile, D both */
	case 'Q': {
		if (**p == 'E')		/* __ptr64, on x86-64 */
			(*p)++;
		char q = *(*p)++;
		if (q < 'A' || q > 'D') {
			o->bad = 1;
			return;
		}
		type_name(o, p);
		if (q == 'B' || q == 'D')
			puts_(o, " const");
		if (q == 'C' || q == 'D')
			puts_(o, " volatile");
		puts_(o, " *");
		return;
	}
	case '_':
		c = *(*p)++;
		for (size_t i = 0; i < sizeof(ext) / sizeof(ext[0]); i++)
			if (ext[i].c == c) {
				puts_(o, ext[i].name);
				return;
			}
		break;
	default:
		for (size_t i = 0; i < sizeof(basic) / sizeof(basic[0]); i++)
			if (basic[i].c == c) {
				puts_(o, basic[i].name);
				return;
			}
	}
	o->bad = 1;
}

/*
 * The undecorated name of a type descriptor's ".?AVfoo@@" (class foo) or
 * ".H" (int); unknown forms give the decorated name without its dot.
 */
char *crt_undecorate_type(const char *mangled)
{
	struct out o = { .n = 0 };
	const char *p = mangled;

	if (*p == '.')
		p++;
	if (p[0] == '?' && p[1] == 'A')
		p += 2;
	type_name(&o, &p);
	return strdup(o.bad || *p ? (mangled[0] == '.' ? mangled + 1 : mangled) :
		      o.buf);
}

