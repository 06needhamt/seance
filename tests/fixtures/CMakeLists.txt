# SPDX-License-Identifier: GPL-2.0-only
# The test fixture DLLs, cross-compiled with mingw-w64. The top-level build
# configures this directory as an external project with
# cmake/mingw-i686.cmake as its toolchain; the DLLs go to VFW_FIXTURE_DIR.
# It configures it a second time with cmake/mingw-x86_64.cmake for the
# 64-bit (PE32+) fixtures, of which there are fewer so far: see the end.
#
# They are built without a C runtime, so each imports only what it names:
# nothing, each other, or system DLLs such as kernel32, user32, gdi32,
# advapi32, version, ole32, oleaut32, shell32, comctl32, comdlg32, winmm,
# msvfw32, msdmo and msacm32 (whose mingw import libraries the harness's
# built-in DLLs stand in for).
cmake_minimum_required(VERSION 3.20)
project(seance-fixtures C RC)

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

if(CMAKE_SYSTEM_PROCESSOR STREQUAL "x86_64")
	set(VFW_FIXTURE_64 TRUE)
	# x64 has one calling convention, so no stdcall decoration; and an
	# image base above 4 GB, as 64-bit DLLs usually have, so pointers
	# that lose their top half show up.
	set(fixture_link_options -Wl,--image-base,0x180000000 -Wl,-e,DllMain)
else()
	set(VFW_FIXTURE_64 FALSE)
	set(fixture_link_options -Wl,--image-base,0x10000000 -Wl,-e,_DllMain@12)
endif()

# The .def file to use: as written, or for x86-64 a copy without the
# stdcall decorations (Name@12), which Win64 names don't have. Ordinals
# (Name @12, with a space) stay.
function(def_for out file)
	if(VFW_FIXTURE_64)
		set(src ${CMAKE_CURRENT_SOURCE_DIR}/${file})
		set(dst ${CMAKE_BINARY_DIR}/def64/${file})
		file(READ ${src} text)
		string(REGEX REPLACE "([A-Za-z0-9_])@[0-9]+" "\\1" text "${text}")
		file(WRITE ${dst}.tmp "${text}")
		configure_file(${dst}.tmp ${dst} COPYONLY)
		set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS ${src})
		set(${out} ${dst} PARENT_SCOPE)
	else()
		set(${out} ${CMAKE_CURRENT_SOURCE_DIR}/${file} PARENT_SCOPE)
	endif()
endfunction()

# An import library made from a .def file (for DLLs that aren't built
# here, or to import by the ordinals the .def gives).
function(import_library name)
	set(lib ${CMAKE_BINARY_DIR}/lib${name}.a)
	def_for(def ${name}.def)
	add_custom_command(OUTPUT ${lib}
		COMMAND ${VFW_DLLTOOL} -k -d ${def} -l ${lib}
		DEPENDS ${def}
		VERBATIM)
	add_custom_target(${name}_implib DEPENDS ${lib})
	add_library(${name}_imports STATIC IMPORTED GLOBAL)
	set_target_properties(${name}_imports PROPERTIES IMPORTED_LOCATION ${lib})
	add_dependencies(${name}_imports ${name}_implib)
endfunction()

# fixture(<name> <sources...> [LIBS <libs...>] [OPTIONS <options...>])
function(fixture name)
	cmake_parse_arguments(F "" "" "LIBS;OPTIONS" ${ARGN})
	if(VFW_FIXTURE_64)
		# Win64 code keeps the stack 16-aligned; there is no 4-byte ABI.
		list(REMOVE_ITEM F_OPTIONS -mpreferred-stack-boundary=2)
	endif()
	set(srcs)
	foreach(src ${F_UNPARSED_ARGUMENTS})
		if(src MATCHES "\\.def$")
			def_for(src ${src})
		endif()
		list(APPEND srcs ${src})
	endforeach()
	add_library(${name} SHARED ${srcs})
	target_compile_options(${name} PRIVATE ${common_options} ${F_OPTIONS})
	target_link_options(${name} PRIVATE ${common_options} ${F_OPTIONS}
		${fixture_link_options})
	target_link_libraries(${name} PRIVATE ${F_LIBS})
	set_target_properties(${name} PROPERTIES
		PREFIX "" SUFFIX ".dll"
		RUNTIME_OUTPUT_DIRECTORY ${VFW_FIXTURE_DIR}
		ARCHIVE_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/implib)
endfunction()

import_library(pe_basic)

# The 64-bit fixtures leave out what is i386 only: fs:[0] SEH frames
# written by hand, cl's i386 C++ exception layout and Win9x thunks.
import_library(zz_nosuch)
import_library(vfw_missing)
import_library(sehcrt)
import_library(cxxcrt)
import_library(oleaut_ord)
import_library(comctl_ord)
import_library(k32_thunk)

fixture(pe_basic pe_basic.c pe_basic.def pe_basic.rc)
fixture(pe_reloc pe_reloc.c)
fixture(pe_tls pe_tls.c)
fixture(pe_fail pe_fail.c)
fixture(pe_realcodec pe_realcodec.c)
if(NOT VFW_FIXTURE_64)
	fixture(pe_thunk pe_thunk.c LIBS k32_thunk_imports)
