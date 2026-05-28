#pragma once
#include "IComponentEditor.h"
#include "EditState.h"
class NavObstacleEditor final : public IComponentEditor {
    EditState<NavObstacleComponent> m_St;
public:
    const char* Label() const override { return "NavMesh Obstacle Component"; }
    bool Has(const ECS& s, EntityId e) const override { return s.HasComponent<NavObstacleComponent>(e); }
    void AddDefault(const EditorContext& ctx, EntityId e) override;
    void Remove(const EditorContext& ctx, EntityId e) override;
    void DrawEditor(const EditorContext& ctx, EntityId e) override;
};
