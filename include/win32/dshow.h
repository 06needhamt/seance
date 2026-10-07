/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * DirectShow definitions (strmif.h, amvideo.h, dvdmedia.h, uuids.h,
 * evcode.h, vfwmsgs.h, and the ocidl.h property-page interface): the
 * interfaces a video transform filter implements and calls, the structures
 * they pass, and the GUIDs that name them. Interfaces are declared as in
 * objbase.h, with each vtable's members named after the methods, in
 * order.
 */
#ifndef VFW_WIN32_DSHOW_H
#define VFW_WIN32_DSHOW_H

#include "objbase.h"
#include "oleauto.h"
#include "types.h"
#include "wingdi.h"

/* Times are in 100 ns units. */
typedef LONGLONG REFERENCE_TIME;
typedef REFERENCE_TIME *LPREFERENCE_TIME;
typedef DWORD_PTR HEVENT;
typedef DWORD_PTR HSEMAPHORE;

#define MAX_PIN_NAME    128
#define MAX_FILTER_NAME 128

/* PIN_DIRECTION and FILTER_STATE (enums in the Windows headers). */
typedef int PIN_DIRECTION;
#define PINDIR_INPUT  0
#define PINDIR_OUTPUT 1

typedef int FILTER_STATE;
#define State_Stopped 0
#define State_Paused  1
#define State_Running 2

/* AM_SAMPLE2_PROPERTIES.dwSampleFlags and dwStreamId. */
#define AM_SAMPLE_SPLICEPOINT        0x001
#define AM_SAMPLE_PREROLL            0x002
#define AM_SAMPLE_DATADISCONTINUITY  0x004
#define AM_SAMPLE_TYPECHANGED        0x008
#define AM_SAMPLE_TIMEVALID          0x010
#define AM_SAMPLE_TIMEDISCONTINUITY  0x040
#define AM_SAMPLE_FLUSH_ON_PAUSE     0x080
#define AM_SAMPLE_STOPVALID          0x100
#define AM_SAMPLE_ENDOFSTREAM        0x200
#define AM_STREAM_MEDIA              0
#define AM_STREAM_CONTROL            1

/* IMemAllocator::GetBuffer flags. */
#define AM_GBF_PREVFRAMESKIPPED 1
#define AM_GBF_NOTASYNCPOINT    2
#define AM_GBF_NOWAIT           4
#define AM_GBF_NODDSURFACELOCK  8

/* Filter merits. */
#define MERIT_PREFERRED     0x800000
#define MERIT_NORMAL        0x600000
#define MERIT_UNLIKELY      0x400000
#define MERIT_DO_NOT_USE    0x200000
#define MERIT_SW_COMPRESSOR 0x100000
#define MERIT_HW_COMPRESSOR 0x100050

/* REGFILTERPINS2.dwFlags. */
#define REG_PINFLAG_B_ZERO     0x1
#define REG_PINFLAG_B_RENDERER 0x2
#define REG_PINFLAG_B_MANY     0x4
#define REG_PINFLAG_B_OUTPUT   0x8

/* Quality.Type (QualityMessageType). */
#define Famine 0
#define Flood  1

/* IAMVideoCompression::GetInfo capabilities (CompressionCaps). */
#define CompressionCaps_CanQuality  0x01
#define CompressionCaps_CanCrunch   0x02
#define CompressionCaps_CanKeyFrame 0x04
#define CompressionCaps_CanBFrame   0x08
#define CompressionCaps_CanWindow   0x10

/* Event codes a filter sends through IMediaEventSink::Notify (evcode.h). */
#define EC_SYSTEMBASE                0x00
#define EC_USER                      0x8000
#define EC_COMPLETE                  0x01
#define EC_USERABORT                 0x02
#define EC_ERRORABORT                0x03
#define EC_TIME                      0x04
#define EC_REPAINT                   0x05
#define EC_STREAM_ERROR_STOPPED      0x06
#define EC_STREAM_ERROR_STILLPLAYING 0x07
#define EC_ERROR_STILLPLAYING        0x08
#define EC_ERRORABORTEX              0x45
#define EC_PALETTE_CHANGED           0x09
#define EC_VIDEO_SIZE_CHANGED        0x0A
#define EC_QUALITY_CHANGE            0x0B
#define EC_SHUTTING_DOWN             0x0C
#define EC_CLOCK_CHANGED             0x0D
#define EC_PAUSED                    0x0E
#define EC_OPENING_FILE              0x10
#define EC_BUFFERING_DATA            0x11
#define EC_FULLSCREEN_LOST           0x12
#define EC_ACTIVATE                  0x13
#define EC_NEED_RESTART              0x14
#define EC_WINDOW_DESTROYED          0x15
#define EC_DISPLAY_CHANGED           0x16
#define EC_STARVATION                0x17
#define EC_OLE_EVENT                 0x18
#define EC_NOTIFY_WINDOW             0x19
#define EC_STREAM_CONTROL_STOPPED    0x1A
#define EC_STREAM_CONTROL_STARTED    0x1B
#define EC_END_OF_SEGMENT            0x1C
#define EC_SEGMENT_STARTED           0x1D
#define EC_LENGTH_CHANGED            0x1E
#define EC_DEVICE_LOST               0x1F

/* VIDEOINFOHEADER2.dwInterlaceFlags and dwControlFlags (dvdmedia.h). */
#define AMINTERLACE_IsInterlaced            0x00000001
#define AMINTERLACE_1FieldPerSample         0x00000002
#define AMINTERLACE_Field1First             0x00000004
#define AMINTERLACE_FieldPatternMask        0x00000030
#define AMINTERLACE_DisplayModeMask         0x000000c0
#define AMCONTROL_USED                      0x00000001
#define AMCONTROL_PAD_TO_4x3                0x00000002
#define AMCONTROL_PAD_TO_16x9               0x00000004
#define AMCONTROL_COLORINFO_PRESENT         0x00000080

