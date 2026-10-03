<!-- SPDX-License-Identifier: GPL-2.0-only -->
# seance: audit and implementation plan

## Context

The goal is to load and run arbitrary 32-bit Video for Windows (VfW / ICM) codec DLLs natively on Linux, both to test them and to run them on real files. Decisions already made with the user:
- Use a custom PE loader with no Wine dependency.
- **Make it completely generic.** Nothing in the design, stubs or tests may depend on any particular codec DLL.
- Read AVI input and write Y4M, raw or BMP output.

## Audit: what exists today

This section records the starting point, before any harness code existed; the rest of the plan marks what has been done since. At the time, `main` and `claude/dreamy-ptolemy-awki9u` were identical (2 commits) and the repo contained:

| File | State |
|---|---|
| `README.md` | Title only. |
| `LICENSE` | GPLv2. |
| `.gitignore` | Stock C template. It ignores `*.dll`/`*.exe`. |
| `vcmimm6 (2).dll` | A third-party codec that was uploaded. It was not a design input and has since been removed from the repo (Phase 0). |

Everything was missing then: the PE loader, the Win32 runtime, the ICM layer, AVI I/O, output writers, the CLI, tests, CI and docs.

**Environment gap:** the container's gcc cannot link `-m32`. The build needs `gcc-multilib`, and the in-repo test codecs need `mingw-w64` (i686).

## Architecture

The harness is one 32-bit C11 program (`gcc -m32`), because the codecs are i386 code:

