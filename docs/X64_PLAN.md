<!-- SPDX-License-Identifier: GPL-2.0-only -->
# seance: 64-bit (x86-64) codec plan

## Context

seance runs i386 codecs: VfW and ACM drivers, DirectShow filters and DMOs
built for 32-bit Windows. Codecs built for 64-bit Windows (PE32+, machine
type `0x8664`) are listed as impossible in `docs/MISSING_FEATURES.md`
(`cinevf2w.dll`, and the x64 builds of LAV Filters and most current
codecs). This plan adds them, through the same commands (`info`, `probe`,
`decode`, `encode`, `register`, `list`, `sweep`), without changing what
the i386 build does.

A process can hold code for one architecture only, and everything the
harness does to host a DLL changes on x86-64:

- **Calling convention.** Win64 has one, which is not the SysV x86-64 one
  Linux uses: arguments in `rcx`, `rdx`, `r8`, `r9`, 32 bytes of shadow
  space, `xmm6`-`xmm15` callee-saved. gcc and clang call and implement it
  with `__attribute__((ms_abi))`; varargs need `__builtin_ms_va_list`.
- **LLP64.** Windows' `long` is 32-bit, Linux x86-64's 64-bit. The Win32
  headers already use fixed-width types, but the CRT's own functions
  (`strtol`, `ftell`, `_lseek`, ...) use the host's `long`.
- **The TEB** is at `gs:` with a different layout (`Self` at 0x30, the
  TLS array at 0x58, `LastErrorValue` at 0x68), set through the GS base
  rather than an LDT segment.
- **Exceptions are table-based.** There is no `fs:[0]` chain: each
  function has a `RUNTIME_FUNCTION` in `.pdata` and unwind codes in
  `.xdata`, and dispatch unwinds frames virtually
  (`RtlVirtualUnwind`, `RtlUnwindEx`, `__C_specific_handler`). C++
  exceptions use `__CxxFrameHandler3`/`4`, funclets, and image-relative
  RVAs in their throw and RTTI data.
- **The image.** PE32+ has a 64-bit `ImageBase`, 8-byte thunks and TLS
  callbacks, `DIR64` relocations, and no `BaseOfData`.

Decisions:
- **Two binaries from one tree.** `seance` stays i386; a build with
  `VFW_ARCH=x86_64` makes `seance64`. What differs goes behind
  `#if defined(__x86_64__)` in a few headers (`include/win32/types.h`,
  `src/runtime/builtin.h`, `src/win32/internal.h`) and into separate
  `*_x64.c` files where the mechanism differs (TEB, SEH, C++ EH,
  setjmp).
- **Parsing is the same in both.** The loader parses PE32 and PE32+ in
  either build; only mapping and running need the matching one. `seance
  info` on a 64-bit DLL works from the i386 binary.
- **Hand-off.** Given a DLL of the other architecture, either binary
  `exec`s the other with the same arguments, so one command serves both.
- **Calling conventions centrally.** On x86-64, `WINAPI`, `CALLBACK`,
  `CDECL`, `STUB_WINAPI`, `STUB_CDECL` and `THISFN` all become
  `ms_abi`; the ~1,800 `WINFN`/`CRTFN` stubs and every COM vtable follow.
- **The ABI check is the safety net.** `check_abi` runs against
  `x86_64-w64-mingw32` too, so every structure with a pointer in it is
  compared with Windows' layout.

## Phases

0. **PE32+ structures.** (Done.) `include/win32/winnt.h` has
   `IMAGE_OPTIONAL_HEADER64`, `IMAGE_NT_HEADERS64`, `IMAGE_THUNK_DATA64`,
   `IMAGE_TLS_DIRECTORY64`, `IMAGE_RUNTIME_FUNCTION_ENTRY`, and the
   `AMD64`, `HDR64_MAGIC`, `DIR64`, `ORDINAL_FLAG64` and
   `HIGH_ENTROPY_VA` constants, defined whatever the build (they are file
   layouts). Windows packs the 64-bit headers to 4 bytes, and so do we.
   Milestone: `check_abi` passes with them in `abi_list.h`.
