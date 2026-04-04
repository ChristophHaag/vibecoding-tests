# SPDX-License-Identifier: MIT
# Top-level build for vibecoding-tests.
# Builds only the tools in this repo; monado and openxr-simple-playground
# have their own independent build systems and are not included here.
#
# Build:
#   cmake -B build && cmake --build build

cmake_minimum_required(VERSION 3.22)
project(vibecoding-tests)

add_subdirectory(remote-driver-client)
