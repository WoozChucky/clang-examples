#pragma once

#include <cstdint>
#include <memory>
#include <vector>

#include "ECS.h"   // ECS + ECS_API
#include "NavServices.h"   // pulls full struct + std::vector/glm dependencies
#include "NetServices.h"
#include "AppRole.h"

// Minimal per-tick context handed to every system.
struct SystemContext {
    ECS&   world;
    double dt;        // seconds since last tick (clamped by GameThread)
    double gameTime;  // absolute time
    const NavServices* Nav = nullptr;  // engine-provided nav table; nullptr in test harness or pre-init
    const NetServices* Net = nullptr;  // engine-provided net table; nullptr in tests/pre-init
    AppRole role = AppRole::Client;    // process role; Server only inside server.exe
};

// Coarse run-order buckets. Systems sort by (phase, registration index).
enum class SystemPhase : uint8_t {
    Input          = 0,   // (future) input-derived state
    Simulation     = 1,   // gameplay decisions / intent (PlayerMovement, AI, MenuInteraction, AppFlow, …)
    Physics        = 2,   // spatial resolution against the world (KinematicMovementSystem + future RigidBodyStep)
    PostSimulation = 3,   // reactions to the resolved world (camera follow, animation triggers, audio cues)
    PreRender      = 4,   // last-chance ECS prep before snapshot
};

// A unit of gameplay logic. Concrete systems live in game.dll; their vtables
// are unmapped on game.dll reload, so SystemScheduler::Clear() MUST run before
// FreeLibrary (see the spec's hot-reload section).
class ECS_API ISystem {
public:
    virtual ~ISystem();   // out-of-line anchor in systems.cpp (single vtable home)
    virtual void Update(SystemContext& ctx) = 0;
    [[nodiscard]] virtual const char* Name() const = 0;
    [[nodiscard]] virtual SystemPhase Phase() const = 0;
};

// Owns registered systems; runs them ordered by (phase, registration index).
// Owned by GameThread — never stored inside ECS and never snapshotted.
class ECS_API SystemScheduler {
public:
    SystemScheduler() = default;
    ~SystemScheduler();

    SystemScheduler(const SystemScheduler&) = delete;
    SystemScheduler& operator=(const SystemScheduler&) = delete;

    // Takes ownership. Within a phase, runs in registration order.
    void Register(std::unique_ptr<ISystem> system);

    // Destroys all systems. MUST be called while game.dll is still loaded
    // (system dtors dispatch through vtables that live in game.dll).
    void Clear();

    // Runs each system once, ordered by (phase, registration index).
    void Run(SystemContext& ctx);

    [[nodiscard]] size_t Count() const { return m_Systems.size(); }

private:
    struct Entry {
        std::unique_ptr<ISystem> system;
        SystemPhase phase;
        uint32_t    order;   // registration index, for stable within-phase order
    };
    std::vector<Entry> m_Systems;
    uint32_t m_NextOrder = 0;
    bool     m_Sorted    = true;
};
