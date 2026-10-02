/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * advapi32 registry definitions (winreg.h, winnt.h).
 */
#ifndef VFW_WIN32_WINREG_H
#define VFW_WIN32_WINREG_H

#include "types.h"

typedef DWORD REGSAM;
typedef HKEY *PHKEY;

/* The predefined keys. */
#define HKEY_CLASSES_ROOT     ((HKEY)(ULONG_PTR)((LONG)0x80000000))
#define HKEY_CURRENT_USER     ((HKEY)(ULONG_PTR)((LONG)0x80000001))
#define HKEY_LOCAL_MACHINE    ((HKEY)(ULONG_PTR)((LONG)0x80000002))
#define HKEY_USERS            ((HKEY)(ULONG_PTR)((LONG)0x80000003))
#define HKEY_PERFORMANCE_DATA ((HKEY)(ULONG_PTR)((LONG)0x80000004))
#define HKEY_CURRENT_CONFIG   ((HKEY)(ULONG_PTR)((LONG)0x80000005))
#define HKEY_DYN_DATA         ((HKEY)(ULONG_PTR)((LONG)0x80000006))

/* Access rights. */
#define KEY_QUERY_VALUE        0x0001
#define KEY_SET_VALUE          0x0002
#define KEY_CREATE_SUB_KEY     0x0004
#define KEY_ENUMERATE_SUB_KEYS 0x0008
#define KEY_NOTIFY             0x0010
#define KEY_CREATE_LINK        0x0020
#define KEY_WOW64_64KEY        0x0100
#define KEY_WOW64_32KEY        0x0200
#define KEY_READ               0x00020019
#define KEY_WRITE              0x00020006
#define KEY_EXECUTE            0x00020019
#define KEY_ALL_ACCESS         0x000f003f

/* Value types. */
#define REG_NONE                0
#define REG_SZ                  1
#define REG_EXPAND_SZ           2
#define REG_BINARY              3
#define REG_DWORD               4
#define REG_DWORD_LITTLE_ENDIAN 4
#define REG_DWORD_BIG_ENDIAN    5
#define REG_LINK                6
#define REG_MULTI_SZ            7
#define REG_RESOURCE_LIST       8
#define REG_QWORD               11
#define REG_QWORD_LITTLE_ENDIAN 11

/* RegCreateKeyEx. */
#define REG_OPTION_NON_VOLATILE 0x00000000
#define REG_OPTION_VOLATILE     0x00000001
#define REG_CREATED_NEW_KEY     0x00000001
#define REG_OPENED_EXISTING_KEY 0x00000002

/* RegGetValue: the types wanted, and how. */
#define RRF_RT_REG_NONE         0x00000001
#define RRF_RT_REG_SZ           0x00000002
#define RRF_RT_REG_EXPAND_SZ    0x00000004
#define RRF_RT_REG_BINARY       0x00000008
#define RRF_RT_REG_DWORD        0x00000010
#define RRF_RT_REG_MULTI_SZ     0x00000020
#define RRF_RT_REG_QWORD        0x00000040
#define RRF_RT_DWORD            (RRF_RT_REG_BINARY | RRF_RT_REG_DWORD)
#define RRF_RT_QWORD            (RRF_RT_REG_BINARY | RRF_RT_REG_QWORD)
#define RRF_RT_ANY              0x0000ffff
#define RRF_SUBKEY_WOW6464KEY   0x00010000
#define RRF_SUBKEY_WOW6432KEY   0x00020000
#define RRF_NOEXPAND            0x10000000
#define RRF_ZEROONFAILURE       0x20000000

/* RegNotifyChangeKeyValue. */
#define REG_NOTIFY_CHANGE_NAME       0x00000001
#define REG_NOTIFY_CHANGE_ATTRIBUTES 0x00000002
#define REG_NOTIFY_CHANGE_LAST_SET   0x00000004
#define REG_NOTIFY_CHANGE_SECURITY   0x00000008

#endif /* VFW_WIN32_WINREG_H */