```
cli: seance info | probe | decode | encode | list
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
  - Delay-load imports. Phase 1 parses and lists them; Phase 2 binds them eagerly at load time (done), so the DLL's own delay-load helper never runs. A delay-loaded DLL or function that can't be had doesn't fail the load (the DLL would only have failed on first use): it is bound to a trap that says why when called.
  - Export lookup by name and by ordinal, including **forwarded exports** (`DLL.func`).
  - **TLS directory:** set up the TLS slot index and callbacks, and the implicit TLS template that `teb.c` copies into every thread's block (`fs:[0x2c]`), including threads that exist when the DLL is loaded (done).
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
- **`seh.c`:** real SEH dispatch (done). A SIGSEGV/SIGILL/SIGFPE/SIGBUS handler builds an EXCEPTION_RECORD and CONTEXT and walks the `fs:[0]` chain - but not there: a handler that jumps away (as one that decides to run a `__except` body does) needs to do that off the signal's alternate stack, with the faulting signal unblocked, or the jump would abandon the kernel's signal frame with the signal blocked forever. So the fault handler instead rewrites the saved context to resume in a trampoline on the thread's own stack once it returns, and the chain is walked from there, in ordinary call context, where a handler's jump behaves like a longjmp. `RaiseException`, `RtlUnwind` and Set/UnhandledExceptionFilter (kernel32; `RtlUnwind` is also given by ntdll) go through the same chain-walking loop without needing any of that, since they already run in ordinary context. `tests/fixtures/pe_seh.c` installs frames by hand (no CRT to compile `__try`/`__except` into `_except_handler3` frames here) and checks recovery from a single frame, a chain of several with an unwind in between, RaiseException reaching a frame, a top-level filter, and that a fault nothing catches still crashes normally. A handler may raise an exception or fault in turn (a C++ throw out of a catch block, an SE translator, a filter probing memory under its own `__try`), and so may a `__finally` or destructor an unwind runs: each is dispatched from the head of the chain, up to 16 deep, and RtlUnwind takes each frame off the chain once its handler is done, as Windows does. Handlers are called through a stub that restores `%esp` afterwards, as Windows does, since the compiler's `_except_handler3/4` are cdecl while hand-written ones are often stdcall. msvcrt's side of `__try`/`__except`/`__finally` is in `crt_seh.c` (see the CRT task below).
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
  - **Threads:** CreateThread (pthread plus a new TEB, and DLL_THREAD_ATTACH / DLL_THREAD_DETACH to the loaded DLLs' TLS callbacks and, unless DisableThreadLibraryCalls, DllMain; done), Tls*, and Fls* falling back to Tls.
  - **Time:** GetTickCount, QPC, GetSystemTime*, GetLocalTime, Sleep.
  - **Process info:** GetCurrent*, IsDebuggerPresent, GetVersion(Ex) reporting XP SP3, GetSystemInfo, IsProcessorFeaturePresent.
  - **Files:** CreateFile/ReadFile/WriteFile/Close/GetFileSize, mapped to a sandbox root.
  - **Environment and command line:** Get/SetEnvironmentVariable, GetCommandLine, GetStartupInfo, GetStdHandle.
  - **Strings and code pages:** MultiByteToWideChar/WideCharToMultiByte, GetACP/GetCPInfo, lstr*.
  - **Errors:** Get/SetLastError, Set/UnhandledExceptionFilter, TerminateProcess/ExitProcess, OutputDebugString (to stderr).
  - **Modules and resources:** LoadLibrary*/FreeLibrary/GetProcAddress/GetModuleFileName, FindResource/LoadResource/LockResource, private-profile INI functions.
- **CRT (msvcrt family):** done, apart from what's still open below.
  - Forward to glibc where the semantics match (math, string, memory, strto*, `qsort`/`bsearch`, the CRT heap) - done (`crt_math.c`, `crt_string.c`).
  - An MSVC-layout `FILE _iob[]` that maps to stdio, `fopen` routed through the same `C:\` sandbox `CreateFile` uses, and the `_pctype` table plus the real `isupper`-family functions mingw-w64 compiles calls to rather than an inlined table lookup - done (`crt_stdio.c`).
  - Startup support: `_initterm`/`_initterm_e`, `_onexit`/`__dllonexit`/`atexit` (running at the harness's own process exit, skipping anything whose module was `mod_free`d first), `_lock`/`_unlock`, `_encode`/`_decode_pointer`, `_malloc_crt` and friends, `__set_app_type`, `_fmode`/`_commode` - done (`crt_startup.c`).
  - `_ftol`/`_ftol2`, `_allmul`/`_alldiv`/`_aulldiv`/`_allrem`/`_aullrem`/`_allshl`/`_allshr`/`_aullshr`: the 64-bit-arithmetic and float-truncation helpers every 32-bit MSVC-compiled codec using `__int64` calls, whatever CRT it links, even a statically linked one - done (`crt_helpers.c`; `_allmul`'s family get the real MASM-runtime calling convention, callee-pops-its-own-args, for free by declaring them `stdcall` over four 32-bit halves rather than a single `__int64`, since the two only differ in stack cleanup; `_ftol` and the shifts take their operands in EDX:EAX/CL with no stack arguments at all, which no C calling convention expresses, so those five are hand-written asm reproduced from their long-published disassembly).
  - SEH (`crt_seh.c`, done): `_except_handler3` and `_except_handler4_common` (the scope-table walk a /MD DLL's `__try` frames need; EH4's GS and EH cookies go through the DLL's own `__security_check_cookie`, as MSVC's handler does), `_local_unwind2/4`, `_global_unwind2` and `_abnormal_termination`. Also `_setjmp`, `_setjmp3` and `longjmp` (MSVC's jmp_buf: longjmp unwinds the frames set up since setjmp, running their `__finally` blocks and destructors, then that frame's own levels through `_seh_longjmp_unwind`, `_seh_longjmp_unwind4` (given the image's cookie by address, as cl passes it to `_local_unwind4`; checked against the frame's EH cookie before it is trusted) or `__CxxLongjmpUnwind`), which libjpeg-based codecs use for their errors. `pe_sehcrt.dll` lays out cl's frames and scope tables by hand and checks catching, EXCEPTION_CONTINUE_EXECUTION, nested levels, a callee's `__finally` unwound by its caller, returning out of a `__try`, the cookie checks (a smashed GS or EH cookie reaching the check function), longjmp out of EH3 and EH4 `__try` blocks, an exception (or longjmp) out of a `__finally` the unwind runs, caught further out, and a filter whose own `__try` takes a fault, each by RaiseException and by a real fault.
  - printf (`crt_printf.c`, done): msvcrt.dll's own dialect rather than glibc's: `I64`/`I32`/`I` sizes, `L` as a double, `%S`/`%ls`/`%C` as UTF-16, `%p` as eight hex digits, three-digit exponents, `1.#INF`/`1.#QNAN`/`1.#IND` with msvcrt's rounding of them (`%.2f` of infinity is `1.#J`), and `%Z` (an ANSI_STRING or UNICODE_STRING); also the wide family (`swprintf`, `_snwprintf`, `wprintf`, ...).
  - `_CI*` x87 intrinsics (`crt_math.c`, done): arguments on the x87 stack, with the caller's other x87 values and control word saved around the libm call.
  - C++ exceptions (`crt_cxx.c`, done, after Wine's except_i386.c): `_CxxThrowException` and `__CxxFrameHandler`/`2`/`3` for FuncInfo magics VC6 to VC8. Catch blocks are matched through the thrown object's type list (a class by its bases, by name as well as by descriptor, const and volatile respected), the object copied into the catch variable (by value with its copy constructor, by reference or pointer with the this-adjustment, virtual bases included), the frames above unwound with RtlUnwind and the catching function's own states down to the try, then the catch block called under a frame of its own that sees `throw;` and exceptions thrown out of it; a try block inside a catch block works, and the exception object is destroyed once, when the last catch block holding it ends. catch (...) takes SEH exceptions unless the FuncInfo says /EHs, where `_set_se_translator`'s translator can first turn them into C++ ones for the frame's catch blocks; a noexcept function calls terminate(), and one with an exception specification (VC7 on) calls unexpected() for a type it doesn't list. An exception with a magic past VC8 goes to its ThrowInfo's own handler. RTTI: `__RTtypeid`, `__RTDynamicCast` (single, multiple and virtual inheritance: down-casts to the instance holding the source, cross-casts to a public, unambiguous base) and `__RTCastToVoid`, which throw bad_typeid, bad_cast, or __non_rtti_object when the RTTI can't be read (through the harness's fault handler). Also terminate/unexpected and their setters (per thread, as in msvcrt), `__uncaught_exception(s)`, `_purecall`, `abort`, operator new/delete (the sized forms too) with `_set_new_handler` and `_callnewh`, `_set_new_mode` (under which malloc, calloc and realloc ask the new handler as well), type_info (name() undecorates classes, structs, unions, enums, basic types and pointers to them) with its vftable export, std::exception, bad_typeid, bad_cast and __non_rtti_object (one class each, answering to the msvcrt names and msvcr80's std:: ones, with RTTI of their own), C++/CLI's helpers (`__CxxExceptionFilter`, `__CxxDetectRethrow`, `__CxxRegister/UnregisterExceptionObject`, the `__CxxCallUnwind*Dtor` guards, StdDelDtor's managed destructor called through its native entry), and vcruntime140's `__std_type_info_*` and `__std_exception_*` (vcruntime140.dll is served by the same built-in). `pe_cxxeh.dll` lays out cl's frames and tables by hand and checks catching by value, by base reference and by copy, destructors of the frames in between and of the catching function's own locals, a catch for another type passing it on, rethrow, a new exception out of a catch block, a try inside a catch block and out of one to the try around it, const objects, SEH exceptions with and without /EHs, the SE translator, an exception specification, an exception with its own handler, the RTTI exceptions caught by either name, longjmp out of and within C++ frames, and (in a child) an unhandled exception, a noexcept function's terminate() and RTTI that faults to read; `test_cxxeh` checks the casts over multiple inheritance directly. msvcp's classes are here too, since the built-in answers to msvcp*.dll: logic_error and runtime_error with their subclasses, bad_alloc and bad_function_call (laid out as from VC8 on), and the helpers its containers and strings throw them through (`std::_Xlength_error`, `_Xout_of_range`, `_Xbad_alloc`, VC6's `_Xlen` and `_Xran`, ...). A destructor that throws while an exception unwinds its frame calls terminate(). Not done: the VC6/VC7 constructors of those classes that take a std::string, whose layout differs by version.
  - Codecs with a statically linked CRT need only the kernel32 side plus the arithmetic helpers above, which they call regardless.
