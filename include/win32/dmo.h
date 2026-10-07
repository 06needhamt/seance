/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * DirectX Media Objects (mediaobj.h, dmoreg.h, mediaerr.h, and
 * IWMCodecPrivateData from wmcodecdsp.h): a codec the caller drives directly,
 * setting an input and an output type and then passing buffers in with
 * ProcessInput and taking results out with ProcessOutput. There is no graph.
 * Interfaces are declared as in objbase.h.
 */
#ifndef VFW_WIN32_DMO_H
#define VFW_WIN32_DMO_H

#include "dshow.h"
#include "objbase.h"
#include "types.h"

/* The same structure as AM_MEDIA_TYPE, and freed the same way. */
typedef AM_MEDIA_TYPE DMO_MEDIA_TYPE;

/* Errors (mediaerr.h). */
#define DMO_E_INVALIDSTREAMINDEX ((HRESULT)0x80040201)
#define DMO_E_INVALIDTYPE        ((HRESULT)0x80040202)
#define DMO_E_TYPE_NOT_SET       ((HRESULT)0x80040203)
#define DMO_E_NOTACCEPTING       ((HRESULT)0x80040204)
#define DMO_E_TYPE_NOT_ACCEPTED  ((HRESULT)0x80040205)
#define DMO_E_NO_MORE_ITEMS      ((HRESULT)0x80040206)

/*
 * GetInputStreamInfo and GetOutputStreamInfo flags. mingw-w64's mediaobj.h
 * leaves these out; the values are the Windows SDK's (and listed in
 * tests/abi/constants_skip.txt for that reason).
 */
#define DMO_INPUT_STREAMF_WHOLE_SAMPLES            0x1
#define DMO_INPUT_STREAMF_SINGLE_SAMPLE_PER_BUFFER 0x2
#define DMO_INPUT_STREAMF_FIXED_SAMPLE_SIZE        0x4
#define DMO_INPUT_STREAMF_HOLDS_BUFFERS            0x8
#define DMO_OUTPUT_STREAMF_WHOLE_SAMPLES            0x1
#define DMO_OUTPUT_STREAMF_SINGLE_SAMPLE_PER_BUFFER 0x2
#define DMO_OUTPUT_STREAMF_FIXED_SAMPLE_SIZE        0x4
#define DMO_OUTPUT_STREAMF_DISCARDABLE              0x8
#define DMO_OUTPUT_STREAMF_OPTIONAL                 0x10

/* GetInputStatus. */
#define DMO_INPUT_STATUSF_ACCEPT_DATA 0x1

/* ProcessInput's dwFlags. */
#define DMO_INPUT_DATA_BUFFERF_SYNCPOINT  0x1
#define DMO_INPUT_DATA_BUFFERF_TIME       0x2
#define DMO_INPUT_DATA_BUFFERF_TIMELENGTH 0x4

/* ProcessOutput's dwFlags, and each DMO_OUTPUT_DATA_BUFFER.dwStatus. */
#define DMO_PROCESS_OUTPUT_DISCARD_WHEN_NO_BUFFER 0x1
#define DMO_OUTPUT_DATA_BUFFERF_SYNCPOINT  0x1
#define DMO_OUTPUT_DATA_BUFFERF_TIME       0x2
#define DMO_OUTPUT_DATA_BUFFERF_TIMELENGTH 0x4
#define DMO_OUTPUT_DATA_BUFFERF_INCOMPLETE 0x1000000

/* SetInputType and SetOutputType. */
#define DMO_SET_TYPEF_TEST_ONLY 0x1
#define DMO_SET_TYPEF_CLEAR     0x2

/* IMediaObjectInPlace::Process. */
#define DMO_INPLACE_NORMAL 0x0
#define DMO_INPLACE_ZERO   0x1

/* IDMOQualityControl and IDMOVideoOutputOptimizations. */
#define DMO_QUALITY_STATUS_ENABLED     0x1
#define DMO_VOSF_NEEDS_PREVIOUS_SAMPLE 0x1

