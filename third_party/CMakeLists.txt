
add_subdirectory(glm)
add_subdirectory(freetype)
set(NVRHI_WITH_DX12 ON)
set(NVRHI_WITH_DX11 ON)
set(NVRHI_WITH_VULKAN ON)
add_subdirectory(NVRHI)

add_library(imgui STATIC
        imgui/imgui.cpp
        imgui/imgui_draw.cpp
        imgui/imgui_tables.cpp
        imgui/imgui_widgets.cpp
        imgui/imgui_impl_win32.cpp
        imgui/imgui_impl_dx11.cpp
        imgui/imgui_impl_dx12.cpp
)
set_target_properties(imgui PROPERTIES
        OUTPUT_NAME imgui
        RUNTIME_OUTPUT_DIRECTORY "${RUNTIME_DIR}"
        ARCHIVE_OUTPUT_DIRECTORY "${RUNTIME_DIR}"
        LIBRARY_OUTPUT_DIRECTORY "${RUNTIME_DIR}"
)
target_include_directories(imgui PUBLIC imgui)