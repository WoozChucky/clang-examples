// src/editor/src/panels/inspector/IComponentEditor.h
#pragma once
#include "ECS.h"   // EntityId

struct EditorContext;
class  ECS;

// One editor per inspector-editable component type. Registered in
// EcsInspectorPanel's constructor (in display order) and iterated for the
// add-menu, remove-menu, and editor sections. Each concrete editor owns its
// own per-frame EditState. GUI-thread only (drawn on the ImGui overlay).
class IComponentEditor {
public:
    virtual ~IComponentEditor() = default;
    virtual const char* Label() const = 0;                              // menu + header text
    virtual bool Has(const ECS& snap, EntityId e) const = 0;            // HasComponent<T>
    virtual void AddDefault(const EditorContext& ctx, EntityId e) = 0;  // push AddComponent
    virtual void Remove(const EditorContext& ctx, EntityId e) = 0;      // push RemoveComponent<T>
    virtual void DrawEditor(const EditorContext& ctx, EntityId e) = 0;  // widgets + edit-state
};
