/*
 * PE32 image format: the on-disk structures and constants the loader uses.
 */
#ifndef VFW_WIN32_WINNT_H
#define VFW_WIN32_WINNT_H

#include "types.h"

/* Signatures. */
#define IMAGE_DOS_SIGNATURE 0x5A4D      /* "MZ" */
#define IMAGE_NT_SIGNATURE  0x00004550  /* "PE\0\0" */

#define IMAGE_FILE_MACHINE_I386 0x014c

/* IMAGE_FILE_HEADER.Characteristics. */
#define IMAGE_FILE_RELOCS_STRIPPED     0x0001
#define IMAGE_FILE_EXECUTABLE_IMAGE    0x0002
#define IMAGE_FILE_LARGE_ADDRESS_AWARE 0x0020
#define IMAGE_FILE_32BIT_MACHINE       0x0100
#define IMAGE_FILE_DEBUG_STRIPPED      0x0200
#define IMAGE_FILE_DLL                 0x2000

#define IMAGE_NT_OPTIONAL_HDR32_MAGIC 0x10b

/* IMAGE_OPTIONAL_HEADER.Subsystem. */
#define IMAGE_SUBSYSTEM_UNKNOWN     0
#define IMAGE_SUBSYSTEM_NATIVE      1
#define IMAGE_SUBSYSTEM_WINDOWS_GUI 2
#define IMAGE_SUBSYSTEM_WINDOWS_CUI 3

/* IMAGE_OPTIONAL_HEADER.DllCharacteristics. */
#define IMAGE_DLLCHARACTERISTICS_DYNAMIC_BASE          0x0040
#define IMAGE_DLLCHARACTERISTICS_FORCE_INTEGRITY       0x0080
#define IMAGE_DLLCHARACTERISTICS_NX_COMPAT             0x0100
#define IMAGE_DLLCHARACTERISTICS_NO_ISOLATION          0x0200
#define IMAGE_DLLCHARACTERISTICS_NO_SEH                0x0400
#define IMAGE_DLLCHARACTERISTICS_NO_BIND               0x0800
#define IMAGE_DLLCHARACTERISTICS_WDM_DRIVER            0x2000
#define IMAGE_DLLCHARACTERISTICS_TERMINAL_SERVER_AWARE 0x8000

/* Data directory indices. */
#define IMAGE_NUMBEROF_DIRECTORY_ENTRIES     16
#define IMAGE_DIRECTORY_ENTRY_EXPORT         0
#define IMAGE_DIRECTORY_ENTRY_IMPORT         1
#define IMAGE_DIRECTORY_ENTRY_RESOURCE       2
#define IMAGE_DIRECTORY_ENTRY_EXCEPTION      3
#define IMAGE_DIRECTORY_ENTRY_SECURITY       4
#define IMAGE_DIRECTORY_ENTRY_BASERELOC      5
#define IMAGE_DIRECTORY_ENTRY_DEBUG          6
#define IMAGE_DIRECTORY_ENTRY_ARCHITECTURE   7
#define IMAGE_DIRECTORY_ENTRY_GLOBALPTR      8
#define IMAGE_DIRECTORY_ENTRY_TLS            9
#define IMAGE_DIRECTORY_ENTRY_LOAD_CONFIG    10
#define IMAGE_DIRECTORY_ENTRY_BOUND_IMPORT   11
#define IMAGE_DIRECTORY_ENTRY_IAT            12
#define IMAGE_DIRECTORY_ENTRY_DELAY_IMPORT   13
#define IMAGE_DIRECTORY_ENTRY_COM_DESCRIPTOR 14

/* IMAGE_SECTION_HEADER.Characteristics. */
#define IMAGE_SIZEOF_SHORT_NAME          8
#define IMAGE_SCN_CNT_CODE               0x00000020
#define IMAGE_SCN_CNT_INITIALIZED_DATA   0x00000040
#define IMAGE_SCN_CNT_UNINITIALIZED_DATA 0x00000080
#define IMAGE_SCN_MEM_DISCARDABLE        0x02000000
#define IMAGE_SCN_MEM_NOT_CACHED         0x04000000
#define IMAGE_SCN_MEM_NOT_PAGED          0x08000000
#define IMAGE_SCN_MEM_SHARED             0x10000000
#define IMAGE_SCN_MEM_EXECUTE            0x20000000
#define IMAGE_SCN_MEM_READ               0x40000000
#define IMAGE_SCN_MEM_WRITE              0x80000000

