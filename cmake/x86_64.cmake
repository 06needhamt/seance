# SPDX-License-Identifier: GPL-2.0-only
# Picks the C compiler for the x86-64 harness, seance64, which loads 64-bit
# Windows DLLs (VFW_ARCH=x86_64). The counterpart of i386.cmake; included by
# CMakeLists.txt before project().
#
# With a toolchain file, that decides. Otherwise a compiler given with
# -DCMAKE_C_COMPILER or $CC, or the default one on an x86-64 host, is used
# as it is; on any other host an x86_64 Linux cross compiler
# (x86_64-linux-gnu-gcc, then clang --target=x86_64-linux-gnu), whose
# tests then run through qemu-x86_64.

if(CMAKE_TOOLCHAIN_FILE)
	return()
endif()

set(VFW_HOST_ARCH "" CACHE STRING
	"Host architecture, as uname -m spells it (empty: detect)")
if(VFW_HOST_ARCH)
	set(vfw_arch "${VFW_HOST_ARCH}")
else()
	cmake_host_system_information(RESULT vfw_arch QUERY OS_PLATFORM)
endif()
string(TOLOWER "${vfw_arch}" vfw_arch)
if(vfw_arch MATCHES "^(x86_64|amd64|x64)$")
	set(vfw_host_x86 TRUE)
else()
	set(vfw_host_x86 FALSE)
endif()

if(NOT vfw_host_x86 AND NOT CMAKE_C_COMPILER AND NOT DEFINED ENV{CC})
	find_program(vfw_cross_cc x86_64-linux-gnu-gcc)
	if(vfw_cross_cc)
		set(CMAKE_C_COMPILER ${vfw_cross_cc} CACHE FILEPATH "C compiler")
	else()
		find_program(vfw_clang clang)
		if(NOT vfw_clang)
			message(FATAL_ERROR "No x86-64 Linux compiler found for "
				"seance64 (host: ${vfw_arch}). Install "
				"gcc-x86-64-linux-gnu, or give one with "
				"-DCMAKE_C_COMPILER=...")
		endif()
		set(CMAKE_C_COMPILER ${vfw_clang} CACHE FILEPATH "C compiler")
		set(CMAKE_C_COMPILER_TARGET x86_64-linux-gnu CACHE STRING
			"C compiler target")
	endif()
	set(CMAKE_SYSTEM_NAME Linux)
	set(CMAKE_SYSTEM_PROCESSOR x86_64)
endif()
