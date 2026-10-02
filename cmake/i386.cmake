# SPDX-License-Identifier: GPL-2.0-only
# Picks the C compiler for the harness, which is always an i386 program
# (it loads 32-bit Windows DLLs into its own address space). Included by
# CMakeLists.txt before project(), so the compiler checks use the choice.
#
# With a toolchain file, that decides and nothing is done here. Otherwise:
#
#   - A compiler given with -DCMAKE_C_COMPILER or $CC is used with -m32.
#   - On an x86 host (x86_64 or i386), the default compiler with -m32, if
#     it can link a 32-bit program (gcc-multilib or the like).
#   - Failing that, or on any other host (aarch64, ...), an i686 Linux
#     cross compiler: i686-linux-gnu-gcc and its other usual names, then
#     clang --target=i686-linux-gnu.
#
# On a host that isn't x86 the build is a cross build; CMakeLists.txt then
# runs the tests through qemu-i386 when there is one.
#
# VFW_HOST_ARCH overrides the detected host architecture (as uname -m
# spells it). The choice is kept in the cache: delete VFW_I386_MODE, or
# the build directory, to choose again.

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
if(vfw_arch MATCHES "^(x86_64|amd64|x64|i[3-6]86|x86)$")
	set(vfw_host_x86 TRUE)
else()
	set(vfw_host_x86 FALSE)
endif()

# Whether `cc args...` compiles and links an i386 program with the system
# headers the harness uses (the kernel's asm/ ones are per architecture, so
# a compiler without an i386 sysroot fails here, not later), libm and
# pthreads.
function(vfw_links_i386 out cc)
	set(dir ${CMAKE_BINARY_DIR}/CMakeFiles/vfw-i386-probe)
	file(MAKE_DIRECTORY ${dir})
	file(WRITE ${dir}/probe.c [=[
#ifndef __i386__
#error not i386
#endif
#include <asm/ldt.h>
#include <errno.h>
#include <fenv.h>
#include <math.h>
#include <pthread.h>
#include <setjmp.h>
#include <signal.h>
#include <sys/mman.h>
#include <sys/syscall.h>
#include <sys/ucontext.h>
int main(void)
{
	struct user_desc d = { 0 };
	ucontext_t uc;
	(void)uc;
	return (int)sqrt((double)(pthread_self() != 0)) + d.entry_number +
	       (SYS_modify_ldt > 0 ? 0 : errno) + (fegetround() < 0);
}
]=])
	execute_process(
		COMMAND ${cc} ${ARGN} ${dir}/probe.c -o ${dir}/probe -lm -pthread
		RESULT_VARIABLE r OUTPUT_QUIET ERROR_QUIET)
	if(r EQUAL 0)
		set(${out} TRUE PARENT_SCOPE)
	else()
		set(${out} FALSE PARENT_SCOPE)
	endif()
endfunction()

if(NOT DEFINED CACHE{VFW_I386_MODE})
	set(mode "")
	set(cc "")
	if(CMAKE_C_COMPILER OR DEFINED ENV{CC})
		set(mode given)
	endif()

	if(NOT mode AND vfw_host_x86)
		unset(vfw_native_cc CACHE)
		find_program(vfw_native_cc NAMES cc gcc clang)
		if(vfw_native_cc)
			vfw_links_i386(ok ${vfw_native_cc} -m32)
			if(ok)
				set(mode native)
			endif()
		endif()
		unset(vfw_native_cc CACHE)
	endif()

	if(NOT mode)
		foreach(name i686-linux-gnu-gcc i686-pc-linux-gnu-gcc
				i686-unknown-linux-gnu-gcc i686-linux-gcc
				i586-linux-gnu-gcc i386-linux-gnu-gcc)
			unset(vfw_cross_cc CACHE)
			find_program(vfw_cross_cc ${name})
			if(vfw_cross_cc)
				vfw_links_i386(ok ${vfw_cross_cc})
				if(ok)
					set(mode cross-gcc)
					set(cc ${vfw_cross_cc})
					break()
				endif()
			endif()
		endforeach()
		unset(vfw_cross_cc CACHE)
	endif()

	if(NOT mode)
		unset(vfw_clang CACHE)
		find_program(vfw_clang clang)
		if(vfw_clang)
			vfw_links_i386(ok ${vfw_clang} --target=i686-linux-gnu)
			if(ok)
				set(mode clang)
				set(cc ${vfw_clang})
			endif()
		endif()
		unset(vfw_clang CACHE)
	endif()

	if(NOT mode)
		if(vfw_host_x86)
			set(hint "gcc-multilib (Debian/Ubuntu), or an i686 cross "
				"compiler such as gcc-i686-linux-gnu")
		else()
			set(hint "an i686 Linux cross compiler (Debian/Ubuntu: "
				"gcc-i686-linux-gnu) or clang with an i386 sysroot, "
				"with qemu-user (qemu-i386) to run the tests")
		endif()
		string(CONCAT hint ${hint})
		message(FATAL_ERROR "No C compiler here builds i386 programs, "
			"which libseance must be (host: ${vfw_arch}). Install "
			"${hint}, or give one with -DCMAKE_C_COMPILER=... or a "
			"toolchain file.")
	endif()

	set(VFW_I386_MODE ${mode} CACHE INTERNAL "How the i386 compiler was found")
	set(VFW_I386_CC "${cc}" CACHE INTERNAL "The i386 cross compiler found")
endif()

if(VFW_I386_MODE STREQUAL "cross-gcc")
	set(CMAKE_C_COMPILER ${VFW_I386_CC} CACHE FILEPATH "C compiler")
elseif(VFW_I386_MODE STREQUAL "clang")
	set(CMAKE_C_COMPILER ${VFW_I386_CC} CACHE FILEPATH "C compiler")
	set(CMAKE_C_COMPILER_TARGET i686-linux-gnu CACHE STRING
		"C compiler target")
else()
	# The default or given compiler, told to build for i386.
	set(CMAKE_C_FLAGS_INIT "-m32")
	set(CMAKE_EXE_LINKER_FLAGS_INIT "-m32")
endif()

if(NOT vfw_host_x86)
	set(CMAKE_SYSTEM_NAME Linux)
	set(CMAKE_SYSTEM_PROCESSOR i686)
endif()

if(VFW_I386_CC)
	message(STATUS "i386 compiler for host ${vfw_arch}: ${VFW_I386_CC} "
		"(${VFW_I386_MODE})")
else()
	message(STATUS "i386 compiler for host ${vfw_arch}: -m32 (${VFW_I386_MODE})")
endif()
