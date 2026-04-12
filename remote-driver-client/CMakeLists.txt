# SPDX-License-Identifier: MIT
# Standalone build for monado-remote-client.
# Builds against the monado submodule headers only; no monado libraries are linked.
#
# Prerequisite, run from ../monado:
#   cmake -G Ninja -S . -B build
#   cmake --build build
#
# Build this project from remote-driver-client/:
#   cmake -G Ninja -S . -B build
#   cmake --build build

cmake_minimum_required(VERSION 3.22)
project(monado_remote_client C)

set(MONADO ${CMAKE_CURRENT_SOURCE_DIR}/../monado)

if(NOT EXISTS ${MONADO}/src/xrt/drivers/remote/r_interface.h)
    message(FATAL_ERROR "Expected Monado sources at ${MONADO}")
endif()

if(NOT EXISTS ${MONADO}/build/src/xrt/include/xrt/xrt_config_build.h)
    message(FATAL_ERROR
        "Expected generated Monado headers in ${MONADO}/build. Build Monado first with: "
        "cd ../monado && cmake -G Ninja -S . -B build && cmake --build build")
endif()

add_executable(monado-remote-client main.c)

set_property(TARGET monado-remote-client PROPERTY C_STANDARD 11)
set_property(TARGET monado-remote-client PROPERTY C_STANDARD_REQUIRED ON)

target_include_directories(
    monado-remote-client PRIVATE
    ${MONADO}/src/xrt/include          # xrt/xrt_defines.h, xrt/xrt_compiler.h, ...
    ${MONADO}/build/src/xrt/include    # generated: xrt/xrt_config_build.h, ...
    ${MONADO}/src/xrt/auxiliary        # util/u_logging.h (pulled in by r_interface.h)
    ${MONADO}/src/xrt/drivers          # remote/r_interface.h
)
