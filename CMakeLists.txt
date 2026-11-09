cmake_minimum_required(VERSION 3.27)

project(hyprdecor
    DESCRIPTION "hyprdecor plugin for Hyprland"
    VERSION 0.3.1
)

set(CMAKE_CXX_STANDARD 23)

file(GLOB_RECURSE SRC "src/*.cpp")

add_library(hyprdecor SHARED ${SRC})
set_target_properties(hyprdecor PROPERTIES PREFIX "")

# Identify the exact build that is loaded. `hyprctl plugin list` shows
# HYPRDECOR_VERSION, and `hyprctl hyprdecor status` prints the version, commit and build
# date. The commit suffix means a locally rebuilt .so is distinguishable even when the
# semantic version has not changed.
execute_process(
    COMMAND git describe --always --dirty
    WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}
    OUTPUT_VARIABLE HYPRDECOR_GIT_COMMIT
    OUTPUT_STRIP_TRAILING_WHITESPACE
    ERROR_QUIET
)
if(NOT HYPRDECOR_GIT_COMMIT)
    set(HYPRDECOR_GIT_COMMIT "unknown")
endif()
string(TIMESTAMP HYPRDECOR_BUILD_DATE "%Y-%m-%d %H:%M")

target_compile_definitions(hyprdecor PRIVATE
    HYPRDECOR_VERSION="${PROJECT_VERSION}"
    HYPRDECOR_GIT_COMMIT="${HYPRDECOR_GIT_COMMIT}"
    HYPRDECOR_BUILD_DATE="${HYPRDECOR_BUILD_DATE}"
)

find_package(PkgConfig REQUIRED)
pkg_check_modules(deps REQUIRED IMPORTED_TARGET
    hyprland
    lua5.5
    libdrm
    libinput
    libudev
    pangocairo
    pixman-1
    wayland-server
    xkbcommon 
)
target_link_libraries(hyprdecor PRIVATE rt PkgConfig::deps)

# The plugin is built with the same flags Hyprland itself uses: warnings on, so
# uninitialised members and unused code surface at build time.
target_compile_options(hyprdecor PRIVATE -Wall -Wextra -Wunused -Wuninitialized)

install(TARGETS hyprdecor)

# ---------------------------------------------------------------------------
# Standalone tests for the nine-patch core (no Hyprland, no GPU required).
#   cmake --build build --target ninepatch_test && ./build/ninepatch_test
# ---------------------------------------------------------------------------
option(HYPRDECOR_BUILD_TESTS "Build the standalone nine-patch tests" ON)
if(HYPRDECOR_BUILD_TESTS)
    add_executable(ninepatch_test tests/ninepatch_test.cpp)
    target_include_directories(ninepatch_test PRIVATE src)
    pkg_check_modules(cairo_only REQUIRED IMPORTED_TARGET cairo)
    target_link_libraries(ninepatch_test PRIVATE PkgConfig::cairo_only)
    target_compile_options(ninepatch_test PRIVATE -Wall -Wextra)

    # Tests the resolved-layout pass: one pure function producing the rectangles that
    # drawing, hit-testing and `hyprctl hyprdecor status` all consume.
    add_executable(layout_test tests/layout_test.cpp)
    target_include_directories(layout_test PRIVATE src)
    target_link_libraries(layout_test PRIVATE PkgConfig::cairo_only)
    target_compile_options(layout_test PRIVATE -Wall -Wextra)

    # Tests the Lua value readers (booleans were being read as numbers and silently
    # ignored). Needs only liblua, which the plugin already links.
    pkg_check_modules(lua_only REQUIRED IMPORTED_TARGET lua5.5)
    add_executable(config_value_test tests/config_value_test.cpp)
    target_include_directories(config_value_test PRIVATE src)
    target_link_libraries(config_value_test PRIVATE PkgConfig::lua_only)
    target_compile_options(config_value_test PRIVATE -Wall -Wextra)
endif()

# ---------------------------------------------------------------------------
# Palette dithering tools (offline; no Hyprland, no GPU, no cairo).
#
#   cmake --build build --target palette-gen dither-test
#   ./build/palette-gen --palette ../orange-palette.png --out-dir ../build
#   ./build/dither-test --palette ../orange-palette.png --in scene.png --out out.png
#
# They read and write images through ImageMagick's `magick` binary rather than linking
# an image library, so the only requirement is magick being on PATH at run time.
# ---------------------------------------------------------------------------
option(HYPRDECOR_BUILD_TOOLS "Build the palette dithering tools" ON)
if(HYPRDECOR_BUILD_TOOLS)
    add_executable(palette-gen tools/palette_gen.cpp)
    target_include_directories(palette-gen PRIVATE tools)
    target_compile_options(palette-gen PRIVATE -Wall -Wextra -Wunused -Wuninitialized)

    add_executable(dither-test tools/dither_test.cpp)
    target_include_directories(dither-test PRIVATE tools)
    target_compile_options(dither-test PRIVATE -Wall -Wextra -Wunused -Wuninitialized)
endif()
