#pragma once
#include <memory>
#include <vector>
#include "ECS.h"
#include "inspector/IComponentEditor.h"

struct EditorContext;

class EcsInspectorPanel {
public:
    EcsInspectorPanel();
    void Draw(const EditorContext& ctx);
    void SetSelectedEntity(EntityId e) { selectedEntity = e; }
    EntityId GetSelectedEntity() const { return selectedEntity; }
private:
    EntityId selectedEntity = INVALID_ENTITY;
    std::vector<std::unique_ptr<IComponentEditor>> m_Editors;

    // --- Un-migrated component edit-state (removed as each batch lands) ---
    // Batch C (Task 4):
    ColliderComponent  editCollider{};      EntityId lastEditedColliderEntity = INVALID_ENTITY;  bool colliderModified = false;
    NavMeshSourceComponent editNavSource{}; EntityId lastEditedNavSourceEntity = INVALID_ENTITY;  bool navSourceModified = false;
    NavObstacleComponent editNavObstacle{}; EntityId lastEditedNavObstacleEntity = INVALID_ENTITY; bool navObstacleModified = false;
    NavAgentComponent  editNavAgent{};      EntityId lastEditedNavAgentEntity = INVALID_ENTITY;  bool navAgentModified = false;
    NavTargetComponent editNavTarget{};     EntityId lastEditedNavTargetEntity = INVALID_ENTITY; bool navTargetModified = false;
};