/* Base relocation types (high 4 bits of each entry). */
#define IMAGE_REL_BASED_ABSOLUTE 0
#define IMAGE_REL_BASED_HIGH     1
#define IMAGE_REL_BASED_LOW      2
#define IMAGE_REL_BASED_HIGHLOW  3
#define IMAGE_REL_BASED_HIGHADJ  4

#define IMAGE_ORDINAL_FLAG32 0x80000000
#define IMAGE_ORDINAL32(o)   ((o) & 0xffff)

#define IMAGE_RESOURCE_NAME_IS_STRING    0x80000000
#define IMAGE_RESOURCE_DATA_IS_DIRECTORY 0x80000000

/* DllMain reasons. */
#define DLL_PROCESS_DETACH 0
#define DLL_PROCESS_ATTACH 1
#define DLL_THREAD_ATTACH  2
#define DLL_THREAD_DETACH  3

/* Resource types (winuser.h on Windows, where they are MAKEINTRESOURCE ids). */
#define RT_CURSOR       1
#define RT_BITMAP       2
#define RT_ICON         3
#define RT_MENU         4
#define RT_DIALOG       5
#define RT_STRING       6
#define RT_FONTDIR      7
#define RT_FONT         8
#define RT_ACCELERATOR  9
#define RT_RCDATA       10
#define RT_MESSAGETABLE 11
#define RT_GROUP_CURSOR 12
#define RT_GROUP_ICON   14
#define RT_VERSION      16
#define RT_DLGINCLUDE   17
#define RT_PLUGPLAY     19
#define RT_VXD          20
#define RT_ANICURSOR    21
#define RT_ANIICON      22
#define RT_HTML         23
#define RT_MANIFEST     24

/* Windows packs the DOS header to 2 bytes (pshpack2.h). */
#pragma pack(push, 2)
typedef struct _IMAGE_DOS_HEADER {
	WORD e_magic;
	WORD e_cblp;
	WORD e_cp;
	WORD e_crlc;
	WORD e_cparhdr;
	WORD e_minalloc;
	WORD e_maxalloc;
	WORD e_ss;
	WORD e_sp;
	WORD e_csum;
	WORD e_ip;
	WORD e_cs;
	WORD e_lfarlc;
	WORD e_ovno;
	WORD e_res[4];
	WORD e_oemid;
	WORD e_oeminfo;
	WORD e_res2[10];
	LONG e_lfanew;
} IMAGE_DOS_HEADER;
#pragma pack(pop)

typedef struct _IMAGE_FILE_HEADER {
	WORD Machine;
	WORD NumberOfSections;
	DWORD TimeDateStamp;
	DWORD PointerToSymbolTable;
	DWORD NumberOfSymbols;
	WORD SizeOfOptionalHeader;
	WORD Characteristics;
} IMAGE_FILE_HEADER;

typedef struct _IMAGE_DATA_DIRECTORY {
	DWORD VirtualAddress;
	DWORD Size;
} IMAGE_DATA_DIRECTORY;

typedef struct _IMAGE_OPTIONAL_HEADER {
	WORD Magic;
	BYTE MajorLinkerVersion;
	BYTE MinorLinkerVersion;
	DWORD SizeOfCode;
	DWORD SizeOfInitializedData;
	DWORD SizeOfUninitializedData;
	DWORD AddressOfEntryPoint;
	DWORD BaseOfCode;
	DWORD BaseOfData;
	DWORD ImageBase;
	DWORD SectionAlignment;
	DWORD FileAlignment;
	WORD MajorOperatingSystemVersion;
	WORD MinorOperatingSystemVersion;
	WORD MajorImageVersion;
	WORD MinorImageVersion;
	WORD MajorSubsystemVersion;
	WORD MinorSubsystemVersion;
	DWORD Win32VersionValue;
	DWORD SizeOfImage;
	DWORD SizeOfHeaders;
	DWORD CheckSum;
	WORD Subsystem;
	WORD DllCharacteristics;
	DWORD SizeOfStackReserve;
	DWORD SizeOfStackCommit;
	DWORD SizeOfHeapReserve;
	DWORD SizeOfHeapCommit;
	DWORD LoaderFlags;
	DWORD NumberOfRvaAndSizes;
	IMAGE_DATA_DIRECTORY DataDirectory[IMAGE_NUMBEROF_DIRECTORY_ENTRIES];
} IMAGE_OPTIONAL_HEADER32;

