#include "MainMenuBar.h"
#include "EditorContext.h"

#include <imgui.h>
#include "ApplicationContext.h"
#include "WorldManager.h"
#include "SettingsManager.h"

MainMenuBarResult MainMenuBar::Draw(const EditorContext& ctx, bool& editMode)
{
    MainMenuBarResult result;
    const double now = ImGui::GetTime();

    // Shared action bodies for the File menu items and their Ctrl shortcuts.
    auto doNew = [&] {
        ctx.App->RequestSceneNew.store(true, std::memory_order_relaxed);
        result.sceneChanged = true;
        m_SceneStatus.Set("New scene", false, now);
    };
    auto doReload = [&] {
        ctx.App->RequestSceneReload.store(true, std::memory_order_relaxed);
        result.sceneChanged = true;
        m_SceneStatus.Set("Reloading world.json", false, now);
    };
    auto doSave = [&] {
        const bool ok = ctx.WorldSnapshot &&
            WorldManager::SaveWorldSnapshot(WorldManager::DEFAULT_WORLD_SNAPSHOT_PATH, ctx.WorldSnapshot.get());
        m_SceneStatus.Set(ok ? "Saved world.json" : "Save failed", !ok, now);
    };

    // Main Menu Bar (File / Edit / About) with placeholder items
    if (ImGui::BeginMainMenuBar())
    {
        if (ImGui::BeginMenu("File"))
        {
            if (ImGui::MenuItem("New", "Ctrl+N"))    { doNew(); }
            if (ImGui::MenuItem("Reload", "Ctrl+R")) { doReload(); }
            ImGui::Separator();
            if (ImGui::MenuItem("Save", "Ctrl+S"))   { doSave(); }
            ImGui::Separator();
            if (ImGui::MenuItem("Exit")) { /* no-op */ }
            ImGui::EndMenu();
        }

        if (ImGui::BeginMenu("Edit"))
        {
            if (ImGui::MenuItem("Undo", "Ctrl+Z")) { /* no-op */ }
            if (ImGui::MenuItem("Redo", "Ctrl+Y")) { /* no-op */ }
            ImGui::Separator();
            if (ImGui::MenuItem("Cut", "Ctrl+X")) { /* no-op */ }
            if (ImGui::MenuItem("Copy", "Ctrl+C")) { /* no-op */ }
            if (ImGui::MenuItem("Paste", "Ctrl+V")) { /* no-op */ }
            if (ImGui::MenuItem("Delete", "Del")) { /* no-op */ }
            ImGui::Separator();
            if (ImGui::MenuItem("Select All", "Ctrl+A")) { /* no-op */ }
            ImGui::EndMenu();
        }

        if (ImGui::BeginMenu("About"))
        {
            if (ImGui::MenuItem("About This App...")) { /* no-op */ }
            if (ImGui::MenuItem("Check for Updates")) { /* no-op */ }
            if (ImGui::MenuItem("Credits")) { /* no-op */ }
            ImGui::EndMenu();
        }

        if (ImGui::BeginMenu("Settings"))
        {
            ImGui::TextDisabled("Renderer");
            ImGui::Separator();

            // Lazy-initialize pending choice from the current persisted setting.
            if (!m_PendingBackendInitialized) {
                m_PendingBackend = ctx.App->Settings.Backend;
                m_PendingBackendInitialized = true;
            }

            const char* current = SettingsManager::BackendToString(m_PendingBackend);
            if (ImGui::BeginCombo("Backend", current))
            {
                if (ImGui::Selectable("directx12", m_PendingBackend == RendererAPI::DirectX12)) {
                    m_PendingBackend = RendererAPI::DirectX12;
                }
                if (ImGui::Selectable("vulkan", m_PendingBackend == RendererAPI::Vulkan)) {
                    m_PendingBackend = RendererAPI::Vulkan;
                }

                // DirectX 11 — disabled; backend not implemented.
                ImGui::BeginDisabled(true);
                ImGui::Selectable("directx11 (not implemented)", false);
                ImGui::EndDisabled();
                if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) {
                    ImGui::SetTooltip("DirectX 11 backend not implemented yet.");
                }

                ImGui::EndCombo();
            }

            const bool dirty = (m_PendingBackend != ctx.App->Settings.Backend);
            ImGui::BeginDisabled(!dirty);
            if (ImGui::Button("Apply##SettingsBackendApply"))
            {
                const RendererAPI previous = ctx.App->Settings.Backend;
                ctx.App->Settings.Backend = m_PendingBackend;
                if (SettingsManager::Save(SettingsManager::DEFAULT_SETTINGS_PATH,
                                          ctx.App->Settings))
                {
                    m_SettingsSaveError.clear();
                    RendererCommand swapCmd{};
                    swapCmd.Type = RendererCommandType::SwapBackend;
                    swapCmd.SwapBackend.TargetApi = m_PendingBackend;
                    if (!ctx.App->PRCommandRing.Push(swapCmd)) {
                        m_SettingsSaveError = "Renderer busy; could not start swap. Restart to apply.";
                    }
                }
                else
                {
                    ctx.App->Settings.Backend = previous;
                    m_SettingsSaveError = "Failed to save editor_settings.json";
                }
            }
            ImGui::EndDisabled();

            if (!m_SettingsSaveError.empty()) {
                ImGui::TextColored(ImVec4(1.0f, 0.4f, 0.4f, 1.0f),
                                   "%s", m_SettingsSaveError.c_str());
            }

            ImGui::EndMenu();
        }

        if (ImGui::BeginMenu("View"))
        {
            if (ImGui::MenuItem("Reset Layout")) { result.resetLayout = true; }
            ImGui::EndMenu();
        }

        // Edit/Play mode toggle (also bound to F6 in ImGuiRenderer). Edit = free editor camera;
        // Play = the game's camera drives the viewport.
        {
            const char* label = editMode ? "Mode: Edit" : "Mode: Play";
            const float bw = ImGui::CalcTextSize(label).x + ImGui::GetStyle().FramePadding.x * 2.0f;
            ImGui::SameLine(ImGui::GetWindowWidth() - bw - 12.0f);
            if (ImGui::Button(label))
                editMode = !editMode;
        }

        ImGui::EndMainMenuBar();
    }

    // Ctrl+N / Ctrl+R / Ctrl+S work without the menu open (gated so they don't fire while typing).
    if (!ImGui::GetIO().WantTextInput) {
        if (ImGui::IsKeyChordPressed(ImGuiMod_Ctrl | ImGuiKey_N)) doNew();
        if (ImGui::IsKeyChordPressed(ImGuiMod_Ctrl | ImGuiKey_R)) doReload();
        if (ImGui::IsKeyChordPressed(ImGuiMod_Ctrl | ImGuiKey_S)) doSave();
    }

    // Transient feedback toast: borderless, non-interactive, bottom-left of the main viewport.
    if (m_SceneStatus.Visible(now)) {
        const ImGuiViewport* vp = ImGui::GetMainViewport();
        ImGui::SetNextWindowPos(ImVec2(vp->WorkPos.x + 12.0f, vp->WorkPos.y + vp->WorkSize.y - 36.0f));
        ImGui::SetNextWindowBgAlpha(0.85f);
        const ImGuiWindowFlags f = ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoInputs
            | ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoFocusOnAppearing
            | ImGuiWindowFlags_NoNav | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoSavedSettings;
        if (ImGui::Begin("##SceneStatusToast", nullptr, f)) {
            const ImVec4 col = m_SceneStatus.IsError() ? ImVec4(1.0f, 0.4f, 0.4f, 1.0f)
                                                       : ImVec4(0.5f, 1.0f, 0.5f, 1.0f);
            ImGui::TextColored(col, "%s", m_SceneStatus.Text().c_str());
        }
        ImGui::End();
    }

    return result;
}
