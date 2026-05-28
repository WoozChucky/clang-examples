#include "TextEditor.h"
#include "EditorContext.h"
#include <imgui.h>
#include <string>
#include "ApplicationContext.h"
#include "ECSCommands.h"
#include "lib.h"

void TextEditor::AddDefault(const EditorContext& ctx, EntityId e) {
    TextComponent newText{};
    newText.Text = "Sample text";
    ECSCommand addCmd = ECSCommand::AddComponent(e, newText);
    if (!ctx.App->ECSCommandRing.Push(addCmd)) {
        SM_WARN("ECS command queue full! Add component command dropped.");
    }
}
void TextEditor::Remove(const EditorContext& ctx, EntityId e) {
    ECSCommand removeCmd = ECSCommand::RemoveComponent<TextComponent>(e);
    if (!ctx.App->ECSCommandRing.Push(removeCmd)) {
        SM_WARN("ECS command queue full! Remove component command dropped.");
    }
}
void TextEditor::DrawEditor(const EditorContext& ctx, EntityId e) {
    const auto* c = m_St.Begin(ctx, e);
    if (!c) return;

    // Text editor
    char buffer[256];
    strncpy_s(buffer, m_St.edit.Text.c_str(), sizeof(buffer));
    if (ImGui::InputTextMultiline("Text", buffer, sizeof(buffer))) {
        m_St.edit.Text = std::string(buffer);
        m_St.modified = true;
    }
    ImGui::Spacing();
    // Text color editor
    ImGui::Text("Text Color:");
    if (ImGui::ColorEdit4("##TextColor", &m_St.edit.Color.r)) {
        m_St.modified = true;
    }
    ImGui::Spacing();
    // Font size editor
    ImGui::Text("Font Size:");
    int fontSizeInt = static_cast<int>(m_St.edit.FontSize);
    if (ImGui::SliderInt("##FontSize", &fontSizeInt, 6, 72, "%d px")) {
        m_St.edit.FontSize = static_cast<size_t>(fontSizeInt);
        m_St.modified = true;
    }
    ImGui::Spacing();
    if (m_St.modified) {
        ImGui::TextColored(ImVec4(1.0f, 1.0f, 0.0f, 1.0f), "* Modified (not yet saved)");
    }
    if (ImGui::Button("Apply Changes##Text", ImVec2(150, 0))) {
        ECSCommand modifyCmd = ECSCommand::ModifyComponent(e, m_St.edit);
        if (!ctx.App->ECSCommandRing.Push(modifyCmd)) {
            SM_WARN("ECS command queue full! Modify command dropped.");
        }
        m_St.modified = false;
    }
    ImGui::SameLine();
    if (ImGui::Button("Revert##Text", ImVec2(150, 0))) {
        m_St.edit = *c;
    }
    ImGui::Separator();
    // Show original values from snapshot (read-only)
    ImGui::TextDisabled("Original values from snapshot:");
    ImGui::TextDisabled("Text: %s", c->Text.c_str());
}
