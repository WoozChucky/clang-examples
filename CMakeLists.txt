cmake_minimum_required(VERSION 3.20)
project(clang_examples LANGUAGES CXX)

# Default to Debug on single-config generators

# set version of cpp to C++23
set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# Common Windows system libs
set(WINDOWS_LIBS user32 gdi32 ole32 kernel32)

# Output layout: put binaries in bin/<Config>
set(RUNTIME_DIR "${CMAKE_BINARY_DIR}/bin/$<CONFIG>")

# third-party libraries
add_subdirectory(third_party)

# game library
add_subdirectory(src/game)

# imgui custom overlay library
add_subdirectory(src/overlay)

# main application (does not link game or imgui_overlay)
add_subdirectory(src/runtime)
# editor application (does not link game or imgui_overlay)
add_subdirectory(src/editor)
