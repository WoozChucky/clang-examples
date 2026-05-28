#pragma once
#include "IComponentEditor.h"
#include "EditState.h"
class MaterialEditor final : public IComponentEditor {
    EditState<MaterialComponent> m_St;
public:
    const char* Label() const override { return "Material Component"; }
    bool Has(const ECS& s, EntityId e) const override { return s.HasComponent<MaterialComponent>(e); }
    void AddDefault(const EditorContext& ctx, EntityId e) override;
    void Remove(const EditorContext& ctx, EntityId e) override;
    void DrawEditor(const EditorContext& ctx, EntityId e) override;
};
