<!-- SPDX-License-Identifier: GPL-2.0-only -->
# seance: DMO and audio codec support plan

## Context

seance drives two kinds of 32-bit Windows codec today: VfW/ICM drivers through
`DriverProc`, and DirectShow video transform filters through a minimal graph
of its own (`docs/DIRECT_SHOW_PLAN.md`). Two kinds of codec that were left out
of that work are the subject of this plan:

- **DirectX Media Objects (DMOs).** In-process COM objects implementing
  `IMediaObject`: the caller sets an input and an output type, then pushes
  `IMediaBuffer`s in with `ProcessInput` and pulls results out with
  `ProcessOutput`. There is no graph, no pins, no allocator negotiation and
  no filter-owned thread. Windows' own WMV, VC-1, WMA and MPEG decoders and
  encoders ship as DMOs, as do many third-party codecs written after 2000.
  DMOs register with `DMORegister` (msdmo.dll) under
  `HKCR\DirectShow\MediaObjects`.
- **Audio decoders and encoders**, as DirectShow transform filters (for
  example LAV Audio, already loading and registering) and as DMOs. They take
  and produce `WAVEFORMATEX`/`WAVEFORMATEXTENSIBLE` media types and are timed
  in samples rather than frames.

Proposed decisions (to confirm before Phase 0):
- **Order:** DMOs first, video before audio, because a DMO is the smallest
  host of the three and reuses the media-type and output code as it stands.
  Audio then comes once for both backends.
- **Scope:** decoders and encoders only, as before. Splitters, sources,
  renderers, capture devices, Media Foundation transforms (`IMFTransform`)
  and the DMO wrapper filter (`CLSID_DMOWrapperFilter`) stay out.
- **CLI:** no new commands besides what registration needs; `probe`,
  `decode`, `encode`, `list`, `register` and `unregister` learn DMOs, and
  `decode`/`encode` learn audio streams.
- **Tests:** in-repo fixtures only (a fixture DMO and a fixture audio codec);
  real DLLs only through `tests/samples.sh`, as now.

What already exists and is reused:
- Everything the DirectShow work reused: PE loader, built-in DLLs, threads
  and synchronisation, ole32's in-process server loading, the registry and its
  throw-away overlay, crash/hang framing, `TRACE`.
- From the DirectShow backend: `AM_MEDIA_TYPE` helpers and the
  `BITMAPINFOHEADER` conversions (`src/dshow/mediatype.c`; `DMO_MEDIA_TYPE`
  has the same layout), `dshow_input_variant`'s VideoInfo/VideoInfo2/MPEG
  forms, `ICodecAPI` and `IAMVideoCompression` controls (`control.c`), the
  `DllRegisterServer` capture used by discovery (`discover.c`), traced
  COM calls (`call.c`) and the `cmd_decode.c` output side (`--start`,
  `--frames`, dynamic format changes, writers).
- The AVI reader (`src/avi/`), which already parses every stream's `strh`
  and `strf` and indexes their chunks, audio included; `WAVEFORMATEX` in
  `include/win32/mmsystem.h`.

## Architecture

```
cli: probe | decode | encode | list | register | unregister
 ├─ vfw/       ICM backend (unchanged)
 ├─ dshow/     DirectShow backend; host generalised from video to any major type
 └─ dmo/       new DMO backend
      ├─ buffer.c     IMediaBuffer (harness side)
      ├─ discover.c   find a DLL's DMOs (DMORegister capture), registry lookup
      ├─ host.c       type negotiation, ProcessInput/ProcessOutput loop
      └─ dmo.h        typed C interface used by the commands
media/         shared: video and audio media types, preference lists
in/  out/      + WAV reader, AVI audio streams; + WAV and raw PCM writers
win32/msdmo.c  built-in msdmo.dll (DMORegister, DMOEnum, Mo*MediaType)
include/win32/dmo.h      mediaobj.h/dmoreg.h definitions
include/win32/mmreg.h    WAVEFORMATEXTENSIBLE, format tags, KSDATAFORMAT GUIDs
```

### 1. Headers and ABI

- `include/win32/dmo.h`: `IMediaObject`, `IMediaObjectInPlace`,
  `IMediaBuffer`, `IEnumDMO`, `IDMOQualityControl`,
  `IDMOVideoOutputOptimizations`; `DMO_MEDIA_TYPE` (a typedef of
  `AM_MEDIA_TYPE`), `DMO_OUTPUT_DATA_BUFFER`, `DMO_PARTIAL_MEDIATYPE`;
  the `DMO_INPUT_DATA_BUFFERF_*`, `DMO_OUTPUT_DATA_BUFFERF_*`,
  `DMO_INPUT_STATUSF_*`, `DMO_SET_TYPEF_*`, `DMO_PROCESS_OUTPUT_*`,
  `DMO_INPUT_STREAMF_*`/`DMO_OUTPUT_STREAMF_*` flags; `DMO_E_*` results;
  `DMOCATEGORY_VIDEO_DECODER`, `_VIDEO_ENCODER`, `_AUDIO_DECODER`,
  `_AUDIO_ENCODER` (and the effect categories, for listing only).