- **user32 / gdi32:** headless (done).
  - user32 (`user32.c`, `u32_window.c`, `u32_string.c`): window classes and windows are real objects that get the messages Windows sends on create/show/move/destroy, with ANSI/UTF-16 text conversion between sender and window procedure; per-thread message queues with posted messages, WM_QUIT, timers and MsgWaitForMultipleObjects; window longs, extra bytes and properties. SendMessage to another thread's window runs on the caller's thread. Dialog boxes fail (there is no GUI) and say so. MessageBox prints to stderr and answers with its default button. LoadString, wsprintf/wvsprintf (own formatter: `%s`/`%S` follow the caller's width, not glibc's `wchar_t`), `Char*`, system metrics and colours (XP classic, 1024x768), rectangles, icons/cursors (empty handles), LoadBitmap/LoadImage, window stations and monitors.
  - gdi32 (`gdi32.c`, `gdi_bitmap.c`): a handle table with Windows' type byte and a generation count, stock objects, memory and display DCs with SaveDC/RestoreDC, DDBs and DIB sections (page-aligned, or in a file mapping), Get/SetDIBits, BitBlt/StretchBlt/PatBlt/StretchDIBits/SetDIBitsToDevice through one pixel layer that reads and writes every DIB format, palettes (default and halftone), brushes, pens, fonts and fixed text metrics. The display is a 32-bit device that discards drawing; text isn't drawn; stretching is nearest-neighbour and ROPs work on RGB values.
  - `pe_gui.dll` drives both from Win32 code with a 4-byte stack.
