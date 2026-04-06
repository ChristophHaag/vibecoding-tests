cmake_minimum_required(VERSION 3.20)
project(xr_debug_gui_layer LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)

include(FetchContent)

# ── OpenXR Headers (header-only) ──────────────────────────────────────────────
FetchContent_Declare(
    openxr_headers
    GIT_REPOSITORY https://github.com/KhronosGroup/OpenXR-SDK.git
    GIT_TAG        release-1.1.46
    GIT_SHALLOW    TRUE
)
FetchContent_MakeAvailable(openxr_headers)

# ── SDL3 (always built from source as a static library) ──────────────────────
# We MUST use a static SDL3 to avoid symbol conflicts with apps that use SDL2.
# Do NOT use find_package — a system SDL3 shared library would leak symbols.
set(SDL_SHARED OFF CACHE BOOL "" FORCE)
set(SDL_STATIC ON CACHE BOOL "" FORCE)
set(SDL_TEST_LIBRARY OFF CACHE BOOL "" FORCE)
set(SDL_TESTS OFF CACHE BOOL "" FORCE)
FetchContent_Declare(
    SDL3
    GIT_REPOSITORY https://github.com/libsdl-org/SDL.git
    GIT_TAG        release-3.2.14
    GIT_SHALLOW    TRUE
    OVERRIDE_FIND_PACKAGE           # intercept any find_package(SDL3) calls
)
FetchContent_MakeAvailable(SDL3)

# ── Dear ImGui (docking branch) ──────────────────────────────────────────────
FetchContent_Declare(
    imgui
    GIT_REPOSITORY https://github.com/ocornut/imgui.git
    GIT_TAG        v1.92.0-docking
    GIT_SHALLOW    TRUE
)
FetchContent_MakeAvailable(imgui)

# Build ImGui as a static library
add_library(imgui_lib STATIC
    ${imgui_SOURCE_DIR}/imgui.cpp
    ${imgui_SOURCE_DIR}/imgui_draw.cpp
    ${imgui_SOURCE_DIR}/imgui_tables.cpp
    ${imgui_SOURCE_DIR}/imgui_widgets.cpp
    ${imgui_SOURCE_DIR}/imgui_demo.cpp
    ${imgui_SOURCE_DIR}/backends/imgui_impl_sdl3.cpp
    ${imgui_SOURCE_DIR}/backends/imgui_impl_opengl3.cpp
)
target_include_directories(imgui_lib PUBLIC
    ${imgui_SOURCE_DIR}
    ${imgui_SOURCE_DIR}/backends
)
target_link_libraries(imgui_lib PUBLIC SDL3::SDL3)
set_target_properties(imgui_lib PROPERTIES POSITION_INDEPENDENT_CODE ON)

# ── Find OpenGL ───────────────────────────────────────────────────────────────
find_package(OpenGL REQUIRED)

# ── Layer shared library ─────────────────────────────────────────────────────
add_library(XrApiLayer_debug_gui SHARED
    src/layer_main.cpp
    src/dispatch.cpp
    src/instance_data.cpp
    src/interceptors/instance.cpp
    src/interceptors/session.cpp
    src/interceptors/actions.cpp
    src/interceptors/spaces.cpp
    src/interceptors/frame.cpp
    src/gui/gui_main.cpp
    src/gui/gui_actions.cpp
    src/gui/gui_spaces.cpp
)

target_include_directories(XrApiLayer_debug_gui PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/src
    ${openxr_headers_SOURCE_DIR}/include
)

target_compile_definitions(XrApiLayer_debug_gui PRIVATE
    XR_NO_PROTOTYPES
)

target_link_libraries(XrApiLayer_debug_gui PRIVATE
    imgui_lib
    SDL3::SDL3
    OpenGL::GL
)

# ── Linker: export only the negotiate symbol ──────────────────────────────────
if(WIN32)
    # On Windows, use a .def file
    target_sources(XrApiLayer_debug_gui PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/XrApiLayer_debug_gui.def
    )
elseif(APPLE)
    set_target_properties(XrApiLayer_debug_gui PROPERTIES
        LINK_FLAGS "-Wl,-exported_symbols_list,\"${CMAKE_CURRENT_SOURCE_DIR}/XrApiLayer_debug_gui.expsym\""
    )
else()
    set_target_properties(XrApiLayer_debug_gui PROPERTIES
        LINK_FLAGS "-Wl,--version-script=\"${CMAKE_CURRENT_SOURCE_DIR}/XrApiLayer_debug_gui.map\" -Wl,-Bsymbolic -Wl,--exclude-libs,ALL"
    )
endif()

# ── Copy manifest next to library ─────────────────────────────────────────────
configure_file(
    ${CMAKE_CURRENT_SOURCE_DIR}/XrApiLayer_debug_gui.json
    ${CMAKE_CURRENT_BINARY_DIR}/XrApiLayer_debug_gui.json
    COPYONLY
)

# ── Generate wrapper script with baked-in paths ───────────────────────────────
set(LAYER_BUILD_DIR "${CMAKE_CURRENT_BINARY_DIR}")
configure_file(
    ${CMAKE_CURRENT_SOURCE_DIR}/xr-with-debug-gui.sh.in
    ${CMAKE_CURRENT_BINARY_DIR}/xr-with-debug-gui.sh
    @ONLY
)
# Make the generated script executable
file(CHMOD ${CMAKE_CURRENT_BINARY_DIR}/xr-with-debug-gui.sh
    PERMISSIONS OWNER_READ OWNER_WRITE OWNER_EXECUTE
                GROUP_READ GROUP_EXECUTE
                WORLD_READ WORLD_EXECUTE
)
