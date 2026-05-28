#pragma once
#include "ECS.h"                 // EntityId, INVALID_ENTITY, component types
#include "GizmoController.h"
struct EditorContext;

// Draws the "ECS Inspector & Editor" window: entity list, create/select/delete, component
// add/remove, and per-component editors (Transform incl. gizmo, Lightning, Mesh, Material, Text,
// Sun marker). Mutations are issued as ECSCommands via EditorContext::App.
class EcsInspectorPanel {
public:
    void Draw(const EditorContext& ctx);
    void SetSelectedEntity(EntityId e) { selectedEntity = e; }
    EntityId GetSelectedEntity() const { return selectedEntity; }
private:
    // Persistent selection — formerly the function-local `static EntityId selectedEntity`.
    EntityId selectedEntity = INVALID_ENTITY;

    // Per-component working copies + "last edited" trackers + dirty flags. Each of these was a
    // function-local `static` in the original inspector block, so they must persist across frames
    // here as members to preserve the exact edit/refresh semantics.
    TransformComponent editTransform{};
    EntityId           lastEditedEntity = INVALID_ENTITY;
    bool               transformModified = false;

    LightningComponent editLightning{};
    EntityId           lastEditedLightningEntity = INVALID_ENTITY;
    bool               lightningModified = false;

    MeshComponent      editMesh{};
    EntityId           lastEditedMeshEntity = INVALID_ENTITY;
    bool               meshModified = false;

    MaterialComponent  editMaterial{};
    EntityId           lastEditedMaterialEntity = INVALID_ENTITY;
    bool               materialModified = false;

    TextComponent      editTextComp{};
    EntityId           lastEditedTextEntity = INVALID_ENTITY;
    bool               textModified = false;

    PlayerComponent    editPlayer{};
    EntityId           lastEditedPlayerEntity = INVALID_ENTITY;
    bool               playerModified = false;

    UIRectComponent editUIRect{};
    EntityId        lastEditedUIRectEntity = INVALID_ENTITY;
    bool            uiRectModified = false;

    StateScopeComponent editScope{};
    EntityId            lastEditedScopeEntity = INVALID_ENTITY;
    bool                scopeModified = false;

    MenuButtonComponent editMenuBtn{};
    EntityId            lastEditedMenuBtnEntity = INVALID_ENTITY;
    bool                menuBtnModified = false;

    ColliderComponent editCollider{};
    EntityId          lastEditedColliderEntity = INVALID_ENTITY;
    bool              colliderModified = false;

    NavMeshSourceComponent editNavSource{};
    EntityId               lastEditedNavSourceEntity = INVALID_ENTITY;
    bool                   navSourceModified = false;

    NavObstacleComponent editNavObstacle{};
    EntityId             lastEditedNavObstacleEntity = INVALID_ENTITY;
    bool                 navObstacleModified = false;

    NavAgentComponent  editNavAgent{};
    EntityId           lastEditedNavAgentEntity = INVALID_ENTITY;
    bool               navAgentModified = false;

    NavTargetComponent editNavTarget{};
    EntityId           lastEditedNavTargetEntity = INVALID_ENTITY;
    bool               navTargetModified = false;

    GizmoController    m_Gizmo;
};
