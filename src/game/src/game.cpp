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
static SoundState* g_SoundState;
static GameState* g_GameState;
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
    const auto jumpSound = "assets/jump_01.wav";
    const auto deathSound = "assets/died_01.wav";
    memcpy(g_GameState->m_JumpSound.path, jumpSound, strlen(jumpSound));
    memcpy(g_GameState->m_DeathSound.path, deathSound, strlen(deathSound));
    // Game Camera
    g_RenderData->gameCamera.aspectRatio = g_Input->screenSize.x / g_Input->screenSize.y;
    // UI Camera
    g_RenderData->uiCamera.dimensions.x = g_Input->screenSize.x;
    g_RenderData->uiCamera.dimensions.y = g_Input->screenSize.y;
    // Top Left is going to be 0/0 now
    g_RenderData->uiCamera.position.x = g_RenderData->uiCamera.dimensions.x / 2.0f;
    g_RenderData->uiCamera.position.y = -g_RenderData->uiCamera.dimensions.y / 2.0f;
  }

  if(!g_GameState->m_Initialized)
  {
    // Game Camera
    g_RenderData->gameCamera.position = {0.0, 2.5f, +5.0f};
    g_RenderData->gameCamera.rotation = {0.0f, 0.0f, 0.0f};
    g_RenderData->gameCamera.fov = glm::radians(80.0f);
    g_RenderData->gameCamera.aspectRatio = g_Input->screenSize.x / g_Input->screenSize.y;
    g_RenderData->gameCamera.nearClip = 0.1f;
    g_RenderData->gameCamera.farClip = 1000.0f;
    g_RenderData->gameCamera.invalidate();

    // 3D model transform default (identity)
    g_RenderData->modelMatrix3D = glm::mat4(1.0f);

    // UI Camera
    g_RenderData->uiCamera.dimensions.x = g_Input->screenSize.x;
    g_RenderData->uiCamera.dimensions.y = g_Input->screenSize.y;
    // Top Left is going to be 0/0 now
    g_RenderData->uiCamera.position.x = g_RenderData->uiCamera.dimensions.x / 2.0f;
    g_RenderData->uiCamera.position.y = -g_RenderData->uiCamera.dimensions.y / 2.0f;
    g_RenderData->uiCamera.zoom = 1.0f;

    g_GameState->m_Initialized = true;
  }

  g_RenderData->clearColor = {0.1f, 0.1f, 0.1f, 1.0f};

  g_GameState->m_UpdateTimer += frameTime;

  switch (g_GameState->m_State) {
    case GameStateId::GAME_STATE_MAIN_MENU:
      game_update_main_menu(gameStateIn, uiStateIn, renderDataIn, inputIn, frameTime);
      break;
    case GameStateId::GAME_STATE_IN_LEVEL:
      game_update_in_level(gameStateIn, uiStateIn, soundStateIn, renderDataIn, inputIn, transientStorageIn, persistentStorageIn->used, lastFrameAllocationBytes, frameTime);
      break;
    case GameStateId::GAME_STATE_EDITOR:
      game_update_editor(gameStateIn, uiStateIn, renderDataIn, inputIn, frameTime);
      break;
    default:
      SM_ASSERT(false, "Unknown Game State!");
      break;
  }

  // Reset Input
  g_Input->relMouse = {};
  for(auto & key : g_Input->keys)
  {
    key.justReleased = false;
    key.justPressed = false;
    key.halfTransitionCount = 0;
  }
  // float interpolatedDT = (float)(g_GameState->updateTimer / UPDATE_DELAY);
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
  if (key_released_this_frame(g_Input, KEY_TAB)) {
    play_sound(g_SoundState, g_GameState->m_JumpSound);
  }

  if (key_released_this_frame(g_Input, KEY_1)) {
    g_UIState->showDiagnostics = !g_UIState->showDiagnostics;
  }
  if (key_released_this_frame(g_Input, KEY_2)) {
    g_UIState->showHUD = !g_UIState->showHUD;
  }
  if (key_released_this_frame(g_Input, KEY_3)) {
    g_UIState->showEditor = !g_UIState->showEditor;
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
    fps = g_GameState->m_Fps;
    frameTimeMs = g_GameState->m_FrameTime;
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
        static char editorName[128] = "";
        //ui_label(g_UIState, g_RenderData, "Name", {2.0f, 10.0f}, {0.9f, 0.9f, 0.9f, 1.0f});
        ui_input_text(g_UIState, g_RenderData, editorName, (int)sizeof(editorName), {180, 26}, "Type here...");

        ui_button(g_UIState, g_RenderData, "Add Panel", {180, 26});
        ui_button(g_UIState, g_RenderData, "Add Label", {180, 26});
        ui_button(g_UIState, g_RenderData, "Add Button", {180, 26});
      }

      ui_end_panel(g_UIState);
      ui_pop_id(g_UIState);
    }



    ui_end_panel(g_UIState);
    ui_pop_id(g_UIState);
  }

  // Crosshair and a random note label using low-level helpers (still valid)
  ui_draw_hline(g_RenderData, {g_Input->screenSize.x / 2 - 5.f, g_Input->screenSize.y / 2}, 12.5f, 2.0f, {0.0f, 1.0f, 0.0f, 1.0f});
  ui_draw_vline(g_RenderData, {g_Input->screenSize.x / 2, g_Input->screenSize.y / 2 - 5.f}, 12.5f, 2.0f, {0.0f, 1.0f, 0.0f, 1.0f});

  char* todoText = bump_alloc(transientStorage, 128);
  snprintf(todoText, 128, "TODO(Nuno): Fix VSync, Press TAB to play sound, WASD+QE+SPACE/SHIFT to fly, RMB+Mouse to look");
  ui_draw_text_ex(g_RenderData, todoText, {g_Input->screenSize.x / 4.f, g_Input->screenSize.y / 2 - 50.f}, {1.0f, 1.0f, 1.0f, 1.0f}, 1);
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
