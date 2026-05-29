#include "NavClassEditor.h"
#include "EditorContext.h"
#include <imgui.h>
#include <cstdio>
#include "ApplicationContext.h"
#include "ECSCommands.h"
#include "lib.h"

void NavClassEditor::AddDefault(const EditorContext& ctx, EntityId e) {
    ECSCommand addCmd = ECSCommand::AddComponent(e, NavClassComponent{});
    if (!ctx.App->ECSCommandRing.Push(addCmd))
        SM_WARN("ECS command queue full! Add component command dropped.");
}
void NavClassEditor::Remove(const EditorContext& ctx, EntityId e) {
    ECSCommand removeCmd = ECSCommand::RemoveComponent<NavClassComponent>(e);
    if (!ctx.App->ECSCommandRing.Push(removeCmd))
        SM_WARN("ECS command queue full! Remove component command dropped.");
}
void NavClassEditor::DrawEditor(const EditorContext& ctx, EntityId e) {
    const auto* c = m_St.Begin(ctx, e);
    if (!c) return;

    // Read the live class list from the nav-config singleton so the combo only
    // offers classes that actually exist (and shows each radius). Falls back to a
    // single class 0 when no config is present.
    const NavMeshConfigComponent* cfg =
        ctx.World ? ctx.World->GetSingleton<NavMeshConfigComponent>() : nullptr;
    const uint8_t classCount = (cfg && cfg->ClassCount > 0) ? cfg->ClassCount : uint8_t{1};

    uint8_t classId = m_St.edit.ClassId;
    if (classId >= classCount) classId = 0;   // display guard (resolves to 0 at runtime too)

    auto labelFor = [&](char* buf, size_t n, uint8_t i) {
        if (cfg && i < cfg->ClassCount)
            std::snprintf(buf, n, "Class %u  (r=%.2f)", (unsigned)i, cfg->Classes[i].AgentRadius);
        else
            std::snprintf(buf, n, "Class %u", (unsigned)i);
    };

    char preview[40];
    labelFor(preview, sizeof(preview), classId);
    if (ImGui::BeginCombo("Class", preview)) {
        for (uint8_t i = 0; i < classCount; ++i) {
            char label[40];
            labelFor(label, sizeof(label), i);
            const bool selected = (i == classId);
            if (ImGui::Selectable(label, selected)) {
                m_St.edit.ClassId = i;
                m_St.modified = true;
            }
            if (selected) ImGui::SetItemDefaultFocus();
        }
        ImGui::EndCombo();
    }
    ImGui::TextDisabled("Which NavMeshConfig class mesh this entity uses.");
    m_St.Commit(ctx, e);
}