1. **PE32+ parsing.** (Done.) `src/loader/pe.c` reads both formats into
   one `struct pe_optional` (the widened fields at 64 bits), walks 8-byte
   thunks with the ordinal flag in bit 63, reads 64-bit TLS directories
   and callback arrays, and applies `DIR64` relocations with the full
   64-bit delta (HIGHLOW and the 16-bit types still use its low half).
   `pe_map` refuses an image of the other architecture, naming the binary
   that runs it; `pe_peek_machine` reads a file's machine type from its
   headers alone, for the hand-off. `seance info` prints the
   architecture and a 64-bit image base.
   Milestone: `test_pe_parse` parses the 64-bit fixtures (exports by name
   and ordinal, forwarders, 8-byte imports, TLS, resources) and survives
   the truncation and corruption runs on them; `test_pe_reloc` checks
   `DIR64`; `samples_fixtures64` runs `info` and `probe` over them.
   - `tests/fixtures` builds a second time with
     `cmake/mingw-x86_64.cmake` into `build/fixtures64`: `pe_basic`,
     `pe_reloc`, `pe_noreloc`, `pe_tls` and `pe_dep`, at image base
     `0x180000000` (above 4 GB, so a lost upper half shows), entry point
     `DllMain` (no stdcall decoration). Tests find them through
     `$VFW_FIXTURES64` (`fixture64()` in `tests/unit/test.h`);
     `VFW_REQUIRE_MINGW64` (on in the `ci` preset) makes them required.
2. **The x86-64 build.** (Done.) `cmake/x86_64.cmake` beside `cmake/i386.cmake`,
   chosen by `VFW_ARCH=x86_64` (default `i386`); its probe needs
   `__x86_64__`, `asm/prctl.h` and `SYS_arch_prctl`. The output is
   `seance64`; cross builds run through `qemu-x86_64`.
   - `types.h` accepts `__x86_64__` and makes the conventions `ms_abi`.
     `force_align_arg_pointer` goes: Win64 keeps the stack 16-aligned.
   - `abi_list.h` splits into common and per-architecture parts (the
     i386 `CONTEXT`, `FLOATING_SAVE_AREA` and the PE32 thunk types on one
     side, the AMD64 `CONTEXT`, `M128A` and `XMM_SAVE_AREA32` on the
     other); a `check_abi64` test compiles it with
     `x86_64-w64-mingw32-gcc`.
   - Every i386 assembly block (`src/runtime/seh.c`, `teb.c`, `trap.c`,
     `crash.c`, `src/win32/crt_seh.c`, `crt_cxx.c`, `crt_helpers.c`,
     `crt_math.c`, `crt_legacy.c`; about 28) gets an x86-64 version or is
     left out of that build, with the exports that need it.
   Milestone: `seance64` builds and the unit tests that don't run DLL
   code pass.
   - What the 64-bit `check_abi` found, now fixed: `commdlg.h` and
     `shellapi.h` are byte-packed on Win32 only; `SLIST_HEADER` is 16
     bytes (with `HeaderX64`); `ACMSTREAMHEADER` has 15 reserved DWORDs;
     `SECURITY_DESCRIPTOR_MIN_LENGTH` is 40; `GWL_WNDPROC` and the other
     pointer indices exist only as `GWLP_`/`DWLP_`.
   - The i386-only parts (the fs:[0] SEH in `seh.c` and `crt_seh.c`, the
     i386 C++ EH, `_allmul` and the other helpers, `_CI*`, `_EH_prolog`)
     are left out of the x86-64 build; `seh_x64.c` sends exceptions to
     the unhandled-exception filter until phase 4.
