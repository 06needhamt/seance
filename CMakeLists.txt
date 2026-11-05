# SPDX-License-Identifier: GPL-2.0-only
# seance build.
#
#   cmake -B build            configure (or: cmake --preset default)
#   cmake --build build       build build/seance (i386) and, with
#                             mingw-w64, the fixture DLLs
#   ctest --test-dir build    unit tests, ABI cross-check, CLI smoke test
#                             and the FFmpeg media checks
#
# Options:
#   VFW_REQUIRE_MINGW   fail instead of skipping what needs mingw-w64
#                       (the ABI check and the fixture DLL tests)
#   VFW_REQUIRE_FFMPEG  fail instead of skipping the FFmpeg checks
#   VFW_SANITIZE        build with AddressSanitizer and UBSan
#   VFW_MINGW_PREFIX    the mingw-w64 tool prefix (i686-w64-mingw32-)
#   VFW_REQUIRE_MINGW64 fail instead of skipping the 64-bit fixture DLLs
#   VFW_MINGW64_PREFIX  the 64-bit mingw-w64 prefix (x86_64-w64-mingw32-)
#   VFW_SAMPLES_DIR     codec DLLs for the samples test (info and probe)
#   VFW_HOST_ARCH       the host architecture, if detecting it goes wrong
#
# The compiler: see cmake/i386.cmake. On an x86 host it is the default one
# with -m32, else (or without 32-bit support) an i686 cross compiler; a
# cross build's tests run through qemu-i386 when there is one.
#
# VFW_ARCH=x86_64 builds seance64 instead, which runs 64-bit (PE32+) DLLs
# (cmake/x86_64.cmake; docs/X64_PLAN.md). Use a build directory of its own.
cmake_minimum_required(VERSION 3.20)

# The harness loads Windows DLLs into its own address space, so it is an
# i386 program for 32-bit DLLs and an x86-64 one for 64-bit DLLs. Chosen
# before project() so the compiler checks use it too.
set(VFW_ARCH "i386" CACHE STRING
	"What the harness runs: i386 (seance) or x86_64 (seance64) DLLs")
set_property(CACHE VFW_ARCH PROPERTY STRINGS i386 x86_64)
if(VFW_ARCH STREQUAL "x86_64")
	include(cmake/x86_64.cmake)
elseif(VFW_ARCH STREQUAL "i386")
	include(cmake/i386.cmake)
else()
	message(FATAL_ERROR "VFW_ARCH must be i386 or x86_64, not ${VFW_ARCH}")
endif()

project(seance VERSION 0.1.0 LANGUAGES C)

if(VFW_ARCH STREQUAL "x86_64")
	set(vfw_pointer_size 8)
	set(vfw_harness seance64)
else()
	set(vfw_pointer_size 4)
	set(vfw_harness seance)
endif()
if(NOT CMAKE_SIZEOF_VOID_P EQUAL vfw_pointer_size)
	message(FATAL_ERROR "VFW_ARCH=${VFW_ARCH} needs a compiler that gives "
		"${vfw_pointer_size}-byte pointers; this one gives "
		"${CMAKE_SIZEOF_VOID_P}. For i386, install gcc-multilib or an "
		"i686 cross compiler, or give a toolchain file.")
endif()

include(cmake/emulator.cmake)
# For the test scripts, which run the harness themselves.
string(REPLACE ";" " " vfw_emulator "${CMAKE_CROSSCOMPILING_EMULATOR}")

option(VFW_REQUIRE_MINGW "Fail if mingw-w64 is missing" OFF)
option(VFW_REQUIRE_FFMPEG "Fail the media checks if FFmpeg is missing" OFF)
option(VFW_REQUIRE_MINGW64 "Fail if the 64-bit mingw-w64 is missing" OFF)
option(VFW_SANITIZE "Build with AddressSanitizer and UBSan" OFF)
set(VFW_MINGW_PREFIX "i686-w64-mingw32-" CACHE STRING
	"Prefix of the mingw-w64 cross tools")
set(VFW_MINGW64_PREFIX "x86_64-w64-mingw32-" CACHE STRING
	"Prefix of the 64-bit mingw-w64 cross tools")
set(VFW_SAMPLES_DIR "" CACHE PATH
	"A directory of codec DLLs to run info and probe over (the samples test)")

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
	set(CMAKE_BUILD_TYPE RelWithDebInfo CACHE STRING "Build type" FORCE)
