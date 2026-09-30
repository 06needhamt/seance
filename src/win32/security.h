/*
 * advapi32's security side (src/win32/adv_security.c), for the registry
 * functions in advapi32.c.
 */
#ifndef VFW_WIN32_SECURITY_H
#define VFW_WIN32_SECURITY_H

#include "runtime/builtin.h"
#include "win32.h"

/*
 * The self-relative security descriptor every object here has, with the
 * parts which asks for: owner Administrators, group None, and a DACL
 * giving SYSTEM, Administrators and the user full control and Users read
 * and execute. malloc()ed; *size is its length.
 */
void *sec_default_descriptor(SECURITY_INFORMATION which, DWORD *size);

/* Whether sd is a security descriptor (IsValidSecurityDescriptor). */
int sec_valid_descriptor(const void *sd);

/* The exports, merged into advapi32's. */
extern const struct builtin_export adv_security_exports[];

#endif /* VFW_WIN32_SECURITY_H */