typedef struct _IMAGE_NT_HEADERS {
	DWORD Signature;
	IMAGE_FILE_HEADER FileHeader;
	IMAGE_OPTIONAL_HEADER32 OptionalHeader;
} IMAGE_NT_HEADERS32;

typedef struct _IMAGE_SECTION_HEADER {
	BYTE Name[IMAGE_SIZEOF_SHORT_NAME];
	union {
		DWORD PhysicalAddress;
		DWORD VirtualSize;
	} Misc;
	DWORD VirtualAddress;
	DWORD SizeOfRawData;
	DWORD PointerToRawData;
	DWORD PointerToRelocations;
	DWORD PointerToLinenumbers;
	WORD NumberOfRelocations;
	WORD NumberOfLinenumbers;
	DWORD Characteristics;
} IMAGE_SECTION_HEADER;

typedef struct _IMAGE_BASE_RELOCATION {
	DWORD VirtualAddress;
	DWORD SizeOfBlock;
	/* followed by WORD entries: type << 12 | offset */
} IMAGE_BASE_RELOCATION;

typedef struct _IMAGE_EXPORT_DIRECTORY {
	DWORD Characteristics;
	DWORD TimeDateStamp;
	WORD MajorVersion;
	WORD MinorVersion;
	DWORD Name;
	DWORD Base;
	DWORD NumberOfFunctions;
	DWORD NumberOfNames;
	DWORD AddressOfFunctions;
	DWORD AddressOfNames;
	DWORD AddressOfNameOrdinals;
} IMAGE_EXPORT_DIRECTORY;

typedef struct _IMAGE_IMPORT_BY_NAME {
	WORD Hint;
	CHAR Name[1];
} IMAGE_IMPORT_BY_NAME;

typedef struct _IMAGE_THUNK_DATA32 {
	union {
		DWORD ForwarderString;
		DWORD Function;
		DWORD Ordinal;
		DWORD AddressOfData;
	} u1;
} IMAGE_THUNK_DATA32;

typedef struct _IMAGE_IMPORT_DESCRIPTOR {
	union {
		DWORD Characteristics;
		DWORD OriginalFirstThunk;
	};
	DWORD TimeDateStamp;
	DWORD ForwarderChain;
	DWORD Name;
	DWORD FirstThunk;
} IMAGE_IMPORT_DESCRIPTOR;

typedef struct _IMAGE_DELAYLOAD_DESCRIPTOR {
	union {
		DWORD AllAttributes;
	} Attributes;
	DWORD DllNameRVA;
	DWORD ModuleHandleRVA;
	DWORD ImportAddressTableRVA;
	DWORD ImportNameTableRVA;
	DWORD BoundImportAddressTableRVA;
	DWORD UnloadInformationTableRVA;
	DWORD TimeDateStamp;
} IMAGE_DELAYLOAD_DESCRIPTOR;

/* Attributes.AllAttributes bit 0: the fields are RVAs (VC7+), not VAs (VC6). */
#define IMAGE_DELAYLOAD_RVA_BASED 0x1

typedef void (WINAPI *PIMAGE_TLS_CALLBACK)(PVOID DllHandle, DWORD Reason,
					   PVOID Reserved);

typedef struct _IMAGE_TLS_DIRECTORY32 {
	DWORD StartAddressOfRawData;	/* these four are VAs, not RVAs */
	DWORD EndAddressOfRawData;
	DWORD AddressOfIndex;
	DWORD AddressOfCallBacks;
	DWORD SizeOfZeroFill;
	DWORD Characteristics;
} IMAGE_TLS_DIRECTORY32;

typedef struct _IMAGE_RESOURCE_DIRECTORY {
	DWORD Characteristics;
	DWORD TimeDateStamp;
	WORD MajorVersion;
	WORD MinorVersion;
	WORD NumberOfNamedEntries;
	WORD NumberOfIdEntries;
	/* followed by the entries, named ones first */
} IMAGE_RESOURCE_DIRECTORY;

