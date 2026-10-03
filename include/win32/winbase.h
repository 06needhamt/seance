/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * kernel32 structures and constants (winbase.h, winnls.h, winerror.h).
 */
#ifndef VFW_WIN32_WINBASE_H
#define VFW_WIN32_WINBASE_H

#include "types.h"

#define MAX_PATH 260
#define INFINITE 0xffffffff
#define INVALID_HANDLE_VALUE ((HANDLE)(LONG_PTR)-1)

/* Error codes (winerror.h). */
#define ERROR_SUCCESS                  0
#define NO_ERROR                       0
#define ERROR_INVALID_FUNCTION         1
#define ERROR_FILE_NOT_FOUND           2
#define ERROR_PATH_NOT_FOUND           3
#define ERROR_TOO_MANY_OPEN_FILES      4
#define ERROR_ACCESS_DENIED            5
#define ERROR_INVALID_HANDLE           6
#define ERROR_NOT_ENOUGH_MEMORY        8
#define ERROR_INVALID_DATA             13
#define ERROR_OUTOFMEMORY              14
#define ERROR_NO_MORE_FILES            18
#define ERROR_GEN_FAILURE              31
#define ERROR_SHARING_VIOLATION        32
#define ERROR_HANDLE_EOF               38
#define ERROR_NOT_SUPPORTED            50
#define ERROR_FILE_EXISTS              80
#define ERROR_CANNOT_MAKE              82
#define ERROR_INVALID_PARAMETER        87
#define ERROR_BROKEN_PIPE              109
#define ERROR_BUFFER_OVERFLOW          111
#define ERROR_DISK_FULL                112
#define ERROR_CALL_NOT_IMPLEMENTED     120
#define ERROR_SEM_TIMEOUT              121
#define ERROR_INSUFFICIENT_BUFFER      122
#define ERROR_INVALID_NAME             123
#define ERROR_MOD_NOT_FOUND            126
#define ERROR_PROC_NOT_FOUND           127
#define ERROR_NEGATIVE_SEEK            131
#define ERROR_DIR_NOT_EMPTY            145
#define ERROR_BAD_ARGUMENTS            160
#define ERROR_BAD_PATHNAME             161
#define ERROR_BUSY                     170
#define ERROR_ALREADY_EXISTS           183
#define ERROR_BAD_EXE_FORMAT           193
#define ERROR_ENVVAR_NOT_FOUND         203
#define ERROR_FILENAME_EXCED_RANGE     206
#define ERROR_MORE_DATA                234
#define ERROR_NO_MORE_ITEMS            259
#define ERROR_NOT_OWNER                288
#define ERROR_TOO_MANY_POSTS           298
#define ERROR_INVALID_ADDRESS          487
#define ERROR_NOACCESS                 998
#define ERROR_INVALID_FLAGS            1004
#define ERROR_NO_TOKEN                 1008
#define ERROR_BADDB                    1009
#define ERROR_BADKEY                   1010
#define ERROR_CANTOPEN                 1011
#define ERROR_CANTREAD                 1012
#define ERROR_CANTWRITE                1013
#define ERROR_KEY_DELETED              1018
#define ERROR_NO_UNICODE_TRANSLATION   1113
#define ERROR_DLL_INIT_FAILED          1114
#define ERROR_NO_ASSOCIATION           1155
#define ERROR_CANCELLED                1223
#define ERROR_NOT_ALL_ASSIGNED         1300
#define ERROR_SOME_NOT_MAPPED          1301
#define ERROR_UNKNOWN_REVISION         1305
#define ERROR_NO_IMPERSONATION_TOKEN   1309
#define ERROR_NO_SUCH_PRIVILEGE        1313
#define ERROR_NONE_MAPPED              1332
#define ERROR_INVALID_ACL              1336
#define ERROR_INVALID_SID              1337
#define ERROR_INVALID_SECURITY_DESCR   1338
#define ERROR_ALLOTTED_SPACE_EXCEEDED  1344
#define ERROR_BAD_IMPERSONATION_LEVEL  1346
#define ERROR_BAD_TOKEN_TYPE           1349
#define ERROR_INVALID_WINDOW_HANDLE    1400
#define ERROR_TLW_WITH_WSCHILD         1406
#define ERROR_CANNOT_FIND_WND_CLASS    1407
#define ERROR_CLASS_ALREADY_EXISTS     1410
#define ERROR_CLASS_DOES_NOT_EXIST     1411
#define ERROR_CLASS_HAS_WINDOWS        1412
#define ERROR_INVALID_INDEX            1413
#define ERROR_CONTROL_ID_NOT_FOUND     1421
#define ERROR_TIMEOUT                  1460
#define ERROR_DATATYPE_MISMATCH        1629
#define ERROR_UNSUPPORTED_TYPE         1630
#define ERROR_RESOURCE_DATA_NOT_FOUND  1812
#define ERROR_RESOURCE_TYPE_NOT_FOUND  1813
#define ERROR_RESOURCE_NAME_NOT_FOUND  1814
#define ERROR_RESOURCE_LANG_NOT_FOUND  1815

