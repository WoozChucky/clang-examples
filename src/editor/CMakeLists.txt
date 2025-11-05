add_executable(editor
    src/main.cpp
    src/PlatformThread.cpp
    src/GameThread.cpp
    src/RenderThread.cpp
    src/RendererBackendDX12.cpp
    src/RendererBackendVulkan.cpp
    src/Renderer.cpp

    src/imgui_nvrhi.cpp
    src/registered_font.cpp
)

set(GLM_DEFINES "GLM_FORCE_DEPTH_ZERO_TO_ONE; GLM_FORCE_RIGHT_HANDED; GLM_ENABLE_EXPERIMENTAL;")

target_compile_definitions(editor PRIVATE NOMINMAX WIN32_LEAN_AND_MEAN ${GLM_DEFINES} GLFW_INCLUDE_VULKAN)

# Allocation tracker flag: ENABLE_ALLOC_TRACKER can be ON, OFF or AUTO (default)
set(ENABLE_ALLOC_TRACKER "AUTO" CACHE STRING "Enable allocation tracker (ON/OFF/AUTO)")
set_property(CACHE ENABLE_ALLOC_TRACKER PROPERTY STRINGS "AUTO" "ON" "OFF")
if(ENABLE_ALLOC_TRACKER STREQUAL "ON")
    target_compile_definitions(editor PRIVATE ALLOC_TRACKER_ENABLED)
elseif(ENABLE_ALLOC_TRACKER STREQUAL "AUTO")
    # Default: enable in Debug builds only
    target_compile_definitions(editor PRIVATE $<$<CONFIG:Debug>:ALLOC_TRACKER_ENABLED>)
endif()

target_include_directories(editor PRIVATE ../common/include)

target_link_libraries(editor PRIVATE glm::glm glfw nvrhi nvrhi_d3d12 nvrhi_d3d11 d3dcompiler dxgi dxcompiler nvrhi_vk imgui)
if (TARGET Vulkan-Headers)
    target_link_libraries(editor PRIVATE Vulkan-Headers)
elseif (TARGET Vulkan::Headers)
    target_link_libraries(editor PRIVATE Vulkan::Headers)
endif()

#target_compile_definitions(glfw PRIVATE _GLFW_VULKAN_LIBRARY="sl.interposer.dll")

set_target_properties(editor PROPERTIES
    OUTPUT_NAME editor
    RUNTIME_OUTPUT_DIRECTORY "${RUNTIME_DIR}"
    FOLDER Applications
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

add_library(GameLogic SHARED
    src/Game.cpp
)
target_link_libraries(GameLogic PRIVATE glm::glm)
target_compile_definitions(GameLogic PRIVATE ${GLM_DEFINES})
target_include_directories(GameLogic PRIVATE ../common/include)
set_target_properties(GameLogic PROPERTIES
  OUTPUT_NAME Game
  RUNTIME_OUTPUT_DIRECTORY "${RUNTIME_DIR}"
  ARCHIVE_OUTPUT_DIRECTORY "${RUNTIME_DIR}"
  LIBRARY_OUTPUT_DIRECTORY "${RUNTIME_DIR}"
  FOLDER Libraries
)