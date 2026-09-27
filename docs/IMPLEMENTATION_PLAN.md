# VfW Harness: audit and implementation plan

## Context

The goal is to load and run arbitrary 32-bit Video for Windows (VfW / ICM) codec DLLs natively on Linux, both to test them and to run them on real files. Decisions already made with the user:
- Use a custom PE loader with no Wine dependency.
- **Make it completely generic.** Nothing in the design, stubs or tests may depend on any particular codec DLL.
- Read AVI input and write Y4M, raw or BMP output.

## Audit: what exists today

`main` and `claude/dreamy-ptolemy-awki9u` are identical (2 commits). **There is no harness code at all.** The repo contains:

| File | State |
|---|---|
| `README.md` | Title only. |
| `LICENSE` | GPLv2. |
| `.gitignore` | Stock C template. It ignores `*.dll`/`*.exe`. |
| `vcmimm6 (2).dll` | A third-party codec that was uploaded. It was not a design input and has since been removed from the repo (Phase 0). |

**Everything is still missing:** the PE loader, the Win32 runtime, the ICM layer, AVI I/O, output writers, the CLI, tests, CI and docs.

**Environment gap:** the container's gcc cannot link `-m32`. The build needs `gcc-multilib`, and the in-repo test codecs need `mingw-w64` (i686).

## Architecture

The harness is one 32-bit C11 program (`gcc -m32`), because the codecs are i386 code:

```
cli: vfw-harness info | probe | decode | encode | list
 ├─ avi/     RIFF AVI + OpenDML reader/writer
 ├─ out/     y4m / bmp / raw writers, colour conversion
 ├─ vfw/     mini msvfw32: driver registry, ICOpen/ICLocate/ICSendMessage + typed wrappers
 └─ loader/  PE loader (recursive deps), TEB/threads, SEH, import resolver
      └─ win32/  stub DLL modules (kernel32, ntdll-lite, msvcrt family, user32, gdi32,
                  advapi32, winmm, version, ole32-lite, msvfw32 re-export)
```

### Loader (`src/loader/`)

- **`pe.c`:** a PE32 loader that fully follows the spec, with no assumptions about any particular image.
  - Validate the DOS/NT headers and the machine type.
  - `mmap` at ImageBase. If that fails, map anywhere and apply `.reloc` (HIGHLOW; reject a stripped-reloc DLL with a clear error).
  - Set per-section `mprotect` flags.
  - Imports: by name, by ordinal, and with bound-import tolerance.
  - Delay-load imports. Phase 1 parses and lists them; binding them eagerly moves to Phase 2, because a delay-loaded DLL's helper code imports kernel32.
  - Export lookup by name and by ordinal, including **forwarded exports** (`DLL.func`).
  - **TLS directory:** set up the TLS slot index and callbacks.
  - A resource directory walker. Codecs use this for strings, dialogs, version info and manifests.
  - Entry-point calls for PROCESS/THREAD ATTACH/DETACH.
