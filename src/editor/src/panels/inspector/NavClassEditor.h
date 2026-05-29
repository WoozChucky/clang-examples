#pragma once
#include "IComponentEditor.h"
#include "EditState.h"
class NavClassEditor final : public IComponentEditor {
    EditState<NavClassComponent> m_St;
public:
    const char* Label() const override { return "NavMesh Class Component"; }
    bool Has(const ECS& s, EntityId e) const override { return s.HasComponent<NavClassComponent>(e); }
    void AddDefault(const EditorContext& ctx, EntityId e) override;
    void Remove(const EditorContext& ctx, EntityId e) override;
    void DrawEditor(const EditorContext& ctx, EntityId e) override;
};
