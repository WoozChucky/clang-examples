#pragma once
#include "IComponentEditor.h"
#include "EditState.h"
class NavTargetEditor final : public IComponentEditor {
    EditState<NavTargetComponent> m_St;
public:
    const char* Label() const override { return "NavMesh Target Component"; }
    bool Has(const ECS& s, EntityId e) const override { return s.HasComponent<NavTargetComponent>(e); }
    void AddDefault(const EditorContext& ctx, EntityId e) override;
    void Remove(const EditorContext& ctx, EntityId e) override;
    void DrawEditor(const EditorContext& ctx, EntityId e) override;
};
