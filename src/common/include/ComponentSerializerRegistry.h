#pragma once
#include <string>
#include <vector>

#include <nlohmann/json.hpp>
#include "ECS.h"   // ECS, EntityId, ECS_API

// Registry of per-component (de)serializers keyed by a stable string name (the world.json
// key). Built-in component types register once in ecs.dll; Game.dll registers its own types
// additively — header-instantiable ECS<T> methods (boundary Piece 1) make the <T> lambdas
// compile in whatever module registers the type. A single exported instance is shared by all
// modules.
//
// Entry function pointers are CAPTURELESS lambdas compiled in the registering module:
//   has(world, e)       -> does entity e have this component?
//   save(world, e, out) -> out = the component's json value (caller keys it by `name`)
//   load(world, e, in)  -> AddComponent<T>(e, in.get<T>())
struct ComponentSerializerEntry {
    std::string name;
    bool (*has)(const ECS&, EntityId);
    void (*save)(const ECS&, EntityId, nlohmann::json&);
    void (*load)(ECS&, EntityId, const nlohmann::json&);
};

class ComponentSerializerRegistry {
public:
    // Upsert: re-registering an existing name REPLACES its function pointers. Required for
    // Game.dll hot-reload — a reloaded DLL re-registers its types so the registry never holds
    // dangling pointers into the unloaded module.
    template <class T>
    void Register(const std::string& name) {
        ComponentSerializerEntry e{
            name,
            [](const ECS& w, EntityId en)                        { return w.HasComponent<T>(en); },
            [](const ECS& w, EntityId en, nlohmann::json& out)   { out = *w.GetComponent<T>(en); },
            [](ECS& w, EntityId en, const nlohmann::json& in)    { w.AddComponent<T>(en, in.template get<T>()); }
        };
        for (auto& existing : m_Entries) {
            if (existing.name == name) { existing = e; return; }
        }
        m_Entries.push_back(std::move(e));
    }

    [[nodiscard]] const std::vector<ComponentSerializerEntry>& Entries() const { return m_Entries; }

    [[nodiscard]] const ComponentSerializerEntry* Find(const std::string& name) const {
        for (const auto& e : m_Entries) if (e.name == name) return &e;
        return nullptr;
    }

private:
    std::vector<ComponentSerializerEntry> m_Entries;
};

// Single process-wide instance (defined + exported from ecs.dll). Lazily registers all
// built-in persisted component serializers on first use.
ECS_API ComponentSerializerRegistry& SerializerRegistry();

// Generic per-entity (de)serialization used by WorldManager (and tests). Save writes each
// present component under its registered name into `jEntity`. Load reads every key (skipping
// "EntityId") through the registry, warning on an unknown component key.
ECS_API void SaveEntityComponents(const ECS& world, EntityId e, nlohmann::json& jEntity);
ECS_API void LoadEntityComponents(ECS& world, EntityId e, const nlohmann::json& jEntity);
