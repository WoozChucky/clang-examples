#include "Systems.h"

#include <algorithm>

#include "lib.h"   // SM_TRACE

// Out-of-line anchor: gives ISystem's vtable + typeinfo a single home in ecs.dll.
ISystem::~ISystem() = default;

SystemScheduler::~SystemScheduler() {
    // Defensive: by contract Clear() runs before game.dll unload. If a scheduler
    // is destroyed with systems still registered AND game.dll already unloaded,
    // this would dispatch dead vtables — so callers must Clear() first. We still
    // clear here for the in-process-teardown (game.dll loaded) case.
    Clear();
}

void SystemScheduler::Register(std::unique_ptr<ISystem> system) {
    if (!system) return;
    const SystemPhase phase = system->Phase();
    m_Systems.push_back(Entry{ std::move(system), phase, m_NextOrder++ });
    m_Sorted = false;
}

void SystemScheduler::Clear() {
    m_Systems.clear();
    m_NextOrder = 0;
    m_Sorted = true;
}

void SystemScheduler::Run(SystemContext& ctx) {
    if (!m_Sorted) {
        std::stable_sort(m_Systems.begin(), m_Systems.end(),
            [](const Entry& a, const Entry& b) {
                if (a.phase != b.phase)
                    return static_cast<uint8_t>(a.phase) < static_cast<uint8_t>(b.phase);
                return a.order < b.order;
            });
        m_Sorted = true;
    }
    for (Entry& e : m_Systems) {
        e.system->Update(ctx);
    }
}
