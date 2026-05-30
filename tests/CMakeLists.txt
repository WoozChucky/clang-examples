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
    ${CMAKE_SOURCE_DIR}/src/editor/src/viewport/EditorCamera.cpp
)

target_link_libraries(test_editorcam PRIVATE
    glm::glm
)

target_include_directories(test_editorcam PRIVATE
    ${CMAKE_SOURCE_DIR}/src/common/include
    ${CMAKE_SOURCE_DIR}/src/editor/src/viewport
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
    ${CMAKE_SOURCE_DIR}/src/editor/src/util
)

set_target_properties(test_metrichistory PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY "${RUNTIME_DIR}"
    FOLDER Tests
)

add_executable(test_transientstatus
    test_transientstatus.cpp
)

target_include_directories(test_transientstatus PRIVATE
    ${CMAKE_SOURCE_DIR}/src/editor/src/util
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
    ${CMAKE_SOURCE_DIR}/src/editor/src/app
    ${CMAKE_SOURCE_DIR}/src/editor/src/viewport
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

add_executable(test_playermove
    test_playermove.cpp
)

target_link_libraries(test_playermove PRIVATE
    glm::glm
)

target_include_directories(test_playermove PRIVATE
    ${CMAKE_SOURCE_DIR}/src/common/include
    ${CMAKE_SOURCE_DIR}/src/game/src
)

target_compile_definitions(test_playermove PRIVATE
    GLM_FORCE_DEPTH_ZERO_TO_ONE
    GLM_FORCE_RIGHT_HANDED
    GLM_ENABLE_EXPERIMENTAL
)

set_target_properties(test_playermove PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY "${RUNTIME_DIR}"
    FOLDER Tests
)

add_executable(test_collision
    test_collision.cpp
)

target_link_libraries(test_collision PRIVATE
    CommonHeaders
    glm::glm
    ecs
)

target_include_directories(test_collision PRIVATE
    ${CMAKE_SOURCE_DIR}/src/common/include
    ${CMAKE_SOURCE_DIR}/src/game/src
)

target_compile_definitions(test_collision PRIVATE
    GLM_FORCE_DEPTH_ZERO_TO_ONE
    GLM_FORCE_RIGHT_HANDED
    GLM_ENABLE_EXPERIMENTAL
)

set_target_properties(test_collision PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY "${RUNTIME_DIR}"
    FOLDER Tests
)

add_executable(test_followcam
    test_followcam.cpp
)

target_link_libraries(test_followcam PRIVATE
    glm::glm
)

target_include_directories(test_followcam PRIVATE
    ${CMAKE_SOURCE_DIR}/src/common/include
    ${CMAKE_SOURCE_DIR}/src/game/src
)

target_compile_definitions(test_followcam PRIVATE
    GLM_FORCE_DEPTH_ZERO_TO_ONE
    GLM_FORCE_RIGHT_HANDED
    GLM_ENABLE_EXPERIMENTAL
)

set_target_properties(test_followcam PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY "${RUNTIME_DIR}"
    FOLDER Tests
)

add_executable(test_atmosphere
    test_atmosphere.cpp
    ${CMAKE_SOURCE_DIR}/src/editor/src/panels/AtmospherePresets.cpp
)

target_link_libraries(test_atmosphere PRIVATE
    glm::glm
    ecs
    nlohmann_json::nlohmann_json
)

target_include_directories(test_atmosphere PRIVATE
    ${CMAKE_SOURCE_DIR}/src/common/include
    ${CMAKE_SOURCE_DIR}/src/game/src
    ${CMAKE_SOURCE_DIR}/src/editor/src/panels
)

target_compile_definitions(test_atmosphere PRIVATE
    GLM_FORCE_DEPTH_ZERO_TO_ONE
    GLM_FORCE_RIGHT_HANDED
    GLM_ENABLE_EXPERIMENTAL
)

set_target_properties(test_atmosphere PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY "${RUNTIME_DIR}"
    FOLDER Tests
)

add_executable(test_settings
    test_settings.cpp
)

target_link_libraries(test_settings PRIVATE
    CommonHeaders
    Engine
    nlohmann_json::nlohmann_json
)

target_include_directories(test_settings PRIVATE
    ${CMAKE_SOURCE_DIR}/src/common/include
    ${CMAKE_SOURCE_DIR}/src/engine/src
)

set_target_properties(test_settings PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY "${RUNTIME_DIR}"
    FOLDER Tests
)

add_executable(test_menu
    test_menu.cpp
)

target_link_libraries(test_menu PRIVATE
    glm::glm
)

target_include_directories(test_menu PRIVATE
    ${CMAKE_SOURCE_DIR}/src/common/include
)

target_compile_definitions(test_menu PRIVATE
    GLM_FORCE_DEPTH_ZERO_TO_ONE
    GLM_FORCE_RIGHT_HANDED
    GLM_ENABLE_EXPERIMENTAL
)

set_target_properties(test_menu PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY "${RUNTIME_DIR}"
    FOLDER Tests
)

add_executable(test_navmesh
    test_navmesh.cpp
)

target_link_libraries(test_navmesh PRIVATE
    CommonHeaders
    glm::glm
    ecs
    Engine
)

target_include_directories(test_navmesh PRIVATE
    ${CMAKE_SOURCE_DIR}/src/common/include
    ${CMAKE_SOURCE_DIR}/src/engine/src
    ${CMAKE_SOURCE_DIR}/src/game/src
)

target_compile_definitions(test_navmesh PRIVATE
    GLM_FORCE_DEPTH_ZERO_TO_ONE
    GLM_FORCE_RIGHT_HANDED
    GLM_ENABLE_EXPERIMENTAL
)

set_target_properties(test_navmesh PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY "${RUNTIME_DIR}"
    FOLDER Tests
)

add_executable(test_navagent
    test_navagent.cpp
)

target_link_libraries(test_navagent PRIVATE
    CommonHeaders
    glm::glm
    ecs
    Engine
)

target_include_directories(test_navagent PRIVATE
    ${CMAKE_SOURCE_DIR}/src/common/include
    ${CMAKE_SOURCE_DIR}/src/engine/src
    ${CMAKE_SOURCE_DIR}/src/game/src
)

target_compile_definitions(test_navagent PRIVATE
    GLM_FORCE_DEPTH_ZERO_TO_ONE
    GLM_FORCE_RIGHT_HANDED
    GLM_ENABLE_EXPERIMENTAL
)

set_target_properties(test_navagent PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY "${RUNTIME_DIR}"
    FOLDER Tests
)

add_executable(test_inspector_editstate
    test_inspector_editstate.cpp
)

target_link_libraries(test_inspector_editstate PRIVATE
    CommonHeaders
    glm::glm
    ecs
    nlohmann_json::nlohmann_json
)

target_include_directories(test_inspector_editstate PRIVATE
    ${CMAKE_SOURCE_DIR}/src/common/include
    ${CMAKE_SOURCE_DIR}/src/engine/include
    ${CMAKE_SOURCE_DIR}/src/engine/src/rendering
    ${CMAKE_SOURCE_DIR}/src/editor/src/app
    ${CMAKE_SOURCE_DIR}/src/editor/src/panels/inspector
)

target_compile_definitions(test_inspector_editstate PRIVATE
    GLM_FORCE_DEPTH_ZERO_TO_ONE
    GLM_FORCE_RIGHT_HANDED
    GLM_ENABLE_EXPERIMENTAL
)

set_target_properties(test_inspector_editstate PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY "${RUNTIME_DIR}"
    FOLDER Tests
)

add_executable(test_ssaomath
    test_ssaomath.cpp
)

target_link_libraries(test_ssaomath PRIVATE
    glm::glm
)

target_include_directories(test_ssaomath PRIVATE
    ${CMAKE_SOURCE_DIR}/src/engine/src/rendering
)

target_compile_definitions(test_ssaomath PRIVATE
    GLM_FORCE_DEPTH_ZERO_TO_ONE
    GLM_FORCE_RIGHT_HANDED
    GLM_ENABLE_EXPERIMENTAL
)

set_target_properties(test_ssaomath PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY "${RUNTIME_DIR}"
    FOLDER Tests
)

add_executable(test_aamode
    test_aamode.cpp
)

target_link_libraries(test_aamode PRIVATE
    nlohmann_json::nlohmann_json
)

target_include_directories(test_aamode PRIVATE
    ${CMAKE_SOURCE_DIR}/src/common/include
)

set_target_properties(test_aamode PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY "${RUNTIME_DIR}"
    FOLDER Tests
)

add_executable(test_netlib
    test_netlib.cpp
    # Framer is an internal (non-exported) class, so the test compiles its own copy rather than importing it from netlib.dll (keeps the DLL ABI clean).
    ${CMAKE_SOURCE_DIR}/src/netlib/src/Framer.cpp
)

target_link_libraries(test_netlib PRIVATE
    CommonHeaders
    netlib
    ws2_32
)

target_include_directories(test_netlib PRIVATE
    ${CMAKE_SOURCE_DIR}/src/netlib/include
    ${CMAKE_SOURCE_DIR}/src/netlib/src
)

set_target_properties(test_netlib PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY "${RUNTIME_DIR}"
    FOLDER Tests
)

add_executable(test_net
    test_net.cpp
)

target_link_libraries(test_net PRIVATE
    CommonHeaders
    Engine
    netlib
)

target_include_directories(test_net PRIVATE
    ${CMAKE_SOURCE_DIR}/src/common/include
    ${CMAKE_SOURCE_DIR}/src/engine/src
    ${CMAKE_SOURCE_DIR}/src/engine/src/network
    ${CMAKE_SOURCE_DIR}/src/netlib/include
)

set_target_properties(test_net PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY "${RUNTIME_DIR}"
    FOLDER Tests
)

add_executable(test_servercontrol
    test_servercontrol.cpp
    ${CMAKE_SOURCE_DIR}/src/editor/src/app/ServerSupervisor.cpp
)

target_link_libraries(test_servercontrol PRIVATE
    Engine
)

target_include_directories(test_servercontrol PRIVATE
    ${CMAKE_SOURCE_DIR}/src/common/include
    ${CMAKE_SOURCE_DIR}/src/editor/src/app
)

set_target_properties(test_servercontrol PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY "${RUNTIME_DIR}"
    FOLDER Tests
)
