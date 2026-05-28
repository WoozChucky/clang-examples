#pragma once
#include "IComponentEditor.h"
#include "EditState.h"
class StateScopeEditor final : public IComponentEditor {
    EditState<StateScopeComponent> m_St;
public:
    const char* Label() const override { return "State Scope Component"; }
    bool Has(const ECS& s, EntityId e) const override { return s.HasComponent<StateScopeComponent>(e); }
    void AddDefault(const EditorContext& ctx, EntityId e) override;
    void Remove(const EditorContext& ctx, EntityId e) override;
    void DrawEditor(const EditorContext& ctx, EntityId e) override;
};
