#pragma once
#include "lib.h"   // RendererAPI
#include <string>
struct EditorContext;

// Draws the main menu bar (File/Edit/About/Settings/View). Returns true if "View -> Reset Layout"
// was clicked this frame, so the host rebuilds the dock layout.
class MainMenuBar {
public:
    bool Draw(const EditorContext& ctx);
private:
    // Settings menu state — pending backend selection until user clicks Apply.
    // Initialized lazily on first menu open from ctx.App->Settings.Backend.
    RendererAPI m_PendingBackend = RendererAPI::Invalid;
    bool        m_PendingBackendInitialized = false;
    std::string m_SettingsSaveError;          // empty when no error
};
