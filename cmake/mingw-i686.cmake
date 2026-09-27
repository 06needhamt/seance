# Toolchain for the test fixture DLLs: 32-bit Windows with mingw-w64.
#
# The prefix can be changed with -DVFW_MINGW_PREFIX=... (default
# i686-w64-mingw32-).
set(CMAKE_SYSTEM_NAME Windows)
set(CMAKE_SYSTEM_PROCESSOR i686)

if(NOT VFW_MINGW_PREFIX)
	set(VFW_MINGW_PREFIX i686-w64-mingw32-)
endif()

set(CMAKE_C_COMPILER ${VFW_MINGW_PREFIX}gcc)
set(CMAKE_RC_COMPILER ${VFW_MINGW_PREFIX}windres)

set(CMAKE_FIND_ROOT_PATH_MODE_PROGRAM NEVER)
set(CMAKE_FIND_ROOT_PATH_MODE_LIBRARY ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_INCLUDE ONLY)
