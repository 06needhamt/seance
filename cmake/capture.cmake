# SPDX-License-Identifier: GPL-2.0-only
# Runs a command and writes its standard output to a file:
#
#   cmake -DOUTPUT=<file> -P capture.cmake -- <command> [args...]
#
# The separator keeps CMake from consuming command options such as -L.
# The file is written only if the command succeeds.
if(NOT OUTPUT)
	message(FATAL_ERROR "capture.cmake: OUTPUT is not set")
endif()

set(cmd)
set(seen_script FALSE)
math(EXPR last "${CMAKE_ARGC} - 1")
foreach(i RANGE 1 ${last})
	if(seen_script)
		list(APPEND cmd "${CMAKE_ARGV${i}}")
	elseif(CMAKE_ARGV${i} MATCHES "capture\\.cmake$")
		set(seen_script TRUE)
	endif()
endforeach()
# CMake retains the separator in CMAKE_ARGV. Accept older callers too.
if(cmd)
	list(GET cmd 0 first)
	if(first STREQUAL "--")
		list(REMOVE_AT cmd 0)
	endif()
endif()
if(NOT cmd)
	message(FATAL_ERROR "capture.cmake: no command given")
endif()

execute_process(COMMAND ${cmd} OUTPUT_VARIABLE out RESULT_VARIABLE rc)
if(NOT "${rc}" STREQUAL "0")
	message(FATAL_ERROR "capture.cmake: ${cmd} failed (${rc})")
endif()
file(WRITE "${OUTPUT}" "${out}")
