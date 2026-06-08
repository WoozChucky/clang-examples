#pragma once
#include "IComponentEditor.h"
#include "EditState.h"
class AnimatorEditor final : public IComponentEditor {
    EditState<AnimatorComponent> m_St;
public:
    const char* Label() const override { return "Animator Component"; }
    bool Has(const ECS& s, EntityId e) const override { return s.HasComponent<AnimatorComponent>(e); }
    void AddDefault(const EditorContext& ctx, EntityId e) override;
    void Remove(const EditorContext& ctx, EntityId e) override;
    void DrawEditor(const EditorContext& ctx, EntityId e) override;
};
