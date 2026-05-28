#pragma once
#include "IComponentEditor.h"
class SunMarkerEditor final : public IComponentEditor {
public:
    const char* Label() const override { return "Sun Marker"; }
    bool Has(const ECS& s, EntityId e) const override { return s.HasComponent<SunMarker>(e); }
    void AddDefault(const EditorContext& ctx, EntityId e) override;
    void Remove(const EditorContext& ctx, EntityId e) override;
    void DrawEditor(const EditorContext& ctx, EntityId e) override;
};
