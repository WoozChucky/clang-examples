#include "game.h"
#include "input.h"
#include "render.h"

// #############################################################################
//                           Game Globals
// #############################################################################
static BumpAllocator* transientStorage;
static BumpAllocator* persistentStorage;
static Input* g_Input;
static RenderData* g_RenderData;
static size_t g_LastFrameAllocationBytes = 0;

// #############################################################################
//                           Game Functions
// #############################################################################
// Input
void update_game_input(float dt);
void update(float dt);

// #############################################################################
//                           Game API Version (Exported)
// #############################################################################
EXPORT_FN uint32_t game_get_api_version()
{
  return GAME_API_VERSION;
}

// #############################################################################
//                   Platform Debug Break Handler (Exported)
// #############################################################################

// Platform-provided debug break callback (optional, set by host EXE)
static game_debug_break_fn g_PlatformDebugBreak = nullptr;

// Setter exported from the game DLL so the host can pass its platform handler
EXPORT_FN void game_set_platform_debug_break(const game_debug_break_fn fn)
{
  g_PlatformDebugBreak = fn;
}

// #############################################################################
//                           Game Update (Exported from DLL)
// #############################################################################
EXPORT_FN void game_update(GameState* gameStateIn, Input* inputIn, RenderData* renderDataIn,
                           SoundState* soundStateIn, UIState* uiStateIn,
                           BumpAllocator* transientStorageIn, BumpAllocator* persistentStorageIn, size_t lastFrameAllocationBytes, float frameTime)
{
  g_LastFrameAllocationBytes = lastFrameAllocationBytes;
  if(g_GameState != gameStateIn)
  {
    g_GameState = gameStateIn;
    g_Input = inputIn;
    g_RenderData = renderDataIn;
    g_SoundState = soundStateIn;
    g_UIState = uiStateIn;
    transientStorage = transientStorageIn;
    persistentStorage = persistentStorageIn;

    // Sounds
    const char* jumpSound = "assets/sounds/jump_01.wav";
    const char* deathSound = "assets/sounds/died_02.wav";
    // memcpy(g_GameState->jumpSound.path, jumpSound, strlen(jumpSound));
    // memcpy(g_GameState->deathSound.path, deathSound, strlen(deathSound));
    // Game Camera
    g_RenderData->gameCamera.aspectRatio = g_Input->screenSize.x / g_Input->screenSize.y;
    // UI Camera
    g_RenderData->uiCamera.dimensions.x = g_Input->screenSize.x;
    g_RenderData->uiCamera.dimensions.y = g_Input->screenSize.y;
    // Top Left is going to be 0/0 now
    g_RenderData->uiCamera.position.x = g_RenderData->uiCamera.dimensions.x / 2.0f;
    g_RenderData->uiCamera.position.y = -g_RenderData->uiCamera.dimensions.y / 2.0f;
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
    g_RenderData->gameCamera.aspectRatio = g_Input->screenSize.x / g_Input->screenSize.y;
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

  update_game_input(frameTime);
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

static bool showDiagnostics = true;
static bool showHUD = true;

void update_game_input(float dt) {
  if (key_is_down(g_Input, KEY_ESCAPE)) {
    g_GameState->quitRequested = true;
    return;
  }

  const glm::vec3 rot= g_RenderData->gameCamera.rotation; // pitch(x), yaw(y), roll(z)
  const float cp = cosf(rot.x), sp = sinf(rot.x);
  const float cy = cosf(rot.y), sy = sinf(rot.y);

  // Camera-to-world forward for RH, y-up, default forward = -Z:
  // forward = (Rx * Ry * Rz) * (0,0,-1) with roll=0
  const glm::vec3 forward = glm::normalize(glm::vec3(
      -sy,            // x
       sp * cy,       // y
      -cp * cy        // z
  ));

  // Right vector consistent with RH system
  const glm::vec3 right = glm::normalize(glm::cross(forward, glm::vec3(0,1,0)));

  float moveSpeed = 7.5f; // units/sec
  if (key_is_down(g_Input, KEY_W)) { g_RenderData->gameCamera.position += forward * (moveSpeed * dt); g_RenderData->gameCamera.invalidate(); }
  if (key_is_down(g_Input, KEY_S)) { g_RenderData->gameCamera.position -= forward * (moveSpeed * dt); g_RenderData->gameCamera.invalidate(); }
  if (key_is_down(g_Input, KEY_A)) { g_RenderData->gameCamera.position -= right   * (moveSpeed * dt); g_RenderData->gameCamera.invalidate(); }
  if (key_is_down(g_Input, KEY_D)) { g_RenderData->gameCamera.position += right   * (moveSpeed * dt); g_RenderData->gameCamera.invalidate(); }

  // For up/down you can keep world y or derive from a camera up vector
  if (key_is_down(g_Input, KEY_SPACE)) { g_RenderData->gameCamera.position.y += moveSpeed * dt; g_RenderData->gameCamera.invalidate(); }
  if (key_is_down(g_Input, KEY_SHIFT)) { g_RenderData->gameCamera.position.y -= moveSpeed * dt; g_RenderData->gameCamera.invalidate(); }

  constexpr float yawSpeed = glm::radians(120.0f);
  auto& yaw = g_RenderData->gameCamera.rotation.y;
  if (key_is_down(g_Input, KEY_Q)) { yaw += yawSpeed * dt; g_RenderData->gameCamera.invalidate(); }
  if (key_is_down(g_Input, KEY_E)) { yaw -= yawSpeed * dt; g_RenderData->gameCamera.invalidate(); }

  if (key_released_this_frame(g_Input, KEY_1)) {
    showDiagnostics = !showDiagnostics;
  }
  if (key_released_this_frame(g_Input, KEY_2)) {
    showHUD = !showHUD;
  }
}

void update(float dt) {

  static float diagnosticsTimer = 0.0f;
  static float fps = 0.0f;
  static float frameTimeMs = 0.0f;
  static size_t persistentMemoryUsed = 0;
  static size_t frameMemoryUsed = 0;
  diagnosticsTimer += dt;
  if (diagnosticsTimer >= 0.25f) {
    diagnosticsTimer = 0.0f;
    fps = g_GameState->fps;
    frameTimeMs = g_GameState->frameTime;
    persistentMemoryUsed = persistentStorage->used;
    frameMemoryUsed = g_LastFrameAllocationBytes;
  }

  if (showDiagnostics) {
    ui_draw_rect(g_RenderData, {0.f, 0.f}, {350.f, 150.f}, {1.0f, 1.0f, 1.0f, 0.3f});

    char* fpsText = bump_alloc(transientStorage, 64);
    snprintf(fpsText, 64, "FPS: %.0f", fps);
    ui_draw_text(g_RenderData, fpsText, {2.0f, 15.0f}, {1.0f, 1.0f, 0.0f, 1.0f});

    char* frameTimeText = bump_alloc(transientStorage, 64);
    snprintf(frameTimeText, 64, "Frame: %.3f ms", frameTimeMs);
    ui_draw_text(g_RenderData, frameTimeText, {2.0f, 30.0f}, {1.0f, 1.0f, 0.0f, 1.0f});

    char* persistentMemText = bump_alloc(transientStorage, 64);
    snprintf(persistentMemText, 64, "Persistent Mem: %.2f MB", persistentMemoryUsed / (1024.0f * 1024.0f));
    ui_draw_text(g_RenderData, persistentMemText, {2.0f, 45.0f}, {1.0f, 1.0f, 0.0f, 1.0f});

    char* transientMemText = bump_alloc(transientStorage, 64);
    snprintf(transientMemText, 64, "Frame Mem: %.2f MB", frameMemoryUsed / (1024.0f * 1024.0f));
    ui_draw_text(g_RenderData, transientMemText, {2.0f, 60.0f}, {1.0f, 1.0f, 0.0f, 1.0f});

    char* screenSizeText = bump_alloc(transientStorage, 64);
    snprintf(screenSizeText, 64, "Resolution: %d x %d", static_cast<int>(g_Input->screenSize.x), static_cast<int>(g_Input->screenSize.y));
    ui_draw_text(g_RenderData, screenSizeText, {2.0f, 75.f}, {1.0f, 1.0f, 0.0f, 1.0f});
  }

  if (showHUD) {
    ui_draw_rect(g_RenderData, {g_Input->screenSize.x - 410.f, g_Input->screenSize.y - 60.f}, {400.f, 50.f}, {0.0f, 0.0f, 0.0f, 0.3f});

    char* positionText = bump_alloc(transientStorage, 128);
    snprintf(positionText, 128, "Camera Pos: (%.2f, %.2f, %.2f)", g_RenderData->gameCamera.position.x, g_RenderData->gameCamera.position.y, g_RenderData->gameCamera.position.z);
    ui_draw_text(g_RenderData, positionText, {g_Input->screenSize.x - 400.f, g_Input->screenSize.y - 45.f}, {1.0f, 1.0f, 1.0f, 1.0f});

    char* rotationText = bump_alloc(transientStorage, 128);
    snprintf(rotationText, 128, "Camera Rot: (%.2f, %.2f, %.2f)", glm::degrees(g_RenderData->gameCamera.rotation.x), glm::degrees(g_RenderData->gameCamera.rotation.y), glm::degrees(g_RenderData->gameCamera.rotation.z));
    ui_draw_text(g_RenderData, rotationText, {g_Input->screenSize.x - 400.f, g_Input->screenSize.y - 30.f}, {1.0f, 1.0f, 1.0f, 1.0f});
  }

  ui_draw_hline(g_RenderData, {0.f, g_Input->screenSize.y / 2}, g_Input->screenSize.x, 2.0f, {1.0f, 0.0f, 0.0f, 1.0f});
  ui_draw_vline(g_RenderData, {g_Input->screenSize.x / 2, 0.f}, g_Input->screenSize.y, 2.0f, {0.0f, 1.0f, 0.0f, 1.0f});

  char* todoText = bump_alloc(transientStorage, 128);
  snprintf(todoText, 128, "TODO(Nuno): Fix VSync, Asserts with MsgBox + Line Number + FileName + Minimize Crash");
  ui_draw_text(g_RenderData, todoText, {g_Input->screenSize.x / 4.f, g_Input->screenSize.y / 2 - 5.f}, {1.0f, 1.0f, 1.0f, 1.0f});
}

// Assertion handler entry point used inside the game DLL.
// If the host EXE provided a platform-specific handler via set_platform_debug_break,
// we forward to it; otherwise, we use a minimal fallback (log + debugbreak).
void platform_debug_break(const char* expr, const char* file, int line, const char* message)
{
  if (g_PlatformDebugBreak)
  {
    g_PlatformDebugBreak(expr, file, line, message);
    return;
  }

  // Fallback when no platform callback has been set yet
  _log("ASSERT:", "Expression: %s | File: %s | Line: %d | %s", TEXT_COLOR_RED,
       (expr ? expr : "<none>"), (file ? file : "<unknown>"), line, (message ? message : ""));
  DEBUG_BREAK();
}
