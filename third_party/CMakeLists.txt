add_subdirectory(glm)
# Set folder for GLM targets
get_directory_property(glm_targets DIRECTORY glm BUILDSYSTEM_TARGETS)
foreach(target ${glm_targets})
    set_target_properties(${target} PROPERTIES FOLDER ThirdParty/GLM)
endforeach()

add_subdirectory(freetype)
# Set folder for FreeType targets
get_directory_property(freetype_targets DIRECTORY freetype BUILDSYSTEM_TARGETS)
foreach(target ${freetype_targets})
    set_target_properties(${target} PROPERTIES FOLDER ThirdParty/FreeType)
endforeach()

set(NVRHI_WITH_DX12 ON)
set(NVRHI_WITH_DX11 ON)
set(NVRHI_WITH_VULKAN ON)
add_subdirectory(NVRHI)
# Set folder for NVRHI targets
get_directory_property(nvrhi_targets DIRECTORY NVRHI BUILDSYSTEM_TARGETS)
foreach(target ${nvrhi_targets})
    set_target_properties(${target} PROPERTIES FOLDER ThirdParty/NVRHI)
endforeach()

set(DXC_ROOT "${CMAKE_SOURCE_DIR}/third_party/dxc-prebuilt")
add_library(dxcompiler SHARED IMPORTED)
set_target_properties(dxcompiler PROPERTIES
    IMPORTED_LOCATION "${DXC_ROOT}/bin/x64/dxcompiler.dll"
    IMPORTED_IMPLIB "${DXC_ROOT}/lib/x64/dxcompiler.lib"
)
target_include_directories(dxcompiler INTERFACE "${DXC_ROOT}/inc")

set(TRACY_ENABLE ON CACHE BOOL "" FORCE)
set(TRACY_ON_DEMAND ON CACHE BOOL "" FORCE)
add_subdirectory(tracy)
# Set folder for Tracy targets
get_directory_property(tracy_targets DIRECTORY tracy BUILDSYSTEM_TARGETS)
foreach(target ${tracy_targets})
    set_target_properties(${target} PROPERTIES FOLDER ThirdParty/Tracy)
endforeach()

option(GLFW_BUILD_EXAMPLES "" OFF)
option(GLFW_BUILD_TESTS "" OFF)
option(GLFW_BUILD_DOCS "" OFF)
option(GLFW_INSTALL "" OFF)
add_subdirectory(glfw)
# Set folder for GLFW targets
get_directory_property(glfw_targets DIRECTORY glfw BUILDSYSTEM_TARGETS)
foreach(target ${glfw_targets})
    set_target_properties(${target} PROPERTIES FOLDER ThirdParty/GLFW)
endforeach()

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
    FOLDER ThirdParty/ImGui
)
target_include_directories(imgui PUBLIC imgui)
target_link_libraries(imgui PRIVATE glfw)


add_library(ImGuizmo STATIC
    ImGuizmo/GraphEditor.cpp
    ImGuizmo/ImCurveEdit.cpp
    ImGuizmo/ImGradient.cpp
    ImGuizmo/ImSequencer.cpp
    ImGuizmo/ImGuizmo.cpp
)
set_target_properties(ImGuizmo PROPERTIES
    OUTPUT_NAME ImGuizmo
    RUNTIME_OUTPUT_DIRECTORY "${RUNTIME_DIR}"
    ARCHIVE_OUTPUT_DIRECTORY "${RUNTIME_DIR}"
    LIBRARY_OUTPUT_DIRECTORY "${RUNTIME_DIR}"
    FOLDER ThirdParty/ImGuizmo
)
target_include_directories(ImGuizmo PUBLIC ImGuizmo)
target_link_libraries(ImGuizmo PRIVATE imgui)

option(ASSIMP_BUILD_TESTS "" OFF)
option(ASSIMP_INSTALL "" OFF)
add_subdirectory(assimp)
# Set folder for GLFW targets
get_directory_property(assimp_targets DIRECTORY assimp BUILDSYSTEM_TARGETS)
foreach(target ${assimp_targets})
  set_target_properties(${target} PROPERTIES FOLDER ThirdParty/assimp)
endforeach()
