#pragma once
#include "lib.h"   // RendererAPI
#include "TransientStatus.h"
#include <string>
struct EditorContext;

// What the host should react to after drawing the menu bar this frame.
struct MainMenuBarResult {
    bool resetLayout  = false; // "View -> Reset Layout" clicked
    bool sceneChanged = false; // New or Reload issued -> host clears the (now stale) selection
};

// Draws the main menu bar (File/Edit/About/Settings/View). Returns a MainMenuBarResult: resetLayout
// when "View -> Reset Layout" was clicked (host rebuilds the dock layout), and sceneChanged when
// New/Reload was issued (host clears the now-stale selection).
class MainMenuBar {
public:
    MainMenuBarResult Draw(const EditorContext& ctx, bool& editMode);
private:
    // Settings menu state — pending backend selection until user clicks Apply.
    // Initialized lazily on first menu open from ctx.App->Settings.Backend.
    RendererAPI m_PendingBackend = RendererAPI::Invalid;
    bool        m_PendingBackendInitialized = false;
    std::string m_SettingsSaveError;          // empty when no error
    TransientStatus m_SceneStatus;            // Save/New/Reload feedback toast
};