/* DMORegister and DMOEnum (dmoreg.h). */
#define DMO_REGISTERF_IS_KEYED  1
#define DMO_ENUMF_INCLUDE_KEYED 1

/* A type a DMO registers: major type and subtype, no format. */
typedef struct _DMO_PARTIAL_MEDIATYPE {
	GUID type;
	GUID subtype;
} DMO_PARTIAL_MEDIATYPE, *PDMO_PARTIAL_MEDIATYPE;

/* IMediaBuffer: a block of memory and how much of it holds data. */
typedef struct IMediaBuffer IMediaBuffer;
typedef struct IMediaBufferVtbl {
	HRESULT (WINAPI *QueryInterface)(IMediaBuffer *This, REFIID riid,
					 void **ppv);
	ULONG (WINAPI *AddRef)(IMediaBuffer *This);
	ULONG (WINAPI *Release)(IMediaBuffer *This);
	HRESULT (WINAPI *SetLength)(IMediaBuffer *This, DWORD length);
	HRESULT (WINAPI *GetMaxLength)(IMediaBuffer *This, DWORD *max_length);
	HRESULT (WINAPI *GetBufferAndLength)(IMediaBuffer *This, BYTE **buffer,
					     DWORD *length);
} IMediaBufferVtbl;
struct IMediaBuffer {
	const IMediaBufferVtbl *lpVtbl;
};

/* One output stream's buffer for ProcessOutput, and what came back in it. */
typedef struct _DMO_OUTPUT_DATA_BUFFER {
	IMediaBuffer *pBuffer;
	DWORD dwStatus;
	REFERENCE_TIME rtTimestamp;
	REFERENCE_TIME rtTimelength;
} DMO_OUTPUT_DATA_BUFFER, *PDMO_OUTPUT_DATA_BUFFER;

/* IMediaObject: the codec itself. */
typedef struct IMediaObject IMediaObject;
typedef struct IMediaObjectVtbl {
	HRESULT (WINAPI *QueryInterface)(IMediaObject *This, REFIID riid,
					 void **ppv);
	ULONG (WINAPI *AddRef)(IMediaObject *This);
	ULONG (WINAPI *Release)(IMediaObject *This);
	HRESULT (WINAPI *GetStreamCount)(IMediaObject *This, DWORD *inputs,
					 DWORD *outputs);
	HRESULT (WINAPI *GetInputStreamInfo)(IMediaObject *This, DWORD stream,
					     DWORD *flags);
	HRESULT (WINAPI *GetOutputStreamInfo)(IMediaObject *This, DWORD stream,
					      DWORD *flags);
	HRESULT (WINAPI *GetInputType)(IMediaObject *This, DWORD stream,
				       DWORD index, DMO_MEDIA_TYPE *mt);
	HRESULT (WINAPI *GetOutputType)(IMediaObject *This, DWORD stream,
					DWORD index, DMO_MEDIA_TYPE *mt);
	HRESULT (WINAPI *SetInputType)(IMediaObject *This, DWORD stream,
				       const DMO_MEDIA_TYPE *mt, DWORD flags);
	HRESULT (WINAPI *SetOutputType)(IMediaObject *This, DWORD stream,
					const DMO_MEDIA_TYPE *mt, DWORD flags);
	HRESULT (WINAPI *GetInputCurrentType)(IMediaObject *This, DWORD stream,
					      DMO_MEDIA_TYPE *mt);
	HRESULT (WINAPI *GetOutputCurrentType)(IMediaObject *This, DWORD stream,
					       DMO_MEDIA_TYPE *mt);
	HRESULT (WINAPI *GetInputSizeInfo)(IMediaObject *This, DWORD stream,
					   DWORD *size, DWORD *max_lookahead,
					   DWORD *alignment);
	HRESULT (WINAPI *GetOutputSizeInfo)(IMediaObject *This, DWORD stream,
					    DWORD *size, DWORD *alignment);
	HRESULT (WINAPI *GetInputMaxLatency)(IMediaObject *This, DWORD stream,
					     REFERENCE_TIME *latency);
	HRESULT (WINAPI *SetInputMaxLatency)(IMediaObject *This, DWORD stream,
					     REFERENCE_TIME latency);
	HRESULT (WINAPI *Flush)(IMediaObject *This);
	HRESULT (WINAPI *Discontinuity)(IMediaObject *This, DWORD stream);
	HRESULT (WINAPI *AllocateStreamingResources)(IMediaObject *This);
	HRESULT (WINAPI *FreeStreamingResources)(IMediaObject *This);
	HRESULT (WINAPI *GetInputStatus)(IMediaObject *This, DWORD stream,
					 DWORD *flags);
	HRESULT (WINAPI *ProcessInput)(IMediaObject *This, DWORD stream,
				       IMediaBuffer *buffer, DWORD flags,
				       REFERENCE_TIME timestamp,
				       REFERENCE_TIME timelength);
	HRESULT (WINAPI *ProcessOutput)(IMediaObject *This, DWORD flags,
					DWORD count,
					DMO_OUTPUT_DATA_BUFFER *buffers,
					DWORD *status);
	HRESULT (WINAPI *Lock)(IMediaObject *This, LONG lock);
} IMediaObjectVtbl;
struct IMediaObject {
	const IMediaObjectVtbl *lpVtbl;
};

