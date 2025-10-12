#include "game.h"
#include "input.h"
#include "render.h"

// #############################################################################
//                           Game Globals
// #############################################################################
static BumpAllocator* transientStorage;

// #############################################################################
//                           Game Functions
// #############################################################################
// Input
void update_game_input(float dt);
bool is_down(GameInputType type);
bool just_pressed(GameInputType type);

// #############################################################################
//                           Update Game (Exported from DLL)
// #############################################################################
EXPORT_FN void update_game(GameState* gameStateIn, Input* inputIn, RenderData* renderDataIn, 
                           SoundState* soundStateIn, UIState* uiStateIn,
                           BumpAllocator* transientStorageIn, float frameTime)
{
  if(g_GameState != gameStateIn)
  {
    g_GameState = gameStateIn;
    g_Input = inputIn;
    g_RenderData = renderDataIn;
    g_SoundState = soundStateIn;
    g_UIState = uiStateIn;
    transientStorage = transientStorageIn;

    // Sounds
    const char* jumpSound = "assets/sounds/jump_01.wav";
    const char* deathSound = "assets/sounds/died_02.wav";
    // memcpy(g_GameState->jumpSound.path, jumpSound, strlen(jumpSound));
    // memcpy(g_GameState->deathSound.path, deathSound, strlen(deathSound));
  }

  if(!g_GameState->initialized)
  {
    // Player
    g_GameState->player.deathAnimTimer = 0.0f;
    g_GameState->level.playerStartPos = {0, -4 * 8};
    g_GameState->player.pos = g_GameState->level.playerStartPos;
    g_GameState->player.prevPos = g_GameState->player.pos;

    // Level
    g_GameState->level.tileMap = {};    

    // Game Camera
    g_RenderData->gameCamera.position.y = -90.0f;
    g_RenderData->gameCamera.dimensions.x = ROOM_WIDTH;
    g_RenderData->gameCamera.dimensions.y = ROOM_HEIGHT;
    g_RenderData->gameCamera.zoom = 1.0f;
    g_RenderData->gameCamera.position.y = 
      g_RenderData->gameCamera.dimensions.y / 2.0f;
    g_GameState->cameraTimer = 1.0f;

    // UI Camera
    g_RenderData->uiCamera.dimensions.x = ROOM_WIDTH; // 320
    g_RenderData->uiCamera.dimensions.y = ROOM_HEIGHT; // 180
    // Top Left is going to be 0/0 now
    g_RenderData->uiCamera.position.x = g_RenderData->uiCamera.dimensions.x / 2.0f;
    g_RenderData->uiCamera.position.y = -g_RenderData->uiCamera.dimensions.y / 2.0f;
    g_RenderData->uiCamera.zoom = 1.0f;

    g_GameState->initialized = true;
  }

  g_RenderData->clearColor = {0.1f, 0.1f, 0.1f, 1.0f};

  g_GameState->updateTimer += frameTime;
  while(g_GameState->updateTimer >= UPDATE_DELAY)
  {
    g_GameState->updateTimer -= UPDATE_DELAY;
    // update();

    // Reset Input
    // input->wheelDelta = 0;
    g_Input->relMouse = {};
    for(int keyIdx = 0; keyIdx < MAX_KEYCODES; keyIdx++)
    {
      g_Input->keys[keyIdx].justReleased = false;
      g_Input->keys[keyIdx].justPressed = false;
      g_Input->keys[keyIdx].halfTransitionCount = 0;
    }
  }
  float interpolatedDT = (float)(g_GameState->updateTimer / UPDATE_DELAY);
  // draw(interpolatedDT);
}