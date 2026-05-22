add_executable(runtime
    src/main.cpp
)

target_include_directories(runtime PRIVATE
    src
)

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

# Clang front-end only: these are gcc/clang-style warning suppressions; MSVC cl rejects them.
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
