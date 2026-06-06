#include "GenericComponentEditor.h"
#include "EditorContext.h"
#include "ApplicationContext.h"
#include "ECSCommands.h"
#include "ComponentSerializerRegistry.h"
#include "lib.h"
#include <imgui.h>

bool GenericComponentEditor::DrawJsonValue(const char* label, nlohmann::json& value) {
    bool changed = false;
    if (value.is_object()) {
        if (ImGui::TreeNodeEx(label, ImGuiTreeNodeFlags_DefaultOpen)) {
            for (auto it = value.begin(); it != value.end(); ++it) {
                ImGui::PushID(it.key().c_str());
                changed |= DrawJsonValue(it.key().c_str(), it.value());
                ImGui::PopID();
            }
            ImGui::TreePop();
        }
    } else if (value.is_array()) {
        if (ImGui::TreeNodeEx(label, ImGuiTreeNodeFlags_DefaultOpen)) {
            for (size_t i = 0; i < value.size(); ++i) {
                ImGui::PushID(static_cast<int>(i));
                char idx[16]; snprintf(idx, sizeof(idx), "[%zu]", i);
                changed |= DrawJsonValue(idx, value[i]);
                ImGui::PopID();
            }
            ImGui::TreePop();
        }
    } else if (value.is_boolean()) {
        bool b = value.get<bool>();
        if (ImGui::Checkbox(label, &b)) { value = b; changed = true; }
    } else if (value.is_number_integer()) {
        int n = value.get<int>();
        if (ImGui::InputInt(label, &n)) { value = n; changed = true; }
    } else if (value.is_number_float()) {
        float f = value.get<float>();
        if (ImGui::InputFloat(label, &f)) { value = f; changed = true; }
    } else if (value.is_string()) {
        std::string s = value.get<std::string>();
        char buf[256]; snprintf(buf, sizeof(buf), "%s", s.c_str());
        if (ImGui::InputText(label, buf, sizeof(buf))) { value = std::string(buf); changed = true; }
    } else {
        ImGui::TextDisabled("%s: (unsupported)", label);
    }
    return changed;
}

void GenericComponentEditor::Draw(const EditorContext& ctx, EntityId entity, const std::string& name) {
    const auto* entry = SerializerRegistry().Find(name);
    if (!entry || !ctx.WorldSnapshot) return;

    if (m_Entity != entity || m_Name != name) {
        m_Entity = entity; m_Name = name; m_Modified = false; m_Edit = nlohmann::json::object();
        entry->save(*ctx.WorldSnapshot, entity, m_Edit);
    } else if (!m_Modified) {
        m_Edit = nlohmann::json::object();
        entry->save(*ctx.WorldSnapshot, entity, m_Edit);
    }

    if (ImGui::CollapsingHeader(name.c_str(), ImGuiTreeNodeFlags_DefaultOpen)) {
        if (DrawJsonValue(name.c_str(), m_Edit)) m_Modified = true;
        if (m_Modified) {
            if (!ctx.App->ECSCommandRing.Push(ECSCommand::ModifyComponentJson(entity, name, m_Edit.dump())))
                SM_WARN("ECS command queue full! ModifyComponentJson dropped.");
            m_Modified = false;
        }
    }
}
