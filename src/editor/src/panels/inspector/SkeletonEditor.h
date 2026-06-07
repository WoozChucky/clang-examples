#pragma once
#include "IComponentEditor.h"
#include "EditState.h"
class SkeletonEditor final : public IComponentEditor {
    EditState<SkeletonComponent> m_St;
public:
    const char* Label() const override { return "Skeleton Component"; }
    bool Has(const ECS& s, EntityId e) const override { return s.HasComponent<SkeletonComponent>(e); }
    void AddDefault(const EditorContext& ctx, EntityId e) override;
    void Remove(const EditorContext& ctx, EntityId e) override;
    void DrawEditor(const EditorContext& ctx, EntityId e) override;
};
