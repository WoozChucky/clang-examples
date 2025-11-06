
add_subdirectory(glm)
add_subdirectory(freetype)
set(NVRHI_WITH_DX12 ON)
set(NVRHI_WITH_DX11 ON)
set(NVRHI_WITH_VULKAN ON)
add_subdirectory(NVRHI)

set(TRACY_ENABLE ON CACHE BOOL "" FORCE)
add_subdirectory(tracy)

option(GLFW_BUILD_EXAMPLES "" OFF)
option(GLFW_BUILD_TESTS "" OFF)
option(GLFW_BUILD_DOCS "" OFF)
option(GLFW_INSTALL "" OFF)
add_subdirectory(glfw)

add_library(imgui STATIC
        imgui/imgui.cpp
        imgui/imgui_draw.cpp
        imgui/imgui_tables.cpp
        imgui/imgui_widgets.cpp
        imgui/imgui_impl_win32.cpp
        imgui/imgui_impl_dx11.cpp
        imgui/imgui_impl_dx12.cpp
        imgui/imgui_impl_glfw.cpp
)
set_target_properties(imgui PROPERTIES
        OUTPUT_NAME imgui
        RUNTIME_OUTPUT_DIRECTORY "${RUNTIME_DIR}"
        ARCHIVE_OUTPUT_DIRECTORY "${RUNTIME_DIR}"
        LIBRARY_OUTPUT_DIRECTORY "${RUNTIME_DIR}"
)
target_include_directories(imgui PUBLIC imgui)
target_link_libraries(imgui PRIVATE glfw)