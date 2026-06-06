#include "WorldManager.h"

#include <fstream>

#include "lib.h"

#include <nlohmann/json.hpp>

#include "ComponentSerialization.h" // shared component json (de)serializers
#include "ComponentSerializerRegistry.h"
#include "navigation/NavMeshSystem.h"

using json = nlohmann::json;

bool WorldManager::SaveWorldSnapshot(const std::string& filepath, const ECS* world) {

    std::ofstream ofs(filepath, std::ios::binary);
    if (!ofs.is_open()) {
        SM_WARN("");
        return false;
    }

    const auto& entities = world->GetActiveEntities();
    size_t entityCount = entities.size();

    // Use nlohmann/json for serialization
    json j;
    j["EntityCount"] = entityCount;
    j["Entities"] = json::array();
    for (const auto& entity : entities) {
        json jEntity;
        jEntity["EntityId"] = entity;

        // Generic: every registered (built-in + game-registered) component present on this
        // entity is written under its registered name. See ComponentSerializerRegistry.h.
        SaveEntityComponents(*world, entity, jEntity);

        j["Entities"].push_back(jEntity);
    }

    // Top-level scene atmosphere + nav config (singletons live on the hidden reserved entity).
    {
        FogComponent fog{};
        SkyComponent sky{};
        DayNightConfigComponent dayNight{};
        NavMeshConfigComponent navmesh{};
        if (const auto* f = world->GetSingleton<FogComponent>())            fog      = *f;
        if (const auto* s = world->GetSingleton<SkyComponent>())            sky      = *s;
        if (const auto* d = world->GetSingleton<DayNightConfigComponent>()) dayNight = *d;
        if (const auto* n = world->GetSingleton<NavMeshConfigComponent>())  navmesh  = *n;
        j["Environment"] = BuildEnvironmentJson(fog, sky, dayNight, navmesh);
    }

    ofs << j.dump(4); // Pretty-print with 4-space indentation

    ofs.close();

    return true;
}

bool WorldManager::LoadWorldSnapshot(const std::string& filepath, ECS* world) {

    std::ifstream ifs(filepath, std::ios::binary);
    if (!ifs.is_open()) {
        SM_WARN("Failed to open world snapshot file for reading: %s", filepath.c_str());
        return false;
    }

    try {
        // Parse + validate the top-level shape BEFORE mutating the world, so a corrupt file leaves
        // the current world intact (no wipe) and never throws out of this function (no crash).
        json j;
        ifs >> j;
        const size_t entityCount = j.at("EntityCount").get<size_t>();
        (void)entityCount; // currently informational; entities are recreated from the array below

        world->Clear();
        for (const auto& jEntity : j.at("Entities")) {
            const EntityId createdEntity = world->CreateEntity();
            // Generic: load every component key (skips "EntityId", warns on unknown keys).
            LoadEntityComponents(*world, createdEntity, jEntity);
        }

        // Apply scene atmosphere + nav config if present. Singletons survive Clear(), so when
        // the block is absent (old world.json) the seeded defaults are left untouched.
        const EnvironmentData env = ParseEnvironmentJson(j);
        if (env.HasFog)            world->SetSingleton(env.Fog);
        if (env.HasSky)            world->SetSingleton(env.Sky);
        if (env.HasDayNight)       world->SetSingleton(env.DayNight);
        if (env.HasNavMeshConfig)  world->SetSingleton(env.NavMeshConfig);
    } catch (const std::exception& e) {
        SM_WARN("Failed to load world snapshot '%s': %s", filepath.c_str(), e.what());
        return false;
    }

    // Spec 4: tell NavMeshSystem the world path so auto-bake (in Rebuild) and
    // SaveCurrentToDisk (Bake button) can derive the sidecar path. Done at the
    // end of load so it only happens on success.
    NavMeshSystem::Instance().SetWorldPath(filepath);

    return true;
}
