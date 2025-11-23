#include "WorldManager.h"

#include <fstream>

#include "lib.h"

#include <nlohmann/json.hpp>

using json = nlohmann::json;

#include <glm/vec3.hpp>
#include <glm/vec4.hpp>

namespace nlohmann {
    template<> struct adl_serializer<glm::vec3> {
        static void to_json(json& j, const glm::vec3& v) {
            // Keep your current object shape: { "X": .., "Y": .., "Z": .. }
            j = json{{"X", v.x}, {"Y", v.y}, {"Z", v.z}};
        }
        static void from_json(const json& j, glm::vec3& v) {
            // Be tolerant: accept either object {X,Y,Z} or array [x,y,z]
            if (j.is_array() && j.size() == 3) {
                v.x = j[0].get<float>();
                v.y = j[1].get<float>();
                v.z = j[2].get<float>();
            } else {
                j.at("X").get_to(v.x);
                j.at("Y").get_to(v.y);
                j.at("Z").get_to(v.z);
            }
        }
    };

    template<> struct adl_serializer<glm::vec4> {
        static void to_json(json& j, const glm::vec4& v) {
            j = json{{"X", v.x}, {"Y", v.y}, {"Z", v.z}, {"W", v.w}};
        }
        static void from_json(const json& j, glm::vec4& v) {
            if (j.is_array() && j.size() == 4) {
                v.x = j[0].get<float>();
                v.y = j[1].get<float>();
                v.z = j[2].get<float>();
                v.w = j[3].get<float>();
            } else {
                j.at("X").get_to(v.x);
                j.at("Y").get_to(v.y);
                j.at("Z").get_to(v.z);
                j.at("W").get_to(v.w);
            }
        }
    };
}

void to_json(json& j, const glm::vec3& t) {
    j = json{{"X", t.x}, {"Y", t.y}, {"Z", t.z}};
}

void from_json(const json& j, glm::vec3& t) {
    j.at("X").get_to(t.x);
    j.at("Y").get_to(t.y);
    j.at("Z").get_to(t.z);
}

void to_json(json& j, const glm::vec4& t) {
    j = json{{"R", t.r}, {"G", t.g}, {"B", t.b}, {"A", t.a}};
}

void from_json(const json& j, glm::vec4& t) {
    j.at("R").get_to(t.r);
    j.at("G").get_to(t.g);
    j.at("B").get_to(t.b);
    j.at("A").get_to(t.a);
}

void to_json(json& j, const TransformComponent& t) {
    j = json{{"Position", t.Position}, {"Rotation", t.Rotation}, {"Scale", t.Scale}};
}

void from_json(const json& j, TransformComponent& t) {
    j.at("Position").get_to(t.Position);
    j.at("Rotation").get_to(t.Rotation);
    j.at("Scale").get_to(t.Scale);
}

void to_json(json& j, const MeshComponent& t) {
    j = json{{"MeshId", t.MeshId}, {"Visible", t.Visible}};
}

void from_json(const json& j, MeshComponent& t) {
    j.at("MeshId").get_to(t.MeshId);
    j.at("Visible").get_to(t.Visible);
}

void to_json(json& j, const MaterialComponent& t) {
    j = json{{"MaterialId", t.MaterialId}, {"BaseColor", t.BaseColor}, {"Flags", t.Flags}};
}

void from_json(const json& j, MaterialComponent& t) {
    j.at("MaterialId").get_to(t.MaterialId);
    j.at("BaseColor").get_to(t.BaseColor);
    j.at("Flags").get_to(t.Flags);
}

void to_json(json& j, const LightningComponent& t) {
    j = json{{"Type", t.Type}, {"Direction", t.Direction}, {"Color", t.Color}, {"Intensity", t.Intensity}, {"Range", t.Range}};
}

void from_json(const json& j, LightningComponent& t) {
    j.at("Type").get_to(t.Type);
    j.at("Direction").get_to(t.Direction);
    j.at("Color").get_to(t.Color);
    j.at("Intensity").get_to(t.Intensity);
    j.at("Range").get_to(t.Range);
}

void to_json(json& j, const TextComponent& t) {
    j = json{{"Text", t.Text}, {"Color", t.Color}, {"FontSize", t.FontSize}};
}

void from_json(const json& j, TextComponent& t) {
    j.at("Text").get_to(t.Text);
    j.at("Color").get_to(t.Color);
    j.at("FontSize").get_to(t.FontSize);
}

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

        j["Entities"].push_back(jEntity);
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

    world->Clear();

    // Use nlohmann/json for deserialization
    json j;
    ifs >> j;
    size_t entityCount = j["EntityCount"].get<size_t>();
    for (const auto& jEntity : j["Entities"]) {
        EntityId entity = jEntity["EntityId"].get<EntityId>();
        // Re-create entity
        // Note: In a real ECS, you might want to ensure the same EntityId is used
        // This example assumes CreateEntity() returns the same ID for simplicity
        auto createdEntity = world->CreateEntity();

        // Deserialize components if they exist
        if (jEntity.contains("TransformComponent")) {
            TransformComponent tc = jEntity["TransformComponent"].get<TransformComponent>();
            world->AddComponent(createdEntity, tc);
        }
        if (jEntity.contains("MeshComponent")) {
            MeshComponent mc = jEntity["MeshComponent"].get<MeshComponent>();
            world->AddComponent(createdEntity, mc);
        }
        if (jEntity.contains("MaterialComponent")) {
            MaterialComponent matc = jEntity["MaterialComponent"].get<MaterialComponent>();
            world->AddComponent(createdEntity, matc);
        }
        if (jEntity.contains("LightningComponent")) {
            LightningComponent lc = jEntity["LightningComponent"].get<LightningComponent>();
            world->AddComponent(createdEntity, lc);
        }
        if (jEntity.contains("TextComponent")) {
            TextComponent textc = jEntity["TextComponent"].get<TextComponent>();
            world->AddComponent(createdEntity, textc);
        }
    }

    return true;

}
