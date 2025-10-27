
add_library(imgui_overlay SHARED
        src/imgui_overlay.cpp
)

target_include_directories(imgui_overlay PUBLIC include)
target_include_directories(imgui_overlay PRIVATE ../common/include)

target_link_libraries(imgui_overlay PRIVATE imgui nvrhi nvrhi_d3d12 nvrhi_d3d11 nvrhi_vk)

set_target_properties(imgui_overlay PROPERTIES
        OUTPUT_NAME imgui_overlay
        RUNTIME_OUTPUT_DIRECTORY "${RUNTIME_DIR}"
        ARCHIVE_OUTPUT_DIRECTORY "${RUNTIME_DIR}"
        LIBRARY_OUTPUT_DIRECTORY "${RUNTIME_DIR}"
        FOLDER Libraries
)