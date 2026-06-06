#include "ComponentSerializerRegistry.h"
#include "ComponentSerialization.h"  // built-in to_json/from_json for the persisted types
#include "lib.h"                      // SM_WARN

ComponentSerializerRegistry& SerializerRegistry() {
    static ComponentSerializerRegistry reg = [] {
        ComponentSerializerRegistry r;
        // The persisted built-in set (mirrors WorldManager's previous explicit lists).
        r.Register<TransformComponent>("TransformComponent");
        r.Register<MeshComponent>("MeshComponent");
        r.Register<MaterialComponent>("MaterialComponent");
        r.Register<LightningComponent>("LightningComponent");
        r.Register<TextComponent>("TextComponent");
        r.Register<SunMarker>("SunMarker");
        r.Register<PlayerComponent>("PlayerComponent");
        r.Register<UIRectComponent>("UIRectComponent");
        r.Register<StateScopeComponent>("StateScopeComponent");
        r.Register<MenuButtonComponent>("MenuButtonComponent");
        r.Register<ColliderComponent>("ColliderComponent");
        r.Register<NavMeshSourceComponent>("NavMeshSourceComponent");
        r.Register<NavObstacleComponent>("NavObstacleComponent");
        r.Register<NavAgentComponent>("NavAgentComponent");
        r.Register<NavTargetComponent>("NavTargetComponent");
        r.Register<NavConstrainedComponent>("NavConstrainedComponent");
        r.Register<NavClassComponent>("NavClassComponent");
        return r;
    }();
    return reg;
}

void SaveEntityComponents(const ECS& world, EntityId e, nlohmann::json& jEntity) {
    for (const auto& en : SerializerRegistry().Entries())
        if (en.has(world, e)) en.save(world, e, jEntity[en.name]);
}

void LoadEntityComponents(ECS& world, EntityId e, const nlohmann::json& jEntity) {
    for (auto it = jEntity.begin(); it != jEntity.end(); ++it) {
        if (it.key() == "EntityId") continue;
        if (const auto* en = SerializerRegistry().Find(it.key()))
            en->load(world, e, it.value());
        else
            SM_WARN("LoadEntityComponents: no serializer for component '%s' — skipped", it.key().c_str());
    }
}