/* DirectShow's HRESULTs (vfwmsgs.h). */
#define VFW_S_NO_MORE_ITEMS              ((HRESULT)0x00040103)
#define VFW_S_DUPLICATE_NAME             ((HRESULT)0x0004022D)
#define VFW_S_STATE_INTERMEDIATE         ((HRESULT)0x00040237)
#define VFW_S_PARTIAL_RENDER             ((HRESULT)0x00040242)
#define VFW_S_SOME_DATA_IGNORED          ((HRESULT)0x00040245)
#define VFW_S_CONNECTIONS_DEFERRED       ((HRESULT)0x00040246)
#define VFW_S_RESOURCE_NOT_NEEDED        ((HRESULT)0x00040250)
#define VFW_S_MEDIA_TYPE_IGNORED         ((HRESULT)0x00040254)
#define VFW_S_VIDEO_NOT_RENDERED         ((HRESULT)0x00040257)
#define VFW_S_AUDIO_NOT_RENDERED         ((HRESULT)0x00040258)
#define VFW_S_RPZA                       ((HRESULT)0x0004025A)
#define VFW_S_ESTIMATED                  ((HRESULT)0x00040260)
#define VFW_S_RESERVED                   ((HRESULT)0x00040263)
#define VFW_S_STREAM_OFF                 ((HRESULT)0x00040267)
#define VFW_S_CANT_CUE                   ((HRESULT)0x00040268)
#define VFW_S_NO_STOP_TIME               ((HRESULT)0x00040270)
#define VFW_S_NOPREVIEWPIN               ((HRESULT)0x0004027E)
#define VFW_S_DVD_NON_ONE_SEQUENTIAL     ((HRESULT)0x00040280)
#define VFW_S_DVD_CHANNEL_CONTENTS_NOT_AVAILABLE ((HRESULT)0x0004028C)
#define VFW_S_DVD_NOT_ACCURATE           ((HRESULT)0x0004028D)
#define VFW_E_INVALIDMEDIATYPE           ((HRESULT)0x80040200)
#define VFW_E_INVALIDSUBTYPE             ((HRESULT)0x80040201)
#define VFW_E_NEED_OWNER                 ((HRESULT)0x80040202)
#define VFW_E_ENUM_OUT_OF_SYNC           ((HRESULT)0x80040203)
#define VFW_E_ALREADY_CONNECTED          ((HRESULT)0x80040204)
#define VFW_E_FILTER_ACTIVE              ((HRESULT)0x80040205)
#define VFW_E_NO_TYPES                   ((HRESULT)0x80040206)
#define VFW_E_NO_ACCEPTABLE_TYPES        ((HRESULT)0x80040207)
#define VFW_E_INVALID_DIRECTION          ((HRESULT)0x80040208)
#define VFW_E_NOT_CONNECTED              ((HRESULT)0x80040209)
#define VFW_E_NO_ALLOCATOR               ((HRESULT)0x8004020A)
#define VFW_E_RUNTIME_ERROR              ((HRESULT)0x8004020B)
#define VFW_E_BUFFER_NOTSET              ((HRESULT)0x8004020C)
#define VFW_E_BUFFER_OVERFLOW            ((HRESULT)0x8004020D)
#define VFW_E_BADALIGN                   ((HRESULT)0x8004020E)
#define VFW_E_ALREADY_COMMITTED          ((HRESULT)0x8004020F)
#define VFW_E_BUFFERS_OUTSTANDING        ((HRESULT)0x80040210)
#define VFW_E_NOT_COMMITTED              ((HRESULT)0x80040211)
#define VFW_E_SIZENOTSET                 ((HRESULT)0x80040212)
#define VFW_E_NO_CLOCK                   ((HRESULT)0x80040213)
#define VFW_E_NO_SINK                    ((HRESULT)0x80040214)
#define VFW_E_NO_INTERFACE               ((HRESULT)0x80040215)
#define VFW_E_NOT_FOUND                  ((HRESULT)0x80040216)
#define VFW_E_CANNOT_CONNECT             ((HRESULT)0x80040217)
#define VFW_E_CANNOT_RENDER              ((HRESULT)0x80040218)
#define VFW_E_CHANGING_FORMAT            ((HRESULT)0x80040219)
#define VFW_E_NO_COLOR_KEY_SET           ((HRESULT)0x8004021A)
#define VFW_E_NOT_OVERLAY_CONNECTION     ((HRESULT)0x8004021B)
#define VFW_E_NOT_SAMPLE_CONNECTION      ((HRESULT)0x8004021C)
#define VFW_E_PALETTE_SET                ((HRESULT)0x8004021D)
#define VFW_E_COLOR_KEY_SET              ((HRESULT)0x8004021E)
#define VFW_E_NO_COLOR_KEY_FOUND         ((HRESULT)0x8004021F)
#define VFW_E_NO_PALETTE_AVAILABLE       ((HRESULT)0x80040220)
#define VFW_E_NO_DISPLAY_PALETTE         ((HRESULT)0x80040221)
#define VFW_E_TOO_MANY_COLORS            ((HRESULT)0x80040222)
#define VFW_E_STATE_CHANGED              ((HRESULT)0x80040223)
#define VFW_E_NOT_STOPPED                ((HRESULT)0x80040224)
#define VFW_E_NOT_PAUSED                 ((HRESULT)0x80040225)
#define VFW_E_NOT_RUNNING                ((HRESULT)0x80040226)
#define VFW_E_WRONG_STATE                ((HRESULT)0x80040227)
#define VFW_E_START_TIME_AFTER_END       ((HRESULT)0x80040228)
#define VFW_E_INVALID_RECT               ((HRESULT)0x80040229)
#define VFW_E_TYPE_NOT_ACCEPTED          ((HRESULT)0x8004022A)
#define VFW_E_SAMPLE_REJECTED            ((HRESULT)0x8004022B)
#define VFW_E_SAMPLE_REJECTED_EOS        ((HRESULT)0x8004022C)
#define VFW_E_DUPLICATE_NAME             ((HRESULT)0x8004022D)
#define VFW_E_TIMEOUT                    ((HRESULT)0x8004022E)
#define VFW_E_INVALID_FILE_FORMAT        ((HRESULT)0x8004022F)
#define VFW_E_ENUM_OUT_OF_RANGE          ((HRESULT)0x80040230)
#define VFW_E_CIRCULAR_GRAPH             ((HRESULT)0x80040231)
#define VFW_E_NOT_ALLOWED_TO_SAVE        ((HRESULT)0x80040232)
#define VFW_E_TIME_ALREADY_PASSED        ((HRESULT)0x80040233)
#define VFW_E_ALREADY_CANCELLED          ((HRESULT)0x80040234)
#define VFW_E_CORRUPT_GRAPH_FILE         ((HRESULT)0x80040235)
#define VFW_E_ADVISE_ALREADY_SET         ((HRESULT)0x80040236)
#define VFW_E_NO_MODEX_AVAILABLE         ((HRESULT)0x80040238)
#define VFW_E_NO_ADVISE_SET              ((HRESULT)0x80040239)
#define VFW_E_NO_FULLSCREEN              ((HRESULT)0x8004023A)
#define VFW_E_IN_FULLSCREEN_MODE         ((HRESULT)0x8004023B)
#define VFW_E_UNKNOWN_FILE_TYPE          ((HRESULT)0x80040240)
#define VFW_E_CANNOT_LOAD_SOURCE_FILTER  ((HRESULT)0x80040241)
#define VFW_E_FILE_TOO_SHORT             ((HRESULT)0x80040243)
#define VFW_E_INVALID_FILE_VERSION       ((HRESULT)0x80040244)
#define VFW_E_INVALID_CLSID              ((HRESULT)0x80040247)
#define VFW_E_INVALID_MEDIA_TYPE         ((HRESULT)0x80040248)
#define VFW_E_SAMPLE_TIME_NOT_SET        ((HRESULT)0x80040249)
#define VFW_E_MEDIA_TIME_NOT_SET         ((HRESULT)0x80040251)
#define VFW_E_NO_TIME_FORMAT_SET         ((HRESULT)0x80040252)
#define VFW_E_MONO_AUDIO_HW              ((HRESULT)0x80040253)
#define VFW_E_NO_DECOMPRESSOR            ((HRESULT)0x80040255)
#define VFW_E_NO_AUDIO_HARDWARE          ((HRESULT)0x80040256)
#define VFW_E_RPZA                       ((HRESULT)0x80040259)
#define VFW_E_PROCESSOR_NOT_SUITABLE     ((HRESULT)0x8004025B)
#define VFW_E_UNSUPPORTED_AUDIO          ((HRESULT)0x8004025C)
#define VFW_E_UNSUPPORTED_VIDEO          ((HRESULT)0x8004025D)
#define VFW_E_MPEG_NOT_CONSTRAINED       ((HRESULT)0x8004025E)
#define VFW_E_NOT_IN_GRAPH               ((HRESULT)0x8004025F)
#define VFW_E_NO_TIME_FORMAT             ((HRESULT)0x80040261)
#define VFW_E_READ_ONLY                  ((HRESULT)0x80040262)
#define VFW_E_BUFFER_UNDERFLOW           ((HRESULT)0x80040264)
#define VFW_E_UNSUPPORTED_STREAM         ((HRESULT)0x80040265)
#define VFW_E_NO_TRANSPORT               ((HRESULT)0x80040266)
#define VFW_E_BAD_VIDEOCD                ((HRESULT)0x80040269)
#define VFW_E_OUT_OF_VIDEO_MEMORY        ((HRESULT)0x80040271)
#define VFW_E_VP_NEGOTIATION_FAILED      ((HRESULT)0x80040272)
#define VFW_E_DDRAW_CAPS_NOT_SUITABLE    ((HRESULT)0x80040273)
#define VFW_E_NO_VP_HARDWARE             ((HRESULT)0x80040274)
#define VFW_E_NO_CAPTURE_HARDWARE        ((HRESULT)0x80040275)
#define VFW_E_DVD_OPERATION_INHIBITED    ((HRESULT)0x80040276)
#define VFW_E_DVD_INVALIDDOMAIN          ((HRESULT)0x80040277)
#define VFW_E_DVD_NO_BUTTON              ((HRESULT)0x80040278)
#define VFW_E_DVD_GRAPHNOTREADY          ((HRESULT)0x80040279)
#define VFW_E_DVD_RENDERFAIL             ((HRESULT)0x8004027A)
#define VFW_E_DVD_DECNOTENOUGH           ((HRESULT)0x8004027B)
#define VFW_E_DDRAW_VERSION_NOT_SUITABLE ((HRESULT)0x8004027C)
#define VFW_E_COPYPROT_FAILED            ((HRESULT)0x8004027D)
#define VFW_E_TIME_EXPIRED               ((HRESULT)0x8004027F)
#define VFW_E_DVD_WRONG_SPEED            ((HRESULT)0x80040281)
#define VFW_E_DVD_MENU_DOES_NOT_EXIST    ((HRESULT)0x80040282)
#define VFW_E_DVD_CMD_CANCELLED          ((HRESULT)0x80040283)
#define VFW_E_DVD_STATE_WRONG_VERSION    ((HRESULT)0x80040284)
#define VFW_E_DVD_STATE_CORRUPT          ((HRESULT)0x80040285)
#define VFW_E_DVD_STATE_WRONG_DISC       ((HRESULT)0x80040286)
#define VFW_E_DVD_INCOMPATIBLE_REGION    ((HRESULT)0x80040287)
#define VFW_E_DVD_NO_ATTRIBUTES          ((HRESULT)0x80040288)
#define VFW_E_DVD_NO_GOUP_PGC            ((HRESULT)0x80040289)
#define VFW_E_DVD_LOW_PARENTAL_LEVEL     ((HRESULT)0x8004028A)
#define VFW_E_DVD_NOT_IN_KARAOKE_MODE    ((HRESULT)0x8004028B)
#define VFW_E_FRAME_STEP_UNSUPPORTED     ((HRESULT)0x8004028E)
#define VFW_E_DVD_STREAM_DISABLED        ((HRESULT)0x8004028F)
#define VFW_E_DVD_TITLE_UNKNOWN          ((HRESULT)0x80040290)
#define VFW_E_DVD_INVALID_DISC           ((HRESULT)0x80040291)
#define VFW_E_DVD_NO_RESUME_INFORMATION  ((HRESULT)0x80040292)
#define VFW_E_PIN_ALREADY_BLOCKED_ON_THIS_THREAD ((HRESULT)0x80040293)
#define VFW_E_PIN_ALREADY_BLOCKED        ((HRESULT)0x80040294)
#define VFW_E_CERTIFICATION_FAILURE      ((HRESULT)0x80040295)
#define VFW_E_VMR_NOT_IN_MIXER_MODE      ((HRESULT)0x80040296)
#define VFW_E_VMR_NO_AP_SUPPLIED         ((HRESULT)0x80040297)
#define VFW_E_VMR_NO_DEINTERLACE_HW      ((HRESULT)0x80040298)
#define VFW_E_VMR_NO_PROCAMP_HW          ((HRESULT)0x80040299)
#define VFW_E_DVD_VMR9_INCOMPATIBLEDEC   ((HRESULT)0x8004029A)
#define VFW_E_NO_COPP_HW                 ((HRESULT)0x8004029B)
#define VFW_E_DVD_NONBLOCKING            ((HRESULT)0x8004029C)
#define VFW_E_DVD_TOO_MANY_RENDERERS_IN_FILTER_GRAPH ((HRESULT)0x8004029D)
#define VFW_E_DVD_NON_EVR_RENDERER_IN_FILTER_GRAPH ((HRESULT)0x8004029E)
#define VFW_E_DVD_RESOLUTION_ERROR       ((HRESULT)0x8004029F)
#define VFW_E_CODECAPI_LINEAR_RANGE      ((HRESULT)0x80040310)
#define VFW_E_CODECAPI_ENUMERATED        ((HRESULT)0x80040311)
#define VFW_E_CODECAPI_NO_DEFAULT        ((HRESULT)0x80040313)
#define VFW_E_CODECAPI_NO_CURRENT_VALUE  ((HRESULT)0x80040314)
#define VFW_E_DVD_CHAPTER_DOES_NOT_EXIST ((HRESULT)0x80040315)
#define VFW_E_BAD_KEY                    ((HRESULT)0x800403F2)

