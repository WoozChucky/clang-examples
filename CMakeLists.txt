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

add_subdirectory(third_party/glm)
set(FASTNOISE2_NOISETOOL OFF CACHE BOOL "Build Noise Tool" FORCE)
add_subdirectory(third_party/FastNoise2)
set(NVRHI_WITH_DX12 ON)
set(NVRHI_WITH_DX11 ON)
set(NVRHI_WITH_VULKAN ON)
add_subdirectory(third_party/NVRHI)

# game: shared library loaded at runtime
add_library(game SHARED
  src/game.cpp
)
target_compile_definitions(game PRIVATE GAME_EXPORTS NOMINMAX WIN32_LEAN_AND_MEAN GLM_FORCE_DEPTH_ZERO_TO_ONE GLM_FORCE_RIGHT_HANDED GLM_ENABLE_EXPERIMENTAL)
target_link_libraries(game PRIVATE ${WINDOWS_LIBS} glm::glm)
set_target_properties(game PROPERTIES
  OUTPUT_NAME game
  RUNTIME_OUTPUT_DIRECTORY "${RUNTIME_DIR}"
  ARCHIVE_OUTPUT_DIRECTORY "${RUNTIME_DIR}"
  LIBRARY_OUTPUT_DIRECTORY "${RUNTIME_DIR}"
)

add_library(imgui STATIC
    third_party/imgui/imgui.cpp
    third_party/imgui/imgui_draw.cpp
    third_party/imgui/imgui_tables.cpp
    third_party/imgui/imgui_widgets.cpp
    third_party/imgui/imgui_impl_win32.cpp
    third_party/imgui/imgui_impl_dx11.cpp
)
set_target_properties(imgui PROPERTIES
    OUTPUT_NAME imgui
    RUNTIME_OUTPUT_DIRECTORY "${RUNTIME_DIR}"
    ARCHIVE_OUTPUT_DIRECTORY "${RUNTIME_DIR}"
    LIBRARY_OUTPUT_DIRECTORY "${RUNTIME_DIR}"
)

add_library(imgui_overlay SHARED
        src/imgui_overlay.cpp
)
target_compile_definitions(imgui_overlay PRIVATE OVERLAY_EXPORTS)
target_include_directories(imgui_overlay PRIVATE
    third_party/imgui
)
target_link_libraries(imgui_overlay PRIVATE imgui)
set_target_properties(imgui_overlay PROPERTIES
    OUTPUT_NAME imgui_overlay
    RUNTIME_OUTPUT_DIRECTORY "${RUNTIME_DIR}"
    ARCHIVE_OUTPUT_DIRECTORY "${RUNTIME_DIR}"
    LIBRARY_OUTPUT_DIRECTORY "${RUNTIME_DIR}"
)

# main application (does not link game or imgui_overlay)
add_executable(main_app
    src/main.cpp
    src/renderer.cpp
    src/renderer_dx12.cpp
    src/image.cpp
)
target_compile_definitions(main_app PRIVATE NOMINMAX WIN32_LEAN_AND_MEAN GLM_FORCE_DEPTH_ZERO_TO_ONE GLM_FORCE_RIGHT_HANDED GLM_ENABLE_EXPERIMENTAL)
target_link_libraries(main_app PRIVATE ${WINDOWS_LIBS} glm::glm FastNoise2 nvrhi nvrhi_d3d12 nvrhi_d3d11 nvrhi_vk)
set_target_properties(main_app PROPERTIES
  OUTPUT_NAME main
  RUNTIME_OUTPUT_DIRECTORY "${RUNTIME_DIR}"
)

# target_link_options(main_app PRIVATE $<$<CONFIG:Debug>:/SUBSYSTEM:WINDOWS>)

# Use GUI subsystem in Release (hide console window)
if(MSVC)
  # Reduce PDB contention for hot-reload: embed debug info in objects for Debug
  # (equivalent to /Z7)
  set_property(TARGET game     PROPERTY MSVC_DEBUG_INFORMATION_FORMAT $<$<CONFIG:Debug>:Embedded>)
  set_property(TARGET main_app PROPERTY MSVC_DEBUG_INFORMATION_FORMAT $<$<CONFIG:Debug>:Embedded>)
endif()

# Clang warning set (only when using clang front-end)
if(CMAKE_CXX_COMPILER_ID STREQUAL "Clang")
  add_compile_options(-Wall -Wextra)
  # Keep your explicit suppressions if desired
  target_compile_options(game PRIVATE -Wno-switch -Wno-writable-strings -Wno-sign-compare -Wno-deprecated-declarations -Wno-format-security -Wmissing-braces)
  target_compile_options(main_app PRIVATE -Wno-switch -Wno-writable-strings -Wno-sign-compare -Wno-deprecated-declarations -Wno-format-security -Wmissing-braces)
endif()

# Visual Studio nicety: select startup project
set_property(DIRECTORY "${CMAKE_SOURCE_DIR}" PROPERTY VS_STARTUP_PROJECT main_app)
