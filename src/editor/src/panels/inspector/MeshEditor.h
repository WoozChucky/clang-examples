#pragma once
#include "IComponentEditor.h"
#include "EditState.h"
class MeshEditor final : public IComponentEditor {
    EditState<MeshComponent> m_St;
public:
    const char* Label() const override { return "Mesh Component"; }
    bool Has(const ECS& s, EntityId e) const override { return s.HasComponent<MeshComponent>(e); }
    void AddDefault(const EditorContext& ctx, EntityId e) override;
    void Remove(const EditorContext& ctx, EntityId e) override;
    void DrawEditor(const EditorContext& ctx, EntityId e) override;
};
