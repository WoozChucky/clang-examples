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

std::vector<PreservedComponent> PreserveNonBuiltinComponents(const ECS& world) {
    std::vector<PreservedComponent> out;
    auto capture = [&](const ComponentSerializerEntry& entry, EntityId e) {
        if (!entry.has(world, e)) return;
        if (entry.reloadExtract) {
            PreservedComponent pc;
            pc.name = entry.name; pc.entity = e; pc.useBytes = true;
            entry.reloadExtract(world, e, pc.bytes);
            out.push_back(std::move(pc));
        } else if (entry.save) {
            PreservedComponent pc;
            pc.name = entry.name; pc.entity = e; pc.useBytes = false;
            entry.save(world, e, pc.json);
            out.push_back(std::move(pc));
        } else {
            SM_WARN("PreserveNonBuiltinComponents: '%s' not preservable "
                    "(non-trivially-copyable, no serializer) — dropped on reload", entry.name.c_str());
        }
    };
    for (const auto& entry : SerializerRegistry().Entries()) {
        if (entry.builtin) continue;
        for (const EntityId e : world.GetActiveEntities()) capture(entry, e);
        capture(entry, world.SingletonEntity());   // singleton entity is invisible to GetActiveEntities()
    }
    return out;
}

void RestoreNonBuiltinComponents(ECS& world, const std::vector<PreservedComponent>& blob) {
    for (const auto& pc : blob) {
        if (pc.entity != world.SingletonEntity() && !world.IsValidEntity(pc.entity)) {
            SM_WARN("RestoreNonBuiltinComponents: entity %llu no longer valid — '%s' dropped",
                    static_cast<unsigned long long>(pc.entity), pc.name.c_str());
            continue;
        }
        const auto* entry = SerializerRegistry().Find(pc.name);
        if (!entry) {
            SM_WARN("RestoreNonBuiltinComponents: '%s' not registered after reload — dropped", pc.name.c_str());
            continue;
        }
        if (pc.useBytes && entry->reloadIngest) entry->reloadIngest(world, pc.entity, pc.bytes);
        else if (!pc.useBytes && entry->load)   entry->load(world, pc.entity, pc.json);
        else SM_WARN("RestoreNonBuiltinComponents: '%s' has no matching strategy after reload — dropped", pc.name.c_str());
    }
}

StateNameRegistry& StateNames() {
    static StateNameRegistry reg;
    return reg;
}