- `IWMCodecPrivateData` and `IPropertyBag` (encoder settings and extradata
  for Windows Media encoders), `IPropertyStore` if the samples run shows a
  need.
- `include/win32/mmreg.h`: `WAVEFORMATEXTENSIBLE`, `WAVE_FORMAT_*` tags
  (PCM, IEEE_FLOAT, EXTENSIBLE, MPEG, MPEGLAYER3, ...), speaker masks,
  `KSDATAFORMAT_SUBTYPE_PCM`/`_IEEE_FLOAT`.
- In `dshow.h`: `FORMAT_WaveFormatEx`, `MEDIATYPE_Audio`, the audio
  subtypes (PCM, IEEE_FLOAT, and the format-tag template
  `{tag-0000-0010-8000-00AA00389B71}`).
- All structures, constants and GUIDs go into `tests/abi/abi_list.h` and the
  GUID list, checked against mingw's `mediaobj.h`, `dmo.h`, `dmoreg.h`,
  `mmreg.h`, `ksmedia.h` and `uuids.h`.

### 2. Win32 runtime additions

- **msdmo.dll built-in** (`src/win32/msdmo.c`):
  - `DMORegister`/`DMOUnregister` writing what Windows writes:
    `HKCR\DirectShow\MediaObjects\{clsid}` (default value the name,
    `InputTypes`/`OutputTypes` as `REG_BINARY` pairs of GUIDs, `Flags`
    when non-zero) and `...\MediaObjects\Categories\{category}\{clsid}`.
  - A **capture mode**, like quartz's, so discovery records each
    registration in memory instead of writing the registry.
  - `DMOEnum` (by category, with an input/output type filter and the
    `DMO_ENUMF_INCLUDE_KEYED` flag) returning an `IEnumDMO`; `DMOGetTypes`,
    `DMOGetName`.
  - `MoInitMediaType`, `MoFreeMediaType`, `MoCopyMediaType`,
    `MoCreateMediaType`, `MoDeleteMediaType`, `MoDuplicateMediaType`, on the
    same CoTaskMem allocation as `dshow_free_media_type`.
- What else real DMOs import is found with `info`. Windows' own codec DLLs
  (`wmvdecod.dll`, `wmadmod.dll`, `msmpeg2vdec.dll`, ...) are both DMOs and
  Media Foundation transforms and import `mfplat.dll`; they would need a
  `mfplat` built-in that reports no Media Foundation, the way d3d9 reports no
  adapter. That is decided by the samples run, not assumed.
- **Audio:** nothing in the runtime is expected; decoders don't open audio
  devices. `waveOut*` stubs already exist.

### 3. DMO backend (`src/dmo/`)

- **`buffer.c`:** a harness `IMediaBuffer` over a heap block below 2 GB:
  `SetLength`, `GetMaxLength`, `GetBufferAndLength`, with refcounting checked
  in tests (a DMO may keep an input buffer until `ProcessOutput` has drained
  it, which `DMO_INPUT_STATUSF_ACCEPT_DATA` tells us).
- **`discover.c`:** a DMO server is recognised the same way as a filter
  server (it exports `DllGetClassObject` and not `DriverProc`); what decides
  is what `DllRegisterServer` registers — DMOs through `DMORegister`, filters
  through the filter mapper — and, for a class given with `--clsid`, whether
  the object answers `IMediaObject` or `IBaseFilter`. A server that registers
  both is listed with both. Registered DMOs come from
  `HKCR\DirectShow\MediaObjects`; role from the category, or from the input
  and output types when the category is missing.
- **`host.c`:**
  - Type negotiation: `GetInputType` enumerated against the stream's type
    (the same VideoInfo, VideoInfo2 and MPEG variants the DirectShow path
    tries), `SetInputType(..., DMO_SET_TYPEF_TEST_ONLY)` before the real
    call, then `GetOutputType` enumerated against the preference list (or
    `--output-format`) and `SetOutputType`.
  - Streaming: `GetInputSizeInfo`/`GetOutputSizeInfo` for buffer sizes and
    alignment, `AllocateStreamingResources`, then for each input chunk
    `ProcessInput` (sync point, time stamp and length flags) followed by
    `ProcessOutput` until it no longer sets
    `DMO_OUTPUT_DATA_BUFFERF_INCOMPLETE`; `DMO_E_NOTACCEPTING` means drain
    first. At the end `Discontinuity` and a final drain; for `--start`,
    `Flush`.
  - Output type changes signalled by `DMO_OUTPUT_DATA_BUFFERF_*` or
    `GetOutputCurrentType` changing are handed to the same format-change
    code as DirectShow's dynamic types.
  - Encoders: `ICodecAPI` through the existing `control.c`;
    `IWMCodecPrivateData` (`SetPartialOutputType`, `GetPrivateData`) for the
    extradata written to the AVI `strf`; `IPropertyBag` settings by name
    for codecs that use them; `IPersistStream` state as for filters.
  - Every call goes through `call.c`'s traced, crash-framed wrappers, so a
    crash report says `in IMediaObject::ProcessOutput of foo.dll`.
