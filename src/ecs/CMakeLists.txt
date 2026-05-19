add_library(ecs SHARED
    src/ecs.cpp
)

target_include_directories(ecs PUBLIC ../common/include)

target_link_libraries(ecs PUBLIC
    CommonHeaders
    glm::glm
)

set(GLM_DEFINES "GLM_FORCE_DEPTH_ZERO_TO_ONE; GLM_FORCE_RIGHT_HANDED; GLM_ENABLE_EXPERIMENTAL;")

target_compile_definitions(ecs PRIVATE
    ECS_EXPORTS
    NOMINMAX
    WIN32_LEAN_AND_MEAN
    ${GLM_DEFINES}
)

set_target_properties(ecs PROPERTIES
    OUTPUT_NAME ecs
    DEBUG_POSTFIX ""
    RUNTIME_OUTPUT_DIRECTORY "${RUNTIME_DIR}"
    ARCHIVE_OUTPUT_DIRECTORY "${RUNTIME_DIR}"
    LIBRARY_OUTPUT_DIRECTORY "${RUNTIME_DIR}"
    FOLDER Libraries
)

if(MSVC)
    set_property(TARGET ecs PROPERTY MSVC_DEBUG_INFORMATION_FORMAT
                 $<$<CONFIG:Debug>:Embedded>)
endif()