/* IEnumDMO: what DMOEnum finds, as CLSIDs and names. */
typedef struct IEnumDMO IEnumDMO;
typedef struct IEnumDMOVtbl {
	HRESULT (WINAPI *QueryInterface)(IEnumDMO *This, REFIID riid,
					 void **ppv);
	ULONG (WINAPI *AddRef)(IEnumDMO *This);
	ULONG (WINAPI *Release)(IEnumDMO *This);
	HRESULT (WINAPI *Next)(IEnumDMO *This, DWORD count, CLSID *clsids,
			       WCHAR **names, DWORD *fetched);
	HRESULT (WINAPI *Skip)(IEnumDMO *This, DWORD count);
	HRESULT (WINAPI *Reset)(IEnumDMO *This);
	HRESULT (WINAPI *Clone)(IEnumDMO *This, IEnumDMO **copy);
} IEnumDMOVtbl;
struct IEnumDMO {
	const IEnumDMOVtbl *lpVtbl;
};

/* IMediaObjectInPlace: effects that work on the buffer they are given. */
typedef struct IMediaObjectInPlace IMediaObjectInPlace;
typedef struct IMediaObjectInPlaceVtbl {
	HRESULT (WINAPI *QueryInterface)(IMediaObjectInPlace *This,
					 REFIID riid, void **ppv);
	ULONG (WINAPI *AddRef)(IMediaObjectInPlace *This);
	ULONG (WINAPI *Release)(IMediaObjectInPlace *This);
	HRESULT (WINAPI *Process)(IMediaObjectInPlace *This, ULONG size,
				  BYTE *data, REFERENCE_TIME start,
				  DWORD flags);
	HRESULT (WINAPI *Clone)(IMediaObjectInPlace *This,
				IMediaObjectInPlace **copy);
	HRESULT (WINAPI *GetLatency)(IMediaObjectInPlace *This,
				     REFERENCE_TIME *latency);
} IMediaObjectInPlaceVtbl;
struct IMediaObjectInPlace {
	const IMediaObjectInPlaceVtbl *lpVtbl;
};

