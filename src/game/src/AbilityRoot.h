#pragma once
#include <string>

// Game-owned ability root policy. The engine never sees this — it only exposes the animator cursor
// (state name + normalized time); the game decides what roots movement. PROTOTYPE: one hardcoded rule
// (root the first kAttackRootEnd of the "Attack" state — a fireball-cast-style partial window). A real
// game would carry per-ability windows on an AbilityComponent (whole-state for heavy/channel, partial
// for cast); they all plug in HERE by varying this function, with NO engine change.
inline constexpr float kAttackRootEnd = 0.6f;

inline bool ShouldRootMovement(const std::string& stateName, float normalizedTime) {
    return stateName == "Attack" && normalizedTime < kAttackRootEnd;
}