/* A media type: what flows over a pin connection. */
typedef struct _AMMediaType {
	GUID majortype;
	GUID subtype;
	BOOL bFixedSizeSamples;
	BOOL bTemporalCompression;
	ULONG lSampleSize;
	GUID formattype;
	IUnknown *pUnk;
	ULONG cbFormat;
	BYTE *pbFormat;
} AM_MEDIA_TYPE;

/* FORMAT_VideoInfo (amvideo.h). */
typedef struct tagVIDEOINFOHEADER {
	RECT rcSource;
	RECT rcTarget;
	DWORD dwBitRate;
	DWORD dwBitErrorRate;
	REFERENCE_TIME AvgTimePerFrame;
	BITMAPINFOHEADER bmiHeader;
} VIDEOINFOHEADER;

/* FORMAT_VideoInfo2 (dvdmedia.h). */
typedef struct tagVIDEOINFOHEADER2 {
	RECT rcSource;
	RECT rcTarget;
	DWORD dwBitRate;
	DWORD dwBitErrorRate;
	REFERENCE_TIME AvgTimePerFrame;
	DWORD dwInterlaceFlags;
	DWORD dwCopyProtectFlags;
	DWORD dwPictAspectRatioX;
	DWORD dwPictAspectRatioY;
	union {
		DWORD dwControlFlags;
		DWORD dwReserved1;
	};
	DWORD dwReserved2;
	BITMAPINFOHEADER bmiHeader;
} VIDEOINFOHEADER2;

/* FORMAT_MPEGVideo and FORMAT_MPEG2Video: headers then sequence headers. */
typedef struct tagMPEG1VIDEOINFO {
	VIDEOINFOHEADER hdr;
	DWORD dwStartTimeCode;
	DWORD cbSequenceHeader;
	BYTE bSequenceHeader[1];
} MPEG1VIDEOINFO;

typedef struct tagMPEG2VIDEOINFO {
	VIDEOINFOHEADER2 hdr;
	DWORD dwStartTimeCode;
	DWORD cbSequenceHeader;
	DWORD dwProfile;
	DWORD dwLevel;
	DWORD dwFlags;
	DWORD dwSequenceHeader[1];
} MPEG2VIDEOINFO;

typedef struct _AllocatorProperties {
	LONG cBuffers;
	LONG cbBuffer;
	LONG cbAlign;
	LONG cbPrefix;
} ALLOCATOR_PROPERTIES;

typedef struct tagAM_SAMPLE2_PROPERTIES {
	DWORD cbData;
	DWORD dwTypeSpecificFlags;
	DWORD dwSampleFlags;
	LONG lActual;
	REFERENCE_TIME tStart;
	REFERENCE_TIME tStop;
	DWORD dwStreamId;
	AM_MEDIA_TYPE *pMediaType;
	BYTE *pbBuffer;
	LONG cbBuffer;
} AM_SAMPLE2_PROPERTIES;

typedef struct tagQuality {
	int Type;		/* Famine or Flood */
	LONG Proportion;
	REFERENCE_TIME Late;
	REFERENCE_TIME TimeStamp;
} Quality;

/* A counted array of GUIDs (ocidl.h), as GetPages returns. */
typedef struct tagCAUUID {
	ULONG cElems;
	GUID *pElems;
} CAUUID;

/* Filter registration (IFilterMapper2::RegisterFilter). */
typedef struct {
	const CLSID *clsMajorType;
	const CLSID *clsMinorType;
} REGPINTYPES;

typedef struct {
	LPWSTR strName;
	BOOL bRendered;
	BOOL bOutput;
	BOOL bZero;
	BOOL bMany;
	const CLSID *clsConnectsToFilter;
	const WCHAR *strConnectsToPin;
	UINT nMediaTypes;
	const REGPINTYPES *lpMediaType;
} REGFILTERPINS;

typedef struct {
	CLSID clsMedium;
	DWORD dw1;
	DWORD dw2;
} REGPINMEDIUM;

typedef struct {
	DWORD dwFlags;
	UINT cInstances;
	UINT nMediaTypes;
	const REGPINTYPES *lpMediaType;
	UINT nMediums;
	const REGPINMEDIUM *lpMedium;
	const CLSID *clsPinCategory;
} REGFILTERPINS2;

/* dwVersion 1 uses cPins/rgPins, version 2 cPins2/rgPins2. */
typedef struct {
	DWORD dwVersion;
	DWORD dwMerit;
	union {
		struct {
			ULONG cPins;
			const REGFILTERPINS *rgPins;
		};
		struct {
			ULONG cPins2;
			const REGFILTERPINS2 *rgPins2;
		};
	};
} REGFILTER2;

typedef struct IBaseFilter IBaseFilter;
typedef struct IFilterGraph IFilterGraph;
typedef struct IPin IPin;
typedef struct IEnumPins IEnumPins;
typedef struct IEnumMediaTypes IEnumMediaTypes;
typedef struct IEnumFilters IEnumFilters;
typedef struct IEnumRegFilters IEnumRegFilters;
typedef struct IMediaSample IMediaSample;
typedef struct IReferenceClock IReferenceClock;

typedef struct _PinInfo {
	IBaseFilter *pFilter;
	PIN_DIRECTION dir;
	WCHAR achName[MAX_PIN_NAME];
} PIN_INFO;

