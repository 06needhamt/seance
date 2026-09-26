/*
 * Installable-driver definitions (the parts of mmsystem.h / mmiscapi.h that
 * VfW codecs use).
 */
#ifndef VFW_WIN32_MMSYSTEM_H
#define VFW_WIN32_MMSYSTEM_H

#include "types.h"

/* Standard driver messages. */
#define DRV_LOAD           0x0001
#define DRV_ENABLE         0x0002
#define DRV_OPEN           0x0003
#define DRV_CLOSE          0x0004
#define DRV_DISABLE        0x0005
#define DRV_FREE           0x0006
#define DRV_CONFIGURE      0x0007
#define DRV_QUERYCONFIGURE 0x0008
#define DRV_INSTALL        0x0009
#define DRV_REMOVE         0x000A
#define DRV_EXITSESSION    0x000B
#define DRV_POWER          0x000F
#define DRV_RESERVED       0x0800
#define DRV_USER           0x4000

/* Return values for DRV_CONFIGURE, DRV_INSTALL and DRV_REMOVE. */
#define DRVCNF_CANCEL  0x0000
#define DRVCNF_OK      0x0001
#define DRVCNF_RESTART 0x0002

/* The Windows SDK's mmsystem.h is byte-packed throughout (pshpack1.h). */
#pragma pack(push, 1)
typedef struct tagDRVCONFIGINFO {
	DWORD dwDCISize;
	LPCWSTR lpszDCISectionName;
	LPCWSTR lpszDCIAliasName;
} DRVCONFIGINFO, *PDRVCONFIGINFO, *LPDRVCONFIGINFO;
#pragma pack(pop)

/* The DriverProc export of an installable driver. */
typedef LRESULT (CALLBACK *DRIVERPROC)(DWORD_PTR dwDriverId, HDRVR hdrvr,
				       UINT msg, LPARAM lParam1, LPARAM lParam2);

typedef DWORD FOURCC;

#define mmioFOURCC(c0, c1, c2, c3) \
	((DWORD)(BYTE)(c0) | ((DWORD)(BYTE)(c1) << 8) | \
	 ((DWORD)(BYTE)(c2) << 16) | ((DWORD)(BYTE)(c3) << 24))

#endif /* VFW_WIN32_MMSYSTEM_H */