/* Waiting. */
#define WAIT_OBJECT_0      0
#define WAIT_ABANDONED     0x80
#define WAIT_ABANDONED_0   0x80
#define WAIT_IO_COMPLETION 0xc0
#define WAIT_TIMEOUT       0x102
#define WAIT_FAILED        0xffffffff
#define MAXIMUM_WAIT_OBJECTS 64

/* Virtual memory. */
#define PAGE_NOACCESS          0x01
#define PAGE_READONLY          0x02
#define PAGE_READWRITE         0x04
#define PAGE_WRITECOPY         0x08
#define PAGE_EXECUTE           0x10
#define PAGE_EXECUTE_READ      0x20
#define PAGE_EXECUTE_READWRITE 0x40
#define PAGE_EXECUTE_WRITECOPY 0x80
#define PAGE_GUARD             0x100
#define PAGE_NOCACHE           0x200
#define PAGE_WRITECOMBINE      0x400
#define MEM_COMMIT             0x1000
#define MEM_RESERVE            0x2000
#define MEM_DECOMMIT           0x4000
#define MEM_RELEASE            0x8000
#define MEM_FREE               0x10000
#define MEM_PRIVATE            0x20000
#define MEM_MAPPED             0x40000
#define MEM_RESET              0x80000
#define MEM_TOP_DOWN           0x100000
#define MEM_IMAGE              0x1000000

/* Heaps. */
#define HEAP_NO_SERIALIZE          0x00000001
#define HEAP_GROWABLE              0x00000002
#define HEAP_GENERATE_EXCEPTIONS   0x00000004
#define HEAP_ZERO_MEMORY           0x00000008
#define HEAP_REALLOC_IN_PLACE_ONLY 0x00000010
#define HEAP_CREATE_ENABLE_EXECUTE 0x00040000

/* Global and local memory. */
#define GMEM_FIXED          0x0000
#define GMEM_MOVEABLE       0x0002
#define GMEM_NOCOMPACT      0x0010
#define GMEM_NODISCARD      0x0020
#define GMEM_ZEROINIT       0x0040
#define GMEM_MODIFY         0x0080
#define GMEM_DISCARDABLE    0x0100
#define GMEM_DDESHARE       0x2000
#define GMEM_SHARE          0x2000
#define GMEM_INVALID_HANDLE 0x8000
#define GMEM_LOCKCOUNT      0x00ff
#define GPTR                0x0040
#define GHND                0x0042
#define LMEM_FIXED          0x0000
#define LMEM_MOVEABLE       0x0002
#define LMEM_ZEROINIT       0x0040
#define LMEM_MODIFY         0x0080
#define LMEM_INVALID_HANDLE 0x8000
#define LMEM_LOCKCOUNT      0x00ff
#define LPTR                0x0040
#define LHND                0x0042

