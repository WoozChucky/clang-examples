#include "AnimatorEditor.h"
#include "EditorContext.h"
#include <imgui.h>
#include <string>
#include "ApplicationContext.h"
#include "ECSCommands.h"
#include "animation/AnimatorControllerStore.h"
#include "AssetKey.h"
#include "lib.h"

void AnimatorEditor::AddDefault(const EditorContext& ctx, EntityId e) {
    ECSCommand addCmd = ECSCommand::AddComponent(e, AnimatorComponent{});
    if (!ctx.App->ECSCommandRing.Push(addCmd))
        SM_WARN("ECS command queue full! Add component command dropped.");
}
void AnimatorEditor::Remove(const EditorContext& ctx, EntityId e) {
    ECSCommand removeCmd = ECSCommand::RemoveComponent<AnimatorComponent>(e);
    if (!ctx.App->ECSCommandRing.Push(removeCmd))
        SM_WARN("ECS command queue full! Remove component command dropped.");
}
void AnimatorEditor::DrawEditor(const EditorContext& ctx, EntityId e) {
    const AnimatorComponent* live = m_St.Begin(ctx, e);
    if (!live) return;

    // Controller picker (list built only while the combo is open).
    std::string current = AnimatorControllerStore::Instance().KeyForHandle(m_St.edit.ControllerId);
    if (current.empty()) current = "(none)";
    if (ImGui::BeginCombo("Controller", current.c_str())) {
        const auto list = AnimatorControllerStore::Instance().GetAssetList();
        if (list.empty()) ImGui::TextDisabled("No controllers loaded");
        for (const auto& [handle, key] : list) {
            const bool sel = (handle == m_St.edit.ControllerId);
            const char* label = key.empty() ? "(none)" : key.c_str();
            if (ImGui::Selectable(label, sel) && handle != m_St.edit.ControllerId) {
                m_St.edit.ControllerId = handle;
                // Fresh start: seed params from the controller's declarations + reset the runtime cursor
                // (so the new controller enters at its default state rather than a stale index).
                m_St.edit.Params.clear();
                if (const auto* c = AnimatorControllerStore::Instance().Get(handle))
                    for (const auto& p : c->params) m_St.edit.Params.emplace_back(AssetKeyHash(p.name), 0.0f);
                m_St.edit.CurrentState = -1; m_St.edit.FromState = -1;
                m_St.edit.TransitionElapsed = 0.0f; m_St.edit.Phase = 0.0f; m_St.edit.StateTime = 0.0f;
                m_St.edit.SnapshotPose.clear();
                m_St.modified = true;
            }
            if (sel) ImGui::SetItemDefaultFocus();
        }
        ImGui::EndCombo();
    }

    // Live debug readout (from the live component, not the edit copy).
    const AnimatorController* c = AnimatorControllerStore::Instance().Get(live->ControllerId);
    if (c) {
        auto stateName = [&](int s){ return (s >= 0 && s < (int)c->states.size()) ? c->states[s].name.c_str() : "-"; };
        ImGui::SeparatorText("Runtime");
        ImGui::Text("State: %s", stateName(live->CurrentState));
        if (live->FromState >= 0) {
            const float w = live->TransitionDur > 0.0f ? live->TransitionElapsed / live->TransitionDur : 1.0f;
            ImGui::Text("Transition: %s -> %s  w=%.2f%s", stateName(live->FromState),
                        stateName(live->CurrentState), w, live->TransitionCyclic ? " (phase-sync)" : "");
        }
        ImGui::Text("Phase: %.3f", live->Phase);

        ImGui::SeparatorText("Params (drag to test)");
        for (const auto& decl : c->params) {
            const uint64_t h = AssetKeyHash(decl.name);
            float liveVal = 0.0f;
            for (const auto& pr : live->Params) if (pr.first == h) { liveVal = pr.second; break; }
            // Editable copy in edit (ensure an entry exists).
            float* edit = nullptr;
            for (auto& pr : m_St.edit.Params) if (pr.first == h) { edit = &pr.second; break; }
            if (!edit) { m_St.edit.Params.emplace_back(h, liveVal); edit = &m_St.edit.Params.back().second; }
            if (decl.type == AnimParamType::Float) {
                if (ImGui::DragFloat(decl.name.c_str(), edit, 0.05f, 0.0f, 20.0f)) m_St.modified = true;
            } else {
                bool b = (*edit != 0.0f);
                if (ImGui::Checkbox(decl.name.c_str(), &b)) { *edit = b ? 1.0f : 0.0f; m_St.modified = true; }
            }
            ImGui::SameLine(); ImGui::TextDisabled("(live %.2f)", liveVal);
        }
    }

    ImGui::Spacing();
    if (m_St.modified) ImGui::TextColored(ImVec4(1,1,0,1), "* Modified (not yet saved)");
    if (ImGui::Button("Apply Changes##Animator", ImVec2(150, 0))) {
        ECSCommand modifyCmd = ECSCommand::ModifyComponent(e, m_St.edit);
        if (!ctx.App->ECSCommandRing.Push(modifyCmd))
            SM_WARN("ECS command queue full! Modify command dropped.");
        m_St.modified = false;
    }
}
