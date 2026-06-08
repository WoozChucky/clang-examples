#pragma once
#include "IComponentEditor.h"
#include "EditState.h"
class AnimationEditor final : public IComponentEditor {
    EditState<AnimationComponent> m_St;
public:
    const char* Label() const override { return "Animation Component"; }
    bool Has(const ECS& s, EntityId e) const override { return s.HasComponent<AnimationComponent>(e); }
    void AddDefault(const EditorContext& ctx, EntityId e) override;
    void Remove(const EditorContext& ctx, EntityId e) override;
    void DrawEditor(const EditorContext& ctx, EntityId e) override;
};
