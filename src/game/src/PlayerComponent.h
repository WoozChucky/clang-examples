#pragma once
#include <nlohmann/json.hpp>

// Marks the player-controlled entity. Moved by the player movement system (game) from input.
// Game-owned component: registered via GameRegisterComponents (non-builtin), not an engine builtin.
struct PlayerComponent {
    float MoveSpeed = 5.0f; // world units / second
};

inline void to_json(nlohmann::json& j, const PlayerComponent& t) {
    j = nlohmann::json{{"MoveSpeed", t.MoveSpeed}};
}
inline void from_json(const nlohmann::json& j, PlayerComponent& t) {
    j.at("MoveSpeed").get_to(t.MoveSpeed);
}
