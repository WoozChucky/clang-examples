#include <game.h>

void update_input(const GameState* gameState, SoundState* soundState, RenderData* renderData, const Input* input, float dt);

void game_update_in_level(GameState* gameState, UIState* uiState, SoundState* soundState, RenderData* renderData, Input* input,
    BumpAllocator* frameAllocator, size_t persistentStorageAllocated, size_t frameStorageAllocated, float dt) {

    update_input(gameState, soundState, renderData, input, dt);

    static float diagnosticsTimer = 0.0f;
    static float fps = 0.0f;
    static float frameTimeMs = 0.0f;
    static size_t persistentMemoryUsed = 0;
    static size_t frameMemoryUsed = 0;
    diagnosticsTimer += dt;
    if (diagnosticsTimer >= 0.25f) {
        diagnosticsTimer = 0.0f;
        fps = gameState->m_Fps;
        frameTimeMs = gameState->m_FrameTime;
        persistentMemoryUsed = persistentStorageAllocated;
        frameMemoryUsed = frameStorageAllocated;
    }

    prim_draw_grid_plane(renderData, glm::mat4(1.0f));

    ui_im_begin_frame(uiState, renderData, input);

    UIPanelOptions opts{};
    opts.dock = UIDock::Fill;
    opts.padding = 0.0f;
    opts.bgColor = {0.0f, 0.0f, 0.0f, 0.0f};
    ui_push_id(uiState, "Screen");
    ui_begin_panel(uiState, renderData, opts);

    {
        UIPanelOptions opt{};
        opt.dock = UIDock::Right;
        opt.x = {10, UIUnit::Px};
        opt.y = {10, UIUnit::Px};
        opt.w = {220, UIUnit::Px};
        opt.h = {80, UIUnit::Px};
        opt.padding = 6.0f;
        opt.bgColor = {0.1f, 0.1f, 0.1f, 0.0f};
        ui_push_id(uiState, "Controls");
        ui_begin_panel(uiState, renderData, opt);


        if (ui_button(uiState, renderData, uiState->showDiagnostics ? "Hide Diagnostics" : "Show Diagnostics", {210, 26})) {
            uiState->showDiagnostics = !uiState->showDiagnostics;
        }

        if (ui_button(uiState, renderData, uiState->showHUD ? "Hide HUD" : "Show HUD", {210, 26})) {
            uiState->showHUD = !uiState->showHUD;
        }

        if (ui_button(uiState, renderData, uiState->showEditor ? "Hide Editor" : "Show Editor", {210, 26})) {
            uiState->showEditor = !uiState->showEditor;
            gameState->m_State = uiState->showEditor ? GameStateId::GAME_STATE_EDITOR : GameStateId::GAME_STATE_IN_LEVEL;
        }

        if (ui_button(uiState, renderData, "Menu", {210, 26})) {
            gameState->m_State = GameStateId::GAME_STATE_MAIN_MENU;
        }

        if (ui_button(uiState, renderData, "Quit", {210, 26})) {
            gameState->m_QuitRequested = true;
        }

        ui_end_panel(uiState);
        ui_pop_id(uiState);
    }

    {
        // Diagnostics panel docked to top (uses percentage+px sizing)
        if (uiState->showDiagnostics) {
          UIPanelOptions opt{};
          opt.dock = UIDock::None;
          opt.x = {10, UIUnit::Px};
          opt.y = {10, UIUnit::Px};
          opt.h = {160, UIUnit::Px};
          opt.w = {340, UIUnit::Px};
          opt.padding = 6.0f;
          opt.bgColor = {0.1f, 0.1f, 0.1f, 0.1f};
          PanelContext pc = ui_begin_panel(uiState, renderData, opt);

          // Hover effect overlay
          const bool hovered = (uiState->mousePosUI.x >= pc.panelRect.pos.x && uiState->mousePosUI.x <= pc.panelRect.pos.x + pc.panelRect.size.x &&
                                uiState->mousePosUI.y >= pc.panelRect.pos.y && uiState->mousePosUI.y <= pc.panelRect.pos.y + pc.panelRect.size.y);
          if (hovered) {
            ui_draw_rect(renderData, pc.panelRect.pos, pc.panelRect.size, {1.0f, 1.0f, 1.0f, 0.20f});
          }

          // Text lines inside content area
          char* fpsText = bump_alloc(frameAllocator, 64);
          snprintf(fpsText, 64, "FPS: %.0f", fps);
          ui_label(uiState, renderData, fpsText, {2.0f, 10.0f}, {0.0f, 1.0f, 0.0f, 1.0f});

          char* frameTimeText = bump_alloc(frameAllocator, 64);
          snprintf(frameTimeText, 64, "Frame: %.3f ms", frameTimeMs);
          ui_label(uiState, renderData, frameTimeText, {2.0f, 25.0f}, {1.0f, 1.0f, 0.0f, 1.0f});

          char* persistentMemText = bump_alloc(frameAllocator, 64);
          const double persistentMB = static_cast<double>(persistentMemoryUsed) / (1024.0 * 1024.0);
          snprintf(persistentMemText, 64, "Persistent Mem: %.2f MB", persistentMB);
          ui_label(uiState, renderData, persistentMemText, {2.0f, 40.0f}, {1.0f, 1.0f, 0.0f, 1.0f});

          char* transientMemText = bump_alloc(frameAllocator, 64);
          const double frameMB = static_cast<double>(frameMemoryUsed) / (1024.0 * 1024.0);
          snprintf(transientMemText, 64, "Frame Mem: %.2f MB", frameMB);
          ui_label(uiState, renderData, transientMemText, {2.0f, 55.0f}, {1.0f, 1.0f, 0.0f, 1.0f});

          char* screenSizeText = bump_alloc(frameAllocator, 64);
          snprintf(screenSizeText, 64, "Resolution: %d x %d", static_cast<int>(input->screenSize.x), static_cast<int>(input->screenSize.y));
          ui_label(uiState, renderData, screenSizeText, {2.0f, 70.0f}, {1.0f, 1.0f, 0.0f, 1.0f});

          ui_end_panel(uiState);
        }
    }

    {
        // HUD panel docked to bottom
        if (uiState->showHUD) {
            UIPanelOptions opt{};
            opt.dock = UIDock::Bottom;
            opt.h = {40, UIUnit::Px};
            opt.padding = 6.0f;
            ui_begin_panel(uiState, renderData, opt);

            char* positionText = bump_alloc(frameAllocator, 128);
            snprintf(positionText, 128, "Camera Pos: (%.2f, %.2f, %.2f)", renderData->gameCamera.position.x, renderData->gameCamera.position.y, renderData->gameCamera.position.z);
            ui_label(uiState, renderData, positionText, {10.0f, 8.0f}, {1.0f, 1.0f, 1.0f, 1.0f});

            char* rotationText = bump_alloc(frameAllocator, 128);
            snprintf(rotationText, 128, "Camera Rot: (%.2f, %.2f, %.2f)", glm::degrees(renderData->gameCamera.rotation.x), glm::degrees(renderData->gameCamera.rotation.y), glm::degrees(renderData->gameCamera.rotation.z));
            ui_label(uiState, renderData, rotationText, {10.0f, 24.0f}, {1.0f, 1.0f, 1.0f, 1.0f});

            ui_end_panel(uiState);
        }
    }

    ui_end_panel(uiState);
    ui_pop_id(uiState);

    ui_im_end_frame(uiState);

    // Crosshair and a random note label using low-level helpers (still valid)
    ui_draw_hline(renderData, {input->screenSize.x / 2 - 5.f, input->screenSize.y / 2}, 12.5f, 2.0f, {0.0f, 1.0f, 0.0f, 1.0f});
    ui_draw_vline(renderData, {input->screenSize.x / 2, input->screenSize.y / 2 - 5.f}, 12.5f, 2.0f, {0.0f, 1.0f, 0.0f, 1.0f});

    char* todoText = bump_alloc(frameAllocator, 128);
    snprintf(todoText, 128, "TODO(Nuno): Fix VSync, Press TAB to play sound, WASD+QE+SPACE/SHIFT to fly, RMB+Mouse to look");
    ui_draw_text_ex(renderData, todoText, {input->screenSize.x / 4.f, input->screenSize.y / 2 - 50.f}, {1.0f, 1.0f, 1.0f, 1.0f}, 1);
}

