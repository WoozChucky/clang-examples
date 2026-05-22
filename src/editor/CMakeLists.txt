set(GLM_DEFINES "GLM_FORCE_DEPTH_ZERO_TO_ONE; GLM_FORCE_RIGHT_HANDED; GLM_ENABLE_EXPERIMENTAL;")

add_executable(editor
    src/main.cpp

    # ImGui editor layer (editor-only; never compiled into Engine)
    src/rendering/imgui/ImGuiRenderer.cpp
    src/rendering/imgui/imgui_nvrhi.cpp
    src/rendering/imgui/registered_font.cpp
    src/rendering/imgui/MeshPreviewRenderer.cpp
    src/rendering/imgui/MemoryPanel.cpp
    src/rendering/imgui/RenderStatsPanel.cpp
    src/rendering/imgui/SceneViewport.cpp
    src/rendering/imgui/EditorFileDialog.cpp
    src/rendering/imgui/StatsPanel.cpp
    src/rendering/imgui/ImGuiOverlay.cpp
)

# Editor include paths. Engine headers (Renderer.h, IOverlay.h, MeshSystem.h,
# etc.) are inherited transitively via Engine's PUBLIC include dirs.
target_include_directories(editor PRIVATE
    src
    src/rendering
)

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

target_link_libraries(editor PRIVATE
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
    imgui
    ImGuizmo
    freetype
    Tracy::TracyClient
)
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

if(CMAKE_CXX_COMPILER_ID STREQUAL "Clang")
    add_compile_options(-Wall -Wextra)
    # Keep your explicit suppressions if desired
    target_compile_options(editor PRIVATE -Wno-switch -Wno-writable-strings -Wno-sign-compare -Wno-deprecated-declarations -Wno-format-security -Wmissing-braces)
endif()

add_dependencies(editor game)