/* IDMOQualityControl: lets a DMO drop late samples. */
typedef struct IDMOQualityControl IDMOQualityControl;
typedef struct IDMOQualityControlVtbl {
	HRESULT (WINAPI *QueryInterface)(IDMOQualityControl *This, REFIID riid,
					 void **ppv);
	ULONG (WINAPI *AddRef)(IDMOQualityControl *This);
	ULONG (WINAPI *Release)(IDMOQualityControl *This);
	HRESULT (WINAPI *SetNow)(IDMOQualityControl *This, REFERENCE_TIME now);
	HRESULT (WINAPI *SetStatus)(IDMOQualityControl *This, DWORD flags);
	HRESULT (WINAPI *GetStatus)(IDMOQualityControl *This, DWORD *flags);
} IDMOQualityControlVtbl;
struct IDMOQualityControl {
	const IDMOQualityControlVtbl *lpVtbl;
};

/* IDMOVideoOutputOptimizations: whether output buffers may be reused. */
typedef struct IDMOVideoOutputOptimizations IDMOVideoOutputOptimizations;
typedef struct IDMOVideoOutputOptimizationsVtbl {
	HRESULT (WINAPI *QueryInterface)(IDMOVideoOutputOptimizations *This,
					 REFIID riid, void **ppv);
	ULONG (WINAPI *AddRef)(IDMOVideoOutputOptimizations *This);
	ULONG (WINAPI *Release)(IDMOVideoOutputOptimizations *This);
	HRESULT (WINAPI *QueryOperationModePreferences)(
		IDMOVideoOutputOptimizations *This, ULONG stream, DWORD *flags);
	HRESULT (WINAPI *SetOperationMode)(IDMOVideoOutputOptimizations *This,
					   ULONG stream, DWORD flags);
	HRESULT (WINAPI *GetCurrentOperationMode)(
		IDMOVideoOutputOptimizations *This, ULONG stream, DWORD *flags);
	HRESULT (WINAPI *GetCurrentSampleRequirements)(
		IDMOVideoOutputOptimizations *This, ULONG stream, DWORD *flags);
} IDMOVideoOutputOptimizationsVtbl;
struct IDMOVideoOutputOptimizations {
	const IDMOVideoOutputOptimizationsVtbl *lpVtbl;
};

/*
 * IWMCodecPrivateData (wmcodecdsp.h): how a Windows Media encoder hands
 * over the codec data a container stores with the stream. Set a partial
 * output type, then read the data, then set the full output type with it.
 * mingw-w64 doesn't declare it, so check_abi can't compare it.
 */
typedef struct IWMCodecPrivateData IWMCodecPrivateData;
typedef struct IWMCodecPrivateDataVtbl {
	HRESULT (WINAPI *QueryInterface)(IWMCodecPrivateData *This,
					 REFIID riid, void **ppv);
	ULONG (WINAPI *AddRef)(IWMCodecPrivateData *This);
	ULONG (WINAPI *Release)(IWMCodecPrivateData *This);
	HRESULT (WINAPI *SetPartialOutputType)(IWMCodecPrivateData *This,
					       DMO_MEDIA_TYPE *mt);
	HRESULT (WINAPI *GetPrivateData)(IWMCodecPrivateData *This,
					 BYTE *data, DWORD *size);
} IWMCodecPrivateDataVtbl;
struct IWMCodecPrivateData {
	const IWMCodecPrivateDataVtbl *lpVtbl;
};

