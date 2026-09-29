cmake_minimum_required(VERSION 3.20)
project(vsock-tail VERSION 0.1.0 LANGUAGES C)

if(NOT CMAKE_SYSTEM_NAME STREQUAL "Linux")
    message(FATAL_ERROR "This reproducer requires Linux.")
endif()

set(CMAKE_C_STANDARD 17)
set(CMAKE_C_STANDARD_REQUIRED ON)

find_package(PkgConfig REQUIRED)
pkg_check_modules(KRUN REQUIRED IMPORTED_TARGET libkrun>=1.19)
if(KRUN_VERSION VERSION_GREATER_EQUAL "2.0")
    message(FATAL_ERROR "This reproducer uses the libkrun 1.x C API; install libkrun 1.19.x.")
endif()

add_executable(vsock-tail vsock_tail.c)
target_link_libraries(vsock-tail PRIVATE PkgConfig::KRUN)
target_compile_options(vsock-tail PRIVATE -Wall -Wextra -Wpedantic)

# The VM root contains only this executable, so it must not need a loader
# or shared libraries from the host filesystem.
add_executable(vsock-tail-guest vsock_tail.c)
target_compile_definitions(vsock-tail-guest PRIVATE REPRO_GUEST)
target_compile_options(vsock-tail-guest PRIVATE -Wall -Wextra -Wpedantic)
target_link_options(vsock-tail-guest PRIVATE -static)
add_dependencies(vsock-tail vsock-tail-guest)