endif()
# Keep assert() in optimised builds.
set(CMAKE_C_FLAGS_RELWITHDEBINFO "-O2 -g")
set(CMAKE_C_FLAGS_RELEASE "-O2")

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_C_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

add_compile_options(-Wall -Wextra -Wno-unused-parameter)
add_compile_definitions(_GNU_SOURCE _FILE_OFFSET_BITS=64)

if(VFW_SANITIZE)
	set(sanitize -fsanitize=address,undefined -fno-sanitize-recover=undefined)
	add_compile_options(-O1 -fno-omit-frame-pointer ${sanitize})
	add_link_options(${sanitize})
endif()

# The harness.

file(GLOB_RECURSE core_sources CONFIGURE_DEPENDS src/*.c)
list(REMOVE_ITEM core_sources ${PROJECT_SOURCE_DIR}/src/main.c)

add_library(vfw_core STATIC ${core_sources})
target_include_directories(vfw_core PUBLIC include src)
target_link_libraries(vfw_core PUBLIC m)

add_executable(seance src/main.c)
target_link_libraries(seance PRIVATE vfw_core)
target_compile_definitions(seance PRIVATE
	SEANCE_VERSION="${PROJECT_VERSION}-dev")
set_target_properties(seance PROPERTIES
	OUTPUT_NAME ${vfw_harness}
	RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR})

install(TARGETS seance RUNTIME DESTINATION bin)

# The fixture DLLs, cross-compiled as a separate project.

set(VFW_FIXTURE_DIR ${CMAKE_BINARY_DIR}/fixtures)
find_program(VFW_MINGW_CC ${VFW_MINGW_PREFIX}gcc)
if(VFW_MINGW_CC)
	include(ExternalProject)
	ExternalProject_Add(fixtures
		SOURCE_DIR ${PROJECT_SOURCE_DIR}/tests/fixtures
		BINARY_DIR ${CMAKE_BINARY_DIR}/fixtures-build
		CMAKE_ARGS
			-DCMAKE_TOOLCHAIN_FILE=${PROJECT_SOURCE_DIR}/cmake/mingw-i686.cmake
			-DVFW_MINGW_PREFIX=${VFW_MINGW_PREFIX}
			-DVFW_FIXTURE_DIR=${VFW_FIXTURE_DIR}
		INSTALL_COMMAND ""
		BUILD_ALWAYS ON
		# Ninja: their output as it comes, not held back to the end.
		USES_TERMINAL_CONFIGURE ON
		USES_TERMINAL_BUILD ON)
elseif(VFW_REQUIRE_MINGW)
	message(FATAL_ERROR "${VFW_MINGW_PREFIX}gcc not found (install mingw-w64)")
else()
	message(STATUS "${VFW_MINGW_PREFIX}gcc not found: the fixture DLL "
		"tests and the ABI check will be skipped")
endif()

# The 64-bit (x86-64, PE32+) fixture DLLs, from the same sources.
set(VFW_FIXTURE64_DIR ${CMAKE_BINARY_DIR}/fixtures64)
find_program(VFW_MINGW64_CC ${VFW_MINGW64_PREFIX}gcc)
if(VFW_MINGW64_CC)
	include(ExternalProject)
	ExternalProject_Add(fixtures64
		SOURCE_DIR ${PROJECT_SOURCE_DIR}/tests/fixtures
		BINARY_DIR ${CMAKE_BINARY_DIR}/fixtures64-build
		CMAKE_ARGS
			-DCMAKE_TOOLCHAIN_FILE=${PROJECT_SOURCE_DIR}/cmake/mingw-x86_64.cmake
			-DVFW_MINGW_PREFIX=${VFW_MINGW64_PREFIX}
			-DVFW_FIXTURE_DIR=${VFW_FIXTURE64_DIR}
		INSTALL_COMMAND ""
		BUILD_ALWAYS ON
		USES_TERMINAL_CONFIGURE ON
		USES_TERMINAL_BUILD ON)
elseif(VFW_REQUIRE_MINGW64)
	message(FATAL_ERROR "${VFW_MINGW64_PREFIX}gcc not found (install mingw-w64)")
else()
	message(STATUS "${VFW_MINGW64_PREFIX}gcc not found: the 64-bit "
		"fixture DLL tests will be skipped")
endif()

# Tests.

option(BUILD_TESTING "Build the tests" ${vfw_can_run})
if(BUILD_TESTING)
	enable_testing()
	add_test(NAME emulator_detection
		COMMAND ${CMAKE_COMMAND}
			-P ${PROJECT_SOURCE_DIR}/tests/cmake/emulator.cmake)
	add_test(NAME capture_arguments
		COMMAND ${CMAKE_COMMAND}
			-P ${PROJECT_SOURCE_DIR}/tests/cmake/capture.cmake)
	set(test_dir ${CMAKE_BINARY_DIR}/tests)

	# A test exits 77 when it needs something that isn't there (fixture
	# DLLs, FFmpeg); CTest reports that as skipped unless it is required.
	function(skippable test required)
		if(NOT required)
			set_tests_properties(${test} PROPERTIES SKIP_RETURN_CODE 77)
		endif()
	endfunction()

	# The fixtures the harness can run: the 64-bit ones in seance64's
	# build. That set is partial so far (docs/X64_PLAN.md), so tests
	# needing the others skip there, and smoke runs without fixtures.
	if(VFW_ARCH STREQUAL "x86_64")
		set(vfw_run_fixtures ${VFW_FIXTURE64_DIR})
		set(vfw_fixtures_required OFF)
		set(vfw_smoke_fixtures "")
	else()
		set(vfw_run_fixtures ${VFW_FIXTURE_DIR})
		set(vfw_fixtures_required ${VFW_REQUIRE_MINGW})
		set(vfw_smoke_fixtures $<$<BOOL:${VFW_MINGW_CC}>:${VFW_FIXTURE_DIR}>)
	endif()

	file(GLOB unit_sources CONFIGURE_DEPENDS tests/unit/test_*.c)
	foreach(src ${unit_sources})
		get_filename_component(name ${src} NAME_WE)
		add_executable(${name} ${src})
		target_include_directories(${name} PRIVATE tests/unit)
		target_link_libraries(${name} PRIVATE vfw_core)
		set_target_properties(${name} PROPERTIES
			RUNTIME_OUTPUT_DIRECTORY ${test_dir})
		add_test(NAME ${name} COMMAND ${name})
		set_tests_properties(${name} PROPERTIES
			ENVIRONMENT "VFW_FIXTURES=${vfw_run_fixtures};VFW_FIXTURES32=${VFW_FIXTURE_DIR};VFW_FIXTURES64=${VFW_FIXTURE64_DIR};VFW_EMULATOR=${vfw_emulator}"
			LABELS unit)
		skippable(${name} ${vfw_fixtures_required})
	endforeach()

	# Every struct, field and constant in include/win32, checked against
	# the mingw-w64 headers: gen_abi_checks prints static assertions from
	# our headers, which then have to compile with mingw's.
	file(GLOB win32_headers CONFIGURE_DEPENDS include/win32/*.h)
	set(abi_constants ${test_dir}/abi_constants.h)
	set(abi_checks ${test_dir}/abi_checks.inc)
	add_custom_command(OUTPUT ${abi_constants}
		COMMAND ${CMAKE_COMMAND} -DOUTPUT=${abi_constants}
			-P ${PROJECT_SOURCE_DIR}/cmake/capture.cmake --
			sh ${PROJECT_SOURCE_DIR}/tests/abi/constants.sh ${win32_headers}
		DEPENDS tests/abi/constants.sh tests/abi/constants_skip.txt
			${win32_headers}
		VERBATIM)
	add_executable(gen_abi_checks tests/abi/gen_abi_checks.c ${abi_constants})
	target_include_directories(gen_abi_checks PRIVATE include src tests/abi
		${test_dir})
	set_target_properties(gen_abi_checks PROPERTIES
		RUNTIME_OUTPUT_DIRECTORY ${test_dir})
	add_custom_command(OUTPUT ${abi_checks}
		COMMAND ${CMAKE_COMMAND} -DOUTPUT=${abi_checks}
			-P ${PROJECT_SOURCE_DIR}/cmake/capture.cmake --
			${CMAKE_CROSSCOMPILING_EMULATOR} $<TARGET_FILE:gen_abi_checks>
		DEPENDS gen_abi_checks
		VERBATIM)
	add_custom_target(abi_checks ALL DEPENDS ${abi_checks})

	# The checks hold for the architecture being built, so they are
	# compiled with the mingw-w64 for it.
	if(VFW_ARCH STREQUAL "x86_64")
		set(vfw_abi_cc "${VFW_MINGW64_CC}")
		set(vfw_abi_prefix ${VFW_MINGW64_PREFIX})
		set(vfw_abi_required ${VFW_REQUIRE_MINGW64})
	else()
		set(vfw_abi_cc "${VFW_MINGW_CC}")
		set(vfw_abi_prefix ${VFW_MINGW_PREFIX})
		set(vfw_abi_required ${VFW_REQUIRE_MINGW})
	endif()
	if(vfw_abi_cc)
		# GUIDs aren't constants, so check_abi gets mingw's values
		# from its preprocessor (see mingw_guids.sh).
		set(abi_mingw_guids ${test_dir}/abi_mingw_guids.inc)
		add_custom_command(OUTPUT ${abi_mingw_guids}
			COMMAND ${CMAKE_COMMAND} -DOUTPUT=${abi_mingw_guids}
				-P ${PROJECT_SOURCE_DIR}/cmake/capture.cmake --
				sh ${PROJECT_SOURCE_DIR}/tests/abi/mingw_guids.sh
				${vfw_abi_cc}
			DEPENDS tests/abi/mingw_guids.sh
			VERBATIM)
		add_custom_target(abi_mingw_guids ALL DEPENDS ${abi_mingw_guids})
		add_test(NAME check_abi
			COMMAND ${vfw_abi_cc} -std=c11 -fsyntax-only -I${test_dir}
				${PROJECT_SOURCE_DIR}/tests/abi/check_abi.c)
	else()
		add_test(NAME check_abi COMMAND sh -c
			"echo 'check_abi: ${vfw_abi_prefix}gcc not found'; exit 77")
	endif()
	set_tests_properties(check_abi PROPERTIES LABELS abi)
	skippable(check_abi ${vfw_abi_required})

	add_test(NAME smoke
		COMMAND sh ${PROJECT_SOURCE_DIR}/tests/smoke.sh
			$<TARGET_FILE:seance> ${PROJECT_SOURCE_DIR}/CMakeLists.txt
			"${vfw_smoke_fixtures}"
			$<IF:$<BOOL:${VFW_SANITIZE}>,0,1>)
	set_tests_properties(smoke PROPERTIES LABELS cli
		ENVIRONMENT "VFW_EMULATOR=${vfw_emulator}")

	# info and probe over codec DLLs you supply; the script itself is
	# checked over the fixture DLLs.
	if(VFW_MINGW_CC)
		add_test(NAME samples_fixtures
			COMMAND sh ${PROJECT_SOURCE_DIR}/tests/samples.sh
				$<TARGET_FILE:seance> ${VFW_FIXTURE_DIR})
		set_tests_properties(samples_fixtures PROPERTIES LABELS cli
			ENVIRONMENT "VFW_EMULATOR=${vfw_emulator}")
	endif()
	if(VFW_MINGW64_CC)
		add_test(NAME samples_fixtures64
			COMMAND sh ${PROJECT_SOURCE_DIR}/tests/samples.sh
				$<TARGET_FILE:seance> ${VFW_FIXTURE64_DIR})
		set_tests_properties(samples_fixtures64 PROPERTIES LABELS cli
			ENVIRONMENT "VFW_EMULATOR=${vfw_emulator}")
	endif()
	if(VFW_SAMPLES_DIR)
		add_test(NAME samples
			COMMAND sh ${PROJECT_SOURCE_DIR}/tests/samples.sh
				$<TARGET_FILE:seance> ${VFW_SAMPLES_DIR})
		set_tests_properties(samples PROPERTIES LABELS samples
			ENVIRONMENT "VFW_EMULATOR=${vfw_emulator}")
	endif()

	# The AVI and image code cross-checked with FFmpeg.
	add_test(NAME validate_media
		COMMAND sh ${PROJECT_SOURCE_DIR}/tests/validate_media.sh
			${CMAKE_BINARY_DIR} ${CMAKE_BINARY_DIR}/media
			$<TARGET_FILE:seance> ${vfw_run_fixtures})
	set_tests_properties(validate_media PROPERTIES LABELS media
		ENVIRONMENT "VFW_EMULATOR=${vfw_emulator}")
	skippable(validate_media ${VFW_REQUIRE_FFMPEG})
endif()

# The fixtures' configure and build wait for everything else, rather than
# running alongside the main build with their output interleaved with it.
if(TARGET fixtures)
	get_property(vfw_targets DIRECTORY PROPERTY BUILDSYSTEM_TARGETS)
	list(REMOVE_ITEM vfw_targets fixtures fixtures64)
	add_dependencies(fixtures ${vfw_targets})
	if(TARGET fixtures64)
		add_dependencies(fixtures64 fixtures)
	endif()
endif()
