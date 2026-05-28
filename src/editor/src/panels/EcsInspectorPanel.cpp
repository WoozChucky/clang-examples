#include "EcsInspectorPanel.h"
#include "EditorContext.h"

#include <cstdio>
#include <memory>

#include <imgui.h>

#include "ApplicationContext.h"
#include "ECSCommands.h"
#include "lib.h"

#include "inspector/TransformEditor.h"
#include "inspector/LightningEditor.h"
#include "inspector/MeshEditor.h"
#include "inspector/MaterialEditor.h"
#include "inspector/TextEditor.h"
#include "inspector/SunMarkerEditor.h"
#include "inspector/PlayerEditor.h"
#include "inspector/UIRectEditor.h"
#include "inspector/StateScopeEditor.h"
#include "inspector/MenuButtonEditor.h"
#include "inspector/ColliderEditor.h"
#include "inspector/NavMeshSourceEditor.h"
#include "inspector/NavObstacleEditor.h"
#include "inspector/NavAgentEditor.h"
#include "inspector/NavTargetEditor.h"

EcsInspectorPanel::EcsInspectorPanel() {
    // Registry order == display order.
    m_Editors.push_back(std::make_unique<TransformEditor>());
    m_Editors.push_back(std::make_unique<LightningEditor>());
    m_Editors.push_back(std::make_unique<MeshEditor>());
    m_Editors.push_back(std::make_unique<MaterialEditor>());
    m_Editors.push_back(std::make_unique<TextEditor>());
    m_Editors.push_back(std::make_unique<SunMarkerEditor>());
    m_Editors.push_back(std::make_unique<PlayerEditor>());
    m_Editors.push_back(std::make_unique<UIRectEditor>());
    m_Editors.push_back(std::make_unique<StateScopeEditor>());
    m_Editors.push_back(std::make_unique<MenuButtonEditor>());
    m_Editors.push_back(std::make_unique<ColliderEditor>());
    m_Editors.push_back(std::make_unique<NavMeshSourceEditor>());
    m_Editors.push_back(std::make_unique<NavObstacleEditor>());
    m_Editors.push_back(std::make_unique<NavAgentEditor>());
    m_Editors.push_back(std::make_unique<NavTargetEditor>());
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
            char entityLabel[64];
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

                ImGui::Separator();

                // Remove component options
                for (auto& ed : m_Editors) {
                    if (ed->Has(*ctx.WorldSnapshot, entity)) {
                        char lbl[96]; snprintf(lbl, sizeof(lbl), "Remove %s", ed->Label());
                        if (ImGui::MenuItem(lbl)) ed->Remove(ctx, entity);
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
        }

        ImGui::Separator();

        // === COMPONENT EDITOR FOR SELECTED ENTITY ===
        if (selectedEntity != INVALID_ENTITY && ctx.WorldSnapshot->IsValidEntity(selectedEntity)) {
            ImGui::TextColored(ImVec4(0.4f, 0.8f, 1.0f, 1.0f), "Editing Entity %llu", selectedEntity);
            ImGui::Separator();

            // Migrated component editors render via the registry (display-order prefix).
            for (auto& ed : m_Editors) {
                if (ed->Has(*ctx.WorldSnapshot, selectedEntity)) {
                    if (ImGui::CollapsingHeader(ed->Label(), ImGuiTreeNodeFlags_DefaultOpen))
                        ed->DrawEditor(ctx, selectedEntity);
                }
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