3. **Loading and running.** (Mostly done; what remains is marked.)
   - `src/runtime/teb_x64.c`: the x64 TEB and PEB (offsets checked with
     `_Static_assert`), installed with `arch_prctl(ARCH_SET_GS)`. glibc
     uses `fs` for its own TLS on x86-64, so `gs` is free. The
     `teb_init_thread`/`teb_exit_thread` interface stays, so
     `runtime_enter` and `CreateThread` don't change. MXCSR is set to
     0x1f80 for each thread.
   - `lowmem` does nothing on x86-64 (it already checks the pointer
     size); `lowmem_run` keeps the worker thread for its stack.
   - Trap thunks for unimplemented imports become
     `movabs rax, imm64; jmp [rip]; .quad target`.
   - The i386-only exports (`_allmul` and the other 64-bit helpers,
     `_ftol`, the `_CI*` math, Win9x thunks) are left out of the x64
     tables; the x64-only ones are added: `Get/SetWindowLongPtrA/W`,
     `Get/SetClassLongPtrA/W`, `RtlCaptureContext`,
     `RtlLookupFunctionEntry`, `RtlVirtualUnwind`, `RtlUnwindEx`,
     `RtlAddFunctionTable`, `RtlDeleteFunctionTable`,
     `__C_specific_handler`, `__chkstk`, `_setjmpex`.
   - The CRT exports that take or return a Windows `long` (`strtol`,
     `strtoul`, `atol`, `labs`, `_ltoa`, `_ultoa`, `fseek`, `ftell`,
     `_lseek`, `_scalb`, `__p__timezone`) use `LONG`: a Win64 caller
     leaves the upper half of a 32-bit argument undefined, and the
     results clamp to 32 bits as MSVC's do. `scanf`'s `%l` integers are
     32-bit too.
   - Varargs exports (`printf` and `scanf` families, `wsprintfA/W`,
     `_snprintf`, `_open`, the `v*` forms) take `win_va_list`
     (`src/win32/winva.h`: `__builtin_ms_va_list` on x86-64). `wsprintf`
     reads 32-bit integers and pointer-sized `%p`. In the CRT's `printf`
     and `scanf`, `%I` alone and `z`/`t` are 64-bit on Win64, and `%p`
     has 16 digits.
   - Callbacks the CRT makes into the DLL (`qsort` and `bsearch`
     comparators, `signal` handlers) use its convention; `qsort` goes
     through `qsort_r` rather than handing the comparator to glibc.
   - `_setjmp`, `_setjmpex` and `longjmp` with the Win64 `_JUMP_BUFFER`
     (`src/win32/crt_seh_x64.c`), restoring registers without unwinding;
     `operator new` and `delete` under their x64 names, and `_purecall`.
   - `WNDCLASS` is copied field by field, not as the tail of a
     `WNDCLASSEX` (the padding differs on x86-64); the lock words of SRW
     locks, condition variables and `InitOnce` are pointer-sized.
   Milestone (reached): the 64-bit `pe_basic`, `pe_reloc`, `pe_tls`,
   `pe_dep` and `pe_crt` load in `seance64`, `DllMain` and TLS callbacks
   run, and their exports return the right values.
4. **Table-based SEH.** `src/runtime/seh_x64.c`, beside the i386
   `seh.c`: function lookup in each module's sorted `.pdata` and in tables
   added with `RtlAddFunctionTable`; a virtual unwinder for every unwind
   code (`PUSH_NONVOL`, `ALLOC_*`, `SET_FPREG`, `SAVE_NONVOL*`,
   `SAVE_XMM128*`, `PUSH_MACHFRAME`), chained unwind info and epilogues;
   two-phase dispatch through the language handlers and `RtlUnwindEx`;
   `RtlCaptureContext`, `RtlRestoreContext`, `RaiseException` and
   `__C_specific_handler`. Faults turn the signal context into an AMD64
   `CONTEXT` and dispatch on the thread's own stack, as on i386.
   `setjmp`/`longjmp` use the x64 `_JUMP_BUFFER`, and `longjmp` unwinds
   when its `Frame` is set.
   Milestone: 64-bit SEH fixtures pass: `__try`/`__except` around an
   access violation, `__finally` on unwind, `RaiseException` with a
   filter, and `longjmp` across frames.
5. **C++ exceptions and RTTI.** `src/win32/crt_cxx_x64.c`:
   `_CxxThrowException` with the image base as the fourth parameter and
   RVA-based `ThrowInfo`; `__CxxFrameHandler3` and `__CxxFrameHandler4`
   (state maps, catch funclets, unwind actions); `__RTtypeid`,
   `__RTDynamicCast` and `__RTCastToVoid` with the x64 complete object
   locator.
   Milestone: a 64-bit C++ exception fixture laid out as cl compiles it
   throws, catches by type, rethrows and destroys on unwind; mingw's own
   `__gxx_personality_seh0` code works through phase 4.
