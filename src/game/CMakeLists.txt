
# game: shared library loaded at runtime

# This is and old reference to how the game library used to be built. Do not remove under any circumstances. 
#
#add_library(game SHARED
#    src/game.cpp
#    src/game_main_menu.cpp
#    src/game_in_level.cpp
#    src/game_editor.cpp
#)
#target_include_directories(game PUBLIC include)
#target_include_directories(game PRIVATE ../common/include)

#target_compile_definitions(game PRIVATE GAME_EXPORTS NOMINMAX WIN32_LEAN_AND_MEAN GLM_FORCE_DEPTH_ZERO_TO_ONE GLM_FORCE_RIGHT_HANDED GLM_ENABLE_EXPERIMENTAL)

#target_link_libraries(game PRIVATE ${WINDOWS_LIBS} glm::glm)

#set_target_properties(game PROPERTIES
#  OUTPUT_NAME game
#  RUNTIME_OUTPUT_DIRECTORY "${RUNTIME_DIR}"
#  ARCHIVE_OUTPUT_DIRECTORY "${RUNTIME_DIR}"
#  LIBRARY_OUTPUT_DIRECTORY "${RUNTIME_DIR}"
#  FOLDER Libraries
#)

#if(MSVC)
  # Reduce PDB contention for hot-reload: embed debug info in objects for Debug
  # (equivalent to /Z7)
#  set_property(TARGET game     PROPERTY MSVC_DEBUG_INFORMATION_FORMAT $<$<CONFIG:Debug>:Embedded>)
#endif()

# Clang warning set (only when using clang front-end)
#if(CMAKE_CXX_COMPILER_ID STREQUAL "Clang")
#    add_compile_options(-Wall -Wextra)
#    # Keep your explicit suppressions if desired
#    target_compile_options(game PRIVATE -Wno-switch -Wno-writable-strings -Wno-sign-compare -Wno-deprecated-declarations -Wno-format-security -Wmissing-braces)
#endif()

# New game library definition

add_library(game SHARED
    src/Game.cpp
)

target_include_directories(game PUBLIC include)

target_link_libraries(game PRIVATE glm::glm CommonHeaders ecs netlib Engine)

target_compile_definitions(game PRIVATE GAME_EXPORTS NOMINMAX WIN32_LEAN_AND_MEAN ${GLM_DEFINES})

# --- protobuf wire-format codegen ---
# protoc (built from the third_party/protobuf submodule) generates wire.pb.{h,cc}
# into the build tree; the .cc compiles into Game.dll and links libprotobuf-lite.
set(GAME_PROTO_DIR "${CMAKE_CURRENT_SOURCE_DIR}/proto")
set(GAME_PROTO_GEN "${CMAKE_CURRENT_BINARY_DIR}/gen")
file(MAKE_DIRECTORY "${GAME_PROTO_GEN}")
set(GAME_PROTO_CC  "${GAME_PROTO_GEN}/wire.pb.cc")
set(GAME_PROTO_H   "${GAME_PROTO_GEN}/wire.pb.h")

add_custom_command(
    OUTPUT  "${GAME_PROTO_CC}" "${GAME_PROTO_H}"
    COMMAND protobuf::protoc
            --cpp_out "${GAME_PROTO_GEN}"
            -I "${GAME_PROTO_DIR}"
            "${GAME_PROTO_DIR}/wire.proto"
    DEPENDS "${GAME_PROTO_DIR}/wire.proto" protobuf::protoc
    COMMENT "protoc: generating wire.pb.{h,cc}"
    VERBATIM
)

target_sources(game PRIVATE "${GAME_PROTO_CC}")
target_include_directories(game PRIVATE "${GAME_PROTO_GEN}")
# libprotobuf-lite links utf8_validity PRIVATE, so its include dir (which holds
# utf8_validity.h, pulled in by the public parse_context.h that generated .pb.cc
# transitively includes) is not propagated. Link it here to expose that header.
target_link_libraries(game PRIVATE protobuf::libprotobuf-lite utf8_validity)

set_target_properties(game PROPERTIES
  OUTPUT_NAME Game
  DEBUG_POSTFIX ""
  RUNTIME_OUTPUT_DIRECTORY "${RUNTIME_DIR}"
  ARCHIVE_OUTPUT_DIRECTORY "${RUNTIME_DIR}"
  LIBRARY_OUTPUT_DIRECTORY "${RUNTIME_DIR}"
  FOLDER Libraries
)

# Embed debug info in OBJ files instead of writing a separate Game.pdb.
# Required for hot-reload — avoids LNK1201 when the editor holds Game.dll
# while the linker tries to rewrite Game.pdb.
if(MSVC)
    set_property(TARGET game PROPERTY MSVC_DEBUG_INFORMATION_FORMAT
                 $<$<CONFIG:Debug>:Embedded>)
endif()

# Wipe stale Game_load_*.dll / .pdb copies before each game rebuild.
# Files currently held by a running editor will refuse delete; rm -f tolerates that.
add_custom_command(TARGET game PRE_BUILD
    COMMAND ${CMAKE_COMMAND} -E rm -f
            "${RUNTIME_DIR}/Game_load_*.dll"
            "${RUNTIME_DIR}/Game_load_*.pdb"
    VERBATIM
    COMMENT "Wiping stale Game_load_*.dll copies before rebuild"
)

add_library(GameHeaders INTERFACE)
target_include_directories(GameHeaders INTERFACE include)
target_link_libraries(GameHeaders INTERFACE glm::glm CommonHeaders)
