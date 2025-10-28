
# game: shared library loaded at runtime
add_library(game SHARED
    src/game.cpp
    src/game_main_menu.cpp
    src/game_in_level.cpp
    src/game_editor.cpp
)
target_include_directories(game PUBLIC include)
target_include_directories(game PRIVATE ../common/include)

target_compile_definitions(game PRIVATE GAME_EXPORTS NOMINMAX WIN32_LEAN_AND_MEAN GLM_FORCE_DEPTH_ZERO_TO_ONE GLM_FORCE_RIGHT_HANDED GLM_ENABLE_EXPERIMENTAL)

target_link_libraries(game PRIVATE ${WINDOWS_LIBS} glm::glm)

set_target_properties(game PROPERTIES
  OUTPUT_NAME game
  RUNTIME_OUTPUT_DIRECTORY "${RUNTIME_DIR}"
  ARCHIVE_OUTPUT_DIRECTORY "${RUNTIME_DIR}"
  LIBRARY_OUTPUT_DIRECTORY "${RUNTIME_DIR}"
  FOLDER Libraries
)

if(MSVC)
  # Reduce PDB contention for hot-reload: embed debug info in objects for Debug
  # (equivalent to /Z7)
  set_property(TARGET game     PROPERTY MSVC_DEBUG_INFORMATION_FORMAT $<$<CONFIG:Debug>:Embedded>)
endif()

# Clang warning set (only when using clang front-end)
if(CMAKE_CXX_COMPILER_ID STREQUAL "Clang")
    add_compile_options(-Wall -Wextra)
    # Keep your explicit suppressions if desired
    target_compile_options(game PRIVATE -Wno-switch -Wno-writable-strings -Wno-sign-compare -Wno-deprecated-declarations -Wno-format-security -Wmissing-braces)
endif()