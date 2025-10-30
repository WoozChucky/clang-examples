#pragma once

#include "lib.h"
#include "ui.h"
#include "input.h"
#include "render.h"
#include "sound.h"

// #############################################################################
//                           Game Constants
// #############################################################################
constexpr int UPDATES_PER_SECOND = 60;
constexpr double UPDATE_DELAY = 1.0 / UPDATES_PER_SECOND;

// #############################################################################
//                           Game Structs
// #############################################################################
enum class GameStateId : uint8_t
{
  GAME_STATE_MAIN_MENU,
  GAME_STATE_IN_LEVEL,
  GAME_STATE_EDITOR,
};

struct GameState
{
  GameStateId m_State = GameStateId::GAME_STATE_MAIN_MENU;
  bool m_Initialized = false;

  double m_UpdateTimer = 0.0;

  Sound m_JumpSound{};
  Sound m_DeathSound{};
  bool m_QuitRequested = false;

  float m_Fps = 0.0f;
  float m_FrameTime = 0.0f;
  float m_GpuTime = 0.0f;

  uint64_t m_FrameCycles = 0;
  uint64_t m_UpdateGameCycles = 0;
  uint64_t m_RenderCycles = 0;
};

// #############################################################################
//                           Game Functions (Not Exposed)
// #############################################################################
void game_update_main_menu(GameState* gameState, UIState* uiState, RenderData* renderData, Input* input, float dt);
void game_update_in_level(GameState* gameState, UIState* uiState, SoundState* soundState, RenderData* renderData, Input* input,
  BumpAllocator* frameAllocator, size_t persistentStorageAllocated, size_t frameStorageAllocated, float dt);
void game_update_editor(GameState* gameState, UIState* uiState, RenderData* renderData, Input* input, float dt);

// #############################################################################
//                           Game Functions (Exposed)
// #############################################################################

// Runtime API versioning between main EXE and game DLL
constexpr uint32_t GAME_API_VERSION = 1;

// Function pointer type for platform-provided debug break handler
using game_debug_break_fn = void(*)(const char* expr, const char* file, int line, const char* message);

extern "C"
{
  // The game DLL must export this; the EXE will check it during hot-reload
  EXPORT_FN uint32_t game_get_api_version();

  // Allows the host executable to provide its platform-specific debug break handler
  EXPORT_FN void game_set_platform_debug_break(game_debug_break_fn fn);

  EXPORT_FN void game_update(GameState* gameStateIn, Input* inputIn, RenderData* renderDataIn,
                             SoundState* soundStateIn, UIState* uiStateIn, 
                             BumpAllocator* transientStorageIn, BumpAllocator* persistentStorageIn, size_t lastFrameAllocationBytes, float frameTime);

  EXPORT_FN void game_resize(int width, int height); // Check
}