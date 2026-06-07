#pragma once
#include <cstdint>
#include <glm/vec4.hpp>
#include <nlohmann/json.hpp>
#include "GlmJson.h"   // glm::vec4 JSON (adl_serializer) — ECS-free

// Marks a UI-rect entity as a clickable menu button (authored). ActionId is an Actions:: id
// (0 = none). The game interaction system drives UIRectComponent.Color between Normal/Hover/Press.
// Game-owned component: registered via GameRegisterComponents (non-builtin), not an engine builtin.
struct MenuButtonComponent {
    uint32_t  ActionId = 0;
    glm::vec4 Normal{0.15f, 0.15f, 0.18f, 1.0f};
    glm::vec4 Hover {0.25f, 0.25f, 0.30f, 1.0f};
    glm::vec4 Press {0.35f, 0.35f, 0.42f, 1.0f};
};

inline void to_json(nlohmann::json& j, const MenuButtonComponent& t) {
    j = nlohmann::json{
        {"ActionId", t.ActionId},
        {"Normal", t.Normal},
        {"Hover",  t.Hover},
        {"Press",  t.Press}
    };
}
inline void from_json(const nlohmann::json& j, MenuButtonComponent& t) {
    j.at("ActionId").get_to(t.ActionId);
    j.at("Normal").get_to(t.Normal);
    j.at("Hover").get_to(t.Hover);
    j.at("Press").get_to(t.Press);
}