typedef struct _FilterInfo {
	WCHAR achName[MAX_FILTER_NAME];
	IFilterGraph *pGraph;
} FILTER_INFO;

/* IReferenceClock: the graph's clock. */
typedef struct IReferenceClockVtbl {
	HRESULT (WINAPI *QueryInterface)(IReferenceClock *This, REFIID riid,
					 void **ppv);
	ULONG (WINAPI *AddRef)(IReferenceClock *This);
	ULONG (WINAPI *Release)(IReferenceClock *This);
	HRESULT (WINAPI *GetTime)(IReferenceClock *This, REFERENCE_TIME *time);
	HRESULT (WINAPI *AdviseTime)(IReferenceClock *This,
				     REFERENCE_TIME base, REFERENCE_TIME offset,
				     HEVENT event, DWORD_PTR *cookie);
	HRESULT (WINAPI *AdvisePeriodic)(IReferenceClock *This,
					 REFERENCE_TIME start,
					 REFERENCE_TIME period,
					 HSEMAPHORE semaphore,
					 DWORD_PTR *cookie);
	HRESULT (WINAPI *Unadvise)(IReferenceClock *This, DWORD_PTR cookie);
} IReferenceClockVtbl;
struct IReferenceClock {
	const IReferenceClockVtbl *lpVtbl;
};

/* IMediaFilter: IPersist plus the state a filter is in. */
typedef struct IMediaFilter IMediaFilter;
typedef struct IMediaFilterVtbl {
	HRESULT (WINAPI *QueryInterface)(IMediaFilter *This, REFIID riid,
					 void **ppv);
	ULONG (WINAPI *AddRef)(IMediaFilter *This);
	ULONG (WINAPI *Release)(IMediaFilter *This);
	HRESULT (WINAPI *GetClassID)(IMediaFilter *This, CLSID *clsid);
	HRESULT (WINAPI *Stop)(IMediaFilter *This);
	HRESULT (WINAPI *Pause)(IMediaFilter *This);
	HRESULT (WINAPI *Run)(IMediaFilter *This, REFERENCE_TIME start);
	HRESULT (WINAPI *GetState)(IMediaFilter *This, DWORD timeout,
				   FILTER_STATE *state);
	HRESULT (WINAPI *SetSyncSource)(IMediaFilter *This,
					IReferenceClock *clock);
	HRESULT (WINAPI *GetSyncSource)(IMediaFilter *This,
					IReferenceClock **clock);
} IMediaFilterVtbl;
struct IMediaFilter {
	const IMediaFilterVtbl *lpVtbl;
};

/* IBaseFilter: IMediaFilter plus its pins and its place in a graph. */
typedef struct IBaseFilterVtbl {
	HRESULT (WINAPI *QueryInterface)(IBaseFilter *This, REFIID riid,
					 void **ppv);
	ULONG (WINAPI *AddRef)(IBaseFilter *This);
	ULONG (WINAPI *Release)(IBaseFilter *This);
	HRESULT (WINAPI *GetClassID)(IBaseFilter *This, CLSID *clsid);
	HRESULT (WINAPI *Stop)(IBaseFilter *This);
	HRESULT (WINAPI *Pause)(IBaseFilter *This);
	HRESULT (WINAPI *Run)(IBaseFilter *This, REFERENCE_TIME start);
	HRESULT (WINAPI *GetState)(IBaseFilter *This, DWORD timeout,
				   FILTER_STATE *state);
	HRESULT (WINAPI *SetSyncSource)(IBaseFilter *This,
					IReferenceClock *clock);
	HRESULT (WINAPI *GetSyncSource)(IBaseFilter *This,
					IReferenceClock **clock);
	HRESULT (WINAPI *EnumPins)(IBaseFilter *This, IEnumPins **pins);
	HRESULT (WINAPI *FindPin)(IBaseFilter *This, LPCWSTR id, IPin **pin);
	HRESULT (WINAPI *QueryFilterInfo)(IBaseFilter *This, FILTER_INFO *info);
	HRESULT (WINAPI *JoinFilterGraph)(IBaseFilter *This,
					  IFilterGraph *graph, LPCWSTR name);
	HRESULT (WINAPI *QueryVendorInfo)(IBaseFilter *This, LPWSTR *info);
} IBaseFilterVtbl;
struct IBaseFilter {
	const IBaseFilterVtbl *lpVtbl;
};

/* IPin: one end of a connection. */
typedef struct IPinVtbl {
	HRESULT (WINAPI *QueryInterface)(IPin *This, REFIID riid, void **ppv);
	ULONG (WINAPI *AddRef)(IPin *This);
	ULONG (WINAPI *Release)(IPin *This);
	HRESULT (WINAPI *Connect)(IPin *This, IPin *receiver,
				  const AM_MEDIA_TYPE *mt);
	HRESULT (WINAPI *ReceiveConnection)(IPin *This, IPin *connector,
					    const AM_MEDIA_TYPE *mt);
	HRESULT (WINAPI *Disconnect)(IPin *This);
	HRESULT (WINAPI *ConnectedTo)(IPin *This, IPin **pin);
	HRESULT (WINAPI *ConnectionMediaType)(IPin *This, AM_MEDIA_TYPE *mt);
	HRESULT (WINAPI *QueryPinInfo)(IPin *This, PIN_INFO *info);
	HRESULT (WINAPI *QueryDirection)(IPin *This, PIN_DIRECTION *dir);
	HRESULT (WINAPI *QueryId)(IPin *This, LPWSTR *id);
	HRESULT (WINAPI *QueryAccept)(IPin *This, const AM_MEDIA_TYPE *mt);
	HRESULT (WINAPI *EnumMediaTypes)(IPin *This, IEnumMediaTypes **types);
	HRESULT (WINAPI *QueryInternalConnections)(IPin *This, IPin **pins,
						   ULONG *count);
	HRESULT (WINAPI *EndOfStream)(IPin *This);
	HRESULT (WINAPI *BeginFlush)(IPin *This);
	HRESULT (WINAPI *EndFlush)(IPin *This);
	HRESULT (WINAPI *NewSegment)(IPin *This, REFERENCE_TIME start,
				     REFERENCE_TIME stop, double rate);
} IPinVtbl;
struct IPin {
	const IPinVtbl *lpVtbl;
};

typedef struct IEnumPinsVtbl {
	HRESULT (WINAPI *QueryInterface)(IEnumPins *This, REFIID riid,
					 void **ppv);
	ULONG (WINAPI *AddRef)(IEnumPins *This);
	ULONG (WINAPI *Release)(IEnumPins *This);
	HRESULT (WINAPI *Next)(IEnumPins *This, ULONG count, IPin **pins,
			       ULONG *fetched);
	HRESULT (WINAPI *Skip)(IEnumPins *This, ULONG count);
	HRESULT (WINAPI *Reset)(IEnumPins *This);
	HRESULT (WINAPI *Clone)(IEnumPins *This, IEnumPins **clone);
} IEnumPinsVtbl;
struct IEnumPins {
	const IEnumPinsVtbl *lpVtbl;
};

/* The types it hands out are CoTaskMemAlloc()ed, format block and all. */
typedef struct IEnumMediaTypesVtbl {
	HRESULT (WINAPI *QueryInterface)(IEnumMediaTypes *This, REFIID riid,
					 void **ppv);
	ULONG (WINAPI *AddRef)(IEnumMediaTypes *This);
	ULONG (WINAPI *Release)(IEnumMediaTypes *This);
	HRESULT (WINAPI *Next)(IEnumMediaTypes *This, ULONG count,
			       AM_MEDIA_TYPE **types, ULONG *fetched);
	HRESULT (WINAPI *Skip)(IEnumMediaTypes *This, ULONG count);
	HRESULT (WINAPI *Reset)(IEnumMediaTypes *This);
	HRESULT (WINAPI *Clone)(IEnumMediaTypes *This,
				IEnumMediaTypes **clone);
} IEnumMediaTypesVtbl;
struct IEnumMediaTypes {
	const IEnumMediaTypesVtbl *lpVtbl;
};

