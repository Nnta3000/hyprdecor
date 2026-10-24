cmake_minimum_required(VERSION 3.27)

project(hyprdecor
    DESCRIPTION "hyprdecor plugin for Hyprland"
    VERSION 0.1
)

set(CMAKE_CXX_STANDARD 23)

file(GLOB_RECURSE SRC "src/*.cpp")

add_library(hyprdecor SHARED ${SRC})
set_target_properties(hyprdecor PROPERTIES PREFIX "")

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

    # Tests the Lua value readers (booleans were being read as numbers and silently
    # ignored). Needs only liblua, which the plugin already links.
    pkg_check_modules(lua_only REQUIRED IMPORTED_TARGET lua5.5)
    add_executable(config_value_test tests/config_value_test.cpp)
    target_link_libraries(config_value_test PRIVATE PkgConfig::lua_only)
    target_compile_options(config_value_test PRIVATE -Wall -Wextra)
endif()
