# SPDX-License-Identifier: MIT
# Builds monado-remote-client against the monado submodule's headers only.
# No add_subdirectory on monado; the executable implements the wire protocol
# itself so no monado libraries need to be linked.
#
# monado/build must exist (so the generated xrt_config_os.h is present).
# Build monado once first: cmake -B monado/build monado && cmake --build monado/build
#
# Then from vibecoding-tests root:
#   cmake -B build && cmake --build build

set(MONADO ${CMAKE_CURRENT_SOURCE_DIR}/../monado)

add_executable(monado-remote-client main.c)

set_property(TARGET monado-remote-client PROPERTY C_STANDARD 11)
set_property(TARGET monado-remote-client PROPERTY C_STANDARD_REQUIRED ON)

target_include_directories(
    monado-remote-client PRIVATE
    ${MONADO}/src/xrt/include          # xrt/xrt_defines.h, xrt/xrt_compiler.h, ...
    ${MONADO}/build/src/xrt/include    # generated: xrt/xrt_config_os.h, ...
    ${MONADO}/src/xrt/auxiliary        # util/u_logging.h (pulled in by r_interface.h)
    ${MONADO}/src/xrt/drivers          # remote/r_interface.h
)