typedef struct IEnumFiltersVtbl {
	HRESULT (WINAPI *QueryInterface)(IEnumFilters *This, REFIID riid,
					 void **ppv);
	ULONG (WINAPI *AddRef)(IEnumFilters *This);
	ULONG (WINAPI *Release)(IEnumFilters *This);
	HRESULT (WINAPI *Next)(IEnumFilters *This, ULONG count,
			       IBaseFilter **filters, ULONG *fetched);
	HRESULT (WINAPI *Skip)(IEnumFilters *This, ULONG count);
	HRESULT (WINAPI *Reset)(IEnumFilters *This);
	HRESULT (WINAPI *Clone)(IEnumFilters *This, IEnumFilters **clone);
} IEnumFiltersVtbl;
struct IEnumFilters {
	const IEnumFiltersVtbl *lpVtbl;
};

/* IFilterGraph: what a filter sees of the graph it has joined. */
typedef struct IFilterGraphVtbl {
	HRESULT (WINAPI *QueryInterface)(IFilterGraph *This, REFIID riid,
					 void **ppv);
	ULONG (WINAPI *AddRef)(IFilterGraph *This);
	ULONG (WINAPI *Release)(IFilterGraph *This);
	HRESULT (WINAPI *AddFilter)(IFilterGraph *This, IBaseFilter *filter,
				    LPCWSTR name);
	HRESULT (WINAPI *RemoveFilter)(IFilterGraph *This, IBaseFilter *filter);
	HRESULT (WINAPI *EnumFilters)(IFilterGraph *This, IEnumFilters **e);
	HRESULT (WINAPI *FindFilterByName)(IFilterGraph *This, LPCWSTR name,
					   IBaseFilter **filter);
	HRESULT (WINAPI *ConnectDirect)(IFilterGraph *This, IPin *out,
					IPin *in, const AM_MEDIA_TYPE *mt);
	HRESULT (WINAPI *Reconnect)(IFilterGraph *This, IPin *pin);
	HRESULT (WINAPI *Disconnect)(IFilterGraph *This, IPin *pin);
	HRESULT (WINAPI *SetDefaultSyncSource)(IFilterGraph *This);
} IFilterGraphVtbl;
struct IFilterGraph {
	const IFilterGraphVtbl *lpVtbl;
};

/* IMediaEventSink: how a filter reports events (EC_*) to the graph. */
typedef struct IMediaEventSink IMediaEventSink;
typedef struct IMediaEventSinkVtbl {
	HRESULT (WINAPI *QueryInterface)(IMediaEventSink *This, REFIID riid,
					 void **ppv);
	ULONG (WINAPI *AddRef)(IMediaEventSink *This);
	ULONG (WINAPI *Release)(IMediaEventSink *This);
	HRESULT (WINAPI *Notify)(IMediaEventSink *This, LONG code,
				 LONG_PTR param1, LONG_PTR param2);
} IMediaEventSinkVtbl;
struct IMediaEventSink {
	const IMediaEventSinkVtbl *lpVtbl;
};

/* IMemAllocator: a pool of samples. */
typedef struct IMemAllocator IMemAllocator;
typedef struct IMemAllocatorVtbl {
	HRESULT (WINAPI *QueryInterface)(IMemAllocator *This, REFIID riid,
					 void **ppv);
	ULONG (WINAPI *AddRef)(IMemAllocator *This);
	ULONG (WINAPI *Release)(IMemAllocator *This);
	HRESULT (WINAPI *SetProperties)(IMemAllocator *This,
					ALLOCATOR_PROPERTIES *request,
					ALLOCATOR_PROPERTIES *actual);
	HRESULT (WINAPI *GetProperties)(IMemAllocator *This,
					ALLOCATOR_PROPERTIES *props);
	HRESULT (WINAPI *Commit)(IMemAllocator *This);
	HRESULT (WINAPI *Decommit)(IMemAllocator *This);
	HRESULT (WINAPI *GetBuffer)(IMemAllocator *This, IMediaSample **sample,
				    REFERENCE_TIME *start, REFERENCE_TIME *end,
				    DWORD flags);
	HRESULT (WINAPI *ReleaseBuffer)(IMemAllocator *This,
					IMediaSample *sample);
} IMemAllocatorVtbl;
struct IMemAllocator {
	const IMemAllocatorVtbl *lpVtbl;
};

/* IMediaSample: one buffer of data, with its times and flags. */
typedef struct IMediaSampleVtbl {
	HRESULT (WINAPI *QueryInterface)(IMediaSample *This, REFIID riid,
					 void **ppv);
	ULONG (WINAPI *AddRef)(IMediaSample *This);
	ULONG (WINAPI *Release)(IMediaSample *This);
	HRESULT (WINAPI *GetPointer)(IMediaSample *This, BYTE **buffer);
	LONG (WINAPI *GetSize)(IMediaSample *This);
	HRESULT (WINAPI *GetTime)(IMediaSample *This, REFERENCE_TIME *start,
				  REFERENCE_TIME *end);
	HRESULT (WINAPI *SetTime)(IMediaSample *This, REFERENCE_TIME *start,
				  REFERENCE_TIME *end);
	HRESULT (WINAPI *IsSyncPoint)(IMediaSample *This);
	HRESULT (WINAPI *SetSyncPoint)(IMediaSample *This, BOOL sync);
	HRESULT (WINAPI *IsPreroll)(IMediaSample *This);
	HRESULT (WINAPI *SetPreroll)(IMediaSample *This, BOOL preroll);
	LONG (WINAPI *GetActualDataLength)(IMediaSample *This);
	HRESULT (WINAPI *SetActualDataLength)(IMediaSample *This, LONG length);
	HRESULT (WINAPI *GetMediaType)(IMediaSample *This,
				       AM_MEDIA_TYPE **mt);
	HRESULT (WINAPI *SetMediaType)(IMediaSample *This, AM_MEDIA_TYPE *mt);
	HRESULT (WINAPI *IsDiscontinuity)(IMediaSample *This);
	HRESULT (WINAPI *SetDiscontinuity)(IMediaSample *This,
					   BOOL discontinuity);
	HRESULT (WINAPI *GetMediaTime)(IMediaSample *This, LONGLONG *start,
				       LONGLONG *end);
	HRESULT (WINAPI *SetMediaTime)(IMediaSample *This, LONGLONG *start,
				       LONGLONG *end);
} IMediaSampleVtbl;
struct IMediaSample {
	const IMediaSampleVtbl *lpVtbl;
};

/* IMediaSample2: all of a sample's properties at once. */
typedef struct IMediaSample2 IMediaSample2;
typedef struct IMediaSample2Vtbl {
	HRESULT (WINAPI *QueryInterface)(IMediaSample2 *This, REFIID riid,
					 void **ppv);
	ULONG (WINAPI *AddRef)(IMediaSample2 *This);
	ULONG (WINAPI *Release)(IMediaSample2 *This);
	HRESULT (WINAPI *GetPointer)(IMediaSample2 *This, BYTE **buffer);
	LONG (WINAPI *GetSize)(IMediaSample2 *This);
	HRESULT (WINAPI *GetTime)(IMediaSample2 *This, REFERENCE_TIME *start,
				  REFERENCE_TIME *end);
	HRESULT (WINAPI *SetTime)(IMediaSample2 *This, REFERENCE_TIME *start,
				  REFERENCE_TIME *end);
	HRESULT (WINAPI *IsSyncPoint)(IMediaSample2 *This);
	HRESULT (WINAPI *SetSyncPoint)(IMediaSample2 *This, BOOL sync);
	HRESULT (WINAPI *IsPreroll)(IMediaSample2 *This);
	HRESULT (WINAPI *SetPreroll)(IMediaSample2 *This, BOOL preroll);
	LONG (WINAPI *GetActualDataLength)(IMediaSample2 *This);
	HRESULT (WINAPI *SetActualDataLength)(IMediaSample2 *This,
					      LONG length);
	HRESULT (WINAPI *GetMediaType)(IMediaSample2 *This,
				       AM_MEDIA_TYPE **mt);
	HRESULT (WINAPI *SetMediaType)(IMediaSample2 *This, AM_MEDIA_TYPE *mt);
	HRESULT (WINAPI *IsDiscontinuity)(IMediaSample2 *This);
	HRESULT (WINAPI *SetDiscontinuity)(IMediaSample2 *This,
					   BOOL discontinuity);
	HRESULT (WINAPI *GetMediaTime)(IMediaSample2 *This, LONGLONG *start,
				       LONGLONG *end);
	HRESULT (WINAPI *SetMediaTime)(IMediaSample2 *This, LONGLONG *start,
				       LONGLONG *end);
	HRESULT (WINAPI *GetProperties)(IMediaSample2 *This, DWORD size,
					BYTE *props);
	HRESULT (WINAPI *SetProperties)(IMediaSample2 *This, DWORD size,
					const BYTE *props);
} IMediaSample2Vtbl;
struct IMediaSample2 {
	const IMediaSample2Vtbl *lpVtbl;
};

