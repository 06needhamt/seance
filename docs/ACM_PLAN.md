<!-- SPDX-License-Identifier: GPL-2.0-only -->
# seance: ACM driver support plan

## Context

seance drives video codecs as VfW/ICM drivers, and audio and video codecs as
DirectShow filters and DMOs (`docs/DIRECT_SHOW_PLAN.md`,
`docs/DMO_AUDIO_PLAN.md`). The Audio Compression Manager is what this plan
adds: the audio counterpart of VfW, and the way most audio codecs from
Windows 3.1 to XP were installed.

- **ACM drivers** are DLLs (usually `.acm`) exporting `DriverProc`, like
  VfW codecs. They are opened through `msacm32.dll`, which sends them
  `ACMDM_*` messages: driver details, the format tags and formats each
  supports, format suggestions, and stream open/size/convert/close.
  Windows' IMA and MS ADPCM, G.711, GSM 6.10 and Fraunhofer MP3 codecs are
  ACM drivers, as are LAME ACM, AC3ACM and the Vorbis ACM driver.
- **`msacm32.dll`** is the API applications use (`acmStreamOpen`,
  `acmStreamConvert`, `acmFormatSuggest`, `acmFormatEnum`, ...). It keeps
  the list of installed drivers, from `[drivers32]` lines and the registry
  (`msacm.imaadpcm=imaadp32.acm`), and has a PCM converter of its own.
- A conversion is a **stream**. The caller gives a source and a
  destination `WAVEFORMATEX`, and msacm32 finds a driver that can do it
  directly. It then passes buffers through `ACMSTREAMHEADER`s. Input is
  converted in whole blocks of the source format
  (`ACM_STREAMCONVERTF_BLOCKALIGN`), and what is left over is the caller's
  to resubmit. `ACM_STREAMCONVERTF_END` flushes. There are no time stamps.

Proposed decisions (to confirm before Phase 1):
- **Backend:** ACM becomes a third kind of transform behind `xform.h`,
  beside DirectShow filters and DMOs. `decode` and `encode` for audio then
  drive ACM drivers through the same code, with the same `--start`,
  `--duration`, `--sample-format`, PCM conversion and WAV/AVI writers.
- **msacm32.dll as a built-in DLL** with the real exports, used by the
  harness and by any DLL that imports it, as `msvfw32.dll` is for VfW.
  The `acm_*` functions serve both, as `ic_*` do in `src/vfw/icm.c`.
- **Registration:** drivers come from `vfw.ini`'s `[drivers32]`
  (`msacm.<name>=<path>`), its view under the registry's `Drivers32` key,
  `acmDriverAdd`, and a path given on the command line. There is no
  `register` for ACM, as there is none for VfW codecs.
- **The built-in PCM converter** converts between PCM sample formats
  (8/16/24/32-bit integer) and between mono and stereo, at
  the same rate. It refuses rate conversion (`ACMERR_NOTPOSSIBLE`).
  `src/media/audio.c` already converts sample formats the way FFmpeg does.
- **Headless choosers:** `acmFormatChoose` and `acmFilterChoose` answer from
  an environment variable (`VFW_ACM_FORMAT`), else cancel, as
  `ICCompressorChoose` does with `VFW_COMPRESSOR`.
- **Scope:** codecs (format conversion). Filters (`ACMDM_FILTER_*`: echo,
  volume) are enumerated and reported, but not driven from the CLI.
  Asynchronous streams are passed to drivers that claim support
  (`ACMDRIVERDETAILS_SUPPORTF_ASYNC`) and refused otherwise. The wave
  mapper (`MIDI`/`wave` devices) and the DirectShow ACM Wrapper filter stay
  out.
- **Tests:** an in-repo fixture driver (`testacm.acm`, TSTA as the other
  fixtures have it). Real drivers are used only through `tests/samples.sh`
  and `tests/audio_codecs.sh`, as now.

What already exists and is reused:
- Installable drivers: `drv_open`/`drv_send`/`drv_close` (`src/vfw/driver.c`)
  with DRV_LOAD/ENABLE/OPEN sequencing, `TRACE` naming of messages, crash
  and hang framing; winmm's `OpenDriver`, `SendDriverMessage`,
  `DefDriverProc`.
