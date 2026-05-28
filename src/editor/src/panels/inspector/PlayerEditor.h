#pragma once
#include "IComponentEditor.h"
#include "EditState.h"
class PlayerEditor final : public IComponentEditor {
    EditState<PlayerComponent> m_St;
public:
    const char* Label() const override { return "Player Component"; }
    bool Has(const ECS& s, EntityId e) const override { return s.HasComponent<PlayerComponent>(e); }
    void AddDefault(const EditorContext& ctx, EntityId e) override;
    void Remove(const EditorContext& ctx, EntityId e) override;
    void DrawEditor(const EditorContext& ctx, EntityId e) override;
};
