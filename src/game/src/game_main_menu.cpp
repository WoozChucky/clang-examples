#include <game.h>

void update_input(GameState* gameState, Input* input, float dt);

void game_update_main_menu(GameState* gameState, UIState* uiState, RenderData* renderData, Input* input, const float dt) {
    update_input(gameState, input, dt);

    ui_im_begin_frame(uiState, renderData, input);

    // Static loading state for the main menu session
    static bool s_Loading = false;
    static float s_Progress = 0.0f;

    UIPanelOptions opts{};
    opts.dock = UIDock::Fill;
    opts.padding = 0.0f;
    opts.bgColor = {0.0f, 0.0f, 0.0f, 0.0f};
    ui_push_id(uiState, "Screen");
    ui_begin_panel(uiState, renderData, opts);

    if (!s_Loading)
    {
        // Centered controls panel
        UIPanelOptions opt{};
        opt.dock = UIDock::None;
        opt.x = {0.5f, UIUnit::Percent};
        opt.y = {0.5f, UIUnit::Percent};
        opt.w = {220, UIUnit::Px};
        opt.h = {80, UIUnit::Px};
        opt.padding = 6.0f;
        opt.bgColor = {0.1f, 0.1f, 0.75f, 0.5f};
        ui_push_id(uiState, "Controls");
        ui_begin_panel(uiState, renderData, opt);

        if (ui_button(uiState, renderData, "Start", {210, 26})) {
            s_Loading = true;
            s_Progress = 0.0f;
        }

        if (ui_button(uiState, renderData, "Quit", {210, 26})) {
            gameState->m_QuitRequested = true;
        }

        ui_end_panel(uiState);
        ui_pop_id(uiState);
    }
    else
    {
        // Simulate loading progress and draw a progress bar
        const float speed = 0.35f; // fraction per second
        s_Progress = glm::clamp(s_Progress + speed * dt, 0.0f, 1.0f);

        UIPanelOptions loading{};
        loading.dock = UIDock::None;
        loading.x = {0.4f, UIUnit::Percent};
        loading.y = {0.5f, UIUnit::Percent};
        loading.w = {420, UIUnit::Px};
        loading.h = {100, UIUnit::Px};
        loading.padding = 10.0f;
        loading.bgColor = {0.08f, 0.08f, 0.08f, 0.8f};
        ui_push_id(uiState, "LoadingPanel");
        ui_begin_panel(uiState, renderData, loading);

        // Title
        ui_label_flow(uiState, renderData, "Loading...", {0, 0}, {1,1,1,1});

        // Progress bar (full width minus padding)
        ui_progress_bar(uiState, renderData, s_Progress, {400, 18});

        // Optional percent text below the bar
        char pct[32];
        int percent = (int)glm::round(s_Progress * 100.0f);
        snprintf(pct, sizeof(pct), "%d%%", percent);
        ui_label_flow(uiState, renderData, pct, {0, 2}, {0.9f,0.9f,0.9f,1});

        ui_end_panel(uiState);
        ui_pop_id(uiState);

        if (s_Progress >= 1.0f)
        {
            s_Loading = false;
            gameState->m_State = GameStateId::GAME_STATE_IN_LEVEL;
        }
    }

    ui_end_panel(uiState);
    ui_pop_id(uiState);

    ui_im_end_frame(uiState);
}

void update_input(GameState* gameState, Input* input, float dt) {
    if (key_released_this_frame(input, KEY_ESCAPE)) {
        gameState->m_QuitRequested = true;
        return;
    }
}