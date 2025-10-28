#include <game.h>
#include <ui.h>

// #############################################################################
//                           Game Globals
// #############################################################################
static BumpAllocator* transientStorage;
static BumpAllocator* persistentStorage;
static Input* g_Input;
static RenderData* g_RenderData;
static UIState* g_UIState;
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
  // Begin immediate-mode UI frame (collect input + clear UI command buffers)
  ui_im_begin_frame(g_UIState, g_RenderData, g_Input);
  update(frameTime);
  // End UI frame (renderer will consume command buffers)
  ui_im_end_frame(g_UIState);

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
    g_RenderData->uiCamera.dimensions.x = static_cast<float>(width);
    g_RenderData->uiCamera.dimensions.y = static_cast<float>(height);
    // Top Left is going to be 0/0 now
    g_RenderData->uiCamera.position.x = g_RenderData->uiCamera.dimensions.x / 2.0f;
    g_RenderData->uiCamera.position.y = -g_RenderData->uiCamera.dimensions.y / 2.0f;
  }
}

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
    g_UIState->showDiagnostics = !g_UIState->showDiagnostics;
  }
  if (key_released_this_frame(g_Input, KEY_2)) {
    g_UIState->showHUD = !g_UIState->showHUD;
  }

  // Mouse look when holding right mouse button
  const float mouseSensitivity = 0.0025f;
  if (key_is_down(g_Input, KEY_MOUSE_RIGHT)) {
    // Apply a small deadzone and axis-dominance filter to prevent cross-axis jitter
    int dx = g_Input->relMouse.x;
    int dy = g_Input->relMouse.y;

    // Deadzone (ignore tiny sub-pixel jitters)
    const int deadzonePx = 1; // tweak to 2 if needed
    if (dx > -deadzonePx && dx < deadzonePx) dx = 0;
    if (dy > -deadzonePx && dy < deadzonePx) dy = 0;

    // Axis-dominance suppression: if one axis movement is much larger, zero the other
    const int axisLockRatio = 3; // dominant axis must be >= 3x the minor axis
    int adx = (dx >= 0) ? dx : -dx;
    int ady = (dy >= 0) ? dy : -dy;
    if (adx >= ady * axisLockRatio) {
      dy = 0; // horizontal look only
    } else if (ady >= adx * axisLockRatio) {
      dx = 0; // vertical look only
    }

    SM_TRACE("Holding Mouse 1 Input (dx: %d, dy: %d)", dx, dy);

    g_RenderData->gameCamera.rotation.y -= dx * mouseSensitivity; // yaw
    g_RenderData->gameCamera.rotation.x -= dy * mouseSensitivity; // pitch
    // Clamp pitch to avoid flipping
    const float pitchLimit = glm::radians(89.0f);
    if (g_RenderData->gameCamera.rotation.x > pitchLimit) g_RenderData->gameCamera.rotation.x = pitchLimit;
    if (g_RenderData->gameCamera.rotation.x < -pitchLimit) g_RenderData->gameCamera.rotation.x = -pitchLimit;
    g_RenderData->gameCamera.invalidate();
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

  prim_draw_grid_plane(g_RenderData, glm::mat4(1.0f));

  // Total screen size container
  {
    UIPanelOptions opts{};
    opts.dock = UIDock::Fill;
    opts.padding = 0.0f;
    opts.bgColor = {0.0f, 0.0f, 0.0f, 0.0f};
    ui_push_id(g_UIState, "Screen");
    ui_begin_panel(g_UIState, g_RenderData, opts);

    // Always-visible small control panel with buttons
    {
      UIPanelOptions opt{};
      opt.dock = UIDock::Right;
      opt.x = {10, UIUnit::Px};
      opt.y = {10, UIUnit::Px};
      opt.w = {220, UIUnit::Px};
      opt.h = {80, UIUnit::Px};
      opt.padding = 6.0f;
      opt.bgColor = {0.1f, 0.1f, 0.1f, 0.0f};
      ui_push_id(g_UIState, "Controls");
      ui_begin_panel(g_UIState, g_RenderData, opt);


      if (ui_button(g_UIState, g_RenderData, g_UIState->showDiagnostics ? "Hide Diagnostics" : "Show Diagnostics", {210, 26})) {
        g_UIState->showDiagnostics = !g_UIState->showDiagnostics;
      }

      if (ui_button(g_UIState, g_RenderData, g_UIState->showHUD ? "Hide HUD" : "Show HUD", {210, 26})) {
        g_UIState->showHUD = !g_UIState->showHUD;
      }

      if (ui_button(g_UIState, g_RenderData, g_UIState->showEditor ? "Hide Editor" : "Show Editor", {210, 26})) {
        g_UIState->showEditor = !g_UIState->showEditor;
      }

      ui_end_panel(g_UIState);
      ui_pop_id(g_UIState);
    }

    if (g_UIState->showEditor) {
      // Lets start by drawing a 2D grid that fills the screen
      // We do this by getting percentages of the screen size for width/height and drawing hlines/vlines all across
      auto screenW    = g_Input->screenSize.x;
      auto screenH    = g_Input->screenSize.y;
      const float gridSpacing = g_UIState->editorState.gridSpacing * g_UIState->editorState.gridScale;
      const int numVLines = static_cast<int>(screenW / gridSpacing);
      const int numHLines = static_cast<int>(screenH / gridSpacing);
      for (int i = 0; i <= numVLines; i++) {
        float x = i * gridSpacing;
        ui_draw_vline(g_RenderData, {x, 0.0f}, screenH, 1.f, {0.2f, 0.2f, 0.2f, 1.0f});
      }
      for (int j = 0; j <= numHLines; j++) {
        float y = j * gridSpacing;
        ui_draw_hline(g_RenderData, {0.0f, y}, screenW, 1.f, {0.2f, 0.2f, 0.2f, 1.0f});
      }

      // Editor panel docked to left
      UIPanelOptions opt{};
      opt.dock = UIDock::Left;
      opt.w = {260, UIUnit::Px};
      opt.padding = 6.0f;
      opt.bgColor = {0.2f, 0.2f, 0.2f, 0.1f};
      ui_push_id(g_UIState, "Editor");
      ui_begin_panel(g_UIState, g_RenderData, opt);

      ui_label(g_UIState, g_RenderData, "Editor Panel", {2.0f, 10.0f}, {1.0f, 1.0f, 1.0f, 1.0f});

      // Editor Controls
      {
        //ui_push_id(g_UIState, "Editor Controls");
        //opt.padding = 6.f;
        //ui_begin_panel(g_UIState, g_RenderData, opt);
        ui_button(g_UIState, g_RenderData, "Add Panel", {180, 26});
        ui_button(g_UIState, g_RenderData, "Add Label", {180, 26});
        ui_button(g_UIState, g_RenderData, "Add Button", {180, 26});
        //ui_end_panel(g_UIState);
        //ui_pop_id(g_UIState);
      }

      ui_end_panel(g_UIState);
      ui_pop_id(g_UIState);
    }

    // Diagnostics panel docked to top (uses percentage+px sizing)
    if (g_UIState->showDiagnostics) {
      UIPanelOptions opt{};
      opt.dock = UIDock::None;
      opt.x = {10, UIUnit::Px};
      opt.y = {10, UIUnit::Px};
      opt.h = {160, UIUnit::Px};
      opt.w = {340, UIUnit::Px};
      opt.padding = 6.0f;
      opt.bgColor = {0.1f, 0.1f, 0.1f, 0.1f};
      PanelContext pc = ui_begin_panel(g_UIState, g_RenderData, opt);

      // Hover effect overlay
      const bool hovered = (g_UIState->mousePosUI.x >= pc.panelRect.pos.x && g_UIState->mousePosUI.x <= pc.panelRect.pos.x + pc.panelRect.size.x &&
                            g_UIState->mousePosUI.y >= pc.panelRect.pos.y && g_UIState->mousePosUI.y <= pc.panelRect.pos.y + pc.panelRect.size.y);
      if (hovered) {
        ui_draw_rect(g_RenderData, pc.panelRect.pos, pc.panelRect.size, {1.0f, 1.0f, 1.0f, 0.20f});
      }

      // Text lines inside content area
      char* fpsText = bump_alloc(transientStorage, 64);
      snprintf(fpsText, 64, "FPS: %.0f", fps);
      ui_label(g_UIState, g_RenderData, fpsText, {2.0f, 10.0f}, {0.0f, 1.0f, 0.0f, 1.0f});

      char* frameTimeText = bump_alloc(transientStorage, 64);
      snprintf(frameTimeText, 64, "Frame: %.3f ms", frameTimeMs);
      ui_label(g_UIState, g_RenderData, frameTimeText, {2.0f, 25.0f}, {1.0f, 1.0f, 0.0f, 1.0f});

      char* persistentMemText = bump_alloc(transientStorage, 64);
      const double persistentMB = static_cast<double>(persistentMemoryUsed) / (1024.0 * 1024.0);
      snprintf(persistentMemText, 64, "Persistent Mem: %.2f MB", persistentMB);
      ui_label(g_UIState, g_RenderData, persistentMemText, {2.0f, 40.0f}, {1.0f, 1.0f, 0.0f, 1.0f});

      char* transientMemText = bump_alloc(transientStorage, 64);
      const double frameMB = static_cast<double>(frameMemoryUsed) / (1024.0 * 1024.0);
      snprintf(transientMemText, 64, "Frame Mem: %.2f MB", frameMB);
      ui_label(g_UIState, g_RenderData, transientMemText, {2.0f, 55.0f}, {1.0f, 1.0f, 0.0f, 1.0f});

      char* screenSizeText = bump_alloc(transientStorage, 64);
      snprintf(screenSizeText, 64, "Resolution: %d x %d", static_cast<int>(g_Input->screenSize.x), static_cast<int>(g_Input->screenSize.y));
      ui_label(g_UIState, g_RenderData, screenSizeText, {2.0f, 70.0f}, {1.0f, 1.0f, 0.0f, 1.0f});

      ui_end_panel(g_UIState);
    }

    // HUD panel docked to bottom
    if (g_UIState->showHUD) {
      UIPanelOptions opt{};
      opt.dock = UIDock::Bottom;
      opt.h = {40, UIUnit::Px};
      opt.padding = 6.0f;
      ui_begin_panel(g_UIState, g_RenderData, opt);

      char* positionText = bump_alloc(transientStorage, 128);
      snprintf(positionText, 128, "Camera Pos: (%.2f, %.2f, %.2f)", g_RenderData->gameCamera.position.x, g_RenderData->gameCamera.position.y, g_RenderData->gameCamera.position.z);
      ui_label(g_UIState, g_RenderData, positionText, {10.0f, 8.0f}, {1.0f, 1.0f, 1.0f, 1.0f});

      char* rotationText = bump_alloc(transientStorage, 128);
      snprintf(rotationText, 128, "Camera Rot: (%.2f, %.2f, %.2f)", glm::degrees(g_RenderData->gameCamera.rotation.x), glm::degrees(g_RenderData->gameCamera.rotation.y), glm::degrees(g_RenderData->gameCamera.rotation.z));
      ui_label(g_UIState, g_RenderData, rotationText, {10.0f, 24.0f}, {1.0f, 1.0f, 1.0f, 1.0f});

      ui_end_panel(g_UIState);
    }

    ui_end_panel(g_UIState);
    ui_pop_id(g_UIState);
  }

  // Crosshair and a random note label using low-level helpers (still valid)
  ui_draw_hline(g_RenderData, {g_Input->screenSize.x / 2 - 5.f, g_Input->screenSize.y / 2}, 12.5f, 2.0f, {0.0f, 1.0f, 0.0f, 1.0f});
  ui_draw_vline(g_RenderData, {g_Input->screenSize.x / 2, g_Input->screenSize.y / 2 - 5.f}, 12.5f, 2.0f, {0.0f, 1.0f, 0.0f, 1.0f});

  char* todoText = bump_alloc(transientStorage, 128);
  snprintf(todoText, 128, "TODO(Nuno): Fix VSync");
  ui_draw_text(g_RenderData, todoText, {g_Input->screenSize.x / 4.f, g_Input->screenSize.y / 2 - 50.f}, {1.0f, 1.0f, 1.0f, 1.0f});
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
