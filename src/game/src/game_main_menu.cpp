#include <game.h>

void update_input(GameState* gameState, Input* input, float dt);

void game_update_main_menu(GameState* gameState, UIState* uiState, RenderData* renderData, Input* input, const float dt) {
    update_input(gameState, input, dt);

    ui_im_begin_frame(uiState, renderData, input);

    UIPanelOptions opts{};
    opts.dock = UIDock::Fill;
    opts.padding = 0.0f;
    opts.bgColor = {0.0f, 0.0f, 0.0f, 0.0f};
    ui_push_id(uiState, "Screen");
    ui_begin_panel(uiState, renderData, opts);

    {
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
            gameState->m_State = GameStateId::GAME_STATE_IN_LEVEL;
        }

        if (ui_button(uiState, renderData, "Quit", {210, 26})) {
            gameState->m_QuitRequested = true;
        }

        ui_end_panel(uiState);
        ui_pop_id(uiState);
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