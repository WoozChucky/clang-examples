add_executable(editor
    src/main.cpp
)

target_link_libraries(editor PRIVATE glm::glm glfw nvrhi nvrhi_d3d12 nvrhi_d3d11)

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