/* Files. */
#define GENERIC_READ    0x80000000
#define GENERIC_WRITE   0x40000000
#define GENERIC_EXECUTE 0x20000000
#define GENERIC_ALL     0x10000000
#define FILE_SHARE_READ   0x1
#define FILE_SHARE_WRITE  0x2
#define FILE_SHARE_DELETE 0x4
#define CREATE_NEW        1
#define CREATE_ALWAYS     2
#define OPEN_EXISTING     3
#define OPEN_ALWAYS       4
#define TRUNCATE_EXISTING 5
#define FILE_ATTRIBUTE_READONLY  0x001
#define FILE_ATTRIBUTE_HIDDEN    0x002
#define FILE_ATTRIBUTE_SYSTEM    0x004
#define FILE_ATTRIBUTE_DIRECTORY 0x010
#define FILE_ATTRIBUTE_ARCHIVE   0x020
#define FILE_ATTRIBUTE_NORMAL    0x080
#define FILE_ATTRIBUTE_TEMPORARY 0x100
#define FILE_FLAG_WRITE_THROUGH   0x80000000
#define FILE_FLAG_OVERLAPPED      0x40000000
#define FILE_FLAG_NO_BUFFERING    0x20000000
#define FILE_FLAG_RANDOM_ACCESS   0x10000000
#define FILE_FLAG_SEQUENTIAL_SCAN 0x08000000
#define FILE_FLAG_DELETE_ON_CLOSE 0x04000000
#define FILE_BEGIN   0
#define FILE_CURRENT 1
#define FILE_END     2
#define INVALID_FILE_SIZE        0xffffffff
#define INVALID_SET_FILE_POINTER 0xffffffff
#define INVALID_FILE_ATTRIBUTES  0xffffffff
#define FILE_TYPE_UNKNOWN 0
#define FILE_TYPE_DISK    1
#define FILE_TYPE_CHAR    2
#define FILE_TYPE_PIPE    3
#define STD_INPUT_HANDLE  ((DWORD)-10)
#define STD_OUTPUT_HANDLE ((DWORD)-11)
#define STD_ERROR_HANDLE  ((DWORD)-12)
#define DUPLICATE_CLOSE_SOURCE 0x1
#define DUPLICATE_SAME_ACCESS  0x2

/* File mappings. */
#define FILE_MAP_COPY       0x0001
#define FILE_MAP_WRITE      0x0002
#define FILE_MAP_READ       0x0004
#define FILE_MAP_EXECUTE    0x0020
#define FILE_MAP_ALL_ACCESS 0xf001f

/* Processes, threads, modules. */
#define CREATE_SUSPENDED 0x4
#define STACK_SIZE_PARAM_IS_A_RESERVATION 0x10000
#define STILL_ACTIVE 0x103
#define THREAD_PRIORITY_LOWEST        (-2)
#define THREAD_PRIORITY_BELOW_NORMAL  (-1)
#define THREAD_PRIORITY_NORMAL        0
#define THREAD_PRIORITY_HIGHEST       2
#define THREAD_PRIORITY_ABOVE_NORMAL  1
#define THREAD_PRIORITY_TIME_CRITICAL 15
#define THREAD_PRIORITY_IDLE          (-15)
#define THREAD_PRIORITY_ERROR_RETURN  0x7fffffff
#define TLS_OUT_OF_INDEXES 0xffffffff
#define FLS_OUT_OF_INDEXES 0xffffffff
#define DONT_RESOLVE_DLL_REFERENCES   0x1
#define LOAD_LIBRARY_AS_DATAFILE      0x2
#define LOAD_WITH_ALTERED_SEARCH_PATH 0x8
#define GET_MODULE_HANDLE_EX_FLAG_PIN                0x1
#define GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT 0x2
#define GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS       0x4
#define SEM_FAILCRITICALERRORS     0x0001
#define SEM_NOGPFAULTERRORBOX      0x0002
#define SEM_NOALIGNMENTFAULTEXCEPT 0x0004
#define SEM_NOOPENFILEERRORBOX     0x8000

/* System information. */
#define VER_PLATFORM_WIN32s        0
#define VER_PLATFORM_WIN32_WINDOWS 1
#define VER_PLATFORM_WIN32_NT      2
#define PROCESSOR_ARCHITECTURE_INTEL 0
#define PROCESSOR_INTEL_PENTIUM      586
#define PF_FLOATING_POINT_PRECISION_ERRATA 0
#define PF_FLOATING_POINT_EMULATED         1
#define PF_COMPARE_EXCHANGE_DOUBLE         2
#define PF_MMX_INSTRUCTIONS_AVAILABLE      3
#define PF_XMMI_INSTRUCTIONS_AVAILABLE     6
#define PF_3DNOW_INSTRUCTIONS_AVAILABLE    7
#define PF_RDTSC_INSTRUCTION_AVAILABLE     8
#define PF_PAE_ENABLED                     9
#define PF_XMMI64_INSTRUCTIONS_AVAILABLE   10
#define PF_NX_ENABLED                      12
#define PF_SSE3_INSTRUCTIONS_AVAILABLE     13
#define TIME_ZONE_ID_UNKNOWN  0
#define TIME_ZONE_ID_STANDARD 1
#define TIME_ZONE_ID_INVALID  0xffffffff

