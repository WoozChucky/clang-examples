add_library(netlib SHARED
    src/WinsockGuard.cpp
    src/Framer.cpp
)

target_include_directories(netlib
    PUBLIC  include
    PRIVATE src ../common/include
)

target_link_libraries(netlib PRIVATE
    ws2_32
)

target_compile_definitions(netlib PRIVATE
    NETLIB_EXPORTS
    NOMINMAX
    WIN32_LEAN_AND_MEAN
)

set_target_properties(netlib PROPERTIES
    OUTPUT_NAME netlib
    DEBUG_POSTFIX ""
    RUNTIME_OUTPUT_DIRECTORY "${RUNTIME_DIR}"
    ARCHIVE_OUTPUT_DIRECTORY "${RUNTIME_DIR}"
    LIBRARY_OUTPUT_DIRECTORY "${RUNTIME_DIR}"
    FOLDER Libraries
)

if(MSVC)
    set_property(TARGET netlib PROPERTY MSVC_DEBUG_INFORMATION_FORMAT
                 $<$<CONFIG:Debug>:Embedded>)
endif()
