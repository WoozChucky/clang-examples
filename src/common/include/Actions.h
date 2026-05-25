#pragma once
#include <cstdint>

// Action identifiers carried by ActionEvent and authored on MenuButtonComponent. Namespaced by
// category in the high 16 bits so each owning system claims a category and ignores the rest.
enum class ActionCategory : uint16_t {
    None = 0,
    Nav  = 1,
    // future: Inventory = 2, Ability = 3, ... (each owned by its own system)
};

constexpr uint32_t MakeAction(ActionCategory c, uint16_t local) {
    return (static_cast<uint32_t>(c) << 16) | local;
}
constexpr ActionCategory CategoryOf(uint32_t id) {
    return static_cast<ActionCategory>(id >> 16);
}

namespace Actions {
    inline constexpr uint32_t None = 0;
    inline constexpr uint32_t Play = MakeAction(ActionCategory::Nav, 1);
    inline constexpr uint32_t Quit = MakeAction(ActionCategory::Nav, 2);
    inline constexpr uint32_t Back = MakeAction(ActionCategory::Nav, 3);
}