- **`modules.c`:** a module table with refcounts. `LoadLibrary` resolves names in this order:
  1. Built-in stub modules.
  2. Real PE DLLs found on a search path (the codec's directory, `$VFW_PATH`). This covers codecs that ship helper DLLs.
  3. A clear "missing DLL" error.

  It also handles `GetModuleHandle` and `GetProcAddress`, including `GetProcAddress` on built-in modules.
- **`teb.c`:**
  - One TEB/TIB per thread (SEH head, self pointer at `+0x18`, StackBase/Limit, PEB pointer, TLS slots at `+0xE10`, LastError), plus a minimal PEB (ProcessHeap, loader data).
  - The TEB is installed through `modify_ldt` into `%fs`. glibc i386 uses `%gs`, so `%fs` is free.
- **`seh.c`:** real SEH dispatch. A SIGSEGV/SIGILL/SIGFPE/SIGBUS handler builds EXCEPTION_RECORD and CONTEXT, walks the `fs:[0]` chain, and honours ExceptionContinueExecution or the unwind that follows. This is needed because many codecs use `__try` or C++ EH. `RaiseException` and `RtlUnwind` use the same path. An unhandled fault prints `module+RVA` and a register dump.
- **`thunks.c`:**
  - Every call from the harness into a DLL goes through `call_dll()`. It aligns the stack to 16 bytes, switches the x87 control word to the Windows default `0x27F` and back, and makes sure `%fs` holds the thread's TEB.
  - Imports that are not implemented get a generated trap thunk that logs `DLL!name` or `DLL!#ord` and aborts. `info` lists them ahead of time.
  - `VFW_TRACE=1` logs every stub call.

### Win32 runtime (`src/win32/`)

- **Registry:** `{dll, name, ordinal, ptr, is_data, calling convention}`. DLL names are matched case-insensitively. `msvcrt`, `msvcr70`–`msvcr120`, `msvcp*`-lite and the `api-ms-win-crt-*` names all map to one CRT module. Data exports are supported (`_iob`, `_pctype`, `__mb_cur_max`, `_adjust_fdiv`, `_environ`, and so on).
- **Calling conventions:** Win32 stubs are `stdcall`, CRT stubs are `cdecl`. Both use `force_align_arg_pointer`, because callers only guarantee 4-byte alignment.
- **kernel32 / ntdll-lite:**
  - **Memory:** Heap* on malloc with a size header, Local/Global*, Virtual* on mmap/mprotect.
  - **Sync:** critical sections (pointer to a recursive pthread mutex stored inside the 24-byte struct), Interlocked*, events, mutexes and semaphores on pthread, WaitFor{Single,Multiple}Object(s).
  - **Threads:** CreateThread (pthread plus a new TEB and THREAD_ATTACH notifications), Tls*, and Fls* falling back to Tls.
  - **Time:** GetTickCount, QPC, GetSystemTime*, GetLocalTime, Sleep.
  - **Process info:** GetCurrent*, IsDebuggerPresent, GetVersion(Ex) reporting XP SP3, GetSystemInfo, IsProcessorFeaturePresent.
  - **Files:** CreateFile/ReadFile/WriteFile/Close/GetFileSize, mapped to a sandbox root.
  - **Environment and command line:** Get/SetEnvironmentVariable, GetCommandLine, GetStartupInfo, GetStdHandle.
  - **Strings and code pages:** MultiByteToWideChar/WideCharToMultiByte, GetACP/GetCPInfo, lstr*.
  - **Errors:** Get/SetLastError, Set/UnhandledExceptionFilter, TerminateProcess/ExitProcess, OutputDebugString (to stderr).
  - **Modules and resources:** LoadLibrary*/FreeLibrary/GetProcAddress/GetModuleFileName, FindResource/LoadResource/LockResource, private-profile INI functions.
- **CRT (msvcrt family):**
  - Forward to glibc where the semantics match (math, string, memory, strto*, sscanf).
  - Provide an MSVC-layout `FILE _iob[]` that maps to stdio.
  - `printf`-family shims for MSVC-only specifiers (`%I64d`).
  - MSVC `_pctype` table.
  - Startup support: `_initterm`/`_initterm_e`, `_onexit`/`__dllonexit`/`atexit`, `_lock`/`_unlock`, `_encode`/`_decode_pointer`, `_malloc_crt` and friends, `_except_handler3`/`4_common` (routed into `seh.c`), `__CxxFrameHandler*` (minimal C++ EH), `_CIpow`-style intrinsics, `_ftol`/`_ftol2`, `_allmul`, `_alldiv` and so on.
  - Codecs with a statically linked CRT need only the kernel32 side.
- **user32 / gdi32:** headless. Window, dialog and message functions return failure or no-ops with fake HWND/HDC values. MessageBox prints to stderr and returns IDOK. LoadString, wsprintf, GetDC and CreateDIBSection work on memory buffers. GetSystemMetrics returns sane defaults.
- **advapi32:** Reg* backed by a per-codec INI or JSON file (`$VFW_REGISTRY`, empty by default), so codec settings persist and can be seeded by tests.
- **winmm:** DefDriverProc following Wine's `dlls/winmm/driver.c` semantics, timeGetTime, SendDriverMessage/OpenDriver on top of `vfw/`.
- **version and ole32-lite:** GetFileVersionInfo* from the resource walker. CoInitialize and CoTaskMemAlloc as trivial stubs.
- **msvfw32 re-export:** some codecs call back into ICM, so they are given the harness's own `vfw/` API.

The stub list is based on what VfW codecs use in general (with Wine and MPlayer's loader as the behavioural reference), not on any single DLL. The trap thunks plus `info` show what is left to add.

### VfW layer (`src/vfw/`)

- **`driver.c`:**
  - Installable-driver lifecycle: `DRV_LOAD`, `DRV_ENABLE`, `DRV_OPEN` with ICOPEN, then `DRV_CLOSE`, `DRV_DISABLE`, `DRV_FREE`.
  - A driver table maps `fccType.fccHandler` to a DLL path, taken from `vfw.ini` (a `[drivers32]`-style file, the Linux equivalent of `system.ini`) or from the command line.
  - Both entry styles are supported: `DriverProc` export only, and codecs that also export ICM-specific helpers.
- **`icm.c`:** the msvfw32 API.
  - Opening and dispatch: ICOpen, ICOpenFunction, ICLocate, ICInfo (enumeration), ICGetInfo, ICSendMessage, ICClose.
  - Decompress: ICDecompressQuery/GetFormat/GetPalette/Begin/Decompress/End, and ICDecompressEx*.
  - Compress: ICCompressQuery/GetFormat/GetSize/Begin/Compress/End, plus ICSeqCompressFrame* (keyframe rate, quality, data rate).
  - Other messages: ICGetState/SetState, ICGetDefaultQuality/KeyFrameRate, ICM_DRAW* (reported as unsupported), and ICConfigure/ICAbout (report "no GUI").
  - Output-format negotiation: first try the codec's own `GET_FORMAT`, then a preference list (I420, YV12, YUY2, BGR32, BGR24, RGB565, RGB555, 8-bit palettised) checked with `DECOMPRESS_QUERY`.

### Container and output (`src/avi/`, `src/out/`)

Status: the AVI reader and writer, the DIB conversion and the Y4M/BMP/raw
writers are done, with the `avi` and `convert` commands; `make validate-media`
checks them against FFmpeg. Still open here: Y4M input for `encode`, and raw
frame input.

- **AVI reader:**
  - RIFF `AVI `, `avih`, and `strl` → `strh`, `strf` (full BITMAPINFOHEADER including extradata or palette), `strn`.
  - Read the `movi` `##dc`/`##db`/`##pc` chunks.
  - Keyframes come from `idx1`; the fallback is a linear scan. Also support OpenDML `indx`/`ix##`/`AVIX` (larger than 1 GB), and multiple streams, where you pick the video stream.
- **AVI writer:** for `encode`, writes `idx1` plus OpenDML when the file exceeds 1 GB.
- **Raw input:** frames from a directory or a length-prefixed stream, plus a user-given BITMAPINFOHEADER, for codecs that have no AVI sample.
- **Writers:**
  - Y4M: I420 passthrough or conversion from other formats (BT.601, limited range).
  - BMP per frame and raw.
  - Handle bottom-up DIBs, stride padding and palettes.

### CLI (`src/main.c`)

- `info <dll>`: PE summary, sections, exports, and each import marked stubbed, trap or missing-DLL.
- `list`: the drivers registered in `vfw.ini`.
- `probe <dll|fcc> [in.avi]`: ICINFO, plus the input and output formats the codec accepts.
- `decode <dll|fcc> <in.avi|raw> -o out.{y4m,bmp%05d,raw} [--fmt ...] [--frames N] [--state file]`
- `encode <dll|fcc> <in.y4m|bmp…> -o out.avi [--quality Q --keyint N --bitrate B --state file]`

## Phases and milestones

All milestones are checked against **test fixtures built in the repo**, never against the uploaded DLL.

0. **Skeleton.**
   - Makefile (i386 build, `make test`), `include/win32.h` (types, structs, DRV_*/ICM_* constants modelled on Wine headers).
   - README with prerequisites and usage.
   - Remove the uploaded third-party DLL.
   - `make test` runs unit tests, an ABI cross-check of `include/win32` against the mingw-w64 headers, and a CLI smoke test.
1. **PE loader and `info`.** Fixtures are mingw-built DLLs under `tests/fixtures/`:
   - `pe_basic.dll`: exports by name and by ordinal, plus a forwarder.
   - `pe_reloc.dll`: loaded with `VFW_FORCE_RELOC=1`.
   - `pe_tls.dll`: TLS callbacks.
   - `pe_dep.dll`: imports another fixture DLL.

   Milestone: all of these load, and exported functions return the expected values.
2. **Win32 runtime.** TEB/`%fs`, threads, SEH, the stub registry and trap thunks, and the stub modules.

   Milestone: the fixtures exercise these checks:
   - The `__try`/`__except` fixture recovers from an access violation.
   - The thread fixture reads per-thread TLS back correctly.
   - The CRT fixture, linked against `msvcrt.dll`, prints through `_iob` and uses `_pctype`.
   - The registry fixture round-trips a value.
3. **VfW layer and `probe`.** `tests/fixtures/testcodec.dll` is a mingw-built lossless ICM codec (FourCC `TST0`). It can compress and decompress, keeps its state through Get/SetState, and supports several output formats.

   Milestone: `probe` lists the correct ICINFO, accepted formats and state size.
4. **AVI and Y4M I/O, `decode` and `encode`.** Milestone: encoding a Y4M clip to AVI with `testcodec` and decoding it back gives a byte-exact result. The AVI output is validated with `ffprobe` when it is installed. The OpenDML path is tested with a large synthetic file.
5. **Hardening and CI.**
   - Keep DLL-visible memory below 2 GB. On a 64-bit kernel a 32-bit process gets addresses up to 4 GB, and glibc's heap and `mmap` live near `0xf7000000`. Windows code without `LARGE_ADDRESS_AWARE` may treat pointers as signed. Options: an `ADDR_LIMIT_3GB`/low-memory personality with a re-exec, or serving Win32 heaps from a low reservation.
   - Build a matrix of fixtures, including 16-byte alignment and SSE use in the codec and C++ exceptions across a codec boundary.
   - A fuzz-style AVI parser test (done: `test_avi` truncates and corrupts files).
   - A GitHub Actions workflow installs `gcc-multilib` and `mingw-w64` and runs `make test`.
   - An optional `make smoke SAMPLES=dir` runs `info` and `probe` over any user-supplied codec DLLs and reports missing imports, without asserting anything specific to a codec.

## Verification

- `apt install gcc-multilib mingw-w64`, then `make`, then `make test`. This runs the unit tests (reloc math, AVI parser, colour conversion) and the fixture-DLL integration tests from Phases 1–4.
- `VFW_TRACE=1 ./vfw-harness probe tests/fixtures/testcodec.dll` shows the full DRV/ICM sequence with no trap hits.
- Round trip: `./vfw-harness encode testcodec.dll in.y4m -o t.avi && ./vfw-harness decode testcodec.dll t.avi -o out.y4m && cmp in.y4m out.y4m`.
- Optional manual check against any real codec: `./vfw-harness info <any.dll>` to see import coverage, then `probe` and `decode` on a matching AVI.