- The `[drivers32]` parser and its `Drivers32` registry view
  (`src/vfw/registry.c`, `src/win32/advapi32.c`), which today ignore
  `msacm.*` lines.
- The audio work of the DMO/audio plan: `WAVEFORMATEX(TENSIBLE)` and format
  tags (`mmreg.h`), media type conversion and PCM preference lists
  (`src/media/audio.c`), WAV/AVI audio sources and sinks, `cmd_audio.c`'s
  decode and encode loops, the TSTA format (`tests/fixtures/tsta.h`), and
  `tests/audio_codecs.sh`.

## Architecture

```
cli: probe | decode | encode | list | info
 ├─ vfw/       ICM backend (unchanged)
 ├─ dshow/     DirectShow backend   ┐
 ├─ dmo/       DMO backend          ├─ xform.h: one transform interface
 └─ acm/       new ACM backend      ┘
      ├─ host.c      a stream: block alignment, leftovers, END flush
      └─ acm.h       typed C interface (find a driver's role, open a stream)
win32/msacm32.c      built-in msacm32.dll: drivers, details, enumeration,
                     suggest, streams, the PCM converter
include/win32/msacm.h  msacm.h and msacmdrv.h definitions
```

### 1. Headers and ABI (`include/win32/msacm.h`)

- **Application side, as mingw-w64's `msacm.h`:**
  - Handles and callbacks: `HACMDRIVERID`, `HACMDRIVER`, `HACMSTREAM`,
    `HACMOBJ`, `ACMDRIVERENUMCB`, `ACMFORMATTAGENUMCB{A,W}`,
    `ACMFORMATENUMCB{A,W}`, `ACMFILTERTAGENUMCB{A,W}`,
    `ACMFILTERENUMCB{A,W}`.
  - Structures:
    - `ACMDRIVERDETAILS{A,W}`;
    - `ACMFORMATTAGDETAILS{A,W}` and `ACMFORMATDETAILS{A,W}`;
    - `ACMFILTERTAGDETAILS{A,W}` and `ACMFILTERDETAILS{A,W}`;
    - `ACMFORMATCHOOSE{A,W}` and `ACMFILTERCHOOSE{A,W}`;
    - `ACMSTREAMHEADER`;
    - `WAVEFILTER`, which goes in `mmreg.h`.
  - Constants: `ACM_*` flags, `ACMERR_*`, `ACM_METRIC_*`,
    `ACMDRIVERDETAILS_SUPPORTF_*`, `ACMSTREAMHEADER_STATUSF_*`, and the
    `MMSYSERR_*` codes not yet in `mmsystem.h`.
  - These all go in `tests/abi/abi_list.h`, so `check_abi` compares them
    with mingw-w64.
- **Driver side, as Wine's `msacmdrv.h`** (mingw-w64 has no counterpart):
  - Messages: `ACMDM_DRIVER_DETAILS`, `ACMDM_FORMATTAG_DETAILS`,
    `ACMDM_FORMAT_DETAILS`, `ACMDM_FORMAT_SUGGEST`, `ACMDM_FILTERTAG_DETAILS`,
    `ACMDM_FILTER_DETAILS`, and `ACMDM_STREAM_OPEN`/`_CLOSE`/`_SIZE`/
    `_CONVERT`/`_RESET`/`_PREPARE`/`_UNPREPARE`.
  - Structures: `ACMDRVOPENDESCW`, `ACMDRVSTREAMINSTANCE`,
    `ACMDRVSTREAMHEADER`, `ACMDRVSTREAMSIZE`, `ACMDRVFORMATSUGGEST`.
  - Their layouts are fixed with `_Static_assert`s on size and offsets,
    taken from Wine's `msacmdrv.h` and the Windows DDK's.

### 2. msacm32.dll (`src/win32/msacm32.c`)

