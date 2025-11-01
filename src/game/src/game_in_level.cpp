#include <game.h>

void update_input(const GameState* gameState, SoundState* soundState, UIState* uiState, RenderData* renderData, const Input* input, float dt);

void game_update_in_level(GameState* gameState, UIState* uiState, SoundState* soundState, RenderData* renderData, Input* input,
    BumpAllocator* frameAllocator, size_t persistentStorageAllocated, size_t frameStorageAllocated, float dt) {

    update_input(gameState, soundState, uiState, renderData, input, dt);

    static float diagnosticsTimer = 0.0f;
    static float fps = 0.0f;
    static float cpuFrameTimeMs = 0.0f;
    static float gpuFrameTimeMs = 0.0f;
    static uint64_t frameCycles = 0;
    static uint64_t gameUpdateCycles = 0;
    static uint64_t renderCycles = 0;
    static size_t persistentMemoryUsed = 0;
    static size_t frameMemoryUsed = 0;
    diagnosticsTimer += dt;
    if (diagnosticsTimer >= 0.25f) {
        diagnosticsTimer = 0.0f;
        fps = gameState->m_Fps;
        cpuFrameTimeMs = gameState->m_FrameTime;
        gpuFrameTimeMs = gameState->m_GpuTime;
        frameCycles = gameState->m_FrameCycles;
        gameUpdateCycles = gameState->m_UpdateGameCycles;
        renderCycles = gameState->m_RenderCycles;
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
            opt.h = {260, UIUnit::Px};
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
            ui_label(uiState, renderData, fpsText, {2.0f, 10.0f}, {0.0f, 1.0f, 0.0f, 1.0f}, 1);

            char* cpuFrameText = bump_alloc(frameAllocator, 64);
            snprintf(cpuFrameText, 64, "CPU Frame: %.3f ms", cpuFrameTimeMs);
            ui_label(uiState, renderData, cpuFrameText, {2.0f, 40.0f}, {1.0f, 1.0f, 0.0f, 1.0f}, 1);

            char* gpuFrameText = bump_alloc(frameAllocator, 64);
            snprintf(gpuFrameText, 64, "GPU Frame: %.3f ms", gpuFrameTimeMs);
            ui_label(uiState, renderData, gpuFrameText, {2.0f, 55.0f}, {1.0f, 1.0f, 0.0f, 1.0f}, 1);

            char* frameCyclesText = bump_alloc(frameAllocator, 64);
            snprintf(frameCyclesText, 64, "Frame Cycles: %llu", frameCycles);
            ui_label(uiState, renderData, frameCyclesText, {2.0f, 85.0f}, {1.0f, 1.0f, 0.0f, 1.0f}, 1);

            char* gameUpdateCyclesText = bump_alloc(frameAllocator, 64);
            snprintf(gameUpdateCyclesText, 64, "Game Update Cycles: %llu", gameUpdateCycles);
            ui_label(uiState, renderData, gameUpdateCyclesText, {8.0f, 100.0f}, {1.0f, 1.0f, 0.0f, 1.0f}, 1);

            char* renderCyclesText = bump_alloc(frameAllocator, 64);
            snprintf(renderCyclesText, 64, "Render Cycles: %llu", renderCycles);
            ui_label(uiState, renderData, renderCyclesText, {8.0f, 115.0f}, {1.0f, 1.0f, 0.0f, 1.0f}, 1);

            char* persistentMemText = bump_alloc(frameAllocator, 64);
            const double persistentMB = static_cast<double>(persistentMemoryUsed) / (1024.0 * 1024.0);
            snprintf(persistentMemText, 64, "Persistent Mem: %.2f MB", persistentMB);
            ui_label(uiState, renderData, persistentMemText, {2.0f, 145.0f}, {1.0f, 1.0f, 0.0f, 1.0f}, 1);

            char* transientMemText = bump_alloc(frameAllocator, 64);
            const double frameMB = static_cast<double>(frameMemoryUsed) / (1024.0 * 1024.0);
            snprintf(transientMemText, 64, "Frame Mem: %.2f MB", frameMB);
            ui_label(uiState, renderData, transientMemText, {2.0f, 160.0f}, {1.0f, 1.0f, 0.0f, 1.0f}, 1);

            char* screenSizeText = bump_alloc(frameAllocator, 64);
            snprintf(screenSizeText, 64, "Resolution: %d x %d", static_cast<int>(input->screenSize.x), static_cast<int>(input->screenSize.y));
            ui_label(uiState, renderData, screenSizeText, {2.0f, 175.0f}, {1.0f, 1.0f, 0.0f, 1.0f}, 1);

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
    snprintf(todoText, 128, "Press TAB to play sound, WASD+QE+SPACE/SHIFT to fly, RMB+Mouse to look");
    ui_draw_text_ex(renderData, todoText, {input->screenSize.x / 4.f, input->screenSize.y / 2 - 50.f}, {1.0f, 1.0f, 1.0f, 1.0f}, 1);

    {
        // Block that will continously move the model position over time
        // The general idea with first it will move the model to an X target position, then to a Y position, then back to X, and so on
        static float animationTimer = 0.0f;
        static bool moveToX = true;
        static bool moveToPositive = true;
        constexpr float animationDuration = 0.25f; // seconds to move to target
        animationTimer += dt;
        if (animationTimer >= animationDuration) {
            animationTimer = 0.0f;
            if (moveToX) {
                // Switch to move to Y next
                moveToX = false;
            } else {
                // Switch to move to X next
                moveToX = true;
                // Also flip direction
                moveToPositive = !moveToPositive;
            }
        }
        float t = animationTimer / animationDuration;
        t = ease_in_out_qubic(t);
        if (moveToX) {
            float targetX = moveToPositive ? 5.0f : -5.0f;
            renderData->modelPosition.x = lerp(renderData->modelPosition.x, targetX, t);
        } else {
            float targetY = moveToPositive ? 5.0f : -5.0f;
            renderData->modelPosition.y = lerp(renderData->modelPosition.y, targetY, t);
        }
    }
}

void update_input(const GameState* gameState, SoundState* soundState, UIState* uiState, RenderData* renderData, const Input* input, const float dt) {
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

    // Check if M1 clicking on modelPosition
    if (key_pressed_this_frame(input, KEY_MOUSE_LEFT)) {
        // Build a picking ray from the mouse position (DirectX clip space: Z in [0,1])
        const float screenW = input->screenSize.x;
        const float screenH = input->screenSize.y;

        // If you have absolute mouse pixels in Input, prefer that (e.g., input->mousePos)
        // Otherwise you can use uiState->mousePosUI (it’s in pixel space too)
        const glm::vec2 mousePx = uiState->mousePosUI; // or input->mousePos if you have it

        // Convert to NDC in DX (X:[-1,1], Y:[-1,1] with top-left pixel origin)
        const float xNdc = (mousePx.x / screenW) * 2.0f - 1.0f;
        const float yNdc = 1.0f - (mousePx.y / screenH) * 2.0f; // flip Y

        // Acquire View and Projection matrices for your game camera
        // Replace these with your actual accessors:
        const glm::mat4 view = renderData->gameCamera.get_view_matrix();
        const glm::mat4 proj = renderData->gameCamera.get_projection_matrix(); // use RH_ZO (e.g., glm::perspectiveRH_ZO)
        const glm::mat4 invViewProj = glm::inverse(proj * view);

        // Near and Far clip-space points (DX: z=0 near, z=1 far)
        const glm::vec4 nearClip(xNdc, yNdc, 0.0f, 1.0f);
        const glm::vec4 farClip (xNdc, yNdc, 1.0f, 1.0f);

        // Unproject to world space
        glm::vec4 nearWorld4 = invViewProj * nearClip;
        glm::vec4 farWorld4  = invViewProj * farClip;
        nearWorld4 /= nearWorld4.w;
        farWorld4  /= farWorld4.w;

        const glm::vec3 rayOrigin = glm::vec3(nearWorld4);
        const glm::vec3 rayDir    = glm::normalize(glm::vec3(farWorld4 - nearWorld4));

        const glm::vec3 boxMin = renderData->modelPosition - glm::vec3(0.5f);
        const glm::vec3 boxMax = renderData->modelPosition + glm::vec3(0.5f);

        float tMin = 0.0f;
        float tMax = 1000.0f; // some large value

        bool hit = true;
        for (int i = 0; i < 3; ++i) {
            if (fabs(rayDir[i]) < 1e-6f) {
                if (rayOrigin[i] < boxMin[i] || rayOrigin[i] > boxMax[i]) { hit = false; break; }
            } else {
                float ood = 1.0f / rayDir[i];
                float t1 = (boxMin[i] - rayOrigin[i]) * ood;
                float t2 = (boxMax[i] - rayOrigin[i]) * ood;
                if (t1 > t2) std::swap(t1, t2);
                tMin = std::max(tMin, t1);
                tMax = std::min(tMax, t2);
                if (tMin > tMax) { hit = false; break; }
            }
        }
        if (hit && tMax >= 0.0f) {
            play_sound(soundState, gameState->m_DeathSound);
        }
    }
}