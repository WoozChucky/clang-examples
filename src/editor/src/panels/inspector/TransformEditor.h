#pragma once
#include "IComponentEditor.h"
#include "EditState.h"
#include "GizmoController.h"   // viewport/ is on the editor include path (Spec A)
class TransformEditor final : public IComponentEditor {
    EditState<TransformComponent> m_St;
    GizmoController m_Gizmo;
public:
    const char* Label() const override { return "Transform Component"; }
    bool Has(const ECS& s, EntityId e) const override { return s.HasComponent<TransformComponent>(e); }
    void AddDefault(const EditorContext& ctx, EntityId e) override;
    void Remove(const EditorContext& ctx, EntityId e) override;
    void DrawEditor(const EditorContext& ctx, EntityId e) override;
};
