/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * gen_tlb PTR OUT: writes test_typelib's library (see tlb_writer.h) for
 * Win32 (PTR 4) or Win64 (PTR 8) to OUT, for pe_typelib.dll's TYPELIB
 * resource. A host program: there is no midl or widl here.
 */
#include <stdio.h>

#include "tlb_writer.h"

int main(int argc, char **argv)
{
	struct buf b;
	FILE *f;
	int ptr;

	if (argc != 3 || ((ptr = atoi(argv[1])) != 4 && ptr != 8)) {
		fprintf(stderr, "usage: gen_tlb 4|8 out.tlb\n");
		return 2;
	}
	b = build_library(ptr);
	if (!(f = fopen(argv[2], "wb")) || fwrite(b.d, 1, b.len, f) != b.len ||
	    fclose(f)) {
		perror(argv[2]);
		return 1;
	}
	free(b.d);
	return 0;
}
