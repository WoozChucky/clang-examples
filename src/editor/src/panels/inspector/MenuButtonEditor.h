#pragma once
#include "IComponentEditor.h"
#include "EditState.h"
class MenuButtonEditor final : public IComponentEditor {
    EditState<MenuButtonComponent> m_St;
public:
    const char* Label() const override { return "Menu Button Component"; }
    bool Has(const ECS& s, EntityId e) const override { return s.HasComponent<MenuButtonComponent>(e); }
    void AddDefault(const EditorContext& ctx, EntityId e) override;
    void Remove(const EditorContext& ctx, EntityId e) override;
    void DrawEditor(const EditorContext& ctx, EntityId e) override;
};