endif()
fixture(pe_fourccs pe_fourccs.c pe_fourccs.rc)
fixture(pe_dep pe_dep.c pe_dep.def LIBS pe_basic_imports)
fixture(pe_missing pe_missing.c LIBS zz_nosuch_imports pe_basic_imports)
# Its own delay-load tables (GNU ld doesn't make them); see pe_delay.c.
fixture(pe_delay pe_delay.c)

# Links against kernel32 (the mingw import library) and a made-up function.
fixture(pe_runtime pe_runtime.c LIBS vfw_missing_imports kernel32)

# Finds kernel32 from DllMain's return address and walks its exports, as
# packers do, and starts a cdecl routine as a thread.
fixture(pe_packer pe_packer.c LIBS kernel32
	OPTIONS -mpreferred-stack-boundary=2)

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
if(NOT VFW_FIXTURE_64)
	fixture(pe_seh pe_seh.c LIBS kernel32 OPTIONS -mpreferred-stack-boundary=2)
endif()

# x86-64 table-based SEH: hand-written functions with .seh_* unwind data and
# __C_specific_handler scope tables (msvcrt's), as cl compiles __try.
if(VFW_FIXTURE_64)
	fixture(pe_seh64 pe_seh64.c LIBS kernel32 msvcrt
		OPTIONS -Wl,--disable-auto-import)
endif()

# C++ exceptions from mingw-w64's g++ (libgcc's unwinder over the Rtl*
# functions), when it is installed: the DLL links its own libgcc/libstdc++.
if(VFW_FIXTURE_64)
	find_program(VFW_MINGW64_CXX ${VFW_MINGW_PREFIX}g++)
	if(VFW_MINGW64_CXX)
		set(CMAKE_CXX_COMPILER ${VFW_MINGW64_CXX})
		enable_language(CXX)
		add_library(pe_gxx SHARED pe_gxx.cc)
		target_compile_options(pe_gxx PRIVATE -O1 -Wall)
		target_link_options(pe_gxx PRIVATE -static-libgcc -static-libstdc++)
		set_target_properties(pe_gxx PROPERTIES PREFIX "" SUFFIX ".dll"
			RUNTIME_OUTPUT_DIRECTORY ${VFW_FIXTURE_DIR})
	endif()
endif()

# x86-64 C++ exceptions: hand-written functions with __CxxFrameHandler3
# tables as cl lays them out.
if(VFW_FIXTURE_64)
	fixture(pe_cxxeh64 pe_cxxeh64.c LIBS cxxcrt_imports kernel32
		OPTIONS -fno-builtin)
endif()

# __try/__except/__finally as cl compiles them against msvcrt's
# _except_handler3 and _except_handler4_common, frames written out by hand.
if(NOT VFW_FIXTURE_64)
	fixture(pe_sehcrt pe_sehcrt.c LIBS sehcrt_imports kernel32
		OPTIONS -mpreferred-stack-boundary=2)
endif()

# C++ exceptions laid out as cl compiles them, against msvcrt's
# __CxxFrameHandler and _CxxThrowException.
if(NOT VFW_FIXTURE_64)
	fixture(pe_cxxeh pe_cxxeh.c LIBS cxxcrt_imports kernel32
		OPTIONS -mpreferred-stack-boundary=2 -fno-builtin)
endif()

# Converts audio through msacm32, as an application would.
fixture(pe_acm pe_acm.c LIBS msacm32 OPTIONS -mpreferred-stack-boundary=2)

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
	LIBS kernel32 winmm msvfw32 advapi32
	OPTIONS -mpreferred-stack-boundary=2 -fno-builtin
		-fno-tree-loop-distribute-patterns)

# An ACM driver (.acm) for TSTA, the fixtures' audio codec.
fixture(testacm testacm.c testacm.def
	LIBS kernel32 winmm
	OPTIONS -mpreferred-stack-boundary=2 -fno-builtin
		-fno-tree-loop-distribute-patterns)
set_target_properties(testacm PROPERTIES SUFFIX ".acm")

# A DirectShow filter server (.ax), registering itself through quartz's
# filter mappers.
fixture(testfilter testfilter.c testfilter_filter.c testfilter.def
	LIBS ole32 advapi32 kernel32
	OPTIONS -mpreferred-stack-boundary=2 -fno-builtin
		-fno-tree-loop-distribute-patterns)
set_target_properties(testfilter PROPERTIES SUFFIX ".ax")
fixture(testdmo testdmo.c testdmo_dmo.c testdmo_audio.c testdmo.def
	LIBS msdmo ole32 advapi32 kernel32
	OPTIONS -mpreferred-stack-boundary=2 -fno-builtin
		-fno-tree-loop-distribute-patterns)

# The same image as pe_reloc.dll with its relocations stripped.
add_custom_command(OUTPUT ${VFW_FIXTURE_DIR}/pe_noreloc.dll
	COMMAND ${VFW_OBJCOPY} --remove-section .reloc
		$<TARGET_FILE:pe_reloc> ${VFW_FIXTURE_DIR}/pe_noreloc.dll
	DEPENDS pe_reloc
	VERBATIM)
add_custom_target(pe_noreloc ALL DEPENDS ${VFW_FIXTURE_DIR}/pe_noreloc.dll)