/* FormatMessage. */
#define FORMAT_MESSAGE_ALLOCATE_BUFFER 0x00000100
#define FORMAT_MESSAGE_IGNORE_INSERTS  0x00000200
#define FORMAT_MESSAGE_FROM_STRING     0x00000400
#define FORMAT_MESSAGE_FROM_HMODULE    0x00000800
#define FORMAT_MESSAGE_FROM_SYSTEM     0x00001000
#define FORMAT_MESSAGE_ARGUMENT_ARRAY  0x00002000
#define FORMAT_MESSAGE_MAX_WIDTH_MASK  0x000000ff

/* Code pages and strings (winnls.h). */
#define CP_ACP        0
#define CP_OEMCP      1
#define CP_MACCP      2
#define CP_THREAD_ACP 3
#define CP_UTF7       65000
#define CP_UTF8       65001
#define MB_PRECOMPOSED       0x01
#define MB_COMPOSITE         0x02
#define MB_USEGLYPHCHARS     0x04
#define MB_ERR_INVALID_CHARS 0x08
#define WC_COMPOSITECHECK    0x200
#define WC_DEFAULTCHAR       0x040
#define WC_NO_BEST_FIT_CHARS 0x400
#define MAX_DEFAULTCHAR 2
#define MAX_LEADBYTES   12
#define CSTR_LESS_THAN    1
#define CSTR_EQUAL        2
#define CSTR_GREATER_THAN 3
#define NORM_IGNORECASE     0x00000001
#define NORM_IGNORENONSPACE 0x00000002
#define NORM_IGNORESYMBOLS  0x00000004
#define LCMAP_LOWERCASE 0x00000100
#define LCMAP_UPPERCASE 0x00000200
#define CT_CTYPE1 0x1
#define CT_CTYPE2 0x2
#define CT_CTYPE3 0x4
#define C1_UPPER  0x0001
#define C1_LOWER  0x0002
#define C1_DIGIT  0x0004
#define C1_SPACE  0x0008
#define C1_PUNCT  0x0010
#define C1_CNTRL  0x0020
#define C1_BLANK  0x0040
#define C1_XDIGIT 0x0080
#define C1_ALPHA  0x0100
#define LOCALE_NOUSEROVERRIDE      0x80000000
#define LOCALE_USE_CP_ACP          0x40000000
#define LOCALE_RETURN_NUMBER       0x20000000
#define LOCALE_ILANGUAGE           0x0001
#define LOCALE_SLANGUAGE           0x0002
#define LOCALE_SABBREVLANGNAME     0x0003
#define LOCALE_SNATIVELANGNAME     0x0004
#define LOCALE_ICOUNTRY            0x0005
#define LOCALE_SCOUNTRY            0x0006
#define LOCALE_SABBREVCTRYNAME     0x0007
#define LOCALE_IDEFAULTLANGUAGE    0x0009
#define LOCALE_IDEFAULTCOUNTRY     0x000a
#define LOCALE_IDEFAULTCODEPAGE    0x000b
#define LOCALE_SLIST               0x000c
#define LOCALE_IMEASURE            0x000d
#define LOCALE_SDECIMAL            0x000e
#define LOCALE_STHOUSAND           0x000f
#define LOCALE_SGROUPING           0x0010
#define LOCALE_IDIGITS             0x0011
#define LOCALE_ILZERO              0x0012
#define LOCALE_SNATIVEDIGITS       0x0013
#define LOCALE_SCURRENCY           0x0014
#define LOCALE_SINTLSYMBOL         0x0015
#define LOCALE_SMONDECIMALSEP      0x0016
#define LOCALE_SMONTHOUSANDSEP     0x0017
#define LOCALE_SMONGROUPING        0x0018
#define LOCALE_ICURRDIGITS         0x0019
#define LOCALE_ICURRENCY           0x001b
#define LOCALE_INEGCURR            0x001c
#define LOCALE_SDATE               0x001d
#define LOCALE_STIME               0x001e
#define LOCALE_SSHORTDATE          0x001f
#define LOCALE_SLONGDATE           0x0020
#define LOCALE_SENGLANGUAGE        0x1001
#define LOCALE_SENGCOUNTRY         0x1002
#define LOCALE_IDEFAULTANSICODEPAGE 0x1004
#define LOCALE_SISO639LANGNAME     0x0059
#define LOCALE_SISO3166CTRYNAME    0x005a
#define LOCALE_USER_DEFAULT   0x0400
#define LOCALE_SYSTEM_DEFAULT 0x0800
#define LOCALE_INVARIANT      0x007f

