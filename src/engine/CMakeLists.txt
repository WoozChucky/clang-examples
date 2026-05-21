add_library(Engine SHARED
    src/MemoryCategory.cpp
    src/AllocatorRegistry.cpp
)

target_include_directories(Engine PUBLIC include)

target_link_libraries(Engine PUBLIC
    CommonHeaders
)

target_compile_definitions(Engine PRIVATE
    ENGINE_EXPORTS
    NOMINMAX
    WIN32_LEAN_AND_MEAN
)

set_target_properties(Engine PROPERTIES
    OUTPUT_NAME Engine
    DEBUG_POSTFIX ""
    RUNTIME_OUTPUT_DIRECTORY "${RUNTIME_DIR}"
    ARCHIVE_OUTPUT_DIRECTORY "${RUNTIME_DIR}"
    LIBRARY_OUTPUT_DIRECTORY "${RUNTIME_DIR}"
    FOLDER Libraries
)

if(MSVC)
    set_property(TARGET Engine PROPERTY MSVC_DEBUG_INFORMATION_FORMAT
                 $<$<CONFIG:Debug>:Embedded>)
endif()
