#pragma once
#include "IComponentEditor.h"
class NavConstrainedEditor final : public IComponentEditor {
public:
    const char* Label() const override { return "NavMesh Constrained"; }
    bool Has(const ECS& s, EntityId e) const override { return s.HasComponent<NavConstrainedComponent>(e); }
    void AddDefault(const EditorContext& ctx, EntityId e) override;
    void Remove(const EditorContext& ctx, EntityId e) override;
    void DrawEditor(const EditorContext& ctx, EntityId e) override;
};