/* IMemInputPin: the input pin's side of passing samples. */
typedef struct IMemInputPin IMemInputPin;
typedef struct IMemInputPinVtbl {
	HRESULT (WINAPI *QueryInterface)(IMemInputPin *This, REFIID riid,
					 void **ppv);
	ULONG (WINAPI *AddRef)(IMemInputPin *This);
	ULONG (WINAPI *Release)(IMemInputPin *This);
	HRESULT (WINAPI *GetAllocator)(IMemInputPin *This,
				       IMemAllocator **allocator);
	HRESULT (WINAPI *NotifyAllocator)(IMemInputPin *This,
					  IMemAllocator *allocator,
					  BOOL read_only);
	HRESULT (WINAPI *GetAllocatorRequirements)(IMemInputPin *This,
						   ALLOCATOR_PROPERTIES *props);
	HRESULT (WINAPI *Receive)(IMemInputPin *This, IMediaSample *sample);
	HRESULT (WINAPI *ReceiveMultiple)(IMemInputPin *This,
					  IMediaSample **samples, LONG count,
					  LONG *processed);
	HRESULT (WINAPI *ReceiveCanBlock)(IMemInputPin *This);
} IMemInputPinVtbl;
struct IMemInputPin {
	const IMemInputPinVtbl *lpVtbl;
};

/* IQualityControl: quality messages passed upstream. */
typedef struct IQualityControl IQualityControl;
typedef struct IQualityControlVtbl {
	HRESULT (WINAPI *QueryInterface)(IQualityControl *This, REFIID riid,
					 void **ppv);
	ULONG (WINAPI *AddRef)(IQualityControl *This);
	ULONG (WINAPI *Release)(IQualityControl *This);
	HRESULT (WINAPI *Notify)(IQualityControl *This, IBaseFilter *self,
				 Quality q);
	HRESULT (WINAPI *SetSink)(IQualityControl *This, IQualityControl *sink);
} IQualityControlVtbl;
struct IQualityControl {
	const IQualityControlVtbl *lpVtbl;
};

/* The legacy filter mapper. Its CLSIDs are passed by value. */
typedef struct IFilterMapper IFilterMapper;
typedef struct IFilterMapperVtbl {
	HRESULT (WINAPI *QueryInterface)(IFilterMapper *This, REFIID riid,
					 void **ppv);
	ULONG (WINAPI *AddRef)(IFilterMapper *This);
	ULONG (WINAPI *Release)(IFilterMapper *This);
	HRESULT (WINAPI *RegisterFilter)(IFilterMapper *This, CLSID clsid,
					 LPCWSTR name, DWORD merit);
	HRESULT (WINAPI *RegisterFilterInstance)(IFilterMapper *This,
						 CLSID clsid, LPCWSTR name,
						 CLSID *mrid);
	HRESULT (WINAPI *RegisterPin)(IFilterMapper *This, CLSID filter,
				      LPCWSTR name, BOOL rendered, BOOL output,
				      BOOL zero, BOOL many,
				      CLSID connects_to_filter,
				      LPCWSTR connects_to_pin);
	HRESULT (WINAPI *RegisterPinType)(IFilterMapper *This, CLSID filter,
					  LPCWSTR pin, CLSID major,
					  CLSID sub);
	HRESULT (WINAPI *UnregisterFilter)(IFilterMapper *This, CLSID filter);
	HRESULT (WINAPI *UnregisterFilterInstance)(IFilterMapper *This,
						   CLSID mrid);
	HRESULT (WINAPI *UnregisterPin)(IFilterMapper *This, CLSID filter,
					LPCWSTR name);
	HRESULT (WINAPI *EnumMatchingFilters)(IFilterMapper *This,
					      IEnumRegFilters **e, DWORD merit,
					      BOOL input_needed,
					      CLSID in_major, CLSID in_sub,
					      BOOL render, BOOL output_needed,
					      CLSID out_major, CLSID out_sub);
} IFilterMapperVtbl;
struct IFilterMapper {
	const IFilterMapperVtbl *lpVtbl;
};

typedef struct IFilterMapper2 IFilterMapper2;
typedef struct IFilterMapper2Vtbl {
	HRESULT (WINAPI *QueryInterface)(IFilterMapper2 *This, REFIID riid,
					 void **ppv);
	ULONG (WINAPI *AddRef)(IFilterMapper2 *This);
	ULONG (WINAPI *Release)(IFilterMapper2 *This);
	HRESULT (WINAPI *CreateCategory)(IFilterMapper2 *This,
					 REFCLSID category, DWORD merit,
					 LPCWSTR description);
	HRESULT (WINAPI *UnregisterFilter)(IFilterMapper2 *This,
					   const CLSID *category,
					   LPCOLESTR instance,
					   REFCLSID filter);
	HRESULT (WINAPI *RegisterFilter)(IFilterMapper2 *This,
					 REFCLSID filter, LPCWSTR name,
					 IMoniker **moniker,
					 const CLSID *category,
					 LPCOLESTR instance,
					 const REGFILTER2 *rf2);
	HRESULT (WINAPI *EnumMatchingFilters)(IFilterMapper2 *This,
					      IEnumMoniker **e, DWORD flags,
					      BOOL exact_match, DWORD merit,
					      BOOL input_needed,
					      DWORD n_input_types,
					      const GUID *input_types,
					      const REGPINMEDIUM *medium_in,
					      const CLSID *pin_category_in,
					      BOOL render, BOOL output_needed,
					      DWORD n_output_types,
					      const GUID *output_types,
					      const REGPINMEDIUM *medium_out,
					      const CLSID *pin_category_out);
} IFilterMapper2Vtbl;
struct IFilterMapper2 {
	const IFilterMapper2Vtbl *lpVtbl;
};

/* ICreateDevEnum: the system device enumerator. */
typedef struct ICreateDevEnum ICreateDevEnum;
typedef struct ICreateDevEnumVtbl {
	HRESULT (WINAPI *QueryInterface)(ICreateDevEnum *This, REFIID riid,
					 void **ppv);
	ULONG (WINAPI *AddRef)(ICreateDevEnum *This);
	ULONG (WINAPI *Release)(ICreateDevEnum *This);
	HRESULT (WINAPI *CreateClassEnumerator)(ICreateDevEnum *This,
						REFCLSID category,
						IEnumMoniker **e,
						DWORD flags);
} ICreateDevEnumVtbl;
struct ICreateDevEnum {
	const ICreateDevEnumVtbl *lpVtbl;
};

/*
 * ICodecAPI (icodecapi.h): an encoder's settings by GUID, the modern
 * equivalent of IAMVideoCompression. Values travel as VARIANTs.
 */
typedef struct ICodecAPI ICodecAPI;
typedef struct ICodecAPIVtbl {
	HRESULT (WINAPI *QueryInterface)(ICodecAPI *This, REFIID riid,
					 void **ppv);
	ULONG (WINAPI *AddRef)(ICodecAPI *This);
	ULONG (WINAPI *Release)(ICodecAPI *This);
	HRESULT (WINAPI *IsSupported)(ICodecAPI *This, const GUID *api);
	HRESULT (WINAPI *IsModifiable)(ICodecAPI *This, const GUID *api);
	HRESULT (WINAPI *GetParameterRange)(ICodecAPI *This, const GUID *api,
					    VARIANT *min, VARIANT *max,
					    VARIANT *step);
	HRESULT (WINAPI *GetParameterValues)(ICodecAPI *This, const GUID *api,
					     VARIANT **values, ULONG *count);
	HRESULT (WINAPI *GetDefaultValue)(ICodecAPI *This, const GUID *api,
					  VARIANT *value);
	HRESULT (WINAPI *GetValue)(ICodecAPI *This, const GUID *api,
				   VARIANT *value);
	HRESULT (WINAPI *SetValue)(ICodecAPI *This, const GUID *api,
				   VARIANT *value);
	HRESULT (WINAPI *RegisterForEvent)(ICodecAPI *This, const GUID *api,
					   LONG_PTR user_data);
	HRESULT (WINAPI *UnregisterForEvent)(ICodecAPI *This, const GUID *api);
	HRESULT (WINAPI *SetAllDefaults)(ICodecAPI *This);
	HRESULT (WINAPI *SetValueWithNotify)(ICodecAPI *This, const GUID *api,
					     VARIANT *value, GUID **changed,
					     ULONG *count);
	HRESULT (WINAPI *SetAllDefaultsWithNotify)(ICodecAPI *This,
						   GUID **changed, ULONG *count);
	HRESULT (WINAPI *GetAllSettings)(ICodecAPI *This, IStream *stream);
	HRESULT (WINAPI *SetAllSettings)(ICodecAPI *This, IStream *stream);
	HRESULT (WINAPI *SetAllSettingsWithNotify)(ICodecAPI *This,
						   IStream *stream,
						   GUID **changed, ULONG *count);
} ICodecAPIVtbl;
struct ICodecAPI {
	const ICodecAPIVtbl *lpVtbl;
};

