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

add_executable(test_alloc
    test_alloc.cpp
)

target_link_libraries(test_alloc PRIVATE
    CommonHeaders
    Engine
)

target_include_directories(test_alloc PRIVATE
    ${CMAKE_SOURCE_DIR}/src/engine/src/rendering/passes
    ${CMAKE_SOURCE_DIR}/src/engine/src/rendering
)

set_target_properties(test_alloc PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY "${RUNTIME_DIR}"
    FOLDER Tests
)

add_executable(test_frustum
    test_frustum.cpp
)

target_link_libraries(test_frustum PRIVATE
    glm::glm
)

target_include_directories(test_frustum PRIVATE
    ${CMAKE_SOURCE_DIR}/src/engine/src/rendering
)

target_compile_definitions(test_frustum PRIVATE
    GLM_FORCE_DEPTH_ZERO_TO_ONE
    GLM_FORCE_RIGHT_HANDED
    GLM_ENABLE_EXPERIMENTAL
)

set_target_properties(test_frustum PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY "${RUNTIME_DIR}"
    FOLDER Tests
)

add_executable(test_input
    test_input.cpp
)

target_link_libraries(test_input PRIVATE
    CommonHeaders
    glm::glm
)

target_compile_definitions(test_input PRIVATE
    GLM_FORCE_DEPTH_ZERO_TO_ONE
    GLM_FORCE_RIGHT_HANDED
    GLM_ENABLE_EXPERIMENTAL
)

set_target_properties(test_input PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY "${RUNTIME_DIR}"
    FOLDER Tests
)

add_executable(test_picking
    test_picking.cpp
)

target_link_libraries(test_picking PRIVATE
    glm::glm
)

target_include_directories(test_picking PRIVATE
    ${CMAKE_SOURCE_DIR}/src/common/include
)

target_compile_definitions(test_picking PRIVATE
    GLM_FORCE_DEPTH_ZERO_TO_ONE
    GLM_FORCE_RIGHT_HANDED
    GLM_ENABLE_EXPERIMENTAL
)

set_target_properties(test_picking PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY "${RUNTIME_DIR}"
    FOLDER Tests
)

add_executable(test_editorcam
    test_editorcam.cpp
    ${CMAKE_SOURCE_DIR}/src/editor/src/rendering/EditorCamera.cpp
)

target_link_libraries(test_editorcam PRIVATE
    glm::glm
)

target_include_directories(test_editorcam PRIVATE
    ${CMAKE_SOURCE_DIR}/src/common/include
    ${CMAKE_SOURCE_DIR}/src/editor/src/rendering
)

target_compile_definitions(test_editorcam PRIVATE
    GLM_FORCE_DEPTH_ZERO_TO_ONE
    GLM_FORCE_RIGHT_HANDED
    GLM_ENABLE_EXPERIMENTAL
)

set_target_properties(test_editorcam PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY "${RUNTIME_DIR}"
    FOLDER Tests
)

add_executable(test_metrichistory
    test_metrichistory.cpp
)

target_include_directories(test_metrichistory PRIVATE
    ${CMAKE_SOURCE_DIR}/src/editor/src/rendering
)

set_target_properties(test_metrichistory PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY "${RUNTIME_DIR}"
    FOLDER Tests
)

add_executable(test_transientstatus
    test_transientstatus.cpp
)

target_include_directories(test_transientstatus PRIVATE
    ${CMAKE_SOURCE_DIR}/src/editor/src/rendering
)

set_target_properties(test_transientstatus PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY "${RUNTIME_DIR}"
    FOLDER Tests
)

add_executable(test_logformat
    test_logformat.cpp
)

target_include_directories(test_logformat PRIVATE
    ${CMAKE_SOURCE_DIR}/src/common/include
)

set_target_properties(test_logformat PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY "${RUNTIME_DIR}"
    FOLDER Tests
)

add_executable(test_debugdraw
    test_debugdraw.cpp
)

target_link_libraries(test_debugdraw PRIVATE
    glm::glm
)

target_include_directories(test_debugdraw PRIVATE
    ${CMAKE_SOURCE_DIR}/src/engine/src/rendering
)

target_compile_definitions(test_debugdraw PRIVATE
    GLM_FORCE_DEPTH_ZERO_TO_ONE
    GLM_FORCE_RIGHT_HANDED
    GLM_ENABLE_EXPERIMENTAL
)

set_target_properties(test_debugdraw PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY "${RUNTIME_DIR}"
    FOLDER Tests
)

add_executable(test_shadowmath
    test_shadowmath.cpp
)

target_link_libraries(test_shadowmath PRIVATE
    glm::glm
)

target_include_directories(test_shadowmath PRIVATE
    ${CMAKE_SOURCE_DIR}/src/engine/src/rendering
)

target_compile_definitions(test_shadowmath PRIVATE
    GLM_FORCE_DEPTH_ZERO_TO_ONE
    GLM_FORCE_RIGHT_HANDED
    GLM_ENABLE_EXPERIMENTAL
)

set_target_properties(test_shadowmath PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY "${RUNTIME_DIR}"
    FOLDER Tests
)

add_executable(test_worldserial
    test_worldserial.cpp
    ${CMAKE_SOURCE_DIR}/src/engine/src/rendering/Fog.cpp
)

target_link_libraries(test_worldserial PRIVATE
    CommonHeaders
    glm::glm
    ecs
    nlohmann_json::nlohmann_json
)

target_include_directories(test_worldserial PRIVATE
    ${CMAKE_SOURCE_DIR}/src/common/include
    ${CMAKE_SOURCE_DIR}/src/engine/src/rendering
)

target_compile_definitions(test_worldserial PRIVATE
    GLM_FORCE_DEPTH_ZERO_TO_ONE
    GLM_FORCE_RIGHT_HANDED
    GLM_ENABLE_EXPERIMENTAL
)

set_target_properties(test_worldserial PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY "${RUNTIME_DIR}"
    FOLDER Tests
)

add_executable(test_editorprefs
    test_editorprefs.cpp
)

target_link_libraries(test_editorprefs PRIVATE
    glm::glm
    nlohmann_json::nlohmann_json
)

target_include_directories(test_editorprefs PRIVATE
    ${CMAKE_SOURCE_DIR}/src/engine/include
    ${CMAKE_SOURCE_DIR}/src/engine/src/rendering
    ${CMAKE_SOURCE_DIR}/src/editor/src
)

target_compile_definitions(test_editorprefs PRIVATE
    GLM_FORCE_DEPTH_ZERO_TO_ONE
    GLM_FORCE_RIGHT_HANDED
    GLM_ENABLE_EXPERIMENTAL
)

set_target_properties(test_editorprefs PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY "${RUNTIME_DIR}"
    FOLDER Tests
)