typedef struct _SECURITY_ATTRIBUTES {
	DWORD nLength;
	LPVOID lpSecurityDescriptor;
	BOOL bInheritHandle;
} SECURITY_ATTRIBUTES, *PSECURITY_ATTRIBUTES, *LPSECURITY_ATTRIBUTES;

typedef struct _OVERLAPPED {
	ULONG_PTR Internal;
	ULONG_PTR InternalHigh;
	union {
		struct {
			DWORD Offset;
			DWORD OffsetHigh;
		};
		PVOID Pointer;
	};
	HANDLE hEvent;
} OVERLAPPED, *LPOVERLAPPED;

typedef struct _SYSTEMTIME {
	WORD wYear;
	WORD wMonth;
	WORD wDayOfWeek;
	WORD wDay;
	WORD wHour;
	WORD wMinute;
	WORD wSecond;
	WORD wMilliseconds;
} SYSTEMTIME, *PSYSTEMTIME, *LPSYSTEMTIME;

typedef struct _TIME_ZONE_INFORMATION {
	LONG Bias;
	WCHAR StandardName[32];
	SYSTEMTIME StandardDate;
	LONG StandardBias;
	WCHAR DaylightName[32];
	SYSTEMTIME DaylightDate;
	LONG DaylightBias;
} TIME_ZONE_INFORMATION, *LPTIME_ZONE_INFORMATION;

typedef struct _SYSTEM_INFO {
	union {
		DWORD dwOemId;
		struct {
			WORD wProcessorArchitecture;
			WORD wReserved;
		};
	};
	DWORD dwPageSize;
	LPVOID lpMinimumApplicationAddress;
	LPVOID lpMaximumApplicationAddress;
	DWORD_PTR dwActiveProcessorMask;
	DWORD dwNumberOfProcessors;
	DWORD dwProcessorType;
	DWORD dwAllocationGranularity;
	WORD wProcessorLevel;
	WORD wProcessorRevision;
} SYSTEM_INFO, *LPSYSTEM_INFO;

typedef struct _OSVERSIONINFOA {
	DWORD dwOSVersionInfoSize;
	DWORD dwMajorVersion;
	DWORD dwMinorVersion;
	DWORD dwBuildNumber;
	DWORD dwPlatformId;
	CHAR szCSDVersion[128];
} OSVERSIONINFOA, *LPOSVERSIONINFOA;

typedef struct _OSVERSIONINFOW {
	DWORD dwOSVersionInfoSize;
	DWORD dwMajorVersion;
	DWORD dwMinorVersion;
	DWORD dwBuildNumber;
	DWORD dwPlatformId;
	WCHAR szCSDVersion[128];
} OSVERSIONINFOW, *LPOSVERSIONINFOW;

typedef struct _OSVERSIONINFOEXA {
	DWORD dwOSVersionInfoSize;
	DWORD dwMajorVersion;
	DWORD dwMinorVersion;
	DWORD dwBuildNumber;
	DWORD dwPlatformId;
	CHAR szCSDVersion[128];
	WORD wServicePackMajor;
	WORD wServicePackMinor;
	WORD wSuiteMask;
	BYTE wProductType;
	BYTE wReserved;
} OSVERSIONINFOEXA;

typedef struct _OSVERSIONINFOEXW {
	DWORD dwOSVersionInfoSize;
	DWORD dwMajorVersion;
	DWORD dwMinorVersion;
	DWORD dwBuildNumber;
	DWORD dwPlatformId;
	WCHAR szCSDVersion[128];
	WORD wServicePackMajor;
	WORD wServicePackMinor;
	WORD wSuiteMask;
	BYTE wProductType;
	BYTE wReserved;
} OSVERSIONINFOEXW;

typedef struct _MEMORYSTATUS {
	DWORD dwLength;
	DWORD dwMemoryLoad;
	SIZE_T dwTotalPhys;
	SIZE_T dwAvailPhys;
	SIZE_T dwTotalPageFile;
	SIZE_T dwAvailPageFile;
	SIZE_T dwTotalVirtual;
	SIZE_T dwAvailVirtual;
} MEMORYSTATUS, *LPMEMORYSTATUS;

