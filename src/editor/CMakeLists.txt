add_executable(editor
    src/main.cpp
    src/tiny_obj_loader.cpp

    # Core
    src/core/Application.cpp
    
    # Threading
    src/threading/PlatformThread.cpp
    src/threading/GameThread.cpp
    src/threading/RenderThread.cpp
    
    # Rendering
    src/rendering/Renderer.cpp
    src/rendering/RendererBackend.cpp
    src/rendering/ImGuiRenderer.cpp
    src/rendering/imgui_nvrhi.cpp
    src/rendering/registered_font.cpp
        
    # Shader Compilation
    src/rendering/shader/ShaderCompiler.cpp

    # Rendering Backends
    src/rendering/backends/RendererBackendDX12.cpp
    src/rendering/backends/RendererBackendVulkan.cpp

    # Rendering Passes
    src/rendering/passes/UiRenderPass.cpp
    src/rendering/passes/PrimitiveRenderPass.cpp
    src/rendering/passes/MeshRenderPass.cpp

    # Plugins
    src/plugins/DotNetPluginHost.cpp
    src/plugins/DotNetPluginManager.cpp
    
    # Third-party (keep Tracy here for now)
    ../../third_party/tracy/public/TracyClient.cpp
)

# Add all subdirectories to include paths for cleaner includes
target_include_directories(editor PRIVATE 
    src
    src/core
    src/threading
    src/rendering
    src/plugins
)

set(GLM_DEFINES "GLM_FORCE_DEPTH_ZERO_TO_ONE; GLM_FORCE_RIGHT_HANDED; GLM_ENABLE_EXPERIMENTAL;")

target_compile_definitions(editor PRIVATE NOMINMAX WIN32_LEAN_AND_MEAN ${GLM_DEFINES} GLFW_INCLUDE_VULKAN) 
# TRACY_ENABLE TRACY_ON_DEMAND

# Allocation tracker flag: ENABLE_ALLOC_TRACKER can be ON, OFF or AUTO (default)
set(ENABLE_ALLOC_TRACKER "AUTO" CACHE STRING "Enable allocation tracker (ON/OFF/AUTO)")
set_property(CACHE ENABLE_ALLOC_TRACKER PROPERTY STRINGS "AUTO" "ON" "OFF")
if(ENABLE_ALLOC_TRACKER STREQUAL "ON")
    target_compile_definitions(editor PRIVATE ALLOC_TRACKER_ENABLED)
elseif(ENABLE_ALLOC_TRACKER STREQUAL "AUTO")
    # Default: enable in Debug builds only
    # target_compile_definitions(editor PRIVATE $<$<CONFIG:Debug>:ALLOC_TRACKER_ENABLED>)
endif()

target_link_libraries(editor PRIVATE CommonHeaders GameHeaders freetype Tracy::TracyClient glm::glm glfw nvrhi nvrhi_d3d12 nvrhi_d3d11 d3dcompiler dxgi dxcompiler nvrhi_vk imgui ImGuizmo game)
if (TARGET Vulkan-Headers)
    target_link_libraries(editor PRIVATE Vulkan-Headers)
elseif (TARGET Vulkan::Headers)
    target_link_libraries(editor PRIVATE Vulkan::Headers)
endif()

set_target_properties(editor PROPERTIES
    OUTPUT_NAME editor
    RUNTIME_OUTPUT_DIRECTORY "${RUNTIME_DIR}"
    FOLDER Applications
    VS_DEBUGGER_WORKING_DIRECTORY "${RUNTIME_DIR}"
)

add_custom_command(TARGET editor POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_directory
    ${CMAKE_SOURCE_DIR}/assets ${RUNTIME_DIR}/assets
)

# Add .NET hosting include directory
target_include_directories(editor PRIVATE
    ${CMAKE_SOURCE_DIR}/third_party/dotnet/include
)

# Link against nethost
target_link_libraries(editor PRIVATE
    ${CMAKE_SOURCE_DIR}/third_party/dotnet/lib/nethost.lib
)

# Copy runtime DLLs to output directory
add_custom_command(TARGET editor POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
    ${CMAKE_SOURCE_DIR}/third_party/dotnet/bin/nethost.dll
    ${CMAKE_SOURCE_DIR}/third_party/dotnet/bin/hostfxr.dll
    $<TARGET_FILE_DIR:editor>
)

add_custom_command(TARGET editor POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
    "${CMAKE_SOURCE_DIR}/third_party/dxc-prebuilt/bin/x64/dxcompiler.dll"
    $<TARGET_FILE_DIR:editor>
)

if(CMAKE_CXX_COMPILER_ID STREQUAL "Clang")
    add_compile_options(-Wall -Wextra)
    # Keep your explicit suppressions if desired
    target_compile_options(editor PRIVATE -Wno-switch -Wno-writable-strings -Wno-sign-compare -Wno-deprecated-declarations -Wno-format-security -Wmissing-braces)
endif()

add_dependencies(editor game)
