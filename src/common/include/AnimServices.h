#pragma once
#include <cstdint>

// Engine-provided animator query surface. Engine populates an instance (AnimServicesImpl::Init);
// GameThread threads a pointer through SystemContext each tick. Game systems call through this
// instead of AnimatorControllerStore/AnimationStore::Instance() — keeps Game.dll's link graph free
// of Engine.dll (same boundary as NavServices/NetServices). GameThread only. Append-only struct
// (fn-ptr offsets are Game.dll ABI).
struct AnimServices {
    // Resolve an animator cursor to (current state name, normalized progress [0,1]) WITHOUT the game
    // touching engine stores. Copies the state's name into outName (size outCap, always NUL-terminated)
    // and returns its normalized progress. Returns -1.0f (and empties outName) if the controller or
    // state index is unknown.
    float (*QueryStateCursor)(uint64_t controllerId, int stateIndex, float stateTime, float phase,
                              char* outName, int outCap) = nullptr;
};
