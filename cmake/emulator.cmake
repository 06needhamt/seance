# SPDX-License-Identifier: GPL-2.0-only
# Running what was built: directly, or through an emulator on a cross
# build (qemu-i386, unless the toolchain file names one). With neither an
# emulator nor binfmt_misc to run i386 programs, nothing can be tested.
# Detect independently of i386.cmake, which returns early for toolchain files.
if(VFW_HOST_ARCH)
	set(vfw_run_arch "${VFW_HOST_ARCH}")
else()
	cmake_host_system_information(RESULT vfw_run_arch QUERY OS_PLATFORM)
endif()
string(TOLOWER "${vfw_run_arch}" vfw_run_arch)
set(VFW_QEMU_LD_PREFIX "" CACHE PATH
	"QEMU i386 runtime root (empty: detect from sysroot or installed runtime)")
set(vfw_can_run TRUE)
if((CMAKE_CROSSCOMPILING OR
		NOT vfw_run_arch MATCHES "^(x86_64|amd64|x64|i[3-6]86|x86)$")
		AND NOT CMAKE_CROSSCOMPILING_EMULATOR)
	find_program(VFW_QEMU_I386 NAMES qemu-i386-static qemu-i386
		NO_CMAKE_FIND_ROOT_PATH)
	if(VFW_QEMU_I386)
		set(vfw_qemu_command "${VFW_QEMU_I386}")
		set(vfw_qemu_prefix "${VFW_QEMU_LD_PREFIX}")
		# Respect a runtime prefix already supplied in the environment.
		if(NOT vfw_qemu_prefix AND "$ENV{QEMU_LD_PREFIX}" STREQUAL "")
			foreach(prefix "${CMAKE_SYSROOT}" /usr/i686-linux-gnu
					/usr/i386-linux-gnu)
				if(prefix AND EXISTS "${prefix}/lib/ld-linux.so.2")
					set(vfw_qemu_prefix "${prefix}")
					break()
				endif()
			endforeach()
		endif()
		if(vfw_qemu_prefix)
			list(APPEND vfw_qemu_command -L "${vfw_qemu_prefix}")
		endif()
		set(CMAKE_CROSSCOMPILING_EMULATOR "${vfw_qemu_command}")
	elseif(EXISTS /proc/sys/fs/binfmt_misc/qemu-i386)
		message(STATUS "i386 programs run through binfmt_misc")
	else()
		set(vfw_can_run FALSE)
		message(WARNING "Cross-compiling with no way to run i386 programs: "
			"install qemu-user (qemu-i386) to build and run the tests.")
	endif()
endif()
if(CMAKE_CROSSCOMPILING_EMULATOR)
	message(STATUS "i386 programs run through ${CMAKE_CROSSCOMPILING_EMULATOR}")
endif()
