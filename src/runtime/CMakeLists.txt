# The legacy src/runtime/src/* sources (deleted in task B2) still sit on disk
# next to main.cpp. One of them, renderer.h, includes the legacy render.h, which
# redefines camera structs already provided by the engine's Camera.h. MSVC's
# quoted #include search looks in the directory of the translation unit's source
# BEFORE the -I list, so Engine's RenderThread.h -> #include "Renderer.h" would
# resolve to src/runtime/src/renderer.h (case-insensitive match) instead of the
# engine's Renderer.h, dragging in render.h and breaking the build.
#
# To keep the legacy files untouched (per task B1) while building a clean TU, we
# compile a copy of main.cpp placed in the build tree, whose directory contains
# no shadowing headers. configure_file re-copies whenever main.cpp changes.
configure_file(
    "${CMAKE_CURRENT_SOURCE_DIR}/src/main.cpp"
    "${CMAKE_CURRENT_BINARY_DIR}/main.cpp"
    COPYONLY
)

add_executable(runtime
    "${CMAKE_CURRENT_BINARY_DIR}/main.cpp"
)

# src/runtime/src is intentionally NOT on the include path either. main.cpp only
# needs Engine/common/game headers (inherited transitively via the linked
# targets below).

target_compile_definitions(runtime PRIVATE
    NOMINMAX
    WIN32_LEAN_AND_MEAN
    GLM_FORCE_DEPTH_ZERO_TO_ONE
    GLM_FORCE_RIGHT_HANDED
    GLM_ENABLE_EXPERIMENTAL
    GLFW_INCLUDE_VULKAN
)

target_link_libraries(runtime PRIVATE
    Engine
    ecs
    CommonHeaders
    GameHeaders
    glm::glm
    glfw
    nvrhi
    nvrhi_d3d12
    nvrhi_d3d11
    nvrhi_vk
    d3dcompiler
    dxgi
    dxcompiler
    freetype
    Tracy::TracyClient
)
if (TARGET Vulkan-Headers)
    target_link_libraries(runtime PRIVATE Vulkan-Headers)
elseif (TARGET Vulkan::Headers)
    target_link_libraries(runtime PRIVATE Vulkan::Headers)
endif()

set_target_properties(runtime PROPERTIES
    OUTPUT_NAME runtime
    RUNTIME_OUTPUT_DIRECTORY "${RUNTIME_DIR}"
    FOLDER Applications
    VS_DEBUGGER_WORKING_DIRECTORY "${RUNTIME_DIR}"
)

if(CMAKE_CXX_COMPILER_ID STREQUAL "Clang")
    target_compile_options(runtime PRIVATE -Wno-switch -Wno-writable-strings -Wno-sign-compare -Wno-deprecated-declarations -Wno-format-security -Wmissing-braces)
endif()

# Build the hot-reloadable Game.dll before the player runs.
add_dependencies(runtime game)

# Ship assets next to runtime.exe so a player-only build is self-sufficient.
add_custom_command(TARGET runtime POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_directory
    ${CMAKE_SOURCE_DIR}/assets ${RUNTIME_DIR}/assets
)
