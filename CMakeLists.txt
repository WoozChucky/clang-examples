cmake_minimum_required(VERSION 4.0.2)
project(clang_examples LANGUAGES CXX)

# Lock all targets to the same MSVC runtime library so STL containers cross
# DLL boundaries safely (editor.exe, ecs.dll, game.dll, test_ecs.exe).
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>DLL"
    CACHE STRING "" FORCE)

# Default to Debug on single-config generators

# set version of cpp to C++23
set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

set(RUNTIME_DIR "${CMAKE_BINARY_DIR}/bin/$<CONFIG>")

# third-party libraries
add_subdirectory(third_party)

# add common headers interface library
add_subdirectory(src/common)

# game library
add_subdirectory(src/game)

# imgui custom overlay library
add_subdirectory(src/overlay)

# main application (does not link game or imgui_overlay)
add_subdirectory(src/runtime)
# editor application (does not link game or imgui_overlay)
add_subdirectory(src/editor)

# Tests
add_subdirectory(tests)

# Visual Studio nicety: select startup project
set_property(DIRECTORY "${CMAKE_SOURCE_DIR}" PROPERTY VS_STARTUP_PROJECT editor)
