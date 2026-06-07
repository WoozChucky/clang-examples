#pragma once

#include <nlohmann/json.hpp>
#include <glm/vec3.hpp>
#include <glm/vec4.hpp>

// JSON (de)serialization for glm vector types. Kept ECS-free (no ECS.h include) so that
// Game.dll-owned component headers can serialize glm vectors without pulling in ECS.h.
// Shared by ComponentSerialization.h (engine/common) and game component headers.
namespace nlohmann {
    template<> struct adl_serializer<glm::vec3> {
        static void to_json(json& j, const glm::vec3& v) {
            j = json{{"X", v.x}, {"Y", v.y}, {"Z", v.z}};
        }
        static void from_json(const json& j, glm::vec3& v) {
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

inline void to_json(nlohmann::json& j, const glm::vec3& t) {
    j = nlohmann::json{{"X", t.x}, {"Y", t.y}, {"Z", t.z}};
}
inline void from_json(const nlohmann::json& j, glm::vec3& t) {
    j.at("X").get_to(t.x);
    j.at("Y").get_to(t.y);
    j.at("Z").get_to(t.z);
}
inline void to_json(nlohmann::json& j, const glm::vec4& t) {
    j = nlohmann::json{{"R", t.r}, {"G", t.g}, {"B", t.b}, {"A", t.a}};
}
inline void from_json(const nlohmann::json& j, glm::vec4& t) {
    j.at("R").get_to(t.r);
    j.at("G").get_to(t.g);
    j.at("B").get_to(t.b);
    j.at("A").get_to(t.a);
}
