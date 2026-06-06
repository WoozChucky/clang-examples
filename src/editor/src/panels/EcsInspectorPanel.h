#pragma once
#include <memory>
#include <vector>
#include "ECS.h"
#include "inspector/IComponentEditor.h"
#include "inspector/GenericComponentEditor.h"

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
    GenericComponentEditor m_GenericEditor;
};
