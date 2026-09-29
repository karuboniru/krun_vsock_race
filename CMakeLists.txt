cmake_minimum_required(VERSION 3.20)
project(vsock-tail VERSION 0.2.0 LANGUAGES C)

if(NOT CMAKE_SYSTEM_NAME STREQUAL "Linux")
    message(FATAL_ERROR "This reproducer requires Linux.")
endif()

set(CMAKE_C_STANDARD 17)
set(CMAKE_C_STANDARD_REQUIRED ON)

find_package(PkgConfig REQUIRED)
# The host backend is selected by the libkrun version: 2.x has the builder API,
# 1.19 has the flat API. Both drive the same guest binary and harness.
pkg_check_modules(KRUN REQUIRED IMPORTED_TARGET libkrun)
if(KRUN_VERSION VERSION_GREATER_EQUAL "2.0")
    pkg_check_modules(KRUN_INIT REQUIRED IMPORTED_TARGET libkrun_init)
    set(host_source host_v2.c)
    set(host_libraries PkgConfig::KRUN PkgConfig::KRUN_INIT)
    message(STATUS "libkrun ${KRUN_VERSION}: using the 2.x builder backend")
else()
    if(KRUN_VERSION VERSION_LESS "1.19")
        message(FATAL_ERROR "libkrun ${KRUN_VERSION} is too old; 1.19 or 2.x is required.")
    endif()
    set(host_source host_v1.c)
    set(host_libraries PkgConfig::KRUN)
    message(STATUS "libkrun ${KRUN_VERSION}: using the 1.x backend")
endif()

add_executable(vsock-tail ${host_source})
target_include_directories(vsock-tail PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(vsock-tail PRIVATE ${host_libraries})
target_compile_options(vsock-tail PRIVATE -Wall -Wextra -Wpedantic)

# The VM root contains only this executable, so it must not need a loader
# or shared libraries from the host filesystem.
add_executable(vsock-tail-guest guest.c)
target_include_directories(vsock-tail-guest PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
target_compile_options(vsock-tail-guest PRIVATE -Wall -Wextra -Wpedantic)
target_link_options(vsock-tail-guest PRIVATE -static)
add_dependencies(vsock-tail vsock-tail-guest)
