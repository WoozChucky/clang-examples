#include "ComponentSerializerRegistry.h"
#include "ComponentSerialization.h"  // built-in to_json/from_json for the persisted types
#include "StateNameRegistry.h"        // game-registered state bit-index -> label table
#include "lib.h"                      // SM_WARN

ComponentSerializerRegistry& SerializerRegistry() {
    static ComponentSerializerRegistry reg = [] {
        ComponentSerializerRegistry r;
        // The persisted built-in set (mirrors WorldManager's previous explicit lists).
        r.Register<TransformComponent>("TransformComponent", true);
        r.Register<MeshComponent>("MeshComponent", true);
        r.Register<MaterialComponent>("MaterialComponent", true);
        r.Register<LightningComponent>("LightningComponent", true);
        r.Register<TextComponent>("TextComponent", true);
        r.Register<SunMarker>("SunMarker", true);
        r.Register<PlayerComponent>("PlayerComponent", true);
        r.Register<UIRectComponent>("UIRectComponent", true);
        r.Register<StateScopeComponent>("StateScopeComponent", true);
        r.Register<MenuButtonComponent>("MenuButtonComponent", true);
        r.Register<ColliderComponent>("ColliderComponent", true);
        r.Register<NavMeshSourceComponent>("NavMeshSourceComponent", true);
        r.Register<NavObstacleComponent>("NavObstacleComponent", true);
        r.Register<NavAgentComponent>("NavAgentComponent", true);
        r.Register<NavTargetComponent>("NavTargetComponent", true);
        r.Register<NavConstrainedComponent>("NavConstrainedComponent", true);
        r.Register<NavClassComponent>("NavClassComponent", true);
        return r;
    }();
    return reg;
}

void ComponentSerializerRegistry::RegisterEditorHook(const std::string& name,
                                                     bool (*draw)(const EditorUI&, nlohmann::json&)) {
    for (auto& e : m_Entries) { if (e.name == name) { e.editorDraw = draw; return; } }
    SM_WARN("RegisterEditorHook: no serializer registered for '%s' — hook ignored", name.c_str());
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

StateNameRegistry& StateNames() {
    static StateNameRegistry reg;
    return reg;
}