void update_input(const GameState* gameState, SoundState* soundState, RenderData* renderData, const Input* input, const float dt) {
    if (key_released_this_frame(input, KEY_TAB)) {
        play_sound(soundState, gameState->m_JumpSound);
    }

    const glm::vec3 rot= renderData->gameCamera.rotation; // pitch(x), yaw(y), roll(z)
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
    if (key_is_down(input, KEY_W)) { renderData->gameCamera.position += forward * (moveSpeed * dt); renderData->gameCamera.invalidate(); }
    if (key_is_down(input, KEY_S)) { renderData->gameCamera.position -= forward * (moveSpeed * dt); renderData->gameCamera.invalidate(); }
    if (key_is_down(input, KEY_A)) { renderData->gameCamera.position -= right   * (moveSpeed * dt); renderData->gameCamera.invalidate(); }
    if (key_is_down(input, KEY_D)) { renderData->gameCamera.position += right   * (moveSpeed * dt); renderData->gameCamera.invalidate(); }

    // For up/down you can keep world y or derive from a camera up vector
    if (key_is_down(input, KEY_SPACE)) { renderData->gameCamera.position.y += moveSpeed * dt; renderData->gameCamera.invalidate(); }
    if (key_is_down(input, KEY_SHIFT)) { renderData->gameCamera.position.y -= moveSpeed * dt; renderData->gameCamera.invalidate(); }

    constexpr float yawSpeed = glm::radians(120.0f);
    auto& yaw = renderData->gameCamera.rotation.y;
    if (key_is_down(input, KEY_Q)) { yaw += yawSpeed * dt; renderData->gameCamera.invalidate(); }
    if (key_is_down(input, KEY_E)) { yaw -= yawSpeed * dt; renderData->gameCamera.invalidate(); }

    // Mouse look when holding right mouse button
    const float mouseSensitivity = 0.0025f;
    if (key_is_down(input, KEY_MOUSE_RIGHT)) {
        // Apply a small deadzone and axis-dominance filter to prevent cross-axis jitter
        int dx = input->relMouse.x;
        int dy = input->relMouse.y;

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

        renderData->gameCamera.rotation.y -= static_cast<float>(dx) * mouseSensitivity; // yaw
        renderData->gameCamera.rotation.x -= static_cast<float>(dy) * mouseSensitivity; // pitch
        // Clamp pitch to avoid flipping
        constexpr float pitchLimit = glm::radians(89.0f);
        if (renderData->gameCamera.rotation.x > pitchLimit) renderData->gameCamera.rotation.x = pitchLimit;
        if (renderData->gameCamera.rotation.x < -pitchLimit) renderData->gameCamera.rotation.x = -pitchLimit;
        renderData->gameCamera.invalidate();
    }
}