typedef struct _IMAGE_RESOURCE_DIRECTORY_ENTRY {
	union {
		DWORD Name;	/* IMAGE_RESOURCE_NAME_IS_STRING | offset, or id */
		WORD Id;
	};
	union {
		DWORD OffsetToData;	/* IMAGE_RESOURCE_DATA_IS_DIRECTORY | offset */
	};
} IMAGE_RESOURCE_DIRECTORY_ENTRY;

typedef struct _IMAGE_RESOURCE_DATA_ENTRY {
	DWORD OffsetToData;	/* an RVA, unlike the directory offsets */
	DWORD Size;
	DWORD CodePage;
	DWORD Reserved;
} IMAGE_RESOURCE_DATA_ENTRY;

/* Version resources (verrsrc.h on Windows). */
#define VS_FFI_SIGNATURE     0xFEEF04BD
#define VS_FFI_STRUCVERSION  0x00010000
#define VS_FFI_FILEFLAGSMASK 0x0000003F
#define VOS_NT_WINDOWS32     0x00040004
#define VFT_UNKNOWN          0x00000000
#define VFT_APP              0x00000001
#define VFT_DLL              0x00000002
#define VFT_DRV              0x00000003

/* Structured exception handling: exception codes (winnt.h's NTSTATUS-based
 * values), EXCEPTION_RECORD/CONTEXT and the fs:[0] chain's raw x86 ABI. */
#define EXCEPTION_DATATYPE_MISALIGNMENT    0x80000002
#define EXCEPTION_BREAKPOINT               0x80000003
#define EXCEPTION_SINGLE_STEP              0x80000004
#define EXCEPTION_ACCESS_VIOLATION         0xC0000005
#define EXCEPTION_IN_PAGE_ERROR            0xC0000006
#define EXCEPTION_ILLEGAL_INSTRUCTION      0xC000001D
#define EXCEPTION_NONCONTINUABLE_EXCEPTION 0xC0000025
#define EXCEPTION_INVALID_DISPOSITION      0xC0000026
#define EXCEPTION_ARRAY_BOUNDS_EXCEEDED    0xC000008C
#define EXCEPTION_FLT_DENORMAL_OPERAND     0xC000008D
#define EXCEPTION_FLT_DIVIDE_BY_ZERO       0xC000008E
#define EXCEPTION_FLT_INEXACT_RESULT       0xC000008F
#define EXCEPTION_FLT_INVALID_OPERATION    0xC0000090
#define EXCEPTION_FLT_OVERFLOW              0xC0000091
#define EXCEPTION_FLT_STACK_CHECK          0xC0000092
#define EXCEPTION_FLT_UNDERFLOW            0xC0000093
#define EXCEPTION_INT_DIVIDE_BY_ZERO       0xC0000094
#define EXCEPTION_INT_OVERFLOW             0xC0000095
#define EXCEPTION_PRIV_INSTRUCTION         0xC0000096
#define EXCEPTION_STACK_OVERFLOW           0xC00000FD

#define EXCEPTION_MAXIMUM_PARAMETERS 15

/* EXCEPTION_RECORD.ExceptionFlags, and RtlUnwind's internal ones. */
#define EXCEPTION_NONCONTINUABLE  0x01
#define EXCEPTION_UNWINDING       0x02
#define EXCEPTION_EXIT_UNWIND     0x04
#define EXCEPTION_STACK_INVALID   0x08
#define EXCEPTION_NESTED_CALL     0x10
#define EXCEPTION_TARGET_UNWIND   0x20
#define EXCEPTION_COLLIDED_UNWIND 0x40
#define EXCEPTION_UNWIND (EXCEPTION_UNWINDING | EXCEPTION_EXIT_UNWIND | \
			  EXCEPTION_TARGET_UNWIND | EXCEPTION_COLLIDED_UNWIND)

/* A __except(filter) expression's value, and UnhandledExceptionFilter's
 * return: distinct from EXCEPTION_DISPOSITION below. */
#define EXCEPTION_CONTINUE_SEARCH    0
#define EXCEPTION_EXECUTE_HANDLER    1
#define EXCEPTION_CONTINUE_EXECUTION (-1)

typedef struct _EXCEPTION_RECORD {
	DWORD ExceptionCode;
	DWORD ExceptionFlags;
	struct _EXCEPTION_RECORD *ExceptionRecord;
	PVOID ExceptionAddress;
	DWORD NumberParameters;
	DWORD_PTR ExceptionInformation[EXCEPTION_MAXIMUM_PARAMETERS];
} EXCEPTION_RECORD, *PEXCEPTION_RECORD;

