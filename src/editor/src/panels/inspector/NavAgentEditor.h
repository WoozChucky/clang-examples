#pragma once
#include "IComponentEditor.h"
#include "EditState.h"
class NavAgentEditor final : public IComponentEditor {
    EditState<NavAgentComponent> m_St;
public:
    const char* Label() const override { return "NavMesh Agent Component"; }
    bool Has(const ECS& s, EntityId e) const override { return s.HasComponent<NavAgentComponent>(e); }
    void AddDefault(const EditorContext& ctx, EntityId e) override;
    void Remove(const EditorContext& ctx, EntityId e) override;
    void DrawEditor(const EditorContext& ctx, EntityId e) override;
};
