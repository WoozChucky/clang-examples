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
    // Rename field state for the selected entity. m_RenameBuf mirrors the selected entity's
    // NameComponent (re-synced when the selection changes, tracked by m_RenameBufFor so live
    // typing isn't clobbered). m_FocusRename = one-shot: focus the field next frame (F2).
    char m_RenameBuf[128] = {};
    EntityId m_RenameBufFor = INVALID_ENTITY;
    bool m_FocusRename = false;
    std::vector<std::unique_ptr<IComponentEditor>> m_Editors;
    GenericComponentEditor m_GenericEditor;
};