6. **Codecs.** (Done.) The 64-bit fixture set is every fixture but the
   i386-only `pe_seh`, `pe_sehcrt`, `pe_cxxeh` and `pe_thunk`, including
   `testcodec`, `testacm`, `testfilter`, `testdmo` and `pe_delay` (whose
   hand-written delay-load tables take the pointer width). On x86-64 the
   fixture build strips the stdcall decorations from the `.def` files and
   drops `-mpreferred-stack-boundary=2`, and every fixture has one
   absolute pointer so GNU ld gives it a `.reloc` (it leaves one out when
   nothing needs relocating). The VfW, ACM, DirectShow and DMO unit
   tests and `validate_media` pass with them under `seance64`.
   Milestone (reached): `seance64 probe`, `encode` and `decode` with each
   64-bit test codec give what the i386 ones do, and `seance64`'s
   `smoke` runs over the 64-bit fixtures (all but its 2 GB checks).
7. **One command for both.** (Done.) `src/handoff.c`: `src/main.c`
   checks the DLL that `probe`, `decode`, `encode`, `register` or
   `unregister` names with `pe_peek_machine` before running it, and
   `exec`s the other binary (`$VFW_SEANCE64`/`$VFW_SEANCE32`, else
   `seance64`/`seance` beside itself, through `$VFW_EMULATOR64`/
   `$VFW_EMULATOR32` if set; `VFW_EMULATOR` is this binary's own) for the
   other architecture. A codec found later (a FourCC from `vfw.ini`, a
   CLSID from the registry) hands off when `pe_map` meets it as the first
   image it maps: a hook that the first native image disarms, so a
   foreign dependency stays an error. `decode auto` tries every codec and
   keeps to its own binary's. `VFW_HANDED_OFF` in the sibling's
   environment stops a second hand-off. `sweep` runs `seance` per step,
   so each step goes to the binary its DLL needs; `register` runs
   `DllRegisterServer` in the matching one; `list` shows each entry's
   architecture and doesn't open the other kind.
   Milestone (reached): `seance probe` of a 64-bit codec works from the
   i386 binary and the reverse, a round trip through the 64-bit
   `testcodec` from `seance` matches the i386 one, and `sweep` over both
   runs both (`tests/handoff.sh`).
8. **CI and real codecs.** (CI done: the default build makes and tests
   both binaries, so the existing `test`, `sanitizers` and `minimal` jobs
   cover `seance64`, and `install` puts both side by side.) Real 64-bit codecs
   (`cinevf2w.dll`, LAV Filters x64) run through `sweep` and
   `validate_media.sh`, with the results in `MISSING_FEATURES.md`.

## Critical files

- New: `cmake/mingw-x86_64.cmake`, `cmake/x86_64.cmake`,
  `src/runtime/teb_x64.c`, `src/runtime/seh_x64.c`,
  `src/win32/crt_seh_x64.c`, `src/win32/crt_cxx_x64.c`.
- Modified: `include/win32/winnt.h`, `include/win32/types.h`,
  `src/loader/pe.c`, `src/loader/pe.h`, `src/runtime/builtin.h`,
  `src/win32/internal.h`, `src/runtime/trap.c`, `src/runtime/crash.c`,
  `src/win32/crt_printf.c`, `src/win32/u32_window.c`, `src/main.c`,
  `src/cmd_sweep.c`, `CMakeLists.txt`, `CMakePresets.json`,
  `tests/fixtures/CMakeLists.txt`, `tests/abi/abi_list.h`,
  `tests/unit/test.h`.

## Verification

- `cmake --preset ci && cmake --build build && ctest --test-dir build`:
  the i386 tests, unchanged, plus the 64-bit parsing tests and
  `samples_fixtures64`.
- `build/seance info build/fixtures64/pe_basic.dll` shows `x86-64` and
  image base `0x0000000180000000`; `build/seance probe` of it says that
  `seance64` runs it.
- The default build makes both: `cmake --build build` configures this
  project again with `VFW_ARCH=x86_64` in `build/x86_64` (reusing the
  fixture DLLs) and puts `seance64` beside `seance`; `ctest --test-dir
  build` runs seance64's tests as the test `x86_64`. Run alone
  (`ctest --test-dir build/x86_64`) they are the same 55 tests against
  the 64-bit fixtures; those for i386-only mechanisms (`test_seh`,
  `test_cxxeh`, `test_crt_helpers`, `test_lowmem`) skip.
  `-DVFW_BUILD_X86_64=OFF` builds `seance` only; a directory configured
  with `-DVFW_ARCH=x86_64` builds `seance64` only.
