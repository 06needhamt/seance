<!-- SPDX-License-Identifier: GPL-2.0-only -->
# libseance: DirectShow filter support plan

## Context

libseance loads 32-bit VfW/ICM codec DLLs natively on Linux and drives them
through `DriverProc` (`probe`, `list`, `decode`, `encode`). Many codecs from
the same era — and most later ones — ship as DirectShow filters (`.ax` DLLs):
in-process COM servers exposing `DllGetClassObject`/`DllRegisterServer`,
whose objects implement `IBaseFilter` with input and output `IPin`s and take
data through `IMemInputPin::Receive`. The goal is to load and exercise these
the same way VfW codecs are exercised today, keeping the project generic (no
dependency on any particular filter) and tested against in-repo fixtures.

Decisions made with the user:
- **Scope:** video decoders and encoders (transform filters). Audio filters,
  DMOs (`msdmo`, `IMediaObject`), source/splitter/renderer filters and
  capture devices are out of scope.
- **Host model:** the harness builds its own minimal graph — its own source
  and sink filters connected straight to the codec filter. No Filter Graph
  Manager / intelligent connect; a built-in `quartz.dll` supplies only what
  filters themselves import or create (filter mapper for self-registration,
  `AMGetErrorText`, …).
- **CLI:** the existing `probe`, `decode`, `encode` and `list` learn to
  handle DirectShow filters (detected automatically, `--clsid` to choose one);
  only registration, which has no VfW equivalent, gets a new command.

What already exists and is reused:
- PE loader, built-in DLL registry (`src/runtime/builtin.c`), TEB/SEH, threads,
  events, critical sections and `WaitForMultipleObjects` (`k32_sync.c`,
  `k32_thread.c`) — strmbase's `CAMEvent`/`CCritSec`/`CAMThread` map onto these.
- ole32's in-process server loading: `ole_CoCreateInstance` /
  `inproc_class_object` / `load_server` (`src/win32/ole32.c`) already resolve
  `HKCR\CLSID\{…}\InprocServer32` and call `DllGetClassObject`.
- The persistent registry (`src/win32/reg.c`, `advapi32.c`), where
  `DllRegisterServer` writes.
- msvcrt's C++ EH/RTTI and msvcp classes (`crt_cxx.c`) — strmbase is C++.
- Crash/hang framing (`crash_enter`/`crash_leave`, `src/runtime/crash.h`) and
  the `TRACE` logging used by `drv_call` (`src/vfw/driver.c`).
- AVI reader/writer (`src/avi/`), frame sources (`src/in/`), writers and
  colour conversion (`src/out/`), standard format list `vfw_std_format`
  (`src/vfw/icm.c`), CLI helpers (`src/cmd_util.c`).

## Architecture

```
cli: probe | decode | encode | list | register   (backend chosen per target)
 ├─ vfw/      existing ICM backend
 └─ dshow/    new DirectShow backend
      ├─ mediatype.c   BITMAPINFOHEADER <-> AM_MEDIA_TYPE, subtype GUIDs
      ├─ discover.c    find a DLL's filters (DllRegisterServer capture), registry lookup
      ├─ alloc.c       IMemAllocator + IMediaSample (harness side)
      ├─ host.c        source filter/pin, sink filter/pin, stub graph, state machine
      ├─ call.c        traced, crash-framed calls into filter COM methods
      └─ dshow.h       typed C interface used by the commands
win32/quartz.c         built-in quartz.dll (filter mapper, AMGetErrorText)
include/win32/dshow.h  strmif/amvideo/uuids definitions
```

### 1. Headers and ABI (`include/win32/dshow.h`)

- C-style interface declarations (same pattern as `include/win32/objbase.h`):
  `IPersist`, `IMediaFilter`, `IBaseFilter`, `IPin`, `IEnumPins`,
  `IEnumMediaTypes`, `IMemInputPin`, `IMemAllocator`, `IMediaSample(2)`,
  `IFilterGraph`, `IMediaEventSink`, `IReferenceClock`, `IFilterMapper2`,
  `IFilterMapper`, `IEnumMoniker`/`ICreateDevEnum` (empty enumeration only),
  `IAMVideoCompression`, `IPersistStream`, `IStream`, `ISpecifyPropertyPages`,
  `IQualityControl`.