/* CONTEXT (i386): GetThreadContext/SetThreadContext's flags selecting which
 * of the sections below are meaningful. */
#define CONTEXT_i386                0x00010000
#define CONTEXT_CONTROL            (CONTEXT_i386 | 0x00000001)
#define CONTEXT_INTEGER            (CONTEXT_i386 | 0x00000002)
#define CONTEXT_SEGMENTS           (CONTEXT_i386 | 0x00000004)
#define CONTEXT_FLOATING_POINT     (CONTEXT_i386 | 0x00000008)
#define CONTEXT_DEBUG_REGISTERS    (CONTEXT_i386 | 0x00000010)
#define CONTEXT_EXTENDED_REGISTERS (CONTEXT_i386 | 0x00000020)
#define CONTEXT_FULL (CONTEXT_CONTROL | CONTEXT_INTEGER | CONTEXT_SEGMENTS)

#define SIZE_OF_80387_REGISTERS     80
#define MAXIMUM_SUPPORTED_EXTENSION 512

typedef struct _FLOATING_SAVE_AREA {
	DWORD ControlWord;
	DWORD StatusWord;
	DWORD TagWord;
	DWORD ErrorOffset;
	DWORD ErrorSelector;
	DWORD DataOffset;
	DWORD DataSelector;
	BYTE  RegisterArea[SIZE_OF_80387_REGISTERS];
	DWORD Cr0NpxState;
} FLOATING_SAVE_AREA;

typedef struct _CONTEXT {
	DWORD ContextFlags;

	DWORD Dr0, Dr1, Dr2, Dr3, Dr6, Dr7;

	FLOATING_SAVE_AREA FloatSave;

	DWORD SegGs, SegFs, SegEs, SegDs;
	DWORD Edi, Esi, Ebx, Edx, Ecx, Eax;
	DWORD Ebp, Eip, SegCs, EFlags, Esp, SegSs;

	BYTE ExtendedRegisters[MAXIMUM_SUPPORTED_EXTENSION];
} CONTEXT, *PCONTEXT;

typedef struct _EXCEPTION_POINTERS {
	PEXCEPTION_RECORD ExceptionRecord;
	PCONTEXT ContextRecord;
} EXCEPTION_POINTERS, *PEXCEPTION_POINTERS;

/* What a handler in the fs:[0] chain returns: whether it resolved the
 * exception (ExceptionContinueExecution), wants the next frame tried
 * (ExceptionContinueSearch), or is unwinding past a re-entrant exception. */
typedef enum _EXCEPTION_DISPOSITION {
	ExceptionContinueExecution,
	ExceptionContinueSearch,
	ExceptionNestedException,
	ExceptionCollidedUnwind,
} EXCEPTION_DISPOSITION;

typedef EXCEPTION_DISPOSITION (WINAPI *PEXCEPTION_ROUTINE)(
	struct _EXCEPTION_RECORD *ExceptionRecord, void *EstablisherFrame,
	struct _CONTEXT *ContextRecord, void *DispatcherContext);

/* The node type of the fs:[0] chain: every stack frame with a __try in it
 * pushes one of these (or, compiler-specific, something starting with
 * these two fields) and points fs:[0] at it. */
typedef struct _EXCEPTION_REGISTRATION_RECORD {
	struct _EXCEPTION_REGISTRATION_RECORD *Next;
	PEXCEPTION_ROUTINE Handler;
} EXCEPTION_REGISTRATION_RECORD, *PEXCEPTION_REGISTRATION_RECORD;

typedef LONG (WINAPI *LPTOP_LEVEL_EXCEPTION_FILTER)(EXCEPTION_POINTERS *);

typedef struct tagVS_FIXEDFILEINFO {
	DWORD dwSignature;
	DWORD dwStrucVersion;
	DWORD dwFileVersionMS;
	DWORD dwFileVersionLS;
	DWORD dwProductVersionMS;
	DWORD dwProductVersionLS;
	DWORD dwFileFlagsMask;
	DWORD dwFileFlags;
	DWORD dwFileOS;
	DWORD dwFileType;
	DWORD dwFileSubtype;
	DWORD dwFileDateMS;
	DWORD dwFileDateLS;
} VS_FIXEDFILEINFO;

#endif /* VFW_WIN32_WINNT_H */
