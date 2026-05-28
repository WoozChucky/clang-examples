#include "EcsInspectorPanel.h"
#include "EditorContext.h"

#include <cstdio>
#include <memory>

#include <imgui.h>
#include <glm/glm.hpp>

#include "ApplicationContext.h"
#include "Actions.h"
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

EcsInspectorPanel::EcsInspectorPanel() {
    // Registry order == display order. Batch C appends here in Task 4.
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
                // (remaining inline "Add X" blocks for Collider..NavTarget stay below, untouched)

                if (!ctx.WorldSnapshot->HasComponent<ColliderComponent>(entity)) {
                    if (ImGui::MenuItem("Add Collider Component")) {
                        ECSCommand addCmd = ECSCommand::AddComponent(entity, ColliderComponent{});
                        if (!ctx.App->ECSCommandRing.Push(addCmd)) {
                            SM_WARN("ECS command queue full! Add component command dropped.");
                        }
                    }
                }

                if (!ctx.WorldSnapshot->HasComponent<NavMeshSourceComponent>(entity)) {
                    if (ImGui::MenuItem("Add NavMesh Source Component")) {
                        ECSCommand addCmd = ECSCommand::AddComponent(entity, NavMeshSourceComponent{});
                        if (!ctx.App->ECSCommandRing.Push(addCmd)) {
                            SM_WARN("ECS command queue full! Add component command dropped.");
                        }
                    }
                }

                if (!ctx.WorldSnapshot->HasComponent<NavObstacleComponent>(entity)) {
                    if (ImGui::MenuItem("Add NavMesh Obstacle Component")) {
                        ECSCommand addCmd = ECSCommand::AddComponent(entity, NavObstacleComponent{});
                        if (!ctx.App->ECSCommandRing.Push(addCmd)) {
                            SM_WARN("ECS command queue full! Add component command dropped.");
                        }
                    }
                }

                if (!ctx.WorldSnapshot->HasComponent<NavAgentComponent>(entity)) {
                    if (ImGui::MenuItem("Add NavMesh Agent Component")) {
                        ECSCommand addCmd = ECSCommand::AddComponent(entity, NavAgentComponent{});
                        if (!ctx.App->ECSCommandRing.Push(addCmd)) {
                            SM_WARN("ECS command queue full! Add component command dropped.");
                        }
                    }
                }

                if (!ctx.WorldSnapshot->HasComponent<NavTargetComponent>(entity)) {
                    if (ImGui::MenuItem("Add NavMesh Target Component")) {
                        ECSCommand addCmd = ECSCommand::AddComponent(entity, NavTargetComponent{});
                        if (!ctx.App->ECSCommandRing.Push(addCmd)) {
                            SM_WARN("ECS command queue full! Add component command dropped.");
                        }
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
                // (remaining inline "Remove X" blocks for Collider..NavTarget stay below)

                if (ctx.WorldSnapshot->HasComponent<ColliderComponent>(entity)) {
                    if (ImGui::MenuItem("Remove Collider Component")) {
                        ECSCommand removeCmd = ECSCommand::RemoveComponent<ColliderComponent>(entity);
                        if (!ctx.App->ECSCommandRing.Push(removeCmd)) {
                            SM_WARN("ECS command queue full! Remove component command dropped.");
                        }
                    }
                }

                if (ctx.WorldSnapshot->HasComponent<NavMeshSourceComponent>(entity)) {
                    if (ImGui::MenuItem("Remove NavMesh Source Component")) {
                        ECSCommand removeCmd = ECSCommand::RemoveComponent<NavMeshSourceComponent>(entity);
                        if (!ctx.App->ECSCommandRing.Push(removeCmd)) {
                            SM_WARN("ECS command queue full! Remove component command dropped.");
                        }
                    }
                }

                if (ctx.WorldSnapshot->HasComponent<NavObstacleComponent>(entity)) {
                    if (ImGui::MenuItem("Remove NavMesh Obstacle Component")) {
                        ECSCommand removeCmd = ECSCommand::RemoveComponent<NavObstacleComponent>(entity);
                        if (!ctx.App->ECSCommandRing.Push(removeCmd)) {
                            SM_WARN("ECS command queue full! Remove component command dropped.");
                        }
                    }
                }

                if (ctx.WorldSnapshot->HasComponent<NavAgentComponent>(entity)) {
                    if (ImGui::MenuItem("Remove NavMesh Agent Component")) {
                        ECSCommand removeCmd = ECSCommand::RemoveComponent<NavAgentComponent>(entity);
                        if (!ctx.App->ECSCommandRing.Push(removeCmd)) {
                            SM_WARN("ECS command queue full! Remove component command dropped.");
                        }
                    }
                }

                if (ctx.WorldSnapshot->HasComponent<NavTargetComponent>(entity)) {
                    if (ImGui::MenuItem("Remove NavMesh Target Component")) {
                        ECSCommand removeCmd = ECSCommand::RemoveComponent<NavTargetComponent>(entity);
                        if (!ctx.App->ECSCommandRing.Push(removeCmd)) {
                            SM_WARN("ECS command queue full! Remove component command dropped.");
                        }
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
            // (remaining inline "// Edit X Component" blocks for Collider..NavTarget stay below)

            // Edit Collider Component
            if (ctx.WorldSnapshot->HasComponent<ColliderComponent>(selectedEntity)) {
                if (ImGui::CollapsingHeader("Collider Component", ImGuiTreeNodeFlags_DefaultOpen)) {
                    auto* collider = ctx.WorldSnapshot->GetComponent<ColliderComponent>(selectedEntity);
                    if (collider) {
                        if (lastEditedColliderEntity != selectedEntity) {
                            editCollider = *collider;
                            lastEditedColliderEntity = selectedEntity;
                            colliderModified = false;
                        }
                        if (!colliderModified) {
                            editCollider = *collider;
                        }
                        static const char* kShapeNames[] = { "Box", "Sphere", "Capsule" };
                        static const ColliderShape kShapes[] = { ColliderShape::Box, ColliderShape::Sphere, ColliderShape::Capsule };
                        int curIdx = 0;
                        for (int i = 0; i < 3; ++i) if (editCollider.Shape == kShapes[i]) curIdx = i;
                        if (ImGui::Combo("Shape", &curIdx, kShapeNames, 3)) {
                            editCollider.Shape = kShapes[curIdx];
                            colliderModified = true;
                        }
                        const char* sizeLabel = "Size (half extents / radius-height)";
                        if (editCollider.Shape == ColliderShape::Sphere) sizeLabel = "Size (radius in X)";
                        else if (editCollider.Shape == ColliderShape::Capsule) sizeLabel = "Size (radius X, half-height Y)";
                        if (ImGui::InputFloat3(sizeLabel, &editCollider.Size.x)) colliderModified = true;
                        if (ImGui::InputFloat3("Offset", &editCollider.Offset.x)) colliderModified = true;
                        if (ImGui::Checkbox("Trigger", &editCollider.IsTrigger)) colliderModified = true;
                        if (ImGui::Checkbox("Static", &editCollider.IsStatic)) colliderModified = true;
                        if (ImGui::InputScalar("Layer", ImGuiDataType_U32, &editCollider.Layer)) colliderModified = true;
                        if (ImGui::InputScalar("Mask", ImGuiDataType_U32, &editCollider.Mask)) colliderModified = true;
                        ImGui::Spacing();
                        if (colliderModified) {
                            ECSCommand modifyCmd = ECSCommand::ModifyComponent(selectedEntity, editCollider);
                            if (!ctx.App->ECSCommandRing.Push(modifyCmd)) {
                                SM_WARN("ECS command queue full! Modify command dropped.");
                            }
                            colliderModified = false;
                        }
                    }
                }
            }

            // Edit NavMesh Source Component
            if (ctx.WorldSnapshot->HasComponent<NavMeshSourceComponent>(selectedEntity)) {
                if (ImGui::CollapsingHeader("NavMesh Source Component", ImGuiTreeNodeFlags_DefaultOpen)) {
                    auto* src = ctx.WorldSnapshot->GetComponent<NavMeshSourceComponent>(selectedEntity);
                    if (src) {
                        if (lastEditedNavSourceEntity != selectedEntity) {
                            editNavSource = *src;
                            lastEditedNavSourceEntity = selectedEntity;
                            navSourceModified = false;
                        }
                        if (!navSourceModified) {
                            editNavSource = *src;
                        }
                        // AreaId: 0-63, Recast convention (63 == RC_WALKABLE_AREA default).
                        int areaId = static_cast<int>(editNavSource.AreaId);
                        if (ImGui::SliderInt("Area ID", &areaId, 0, 63)) {
                            editNavSource.AreaId = static_cast<uint8_t>(areaId);
                            navSourceModified = true;
                        }
                        // Geometry: Unset sentinel forces explicit author choice. "-- choose --" entry
                        // surfaces the unselected state loudly so authors don't ship Unset accidentally.
                        static const char* kGeomNames[]   = { "-- choose --", "Collider", "Mesh" };
                        static const NavMeshGeometrySource kGeoms[] = {
                            NavMeshGeometrySource::Unset,
                            NavMeshGeometrySource::Collider,
                            NavMeshGeometrySource::Mesh,
                        };
                        int geomIdx = 0;
                        for (int i = 0; i < 3; ++i) if (editNavSource.Geometry == kGeoms[i]) geomIdx = i;
                        if (ImGui::Combo("Geometry", &geomIdx, kGeomNames, 3)) {
                            editNavSource.Geometry = kGeoms[geomIdx];
                            navSourceModified = true;
                        }
                        if (editNavSource.Geometry == NavMeshGeometrySource::Unset) {
                            ImGui::TextColored(ImVec4(1.0f, 0.7f, 0.2f, 1.0f),
                                               "Unset: SM_WARN + skip at build. Pick a geometry source.");
                        }
                        ImGui::Spacing();
                        if (navSourceModified) {
                            ECSCommand modifyCmd = ECSCommand::ModifyComponent(selectedEntity, editNavSource);
                            if (!ctx.App->ECSCommandRing.Push(modifyCmd)) {
                                SM_WARN("ECS command queue full! Modify command dropped.");
                            }
                            navSourceModified = false;
                        }
                    }
                }
            }

            // Edit NavMesh Obstacle Component
            if (ctx.WorldSnapshot->HasComponent<NavObstacleComponent>(selectedEntity)) {
                if (ImGui::CollapsingHeader("NavMesh Obstacle Component", ImGuiTreeNodeFlags_DefaultOpen)) {
                    auto* obs = ctx.WorldSnapshot->GetComponent<NavObstacleComponent>(selectedEntity);
                    if (obs) {
                        if (lastEditedNavObstacleEntity != selectedEntity) {
                            editNavObstacle = *obs;
                            lastEditedNavObstacleEntity = selectedEntity;
                            navObstacleModified = false;
                        }
                        if (!navObstacleModified) {
                            editNavObstacle = *obs;
                        }
                        static const char* kShapeNames[] = { "Cylinder", "Box" };
                        static const NavObstacleShape kShapes[] = {
                            NavObstacleShape::Cylinder, NavObstacleShape::Box,
                        };
                        int idx = 0;
                        for (int i = 0; i < 2; ++i) if (editNavObstacle.Shape == kShapes[i]) idx = i;
                        if (ImGui::Combo("Shape", &idx, kShapeNames, 2)) {
                            editNavObstacle.Shape = kShapes[idx];
                            navObstacleModified = true;
                        }
                        const char* sizeLabel = (editNavObstacle.Shape == NavObstacleShape::Cylinder)
                            ? "Size (X=radius, Y=height)"
                            : "Size (half-extents)";
                        if (ImGui::InputFloat3(sizeLabel, &editNavObstacle.Size.x)) navObstacleModified = true;
                        if (ImGui::InputFloat3("Offset", &editNavObstacle.Offset.x)) navObstacleModified = true;
                        ImGui::Spacing();
                        if (navObstacleModified) {
                            ECSCommand modifyCmd = ECSCommand::ModifyComponent(selectedEntity, editNavObstacle);
                            if (!ctx.App->ECSCommandRing.Push(modifyCmd)) {
                                SM_WARN("ECS command queue full! Modify command dropped.");
                            }
                            navObstacleModified = false;
                        }
                    }
                }
            }

            // Edit NavMesh Agent Component
            if (ctx.WorldSnapshot->HasComponent<NavAgentComponent>(selectedEntity)) {
                if (ImGui::CollapsingHeader("NavMesh Agent Component", ImGuiTreeNodeFlags_DefaultOpen)) {
                    auto* a = ctx.WorldSnapshot->GetComponent<NavAgentComponent>(selectedEntity);
                    if (a) {
                        if (lastEditedNavAgentEntity != selectedEntity) {
                            editNavAgent = *a;
                            lastEditedNavAgentEntity = selectedEntity;
                            navAgentModified = false;
                        }
                        if (!navAgentModified) {
                            editNavAgent = *a;
                        }
                        if (ImGui::DragFloat("Move Speed",      &editNavAgent.MoveSpeed,      0.05f, 0.0f, 50.0f, "%.2f m/s")) navAgentModified = true;
                        if (ImGui::DragFloat("Radius",          &editNavAgent.Radius,         0.01f, 0.05f, 5.0f, "%.2f m"))   navAgentModified = true;
                        if (ImGui::DragFloat("Reached Epsilon", &editNavAgent.ReachedEpsilon, 0.01f, 0.01f, 2.0f, "%.2f m"))   navAgentModified = true;
                        ImGui::Spacing();
                        if (navAgentModified) {
                            ECSCommand modifyCmd = ECSCommand::ModifyComponent(selectedEntity, editNavAgent);
                            if (!ctx.App->ECSCommandRing.Push(modifyCmd)) {
                                SM_WARN("ECS command queue full! Modify command dropped.");
                            }
                            navAgentModified = false;
                        }
                    }
                }
            }

            // Edit NavMesh Target Component
            if (ctx.WorldSnapshot->HasComponent<NavTargetComponent>(selectedEntity)) {
                if (ImGui::CollapsingHeader("NavMesh Target Component", ImGuiTreeNodeFlags_DefaultOpen)) {
                    auto* t = ctx.WorldSnapshot->GetComponent<NavTargetComponent>(selectedEntity);
                    if (t) {
                        if (lastEditedNavTargetEntity != selectedEntity) {
                            editNavTarget = *t;
                            lastEditedNavTargetEntity = selectedEntity;
                            navTargetModified = false;
                        }
                        if (!navTargetModified) {
                            editNavTarget = *t;
                        }
                        if (ImGui::InputFloat3("Destination", &editNavTarget.Destination.x)) navTargetModified = true;
                        ImGui::Spacing();
                        if (navTargetModified) {
                            ECSCommand modifyCmd = ECSCommand::ModifyComponent(selectedEntity, editNavTarget);
                            if (!ctx.App->ECSCommandRing.Push(modifyCmd)) {
                                SM_WARN("ECS command queue full! Modify command dropped.");
                            }
                            navTargetModified = false;
                        }
                    }
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
