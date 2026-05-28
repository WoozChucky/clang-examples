#pragma once
#include "IComponentEditor.h"
#include "EditState.h"
class TextEditor final : public IComponentEditor {
    EditState<TextComponent> m_St;
public:
    const char* Label() const override { return "Text Component"; }
    bool Has(const ECS& s, EntityId e) const override { return s.HasComponent<TextComponent>(e); }
    void AddDefault(const EditorContext& ctx, EntityId e) override;
    void Remove(const EditorContext& ctx, EntityId e) override;
    void DrawEditor(const EditorContext& ctx, EntityId e) override;
};
