#pragma once
#include <glm/glm.hpp>
#include <nlohmann/json.hpp>

// Game-owned post-collision velocity, finite-differenced from the actual transform each tick by
// VelocitySystem. The animator reads its horizontal magnitude as the "speed" parameter.
struct VelocityComponent {
    glm::vec3 Linear{0.0f};   // world units / second
    glm::vec3 PrevPos{0.0f};  // last tick's position
    bool      Init = false;   // false until PrevPos seeded
};

inline void to_json(nlohmann::json& j, const VelocityComponent& t) {
    j = nlohmann::json{ {"Linear", { t.Linear.x, t.Linear.y, t.Linear.z }} };
    // PrevPos/Init are transient; not persisted.
}
inline void from_json(const nlohmann::json& j, VelocityComponent& t) {
    if (j.contains("Linear") && j["Linear"].is_array() && j["Linear"].size() == 3) {
        t.Linear = glm::vec3(j["Linear"][0].get<float>(), j["Linear"][1].get<float>(), j["Linear"][2].get<float>());
    }
}
