
add_library(imgui_overlay SHARED
        src/imgui_overlay.cpp
)

target_include_directories(imgui_overlay PUBLIC include)
target_include_directories(imgui_overlay PRIVATE ../common/include)
target_include_directories(imgui_overlay PRIVATE ../runtime/src)

target_compile_definitions(imgui_overlay PRIVATE GLM_FORCE_DEPTH_ZERO_TO_ONE GLM_FORCE_RIGHT_HANDED GLM_ENABLE_EXPERIMENTAL)

target_link_libraries(imgui_overlay PRIVATE imgui nvrhi nvrhi_d3d12 nvrhi_d3d11 nvrhi_vk glm::glm)

set_target_properties(imgui_overlay PROPERTIES
        OUTPUT_NAME imgui_overlay
        RUNTIME_OUTPUT_DIRECTORY "${RUNTIME_DIR}"
        ARCHIVE_OUTPUT_DIRECTORY "${RUNTIME_DIR}"
        LIBRARY_OUTPUT_DIRECTORY "${RUNTIME_DIR}"
        FOLDER Libraries
)