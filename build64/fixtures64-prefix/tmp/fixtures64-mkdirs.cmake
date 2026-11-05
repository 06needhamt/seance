# Distributed under the OSI-approved BSD 3-Clause License.  See accompanying
# file Copyright.txt or https://cmake.org/licensing for details.

cmake_minimum_required(VERSION 3.5)

file(MAKE_DIRECTORY
  "/home/user/libseance/tests/fixtures"
  "/home/user/libseance/build64/fixtures64-build"
  "/home/user/libseance/build64/fixtures64-prefix"
  "/home/user/libseance/build64/fixtures64-prefix/tmp"
  "/home/user/libseance/build64/fixtures64-prefix/src/fixtures64-stamp"
  "/home/user/libseance/build64/fixtures64-prefix/src"
  "/home/user/libseance/build64/fixtures64-prefix/src/fixtures64-stamp"
)

set(configSubDirs )
foreach(subDir IN LISTS configSubDirs)
    file(MAKE_DIRECTORY "/home/user/libseance/build64/fixtures64-prefix/src/fixtures64-stamp/${subDir}")
endforeach()
if(cfgdir)
  file(MAKE_DIRECTORY "/home/user/libseance/build64/fixtures64-prefix/src/fixtures64-stamp${cfgdir}") # cfgdir has leading slash
endif()
