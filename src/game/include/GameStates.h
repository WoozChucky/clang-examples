#pragma once
#include <cstdint>

// Game-owned application states. The engine never names these — it stores the
// current state as an opaque uint32_t bit index (GameStateComponent.Current) and
// compares it against StateScopeComponent.StateMask bits. Add/rename/reorder freely;
// only the bit indices (0..31) are an implicit contract with authored StateMask bits.
enum class GameStateId : uint32_t {
    Uninitialized = 0,
    MainMenu      = 1,
    InLevel       = 2,
    InEditor      = 3,
    Paused        = 4,
};

// Convert between the typed game enum and the engine's opaque uint32_t index.
constexpr uint32_t   StateIndex(GameStateId s) noexcept { return static_cast<uint32_t>(s); }
constexpr GameStateId AsGameState(uint32_t v)  noexcept { return static_cast<GameStateId>(v); }
