# SPDX-License-Identifier: GPL-2.0-only
# Toolchain for the 64-bit test fixture DLLs: x86-64 Windows with mingw-w64.
#
# The prefix can be changed with -DVFW_MINGW_PREFIX=... (default
# x86_64-w64-mingw32-).
set(CMAKE_SYSTEM_NAME Windows)
set(CMAKE_SYSTEM_PROCESSOR x86_64)

if(NOT VFW_MINGW_PREFIX)
	set(VFW_MINGW_PREFIX x86_64-w64-mingw32-)
endif()

set(CMAKE_C_COMPILER ${VFW_MINGW_PREFIX}gcc)
set(CMAKE_RC_COMPILER ${VFW_MINGW_PREFIX}windres)

set(CMAKE_FIND_ROOT_PATH_MODE_PROGRAM NEVER)
set(CMAKE_FIND_ROOT_PATH_MODE_LIBRARY ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_INCLUDE ONLY)
