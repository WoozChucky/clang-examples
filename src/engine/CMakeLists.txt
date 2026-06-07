set(GLM_DEFINES "GLM_FORCE_DEPTH_ZERO_TO_ONE; GLM_FORCE_RIGHT_HANDED; GLM_ENABLE_EXPERIMENTAL;")

add_library(Engine SHARED
    # Allocator toolkit (original Engine sources)
    src/MemoryCategory.cpp
    src/AllocatorRegistry.cpp

    # Platform debug hook (SM_ASSERT backend for the runtime core)
    src/PlatformDebug.cpp

    # Core
    src/core/Application.cpp
    src/core/ServerApplication.cpp
    src/core/EngineLogSink.cpp

    # Threading
    src/threading/PlatformThread.cpp
    src/threading/GameThread.cpp
    src/threading/RenderThread.cpp
    src/threading/GameLibrary.cpp

    # Rendering
    src/rendering/Renderer.cpp
    src/rendering/RendererBackend.cpp
    src/rendering/StagingBufferPool.cpp
    src/rendering/RenderStats.cpp
    src/rendering/Fog.cpp

    # Rendering Systems
    src/rendering/MeshSystem.cpp
    src/rendering/MaterialSystem.cpp

    # Rendering Backends
    src/rendering/backends/RendererBackendDX12.cpp
    src/rendering/backends/RendererBackendVulkan.cpp

    # Rendering Passes
    src/rendering/passes/GBufferFillPass.cpp
    src/rendering/passes/LightingRenderPass.cpp
    src/rendering/passes/SkyRenderPass.cpp
    src/rendering/passes/OutlineRenderPass.cpp
    src/rendering/passes/ShadowDepthPass.cpp
    src/rendering/passes/DebugRenderPass.cpp
    src/rendering/passes/UiRenderPass.cpp
    src/rendering/passes/FxaaRenderPass.cpp
    src/rendering/passes/SmaaRenderPass.cpp
    src/rendering/passes/SsaoRenderPass.cpp

    # Shader Compilation
    src/rendering/shader/ShaderCompiler.cpp

    # Plugins
    src/plugins/DotNetPluginManager.cpp
    src/plugins/DotNetPluginHost.cpp

    # Utilities
    src/utilities/MeshLoader.cpp
    src/utilities/MaterialLoader.cpp
    src/utilities/WorldManager.cpp
    src/utilities/SettingsManager.cpp

    # Navigation (Recast/Detour)
    src/navigation/NavMeshBuilder.cpp
    src/navigation/NavMesh.cpp
    src/navigation/NavMeshSystem.cpp
    src/navigation/NavServicesImpl.cpp

    # Animation
    src/animation/SkeletonStore.cpp

    # Networking (netlib integration)
    src/network/NetSubsystem.cpp
    src/network/NetServicesImpl.cpp

    # Third-party
    src/tiny_obj_loader.cpp
)

target_include_directories(Engine PUBLIC
    include
    src
    src/core
    src/threading
    src/rendering
    src/rendering/backends
    src/rendering/passes
    src/rendering/shader
    src/plugins
    src/utilities
    src/navigation
    src/network
    ${CMAKE_SOURCE_DIR}/third_party/dotnet/include
    ${CMAKE_SOURCE_DIR}/third_party/smaa/Textures
)

target_compile_definitions(Engine PUBLIC
    GLM_FORCE_DEPTH_ZERO_TO_ONE
    GLM_FORCE_RIGHT_HANDED
    GLM_ENABLE_EXPERIMENTAL
    GLFW_INCLUDE_VULKAN
    NOMINMAX
    WIN32_LEAN_AND_MEAN
)

target_compile_definitions(Engine PRIVATE
    ENGINE_EXPORTS
)

target_link_libraries(Engine PUBLIC
    CommonHeaders
    GameHeaders
    ecs
    netlib
    glm::glm
    glfw
    freetype
    Tracy::TracyClient
    nvrhi
    nvrhi_d3d12
    nvrhi_d3d11
    nvrhi_vk
    d3dcompiler
    dxgi
    dxcompiler
    assimp::assimp
    nlohmann_json::nlohmann_json
    Recast
    Detour
    DetourTileCache
    DebugUtils
    ${CMAKE_SOURCE_DIR}/third_party/dotnet/lib/nethost.lib
)
if (TARGET Vulkan-Headers)
    target_link_libraries(Engine PUBLIC Vulkan-Headers)
elseif (TARGET Vulkan::Headers)
    target_link_libraries(Engine PUBLIC Vulkan::Headers)
endif()

set_target_properties(Engine PROPERTIES
    OUTPUT_NAME Engine
    DEBUG_POSTFIX ""
    RUNTIME_OUTPUT_DIRECTORY "${RUNTIME_DIR}"
    ARCHIVE_OUTPUT_DIRECTORY "${RUNTIME_DIR}"
    LIBRARY_OUTPUT_DIRECTORY "${RUNTIME_DIR}"
    FOLDER Libraries
)

if(MSVC)
    set_property(TARGET Engine PROPERTY MSVC_DEBUG_INFORMATION_FORMAT
                 $<$<CONFIG:Debug>:Embedded>)
endif()

if(CMAKE_CXX_COMPILER_ID STREQUAL "Clang")
    target_compile_options(Engine PRIVATE -Wno-switch -Wno-writable-strings -Wno-sign-compare -Wno-deprecated-declarations -Wno-format-security -Wmissing-braces)
endif()

# Game.dll is loaded dynamically at runtime via GameLibrary; Engine must NOT
# link it. The Game DLL itself now links Engine.dll (for NavMeshSystem and
# other engine-side services it calls), which means Engine cannot depend on
# game without creating a CMake target cycle. Build-order is instead enforced
# by each executable target (editor, runtime) via its own
# add_dependencies(<exe> game).

# Copy runtime DLLs into the shared output dir so any executable that loads
# Engine.dll (editor or future runtime) finds them.
add_custom_command(TARGET Engine POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
    ${CMAKE_SOURCE_DIR}/third_party/dotnet/bin/nethost.dll
    ${CMAKE_SOURCE_DIR}/third_party/dotnet/bin/hostfxr.dll
    $<TARGET_FILE_DIR:Engine>
)

add_custom_command(TARGET Engine POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
    "${CMAKE_SOURCE_DIR}/third_party/dxc-prebuilt/bin/x64/dxcompiler.dll"
    $<TARGET_FILE_DIR:Engine>
)
