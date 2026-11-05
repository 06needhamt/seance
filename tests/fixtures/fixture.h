/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * Shared by the test fixture DLLs. They are built with -nostdlib, so they
 * link against nothing but each other; <windows.h> is used for types only.
 */
#ifndef VFW_FIXTURE_H
#define VFW_FIXTURE_H

#include <windows.h>

#define EXPORT __declspec(dllexport)

#ifdef _WIN64
/*
 * x86-64 code reaches its own data RIP-relative, so a small DLL can have
 * nothing for the loader to relocate, and GNU ld then leaves out .reloc
 * altogether, which pins it to its base. One absolute pointer gives every
 * 64-bit fixture a relocation table, as MSVC-built DLLs always have, so
 * fixtures sharing a base can load together. (pe_noreloc strips it.)
 */
static void *const volatile fixture_self __attribute__((used)) =
	(void *)&fixture_self;
#endif

#endif
