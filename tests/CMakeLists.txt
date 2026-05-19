add_executable(test_ecs
    test_ecs.cpp
)

target_link_libraries(test_ecs PRIVATE
    CommonHeaders
    glm::glm
    ecs
)

target_compile_definitions(test_ecs PRIVATE
    GLM_FORCE_DEPTH_ZERO_TO_ONE
    GLM_FORCE_RIGHT_HANDED
    GLM_ENABLE_EXPERIMENTAL
)

set_target_properties(test_ecs PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY "${RUNTIME_DIR}"
    FOLDER Tests
)
