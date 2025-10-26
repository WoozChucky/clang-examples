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
void update(float dt);

// #############################################################################
//                           Game Update (Exported from DLL)
// #############################################################################
EXPORT_FN void game_update(GameState* gameStateIn, Input* inputIn, RenderData* renderDataIn,
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
    g_GameState->level.solids = {};

    // Game Camera
    g_RenderData->gameCamera.position = {0.0, 2.5f, +5.0f};
    g_RenderData->gameCamera.rotation = {0.0f, 0.0f, 0.0f};
    g_RenderData->gameCamera.fov = glm::radians(80.0f);
    g_RenderData->gameCamera.aspectRatio = (float)g_Input->screenSize.x / (float)g_Input->screenSize.y;
    g_RenderData->gameCamera.nearClip = 0.1f;
    g_RenderData->gameCamera.farClip = 1000.0f;
    g_RenderData->gameCamera.invalidate();

    g_GameState->cameraTimer = 1.0f;

    // 3D model transform default (identity)
    g_RenderData->modelMatrix3D = glm::mat4(1.0f);

    // UI Camera
    g_RenderData->uiCamera.dimensions.x = g_Input->screenSize.x;
    g_RenderData->uiCamera.dimensions.y = g_Input->screenSize.y;
    // Top Left is going to be 0/0 now
    g_RenderData->uiCamera.position.x = g_RenderData->uiCamera.dimensions.x / 2.0f;
    g_RenderData->uiCamera.position.y = -g_RenderData->uiCamera.dimensions.y / 2.0f;
    g_RenderData->uiCamera.zoom = 1.0f;

    g_GameState->initialized = true;
  }

  g_RenderData->clearColor = {0.1f, 0.1f, 0.1f, 1.0f};

  g_GameState->updateTimer += frameTime;

  update(frameTime);

  // Reset Input
  // input->wheelDelta = 0;
  g_Input->relMouse = {};
  for(int keyIdx = 0; keyIdx < MAX_KEYCODES; keyIdx++)
  {
    g_Input->keys[keyIdx].justReleased = false;
    g_Input->keys[keyIdx].justPressed = false;
    g_Input->keys[keyIdx].halfTransitionCount = 0;
  }
  float interpolatedDT = (float)(g_GameState->updateTimer / UPDATE_DELAY);
  // draw(interpolatedDT);
}

// #############################################################################
//                           Game Resize (Exported from DLL)
// #############################################################################

EXPORT_FN void game_resize(const int width, const int height)
{
  if(g_RenderData)
  {
    // Game Camera
    g_RenderData->gameCamera.aspectRatio = static_cast<float>(width) / static_cast<float>(height);
    g_RenderData->gameCamera.invalidate();
    // UI Camera
    g_RenderData->uiCamera.dimensions.x = width;
    g_RenderData->uiCamera.dimensions.y = height;
    // Top Left is going to be 0/0 now
    g_RenderData->uiCamera.position.x = g_RenderData->uiCamera.dimensions.x / 2.0f;
    g_RenderData->uiCamera.position.y = -g_RenderData->uiCamera.dimensions.y / 2.0f;
  }
}

void update(float dt) {
  if (key_is_down(KEY_ESCAPE)) {
    g_GameState->quitRequested = true;
    return;
  }
  if (key_is_down(KEY_W)) {
    // Z+
    g_RenderData->gameCamera.position.z -= 10.0f * dt;
    g_RenderData->gameCamera.invalidate();
  }
  if (key_is_down(KEY_S)) {
    // Z-
    g_RenderData->gameCamera.position.z += 10.0f * dt;
    g_RenderData->gameCamera.invalidate();
  }
  if (key_is_down(KEY_A)) {
    // X-
    g_RenderData->gameCamera.position.x -= 10.0f * dt;
    g_RenderData->gameCamera.invalidate();
  }
  if (key_is_down(KEY_D)) {
    // X+
    g_RenderData->gameCamera.position.x += 10.0f * dt;
    g_RenderData->gameCamera.invalidate();
  }
  if (key_is_down(KEY_SPACE)) {
    // Y+
    g_RenderData->gameCamera.position.y += 10.0f * dt;
    g_RenderData->gameCamera.invalidate();
  }
  if (key_is_down(KEY_SHIFT)) {
    // Y-
    g_RenderData->gameCamera.position.y -= 10.0f * dt;
    g_RenderData->gameCamera.invalidate();
  }
  if (key_is_down(KEY_MOUSE_LEFT)) {

  }
}