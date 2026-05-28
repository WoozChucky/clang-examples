#pragma once
#include "IComponentEditor.h"
#include "EditState.h"
class ColliderEditor final : public IComponentEditor {
    EditState<ColliderComponent> m_St;
public:
    const char* Label() const override { return "Collider Component"; }
    bool Has(const ECS& s, EntityId e) const override { return s.HasComponent<ColliderComponent>(e); }
    void AddDefault(const EditorContext& ctx, EntityId e) override;
    void Remove(const EditorContext& ctx, EntityId e) override;
    void DrawEditor(const EditorContext& ctx, EntityId e) override;
};
