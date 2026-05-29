#include "WorldManager.h"

#include <fstream>

#include "lib.h"

#include <nlohmann/json.hpp>

#include "ComponentSerialization.h" // shared component json (de)serializers
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

        // Serialize components if they exist
        if (world->HasComponent<TransformComponent>(entity)) {
            jEntity["TransformComponent"] = *(world->GetComponent<TransformComponent>(entity));
        }
        if (world->HasComponent<MeshComponent>(entity)) {
            jEntity["MeshComponent"] = *(world->GetComponent<MeshComponent>(entity));
        }
        if (world->HasComponent<MaterialComponent>(entity)) {
            jEntity["MaterialComponent"] = *(world->GetComponent<MaterialComponent>(entity));
        }
        if (world->HasComponent<LightningComponent>(entity)) {
            jEntity["LightningComponent"] = *(world->GetComponent<LightningComponent>(entity));
        }
        if (world->HasComponent<TextComponent>(entity)) {
            jEntity["TextComponent"] = *(world->GetComponent<TextComponent>(entity));
        }
        if (world->HasComponent<SunMarker>(entity)) {
            jEntity["SunMarker"] = *(world->GetComponent<SunMarker>(entity));
        }
        if (world->HasComponent<PlayerComponent>(entity)) {
            jEntity["PlayerComponent"] = *(world->GetComponent<PlayerComponent>(entity));
        }
        if (world->HasComponent<UIRectComponent>(entity)) {
            jEntity["UIRectComponent"] = *(world->GetComponent<UIRectComponent>(entity));
        }
        if (world->HasComponent<StateScopeComponent>(entity)) {
            jEntity["StateScopeComponent"] = *(world->GetComponent<StateScopeComponent>(entity));
        }
        if (world->HasComponent<MenuButtonComponent>(entity)) {
            jEntity["MenuButtonComponent"] = *(world->GetComponent<MenuButtonComponent>(entity));
        }
        if (world->HasComponent<ColliderComponent>(entity)) {
            jEntity["ColliderComponent"] = *(world->GetComponent<ColliderComponent>(entity));
        }
        if (world->HasComponent<NavMeshSourceComponent>(entity)) {
            jEntity["NavMeshSourceComponent"] = *(world->GetComponent<NavMeshSourceComponent>(entity));
        }
        if (world->HasComponent<NavObstacleComponent>(entity)) {
            jEntity["NavObstacleComponent"] = *(world->GetComponent<NavObstacleComponent>(entity));
        }
        if (world->HasComponent<NavAgentComponent>(entity)) {
            jEntity["NavAgentComponent"] = *(world->GetComponent<NavAgentComponent>(entity));
        }
        if (world->HasComponent<NavTargetComponent>(entity)) {
            jEntity["NavTargetComponent"] = *(world->GetComponent<NavTargetComponent>(entity));
        }
        if (world->HasComponent<NavConstrainedComponent>(entity)) {
            jEntity["NavConstrainedComponent"] = *(world->GetComponent<NavConstrainedComponent>(entity));
        }
        if (world->HasComponent<NavClassComponent>(entity)) {
            jEntity["NavClassComponent"] = *(world->GetComponent<NavClassComponent>(entity));
        }

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

            if (jEntity.contains("TransformComponent"))
                world->AddComponent(createdEntity, jEntity["TransformComponent"].get<TransformComponent>());
            if (jEntity.contains("MeshComponent"))
                world->AddComponent(createdEntity, jEntity["MeshComponent"].get<MeshComponent>());
            if (jEntity.contains("MaterialComponent"))
                world->AddComponent(createdEntity, jEntity["MaterialComponent"].get<MaterialComponent>());
            if (jEntity.contains("LightningComponent"))
                world->AddComponent(createdEntity, jEntity["LightningComponent"].get<LightningComponent>());
            if (jEntity.contains("TextComponent"))
                world->AddComponent(createdEntity, jEntity["TextComponent"].get<TextComponent>());
            if (jEntity.contains("SunMarker"))
                world->AddComponent(createdEntity, SunMarker{});
            if (jEntity.contains("PlayerComponent"))
                world->AddComponent(createdEntity, jEntity["PlayerComponent"].get<PlayerComponent>());
            if (jEntity.contains("UIRectComponent"))
                world->AddComponent(createdEntity, jEntity["UIRectComponent"].get<UIRectComponent>());
            if (jEntity.contains("StateScopeComponent"))
                world->AddComponent(createdEntity, jEntity["StateScopeComponent"].get<StateScopeComponent>());
            if (jEntity.contains("MenuButtonComponent"))
                world->AddComponent(createdEntity, jEntity["MenuButtonComponent"].get<MenuButtonComponent>());
            if (jEntity.contains("ColliderComponent"))
                world->AddComponent(createdEntity, jEntity["ColliderComponent"].get<ColliderComponent>());
            if (jEntity.contains("NavMeshSourceComponent"))
                world->AddComponent(createdEntity, jEntity["NavMeshSourceComponent"].get<NavMeshSourceComponent>());
            if (jEntity.contains("NavObstacleComponent"))
                world->AddComponent(createdEntity, jEntity["NavObstacleComponent"].get<NavObstacleComponent>());
            if (jEntity.contains("NavAgentComponent"))
                world->AddComponent(createdEntity, jEntity["NavAgentComponent"].get<NavAgentComponent>());
            if (jEntity.contains("NavTargetComponent"))
                world->AddComponent(createdEntity, jEntity["NavTargetComponent"].get<NavTargetComponent>());
            if (jEntity.contains("NavConstrainedComponent"))
                world->AddComponent(createdEntity, NavConstrainedComponent{});
            if (jEntity.contains("NavClassComponent"))
                world->AddComponent(createdEntity, jEntity["NavClassComponent"].get<NavClassComponent>());
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
