
add_executable(runtime
        src/main.cpp
        src/renderer.cpp
        src/renderer_dx12.cpp
        src/renderer_dx11.cpp
        # src/renderer_vulkan.cpp
        src/image.cpp
        src/font.cpp
)

target_include_directories(runtime PRIVATE ../common/include)
target_include_directories(runtime PRIVATE ../game/include)
target_include_directories(runtime PRIVATE ../overlay/include)

target_compile_definitions(runtime PRIVATE NOMINMAX WIN32_LEAN_AND_MEAN GLM_FORCE_DEPTH_ZERO_TO_ONE GLM_FORCE_RIGHT_HANDED GLM_ENABLE_EXPERIMENTAL VULKAN_HPP_DISPATCH_LOADER_DYNAMIC=1 VK_USE_PLATFORM_WIN32_KHR)

target_link_libraries(runtime PRIVATE ${WINDOWS_LIBS} glm::glm nvrhi nvrhi_d3d12 nvrhi_d3d11 nvrhi_vk freetype Vulkan::Headers)

set_target_properties(runtime PROPERTIES
    OUTPUT_NAME main
    RUNTIME_OUTPUT_DIRECTORY "${RUNTIME_DIR}"
    FOLDER Applications
)

# Now add a post build step to coppy the assets folder to the RUNTIME_DIR
add_custom_command(TARGET runtime POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_directory
    ${CMAKE_SOURCE_DIR}/assets ${RUNTIME_DIR}/assets
)

# Use GUI subsystem in Release (hide console window)
# target_link_options(runtime PRIVATE $<$<CONFIG:Debug>:/SUBSYSTEM:WINDOWS>)

if(MSVC)
    # Reduce PDB contention for hot-reload: embed debug info in objects for Debug
    # (equivalent to /Z7)
    set_property(TARGET runtime PROPERTY MSVC_DEBUG_INFORMATION_FORMAT $<$<CONFIG:Debug>:Embedded>)
endif()

# Clang warning set (only when using clang front-end)
if(CMAKE_CXX_COMPILER_ID STREQUAL "Clang")
    add_compile_options(-Wall -Wextra)
    # Keep your explicit suppressions if desired
    target_compile_options(runtime PRIVATE -Wno-switch -Wno-writable-strings -Wno-sign-compare -Wno-deprecated-declarations -Wno-format-security -Wmissing-braces)
endif()

# Visual Studio nicety: select startup project
set_property(DIRECTORY "${CMAKE_SOURCE_DIR}" PROPERTY VS_STARTUP_PROJECT runtime)