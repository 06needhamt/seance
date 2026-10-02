/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * Shared by the test fixture DLLs. They are built with -nostdlib, so they
 * link against nothing but each other; <windows.h> is used for types only.
 */
#ifndef VFW_FIXTURE_H
#define VFW_FIXTURE_H

#include <windows.h>

#define EXPORT __declspec(dllexport)

#endif
