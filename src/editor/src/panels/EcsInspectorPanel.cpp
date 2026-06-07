#include "EcsInspectorPanel.h"
#include "EditorContext.h"

#include <cstdio>
#include <memory>

#include <imgui.h>

#include "ApplicationContext.h"
#include "ECSCommands.h"
#include "ComponentSerializerRegistry.h"
#include "lib.h"

#include "inspector/TransformEditor.h"
#include "inspector/LightningEditor.h"
#include "inspector/MeshEditor.h"
#include "inspector/SkeletonEditor.h"
#include "inspector/MaterialEditor.h"
#include "inspector/TextEditor.h"
#include "inspector/SunMarkerEditor.h"
#include "inspector/UIRectEditor.h"
#include "inspector/StateScopeEditor.h"
#include "inspector/ColliderEditor.h"
#include "inspector/NavMeshSourceEditor.h"
#include "inspector/NavObstacleEditor.h"
#include "inspector/NavAgentEditor.h"
#include "inspector/NavTargetEditor.h"
#include "inspector/NavConstrainedEditor.h"
#include "inspector/NavClassEditor.h"

EcsInspectorPanel::EcsInspectorPanel() {
    // Registry order == display order.
    m_Editors.push_back(std::make_unique<TransformEditor>());
    m_Editors.push_back(std::make_unique<LightningEditor>());
    m_Editors.push_back(std::make_unique<MeshEditor>());
    m_Editors.push_back(std::make_unique<SkeletonEditor>());
    m_Editors.push_back(std::make_unique<MaterialEditor>());
    m_Editors.push_back(std::make_unique<TextEditor>());
    m_Editors.push_back(std::make_unique<SunMarkerEditor>());
    m_Editors.push_back(std::make_unique<UIRectEditor>());
    m_Editors.push_back(std::make_unique<StateScopeEditor>());
    m_Editors.push_back(std::make_unique<ColliderEditor>());
    m_Editors.push_back(std::make_unique<NavMeshSourceEditor>());
    m_Editors.push_back(std::make_unique<NavObstacleEditor>());
    m_Editors.push_back(std::make_unique<NavAgentEditor>());
    m_Editors.push_back(std::make_unique<NavTargetEditor>());
    m_Editors.push_back(std::make_unique<NavConstrainedEditor>());
    m_Editors.push_back(std::make_unique<NavClassEditor>());
}