- Structures: `AM_MEDIA_TYPE`, `VIDEOINFOHEADER`, `VIDEOINFOHEADER2`,
  `MPEG1VIDEOINFO`/`MPEG2VIDEOINFO` (read-only, for extradata), `PIN_INFO`,
  `FILTER_INFO`, `ALLOCATOR_PROPERTIES`, `REGFILTER2`, `REGFILTERPINS(2)`,
  `REGPINTYPES`, `REGPINMEDIUM`; `REFERENCE_TIME`; `FILTER_STATE`,
  `PIN_DIRECTION`, `AM_SAMPLE2_PROPERTIES`.
- GUID constants (MEDIATYPE_Video, FORMAT_VideoInfo/VideoInfo2/MPEGVideo/
  MPEG2Video, MEDIASUBTYPE_RGB*/YUY2/UYVY/YV12/I420/IYUV/NV12, the FOURCC
  subtype template `{XXXXXXXX-0000-0010-8000-00AA00389B71}`, CLSID_FilterMapper2,
  CLSID_FilterMapper, CLSID_SystemDeviceEnum, CLSID_LegacyAmFilterCategory,
  CLSID_VideoCompressorCategory), and HRESULTs (`VFW_E_*`, `VFW_S_*`).
- Add every new struct size/offset and constant to `tests/abi/abi_list.h` so
  `check_abi` compares them with mingw-w64's `strmif.h`/`amvideo.h`/
  `dvdmedia.h`/`uuids.h`.

### 2. Win32 runtime additions

