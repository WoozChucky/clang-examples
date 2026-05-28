#pragma once
#include "IComponentEditor.h"
#include "EditState.h"
class LightningEditor final : public IComponentEditor {
    EditState<LightningComponent> m_St;
public:
    const char* Label() const override { return "Lightning Component"; }
    bool Has(const ECS& s, EntityId e) const override { return s.HasComponent<LightningComponent>(e); }
    void AddDefault(const EditorContext& ctx, EntityId e) override;
    void Remove(const EditorContext& ctx, EntityId e) override;
    void DrawEditor(const EditorContext& ctx, EntityId e) override;
};