- **Driver list.** It holds:
  - the built-in PCM converter, always present;
  - `msacm.*` entries from `vfw.ini` and the `Drivers32` view, as
    `HACMDRIVERID`s in registry order;
  - drivers added with `acmDriverAdd{A,W}` (`ACM_DRIVERADDF_NAME`,
    `_FUNCTION`, `_NOTIFYHWND` refused).

  Each driver is loaded on first use, and is sent DRV_LOAD/DRV_ENABLE
  through `drv_open` with an `ACMDRVOPENDESCW` (`fccType` `'audc'`). Its
  `ACMDRIVERDETAILSW` is cached. `acmDriverPriority` reorders the list and
  enables or disables drivers, for this process only.
- **Exports, after Wine's `dlls/msacm32`:**
  - `acmGetVersion` (an NT version, as Wine gives) and `acmMetrics`.
  - `acmDriverAdd{A,W}`, `acmDriverRemove`, `acmDriverEnum`,
    `acmDriverDetails{A,W}`, `acmDriverOpen`, `acmDriverClose`,
    `acmDriverID`, `acmDriverMessage` and `acmDriverPriority`.
  - `acmFormatTagDetails{A,W}`, `acmFormatTagEnum{A,W}`,
    `acmFormatDetails{A,W}`, `acmFormatEnum{A,W}` (with its `ACM_FORMATENUMF_*`
    filters: `WFORMATTAG`, `NCHANNELS`, `NSAMPLESPERSEC`, `WBITSPERSAMPLE`,
    `CONVERT`, `SUGGEST`, `INPUT`, `OUTPUT`) and `acmFormatSuggest`.
  - `acmFilterTagDetails{A,W}`, `acmFilterTagEnum{A,W}`,
    `acmFilterDetails{A,W}` and `acmFilterEnum{A,W}`.
  - `acmFormatChoose{A,W}` and `acmFilterChoose{A,W}`, headless (see above).
  - `acmStreamOpen` (with `ACM_STREAMOPENF_QUERY` and
    `_NONREALTIME`), `acmStreamClose`, `acmStreamSize`,
    `acmStreamPrepareHeader`, `acmStreamUnprepareHeader`,
    `acmStreamConvert`, `acmStreamReset` and `acmStreamMessage`.
- **Stream open.** It tries the driver given, else each enabled driver in
  priority order. It asks each with `ACMDM_STREAM_OPEN` and takes the first
  that agrees, as Windows does. The headers' prepared/done state and the
  driver's `ACMDRVSTREAMHEADER` shadow are kept as msacm32 keeps them, so a
  driver that looks at `fdwStatus` or `dwReservedDriver` sees what it would
  on Windows.
- **The PCM converter:** `MS-PCM` with format tag 1. It enumerates the
  standard PCM formats (the rates Wine's converter lists, mono and
  stereo, 8, 16 and 24 bits), suggests within them, and converts through
  `audio_convert`.
- **Tracing.** Every driver message is traced under `VFW_TRACE=1`, with
  `ACMDM_*` names added to `drv_msg_name`. Calls are crash-framed, so a
  report names the message: `in DriverProc(ACMDM_STREAM_CONVERT) of
  l3codeca.acm`.

### 3. ACM backend (`src/acm/`)

