# The test fixture DLLs, cross-compiled with mingw-w64. The top-level build
# configures this directory as an external project with
# cmake/mingw-i686.cmake as its toolchain; the DLLs go to VFW_FIXTURE_DIR.
#
# They are built without a C runtime, so each imports only what it names:
# nothing, each other, or system DLLs such as kernel32, user32, gdi32,
# advapi32, version, ole32, oleaut32, shell32, comctl32, comdlg32, winmm and
# msvfw32 (whose mingw import libraries the harness's built-in DLLs stand in
# for).
cmake_minimum_required(VERSION 3.20)
project(libseance-fixtures C RC)

if(NOT VFW_FIXTURE_DIR)
	set(VFW_FIXTURE_DIR ${CMAKE_BINARY_DIR}/dlls)
endif()

# No default Win32 libraries: an import appears only where a DLL asks.
set(CMAKE_C_STANDARD_LIBRARIES "" CACHE STRING "" FORCE)

find_program(VFW_DLLTOOL NAMES ${VFW_MINGW_PREFIX}dlltool REQUIRED)
find_program(VFW_OBJCOPY NAMES ${VFW_MINGW_PREFIX}objcopy REQUIRED)

set(common_options
	-O2 -Wall -nostdlib -ffreestanding -fno-stack-protector
	-mno-stack-arg-probe -fno-asynchronous-unwind-tables)

# An import library made from a .def file (for DLLs that aren't built
# here, or to import by the ordinals the .def gives).
function(import_library name)
	set(lib ${CMAKE_BINARY_DIR}/lib${name}.a)
	add_custom_command(OUTPUT ${lib}
		COMMAND ${VFW_DLLTOOL} -k -d ${CMAKE_CURRENT_SOURCE_DIR}/${name}.def -l ${lib}
		DEPENDS ${name}.def
		VERBATIM)
	add_custom_target(${name}_implib DEPENDS ${lib})
	add_library(${name}_imports STATIC IMPORTED GLOBAL)
	set_target_properties(${name}_imports PROPERTIES IMPORTED_LOCATION ${lib})
	add_dependencies(${name}_imports ${name}_implib)
endfunction()

# fixture(<name> <sources...> [LIBS <libs...>] [OPTIONS <options...>])
function(fixture name)
	cmake_parse_arguments(F "" "" "LIBS;OPTIONS" ${ARGN})
	add_library(${name} SHARED ${F_UNPARSED_ARGUMENTS})
	target_compile_options(${name} PRIVATE ${common_options} ${F_OPTIONS})
	target_link_options(${name} PRIVATE ${common_options} ${F_OPTIONS}
		-Wl,--image-base,0x10000000 -Wl,-e,_DllMain@12)
	target_link_libraries(${name} PRIVATE ${F_LIBS})
	set_target_properties(${name} PROPERTIES
		PREFIX "" SUFFIX ".dll"
		RUNTIME_OUTPUT_DIRECTORY ${VFW_FIXTURE_DIR}
		ARCHIVE_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/implib)
endfunction()

import_library(pe_basic)
import_library(zz_nosuch)
import_library(vfw_missing)
import_library(sehcrt)
import_library(cxxcrt)
import_library(oleaut_ord)
import_library(comctl_ord)

fixture(pe_basic pe_basic.c pe_basic.def pe_basic.rc)
fixture(pe_reloc pe_reloc.c)
fixture(pe_tls pe_tls.c)
fixture(pe_fail pe_fail.c)
fixture(pe_dep pe_dep.c pe_dep.def LIBS pe_basic_imports)
fixture(pe_missing pe_missing.c LIBS zz_nosuch_imports pe_basic_imports)
# Its own delay-load tables (GNU ld doesn't make them); see pe_delay.c.
fixture(pe_delay pe_delay.c)

# Links against kernel32 (the mingw import library) and a made-up function.
fixture(pe_runtime pe_runtime.c LIBS vfw_missing_imports kernel32)

# Calls into kernel32 with a 4-byte-aligned stack, as MSVC code does.
fixture(pe_k32 pe_k32.c pe_k32.rc LIBS kernel32
	OPTIONS -mpreferred-stack-boundary=2)

