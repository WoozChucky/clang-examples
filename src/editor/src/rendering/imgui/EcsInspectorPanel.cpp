#include "EcsInspectorPanel.h"
#include "EditorContext.h"

#include <cstdio>
#include <string>
#include <vector>

#include <imgui.h>
#include <ImGuizmo.h>
#include <glm/gtc/type_ptr.hpp>
#include <glm/glm.hpp>

#include "ApplicationContext.h"
#include "Actions.h"
#include "ECSCommands.h"
#include "MeshSystem.h"
#include "MaterialSystem.h"
#include "lib.h"
#include "TransformMath.h"

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
                if (!ctx.WorldSnapshot->HasComponent<TransformComponent>(entity)) {
                    if (ImGui::MenuItem("Add Transform Component")) {
                        TransformComponent newTransform{};
                        newTransform.Position = glm::vec3(0.0f, 0.0f, 0.0f);
                        newTransform.Rotation = glm::vec3(0.0f, 0.0f, 0.0f);
                        newTransform.Scale = glm::vec3(1.0f, 1.0f, 1.0f);

                        ECSCommand addCmd = ECSCommand::AddComponent(entity, newTransform);
                        if (!ctx.App->ECSCommandRing.Push(addCmd)) {
                            SM_WARN("ECS command queue full! Add component command dropped.");
                        }
                    }
                }

                if (!ctx.WorldSnapshot->HasComponent<LightningComponent>(entity)) {
                    if (ImGui::MenuItem("Add Lightning Component")) {
                        LightningComponent newLightning{};
                        newLightning.Type = LightningType::Directional;
                        newLightning.Direction = glm::vec4(0.0f, -1.0f, 0.0f, 0.0f);
                        newLightning.Color = glm::vec4(1.0f);
                        newLightning.Intensity = 1.0f;
                        ECSCommand addCmd = ECSCommand::AddComponent(entity, newLightning);
                        if (!ctx.App->ECSCommandRing.Push(addCmd)) {
                            SM_WARN("ECS command queue full! Add component command dropped.");
                        }
                    }
                }

                if (!ctx.WorldSnapshot->HasComponent<MeshComponent>(entity)) {
                    if (ImGui::MenuItem("Add Mesh Component")) {
                        MeshComponent newMesh{};
                        newMesh.MeshId = 0;
                        newMesh.Visible = false;

                        ECSCommand addCmd = ECSCommand::AddComponent(entity, newMesh);
                        if (!ctx.App->ECSCommandRing.Push(addCmd)) {
                            SM_WARN("ECS command queue full! Add component command dropped.");
                        }
                    }
                }

                if (!ctx.WorldSnapshot->HasComponent<MaterialComponent>(entity)) {
                    if (ImGui::MenuItem("Add Material Component")) {
                        MaterialComponent newMaterial{};
                        newMaterial.MaterialId = 0;
                        newMaterial.BaseColor = glm::vec4(1.0f);
                        newMaterial.Flags = 0;

                        ECSCommand addCmd = ECSCommand::AddComponent(entity, newMaterial);
                        if (!ctx.App->ECSCommandRing.Push(addCmd)) {
                            SM_WARN("ECS command queue full! Add component command dropped.");
                        }
                    }
                }

                if (!ctx.WorldSnapshot->HasComponent<TextComponent>(entity)) {
                    if (ImGui::MenuItem("Add Text Component")) {
                        TextComponent newText{};
                        newText.Text = "Sample text";
                        ECSCommand addCmd = ECSCommand::AddComponent(entity, newText);
                        if (!ctx.App->ECSCommandRing.Push(addCmd)) {
                            SM_WARN("ECS command queue full! Add component command dropped.");
                        }
                    }
                }

                if (!ctx.WorldSnapshot->HasComponent<SunMarker>(entity)) {
                    if (ImGui::MenuItem("Add Sun Marker")) {
                        ECSCommand addCmd = ECSCommand::AddComponent(entity, SunMarker{});
                        if (!ctx.App->ECSCommandRing.Push(addCmd)) {
                            SM_WARN("ECS command queue full! Add component command dropped.");
                        }
                    }
                }

                if (!ctx.WorldSnapshot->HasComponent<PlayerComponent>(entity)) {
                    if (ImGui::MenuItem("Add Player Component")) {
                        ECSCommand addCmd = ECSCommand::AddComponent(entity, PlayerComponent{});
                        if (!ctx.App->ECSCommandRing.Push(addCmd)) {
                            SM_WARN("ECS command queue full! Add component command dropped.");
                        }
                    }
                }

                if (!ctx.WorldSnapshot->HasComponent<UIRectComponent>(entity)) {
                    if (ImGui::MenuItem("Add UI Rect Component")) {
                        ECSCommand addCmd = ECSCommand::AddComponent(entity, UIRectComponent{});
                        if (!ctx.App->ECSCommandRing.Push(addCmd)) {
                            SM_WARN("ECS command queue full! Add component command dropped.");
                        }
                    }
                }
                if (!ctx.WorldSnapshot->HasComponent<StateScopeComponent>(entity)) {
                    if (ImGui::MenuItem("Add State Scope Component")) {
                        ECSCommand addCmd = ECSCommand::AddComponent(entity, StateScopeComponent{});
                        if (!ctx.App->ECSCommandRing.Push(addCmd)) {
                            SM_WARN("ECS command queue full! Add component command dropped.");
                        }
                    }
                }
                if (!ctx.WorldSnapshot->HasComponent<MenuButtonComponent>(entity)) {
                    if (ImGui::MenuItem("Add Menu Button Component")) {
                        ECSCommand addCmd = ECSCommand::AddComponent(entity, MenuButtonComponent{});
                        if (!ctx.App->ECSCommandRing.Push(addCmd)) {
                            SM_WARN("ECS command queue full! Add component command dropped.");
                        }
                    }
                }

                if (!ctx.WorldSnapshot->HasComponent<ColliderComponent>(entity)) {
                    if (ImGui::MenuItem("Add Collider Component")) {
                        ECSCommand addCmd = ECSCommand::AddComponent(entity, ColliderComponent{});
                        if (!ctx.App->ECSCommandRing.Push(addCmd)) {
                            SM_WARN("ECS command queue full! Add component command dropped.");
                        }
                    }
                }

                ImGui::Separator();

                // Remove component options
                if (ctx.WorldSnapshot->HasComponent<TransformComponent>(entity)) {
                    if (ImGui::MenuItem("Remove Transform Component")) {
                        ECSCommand removeCmd = ECSCommand::RemoveComponent<TransformComponent>(entity);
                        if (!ctx.App->ECSCommandRing.Push(removeCmd)) {
                            SM_WARN("ECS command queue full! Remove component command dropped.");
                        }
                    }
                }

                if (ctx.WorldSnapshot->HasComponent<LightningComponent>(entity)) {
                    if (ImGui::MenuItem("Remove Lightning Component")) {
                        ECSCommand removeCmd = ECSCommand::RemoveComponent<LightningComponent>(entity);
                        if (!ctx.App->ECSCommandRing.Push(removeCmd)) {
                            SM_WARN("ECS command queue full! Remove component command dropped.");
                        }
                    }
                }

                if (ctx.WorldSnapshot->HasComponent<MeshComponent>(entity)) {
                    if (ImGui::MenuItem("Remove Mesh Component")) {
                        ECSCommand removeCmd = ECSCommand::RemoveComponent<MeshComponent>(entity);
                        if (!ctx.App->ECSCommandRing.Push(removeCmd)) {
                            SM_WARN("ECS command queue full! Remove component command dropped.");
                        }
                    }
                }

                if (ctx.WorldSnapshot->HasComponent<MaterialComponent>(entity)) {
                    if (ImGui::MenuItem("Remove Material Component")) {
                        ECSCommand removeCmd = ECSCommand::RemoveComponent<MaterialComponent>(entity);
                        if (!ctx.App->ECSCommandRing.Push(removeCmd)) {
                            SM_WARN("ECS command queue full! Remove component command dropped.");
                        }
                    }
                }

                if (ctx.WorldSnapshot->HasComponent<TextComponent>(entity)) {
                    if (ImGui::MenuItem("Remove Text Component")) {
                        ECSCommand removeCmd = ECSCommand::RemoveComponent<TextComponent>(entity);
                        if (!ctx.App->ECSCommandRing.Push(removeCmd)) {
                            SM_WARN("ECS command queue full! Remove component command dropped.");
                        }
                    }
                }

                if (ctx.WorldSnapshot->HasComponent<SunMarker>(entity)) {
                    if (ImGui::MenuItem("Remove Sun Marker")) {
                        ECSCommand removeCmd = ECSCommand::RemoveComponent<SunMarker>(entity);
                        if (!ctx.App->ECSCommandRing.Push(removeCmd)) {
                            SM_WARN("ECS command queue full! Remove component command dropped.");
                        }
                    }
                }

                if (ctx.WorldSnapshot->HasComponent<PlayerComponent>(entity)) {
                    if (ImGui::MenuItem("Remove Player Component")) {
                        ECSCommand removeCmd = ECSCommand::RemoveComponent<PlayerComponent>(entity);
                        if (!ctx.App->ECSCommandRing.Push(removeCmd)) {
                            SM_WARN("ECS command queue full! Remove component command dropped.");
                        }
                    }
                }

                if (ctx.WorldSnapshot->HasComponent<UIRectComponent>(entity)) {
                    if (ImGui::MenuItem("Remove UI Rect Component")) {
                        ECSCommand removeCmd = ECSCommand::RemoveComponent<UIRectComponent>(entity);
                        if (!ctx.App->ECSCommandRing.Push(removeCmd)) {
                            SM_WARN("ECS command queue full! Remove component command dropped.");
                        }
                    }
                }
                if (ctx.WorldSnapshot->HasComponent<StateScopeComponent>(entity)) {
                    if (ImGui::MenuItem("Remove State Scope Component")) {
                        ECSCommand removeCmd = ECSCommand::RemoveComponent<StateScopeComponent>(entity);
                        if (!ctx.App->ECSCommandRing.Push(removeCmd)) {
                            SM_WARN("ECS command queue full! Remove component command dropped.");
                        }
                    }
                }
                if (ctx.WorldSnapshot->HasComponent<MenuButtonComponent>(entity)) {
                    if (ImGui::MenuItem("Remove Menu Button Component")) {
                        ECSCommand removeCmd = ECSCommand::RemoveComponent<MenuButtonComponent>(entity);
                        if (!ctx.App->ECSCommandRing.Push(removeCmd)) {
                            SM_WARN("ECS command queue full! Remove component command dropped.");
                        }
                    }
                }

                if (ctx.WorldSnapshot->HasComponent<ColliderComponent>(entity)) {
                    if (ImGui::MenuItem("Remove Collider Component")) {
                        ECSCommand removeCmd = ECSCommand::RemoveComponent<ColliderComponent>(entity);
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

            // Edit Transform Component
            if (ctx.WorldSnapshot->HasComponent<TransformComponent>(selectedEntity)) {
                if (ImGui::CollapsingHeader("Transform Component", ImGuiTreeNodeFlags_DefaultOpen)) {
                    auto* transform = ctx.WorldSnapshot->GetComponent<TransformComponent>(selectedEntity);
                    if (transform) {
                        // Reset when switching entities
                        if (lastEditedEntity != selectedEntity) {
                            editTransform = *transform;
                            lastEditedEntity = selectedEntity;
                            transformModified = false;
                        }
                        // Live-refresh from snapshot every frame while not editing,
                        // so game-driven mutations (e.g. day/night) show up in inspector.
                        if (!transformModified) {
                            editTransform = *transform;
                        }

                        // Position editor
                        ImGui::Text("Position:");
                        if (ImGui::InputFloat3("##Position", &editTransform.Position.x)) {
                            transformModified = true;
                        }

                        // Rotation editor (in degrees for user-friendliness)
                        ImGui::Text("Rotation:");
                        glm::vec3 rotationDegrees = glm::degrees(editTransform.Rotation);
                        if (ImGui::InputFloat3("##Rotation", &rotationDegrees.x)) {
                            editTransform.Rotation = glm::radians(rotationDegrees);
                            transformModified = true;
                        }

                        // Scale editor
                        ImGui::Text("Scale:");
                        if (ImGui::InputFloat3("##Scale", &editTransform.Scale.x)) {
                            transformModified = true;
                        }

                        const auto hasTextTransform = ctx.WorldSnapshot->HasComponent<TextComponent>(selectedEntity);
                        // ImGuizmo integration: manipulate this entity's transform using camera
                        if (!hasTextTransform) {
                            glm::mat4 cameraView(1.0f), cameraProjection(1.0f);
                            if (ctx.EditorCameraActive) {
                                cameraView = ctx.EditorCamView;
                                cameraProjection = ctx.EditorCamProj;
                            } else if (const auto* cam = ctx.World ? ctx.World->GetSingleton<WorldCameraComponent>() : nullptr) {
                                cameraView = cam->View;
                                cameraProjection = cam->Projection;
                            }

                            m_Gizmo.DrawControls();

                            // Build model matrix from current editable transform
                            glm::mat4 M = ModelMatrix(editTransform);

                            // Manipulate matrix within this inspector window
                            m_Gizmo.EditTransform(glm::value_ptr(cameraView), glm::value_ptr(cameraProjection), glm::value_ptr(M), ctx);

                            // If user manipulated the gizmo, decompose back into component fields
                            if (ImGuizmo::IsUsing())
                            {
                                float tr[3], rtDeg[3], sc[3];
                                ImGuizmo::DecomposeMatrixToComponents(glm::value_ptr(M), tr, rtDeg, sc);
                                editTransform.Position = glm::vec3(tr[0], tr[1], tr[2]);
                                editTransform.Rotation = glm::radians(glm::vec3(rtDeg[0], rtDeg[1], rtDeg[2]));
                                editTransform.Scale    = glm::vec3(sc[0], sc[1], sc[2]);
                                transformModified = true;
                            }
                        }

                        // Buttons to apply or revert changes
                        ImGui::Spacing();

                        if (transformModified) {
                            ImGui::TextColored(ImVec4(1.0f, 1.0f, 0.0f, 1.0f), "* Modified (not yet saved)");
                        }

                        if (transformModified) {
                            transformModified = false;
                            ECSCommand modifyCmd = ECSCommand::ModifyComponent(selectedEntity, editTransform);
                            if (!ctx.App->ECSCommandRing.Push(modifyCmd)) {
                                SM_WARN("ECS command queue full! Modify command dropped.");
                            }
                        }
                    }
                }
            }

            // Edit Lightning Component
            if (ctx.WorldSnapshot->HasComponent<LightningComponent>(selectedEntity)) {
                if (ImGui::CollapsingHeader("Lightning Component", ImGuiTreeNodeFlags_DefaultOpen)) {
                    auto* lightning = ctx.WorldSnapshot->GetComponent<LightningComponent>(selectedEntity);
                    if (lightning) {
                        // Reset when switching entities
                        if (lastEditedLightningEntity != selectedEntity) {
                            editLightning = *lightning;
                            lastEditedLightningEntity = selectedEntity;
                            lightningModified = false;
                        }
                        // Live-refresh while not editing — exposes day/night cycle changes.
                        if (!lightningModified) {
                            editLightning = *lightning;
                        }
                        // Type editor
                        const char* types[] = { "Directional", "Point", "Spot" };
                        int currentType = static_cast<int>(editLightning.Type);
                        if (ImGui::Combo("Type", &currentType, types, IM_ARRAYSIZE(types))) {
                            editLightning.Type = static_cast<LightningType>(currentType);
                            lightningModified = true;
                        }
                        // Direction editor
                        ImGui::Text("Direction:");
                        if (ImGui::DragFloat3("##Direction", &editLightning.Direction.x, 0.1f, -1.0f, 1.0f)) {
                            lightningModified = true;
                        }
                        // Color editor
                        ImGui::Text("Color:");
                        if (ImGui::ColorEdit4("##Color", &editLightning.Color.r)) {
                            lightningModified = true;
                        }
                        // Intensity editor
                        if (ImGui::DragFloat("Intensity", &editLightning.Intensity, 0.1f, 0.0f, 100.0f)) {
                            lightningModified = true;
                        }
                        // Range editor (for Point and Spot lights)
                        if (editLightning.Type != LightningType::Directional) {
                            if (ImGui::DragFloat("Range", &editLightning.Range, 0.1f, 0.0f, 1000.0f)) {
                                lightningModified = true;
                            }
                        }
                        ImGui::Spacing();

                        if (lightningModified) {
                            ECSCommand modifyCmd = ECSCommand::ModifyComponent(selectedEntity, editLightning);
                            if (!ctx.App->ECSCommandRing.Push(modifyCmd)) {
                                SM_WARN("ECS command queue full! Modify command dropped.");
                            }
                            lightningModified = false;
                        }
                    }
                }
            }

            // Edit Mesh Component
            if (ctx.WorldSnapshot->HasComponent<MeshComponent>(selectedEntity)) {
                if (ImGui::CollapsingHeader("Mesh Component", ImGuiTreeNodeFlags_DefaultOpen)) {
                    auto* mesh = ctx.WorldSnapshot->GetComponent<MeshComponent>(selectedEntity);
                    if (mesh) {
                        // Reset when switching entities
                        if (lastEditedMeshEntity != selectedEntity) {
                            editMesh = *mesh;
                            lastEditedMeshEntity = selectedEntity;
                            meshModified = false;
                        }
                        if (!meshModified) {
                            editMesh = *mesh;
                        }

                        // Mesh ID editor with dropdown
                        if (ctx.MeshSys) {
                            const uint32_t meshCount = ctx.MeshSys->GetMeshCount();
                            if (meshCount > 0) {
                                // Build combo items
                                std::vector<std::string> meshItems;
                                meshItems.reserve(meshCount);
                                for (uint32_t i = 0; i < meshCount; ++i) {
                                    meshItems.push_back("Mesh " + std::to_string(i));
                                }

                                // Current selection
                                int currentMeshIdx = static_cast<int>(editMesh.MeshId);
                                if (currentMeshIdx >= static_cast<int>(meshCount)) {
                                    currentMeshIdx = 0; // Default to first mesh if invalid
                                }

                                // Combo dropdown
                                if (ImGui::BeginCombo("Mesh ID", meshItems[currentMeshIdx].c_str())) {
                                    for (uint32_t i = 0; i < meshCount; ++i) {
                                        const bool isSelected = (currentMeshIdx == static_cast<int>(i));
                                        if (ImGui::Selectable(meshItems[i].c_str(), isSelected)) {
                                            editMesh.MeshId = i;
                                            meshModified = true;
                                        }
                                        if (isSelected) {
                                            ImGui::SetItemDefaultFocus();
                                        }
                                    }
                                    ImGui::EndCombo();
                                }
                            } else {
                                ImGui::TextDisabled("No meshes loaded");
                            }
                        } else {
                            // Fallback if MeshSystem is not available
                            if (ImGui::InputScalar("Mesh ID", ImGuiDataType_U32, &editMesh.MeshId)) {
                                meshModified = true;
                            }
                        }

                        // Visibility toggle
                        if (ImGui::Checkbox("Visible", &editMesh.Visible)) {
                            meshModified = true;
                        }


                        ImGui::Spacing();

                        if (meshModified) {
                            ImGui::TextColored(ImVec4(1.0f, 1.0f, 0.0f, 1.0f), "* Modified (not yet saved)");
                        }

                        if (ImGui::Button("Apply Changes##Mesh", ImVec2(150, 0))) {
                            ECSCommand modifyCmd = ECSCommand::ModifyComponent(selectedEntity, editMesh);
                            if (!ctx.App->ECSCommandRing.Push(modifyCmd)) {
                                SM_WARN("ECS command queue full! Modify command dropped.");
                            }
                            meshModified = false;
                        }
                    }
                }
            }

            // Edit Material Component
            if (ctx.WorldSnapshot->HasComponent<MaterialComponent>(selectedEntity)) {
                if (ImGui::CollapsingHeader("Material Component", ImGuiTreeNodeFlags_DefaultOpen)) {
                    auto* material = ctx.WorldSnapshot->GetComponent<MaterialComponent>(selectedEntity);
                    if (material) {
                        // Reset when switching entities
                        if (lastEditedMaterialEntity != selectedEntity) {
                            editMaterial = *material;
                            lastEditedMaterialEntity = selectedEntity;
                            materialModified = false;
                        }
                        if (!materialModified) {
                            editMaterial = *material;
                        }
                        // Material ID editor — dropdown over MaterialSystem entries.
                        if (ctx.MatSys) {
                            const uint32_t materialCount = ctx.MatSys->GetMaterialCount();
                            if (materialCount > 0) {
                                std::vector<std::string> materialItems;
                                materialItems.reserve(materialCount);
                                for (uint32_t i = 0; i < materialCount; ++i) {
                                    materialItems.push_back("Material " + std::to_string(i));
                                }

                                int currentMaterialIdx = static_cast<int>(editMaterial.MaterialId);
                                if (currentMaterialIdx >= static_cast<int>(materialCount)) {
                                    currentMaterialIdx = 0;
                                }

                                if (ImGui::BeginCombo("Material ID", materialItems[currentMaterialIdx].c_str())) {
                                    for (uint32_t i = 0; i < materialCount; ++i) {
                                        const bool isSelected = (currentMaterialIdx == static_cast<int>(i));
                                        if (ImGui::Selectable(materialItems[i].c_str(), isSelected)) {
                                            editMaterial.MaterialId = i;
                                            materialModified = true;
                                        }
                                        if (isSelected) {
                                            ImGui::SetItemDefaultFocus();
                                        }
                                    }
                                    ImGui::EndCombo();
                                }
                            } else {
                                ImGui::TextDisabled("No materials loaded");
                            }
                        } else {
                            if (ImGui::InputScalar("Material ID", ImGuiDataType_U32, &editMaterial.MaterialId)) {
                                materialModified = true;
                            }
                        }
                        // Base color editor
                        ImGui::Text("Base Color:");
                        if (ImGui::ColorEdit4("##BaseColor", &editMaterial.BaseColor.r)) {
                            materialModified = true;
                        }
                        ImGui::Spacing();

                        // Flags editor - Use Texture checkbox (bit 0)
                        bool useTexture = (editMaterial.Flags & 1u) != 0;
                        if (ImGui::Checkbox("Use Texture", &useTexture)) {
                            if (useTexture) {
                                editMaterial.Flags |= 1u;  // Set bit 0
                            } else {
                                editMaterial.Flags &= ~1u; // Clear bit 0
                            }
                            materialModified = true;
                        }
                        if (ImGui::IsItemHovered()) {
                            ImGui::SetTooltip("Enable texture sampling in shader (requires valid Material ID with texture)");
                        }
                        ImGui::Spacing();

                        if (materialModified) {
                            ECSCommand modifyCmd = ECSCommand::ModifyComponent(selectedEntity, editMaterial);
                            if (!ctx.App->ECSCommandRing.Push(modifyCmd)) {
                                SM_WARN("ECS command queue full! Modify command dropped.");
                            }
                            materialModified = false;
                        }
                    }
                }
            }

            // Edit Text Component
            if (ctx.WorldSnapshot->HasComponent<TextComponent>(selectedEntity)) {
                if (ImGui::CollapsingHeader("Text Component", ImGuiTreeNodeFlags_DefaultOpen)) {
                    auto* textComp = ctx.WorldSnapshot->GetComponent<TextComponent>(selectedEntity);
                    if (textComp) {
                        // Reset when switching entities
                        if (lastEditedTextEntity != selectedEntity) {
                            editTextComp = *textComp;
                            lastEditedTextEntity = selectedEntity;
                            textModified = false;
                        }
                        if (!textModified) {
                            editTextComp = *textComp;
                        }
                        // Text editor
                        char buffer[256];
                        strncpy_s(buffer, editTextComp.Text.c_str(), sizeof(buffer));
                        if (ImGui::InputTextMultiline("Text", buffer, sizeof(buffer))) {
                            editTextComp.Text = std::string(buffer);
                            textModified = true;
                        }
                        ImGui::Spacing();
                        // Text color editor
                        ImGui::Text("Text Color:");
                        if (ImGui::ColorEdit4("##TextColor", &editTextComp.Color.r)) {
                            textModified = true;
                        }
                        ImGui::Spacing();
                        // Font size editor
                        ImGui::Text("Font Size:");
                        int fontSizeInt = static_cast<int>(editTextComp.FontSize);
                        if (ImGui::SliderInt("##FontSize", &fontSizeInt, 6, 72, "%d px")) {
                            editTextComp.FontSize = static_cast<size_t>(fontSizeInt);
                            textModified = true;
                        }
                        ImGui::Spacing();
                        if (textModified) {
                            ImGui::TextColored(ImVec4(1.0f, 1.0f, 0.0f, 1.0f), "* Modified (not yet saved)");
                        }
                        if (ImGui::Button("Apply Changes##Text", ImVec2(150, 0))) {
                            ECSCommand modifyCmd = ECSCommand::ModifyComponent(selectedEntity, editTextComp);
                            if (!ctx.App->ECSCommandRing.Push(modifyCmd)) {
                                SM_WARN("ECS command queue full! Modify command dropped.");
                            }
                            textModified = false;
                        }
                        ImGui::SameLine();
                        if (ImGui::Button("Revert##Text", ImVec2(150, 0))) {
                            editTextComp = *textComp;
                        }
                        ImGui::Separator();
                        // Show original values from snapshot (read-only)
                        ImGui::TextDisabled("Original values from snapshot:");
                        ImGui::TextDisabled("Text: %s", textComp->Text.c_str());
                    }
                }
            }

            // Sun Marker — zero-size tag; display as read-only badge.
            if (ctx.WorldSnapshot->HasComponent<SunMarker>(selectedEntity)) {
                if (ImGui::CollapsingHeader("Sun Marker", ImGuiTreeNodeFlags_DefaultOpen)) {
                    ImGui::TextColored(ImVec4(1.0f, 0.85f, 0.3f, 1.0f), "Tagged as Sun");
                    ImGui::TextDisabled("Day/night cycle drives this entity.");
                }
            }

            // Edit Player Component
            if (ctx.WorldSnapshot->HasComponent<PlayerComponent>(selectedEntity)) {
                if (ImGui::CollapsingHeader("Player Component", ImGuiTreeNodeFlags_DefaultOpen)) {
                    auto* player = ctx.WorldSnapshot->GetComponent<PlayerComponent>(selectedEntity);
                    if (player) {
                        if (lastEditedPlayerEntity != selectedEntity) {
                            editPlayer = *player;
                            lastEditedPlayerEntity = selectedEntity;
                            playerModified = false;
                        }
                        if (!playerModified) {
                            editPlayer = *player;
                        }
                        if (ImGui::DragFloat("Move speed", &editPlayer.MoveSpeed, 0.1f, 0.5f, 50.0f, "%.1f")) {
                            playerModified = true;
                        }
                        ImGui::Spacing();
                        if (playerModified) {
                            ECSCommand modifyCmd = ECSCommand::ModifyComponent(selectedEntity, editPlayer);
                            if (!ctx.App->ECSCommandRing.Push(modifyCmd)) {
                                SM_WARN("ECS command queue full! Modify command dropped.");
                            }
                            playerModified = false;
                        }
                    }
                }
            }

            // Edit UI Rect Component
            if (ctx.WorldSnapshot->HasComponent<UIRectComponent>(selectedEntity)) {
                if (ImGui::CollapsingHeader("UI Rect Component", ImGuiTreeNodeFlags_DefaultOpen)) {
                    auto* rect = ctx.WorldSnapshot->GetComponent<UIRectComponent>(selectedEntity);
                    if (rect) {
                        if (lastEditedUIRectEntity != selectedEntity) {
                            editUIRect = *rect;
                            lastEditedUIRectEntity = selectedEntity;
                            uiRectModified = false;
                        }
                        if (!uiRectModified) {
                            editUIRect = *rect;
                        }
                        if (ImGui::DragFloat2("Size (px)", &editUIRect.Size.x, 1.0f, 1.0f, 4096.0f, "%.0f")) {
                            uiRectModified = true;
                        }
                        if (ImGui::ColorEdit4("Color##UIRect", &editUIRect.Color.x)) {
                            uiRectModified = true;
                        }
                        ImGui::Spacing();
                        if (uiRectModified) {
                            ECSCommand modifyCmd = ECSCommand::ModifyComponent(selectedEntity, editUIRect);
                            if (!ctx.App->ECSCommandRing.Push(modifyCmd)) {
                                SM_WARN("ECS command queue full! Modify command dropped.");
                            }
                            uiRectModified = false;
                        }
                    }
                }
            }

            // Edit State Scope Component (one checkbox per state; mask bit i = GameStateId i)
            if (ctx.WorldSnapshot->HasComponent<StateScopeComponent>(selectedEntity)) {
                if (ImGui::CollapsingHeader("State Scope Component", ImGuiTreeNodeFlags_DefaultOpen)) {
                    auto* scope = ctx.WorldSnapshot->GetComponent<StateScopeComponent>(selectedEntity);
                    if (scope) {
                        if (lastEditedScopeEntity != selectedEntity) {
                            editScope = *scope;
                            lastEditedScopeEntity = selectedEntity;
                            scopeModified = false;
                        }
                        if (!scopeModified) {
                            editScope = *scope;
                        }
                        ImGui::TextDisabled("Active in states (none = always):");
                        struct { const char* label; GameStateId id; } kStates[] = {
                            {"Main Menu", GameStateId::MainMenu},
                            {"In Level",  GameStateId::InLevel},
                            {"In Editor", GameStateId::InEditor},
                            {"Paused",    GameStateId::Paused},
                        };
                        for (const auto& s : kStates) {
                            const uint32_t bit = 1u << static_cast<uint32_t>(s.id);
                            bool on = (editScope.StateMask & bit) != 0u;
                            if (ImGui::Checkbox(s.label, &on)) {
                                if (on) editScope.StateMask |= bit; else editScope.StateMask &= ~bit;
                                scopeModified = true;
                            }
                        }
                        ImGui::Spacing();
                        if (scopeModified) {
                            ECSCommand modifyCmd = ECSCommand::ModifyComponent(selectedEntity, editScope);
                            if (!ctx.App->ECSCommandRing.Push(modifyCmd)) {
                                SM_WARN("ECS command queue full! Modify command dropped.");
                            }
                            scopeModified = false;
                        }
                    }
                }
            }

            // Edit Menu Button Component
            if (ctx.WorldSnapshot->HasComponent<MenuButtonComponent>(selectedEntity)) {
                if (ImGui::CollapsingHeader("Menu Button Component", ImGuiTreeNodeFlags_DefaultOpen)) {
                    auto* btn = ctx.WorldSnapshot->GetComponent<MenuButtonComponent>(selectedEntity);
                    if (btn) {
                        if (lastEditedMenuBtnEntity != selectedEntity) {
                            editMenuBtn = *btn;
                            lastEditedMenuBtnEntity = selectedEntity;
                            menuBtnModified = false;
                        }
                        if (!menuBtnModified) {
                            editMenuBtn = *btn;
                        }
                        static const char* kActionNames[] = { "None", "Play", "Quit", "Back" };
                        static const uint32_t kActionIds[] = { Actions::None, Actions::Play, Actions::Quit, Actions::Back };
                        int curIdx = 0;
                        for (int i = 0; i < 4; ++i) if (editMenuBtn.ActionId == kActionIds[i]) curIdx = i;
                        if (ImGui::Combo("Action", &curIdx, kActionNames, 4)) {
                            editMenuBtn.ActionId = kActionIds[curIdx];
                            menuBtnModified = true;
                        }
                        if (ImGui::ColorEdit4("Normal##MenuBtn", &editMenuBtn.Normal.x)) menuBtnModified = true;
                        if (ImGui::ColorEdit4("Hover##MenuBtn",  &editMenuBtn.Hover.x))  menuBtnModified = true;
                        if (ImGui::ColorEdit4("Press##MenuBtn",  &editMenuBtn.Press.x))  menuBtnModified = true;
                        ImGui::Spacing();
                        if (menuBtnModified) {
                            ECSCommand modifyCmd = ECSCommand::ModifyComponent(selectedEntity, editMenuBtn);
                            if (!ctx.App->ECSCommandRing.Push(modifyCmd)) {
                                SM_WARN("ECS command queue full! Modify command dropped.");
                            }
                            menuBtnModified = false;
                        }
                    }
                }
            }

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