typedef struct _MEMORYSTATUSEX {
	DWORD dwLength;
	DWORD dwMemoryLoad;
	DWORDLONG ullTotalPhys;
	DWORDLONG ullAvailPhys;
	DWORDLONG ullTotalPageFile;
	DWORDLONG ullAvailPageFile;
	DWORDLONG ullTotalVirtual;
	DWORDLONG ullAvailVirtual;
	DWORDLONG ullAvailExtendedVirtual;
} MEMORYSTATUSEX, *LPMEMORYSTATUSEX;

typedef struct _MEMORY_BASIC_INFORMATION {
	PVOID BaseAddress;
	PVOID AllocationBase;
	DWORD AllocationProtect;
	SIZE_T RegionSize;
	DWORD State;
	DWORD Protect;
	DWORD Type;
} MEMORY_BASIC_INFORMATION, *PMEMORY_BASIC_INFORMATION;

typedef struct _RTL_CRITICAL_SECTION {
	PVOID DebugInfo;
	LONG LockCount;
	LONG RecursionCount;
	HANDLE OwningThread;
	HANDLE LockSemaphore;
	ULONG_PTR SpinCount;
} CRITICAL_SECTION, *LPCRITICAL_SECTION, RTL_CRITICAL_SECTION;

typedef struct _STARTUPINFOA {
	DWORD cb;
	LPSTR lpReserved;
	LPSTR lpDesktop;
	LPSTR lpTitle;
	DWORD dwX;
	DWORD dwY;
	DWORD dwXSize;
	DWORD dwYSize;
	DWORD dwXCountChars;
	DWORD dwYCountChars;
	DWORD dwFillAttribute;
	DWORD dwFlags;
	WORD wShowWindow;
	WORD cbReserved2;
	LPBYTE lpReserved2;
	HANDLE hStdInput;
	HANDLE hStdOutput;
	HANDLE hStdError;
} STARTUPINFOA, *LPSTARTUPINFOA;

typedef struct _STARTUPINFOW {
	DWORD cb;
	LPWSTR lpReserved;
	LPWSTR lpDesktop;
	LPWSTR lpTitle;
	DWORD dwX;
	DWORD dwY;
	DWORD dwXSize;
	DWORD dwYSize;
	DWORD dwXCountChars;
	DWORD dwYCountChars;
	DWORD dwFillAttribute;
	DWORD dwFlags;
	WORD wShowWindow;
	WORD cbReserved2;
	LPBYTE lpReserved2;
	HANDLE hStdInput;
	HANDLE hStdOutput;
	HANDLE hStdError;
} STARTUPINFOW, *LPSTARTUPINFOW;

typedef struct _cpinfo {
	UINT MaxCharSize;
	BYTE DefaultChar[MAX_DEFAULTCHAR];
	BYTE LeadByte[MAX_LEADBYTES];
} CPINFO, *LPCPINFO;

typedef struct _WIN32_FIND_DATAA {
	DWORD dwFileAttributes;
	FILETIME ftCreationTime;
	FILETIME ftLastAccessTime;
	FILETIME ftLastWriteTime;
	DWORD nFileSizeHigh;
	DWORD nFileSizeLow;
	DWORD dwReserved0;
	DWORD dwReserved1;
	CHAR cFileName[MAX_PATH];
	CHAR cAlternateFileName[14];
} WIN32_FIND_DATAA, *LPWIN32_FIND_DATAA;

typedef struct _WIN32_FIND_DATAW {
	DWORD dwFileAttributes;
	FILETIME ftCreationTime;
	FILETIME ftLastAccessTime;
	FILETIME ftLastWriteTime;
	DWORD nFileSizeHigh;
	DWORD nFileSizeLow;
	DWORD dwReserved0;
	DWORD dwReserved1;
	WCHAR cFileName[MAX_PATH];
	WCHAR cAlternateFileName[14];
} WIN32_FIND_DATAW, *LPWIN32_FIND_DATAW;

typedef DWORD (WINAPI *LPTHREAD_START_ROUTINE)(LPVOID param);
typedef void (WINAPI *PFLS_CALLBACK_FUNCTION)(PVOID data);

#endif /* VFW_WIN32_WINBASE_H */
