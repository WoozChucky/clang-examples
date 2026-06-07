#include "SkeletonEditor.h"
#include "EditorContext.h"
#include <imgui.h>
#include <string>
#include <vector>
#include <utility>
#include "ApplicationContext.h"
#include "ECSCommands.h"
#include "animation/SkeletonStore.h"
#include "lib.h"

void SkeletonEditor::AddDefault(const EditorContext& ctx, EntityId e) {
    ECSCommand addCmd = ECSCommand::AddComponent(e, SkeletonComponent{});
    if (!ctx.App->ECSCommandRing.Push(addCmd))
        SM_WARN("ECS command queue full! Add component command dropped.");
}
void SkeletonEditor::Remove(const EditorContext& ctx, EntityId e) {
    ECSCommand removeCmd = ECSCommand::RemoveComponent<SkeletonComponent>(e);
    if (!ctx.App->ECSCommandRing.Push(removeCmd))
        SM_WARN("ECS command queue full! Remove component command dropped.");
}
void SkeletonEditor::DrawEditor(const EditorContext& ctx, EntityId e) {
    if (!m_St.Begin(ctx, e)) return;

    // Skeleton picker over SkeletonStore (singleton; keyed by "<modelKey>#skeleton").
    // Preview label is a single cheap lookup; the full list is built ONLY while the dropdown is
    // open (BeginCombo == true), not every frame the editor is visible.
    std::string current = SkeletonStore::Instance().KeyForHandle(m_St.edit.SkeletonId);
    if (current.empty()) current = "(none)";
    if (ImGui::BeginCombo("Skeleton", current.c_str())) {
        const auto assets = SkeletonStore::Instance().GetAssetList(); // vector<pair<uint64_t,string>>
        if (assets.empty()) ImGui::TextDisabled("No skeletons loaded");
        for (const auto& [handle, key] : assets) {
            const bool isSelected = (handle == m_St.edit.SkeletonId);
            const char* label = key.empty() ? "(none)" : key.c_str();
            if (ImGui::Selectable(label, isSelected)) { m_St.edit.SkeletonId = handle; m_St.modified = true; }
            if (isSelected) ImGui::SetItemDefaultFocus();
        }
        ImGui::EndCombo();
    }

    ImGui::Spacing();
    if (m_St.modified)
        ImGui::TextColored(ImVec4(1.0f, 1.0f, 0.0f, 1.0f), "* Modified (not yet saved)");

    if (ImGui::Button("Apply Changes##Skeleton", ImVec2(150, 0))) {
        ECSCommand modifyCmd = ECSCommand::ModifyComponent(e, m_St.edit);
        if (!ctx.App->ECSCommandRing.Push(modifyCmd))
            SM_WARN("ECS command queue full! Modify command dropped.");
        m_St.modified = false;
    }
}