- **advapi32:** Reg* (done, `advapi32.c`, `reg.c`), backed by a regedit-format `.reg` file (`$VFW_REGISTRY`, else `$VFW_ROOT/registry.reg`; empty means in memory only), saved after each change, so codec settings persist and can be seeded by tests or from a Windows export (UTF-16 and REGEDIT4 files are read too). Also GetUserName, RegGetValue (subkey, type filter, NUL termination, `%VAR%` expansion), RegNotifyChangeKeyValue (synchronous or with an event; a watch fires once, on its filter's change in its key or subtree, on the key's deletion, or when its handle closes), Reg{Get,Set}KeySecurity and RegOpenCurrentUser. `HKLM\Software\Microsoft\Windows NT\CurrentVersion\Drivers32`, where msvfw32 on NT finds codecs, is a view of the codec table (`vfw.ini`'s `[drivers32]` and run-time registrations): it is never saved, and setting or deleting a `vidc.XXXX` value there registers or removes that codec for the process.
  - Security (`adv_security.c`, done): one pretend machine whose one user, named as GetUserName names them, is an XP administrator. Tokens (OpenProcessToken, OpenThreadToken while impersonating, ImpersonateSelf/RevertToSelf/SetThreadToken, DuplicateToken(Ex), GetTokenInformation for the user, groups, privileges, owner, primary group, type, statistics, elevation and integrity), AdjustTokenPrivileges and PrivilegeCheck over XP's administrator privileges with Windows' LUIDs, CheckTokenMembership; SIDs (allocation, comparison, well-known SIDs, string forms with SDDL's two-letter names) and LookupAccountSid/Name for the well-known, builtin and local accounts; ACLs and security descriptors, absolute and self-relative, laid out as Windows lays them out. Files and keys report one descriptor (owner Administrators, SYSTEM, Administrators and the user in full control, Users read), and setting one changes nothing: the harness enforces no access control. CryptAcquireContext/CryptGenRandom and RtlGenRandom (SystemFunction036) give the kernel's random numbers; there are no keys or hashes. Not done: SDDL strings for whole descriptors, SetEntriesInAcl, AccessCheck, the event log and services.
- **shell32:** done (`shell32.c`, `shell_file.c`). Special folders (SHGetFolderPath(AndSubDir), SHGetSpecialFolderPath/Location, SHGetFolderLocation) laid out as on XP under C:, the profile named after GetUserName's `$USER`, each created on first use as a real profile has them all; virtual folders (My Computer, the Recycle Bin, ...) have item ID lists but no path. Item ID lists are this shell32's own format (one item holding a path or a virtual folder's CSIDL; the desktop is the empty list) from the COM task allocator, with SHGetPathFromIDList, ILCreateFromPath, ILClone, ILGetSize, ILFree and SHGetMalloc. CommandLineToArgvW splits as Windows does (after Wine). SHFileOperation copies, moves, renames and deletes lists with wildcards and whole trees, FOF_MULTIDESTFILES, FOF_FILESONLY and FOF_RENAMEONCOLLISION ("Copy of"), never asking. SHCreateDirectoryEx, SHGetFileInfo (attributes, XP's display and type names, SHGFI_EXETYPE from the PE header, no icons). Headless: ShellExecute(Ex) and FindExecutable find no program (SE_ERR_NOASSOC, or SE_ERR_FNF for a missing file), SHBrowseForFolder is cancelled, ShellAbout and Shell_NotifyIcon succeed, there are no icons, dropped files or appbars. `pe_shell.dll` uses it from Win32 code. Not done: SHGetDesktopFolder and the IShellFolder side, SHGetKnownFolderPath, the Recycle Bin, shortcuts (IShellLink).
- **winmm:** DefDriverProc following Wine's `dlls/winmm/driver.c` semantics, timeGetTime, SendDriverMessage/OpenDriver on top of `vfw/`.
- **version:** GetFileVersionInfo(Size)(Ex)A/W and VerQueryValueA/W (done, `version.c`). The block is the RT_VERSION resource, sized as Windows sizes it (twice the resource plus the `FE2X` signature), but VerQueryValueA keeps its ANSI copies of strings in a table rather than the second half, since it isn't told how big the caller's buffer really is; names resolve the way LoadLibrary would (a loaded module, a built-in, then the search path from the caller's directory), and the built-in DLLs answer with a generated block versioned 5.1.2600.5512 (XP SP3). So a truncated copy or a raw resource from FindResource/LockResource is never written past or into. Not done: VerFindFile, VerInstallFile, VerLanguageName and the 16-bit (NE) format.
- **ole32-lite:** done (`ole32.c`). CoInitialize(Ex)/CoUninitialize and OleInitialize are counted per thread, with RPC_E_CHANGED_MODE and the implicit multithreaded apartment. CoTaskMem* and CoGetMalloc use the process heap; there are also GUID strings (StringFromGUID2/CLSID/IID, CLSIDFromString, IIDFromString), ProgIDs from the registry, and CoCreateGuid. CoCreateInstance and CoGetClassObject find classes registered with CoRegisterClassObject (honouring REGCLS_SUSPENDED with CoResumeClassObjects, REGCLS_SINGLEUSE, and a multiple-use local server serving in-process), or in-process servers named by `HKCR\CLSID\{...}\InprocServer32` (per-user classes first), which stay loaded. `pe_com.dll` is such a server and uses version.dll on itself. Not done: out-of-process servers, marshalling, CreateStreamOnHGlobal and the rest of OLE.
- **oleaut32:** done (`oleaut32.c`, `oleaut_conv.c`, `oleaut_array.c`, `errinfo.c`), after Wine's. BSTRs (a byte count, the characters and a NUL WCHAR, from the process heap). VARIANTs: VariantInit/Clear/Copy/CopyInd and VariantChangeType(Ex) through one conversion engine that the whole Var*From* family (192 functions) wraps: 64-bit integers, doubles and currency with rounding to even, DISP_E_OVERFLOW outside a type's range, VARIANT_TRUE as -1 (all ones when unsigned), strings read and written as in US English whatever the LCID (thousands separators, "$", parentheses, &H/&O filling the target's width; dates as M/D/YYYY, ISO or with month names, times with AM/PM), VT_DISPATCH through its value property, byte vectors to and from BSTRs; VT_DECIMAL is copied but not converted. Dates: SystemTime/DosDateTime to and from VariantTime, to the nearest second. SAFEARRAYs laid out as on Windows (bounds stored rightmost first, the leftmost index fastest, the element type, IID or IRecordInfo in the 16 bytes before the descriptor) with every Create/Copy/Redim/Get/Put/Lock function, owning elements copied and freed as such. CreateErrorInfo/SetErrorInfo/GetErrorInfo (also exported by ole32), DispGetParam, VarBstrCat/Cmp. Headless: OleCreatePropertyFrame(Indirect) is closed as if with Cancel, OleTranslateColor reads system colours from user32, and there are no type libraries (LoadTypeLib gives TYPE_E_CANTLOADLIBRARY), OLE pictures or fonts (E_NOTIMPL). Every export has Windows' ordinal, from Wine's oleaut32.spec, since MSVC's oleaut32.lib imports by ordinal; `pe_oleaut.dll` imports it only by ordinal. Not done: VarParseNumFromStr and the rest of the numeric parser's API, VarFormat*, variant arithmetic (VarAdd and so on), decimal arithmetic, IDispatch helpers (DispInvoke, CreateStdDispatch), type libraries and marshalling.
- **comctl32:** done (`comctl32.c`, `comctl_dpa.c`, `comctl_imagelist.c`, `comctl_sheet.c`), after Wine's. InitCommonControls(Ex) registers the control classes (so do the Create* helpers). Nothing is drawn, so a control is its state: trackbars (range, position, selection, page and line sizes, auto and manual tics, buddies, geometry), progress bars (ranges, SETPOS/DELTAPOS clamping, STEPIT wrapping round, colours, state), up-down controls (Windows' backwards default range, 16- and 32-bit ranges and positions, accelerators, base 10 or 16, UDS_SETBUDDYINT writing "1,234" or "0x04D2" to the buddy and reading it back) and status bars (parts, text per part with its SBT_* type, simple mode, rectangles, placing themselves along the parent's bottom); the other classes are plain windows. SetWindowSubclass/DefSubclassProc/RemoveWindowSubclass/GetWindowSubclass keep the window's charset and give it its own procedure back when the last one goes (and drop them after WM_NCDESTROY). DPA and DSA with the public count and pointer fields commctrl.h's macros read, a stable merge sort and sorted search with insertion points; Alloc/ReAlloc/Free/GetSize and Str_GetPtr/Str_SetPtr. Image lists keep their images in one row of a 32-bit DIB section with a monochrome mask beside it (Add, AddMasked, Replace, Remove, Copy with ILCF_SWAP, Duplicate, Merge, overlays, Draw/DrawEx/DrawIndirect transparent, on a background colour or as the mask); icons are blank and transparent, since user32's icons have nothing behind them, and there is nothing to drag. Property sheet pages are copies of the caller's PROPSHEETPAGE (all dwSize bytes) with their PSPCB_ADDREF/PSPCB_RELEASE callbacks and pcRefParent count; PropertySheet destroys them and answers as if Cancel were pressed. TaskDialog(Indirect) prints its text and answers with the default button, radio button and verification state. Every export has Windows' ordinal (TaskDialog and LoadIconMetric at comctl32 6's); `pe_comctl.dll` imports it only by ordinal. Not done: the list view, tree view, toolbar, tooltip, tab, header and other controls' messages, MRU lists, DPA_Merge and the stream functions, flat scroll bars, image list streams.
- **comdlg32:** done (`comdlg32.c`). GetOpenFileName/GetSaveFileName answer with `$VFW_OPEN_FILE`/`$VFW_SAVE_FILE` (Windows paths, relative ones in lpstrInitialDir, or absolute Unix ones on Z:), checked against OFN_FILEMUSTEXIST and OFN_PATHMUSTEXIST, with lpstrDefExt added to a name without an extension, OFN_EXTENSIONDIFFERENT, nFileOffset/nFileExtension in the structure's own charset, lpstrFileTitle, the double NUL of a multiple selection, FNERR_BUFFERTOOSMALL with the size needed in the first WORD, and the current directory following unless OFN_NOCHANGEDIR; unset or "cancel", they are cancelled. GetFileTitle, CommDlgExtendedError (per thread), ChooseColor and ChooseFont answering OK on their initial colour or font (8-point MS Sans Serif without one), PrintDlg/PrintDlgEx/PageSetupDlg failing with PDERR_NODEFAULTPRN (no printers), FindText/ReplaceText failing with CDERR_DIALOGFAILURE after checking their arguments. The structures are byte-packed as in 32-bit commdlg.h. Hooks and templates are never used.
- **msvfw32 re-export:** some codecs call back into ICM, so they are given the harness's own `vfw/` API.

The stub list is based on what VfW codecs use in general (with Wine and MPlayer's loader as the behavioural reference), not on any single DLL. The trap thunks plus `info` show what is left to add.

### VfW layer (`src/vfw/`)

Status: done. `driver.c`, `registry.c` (`vfw.ini`) and `icm.c`, the winmm
and msvfw32 built-ins, `probe` and `list`, and `testcodec.dll` with
`test_vfw`, ICSeqCompressFrameStart/Frame/End and ICCompressorFree
(`seq.c`), which `encode` uses, and ICImageCompress/ICImageDecompress
(`image.c`: one image to a packed DIB in a GMEM_MOVEABLE block, with the
codec found by format when none is given; ICImageDecompress after Wine,
ICImageCompress, which Wine only stubs, as documented, passing the size
asked for to the codec as its target), and ICCompressorChoose
(`choose.c`: no dialog; it answers as OK on the initial selection would,
the compressor the COMPVARS already names if the dialog would list it,
else Full Frames, unless `$VFW_COMPRESSOR` names one, "uncompressed" or
"cancel"; the list and what is filled in follow Wine's dialog), and
DrawDib* with ICDrawBegin/ICDraw (`drawdib.c`, after Wine: a compressed
DIB goes through a decompressor found for it, into a DIB section in a
memory DC, and StretchBlt puts it on the target DC; drawing to a window
is discarded as gdi32 discards it, while drawing to a memory DC is real;
testcodec draws its own frames through it, which tests the ICM_DRAW path
from Win32 code).

- **`driver.c`:**
  - Installable-driver lifecycle: `DRV_LOAD`, `DRV_ENABLE`, `DRV_OPEN` with ICOPEN, then `DRV_CLOSE`, `DRV_DISABLE`, `DRV_FREE`.
  - A driver table maps `fccType.fccHandler` to a DLL path, taken from `vfw.ini` (a `[drivers32]`-style file, the Linux equivalent of `system.ini`) or from the command line.
  - Both entry styles are supported: `DriverProc` export only, and codecs that also export ICM-specific helpers.
- **`icm.c`:** the msvfw32 API.
  - Opening and dispatch: ICOpen, ICOpenFunction, ICLocate, ICInfo (enumeration), ICGetInfo, ICSendMessage, ICClose.
  - Decompress: ICDecompressQuery/GetFormat/GetPalette/Begin/Decompress/End, and ICDecompressEx*.
  - Compress: ICCompressQuery/GetFormat/GetSize/Begin/Compress/End, plus ICSeqCompressFrame* (keyframe rate, quality, data rate).
  - Other messages: ICGetState/SetState, ICGetDefaultQuality/KeyFrameRate, ICDrawBegin/ICDraw (packed into ICDRAWBEGIN/ICDRAW for codecs that draw themselves), and ICConfigure/ICAbout (report "no GUI").
  - Output-format negotiation: first try the codec's own `GET_FORMAT`, then a preference list (I420, YV12, YUY2, BGR32, BGR24, RGB565, RGB555, 8-bit palettised) checked with `DECOMPRESS_QUERY`.

### Container and output (`src/avi/`, `src/out/`)

Status: done. The AVI reader and writer, the DIB conversion both ways
(`image_pack` gives a codec the input format it takes), the Y4M/BMP/raw
writers, and the frame sources for `encode` in `src/in/` (Y4M, uncompressed
AVI, BMP files, raw frames), with the `avi`, `convert`, `decode` and `encode`
commands; the `validate_media` test checks them against FFmpeg.

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
- `decode <dll|fcc|auto> <in.avi> -o out.{y4m,bmp%05d,yuv,bgr,raw} [--output-format F --start N --frames N --state file --keep-going]` (done)
- `encode <dll|fcc> <in.y4m|avi|bmp…|raw> -o out.avi [--quality Q --keyint N --bitrate B --rate R --codec-input F --state file --save-state file --odml]` (done)

## Phases and milestones

All milestones are checked against **test fixtures built in the repo**, never against the uploaded DLL.

0. **Skeleton.**
   - Build system (i386 build and tests; a Makefile at first, now CMake with CTest), `include/win32.h` (types, structs, DRV_*/ICM_* constants modelled on Wine headers).
   - README with prerequisites and usage.
   - Remove the uploaded third-party DLL.
   - The test suite runs unit tests, an ABI cross-check of `include/win32` against the mingw-w64 headers, and a CLI smoke test.
1. **PE loader and `info`.** Fixtures are mingw-built DLLs under `tests/fixtures/`:
   - `pe_basic.dll`: exports by name and by ordinal, plus a forwarder.
   - `pe_reloc.dll`: loaded with `VFW_FORCE_RELOC=1`.
   - `pe_tls.dll`: TLS callbacks.
   - `pe_dep.dll`: imports another fixture DLL.

   Milestone: all of these load, and exported functions return the expected values.
2. **Win32 runtime.** TEB/`%fs`, threads, SEH, the stub registry and trap thunks, and the stub modules.

   Milestone: the fixtures exercise these checks:
   - The SEH fixture (`pe_seh.dll`, done) recovers from an access violation, walks a chain of several hand-installed frames with an RtlUnwind in between, and reaches a frame through RaiseException.
   - The thread fixture (`pe_k32.dll`, done) reads per-thread TLS back correctly.
   - The CRT fixture (`pe_crt.dll`, done) prints through `_iob`, round-trips a file through `fopen`/`fwrite`/`fread`, and uses `_pctype`, math, string and the 64-bit arithmetic helpers.
   - The registry fixture (`pe_reg.dll`, done) round-trips values through the registry file.
   - The COM fixture (`pe_com.dll`, done) is created through CoCreateInstance from its InprocServer32 key and reads its own version resource through version.dll.
3. **VfW layer and `probe`.** `tests/fixtures/testcodec.dll` is a mingw-built lossless ICM codec (FourCC `TST0`). It can compress and decompress, keeps its state through Get/SetState, and supports several output formats.

   Milestone: `probe` lists the correct ICINFO, accepted formats and state size.
4. **AVI and Y4M I/O, `decode` and `encode`.** (Done.) Milestone: encoding a Y4M clip to AVI with `testcodec` and decoding it back gives a byte-exact result. The AVI output is validated with `ffprobe` when it is installed. The OpenDML path is tested with a large synthetic file.
5. **Hardening and CI.** (Done.)
   - Keep DLL-visible memory below 2 GB (done). On a 64-bit kernel a 32-bit process gets addresses up to 4 GB, and glibc's heap and `mmap` live near `0xf7000000`; Windows code without `LARGE_ADDRESS_AWARE` may treat pointers as signed. `src/runtime/lowmem.c` reserves the free space from 2 GB up at startup, sparing the main stack's growth, and each command runs on a worker thread with a low stack. `test_lowmem` checks each kind of allocation, and `testcodec`'s strict mode rejects any pointer at or above 2 GB in a smoke-test round trip.
   - Crash and hang reports (done): a fault handler on an alternate stack names the faulting `module+offset`, the address touched and the call into the DLL in progress; `VFW_TIMEOUT` reports a call that runs too long.
   - A matrix of fixtures (done: `pe_sse.dll` with SSE2 code assuming a 16-byte stack on entry and the FPU/MXCSR defaults on new threads; `pe_seh.dll`, see above). C++ exceptions thrown and caught inside a codec are covered by `pe_cxxeh.dll` (see the CRT task).
   - Fuzz-style tests (done): `test_avi` truncates and corrupts AVIs; `test_pe_parse` does the same to the fixture DLLs and maps and relocates some of the results; `test_source` does it to Y4M, BMP and AVI input.
   - A GitHub Actions workflow installs `gcc-multilib` and `mingw-w64` and runs the whole test suite (`ctest --preset ci`, done).
   - An optional samples run (done): `tests/samples.sh` runs `info` and `probe` over any user-supplied codec DLLs and reports unimplemented imports, without asserting anything specific to a codec; `-DVFW_SAMPLES_DIR` adds it to CTest.

## Verification

- `apt install gcc-multilib cmake ninja-build mingw-w64`, then `cmake --preset default`, `cmake --build --preset default` and `ctest --preset default`. This runs the unit tests (reloc math, AVI parser, colour conversion) and the fixture-DLL integration tests from Phases 1–4.
- `VFW_TRACE=1 ./seance probe tests/fixtures/testcodec.dll` shows the full DRV/ICM sequence with no trap hits.
- Round trip: `./seance encode testcodec.dll in.y4m -o t.avi && ./seance decode testcodec.dll t.avi -o out.y4m && cmp in.y4m out.y4m`.
- Optional manual check against any real codec: `./seance info <any.dll>` to see import coverage, then `probe` and `decode` on a matching AVI.