void EcsInspectorPanel::Draw(const EditorContext& ctx)
{
    // ECS Inspector Window - demonstrates reading from ECS snapshot AND modifying via commands
    ImGui::Begin("ECS Inspector & Editor");

    if (ctx.WorldSnapshot) {
        ImGui::Text("ECS World Snapshot (Tick: %llu)", ctx.Snapshot->Tick);
        ImGui::Text("Entity Count: %zu", ctx.WorldSnapshot->GetEntityCount());
        ImGui::Separator();

        // === ENTITY CREATION SECTION ===
        ImGui::TextColored(ImVec4(0.4f, 1.0f, 0.4f, 1.0f), "Create New Entity");

        if (ImGui::Button("Create Entity", ImVec2(200, 0))) {
            // Create entity command
            ECSCommand createCmd = ECSCommand::CreateEntity();
            if (!ctx.App->ECSCommandRing.Push(createCmd)) {
                SM_WARN("ECS command queue full! Create entity command dropped.");
            }

            // Note: We don't know the new entity ID yet!
            // For now, we'll just create the entity and manually add components to "last entity"
            // This is a limitation of the current one-way command system.
            ImGui::OpenPopup("Entity Created");
        }

        // Popup notification
        if (ImGui::BeginPopupModal("Entity Created", NULL, ImGuiWindowFlags_AlwaysAutoResize)) {
            ImGui::Text("Entity creation command sent!");
            ImGui::Text("Note: Components will be added to the newest entity.");
            ImGui::Text("Check the entity list below after next frame.");
            if (ImGui::Button("OK", ImVec2(120, 0))) {
                ImGui::CloseCurrentPopup();
            }
            ImGui::EndPopup();
        }

        ImGui::Separator();

        // === ENTITY LIST AND EDITING SECTION ===
        ImGui::TextColored(ImVec4(1.0f, 1.0f, 0.4f, 1.0f), "Entity List & Editor");

        // Display all entities
        for (EntityId entity : ctx.WorldSnapshot->GetActiveEntities()) {
            ImGui::PushID(static_cast<int>(entity));

            // Selectable entity item
            bool isSelected = (selectedEntity == entity);
            char entityLabel[128];
            const NameComponent* nameComp = ctx.WorldSnapshot->GetComponent<NameComponent>(entity);
            if (nameComp && !nameComp->Name.empty())
                snprintf(entityLabel, sizeof(entityLabel), "%s", nameComp->Name.c_str());
            else
                snprintf(entityLabel, sizeof(entityLabel), "Entity %llu", entity);
            if (ImGui::Selectable(entityLabel, isSelected)) {
                selectedEntity = entity;
            }

            // Right-click context menu
            if (ImGui::BeginPopupContextItem()) {
                if (ImGui::MenuItem("Delete Entity")) {
                    ECSCommand deleteCmd = ECSCommand::DestroyEntity(entity);
                    if (!ctx.App->ECSCommandRing.Push(deleteCmd)) {
                        SM_WARN("ECS command queue full! Delete command dropped.");
                    }
                    if (selectedEntity == entity) {
                        selectedEntity = INVALID_ENTITY;
                    }
                }

                if (ImGui::MenuItem("Duplicate Entity")) {
                    if (!ctx.App->ECSCommandRing.Push(ECSCommand::DuplicateEntity(entity))) {
                        SM_WARN("ECS command queue full! Duplicate command dropped.");
                    }
                }

                ImGui::Separator();

                // Add component options
                for (auto& ed : m_Editors) {
                    if (!ed->Has(*ctx.WorldSnapshot, entity)) {
                        char lbl[96]; snprintf(lbl, sizeof(lbl), "Add %s", ed->Label());
                        if (ImGui::MenuItem(lbl)) ed->AddDefault(ctx, entity);
                    }
                }

                // Generic add for game-registered (non-builtin) components.
                for (const auto& en : SerializerRegistry().Entries()) {
                    if (en.builtin || en.has(*ctx.WorldSnapshot, entity)) continue;
                    char lbl[96]; snprintf(lbl, sizeof(lbl), "Add %s", en.name.c_str());
                    if (ImGui::MenuItem(lbl)) {
                        if (!ctx.App->ECSCommandRing.Push(ECSCommand::AddComponentByName(entity, en.name)))
                            SM_WARN("ECS command queue full! AddComponentByName dropped.");
                    }
                }

                ImGui::Separator();

                // Remove component options
                for (auto& ed : m_Editors) {
                    if (ed->Has(*ctx.WorldSnapshot, entity)) {
                        char lbl[96]; snprintf(lbl, sizeof(lbl), "Remove %s", ed->Label());
                        if (ImGui::MenuItem(lbl)) ed->Remove(ctx, entity);
                    }
                }

                // Generic remove for game-registered (non-builtin) components.
                for (const auto& en : SerializerRegistry().Entries()) {
                    if (en.builtin || !en.has(*ctx.WorldSnapshot, entity)) continue;
                    char lbl[96]; snprintf(lbl, sizeof(lbl), "Remove %s", en.name.c_str());
                    if (ImGui::MenuItem(lbl)) {
                        if (!ctx.App->ECSCommandRing.Push(ECSCommand::RemoveComponentByName(entity, en.name)))
                            SM_WARN("ECS command queue full! RemoveComponentByName dropped.");
                    }
                }

                ImGui::EndPopup();
            }

            ImGui::PopID();
        }

        // Keyboard ops on the selected entity. Gate on !WantTextInput so pressing Delete while
        // editing a field's text doesn't destroy the entity. Check Ctrl+D before plain Del.
        ImGuiIO& io = ImGui::GetIO();
        if (selectedEntity != INVALID_ENTITY && !io.WantTextInput) {
            if (ImGui::IsKeyChordPressed(ImGuiMod_Ctrl | ImGuiKey_D)) {
                if (!ctx.App->ECSCommandRing.Push(ECSCommand::DuplicateEntity(selectedEntity))) {
                    SM_WARN("ECS command queue full! Duplicate command dropped.");
                }
            } else if (ImGui::IsKeyPressed(ImGuiKey_Delete)) {
                if (!ctx.App->ECSCommandRing.Push(ECSCommand::DestroyEntity(selectedEntity))) {
                    SM_WARN("ECS command queue full! Delete command dropped.");
                }
                selectedEntity = INVALID_ENTITY;
            }
            else if (ImGui::IsKeyPressed(ImGuiKey_F2)) {
                m_FocusRename = true;
            }
        }

        ImGui::Separator();

        // === COMPONENT EDITOR FOR SELECTED ENTITY ===
        if (selectedEntity != INVALID_ENTITY && ctx.WorldSnapshot->IsValidEntity(selectedEntity)) {
            ImGui::TextColored(ImVec4(0.4f, 0.8f, 1.0f, 1.0f), "Editing Entity %llu", selectedEntity);
            ImGui::Separator();

            // --- Dev name (always-on rename field; drives the optional builtin NameComponent) ---
            // Re-sync the buffer from the snapshot when the selection changes, so we don't clobber
            // live typing on the same entity. Empty buffer => unnamed.
            if (m_RenameBufFor != selectedEntity) {
                const NameComponent* nc = ctx.WorldSnapshot->GetComponent<NameComponent>(selectedEntity);
                snprintf(m_RenameBuf, sizeof(m_RenameBuf), "%s", (nc && !nc->Name.empty()) ? nc->Name.c_str() : "");
                m_RenameBufFor = selectedEntity;
            }
            if (m_FocusRename) {
                ImGui::SetKeyboardFocusHere();
                m_FocusRename = false;
            }
            const bool committed =
                ImGui::InputText("Name", m_RenameBuf, sizeof(m_RenameBuf), ImGuiInputTextFlags_EnterReturnsTrue)
                | (ImGui::IsItemDeactivatedAfterEdit() ? 1 : 0);
            if (committed) {
                // Trim leading/trailing whitespace.
                std::string name(m_RenameBuf);
                const size_t b = name.find_first_not_of(" \t\r\n");
                const size_t e = name.find_last_not_of(" \t\r\n");
                name = (b == std::string::npos) ? std::string() : name.substr(b, e - b + 1);

                const bool has = ctx.WorldSnapshot->GetComponent<NameComponent>(selectedEntity) != nullptr;
                if (!name.empty()) {
                    // load() is AddComponent (upsert): adds-or-sets in one command.
                    const std::string json = nlohmann::json{{"Name", name}}.dump();
                    if (!ctx.App->ECSCommandRing.Push(
                            ECSCommand::ModifyComponentJson(selectedEntity, "NameComponent", json)))
                        SM_WARN("ECS command queue full! Rename (ModifyComponentJson) dropped.");
                } else if (has) {
                    if (!ctx.App->ECSCommandRing.Push(
                            ECSCommand::RemoveComponentByName(selectedEntity, "NameComponent")))
                        SM_WARN("ECS command queue full! Rename (RemoveComponentByName) dropped.");
                }
                // empty + !has => no-op.
            }
            ImGui::Separator();

            // Migrated component editors render via the registry (display-order prefix).
            for (auto& ed : m_Editors) {
                if (ed->Has(*ctx.WorldSnapshot, selectedEntity)) {
                    if (ImGui::CollapsingHeader(ed->Label(), ImGuiTreeNodeFlags_DefaultOpen))
                        ed->DrawEditor(ctx, selectedEntity);
                }
            }

            // Generic JSON editor for game-registered (non-builtin) components on this entity.
            for (const auto& en : SerializerRegistry().Entries()) {
                if (en.builtin || !en.has(*ctx.WorldSnapshot, selectedEntity)) continue;
                m_GenericEditor.Draw(ctx, selectedEntity, en.name);
            }

        } else if (selectedEntity != INVALID_ENTITY) {
            ImGui::TextDisabled("Selected entity no longer exists.");
            if (ImGui::Button("Clear Selection")) {
                selectedEntity = INVALID_ENTITY;
            }
        } else {
            ImGui::TextDisabled("Select an entity to edit its components.");
        }

    } else {
        ImGui::TextDisabled("No ECS snapshot available");
    }

    ImGui::End();
}