# SSE2 code that assumes a 16-byte-aligned stack on entry, as GCC-built and
# SIMD codecs do.
fixture(pe_sse pe_sse.c LIBS kernel32
	OPTIONS -msse2 -mfpmath=sse -mpreferred-stack-boundary=4
		-mincoming-stack-boundary=4 -fno-builtin)

# Installs its own fs:[0] SEH frames by hand (no CRT here to compile
# __try/__except into _except_handler3 frames) and faults under them.
fixture(pe_seh pe_seh.c LIBS kernel32 OPTIONS -mpreferred-stack-boundary=2)

# __try/__except/__finally as cl compiles them against msvcrt's
# _except_handler3 and _except_handler4_common, frames written out by hand.
fixture(pe_sehcrt pe_sehcrt.c LIBS sehcrt_imports kernel32
	OPTIONS -mpreferred-stack-boundary=2)

# C++ exceptions laid out as cl compiles them, against msvcrt's
# __CxxFrameHandler and _CxxThrowException.
fixture(pe_cxxeh pe_cxxeh.c LIBS cxxcrt_imports kernel32
	OPTIONS -mpreferred-stack-boundary=2 -fno-builtin)

# Keeps settings in the registry (advapi32), as many codecs do.
fixture(pe_reg pe_reg.c LIBS advapi32 OPTIONS -mpreferred-stack-boundary=2)

# Uses user32 and gdi32 the way a codec might: strings, wsprintf, a
# hidden window with messages and timers, a bitmap resource.
fixture(pe_gui pe_gui.c pe_gui.rc LIBS user32 gdi32 kernel32
	OPTIONS -mpreferred-stack-boundary=2 -fno-builtin)

# Uses shell32 as a codec might: its settings folder, SHFileOperation, an
# item ID list, a command line, a link nothing can open.
fixture(pe_shell pe_shell.c LIBS shell32 kernel32
	OPTIONS -mpreferred-stack-boundary=2 -fno-builtin)

# Uses oleaut32 imported by ordinal only, as MSVC-built code imports it.
fixture(pe_oleaut pe_oleaut.c LIBS oleaut_ord_imports kernel32
	OPTIONS -mpreferred-stack-boundary=2 -fno-builtin)

# Uses comctl32 imported by ordinal only, and comdlg32 by name.
fixture(pe_comctl pe_comctl.c LIBS comctl_ord_imports comdlg32 user32 kernel32
	OPTIONS -mpreferred-stack-boundary=2 -fno-builtin)

# An in-process COM server that also calls version.dll and ole32 itself.
fixture(pe_com pe_com.c pe_com.def pe_com.rc LIBS ole32 version kernel32
	OPTIONS -mpreferred-stack-boundary=2 -fno-builtin)

# Imports msvcrt's math and string functions, as a codec with a dynamic
# CRT (/MD) would.
# -Wl,--disable-auto-import: no CRT startup object is linked (-nostdlib),
# so nothing provides the runtime pseudo-relocator GNU ld's default
# auto-import of data symbols across the DLL boundary would need; none of
# msvcrt's functions used here need it anyway.
fixture(pe_crt pe_crt.c LIBS msvcrt OPTIONS -Wl,--disable-auto-import)

# A small VfW codec. -fno-builtin and no loop-to-memset rewriting keep it
# free of calls into a C runtime it doesn't link.
fixture(testcodec testcodec.c testcodec.def testcodec.rc
	LIBS kernel32 winmm msvfw32
	OPTIONS -mpreferred-stack-boundary=2 -fno-builtin
		-fno-tree-loop-distribute-patterns)

# The same image as pe_reloc.dll with its relocations stripped.
add_custom_command(OUTPUT ${VFW_FIXTURE_DIR}/pe_noreloc.dll
	COMMAND ${VFW_OBJCOPY} --remove-section .reloc
		$<TARGET_FILE:pe_reloc> ${VFW_FIXTURE_DIR}/pe_noreloc.dll
	DEPENDS pe_reloc
	VERBATIM)
add_custom_target(pe_noreloc ALL DEPENDS ${VFW_FIXTURE_DIR}/pe_noreloc.dll)
