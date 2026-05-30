add_executable(server
    src/main.cpp
)

target_include_directories(server PRIVATE
    src
    ${CMAKE_SOURCE_DIR}/src/engine/src        # core/ServerApplication.h
)

target_compile_definitions(server PRIVATE
    NOMINMAX
    WIN32_LEAN_AND_MEAN
    GLM_FORCE_DEPTH_ZERO_TO_ONE
    GLM_FORCE_RIGHT_HANDED
    GLM_ENABLE_EXPERIMENTAL
)

target_link_libraries(server PRIVATE
    Engine
    ecs
    netlib
    CommonHeaders
    GameHeaders
    glm::glm
)

set_target_properties(server PROPERTIES
    OUTPUT_NAME server
    RUNTIME_OUTPUT_DIRECTORY "${RUNTIME_DIR}"
    FOLDER Applications
    VS_DEBUGGER_WORKING_DIRECTORY "${RUNTIME_DIR}"
)

if(CMAKE_CXX_COMPILER_ID STREQUAL "Clang")
    target_compile_options(server PRIVATE -Wno-switch -Wno-writable-strings -Wno-sign-compare -Wno-deprecated-declarations -Wno-format-security -Wmissing-braces)
endif()

add_dependencies(server game)

add_custom_command(TARGET server POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_directory
    ${CMAKE_SOURCE_DIR}/assets ${RUNTIME_DIR}/assets
)
