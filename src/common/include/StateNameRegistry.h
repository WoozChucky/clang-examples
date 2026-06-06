#pragma once
#include <cstdint>
#include <string>
#include <utility>
#include <vector>

#include "ECS.h"  // ECS_API

// Game-owned mapping from a state BIT INDEX (see the game's GameStateId) to a display label,
// shared across modules so the editor can show named StateScope checkboxes without naming the
// game's enum. The game registers its states at startup; the editor reads them. Single
// exported instance (defined in ecs.dll). Upsert by index (hot-reload safe).
class StateNameRegistry {
public:
    void Register(uint32_t bitIndex, const std::string& label) {
        for (auto& e : m_Entries) { if (e.first == bitIndex) { e.second = label; return; } }
        m_Entries.emplace_back(bitIndex, label);
    }
    [[nodiscard]] const std::vector<std::pair<uint32_t, std::string>>& Entries() const { return m_Entries; }

private:
    std::vector<std::pair<uint32_t, std::string>> m_Entries;
};

ECS_API StateNameRegistry& StateNames();
