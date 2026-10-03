# SPDX-License-Identifier: GPL-2.0-only
# Run independently with cmake -P tests/cmake/capture.cmake.
get_filename_component(root "${CMAKE_CURRENT_LIST_DIR}/../.." ABSOLUTE)
set(output "${CMAKE_CURRENT_BINARY_DIR}/capture-regression.txt")
execute_process(
	COMMAND "${CMAKE_COMMAND}" "-DOUTPUT=${output}"
		-P "${root}/cmake/capture.cmake" --
		"${CMAKE_COMMAND}" -E echo -L "/runtime with spaces" -Dguest=value
	RESULT_VARIABLE rc)
if(NOT "${rc}" STREQUAL "0")
	message(FATAL_ERROR "capture invocation failed: ${rc}")
endif()
file(READ "${output}" actual)
file(REMOVE "${output}")
if(NOT actual STREQUAL "-L /runtime with spaces -Dguest=value\n")
	message(FATAL_ERROR "capture changed command arguments: [${actual}]")
endif()