/* CODECAPI_AVEncCommonRateControlMode's values (codecapi.h). */
enum {
	eAVEncCommonRateControlMode_CBR = 0,
	eAVEncCommonRateControlMode_PeakConstrainedVBR = 1,
	eAVEncCommonRateControlMode_UnconstrainedVBR = 2,
	eAVEncCommonRateControlMode_Quality = 3,
};

/* IAMVideoCompression: an encoder's keyframe and quality settings. */
typedef struct IAMVideoCompression IAMVideoCompression;
typedef struct IAMVideoCompressionVtbl {
	HRESULT (WINAPI *QueryInterface)(IAMVideoCompression *This,
					 REFIID riid, void **ppv);
	ULONG (WINAPI *AddRef)(IAMVideoCompression *This);
	ULONG (WINAPI *Release)(IAMVideoCompression *This);
	HRESULT (WINAPI *put_KeyFrameRate)(IAMVideoCompression *This,
					   LONG rate);
	HRESULT (WINAPI *get_KeyFrameRate)(IAMVideoCompression *This,
					   LONG *rate);
	HRESULT (WINAPI *put_PFramesPerKeyFrame)(IAMVideoCompression *This,
						 LONG count);
	HRESULT (WINAPI *get_PFramesPerKeyFrame)(IAMVideoCompression *This,
						 LONG *count);
	HRESULT (WINAPI *put_Quality)(IAMVideoCompression *This,
				      double quality);
	HRESULT (WINAPI *get_Quality)(IAMVideoCompression *This,
				      double *quality);
	HRESULT (WINAPI *put_WindowSize)(IAMVideoCompression *This,
					 DWORDLONG size);
	HRESULT (WINAPI *get_WindowSize)(IAMVideoCompression *This,
					 DWORDLONG *size);
	HRESULT (WINAPI *GetInfo)(IAMVideoCompression *This, LPWSTR version,
				  int *version_size, LPWSTR description,
				  int *description_size,
				  LONG *default_key_frame_rate,
				  LONG *default_p_frames_per_key,
				  double *default_quality,
				  LONG *capabilities);
	HRESULT (WINAPI *OverrideKeyFrame)(IAMVideoCompression *This,
					   LONG frame);
	HRESULT (WINAPI *OverrideFrameSize)(IAMVideoCompression *This,
					    LONG frame, LONG size);
} IAMVideoCompressionVtbl;
struct IAMVideoCompression {
	const IAMVideoCompressionVtbl *lpVtbl;
};

/* ISpecifyPropertyPages (ocidl.h): a filter's configuration pages. */
typedef struct ISpecifyPropertyPages ISpecifyPropertyPages;
typedef struct ISpecifyPropertyPagesVtbl {
	HRESULT (WINAPI *QueryInterface)(ISpecifyPropertyPages *This,
					 REFIID riid, void **ppv);
	ULONG (WINAPI *AddRef)(ISpecifyPropertyPages *This);
	ULONG (WINAPI *Release)(ISpecifyPropertyPages *This);
	HRESULT (WINAPI *GetPages)(ISpecifyPropertyPages *This, CAUUID *pages);
} ISpecifyPropertyPagesVtbl;
struct ISpecifyPropertyPages {
	const ISpecifyPropertyPagesVtbl *lpVtbl;
};

/*
 * The subtype for a FOURCC: {XXXXXXXX-0000-0010-8000-00AA00389B71}, with
 * the FOURCC as Data1. MEDIATYPE_Video is the same shape ('vids').
 */
#define FOURCC_SUBTYPE_INIT(fcc) \
	{ (fcc), 0x0000, 0x0010, \
	  { 0x80, 0x00, 0x00, 0xaa, 0x00, 0x38, 0x9b, 0x71 } }