- **`dmo.h`:** `dmo_open`, `dmo_probe`, `dmo_decode_begin`/`dmo_push`/
  `dmo_end` with an output callback, mirroring the DirectShow interface so the
  commands treat the two backends alike.

### 4. Audio

- **Media types** (moved into a shared `media/` layer used by both
  backends): `WAVEFORMATEX` and `WAVEFORMATEXTENSIBLE` ↔ `AM_MEDIA_TYPE`
  (`MEDIATYPE_Audio`, subtype from the format tag or the extensible
  `SubFormat`, `FORMAT_WaveFormatEx`, extradata after the header). An audio
  preference list: the input's own channel count and rate in 16-bit PCM,
  then 24- and 32-bit PCM and 32-bit float, extensible forms for more than
  two channels or 16 bits; `--output-format s16|s24|s32|f32`.
- **Input:** the AVI reader gains `avi_audio_stream()` and per-chunk audio
  timing (constant-rate streams from `nBlockAlign`/`nAvgBytesPerSec` and the
  byte position; variable-rate ones, `dwSampleSize` 0, one frame per chunk at
  `dwScale`/`dwRate`). A WAV reader (`RIFF WAVE`, PCM, float, extensible and
  compressed format tags, `data` chunk) and raw PCM with an explicit format
  are new `source` kinds, so `encode` can read them.
- **Output:** a WAV writer (PCM and float, extensible where needed, sizes
  patched on close) and raw PCM. The AVI writer gains an audio stream (an
  `auds` `strh`, a `WAVEFORMATEX` `strf`, sample-based `dwScale`/`dwRate`
  and `dwSampleSize`) so `encode` can write compressed audio; a WAV file with
  the codec's format tag is the alternative for codecs that allow it.
- **DirectShow host:** today the source pin, allocator sizing, preference
  list and format adoption assume video. They move behind a small per-major
  type table (sample buffer size from `biSizeImage` or from the audio frame
  size, preference list, "same format" test). Audio samples carry times
  computed from sample counts; a decoder may return differently sized
  samples, which the sink writes as they come; the tail a decoder flushes on
  `EndOfStream` is kept. `--start` trims to the sample.
- **Roles:** `enum dshow_role` and the DMO equivalent become a major type
  (video or audio) plus a direction (decoder or encoder), judged from pins or
  types; `list` and `probe` print both.
- **CLI:** `decode` and `encode` pick the stream with `--stream N`, or `--audio`
  for the first audio stream; an input without video is audio by default.
  `--start` takes a time as well as a frame number (`--start 2.5s`), and
  `--duration` limits audio output where `--frames` limits video. `--bitrate`
  and `--quality` reach audio encoders through `ICodecAPI`
  (`CODECAPI_AVEncCommonMeanBitRate`, `_Quality`) and `IPropertyBag`.

### 5. Test fixtures

- **`testdmo.dll`** (`tests/fixtures/testdmo.c`, CRT-less C like
  `testfilter.c`): a TST0 video decoder DMO and encoder DMO reusing the
  lossless bitstream, so `testcodec.dll`, `testfilter.ax` and `testdmo.dll`
  can be cross-checked byte for byte. Registered through `DMORegister`
  (decoder) and by writing the keys itself (encoder, as some DMOs do).
  Modes, as in `testfilter`: keeps the input buffer until drained;
  `DMO_E_NOTACCEPTING` every other call; `INCOMPLETE` output split across
  several `ProcessOutput` calls; an output type change on the first frame;
  strict mode rejecting pointers at or above 2 GB; `IWMCodecPrivateData` on
  the encoder.
- **A lossless audio codec, `TSTA`** (a private `WAVE_FORMAT_` tag and its
  subtype GUID): a reversible delta coding of PCM in fixed frames with a
  small header, so output is checked byte for byte. It is added to
  `testfilter.ax` as a decoder and encoder filter pair and to `testdmo.dll`
  as a DMO pair, so each audio path has its own fixture class. Modes: output
  samples of a different size from the input; a tail delivered only at
  end of stream; a sample rate change mid-stream (dynamic type).
