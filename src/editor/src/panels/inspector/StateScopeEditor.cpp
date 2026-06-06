#include "StateScopeEditor.h"
#include "EditorContext.h"
#include <imgui.h>
#include "ApplicationContext.h"
#include "ECSCommands.h"
#include "StateNameRegistry.h"
#include "lib.h"

void StateScopeEditor::AddDefault(const EditorContext& ctx, EntityId e) {
    ECSCommand addCmd = ECSCommand::AddComponent(e, StateScopeComponent{});
    if (!ctx.App->ECSCommandRing.Push(addCmd)) {
        SM_WARN("ECS command queue full! Add component command dropped.");
    }
}
void StateScopeEditor::Remove(const EditorContext& ctx, EntityId e) {
    ECSCommand removeCmd = ECSCommand::RemoveComponent<StateScopeComponent>(e);
    if (!ctx.App->ECSCommandRing.Push(removeCmd)) {
        SM_WARN("ECS command queue full! Remove component command dropped.");
    }
}
// Edit State Scope Component (one checkbox per state; mask bit i = game state index i)
void StateScopeEditor::DrawEditor(const EditorContext& ctx, EntityId e) {
    const auto* c = m_St.Begin(ctx, e);
    if (!c) return;

    ImGui::TextDisabled("Active in states (none = always):");
    // State labels come from the game-registered StateNameRegistry (game owns GameStateId).
    // Empty (game not loaded / not yet seeded) => no checkboxes this frame.
    for (const auto& [bitIndex, label] : StateNames().Entries()) {
        const uint32_t bit = 1u << bitIndex;
        bool on = (m_St.edit.StateMask & bit) != 0u;
        if (ImGui::Checkbox(label.c_str(), &on)) {
            if (on) m_St.edit.StateMask |= bit; else m_St.edit.StateMask &= ~bit;
            m_St.modified = true;
        }
    }
    ImGui::Spacing();
    m_St.Commit(ctx, e);
}
