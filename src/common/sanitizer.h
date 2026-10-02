/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * VFW_SANITIZER_BUILD: 1 in AddressSanitizer builds, whose memory layout
 * and fault handling the harness leaves alone.
 */
#ifndef VFW_COMMON_SANITIZER_H
#define VFW_COMMON_SANITIZER_H

#if defined(__SANITIZE_ADDRESS__)
#define VFW_SANITIZER_BUILD 1
#elif defined(__has_feature)
#if __has_feature(address_sanitizer)
#define VFW_SANITIZER_BUILD 1
#endif
#endif
#ifndef VFW_SANITIZER_BUILD
#define VFW_SANITIZER_BUILD 0
#endif

#endif /* VFW_COMMON_SANITIZER_H */
