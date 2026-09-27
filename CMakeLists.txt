# vfw-harness build.
#
#   cmake -B build            configure (or: cmake --preset default)
#   cmake --build build       build build/vfw-harness (i386) and, with
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
#   VFW_SAMPLES_DIR     codec DLLs for the samples test (info and probe)
cmake_minimum_required(VERSION 3.20)

# The harness loads 32-bit Windows DLLs into its own address space, so it
# is always an i386 program. Set before project() so the compiler checks
# use it too; they fail here if gcc-multilib (or the like) is missing.
if(NOT CMAKE_TOOLCHAIN_FILE)
	set(CMAKE_C_FLAGS_INIT "-m32")
	set(CMAKE_EXE_LINKER_FLAGS_INIT "-m32")
endif()

project(vfw-harness VERSION 0.1.0 LANGUAGES C)

if(NOT CMAKE_SIZEOF_VOID_P EQUAL 4)
	message(FATAL_ERROR "vfw-harness must be built as a 32-bit x86 "
		"program; the compiler gives ${CMAKE_SIZEOF_VOID_P}-byte pointers. "
		"Install gcc-multilib, or give a toolchain file for i386.")
endif()

option(VFW_REQUIRE_MINGW "Fail if mingw-w64 is missing" OFF)
option(VFW_REQUIRE_FFMPEG "Fail the media checks if FFmpeg is missing" OFF)
option(VFW_SANITIZE "Build with AddressSanitizer and UBSan" OFF)
set(VFW_MINGW_PREFIX "i686-w64-mingw32-" CACHE STRING
	"Prefix of the mingw-w64 cross tools")
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

add_executable(vfw-harness src/main.c)
target_link_libraries(vfw-harness PRIVATE vfw_core)
target_compile_definitions(vfw-harness PRIVATE
	VFW_HARNESS_VERSION="${PROJECT_VERSION}-dev")
set_target_properties(vfw-harness PROPERTIES
	RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR})

install(TARGETS vfw-harness RUNTIME DESTINATION bin)

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
		BUILD_ALWAYS ON)
elseif(VFW_REQUIRE_MINGW)
	message(FATAL_ERROR "${VFW_MINGW_PREFIX}gcc not found (install mingw-w64)")
else()
	message(STATUS "${VFW_MINGW_PREFIX}gcc not found: the fixture DLL "
		"tests and the ABI check will be skipped")
endif()

# Tests.

option(BUILD_TESTING "Build the tests" ON)
if(BUILD_TESTING)
	enable_testing()
	set(test_dir ${CMAKE_BINARY_DIR}/tests)

	# A test exits 77 when it needs something that isn't there (fixture
	# DLLs, FFmpeg); CTest reports that as skipped unless it is required.
	function(skippable test required)
		if(NOT required)
			set_tests_properties(${test} PROPERTIES SKIP_RETURN_CODE 77)
		endif()
	endfunction()

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
			ENVIRONMENT VFW_FIXTURES=${VFW_FIXTURE_DIR}
			LABELS unit)
		skippable(${name} ${VFW_REQUIRE_MINGW})
	endforeach()

	# Every struct, field and constant in include/win32, checked against
	# the mingw-w64 headers: gen_abi_checks prints static assertions from
	# our headers, which then have to compile with mingw's.
	file(GLOB win32_headers CONFIGURE_DEPENDS include/win32/*.h)
	set(abi_constants ${test_dir}/abi_constants.h)
	set(abi_checks ${test_dir}/abi_checks.inc)
	add_custom_command(OUTPUT ${abi_constants}
		COMMAND ${CMAKE_COMMAND} -DOUTPUT=${abi_constants}
			-P ${PROJECT_SOURCE_DIR}/cmake/capture.cmake
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
			-P ${PROJECT_SOURCE_DIR}/cmake/capture.cmake
			$<TARGET_FILE:gen_abi_checks>
		DEPENDS gen_abi_checks
		VERBATIM)
	add_custom_target(abi_checks ALL DEPENDS ${abi_checks})

	if(VFW_MINGW_CC)
		add_test(NAME check_abi
			COMMAND ${VFW_MINGW_CC} -std=c11 -fsyntax-only -I${test_dir}
				${PROJECT_SOURCE_DIR}/tests/abi/check_abi.c)
	else()
		add_test(NAME check_abi COMMAND sh -c
			"echo 'check_abi: ${VFW_MINGW_PREFIX}gcc not found'; exit 77")
	endif()
	set_tests_properties(check_abi PROPERTIES LABELS abi)
	skippable(check_abi ${VFW_REQUIRE_MINGW})

	add_test(NAME smoke
		COMMAND sh ${PROJECT_SOURCE_DIR}/tests/smoke.sh
			$<TARGET_FILE:vfw-harness> ${PROJECT_SOURCE_DIR}/CMakeLists.txt
			$<$<BOOL:${VFW_MINGW_CC}>:${VFW_FIXTURE_DIR}>
			$<IF:$<BOOL:${VFW_SANITIZE}>,0,1>)
	set_tests_properties(smoke PROPERTIES LABELS cli)

	# info and probe over codec DLLs you supply; the script itself is
	# checked over the fixture DLLs.
	if(VFW_MINGW_CC)
		add_test(NAME samples_fixtures
			COMMAND sh ${PROJECT_SOURCE_DIR}/tests/samples.sh
				$<TARGET_FILE:vfw-harness> ${VFW_FIXTURE_DIR})
		set_tests_properties(samples_fixtures PROPERTIES LABELS cli)
	endif()
	if(VFW_SAMPLES_DIR)
		add_test(NAME samples
			COMMAND sh ${PROJECT_SOURCE_DIR}/tests/samples.sh
				$<TARGET_FILE:vfw-harness> ${VFW_SAMPLES_DIR})
		set_tests_properties(samples PROPERTIES LABELS samples)
	endif()

	# The AVI and image code cross-checked with FFmpeg.
	add_test(NAME validate_media
		COMMAND sh ${PROJECT_SOURCE_DIR}/tests/validate_media.sh
			${CMAKE_BINARY_DIR} ${CMAKE_BINARY_DIR}/media)
	set_tests_properties(validate_media PROPERTIES LABELS media)
	skippable(validate_media ${VFW_REQUIRE_FFMPEG})
endif()
