#include "AnimationEditor.h"
#include "EditorContext.h"
#include <imgui.h>
#include <string>
#include <vector>
#include <utility>
#include "ApplicationContext.h"
#include "ECSCommands.h"
#include "animation/AnimationStore.h"
#include "lib.h"

void AnimationEditor::AddDefault(const EditorContext& ctx, EntityId e) {
    ECSCommand addCmd = ECSCommand::AddComponent(e, AnimationComponent{});
    if (!ctx.App->ECSCommandRing.Push(addCmd))
        SM_WARN("ECS command queue full! Add component command dropped.");
}
void AnimationEditor::Remove(const EditorContext& ctx, EntityId e) {
    ECSCommand removeCmd = ECSCommand::RemoveComponent<AnimationComponent>(e);
    if (!ctx.App->ECSCommandRing.Push(removeCmd))
        SM_WARN("ECS command queue full! Remove component command dropped.");
}
void AnimationEditor::DrawEditor(const EditorContext& ctx, EntityId e) {
    if (!m_St.Begin(ctx, e)) return;

    std::string current = AnimationStore::Instance().KeyForHandle(m_St.edit.ClipId);
    if (current.empty()) current = "(none)";
    if (ImGui::BeginCombo("Clip", current.c_str())) {
        const auto clips = AnimationStore::Instance().GetAssetList(); // built only while open
        if (clips.empty()) ImGui::TextDisabled("No clips loaded");
        for (const auto& [handle, key] : clips) {
            const bool isSelected = (handle == m_St.edit.ClipId);
            const char* label = key.empty() ? "(none)" : key.c_str();
            if (ImGui::Selectable(label, isSelected)) { m_St.edit.ClipId = handle; m_St.modified = true; }
            if (isSelected) ImGui::SetItemDefaultFocus();
        }
        ImGui::EndCombo();
    }
    if (ImGui::Checkbox("Playing", &m_St.edit.Playing)) m_St.modified = true;
    if (ImGui::Checkbox("Looping", &m_St.edit.Looping)) m_St.modified = true;
    if (ImGui::DragFloat("Speed", &m_St.edit.Speed, 0.05f, 0.0f, 8.0f)) m_St.modified = true;
    if (ImGui::DragFloat("Time", &m_St.edit.Time, 0.01f, 0.0f, 1000.0f)) m_St.modified = true;

    // Clip B (blend target)
    std::string currentB = AnimationStore::Instance().KeyForHandle(m_St.edit.ClipB);
    if (currentB.empty()) currentB = "(none)";
    if (ImGui::BeginCombo("Clip B", currentB.c_str())) {
        const auto clips = AnimationStore::Instance().GetAssetList();
        if (clips.empty()) ImGui::TextDisabled("No clips loaded");
        for (const auto& [handle, key] : clips) {
            const bool isSelected = (handle == m_St.edit.ClipB);
            const char* label = key.empty() ? "(none)" : key.c_str();
            if (ImGui::Selectable(label, isSelected)) { m_St.edit.ClipB = handle; m_St.modified = true; }
            if (isSelected) ImGui::SetItemDefaultFocus();
        }
        ImGui::EndCombo();
    }
    if (ImGui::SliderFloat("Blend Weight", &m_St.edit.BlendWeight, 0.0f, 1.0f)) m_St.modified = true;
    if (ImGui::DragFloat("Time B", &m_St.edit.TimeB, 0.01f, 0.0f, 1000.0f)) m_St.modified = true;

    ImGui::Spacing();
    if (m_St.modified)
        ImGui::TextColored(ImVec4(1.0f, 1.0f, 0.0f, 1.0f), "* Modified (not yet saved)");
    if (ImGui::Button("Apply Changes##Animation", ImVec2(150, 0))) {
        ECSCommand modifyCmd = ECSCommand::ModifyComponent(e, m_St.edit);
        if (!ctx.App->ECSCommandRing.Push(modifyCmd))
            SM_WARN("ECS command queue full! Modify command dropped.");
        m_St.modified = false;
    }
}
