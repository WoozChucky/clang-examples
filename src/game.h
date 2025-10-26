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
enum GameInputType
{
  INPUT_MOVE_LEFT,
  INPUT_MOVE_RIGHT,
  INPUT_JUMP,
  INPUT_WALL_GRAB,
  INPUT_MOVE_UP,
  INPUT_MOVE_DOWN,
  INPUT_DASH,

  GAME_INPUT_COUNT
};

struct GameInput
{
  bool isDown;
  bool justPressed;
  float bufferingTime;
};

enum TileType
{
  TILE_TYPE_NONE,
  TILE_TYPE_SOLID,
  TILE_TYPE_SPIKE
};

struct Tile
{
  TileType type;
  int neighbourMask;
};

enum AnimationState
{
  ANIMATION_STATE_IDLE,
  ANIMATION_STATE_JUMP,
  ANIMATION_STATE_RUN,

  ANIMATION_STATE_COUNT
};

struct Player
{
  glm::ivec2 pos;
  glm::ivec2 prevPos;
  glm::vec2 solidSpeed;
  int renderOptions;
  float deathAnimTimer;
  float runAnimTimer;
  AnimationState animationState;
  // SpriteID animationSprites[ANIMATION_STATE_COUNT]; 
};

struct Tileset
{
  Array<glm::ivec2, 21> tileCoords;
};

struct Keyframe
{
  glm::ivec2 pos;
  float time; // how long to get there
};

struct Solid
{
  // SpriteID spriteID;

  // Pixel Movement
  glm::vec2 prevRemainder;
  glm::vec2 remainder;

  // Used by "interpolated rendering"
  glm::ivec2 prevPos;
  glm::ivec2 pos;

  int keyframeIdx;
  Array<Keyframe, 10> keyframes;

  // Animation
  float time;
  float waitingTime;
  float waitingDuration;
};

struct Level
{
  int version = 1;
  glm::ivec2 playerStartPos;
  Array<Solid, 50> solids;
};

enum GameStateID
{
  GAME_STATE_MAIN_MENU,
  GAME_STATE_IN_LEVEL
};

struct GameState
{
  GameStateID state;

  double updateTimer;
  bool initialized = false;
  float cameraTimer;
  GameInput gameInput[GAME_INPUT_COUNT];

  Player player;
  Level level;

  Sound jumpSound;
  Sound deathSound;
  bool quitRequested;

  float fps;
  float frameTime;
};

// #############################################################################
//                           Game Globals
// #############################################################################
static GameState* g_GameState;

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