/* The GUIDs of DirectShow, in VFW_GUID_DECLARE's form (see objbase.h). */
#define VFW_DSHOW_GUIDS(G) \
	/* Interfaces */ \
	G(IID_IMediaFilter, 0x56a86899, 0x0ad4, 0x11ce, \
	  0xb0, 0x3a, 0x00, 0x20, 0xaf, 0x0b, 0xa7, 0x70) \
	G(IID_IBaseFilter, 0x56a86895, 0x0ad4, 0x11ce, \
	  0xb0, 0x3a, 0x00, 0x20, 0xaf, 0x0b, 0xa7, 0x70) \
	G(IID_IPin, 0x56a86891, 0x0ad4, 0x11ce, \
	  0xb0, 0x3a, 0x00, 0x20, 0xaf, 0x0b, 0xa7, 0x70) \
	G(IID_IEnumPins, 0x56a86892, 0x0ad4, 0x11ce, \
	  0xb0, 0x3a, 0x00, 0x20, 0xaf, 0x0b, 0xa7, 0x70) \
	G(IID_IEnumMediaTypes, 0x89c31040, 0x846b, 0x11ce, \
	  0x97, 0xd3, 0x00, 0xaa, 0x00, 0x55, 0x59, 0x5a) \
	G(IID_IEnumFilters, 0x56a86893, 0x0ad4, 0x11ce, \
	  0xb0, 0x3a, 0x00, 0x20, 0xaf, 0x0b, 0xa7, 0x70) \
	G(IID_IFilterGraph, 0x56a8689f, 0x0ad4, 0x11ce, \
	  0xb0, 0x3a, 0x00, 0x20, 0xaf, 0x0b, 0xa7, 0x70) \
	G(IID_IMediaEventSink, 0x56a868a2, 0x0ad4, 0x11ce, \
	  0xb0, 0x3a, 0x00, 0x20, 0xaf, 0x0b, 0xa7, 0x70) \
	G(IID_IReferenceClock, 0x56a86897, 0x0ad4, 0x11ce, \
	  0xb0, 0x3a, 0x00, 0x20, 0xaf, 0x0b, 0xa7, 0x70) \
	G(IID_IMemInputPin, 0x56a8689d, 0x0ad4, 0x11ce, \
	  0xb0, 0x3a, 0x00, 0x20, 0xaf, 0x0b, 0xa7, 0x70) \
	G(IID_IMemAllocator, 0x56a8689c, 0x0ad4, 0x11ce, \
	  0xb0, 0x3a, 0x00, 0x20, 0xaf, 0x0b, 0xa7, 0x70) \
	G(IID_IMediaSample, 0x56a8689a, 0x0ad4, 0x11ce, \
	  0xb0, 0x3a, 0x00, 0x20, 0xaf, 0x0b, 0xa7, 0x70) \
	G(IID_IMediaSample2, 0x36b73884, 0xc2c8, 0x11cf, \
	  0x8b, 0x46, 0x00, 0x80, 0x5f, 0x6c, 0xef, 0x60) \
	G(IID_IQualityControl, 0x56a868a5, 0x0ad4, 0x11ce, \
	  0xb0, 0x3a, 0x00, 0x20, 0xaf, 0x0b, 0xa7, 0x70) \
	G(IID_IFilterMapper, 0x56a868a3, 0x0ad4, 0x11ce, \
	  0xb0, 0x3a, 0x00, 0x20, 0xaf, 0x0b, 0xa7, 0x70) \
	G(IID_IFilterMapper2, 0xb79bb0b0, 0x33c1, 0x11d1, \
	  0xab, 0xe1, 0x00, 0xa0, 0xc9, 0x05, 0xf3, 0x75) \
	G(IID_IEnumRegFilters, 0x56a868a4, 0x0ad4, 0x11ce, \
	  0xb0, 0x3a, 0x00, 0x20, 0xaf, 0x0b, 0xa7, 0x70) \
	G(IID_ICreateDevEnum, 0x29840822, 0x5b84, 0x11d0, \
	  0xbd, 0x3b, 0x00, 0xa0, 0xc9, 0x11, 0xce, 0x86) \
	G(IID_IAMVideoCompression, 0xc6e13343, 0x30ac, 0x11d0, \
	  0xa1, 0x8c, 0x00, 0xa0, 0xc9, 0x11, 0x89, 0x56) \
	G(IID_ISpecifyPropertyPages, 0xb196b28b, 0xbab4, 0x101a, \
	  0xb6, 0x9c, 0x00, 0xaa, 0x00, 0x34, 0x1d, 0x07) \
	G(IID_ICodecAPI, 0x901db4c7, 0x31ce, 0x41a2, \
	  0x85, 0xdc, 0x8f, 0xa0, 0xbf, 0x41, 0xb8, 0xda) \
	/* The ICodecAPI settings seance sets (codecapi.h) */ \
	G(CODECAPI_AVEncCommonRateControlMode, 0x1c0608e9, 0x370c, 0x4710, \
	  0x8a, 0x58, 0xcb, 0x61, 0x81, 0xc4, 0x24, 0x23) \
	G(CODECAPI_AVEncCommonQuality, 0xfcbf57a3, 0x7ea5, 0x4b0c, \
	  0x96, 0x44, 0x69, 0xb4, 0x0c, 0x39, 0xc3, 0x91) \
	G(CODECAPI_AVEncCommonMeanBitRate, 0xf7222374, 0x2144, 0x4815, \
	  0xb5, 0x50, 0xa3, 0x7f, 0x8e, 0x12, 0xee, 0x52) \
	G(CODECAPI_AVEncMPVGOPSize, 0x95f31b26, 0x95a4, 0x41aa, \
	  0x93, 0x03, 0x24, 0x6a, 0x7f, 0xc6, 0xee, 0xf1) \
	/* Classes and categories */ \
	G(CLSID_FilterMapper, 0xe436ebb2, 0x524f, 0x11ce, \
	  0x9f, 0x53, 0x00, 0x20, 0xaf, 0x0b, 0xa7, 0x70) \
	G(CLSID_FilterMapper2, 0xcda42200, 0xbd88, 0x11d0, \
	  0xbd, 0x4e, 0x00, 0xa0, 0xc9, 0x11, 0xce, 0x86) \
	G(CLSID_SystemDeviceEnum, 0x62be5d10, 0x60eb, 0x11d0, \
	  0xbd, 0x3b, 0x00, 0xa0, 0xc9, 0x11, 0xce, 0x86) \
	G(CLSID_MemoryAllocator, 0x1e651cc0, 0xb199, 0x11d0, \
	  0x82, 0x12, 0x00, 0xc0, 0x4f, 0xc3, 0x2c, 0x45) \
	G(CLSID_ActiveMovieCategories, 0xda4e3da0, 0xd07d, 0x11d0, \
	  0xbd, 0x50, 0x00, 0xa0, 0xc9, 0x11, 0xce, 0x86) \
	G(CLSID_LegacyAmFilterCategory, 0x083863f1, 0x70de, 0x11d0, \
	  0xbd, 0x40, 0x00, 0xa0, 0xc9, 0x11, 0xce, 0x86) \
	G(CLSID_VideoCompressorCategory, 0x33d9a760, 0x90c8, 0x11d0, \
	  0xbd, 0x43, 0x00, 0xa0, 0xc9, 0x11, 0xce, 0x86) \
	/* Major types and formats */ \
	G(MEDIATYPE_Video, 0x73646976, 0x0000, 0x0010, \
	  0x80, 0x00, 0x00, 0xaa, 0x00, 0x38, 0x9b, 0x71) \
	G(FORMAT_None, 0x0f6417d6, 0xc318, 0x11d0, \
	  0xa4, 0x3f, 0x00, 0xa0, 0xc9, 0x22, 0x31, 0x96) \
	G(FORMAT_VideoInfo, 0x05589f80, 0xc356, 0x11ce, \
	  0xbf, 0x01, 0x00, 0xaa, 0x00, 0x55, 0x59, 0x5a) \
	G(FORMAT_VideoInfo2, 0xf72a76a0, 0xeb0a, 0x11d0, \
	  0xac, 0xe4, 0x00, 0x00, 0xc0, 0xcc, 0x16, 0xba) \
	G(MEDIASUBTYPE_MPEG2_VIDEO, 0xe06d8026, 0xdb46, 0x11cf, \
	  0xb4, 0xd1, 0x00, 0x80, 0x5f, 0x6c, 0xbb, 0xea) \
	G(MEDIASUBTYPE_MPEG1Video, 0xe436eb86, 0x524f, 0x11ce, \
	  0x9f, 0x53, 0x00, 0x20, 0xaf, 0x0b, 0xa7, 0x70) \
	G(FORMAT_MPEGVideo, 0x05589f82, 0xc356, 0x11ce, \
	  0xbf, 0x01, 0x00, 0xaa, 0x00, 0x55, 0x59, 0x5a) \
	G(FORMAT_MPEG2Video, 0xe06d80e3, 0xdb46, 0x11cf, \
	  0xb4, 0xd1, 0x00, 0x80, 0x5f, 0x6c, 0xbb, 0xea) \
	/* Uncompressed subtypes that aren't FOURCC-shaped */ \
	G(MEDIASUBTYPE_None, 0xe436eb8e, 0x524f, 0x11ce, \
	  0x9f, 0x53, 0x00, 0x20, 0xaf, 0x0b, 0xa7, 0x70) \
	G(MEDIASUBTYPE_RGB1, 0xe436eb78, 0x524f, 0x11ce, \
	  0x9f, 0x53, 0x00, 0x20, 0xaf, 0x0b, 0xa7, 0x70) \
	G(MEDIASUBTYPE_RGB4, 0xe436eb79, 0x524f, 0x11ce, \
	  0x9f, 0x53, 0x00, 0x20, 0xaf, 0x0b, 0xa7, 0x70) \
	G(MEDIASUBTYPE_RGB8, 0xe436eb7a, 0x524f, 0x11ce, \
	  0x9f, 0x53, 0x00, 0x20, 0xaf, 0x0b, 0xa7, 0x70) \
	G(MEDIASUBTYPE_RGB565, 0xe436eb7b, 0x524f, 0x11ce, \
	  0x9f, 0x53, 0x00, 0x20, 0xaf, 0x0b, 0xa7, 0x70) \
	G(MEDIASUBTYPE_RGB555, 0xe436eb7c, 0x524f, 0x11ce, \
	  0x9f, 0x53, 0x00, 0x20, 0xaf, 0x0b, 0xa7, 0x70) \
	G(MEDIASUBTYPE_RGB24, 0xe436eb7d, 0x524f, 0x11ce, \
	  0x9f, 0x53, 0x00, 0x20, 0xaf, 0x0b, 0xa7, 0x70) \
	G(MEDIASUBTYPE_RGB32, 0xe436eb7e, 0x524f, 0x11ce, \
	  0x9f, 0x53, 0x00, 0x20, 0xaf, 0x0b, 0xa7, 0x70) \
	G(MEDIASUBTYPE_ARGB32, 0x773c9ac0, 0x3274, 0x11d0, \
	  0xb7, 0x24, 0x00, 0xaa, 0x00, 0x6c, 0x1a, 0x01) \
	/* FOURCC subtypes, by name */ \
	G(MEDIASUBTYPE_YUY2, 0x32595559, 0x0000, 0x0010, \
	  0x80, 0x00, 0x00, 0xaa, 0x00, 0x38, 0x9b, 0x71) \
	G(MEDIASUBTYPE_UYVY, 0x59565955, 0x0000, 0x0010, \
	  0x80, 0x00, 0x00, 0xaa, 0x00, 0x38, 0x9b, 0x71) \
	G(MEDIASUBTYPE_YVYU, 0x55595659, 0x0000, 0x0010, \
	  0x80, 0x00, 0x00, 0xaa, 0x00, 0x38, 0x9b, 0x71) \
	G(MEDIASUBTYPE_YV12, 0x32315659, 0x0000, 0x0010, \
	  0x80, 0x00, 0x00, 0xaa, 0x00, 0x38, 0x9b, 0x71) \
	G(MEDIASUBTYPE_IYUV, 0x56555949, 0x0000, 0x0010, \
	  0x80, 0x00, 0x00, 0xaa, 0x00, 0x38, 0x9b, 0x71) \
	G(MEDIASUBTYPE_NV12, 0x3231564e, 0x0000, 0x0010, \
	  0x80, 0x00, 0x00, 0xaa, 0x00, 0x38, 0x9b, 0x71)

VFW_DSHOW_GUIDS(VFW_GUID_DECLARE)

#endif /* VFW_WIN32_DSHOW_H */