- **`acm_find(path, audio role)`.** Opens the DLL as an ACM driver
  (`acmDriverAdd` with the path, then `ACMDM_DRIVER_DETAILS`; a VfW codec
  doesn't answer with ACM's details). It then derives the roles from the
  format tags:
  - a **decoder** if some non-PCM tag converts to PCM (`acmFormatSuggest`
    from that tag's first format to `WAVE_FORMAT_PCM`);
  - an **encoder** if PCM converts to some non-PCM tag.

  An encoder's `compression` is that tag. A driver with several such tags
  needs `--format-tag` to choose.
- **xform backend** (`struct xform` gains a third kind):
  - `xform_connect` turns the media types into `WAVEFORMATEX`
    (`audio_mt_to_wfx`) and opens a stream with
    `ACM_STREAMOPENF_NONREALTIME`. A partial output type (major type and
    subtype only) is completed with `acmFormatSuggest`
    (`ACM_FORMATSUGGESTF_WFORMATTAG`), as an encoder filter completes one.
  - `xform_push` appends to an input buffer, converts whole source blocks
    with `ACM_STREAMCONVERTF_BLOCKALIGN`, keeps what's left for the next
    push, and hands out each destination buffer, untimed. The first
    conversion carries `ACM_STREAMCONVERTF_START`. Output buffers come
    from `acmStreamSize`.
  - `xform_end` converts the rest with `ACM_STREAMCONVERTF_END`.
  - `xform_seek` calls `acmStreamReset` and drops the leftovers.
- **Encoder settings.** `--bitrate` picks, among the tag's formats with
  the source's rate and channels (`acmFormatEnum`), the one whose
  `nAvgBytesPerSec` is nearest. `--quality` is refused, because ACM has no
  quality setting. `--state`/`--save-state` are refused too: ACM drivers
  keep settings in their own registry keys, which `VFW_REGISTRY` already
  covers.
- **CLI:**
  - `decode` and `encode` take a driver path when the stream is audio.
  - `decode auto` tries registered filters, then DMOs, then ACM drivers
    (`acmStreamOpen` with `ACM_STREAMOPENF_QUERY` and no driver given),
    for the input as it is and in its unwrapped form.
  - `probe` on an ACM driver prints:
    - its details (short and long names, copyright, licensing, features,
      support flags);
    - each format tag with its formats (`acmFormatEnum`);
    - which standard PCM formats each tag decodes to and encodes from.

    With a WAV or AVI argument, it shows what that stream's format
    converts to.
  - `list` adds the registered ACM drivers (alias, path, short name,
    tags).
  - `info` names a `DriverProc` DLL as a "VfW codec or ACM driver".

### 4. Fixture (`tests/fixtures/testacm.c`, built as `testacm.acm`)

A CRT-less mingw DLL like `testcodec.dll`, sharing `tsta.h`:
- **`DriverProc`** answers DRV_* and every `ACMDM_*` codec message. It
  supports two format tags, PCM (1) and TSTA (0x7354), each with its
  formats enumerated for 8000/11025/22050/44100/48000 Hz, 1/2/6 channels
  and 8/16/24 bits.
- **Conversions:**
  - TSTA → PCM: one packet (`nBlockAlign`) per 1024 frames, the last
    one short.
  - PCM → TSTA: whole packets, with the short last one after
    `ACM_STREAMCONVERTF_END`.
  - `ACMDM_STREAM_SIZE` answers both ways.
  - A stream it can't convert gets `ACMERR_NOTPOSSIBLE`; asking with
    `ACM_STREAMOPENF_QUERY` gets the same answer without opening one.
- **Modes**, from the environment, as the other fixtures have them:
  - `STRICT`: refuses pointers at or above 2 GB.
  - `NOTREALTIME`: refuses an open without `ACM_STREAMOPENF_NONREALTIME`.
  - `SMALL`: answers `ACMDM_STREAM_SIZE` with less than a block. msacm32
    passes that on, as Windows does; the backend's buffers must still
    hold a whole block.
  - `PREPARE`: wants its own `ACMDM_STREAM_PREPARE`, and checks that
    the header is prepared.
  - `CONFIG`: answers DRV_QUERYCONFIGURE.
  - `CRASH`: faults in `ACMDM_STREAM_CONVERT`, for `test_crash`.

## Phases

0. **Headers and ABI.** (Done.) `include/win32/msacm.h`, `WAVEFILTER`, the
   `MMSYSERR_*`/`ACMERR_*` codes, and the ABI list entries.
   Milestone: `check_abi` passes.
   - `msacm.h` has the application side as mingw-w64's (byte-packed, as
     the SDK's): handles, callbacks, the details, choose and stream header
     structures and every `ACM_*` constant, all in `abi_list.h` (212
     sizes, alignments and offsets; 163 constants compared by
     `constants.sh`).
   - The driver side (`ACMDM_*` messages, `ACMDRVOPENDESCW`,
     `ACMDRVSTREAMINSTANCE`, `ACMDRVSTREAMHEADER`, `ACMDRVSTREAMSIZE`,
     `ACMDRVFORMATSUGGEST`) has no mingw-w64 header to compare with: its
     sizes and offsets are `_Static_assert`ed in the header, and the
     message numbers are in `constants_skip.txt`. `ACMDRVSTREAMHEADER`
     is asserted to overlay `ACMSTREAMHEADER`, its driver fields on
     `dwReservedDriver`.
   - `mmsystem.h` gained `MMRESULT`, the full `MMSYSERR_*` list,
     `CALLBACK_*` and `MM_STREAM_*`; `mmreg.h` `WAVEFILTER` and the
     `WAVE_FILTER_*` tags.
1. **msacm32 and the PCM converter.** (Done.) The built-in DLL with every
   export above. The driver list comes from `vfw.ini` (`msacm.*` lines now
   parsed; `vfw_parse_fourcc` still refuses them), from the `Drivers32`
   view and from `acmDriverAdd`. `ACMDM_*` names go into
   `drv_msg_name`.
   Milestone: `tests/unit/test_msacm32.c` enumerates and converts with the
   PCM converter alone, and a fixture DLL importing msacm32 loads.
   - `src/win32/msacm32.c` (drivers, streams, metrics),
     `msacm_format.c` (tags, formats, filters, suggestions, choosers) and
     `msacm_pcm.c` (the converter); `msacm32.h` has the API for the
     harness and `msacm_add_path`, which adds a driver by its DLL's path
     as `msacm.<name>`.
   - `vfw.ini`'s `msacm.*` entries are a table of their own in
     `src/vfw/registry.c` (`vfw_acm_get`, `vfw_acm_register_value`,
     `vfw_acm_unregister`); the `Drivers32` view lists them and follows
     changes made there, as for codecs. msacm32 follows the table:
     entries added join the list, entries removed leave it.
   - A driver's details and tags are read the first time anything needs
     them, by opening it once. One that doesn't open, or opens but gives
     no `ACMDM_DRIVER_DETAILS` with `fccType` `'audc'` (a VfW codec),
     takes no further part: it isn't enumerated, counted or searched.
   - Differences from the decisions above: the PCM converter handles 8,
     16, 24 and 32-bit integers but not float (it is tag 1 only, as
     Windows' is), and lists the standard formats Wine's does (8000 to
     96000 Hz, mono and stereo, 8, 16 and 24 bits; 36 in all).
     `acmStreamMessage` returns `MMSYSERR_NOTSUPPORTED` (no stream
     messages are defined for applications), `acmStreamReset` only
     reaches drivers for asynchronous streams, and
     `ACM_DRIVERADDF_NOTIFYHWND` is refused.
   - Tests: `test_msacm32` (224 checks) covers the driver list, a broken
     table entry, a function driver added with `acmDriverAdd` (found by
     stream searches, disabled, removed, busy while open), priorities,
     details in both character sets, tags, formats and their filtered
     enumeration, suggestions, streams (block alignment, buffers moved
     after preparing, stereo to mono and back, bit depths), the choosers
     and `pe_acm.dll`, a fixture that runs a conversion through its
     msacm32 imports and a driver-enumeration callback.
     `test_vfw` and `test_advapi32` cover the table and its `Drivers32`
     view.
2. **Fixture driver.** (Done.) `testacm.acm` with its modes.
   Milestone:
   - `test_msacm32` adds, enumerates, suggests and converts TSTA through
     msacm32, round trip byte-exact, in every mode, each in a child
     process (`test_rerun_prefix`).
   - Afterwards the open driver count is back to zero.
   - `tests/fixtures/testacm.c`: PCM and TSTA tags with 45 standard
     formats each (8000 to 48000 Hz; 1, 2 and 6 channels; 8, 16 and 24
     bits), suggestions each way, and conversion with no state kept
     between calls. Encoding makes a packet per 1024 frames; with
     `ACM_STREAMCONVERTF_BLOCKALIGN` only whole packets, the rest left
     for the caller, and with `ACM_STREAMCONVERTF_END` (or without
     BLOCKALIGN) the short last one. Decoding takes whole packets and
     leaves a cut one. `ACMDM_STREAM_SIZE` answers both ways: a source
     size gets the most it could make, a destination size what fits in
     whole packets.
   - `tests/fixtures/acmdrv.h` has the driver-side definitions for the
     fixture, since mingw-w64 has no `msacmdrv.h`; the round trips are
     what check they agree with `include/win32/msacm.h`.
   - Modes `TESTACM_STRICT`, `_NOTREALTIME`, `_SMALL`, `_PREPARE`,
     `_CONFIG` and `_CRASH`, as described above. The strict driver also
     checks its own stack, so the mode children run below 2 GB
     (`lowmem_run`), as the commands do; the crash child must die with a
     report naming `DriverProc(ACMDM_STREAM_CONVERT) of testacm.acm` (in
     a sanitizer build, the sanitizer reports it).
   - `test_msacm32` (294 checks with the modes) also covers the driver
     named in `vfw.ini` and found again by `msacm_add_path`, its tags and
     formats enumerated and filtered, stream sizes each way, conversions
     it refuses, configuration messages, and a host loop
     (`run_stream`: pieces of a few kilobytes, whole blocks, leftovers
     given again, the end flushed) that Phase 3's backend will follow.
3. **Backend and commands.** `src/acm/`, the third `xform` kind, decode,
   encode, `auto`, probe, list and info.
   Milestone: `test_xform_audio` runs its cases against the ACM driver too.
   The TSTA packets from the filter, the DMO and the ACM driver decode with
   each other, and `seance encode testacm.acm in.wav -o a.avi && seance
   decode testfilter.ax a.avi -o out.wav` is byte-exact.
4. **Hardening, real drivers and docs.**
   - Fuzzed formats in `test_msacm32`, and crash and timeout reports naming
     `ACMDM_*` messages.
   - `samples.sh` reports ACM drivers and their tags, and
     `audio_codecs.sh` takes a driver path (it already goes through
     `decode`).
   - Real drivers:
     - user-supplied Windows ones (`imaadp32.acm`, `msadp32.acm`,
       `msg711.acm`, `msgsm32.acm`, `l3codeca.acm`/`l3codecp.acm`), which
       aren't redistributable and so stay outside the repository;
     - open-source ones where binaries can be had (LAME ACM, AC3ACM,
       Vorbis ACM).
   - Results go in `MISSING_FEATURES.md`. `smoke.sh`, `validate_media.sh`
     and README sections are updated.

## Critical files

- New:
  - `include/win32/msacm.h`
  - `src/win32/msacm32.c`
  - `src/acm/*.c|h`
  - `tests/fixtures/testacm.{c,def}`
  - `tests/unit/test_msacm32.c`
- Modified:
  - `include/win32/mmreg.h`, `include/win32/mmsystem.h`
  - `src/runtime/builtin.c`
  - `src/vfw/driver.c` (message names), `src/vfw/registry.{c,h}`,
    `src/win32/advapi32.c` (`msacm.*` entries)
  - `src/xform.{c,h}`
  - `src/cmd_{audio,decode,encode,probe,list,info}.c`
  - `tests/fixtures/CMakeLists.txt`
  - `tests/abi/abi_list.h`
  - `tests/unit/test_xform_audio.c`
  - `tests/{smoke,samples,audio_codecs}.sh`, `tests/validate_media.sh`
  - `README.md`, `MISSING_FEATURES.md`

## Verification

- `ctest` under the `default`, `ci` and `asan` presets: `check_abi`,
  `test_msacm32`, the extended `test_xform_audio`, smoke and media tests.
- `VFW_TRACE=1 build/seance probe build/fixtures/testacm.acm` shows the
  `ACMDM_*` conversation, with no trap hits.
- Round trips:
  - `seance encode testacm.acm in.wav -o a.avi && seance decode
    testacm.acm a.avi -o out.wav && cmp`;
  - the same crossed with `testfilter.ax` and `testdmo.dll`;
  - `decode auto` with `testacm.acm` registered in `vfw.ini`.
- Optional: `tests/audio_codecs.sh` with Windows' or open-source ACM drivers
  (IMA and MS ADPCM, A-law, MP3, AC-3), compared with FFmpeg's decoding.
