#pragma once
#include "IComponentEditor.h"
#include "EditState.h"
class UIRectEditor final : public IComponentEditor {
    EditState<UIRectComponent> m_St;
public:
    const char* Label() const override { return "UI Rect Component"; }
    bool Has(const ECS& s, EntityId e) const override { return s.HasComponent<UIRectComponent>(e); }
    void AddDefault(const EditorContext& ctx, EntityId e) override;
    void Remove(const EditorContext& ctx, EntityId e) override;
    void DrawEditor(const EditorContext& ctx, EntityId e) override;
};