- FFmpeg (when installed) makes PCM WAVs and AVIs with PCM audio for the
  media tests; nothing in the unit tests needs it.

## Phases

0. **Headers and ABI.** `dmo.h`, `mmreg.h`, the audio GUIDs, ABI entries.
   Milestone: `check_abi` passes.
1. **msdmo and discovery.** The built-in msdmo.dll with capture mode, DMO
   discovery, `register`/`unregister`/`list`/`info`/`probe` for DMOs.
   Milestone: `test_msdmo` registers and unregisters a fake DMO and reads the
   keys back; `testdmo.dll`'s two DMOs are discovered without touching the
   registry, then registered and listed.
2. **DMO video decode and encode.** `buffer.c`, `host.c`, CLI integration.
   Milestone: `tests/unit/test_dmo.c` runs the decoder in every fixture mode
   with no leaked references (`DllCanUnloadNow == S_OK`); an AVI made by
   `testcodec.dll` decodes identically through all three backends, and
   `encode testdmo.dll` → `decode testcodec.dll` is byte-exact.
3. **Audio media and I/O.** Shared media-type layer, WAV reader and writer,
   AVI audio streams in the reader and writer, `convert` and `avi` showing
   audio streams. Milestone: `test_wav`, `test_avi` audio cases (including
   truncated and corrupt files, as for video); a WAV round-trips through
   `convert`, and `seance avi` lists an FFmpeg-made AVI's audio stream.
4. **Audio through DirectShow.** The host's per-major-type table, audio
   roles, `decode`/`encode` with `--audio`/`--stream`/`--duration`.
   Milestone: `TSTA` encode → decode through `testfilter.ax` round-trips a
   WAV exactly, in every fixture mode.
5. **Audio through DMOs.** The same over `testdmo.dll`.
   Milestone: as Phase 4, and the DirectShow and DMO `TSTA` outputs match.
6. **Real codecs and docs.** `tests/samples.sh` reports DMOs and audio
   classes; LAV Audio decoding FFmpeg-made streams (MP3, AAC, AC-3, FLAC,
   Vorbis, Opus) compared with `ffmpeg -f s16le`/`f32le` output; any DMOs the
   user supplies (Windows' own are not redistributable, so never in the
   repository), which is also where VC-1/WMV3 decoding would first be
   exercised. Results recorded in `MISSING_FEATURES.md`; README sections and
   status rows.

## Possible follow-on (not in this plan)

- **ACM drivers** (`msacm32.dll`, `.acm` files): the audio counterpart of
  the VfW backend, driven through `DriverProc` with `ACMDM_*` messages.
  Phases 3 and 4 make it cheap to add later, as its own backend.
- The DMO wrapper filter, so a DMO could be put in a DirectShow graph; not
  needed while the harness drives DMOs directly.

## Critical files

- New: `include/win32/dmo.h`, `include/win32/mmreg.h`, `src/dmo/*.c|h`,
  `src/win32/msdmo.c`, `src/media/*.c|h`, `src/in/wav.c`, `src/out/wav.c`,
  `tests/fixtures/testdmo.{c,def}`, `tests/unit/test_dmo.c`,
  `tests/unit/test_msdmo.c`, `tests/unit/test_wav.c`.
- Modified: `src/runtime/builtin.c`, `src/dshow/{host,mediatype,discover,
  alloc}.c`, `src/avi/avi_reader.c`, `src/avi/avi_writer.c`, `src/avi/avi.h`,
  `src/in/source.{c,h}`, `src/out/output.{c,h}`, `src/cmd_*.c`,
  `tests/fixtures/testfilter_filter.c` (the `TSTA` classes),
  `tests/fixtures/CMakeLists.txt`, `tests/abi/abi_list.h`,
  `tests/abi/mingw_guids.sh`, `tests/samples.sh`, `tests/smoke.sh`,
  `README.md`, `MISSING_FEATURES.md`.

## Verification

- `ctest` under the `default`, `ci` and `asan` presets: `check_abi`,
  `test_msdmo`, `test_dmo`, `test_wav`, the extended `test_dshow`,
  `test_avi` and smoke/media tests.
- `VFW_TRACE=1 build/seance probe build/fixtures/testdmo.dll` shows the type
  negotiation and `ProcessInput`/`ProcessOutput` calls with no trap hits.
- Round trips:
  `seance encode testcodec.dll in.y4m -o a.avi && seance decode testdmo.dll a.avi -o out.y4m && cmp in.y4m out.y4m`;
  `seance encode testfilter.ax in.wav -o a.avi && seance decode testdmo.dll a.avi -o out.wav && cmp in.wav out.wav`.
- `seance register testdmo.dll && seance list && seance decode auto a.avi`
  finding the registered DMO.
- Optional: `tests/samples.sh` over LAV Filters and any user-supplied DMOs.
