cmake_minimum_required(VERSION 3.20)
project(ScanSnapIX1300Twain C)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)

# The TWAIN data source must be 32-bit: NAPS2 runs TWAIN in its Win32 worker
# (lib/NAPS2.Worker.exe) and the 32-bit DSM only loads 32-bit sources.
if(NOT CMAKE_SIZEOF_VOID_P EQUAL 4)
  message(WARNING "This data source targets x86; configure with -A Win32.")
endif()

set(COMMON_SOURCES src/proto.c src/usb.c src/image.c src/misc.c)

add_library(ScanSnapIX1300 MODULE
  src/ds.c src/session.c ${COMMON_SOURCES} src/version.rc)
target_include_directories(ScanSnapIX1300 PRIVATE third_party/twain src)
target_link_libraries(ScanSnapIX1300 PRIVATE winusb setupapi user32)
target_compile_definitions(ScanSnapIX1300 PRIVATE _CRT_SECURE_NO_WARNINGS)
target_link_options(ScanSnapIX1300 PRIVATE /DEF:${CMAKE_CURRENT_SOURCE_DIR}/src/entry.def)
set_target_properties(ScanSnapIX1300 PROPERTIES OUTPUT_NAME "ScanSnap-iX1300" PREFIX "" SUFFIX ".ds")

add_executable(selftest tools/selftest.c src/session.c ${COMMON_SOURCES})
target_include_directories(selftest PRIVATE third_party/twain src)
target_link_libraries(selftest PRIVATE winusb setupapi)
target_compile_definitions(selftest PRIVATE _CRT_SECURE_NO_WARNINGS)

add_executable(probe tools/probe.c src/session.c ${COMMON_SOURCES})
target_include_directories(probe PRIVATE third_party/twain src)
target_link_libraries(probe PRIVATE winusb setupapi)
target_compile_definitions(probe PRIVATE _CRT_SECURE_NO_WARNINGS)
