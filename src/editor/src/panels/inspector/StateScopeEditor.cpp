#include "StateScopeEditor.h"
#include "EditorContext.h"
#include <imgui.h>
#include "ApplicationContext.h"
#include "ECSCommands.h"
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
// Edit State Scope Component (one checkbox per state; mask bit i = GameStateId i)
void StateScopeEditor::DrawEditor(const EditorContext& ctx, EntityId e) {
    const auto* c = m_St.Begin(ctx, e);
    if (!c) return;

    ImGui::TextDisabled("Active in states (none = always):");
    // Bit indices mirror the game-owned GameStateId (Uninitialized=0 is omitted — nothing
    // scopes to it). A registered state-name table (boundary Piece 5) will replace this
    // hardcoded mirror so the editor stops duplicating the game's state vocabulary.
    struct { const char* label; uint32_t bitIndex; } kStates[] = {
        {"Main Menu", 1},
        {"In Level",  2},
        {"In Editor", 3},
        {"Paused",    4},
    };
    for (const auto& s : kStates) {
        const uint32_t bit = 1u << s.bitIndex;
        bool on = (m_St.edit.StateMask & bit) != 0u;
        if (ImGui::Checkbox(s.label, &on)) {
            if (on) m_St.edit.StateMask |= bit; else m_St.edit.StateMask &= ~bit;
            m_St.modified = true;
        }
    }
    ImGui::Spacing();
    m_St.Commit(ctx, e);
}