- **quartz.dll built-in** (`src/win32/quartz.c`, added to `builtin_dlls[]` in
  `src/runtime/builtin.c`):
  - `DllGetClassObject` serving `CLSID_FilterMapper2` and `CLSID_FilterMapper`
    (also registered with `CoRegisterClassObject` at startup, or answered from
    a built-in class table in `ole_CoGetClassObject`, so filters'
    `CoCreateInstance(CLSID_FilterMapper2)` finds it without a registry entry).
  - `IFilterMapper2::RegisterFilter`/`UnregisterFilter`/`CreateCategory` write
    and remove what Windows writes:
    `HKCR\CLSID\{category}\Instance\{clsid}` with `CLSID`, `FriendlyName` and
    the `FilterData` REG_BINARY blob (format after Wine's `filtermapper.c`);
    legacy `IFilterMapper::RegisterFilter/RegisterPin/RegisterPinType` write
    `HKCR\Filter\{clsid}` and `HKCR\CLSID\{clsid}\Pins\…`.
  - A **capture mode** (internal hook used by `dshow/discover.c`) that records
    each registration — name, merit, pins and their media types — in memory
    instead of writing the registry.
  - `CLSID_SystemDeviceEnum` → `ICreateDevEnum::CreateClassEnumerator` returns
    `S_FALSE` (empty category), for filters that probe for devices.
  - `AMGetErrorTextA/W` (messages for VFW_E_* codes).
- **ole32 gaps** filters commonly import: `CreateStreamOnHGlobal` (+ an
  `IStream` over an HGLOBAL; also used for `IPersistStream` state),
  `CoCreateFreeThreadedMarshaler` (an aggregatable object that only answers
  `IUnknown`/`IMarshal` → E_NOTIMPL), `CoGetInterfaceAndReleaseStream`/
  `CoMarshalInterThreadInterfaceInStream` passing the pointer straight
  through (single process, no apartments enforced).
- **winmm gaps** (strmbase's `CAMSchedule`/system clock):
  `timeSetEvent`/`timeKillEvent`, `timeBeginPeriod`/`timeEndPeriod`, if not
  already present.
- The exact list is driven by `seance info` over the fixture and real `.ax`
  files (see Verification); anything else reached is a trap thunk naming it.

### 3. DirectShow backend (`src/dshow/`)

- **`call.c`:** every call from the harness into a filter's vtable goes
  through small typed wrappers that `crash_enter("IPin::Receive", fn, …)`/
  `crash_leave` and `TRACE` the method and its HRESULT under `VFW_TRACE=1`
  (named via a `hr_name()` like `icerr_name`). Crash reports then say
  `in IPin::ReceiveConnection of foo.ax`.
- **`mediatype.c`:**
  - `bih_to_mt()`: AVI `strf` / harness format → `AM_MEDIA_TYPE` (Video,
    subtype from FOURCC or RGB depth/bitfields, FORMAT_VideoInfo with
    `AvgTimePerFrame` from rate/scale, extradata after the header;
    VideoInfo2 variant tried when VideoInfo is refused).
  - `mt_to_bih()`: back again for VideoInfo, VideoInfo2 and MPEG*VideoInfo,
    including `rcSource`/`rcTarget` and stride (`biWidth` wider than the
    picture) so the writers get the right geometry.
  - Free/copy helpers (`FreeMediaType` semantics, CoTaskMem allocation, as
    filters expect to free what we hand them and vice versa).
  - Standard output preferences mirror `vfw_std_format` (I420, YV12, YUY2,
    UYVY, RGB32, RGB24, RGB565, RGB555).
- **`alloc.c`:** a harness `IMemAllocator`/`IMediaSample2` (buffers in
  low memory, `SetProperties`/`Commit`/`Decommit`/`GetBuffer` blocking until a
  sample is free, `ReleaseBuffer` via sample refcount), used when the filter
  doesn't provide one; otherwise the filter's allocator is used and our side
  fills/reads its samples. Sample properties: time stamps, sync point
  (keyframe), preroll, discontinuity, attached media type (dynamic format
  change).
- **`discover.c`:**
  - Recognise a DirectShow DLL: exports `DllGetClassObject` and not
    `DriverProc` (`.ax` extension is a hint only).
  - `dshow_discover(path)`: load the DLL, call `DllRegisterServer` with
    quartz in capture mode and the registry writes it makes redirected to a
    throw-away in-memory overlay (a small hook in `reg.c`), giving the list
    of filters (CLSID, name, merit, pins, media types) without touching the
    user's registry. Fallback when that fails: `--clsid` is required.
  - Registered filters: enumerate `HKCR\CLSID\{LegacyAmFilterCategory}\Instance`
    and `HKCR\Filter`, parse `FilterData` blobs (also from `.reg` files
    exported on Windows) — used by `list` and `decode auto`.
  - Creating a filter: from a path, `DllGetClassObject` →
    `IClassFactory::CreateInstance` directly (no registration needed); from a
    CLSID, `CoCreateInstance` via the existing ole32 path.
- **`host.c`:** the minimal graph, all harness-side C COM objects with
  correct refcounting:
  - **Stub graph** (`IFilterGraph` + `IMediaEventSink`) passed to
    `JoinFilterGraph`; `Notify` records events (EC_ERRORABORT,
    EC_COMPLETE, …) so failures surface as messages; other methods E_NOTIMPL.
  - **Source filter/output pin:** offers the input media type, drives
    connection by calling the codec input pin's `ReceiveConnection`, does
    allocator negotiation (`IMemInputPin::GetAllocator`/`NotifyAllocator`,
    `GetAllocatorRequirements`), then pushes samples with `Receive` (or
    `ReceiveMultiple`), honouring `ReceiveCanBlock`; `EndOfStream`,
    `BeginFlush`/`EndFlush`/`NewSegment` for seeks.
  - **Sink filter/input pin:** connected from the codec's output pin
    (`IPin::Connect(our pin, mt)`), accepting a type chosen from the codec
    output pin's `EnumMediaTypes` by the preference list (or
    `--output-format`); `QueryAccept` for dynamic changes; `Receive` hands
    each sample (data, times, sync point, attached media type) to a callback;
    `EndOfStream` signals an event. Thread-safe, since filters may deliver
    from their own worker threads.
  - **State machine:** Stop → Pause → Run on all three filters with
    `SetSyncSource(NULL)` (run as fast as possible, no clock), `GetState`
    waiting through `VFW_S_STATE_INTERMEDIATE`; Stop and disconnect on
    teardown; final `Release` of everything and `DllCanUnloadNow` checked in
    tests to catch leaks.
  - **Timeouts:** waiting for output/EOS uses `VFW_TIMEOUT` (or a default)
    so a filter that never delivers is reported, not hung.
- **`dshow.h`:** the interface the commands use, e.g.
  `dshow_open(target, clsid, &filter)`, `dshow_probe(...)`,
  `dshow_decode_begin(in_bih, out_pref, …)`/`dshow_push(frame, flags, ts)`/
  `dshow_end()` with an output callback, and the encode equivalents.

### 4. CLI integration

- **`cmd_util.c`:** extend `struct cli_codec` with a backend tag and a
  DirectShow handle; `cli_open_codec` tries DirectShow when the DLL looks like
  one, the target is a `{CLSID}`, or `--clsid` is given. When a DLL holds
  several filters, pick by direction (decoder: accepts compressed input /
  outputs uncompressed; encoder: the reverse, or registered under
  `CLSID_VideoCompressorCategory`) and the input type, else require `--clsid`.
- **`probe`** (`cmd_probe.c`): for a filter, print name, CLSID, merit, pins
  with registered media types, exposed interfaces (`IAMVideoCompression`,
  `IPersistStream`, `ISpecifyPropertyPages`, `ICodecAPI`), then the same
  decompress/compress checks as today: which standard formats the input pin
  connects with, and the output types offered for each.
- **`decode`** (`cmd_decode.c`): factor the per-frame output side
  (`--start`/`--frames`, preroll from the previous keyframe, `--keep-going`,
  `output_write`/`output_repeat`, format conversion) so frames can come from
  either `ic_decompress` or the DirectShow sink callback. AVI frames become
  samples with `SetSyncPoint` from the index and `SetPreroll` for frames
  before `--start`; empty chunks repeat the previous frame. `auto` tries
  `ICLocate` first, then registered filters whose input types match, by merit.
- **`encode`** (`cmd_encode.c`): frames from `src/in/` pushed into the
  encoder; compressed samples (sync point → AVI keyframe flag) written by the
  AVI writer with the output media type converted to `strf`. `--keyint`,
  `--quality`, `--bitrate` through `IAMVideoCompression` (and `ICodecAPI`
  when present); `--state`/`--save-state` through `IPersistStream` on an
  HGLOBAL stream.
- **`list`** (`cmd_list.c`): after the vfw.ini entries, list registered
  DirectShow video decoders/encoders (name, CLSID, path, merit).
- **New `register`/`unregister`** command (`src/cmd_register.c`, added to
  `main.c` and `commands.h`): calls `DllRegisterServer`/`DllUnregisterServer`
  so the filter persists in `$VFW_REGISTRY` and can be named by CLSID.
- **`info`:** note "DirectShow filter" and list the filters `dshow_discover`
  finds (it already lists imports, so missing quartz/ole32 functions show).
- Configure/about dialogs: `ISpecifyPropertyPages` is reported and skipped
  (headless, same as `ICConfigure`/`ICAbout`).

### 5. Test fixture (`tests/fixtures/testfilter.c`, `testfilter.def`)

A mingw-built, CRT-less C DLL (`testfilter.ax`, hand-written vtables, no
strmbase — same constraints as `testcodec.c`/`pe_com.c`), generic and small:
- Two classes: a **TST0 decoder** and a **TST0 encoder**, reusing
  `testcodec.c`'s lossless bitstream so VfW and DirectShow outputs can be
  cross-checked byte for byte.
- `DllRegisterServer`/`DllUnregisterServer` through `IFilterMapper2`
  (one class) and legacy `IFilterMapper` (the other), `DllCanUnloadNow`.
- Modes selected by a registry value or env var to cover host paths: own
  allocator vs. ours; synchronous delivery vs. a worker thread delivering
  from `CreateThread`; a dynamic output format change on the first sample;
  `IMediaEventSink::Notify` on a bad frame; strict mode rejecting pointers
  at or above 2 GB (like testcodec).
- Implements `IAMVideoCompression` and `IPersistStream` on the encoder.
- Built in `tests/fixtures/CMakeLists.txt` with `fixture(testfilter … LIBS
  ole32 kernel32)` and a `SUFFIX ".ax"` override.

## Phases

0. **Headers and ABI.** (Done.) `include/win32/dshow.h`, ABI list entries.
   Milestone: `check_abi` passes.
   - `IPersist`, `ISequentialStream`, `IStream`/`STATSTG`, `IPersistStream`
     and `IEnumMoniker` (with `IMoniker` opaque) went into
     `include/win32/objbase.h`, where objidl.h's interfaces belong; the rest
     is in `dshow.h`. `ICodecAPI` is only named by its IID for now.
   - GUIDs are kept as lists (`VFW_OBJBASE_GUIDS`, `VFW_DSHOW_GUIDS`, entries
     `G(name, Data1, Data2, Data3, Data4...)`), declared `extern` through
     `VFW_GUID_DECLARE` and defined once in `src/win32/guids.c`.
     `FOURCC_SUBTYPE_INIT(fcc)` builds the subtype for any FOURCC.
   - `check_abi` now compares GUIDs too. mingw's `DEFINE_GUID` can't be
     redefined to capture values (guiddef.h redefines it on every include),
     so `tests/abi/mingw_guids.sh` preprocesses the mingw headers with
     `INITGUID` and reads the initialisers back as `ABI_GUID(...)` lines;
     `check_abi.c` turns each into typedefs whose sizes are the GUID's 16-bit
     pieces, and `gen_abi_checks` asserts ours against them.
   - `constants.sh` now checks `VFW_E_`/`VFW_S_` names, which it used to skip
     as harness-only along with the rest of the `VFW_` prefix.
1. **Runtime surface.** (Done.) quartz built-in (filter mapper write + capture mode,
   device enum, `AMGetErrorText`), ole32/winmm additions, registry overlay
   hook. Milestone: unit tests in `tests/unit/test_quartz.c` register and
   unregister a fake filter and read back `FilterData`; `test_ole32` covers
   the new stream functions.
   - `src/win32/quartz.c` serves `CLSID_FilterMapper2`, `CLSID_FilterMapper`
     (one object with both interfaces, as in Wine) and
     `CLSID_SystemDeviceEnum`. ole32 now has a table of Windows' own classes
     that built-in DLLs serve, so `CoCreateInstance` finds these with no
     registry entry; a registry entry naming quartz.dll by any path also
     reaches the built-in.
   - `IFilterMapper2::RegisterFilter` writes `FriendlyName`, `CLSID` and a
     version 2 `FilterData` under `HKCR\CLSID\{category}\Instance\{instance}`,
     converting version 1 pins; `CreateCategory` and `UnregisterFilter` too.
     The legacy mapper writes `HKCR\Filter\{clsid}`, `Merit` and
     `Pins\name\Types\{major}\{sub}`. `EnumMatchingFilters` (both) is
     E_NOTIMPL: with no graph manager nothing needs it. `RegisterFilter` gives
     no moniker back (`*ppMoniker` is NULL).
   - `src/win32/quartz.h` is the host's side: `struct qz_filter`/`qz_pin`,
     `qz_filterdata_write`/`qz_filterdata_parse` (bounds-checked, fuzzed by
     truncation in the test), and `quartz_capture_begin`/`_end`, which record
     registrations from either mapper (legacy pins keep their names).
   - The registry overlay is `reg_sandbox_begin`/`reg_sandbox_end` in `reg.c`:
     a deep copy stands in for the tree and is never saved, then is thrown
     away. Handles are paths, so ones opened before keep working.
   - ole32: `CreateStreamOnHGlobal`/`GetHGlobalFromStream` (`ole_stream.c`,
     after Wine; clones share the block), in-process marshalling
     (`CoMarshalInterface`, `CoUnmarshalInterface`, `CoReleaseMarshalData`,
     `CoMarshalInterThreadInterfaceInStream`, `CoGetInterfaceAndReleaseStream`:
     the pointer itself, with a reference) and a free-threaded marshaler that
     answers only IUnknown. `ole.h` shares the GUID string helpers.
   - winmm: `timeSetEvent`/`timeKillEvent` on one timer thread made with
     `CreateThread`, so callbacks run on a Win32 thread; event timers;
     `TIME_KILL_SYNCHRONOUS` waits for a running callback unless the timer
     thread itself kills. kernel32 gained `thread_create` and `event_signal`
     for built-in DLLs.
   - `AMGetErrorTextA/W` know every `VFW_E_`/`VFW_S_` code by name, with a
     description for the common ones (`quartz_errors.h`).
   - Tests: `test_quartz`, `test_winmm` (new), and stream, marshalling and
     sandbox cases in `test_ole32` and `test_advapi32`.
2. **Discovery and registration.** `dshow/discover.c`, `register`/
   `unregister`, `info` and `list` output. Milestone: `testfilter.ax`'s two
   filters are discovered without touching the registry, and registered/listed.
3. **Host and decode.** `mediatype.c`, `alloc.c`, `host.c`, `call.c`;
   `probe` and `decode` integration. Milestone: `tests/unit/test_dshow.c`
   connects, runs and tears down the decoder in every fixture mode with no
   leaked references (`DllCanUnloadNow == S_OK`); decoding a testcodec-made
   AVI via `testfilter.ax` matches decoding it via `testcodec.dll`.
4. **Encode.** Encoder path, `IAMVideoCompression`, `IPersistStream` state.
   Milestone: Y4M → `encode testfilter.ax` → `decode testcodec.dll` is
   byte-exact, and vice versa.
5. **Hardening and docs.** Seeks (`--start`) via flush/new segment, worker
   thread delivery, timeouts, crash reports naming COM methods, fuzzed media
   types in `test_dshow`, `tests/samples.sh` also running over `*.ax`,
   `smoke.sh`/`validate_media.sh` cases, README section and a status row
   in `docs/IMPLEMENTATION_PLAN.md`. CI needs no new packages.

## Critical files

- New: `include/win32/dshow.h`, `src/dshow/*.c|h`, `src/win32/quartz.c`,
  `src/cmd_register.c`, `tests/fixtures/testfilter.{c,def}`,
  `tests/unit/test_dshow.c`, `tests/unit/test_quartz.c`.
- Modified: `CMakeLists.txt` (sources, tests), `src/runtime/builtin.c`,
  `src/win32/ole32.c`, `src/win32/winmm.c`, `src/win32/reg.c`,
  `src/cmd_util.{c,h}`, `src/cmd_probe.c`, `src/cmd_decode.c`,
  `src/cmd_encode.c`, `src/cmd_list.c`, `src/cmd_info.c`, `src/main.c`,
  `src/commands.h`, `tests/fixtures/CMakeLists.txt`, `tests/abi/abi_list.h`,
  `tests/samples.sh`, `tests/smoke.sh`, `README.md`,
  `docs/IMPLEMENTATION_PLAN.md`.

## Verification

- `cmake --preset default && cmake --build --preset default && ctest --preset default`
  (and the `asan` preset): `check_abi`, `test_quartz`, `test_dshow`, the
  extended smoke/media tests all pass.
- `VFW_TRACE=1 build/seance probe build/fixtures/testfilter.ax` shows the
  connection sequence and COM calls with no trap hits.
- Round trips:
  `seance encode testcodec.dll in.y4m -o a.avi && seance decode build/fixtures/testfilter.ax a.avi -o out.y4m && cmp in.y4m out.y4m`,
  and the reverse with `encode testfilter.ax` / `decode testcodec.dll`.
- `seance register testfilter.ax && seance list && seance decode auto a.avi -o x.y4m`
  using the registered filter (with `VFW_REGISTRY` pointing at a temp file).
- Optional: `tests/samples.sh build/seance ~/filters` over real `.ax` files to
  see unimplemented imports and whether each filter is discovered and connects.
