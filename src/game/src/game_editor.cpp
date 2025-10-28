#include <game.h>

void game_update_editor(GameState* gameState, UIState* uiState, RenderData* renderData, Input* input, float dt) {

    ui_im_begin_frame(uiState, renderData, input);

    UIPanelOptions opts{};
    opts.dock = UIDock::Fill;
    opts.padding = 0.0f;
    opts.bgColor = {0.0f, 0.0f, 0.0f, 0.0f};
    ui_push_id(uiState, "Screen");
    ui_begin_panel(uiState, renderData, opts);

    // Lets start by drawing a 2D grid that fills the screen
    // We do this by getting percentages of the screen size for width/height and drawing hlines/vlines all across
    auto screenW    = input->screenSize.x;
    auto screenH    = input->screenSize.y;
    const float gridSpacing = uiState->editorState.gridSpacing * uiState->editorState.gridScale;
    const int numVLines = static_cast<int>(screenW / gridSpacing);
    const int numHLines = static_cast<int>(screenH / gridSpacing);
    for (int i = 0; i <= numVLines; i++) {
        float x = i * gridSpacing;
        ui_draw_vline(renderData, {x, 0.0f}, screenH, 1.f, {0.2f, 0.2f, 0.2f, 1.0f});
    }
    for (int j = 0; j <= numHLines; j++) {
        float y = j * gridSpacing;
        ui_draw_hline(renderData, {0.0f, y}, screenW, 1.f, {0.2f, 0.2f, 0.2f, 1.0f});
    }

    // Editor panel docked to left
    {
        UIPanelOptions opt{};
        opt.dock = UIDock::Left;
        opt.w = {260, UIUnit::Px};
        opt.padding = 6.0f;
        opt.bgColor = {0.2f, 0.2f, 0.2f, 0.1f};
        ui_push_id(uiState, "Editor");
        ui_begin_panel(uiState, renderData, opt);

        ui_label(uiState, renderData, "Editor Panel", {2.0f, 10.0f}, {1.0f, 1.0f, 1.0f, 1.0f}, 0);

        // Editor Controls
        {
            static char editorName[128] = "";
            //ui_label(g_UIState, g_RenderData, "Name", {2.0f, 10.0f}, {0.9f, 0.9f, 0.9f, 1.0f});
            ui_input_text(uiState, renderData, editorName, (int)sizeof(editorName), {180, 26}, "Type here...");

            ui_button(uiState, renderData, "Add Panel", {180, 26});
            ui_button(uiState, renderData, "Add Label", {180, 26});
            ui_button(uiState, renderData, "Add Button", {180, 26});
            if (ui_button(uiState, renderData, "Back", {180, 26})) {
                uiState->showEditor = false;
                gameState->m_State = GameStateId::GAME_STATE_IN_LEVEL;
                SM_TRACE("Back pressed")
            }
        }

        ui_end_panel(uiState);
        ui_pop_id(uiState);
    }


    ui_end_panel(uiState);
    ui_pop_id(uiState);

    ui_im_end_frame(uiState);
}