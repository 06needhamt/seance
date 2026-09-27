#include <stdio.h>

#include "common/fourcc.h"

const char *fourcc_str(FOURCC f, char buf[FOURCC_STR_SIZE])
{
	char *p = buf;

	for (int i = 0; i < 4; i++) {
		unsigned char c = (f >> (8 * i)) & 0xff;
		if (c >= 0x20 && c < 0x7f && c != '\\')
			*p++ = c;
		else
			p += sprintf(p, "\\x%02x", c);
	}
	*p = '\0';
	return buf;
}