/* The GUIDs of DMOs, in VFW_GUID_DECLARE's form (see objbase.h). */
#define VFW_DMO_GUIDS(G) \
	/* Interfaces */ \
	G(IID_IMediaBuffer, 0x59eff8b9, 0x938c, 0x4a26, \
	  0x82, 0xf2, 0x95, 0xcb, 0x84, 0xcd, 0xc8, 0x37) \
	G(IID_IMediaObject, 0xd8ad0f58, 0x5494, 0x4102, \
	  0x97, 0xc5, 0xec, 0x79, 0x8e, 0x59, 0xbc, 0xf4) \
	G(IID_IEnumDMO, 0x2c3cd98a, 0x2bfa, 0x4a53, \
	  0x9c, 0x27, 0x52, 0x49, 0xba, 0x64, 0xba, 0x0f) \
	G(IID_IMediaObjectInPlace, 0x651b9ad0, 0x0fc7, 0x4aa9, \
	  0x95, 0x38, 0xd8, 0x99, 0x31, 0x01, 0x07, 0x41) \
	G(IID_IDMOQualityControl, 0x65abea96, 0xcf36, 0x453f, \
	  0xaf, 0x8a, 0x70, 0x5e, 0x98, 0xf1, 0x62, 0x60) \
	G(IID_IDMOVideoOutputOptimizations, 0xbe8f4f4e, 0x5b16, 0x4d29, \
	  0xb3, 0x50, 0x7f, 0x6b, 0x5d, 0x92, 0x98, 0xac) \
	/* Categories */ \
	G(DMOCATEGORY_AUDIO_DECODER, 0x57f2db8b, 0xe6bb, 0x4513, \
	  0x9d, 0x43, 0xdc, 0xd2, 0xa6, 0x59, 0x31, 0x25) \
	G(DMOCATEGORY_AUDIO_ENCODER, 0x33d9a761, 0x90c8, 0x11d0, \
	  0xbd, 0x43, 0x00, 0xa0, 0xc9, 0x11, 0xce, 0x86) \
	G(DMOCATEGORY_VIDEO_DECODER, 0x4a69b442, 0x28be, 0x4991, \
	  0x96, 0x9c, 0xb5, 0x00, 0xad, 0xf5, 0xd8, 0xa8) \
	G(DMOCATEGORY_VIDEO_ENCODER, 0x33d9a760, 0x90c8, 0x11d0, \
	  0xbd, 0x43, 0x00, 0xa0, 0xc9, 0x11, 0xce, 0x86) \
	G(DMOCATEGORY_AUDIO_EFFECT, 0xf3602b3f, 0x0592, 0x48df, \
	  0xa4, 0xcd, 0x67, 0x47, 0x21, 0xe7, 0xeb, 0xeb) \
	G(DMOCATEGORY_VIDEO_EFFECT, 0xd990ee14, 0x776c, 0x4723, \
	  0xbe, 0x46, 0x3d, 0xa2, 0xf5, 0x6f, 0x10, 0xb9) \
	G(DMOCATEGORY_AUDIO_CAPTURE_EFFECT, 0xf665aaba, 0x3e09, 0x4920, \
	  0xaa, 0x5f, 0x21, 0x98, 0x11, 0x14, 0x8f, 0x09) \
	G(DMOCATEGORY_ACOUSTIC_ECHO_CANCEL, 0xbf963d80, 0xc559, 0x11d0, \
	  0x8a, 0x2b, 0x00, 0xa0, 0xc9, 0x25, 0x5a, 0xc1) \
	G(DMOCATEGORY_AUDIO_NOISE_SUPPRESS, 0xe07f903f, 0x62fd, 0x4e60, \
	  0x8c, 0xdd, 0xde, 0xa7, 0x23, 0x66, 0x65, 0xb5) \
	G(DMOCATEGORY_AGC, 0xe88c9ba0, 0xc557, 0x11d0, \
	  0x8a, 0x2b, 0x00, 0xa0, 0xc9, 0x25, 0x5a, 0xc1)

/*
 * GUIDs mingw-w64 doesn't define, so check_abi can't compare them: from the
 * Windows SDK's wmcodecdsp.h.
 */
#define VFW_DMO_SDK_GUIDS(G) \
	G(IID_IWMCodecPrivateData, 0x73f0be8e, 0x57f7, 0x4f01, \
	  0xaa, 0x66, 0x9f, 0x57, 0x34, 0x0c, 0xfe, 0x0e)

VFW_DMO_GUIDS(VFW_GUID_DECLARE)
VFW_DMO_SDK_GUIDS(VFW_GUID_DECLARE)

#endif /* VFW_WIN32_DMO_H */
