# Allocator Toolkit + Engine Library Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Build a reusable allocator toolkit (fixed arena + growable pool) behind a minimal `IAllocator`, hosted by a new `Engine` SHARED library that exports a cross-DLL `AllocatorRegistry` for per-category memory stats, surfaced through an editor ImGui panel.

**Architecture:** Allocator value types are header-only (zero link to use). The one piece needing a single shared instance — the registry — lives in `Engine.dll` (loaded once, never unloaded, like `ecs.dll`). Category is a per-allocator tag; the registry aggregates by category. `FrameAllocator` becomes a trivial `ArenaAllocator` subclass so render code is untouched. ECS clone churn is a deferred follow-up.

**Tech Stack:** C++23, CMake (MSVC + clang-cl presets), Windows-only, ImGui (editor only), custom test harness mirroring `test_ecs`.

**Spec:** `docs/superpowers/specs/2026-05-21-allocator-toolkit-design.md`

**Model guidance:** Per project convention for high-risk infra, dispatch **all** implementer and reviewer subagents on **Opus 4.7** (no cheap-model tiering). Tasks 1, 2, and 4 are the highest-risk (CMake/export, cross-DLL singleton, slab growth).

**Branch:** Create a feature branch before Task 1 (e.g. `allocator-toolkit`). Do not implement on `main`.

---

## File Structure

**New — Engine library:**
- `src/engine/CMakeLists.txt` — `add_library(Engine SHARED ...)`, `ENGINE_API`, output to `RUNTIME_DIR`.
- `src/engine/include/Engine.h` — `ENGINE_API` export macro.
- `src/engine/include/memory/MemoryCategory.h` — `MemCategory` enum + `ToString` decl.
- `src/engine/include/memory/AllocatorStats.h` — POD stats struct.
- `src/engine/include/memory/MemUtil.h` — `AlignUp`, `New`/`Delete` helpers (header-only).
- `src/engine/include/memory/IAllocator.h` — abstract interface (header-only).
- `src/engine/include/memory/ArenaAllocator.h` — fixed bump allocator (header-only).
- `src/engine/include/memory/PoolAllocator.h` — growable fixed-block pool (header-only).
- `src/engine/include/memory/AllocatorRegistry.h` — registry decl (`ENGINE_API`).
- `src/engine/src/MemoryCategory.cpp` — `ToString` impl (exported TU).
- `src/engine/src/AllocatorRegistry.cpp` — registry impl + `Registry()` singleton (exported TU).

**New — tests:**
- `tests/test_alloc.cpp` — unit tests (harness mirrors `test_ecs.cpp`).

**New — editor panel:**
- `src/editor/src/rendering/imgui/MemoryPanel.h` / `.cpp` — ImGui memory view.

**Modified:**
- `CMakeLists.txt` — add `add_subdirectory(src/engine)` before `editor`.
- `tests/CMakeLists.txt` — add `test_alloc` target.
- `src/editor/CMakeLists.txt` — link `Engine`; add `MemoryPanel.cpp` to sources.
- `src/editor/src/rendering/FrameAllocator.h` — subclass `Engine::ArenaAllocator`.
- `src/editor/src/rendering/Renderer.cpp` — register/unregister `m_FrameAllocator`.
- `src/editor/src/rendering/imgui/ImGuiRenderer.cpp` — call `DrawMemoryPanel`.

---

## Conventions for every task

- All Engine code is in `namespace Engine`. Headers use `#pragma once`.
- Build/configure commands assume the `msvc-win64-vs2026-enterprise` preset (same as `test_ecs` in CLAUDE.md). **Whenever a `CMakeLists.txt` is created or changed, re-run configure first** so CMake sees new files:
  - Configure: `cmake --preset msvc-win64-vs2026-enterprise`
  - Build a target: `cmake --build --preset msvc-win64-vs2026-enterprise --target <name>`
- Test exe path: `./out/build/msvc-win64-vs2026-enterprise/bin/Debug/test_alloc.exe`
- Commit after each task with the message shown in that task's final step.

---

### Task 1: Engine SHARED lib scaffold + header primitives + test harness

Creates the Engine library as a SHARED target, the export macro, the header-only primitives that need no other types (`MemCategory`, `AllocatorStats`, `MemUtil`), the one exported symbol (`ToString`), and a `test_alloc` target with the harness. Proves the library builds as a DLL, links into a consumer, and the headers compile.

**Files:**
- Create: `src/engine/include/Engine.h`
- Create: `src/engine/include/memory/MemoryCategory.h`
- Create: `src/engine/include/memory/AllocatorStats.h`
- Create: `src/engine/include/memory/MemUtil.h`
- Create: `src/engine/src/MemoryCategory.cpp`
- Create: `src/engine/CMakeLists.txt`
- Create: `tests/test_alloc.cpp`
- Modify: `CMakeLists.txt` (add `add_subdirectory(src/engine)`)
- Modify: `tests/CMakeLists.txt` (add `test_alloc`)

- [ ] **Step 1: Write the export macro header**

Create `src/engine/include/Engine.h`:

```cpp
#pragma once

// Cross-DLL export annotation for Engine.dll. dllexport inside Engine's own
// TUs (ENGINE_EXPORTS defined), dllimport everywhere else. Mirrors ECS_API.
#ifndef ENGINE_API
  #ifdef _WIN32
    #ifdef ENGINE_EXPORTS
      #define ENGINE_API __declspec(dllexport)
    #else
      #define ENGINE_API __declspec(dllimport)
    #endif
  #else
    #define ENGINE_API
  #endif
#endif
```

- [ ] **Step 2: Write MemoryCategory + AllocatorStats + MemUtil headers**

Create `src/engine/include/memory/MemoryCategory.h`:

```cpp
#pragma once
#include <cstdint>
#include <Engine.h>

namespace Engine {

enum class MemCategory : uint8_t {
    General, FrameTransient, Renderer, Mesh, Material,
    Texture, UI, ECS, Snapshot, Game, Count
};

// Human-readable name for the memory panel. Exported from Engine.dll.
ENGINE_API const char* ToString(MemCategory c);

} // namespace Engine
```

Create `src/engine/include/memory/AllocatorStats.h`:

```cpp
#pragma once
#include <cstddef>
#include <cstdint>

namespace Engine {

struct AllocatorStats {
    size_t   Used     = 0; // bytes currently handed out
    size_t   Peak     = 0; // high-water mark of Used
    size_t   Capacity = 0; // total backing bytes
    uint64_t AllocCount = 0;
    uint64_t FreeCount  = 0;
};

} // namespace Engine
```

Create `src/engine/include/memory/MemUtil.h`:

```cpp
#pragma once
#include <cstddef>
#include <new>
#include <utility>

namespace Engine {

constexpr size_t AlignUp(size_t value, size_t alignment) {
    return (value + alignment - 1) & ~(alignment - 1);
}

// Construct a T from any allocator exposing Allocate(size, align)/Deallocate.
template<class A, class T, class... Args>
T* New(A& alloc, Args&&... args) {
    void* p = alloc.Allocate(sizeof(T), alignof(T));
    return p ? ::new (p) T(std::forward<Args>(args)...) : nullptr;
}

template<class A, class T>
void Delete(A& alloc, T* ptr) {
    if (!ptr) return;
    ptr->~T();
    alloc.Deallocate(ptr, sizeof(T));
}

} // namespace Engine
```

- [ ] **Step 3: Write the ToString implementation (the exported TU)**

Create `src/engine/src/MemoryCategory.cpp`:

```cpp
#include <memory/MemoryCategory.h>

namespace Engine {

const char* ToString(MemCategory c) {
    switch (c) {
        case MemCategory::General:        return "General";
        case MemCategory::FrameTransient: return "FrameTransient";
        case MemCategory::Renderer:       return "Renderer";
        case MemCategory::Mesh:           return "Mesh";
        case MemCategory::Material:       return "Material";
        case MemCategory::Texture:        return "Texture";
        case MemCategory::UI:             return "UI";
        case MemCategory::ECS:            return "ECS";
        case MemCategory::Snapshot:       return "Snapshot";
        case MemCategory::Game:           return "Game";
        default:                          return "?";
    }
}

} // namespace Engine
```

- [ ] **Step 4: Write the Engine CMakeLists**

Create `src/engine/CMakeLists.txt`:

```cmake
add_library(Engine SHARED
    src/MemoryCategory.cpp
    src/AllocatorRegistry.cpp
)

target_include_directories(Engine PUBLIC include)

target_link_libraries(Engine PUBLIC
    CommonHeaders
)

target_compile_definitions(Engine PRIVATE
    ENGINE_EXPORTS
    NOMINMAX
    WIN32_LEAN_AND_MEAN
)

set_target_properties(Engine PROPERTIES
    OUTPUT_NAME Engine
    DEBUG_POSTFIX ""
    RUNTIME_OUTPUT_DIRECTORY "${RUNTIME_DIR}"
    ARCHIVE_OUTPUT_DIRECTORY "${RUNTIME_DIR}"
    LIBRARY_OUTPUT_DIRECTORY "${RUNTIME_DIR}"
    FOLDER Libraries
)

if(MSVC)
    set_property(TARGET Engine PROPERTY MSVC_DEBUG_INFORMATION_FORMAT
                 $<$<CONFIG:Debug>:Embedded>)
endif()
```

Note: `src/AllocatorRegistry.cpp` is referenced here but created in Task 2. Create a temporary placeholder now so the target builds:

Create `src/engine/src/AllocatorRegistry.cpp` (placeholder, replaced in Task 2):

```cpp
// Placeholder; real implementation lands in Task 2.
namespace Engine { namespace { int kEnginePlaceholder = 0; } }
```

- [ ] **Step 5: Wire Engine into the root CMake**

In `CMakeLists.txt`, add the Engine subdirectory immediately after the ECS one. Change:

```cmake
# ECS shared library (must be configured before game/editor link it)
add_subdirectory(src/ecs)

# game library
add_subdirectory(src/game)
```

to:

```cmake
# ECS shared library (must be configured before game/editor link it)
add_subdirectory(src/ecs)

# Engine shared library (cross-cutting infra: allocator registry)
add_subdirectory(src/engine)

# game library
add_subdirectory(src/game)
```

- [ ] **Step 6: Write the test harness + first tests**

Create `tests/test_alloc.cpp`:

```cpp
#include <cstdio>
#include <cstdlib>
#include <cstdint>

#include "lib.h"
#include <memory/MemUtil.h>
#include <memory/MemoryCategory.h>

// Required by lib.h's SM_ASSERT. Test exe: print + abort, no MessageBox.
void platform_debug_break(const char* expr, const char* file, int line, const char* message)
{
    std::fprintf(stderr, "ASSERT FAIL %s:%d: %s (expr: %s)\n",
                 (file ? file : "<unknown>"), line,
                 (message ? message : "<no message>"),
                 (expr ? expr : "<none>"));
    std::abort();
}

static int g_Failures = 0;

#define EXPECT(cond)                                               \
    do {                                                           \
        if (!(cond)) {                                             \
            SM_ERROR("FAIL %s:%d: %s", __FILE__, __LINE__, #cond); \
            ++g_Failures;                                          \
        }                                                          \
    } while (0)

#define EXPECT_EQ(a, b) EXPECT((a) == (b))
#define EXPECT_NE(a, b) EXPECT((a) != (b))

static void T00_smoke()
{
    EXPECT_EQ(1 + 1, 2);
}

static void T01_alignup()
{
    EXPECT_EQ(Engine::AlignUp(0, 16), (size_t)0);
    EXPECT_EQ(Engine::AlignUp(1, 16), (size_t)16);
    EXPECT_EQ(Engine::AlignUp(16, 16), (size_t)16);
    EXPECT_EQ(Engine::AlignUp(17, 16), (size_t)32);
    EXPECT_EQ(Engine::AlignUp(13, 8), (size_t)16);
}

static void T02_category_tostring()
{
    EXPECT_EQ(std::string(Engine::ToString(Engine::MemCategory::ECS)), std::string("ECS"));
    EXPECT_EQ(std::string(Engine::ToString(Engine::MemCategory::FrameTransient)),
              std::string("FrameTransient"));
}

int main()
{
    T00_smoke();
    T01_alignup();
    T02_category_tostring();

    if (g_Failures == 0) {
        std::printf("All allocator tests passed.\n");
        return 0;
    }
    std::printf("%d allocator test(s) FAILED.\n", g_Failures);
    return 1;
}
```

Add `#include <string>` at the top of the includes (used by `T02`).

- [ ] **Step 7: Add the test_alloc target**

In `tests/CMakeLists.txt`, append after the existing `test_ecs` block:

```cmake
add_executable(test_alloc
    test_alloc.cpp
)

target_link_libraries(test_alloc PRIVATE
    CommonHeaders
    Engine
)

set_target_properties(test_alloc PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY "${RUNTIME_DIR}"
    FOLDER Tests
)
```

- [ ] **Step 8: Configure, build, run — verify pass**

Run:
```
cmake --preset msvc-win64-vs2026-enterprise
cmake --build --preset msvc-win64-vs2026-enterprise --target test_alloc
./out/build/msvc-win64-vs2026-enterprise/bin/Debug/test_alloc.exe
```
Expected: builds `Engine.dll` + `test_alloc.exe`, output `All allocator tests passed.`

- [ ] **Step 9: Commit**

```bash
git add src/engine CMakeLists.txt tests/test_alloc.cpp tests/CMakeLists.txt
git commit -m "feat(engine): Engine SHARED lib scaffold + memory primitives + test_alloc"
```

---

### Task 2: IAllocator interface + cross-DLL AllocatorRegistry

Adds the abstract allocator interface and the locked, single-instance registry exported from `Engine.dll`. **High-risk:** cross-DLL singleton + locking.

**Files:**
- Create: `src/engine/include/memory/IAllocator.h`
- Create: `src/engine/include/memory/AllocatorRegistry.h`
- Modify (replace placeholder): `src/engine/src/AllocatorRegistry.cpp`
- Modify: `tests/test_alloc.cpp`

- [ ] **Step 1: Write the IAllocator header**

Create `src/engine/include/memory/IAllocator.h`:

```cpp
#pragma once
#include <cstddef>
#include <memory/MemoryCategory.h>
#include <memory/AllocatorStats.h>

namespace Engine {

// Minimal allocator interface. Hot paths use concrete allocator types directly
// (devirtualized). This interface exists for polymorphic observation (the
// registry holds IAllocator*). Header-only, not exported: vtables live in each
// deriving module and are consistent across modules.
class IAllocator {
public:
    virtual ~IAllocator() = default;
    virtual void* Allocate(size_t size, size_t align = alignof(std::max_align_t)) = 0;
    virtual void  Deallocate(void* ptr, size_t size = 0) = 0;
    [[nodiscard]] virtual const AllocatorStats& Stats() const = 0;
    [[nodiscard]] virtual MemCategory Category() const = 0;
    [[nodiscard]] virtual const char* Name() const = 0;
};

} // namespace Engine
```

- [ ] **Step 2: Write the registry header**

Create `src/engine/include/memory/AllocatorRegistry.h`:

```cpp
#pragma once
#include <Engine.h>
#include <memory/MemoryCategory.h>
#include <memory/AllocatorStats.h>
#include <functional>
#include <mutex>
#include <vector>

namespace Engine {

class IAllocator;

// Non-owning observer registry. Single instance via Registry(), which lives in
// Engine.dll's TU so every module sees the same one. All methods are out-of-line
// (defined in AllocatorRegistry.cpp) so callers never touch the std members
// across the DLL boundary. Locked, but only on register/unregister/enumerate —
// never on the allocation hot path.
class ENGINE_API AllocatorRegistry {
public:
    void Register(IAllocator* a);
    void Unregister(IAllocator* a);
    void ForEach(const std::function<void(IAllocator*)>& fn) const;
    AllocatorStats SumByCategory(MemCategory cat) const;
    [[nodiscard]] size_t Count() const;

private:
    mutable std::mutex       m_Mutex;
    std::vector<IAllocator*> m_Allocators;
};

ENGINE_API AllocatorRegistry& Registry();

} // namespace Engine
```

- [ ] **Step 3: Replace the placeholder registry .cpp with the real implementation**

Overwrite `src/engine/src/AllocatorRegistry.cpp`:

```cpp
#include <memory/AllocatorRegistry.h>
#include <memory/IAllocator.h>
#include <algorithm>

namespace Engine {

void AllocatorRegistry::Register(IAllocator* a) {
    if (!a) return;
    std::lock_guard<std::mutex> lock(m_Mutex);
    m_Allocators.push_back(a);
}

void AllocatorRegistry::Unregister(IAllocator* a) {
    std::lock_guard<std::mutex> lock(m_Mutex);
    m_Allocators.erase(
        std::remove(m_Allocators.begin(), m_Allocators.end(), a),
        m_Allocators.end());
}

void AllocatorRegistry::ForEach(const std::function<void(IAllocator*)>& fn) const {
    std::lock_guard<std::mutex> lock(m_Mutex);
    for (IAllocator* a : m_Allocators) fn(a);
}

AllocatorStats AllocatorRegistry::SumByCategory(MemCategory cat) const {
    std::lock_guard<std::mutex> lock(m_Mutex);
    AllocatorStats sum;
    for (IAllocator* a : m_Allocators) {
        if (a->Category() != cat) continue;
        const AllocatorStats& s = a->Stats();
        sum.Used       += s.Used;
        sum.Peak       += s.Peak;
        sum.Capacity   += s.Capacity;
        sum.AllocCount += s.AllocCount;
        sum.FreeCount  += s.FreeCount;
    }
    return sum;
}

size_t AllocatorRegistry::Count() const {
    std::lock_guard<std::mutex> lock(m_Mutex);
    return m_Allocators.size();
}

AllocatorRegistry& Registry() {
    static AllocatorRegistry instance; // single instance in Engine.dll
    return instance;
}

} // namespace Engine
```

- [ ] **Step 4: Write failing registry tests (with a fake allocator)**

In `tests/test_alloc.cpp`, add the includes:

```cpp
#include <memory/IAllocator.h>
#include <memory/AllocatorRegistry.h>
```

Add a fake allocator + tests before `main`:

```cpp
// Minimal IAllocator for registry tests — does not actually allocate.
class FakeAllocator final : public Engine::IAllocator {
public:
    FakeAllocator(Engine::MemCategory cat, const char* name, size_t used, size_t cap)
        : m_Category(cat), m_Name(name) { m_Stats.Used = used; m_Stats.Peak = used; m_Stats.Capacity = cap; }
    void* Allocate(size_t, size_t) override { return nullptr; }
    void  Deallocate(void*, size_t) override {}
    const Engine::AllocatorStats& Stats() const override { return m_Stats; }
    Engine::MemCategory Category() const override { return m_Category; }
    const char* Name() const override { return m_Name; }
private:
    Engine::MemCategory   m_Category;
    const char*           m_Name;
    Engine::AllocatorStats m_Stats;
};

static void T10_registry_register_unregister()
{
    auto& reg = Engine::Registry();
    const size_t before = reg.Count();
    FakeAllocator a(Engine::MemCategory::Mesh, "A", 100, 1000);
    reg.Register(&a);
    EXPECT_EQ(reg.Count(), before + 1);
    reg.Unregister(&a);
    EXPECT_EQ(reg.Count(), before);
}

static void T11_registry_foreach_and_sum()
{
    auto& reg = Engine::Registry();
    FakeAllocator a(Engine::MemCategory::Mesh, "A", 100, 1000);
    FakeAllocator b(Engine::MemCategory::Mesh, "B", 250, 2000);
    FakeAllocator c(Engine::MemCategory::Game, "C", 999, 9999);
    reg.Register(&a); reg.Register(&b); reg.Register(&c);

    int seen = 0;
    reg.ForEach([&](Engine::IAllocator*){ ++seen; });
    EXPECT(seen >= 3);

    Engine::AllocatorStats mesh = reg.SumByCategory(Engine::MemCategory::Mesh);
    EXPECT_EQ(mesh.Used, (size_t)350);
    EXPECT_EQ(mesh.Capacity, (size_t)3000);

    reg.Unregister(&a); reg.Unregister(&b); reg.Unregister(&c);
}
```

Register the new tests in `main` (after the existing calls):

```cpp
    T10_registry_register_unregister();
    T11_registry_foreach_and_sum();
```

- [ ] **Step 5: Configure, build, run — verify pass**

Run (configure needed: registry .cpp content changed but no new files, so a plain build is enough; configure anyway if the previous step added headers):
```
cmake --build --preset msvc-win64-vs2026-enterprise --target test_alloc
./out/build/msvc-win64-vs2026-enterprise/bin/Debug/test_alloc.exe
```
Expected: `All allocator tests passed.`

- [ ] **Step 6: Commit**

```bash
git add src/engine tests/test_alloc.cpp
git commit -m "feat(engine): IAllocator interface + cross-DLL AllocatorRegistry"
```

---

### Task 3: ArenaAllocator (fixed bump)

Header-only linear allocator generalizing `FrameAllocator`. Provides the full `FrameAllocator`-compatible API so the subclass in Task 5 is churn-free.

**Files:**
- Create: `src/engine/include/memory/ArenaAllocator.h`
- Modify: `tests/test_alloc.cpp`

- [ ] **Step 1: Write failing arena tests**

In `tests/test_alloc.cpp`, add the include:

```cpp
#include <memory/ArenaAllocator.h>
```

Add tests before `main`:

```cpp
static void T20_arena_bump_and_align()
{
    Engine::ArenaAllocator a(1024, Engine::MemCategory::FrameTransient, "T20");
    void* p0 = a.Allocate(10, 16);
    void* p1 = a.Allocate(10, 16);
    EXPECT_NE(p0, nullptr);
    EXPECT_NE(p1, nullptr);
    EXPECT_EQ((uintptr_t)p0 % 16, (uintptr_t)0);
    EXPECT_EQ((uintptr_t)p1 % 16, (uintptr_t)0);
    EXPECT(p1 != p0);
    EXPECT(a.Stats().Used >= 20);
}

static void T21_arena_overflow_returns_null()
{
    Engine::ArenaAllocator a(64, Engine::MemCategory::FrameTransient, "T21");
    void* ok = a.Allocate(32, 8);
    EXPECT_NE(ok, nullptr);
    void* fail = a.Allocate(1024, 8); // exceeds capacity
    EXPECT_EQ(fail, nullptr);
}

static void T22_arena_reset_keeps_peak()
{
    Engine::ArenaAllocator a(1024, Engine::MemCategory::FrameTransient, "T22");
    a.Allocate(500, 8);
    const size_t peak = a.Stats().Peak;
    EXPECT(peak >= 500);
    a.Reset();
    EXPECT_EQ(a.Stats().Used, (size_t)0);
    EXPECT_EQ(a.Stats().Peak, peak); // peak retained
}

static void T23_arena_marker_rewind()
{
    Engine::ArenaAllocator a(1024, Engine::MemCategory::FrameTransient, "T23");
    a.Allocate(100, 8);
    Engine::ArenaAllocator::Marker m = a.GetMarker();
    a.Allocate(200, 8);
    EXPECT(a.Stats().Used > (size_t)m);
    a.RewindTo(m);
    EXPECT_EQ(a.Stats().Used, (size_t)m);
}

static void T24_arena_allocate_array_and_typed()
{
    Engine::ArenaAllocator a(1024, Engine::MemCategory::FrameTransient, "T24");
    int* arr = a.AllocateArray<int>(8);
    EXPECT_NE(arr, nullptr);
    EXPECT_EQ((uintptr_t)arr % alignof(int), (uintptr_t)0);
    double* d = a.Allocate<double>();
    EXPECT_NE(d, nullptr);
    EXPECT_EQ((uintptr_t)d % alignof(double), (uintptr_t)0);
}

static void T25_arena_external_buffer()
{
    alignas(16) static unsigned char buf[256];
    Engine::ArenaAllocator a(buf, sizeof(buf), Engine::MemCategory::General, "T25");
    void* p = a.Allocate(16, 16);
    EXPECT_NE(p, nullptr);
    EXPECT(p >= (void*)buf);
    EXPECT(p < (void*)(buf + sizeof(buf)));
    EXPECT_EQ(a.Stats().Capacity, (size_t)256);
}
```

Register in `main`:

```cpp
    T20_arena_bump_and_align();
    T21_arena_overflow_returns_null();
    T22_arena_reset_keeps_peak();
    T23_arena_marker_rewind();
    T24_arena_allocate_array_and_typed();
    T25_arena_external_buffer();
```

- [ ] **Step 2: Run to verify failure**

Run:
```
cmake --build --preset msvc-win64-vs2026-enterprise --target test_alloc
```
Expected: compile error — `ArenaAllocator.h` not found / `Engine::ArenaAllocator` undefined.

- [ ] **Step 3: Implement ArenaAllocator**

Create `src/engine/include/memory/ArenaAllocator.h`:

```cpp
#pragma once
#include <cstdint>
#include <cstdlib>
#include <lib.h>                 // SM_ERROR
#include <memory/IAllocator.h>
#include <memory/MemUtil.h>      // AlignUp

namespace Engine {

// Fixed-capacity linear (bump) allocator. Allocate is O(1); individual frees are
// no-ops. Reset() reclaims everything (Peak retained). On overflow returns
// nullptr and logs — a deliberate guardrail, not silent growth. Not thread-safe.
class ArenaAllocator : public IAllocator {
public:
    using Marker = size_t;

    // Owns a freshly malloc'd buffer.
    ArenaAllocator(size_t capacity, MemCategory cat, const char* name)
        : m_Category(cat), m_Name(name), m_OwnsMemory(true)
    {
        m_Buffer = static_cast<uint8_t*>(std::malloc(capacity ? capacity : 1));
        if (!m_Buffer) {
            SM_ERROR("ArenaAllocator '%s': failed to allocate %zu bytes", name, capacity);
            m_Stats.Capacity = 0;
            m_OwnsMemory = false;
        } else {
            m_Stats.Capacity = capacity;
        }
    }

    // Views an externally-owned buffer (does not free it).
    ArenaAllocator(void* buffer, size_t capacity, MemCategory cat, const char* name)
        : m_Buffer(static_cast<uint8_t*>(buffer)), m_Category(cat),
          m_Name(name), m_OwnsMemory(false)
    {
        m_Stats.Capacity = m_Buffer ? capacity : 0;
    }

    ~ArenaAllocator() override {
        if (m_OwnsMemory && m_Buffer) std::free(m_Buffer);
        m_Buffer = nullptr;
    }

    ArenaAllocator(const ArenaAllocator&) = delete;
    ArenaAllocator& operator=(const ArenaAllocator&) = delete;

    void* Allocate(size_t size, size_t align) override {
        if (!m_Buffer || size == 0) return nullptr;
        const size_t alignedOffset = AlignUp(m_Offset, align);
        const size_t newOffset     = alignedOffset + size;
        if (newOffset > m_Stats.Capacity) {
            SM_ERROR("ArenaAllocator '%s' exhausted: req %zu, used %zu, cap %zu",
                     m_Name, size, m_Offset, m_Stats.Capacity);
            return nullptr;
        }
        m_Offset = newOffset;
        m_Stats.Used = m_Offset;
        if (m_Offset > m_Stats.Peak) m_Stats.Peak = m_Offset;
        ++m_Stats.AllocCount;
        return m_Buffer + alignedOffset;
    }

    void Deallocate(void*, size_t = 0) override {} // bulk free via Reset

    void Reset() {
        m_Offset = 0;
        m_Stats.Used = 0; // Peak retained
    }

    Marker GetMarker() const { return m_Offset; }
    void   RewindTo(Marker m) { m_Offset = m; m_Stats.Used = m; }

    // FrameAllocator-compatible API (so the subclass needs no render-code changes).
    void* AllocateBytes(size_t size, size_t alignment = 16) { return Allocate(size, alignment); }
    template<class T> T* Allocate() { return static_cast<T*>(Allocate(sizeof(T), alignof(T))); }
    template<class T> T* AllocateArray(size_t count) {
        return count ? static_cast<T*>(Allocate(sizeof(T) * count, alignof(T))) : nullptr;
    }
    [[nodiscard]] size_t GetUsedBytes()   const { return m_Stats.Used; }
    [[nodiscard]] size_t GetCapacity()    const { return m_Stats.Capacity; }
    [[nodiscard]] size_t GetPeakUsage()   const { return m_Stats.Peak; }
    [[nodiscard]] float  GetUsagePercent() const {
        return m_Stats.Capacity ? (float)m_Stats.Used / (float)m_Stats.Capacity * 100.0f : 0.0f;
    }
    void ResetPeakUsage() { m_Stats.Peak = m_Stats.Used; }

    const AllocatorStats& Stats() const override { return m_Stats; }
    MemCategory Category() const override { return m_Category; }
    const char* Name() const override { return m_Name; }

private:
    uint8_t*       m_Buffer = nullptr;
    size_t         m_Offset = 0;
    MemCategory    m_Category;
    const char*    m_Name;
    bool           m_OwnsMemory = true;
    AllocatorStats m_Stats;
};

} // namespace Engine
```

- [ ] **Step 4: Run to verify pass**

Run:
```
cmake --build --preset msvc-win64-vs2026-enterprise --target test_alloc
./out/build/msvc-win64-vs2026-enterprise/bin/Debug/test_alloc.exe
```
Expected: `All allocator tests passed.`

- [ ] **Step 5: Commit**

```bash
git add src/engine/include/memory/ArenaAllocator.h tests/test_alloc.cpp
git commit -m "feat(engine): ArenaAllocator (fixed bump, FrameAllocator-compatible API)"
```

---

### Task 4: PoolAllocator (growable, chained slabs)

Header-only fixed-block free-list allocator that grows by adding slabs on exhaustion. **High-risk:** stable pointers across growth, intrusive free-list, multi-slab Reset.

**Files:**
- Create: `src/engine/include/memory/PoolAllocator.h`
- Modify: `tests/test_alloc.cpp`

- [ ] **Step 1: Write failing pool tests**

In `tests/test_alloc.cpp`, add the include:

```cpp
#include <memory/PoolAllocator.h>
```

Add tests before `main`:

```cpp
static void T30_pool_alloc_free_reuse()
{
    Engine::PoolAllocator p(32, 4, 16, Engine::MemCategory::Mesh, "T30");
    void* a = p.Allocate(32, 16);
    EXPECT_NE(a, nullptr);
    EXPECT_EQ((uintptr_t)a % 16, (uintptr_t)0);
    p.Deallocate(a);
    void* b = p.Allocate(32, 16);
    EXPECT_EQ(a, b); // freed block is reused
}

static void T31_pool_grows_and_keeps_pointers_valid()
{
    Engine::PoolAllocator p(sizeof(uint64_t), 2, alignof(uint64_t),
                            Engine::MemCategory::Mesh, "T31");
    // Fill the first slab.
    auto* x0 = static_cast<uint64_t*>(p.Allocate(sizeof(uint64_t), alignof(uint64_t)));
    auto* x1 = static_cast<uint64_t*>(p.Allocate(sizeof(uint64_t), alignof(uint64_t)));
    EXPECT_NE(x0, nullptr);
    EXPECT_NE(x1, nullptr);
    *x0 = 0xAAAA; *x1 = 0xBBBB;
    const size_t capBefore = p.Stats().Capacity;

    // This forces a new slab (growth).
    auto* x2 = static_cast<uint64_t*>(p.Allocate(sizeof(uint64_t), alignof(uint64_t)));
    EXPECT_NE(x2, nullptr);
    *x2 = 0xCCCC;

    EXPECT(p.Stats().Capacity > capBefore);   // grew
    EXPECT_EQ(*x0, (uint64_t)0xAAAA);          // earlier-slab pointers still valid
    EXPECT_EQ(*x1, (uint64_t)0xBBBB);
    EXPECT_EQ(*x2, (uint64_t)0xCCCC);
}

static void T32_pool_reset_frees_all()
{
    Engine::PoolAllocator p(32, 2, 16, Engine::MemCategory::Mesh, "T32");
    p.Allocate(32, 16);
    p.Allocate(32, 16);
    EXPECT(p.Stats().Used > 0);
    p.Reset();
    EXPECT_EQ(p.Stats().Used, (size_t)0);
    void* a = p.Allocate(32, 16); // allocatable again
    EXPECT_NE(a, nullptr);
}
```

Register in `main`:

```cpp
    T30_pool_alloc_free_reuse();
    T31_pool_grows_and_keeps_pointers_valid();
    T32_pool_reset_frees_all();
```

- [ ] **Step 2: Run to verify failure**

Run:
```
cmake --build --preset msvc-win64-vs2026-enterprise --target test_alloc
```
Expected: compile error — `PoolAllocator.h` not found / `Engine::PoolAllocator` undefined.

- [ ] **Step 3: Implement PoolAllocator**

Create `src/engine/include/memory/PoolAllocator.h`:

```cpp
#pragma once
#include <cstdint>
#include <malloc.h>              // _aligned_malloc / _aligned_free (Windows)
#include <vector>
#include <lib.h>                 // SM_ERROR, SM_ASSERT
#include <memory/IAllocator.h>
#include <memory/MemUtil.h>      // AlignUp

namespace Engine {

// Fixed-block free-list allocator. All blocks are the same size, so there is no
// fragmentation. Grows by adding a new slab when full; slabs are never moved or
// freed until destruction, so every outstanding pointer stays valid. O(1)
// alloc/free. Not thread-safe.
class PoolAllocator : public IAllocator {
public:
    PoolAllocator(size_t blockSize, size_t blockCount, size_t blockAlign,
                  MemCategory cat, const char* name)
        : m_BlockCount(blockCount), m_Category(cat), m_Name(name)
    {
        SM_ASSERT(blockSize >= sizeof(void*),
                  "PoolAllocator '%s': blockSize must be >= sizeof(void*)", name);
        SM_ASSERT(blockCount > 0, "PoolAllocator '%s': blockCount must be > 0", name);
        m_BlockAlign = blockAlign < alignof(void*) ? alignof(void*) : blockAlign;
        const size_t minBlock = blockSize > sizeof(void*) ? blockSize : sizeof(void*);
        m_Stride = AlignUp(minBlock, m_BlockAlign);
        AddSlab(); // initial slab
    }

    ~PoolAllocator() override {
        for (void* slab : m_Slabs) _aligned_free(slab);
    }

    PoolAllocator(const PoolAllocator&) = delete;
    PoolAllocator& operator=(const PoolAllocator&) = delete;

    void* Allocate(size_t size, size_t align) override {
        SM_ASSERT(size <= m_Stride, "PoolAllocator '%s': request %zu exceeds block %zu",
                  m_Name, size, m_Stride);
        SM_ASSERT(align <= m_BlockAlign, "PoolAllocator '%s': align %zu exceeds %zu",
                  m_Name, align, m_BlockAlign);
        if (!m_FreeList) AddSlab();        // grow
        if (!m_FreeList) return nullptr;   // growth failed
        void* block = m_FreeList;
        m_FreeList = *reinterpret_cast<void**>(block);
        m_Stats.Used += m_Stride;
        ++m_Stats.AllocCount;
        if (m_Stats.Used > m_Stats.Peak) m_Stats.Peak = m_Stats.Used;
        return block;
    }

    void Deallocate(void* ptr, size_t = 0) override {
        if (!ptr) return;
        *reinterpret_cast<void**>(ptr) = m_FreeList;
        m_FreeList = ptr;
        m_Stats.Used -= m_Stride;
        ++m_Stats.FreeCount;
    }

    void Reset() {
        m_FreeList = nullptr;
        for (void* slab : m_Slabs) ThreadSlab(slab);
        m_Stats.Used = 0; // Peak retained
    }

    const AllocatorStats& Stats() const override { return m_Stats; }
    MemCategory Category() const override { return m_Category; }
    const char* Name() const override { return m_Name; }

private:
    void ThreadSlab(void* slab) {
        uint8_t* base = static_cast<uint8_t*>(slab);
        for (size_t i = 0; i < m_BlockCount; ++i) {
            void* block = base + i * m_Stride;
            *reinterpret_cast<void**>(block) = m_FreeList;
            m_FreeList = block;
        }
    }

    void AddSlab() {
        void* slab = _aligned_malloc(m_Stride * m_BlockCount, m_BlockAlign);
        if (!slab) {
            SM_ERROR("PoolAllocator '%s': slab allocation failed (%zu bytes)",
                     m_Name, m_Stride * m_BlockCount);
            return;
        }
        m_Slabs.push_back(slab);
        ThreadSlab(slab);
        m_Stats.Capacity += m_Stride * m_BlockCount;
    }

    size_t              m_Stride = 0;
    size_t              m_BlockCount = 0;
    size_t              m_BlockAlign = 0;
    void*               m_FreeList = nullptr;
    std::vector<void*>  m_Slabs;
    MemCategory         m_Category;
    const char*         m_Name;
    AllocatorStats      m_Stats;
};

} // namespace Engine
```

- [ ] **Step 4: Run to verify pass**

Run:
```
cmake --build --preset msvc-win64-vs2026-enterprise --target test_alloc
./out/build/msvc-win64-vs2026-enterprise/bin/Debug/test_alloc.exe
```
Expected: `All allocator tests passed.`

- [ ] **Step 5: Commit**

```bash
git add src/engine/include/memory/PoolAllocator.h tests/test_alloc.cpp
git commit -m "feat(engine): PoolAllocator (growable chained slabs, stable pointers)"
```

---

### Task 5: FrameAllocator subclasses ArenaAllocator + Renderer registers it

Folds the existing `FrameAllocator` into the toolkit and links the editor to `Engine`. Render code is untouched because `ArenaAllocator` provides the full `FrameAllocator` API. **Integration task — verified by building the editor.**

**Files:**
- Modify: `src/editor/src/rendering/FrameAllocator.h`
- Modify: `src/editor/CMakeLists.txt`
- Modify: `src/editor/src/rendering/Renderer.cpp`

- [ ] **Step 1: Confirm the FrameAllocator API surface used by render code**

Run a search to list every member called on `FrameAllocator` / `m_FrameAllocator`:

Grep for `FrameAllocator` and `m_FrameAllocator` under `src/editor/src/rendering/`. Confirm every called member exists on `ArenaAllocator` (Task 3 provides: `Allocate<T>`, `AllocateArray<T>`, `AllocateBytes`, `Reset`, `GetUsedBytes`, `GetCapacity`, `GetPeakUsage`, `GetUsagePercent`, `ResetPeakUsage`). If a member is used that is NOT on `ArenaAllocator`, add it to `ArenaAllocator.h` (expressed over `m_Stats`/`m_Offset`) and re-run Task 3's tests before continuing.

- [ ] **Step 2: Rewrite FrameAllocator.h as a subclass**

Replace the entire contents of `src/editor/src/rendering/FrameAllocator.h` with:

```cpp
#pragma once

#include <lib.h>                    // MB()
#include <memory/ArenaAllocator.h>

// FrameAllocator: per-frame transient linear allocator. Now a thin subclass of
// Engine::ArenaAllocator (fixed capacity, bump, reset each frame). Kept as a
// distinct type so the `class FrameAllocator;` forward-decl in IRenderPass.h and
// all FrameAllocator* signatures stay valid — render code is unchanged.
class FrameAllocator : public Engine::ArenaAllocator {
public:
    explicit FrameAllocator(const size_t capacity = MB(16))
        : Engine::ArenaAllocator(capacity, Engine::MemCategory::FrameTransient, "Frame") {}
};
```

- [ ] **Step 3: Link the editor against Engine**

In `src/editor/CMakeLists.txt`, add `Engine` to the editor's link list. Change the line:

```cmake
target_link_libraries(editor PRIVATE CommonHeaders GameHeaders ecs freetype Tracy::TracyClient glm::glm glfw nvrhi nvrhi_d3d12 nvrhi_d3d11 d3dcompiler dxgi dxcompiler nvrhi_vk imgui ImGuizmo assimp::assimp nlohmann_json::nlohmann_json)
```

to include `Engine` (after `ecs`):

```cmake
target_link_libraries(editor PRIVATE CommonHeaders GameHeaders ecs Engine freetype Tracy::TracyClient glm::glm glfw nvrhi nvrhi_d3d12 nvrhi_d3d11 d3dcompiler dxgi dxcompiler nvrhi_vk imgui ImGuizmo assimp::assimp nlohmann_json::nlohmann_json)
```

(`Engine`'s PUBLIC include dir propagates `src/engine/include`, so `<memory/...>` headers resolve in the editor.)

- [ ] **Step 4: Register the frame allocator in Renderer**

In `src/editor/src/rendering/Renderer.cpp`, add near the top with the other includes:

```cpp
#include <memory/AllocatorRegistry.h>
```

Find where `Renderer` finishes initialization (the `Initialize`/`Init` method that sets up `m_FrameAllocator`'s siblings; if there is no explicit init, use the constructor body). Add:

```cpp
    Engine::Registry().Register(&m_FrameAllocator);
```

Find the matching teardown (`Shutdown` method, or the destructor `Renderer::~Renderer`). Add **before** any device teardown:

```cpp
    Engine::Registry().Unregister(&m_FrameAllocator);
```

If `Renderer` has neither an explicit `Initialize` nor `Shutdown`, register in the constructor and unregister in the destructor. The allocator must be unregistered before it is destroyed.

- [ ] **Step 5: Build the editor — verify it compiles and links**

Run (configure first — CMakeLists changed):
```
cmake --preset msvc-win64-vs2026-enterprise
cmake --build --preset msvc-win64-vs2026-enterprise --target editor
```
Expected: editor builds and links cleanly; `Engine.dll` is present in `out/build/msvc-win64-vs2026-enterprise/bin/Debug/`.

- [ ] **Step 6: Re-run allocator tests (no regression)**

Run:
```
cmake --build --preset msvc-win64-vs2026-enterprise --target test_alloc
./out/build/msvc-win64-vs2026-enterprise/bin/Debug/test_alloc.exe
```
Expected: `All allocator tests passed.`

- [ ] **Step 7: Commit**

```bash
git add src/editor/src/rendering/FrameAllocator.h src/editor/CMakeLists.txt src/editor/src/rendering/Renderer.cpp
git commit -m "refactor(editor): FrameAllocator subclasses ArenaAllocator; register with Engine registry"
```

---

### Task 6: Editor ImGui Memory panel

Adds a "Memory" window that reads `Engine::Registry()` and shows per-allocator and per-category stats. **Integration task — verified by building the editor; visual behavior smoke-tested by the user.**

**Files:**
- Create: `src/editor/src/rendering/imgui/MemoryPanel.h`
- Create: `src/editor/src/rendering/imgui/MemoryPanel.cpp`
- Modify: `src/editor/CMakeLists.txt`
- Modify: `src/editor/src/rendering/imgui/ImGuiRenderer.cpp`

- [ ] **Step 1: Write the panel header**

Create `src/editor/src/rendering/imgui/MemoryPanel.h`:

```cpp
#pragma once

// Draws the "Memory" debug window from the Engine allocator registry.
// `open` may be null (always draw) or point to a toggle bool.
void DrawMemoryPanel(bool* open);
```

- [ ] **Step 2: Write the panel implementation**

Create `src/editor/src/rendering/imgui/MemoryPanel.cpp`:

```cpp
#include "MemoryPanel.h"

#include <imgui.h>

#include <memory/AllocatorRegistry.h>
#include <memory/IAllocator.h>
#include <memory/MemoryCategory.h>

void DrawMemoryPanel(bool* open)
{
    if (open && !*open) return;
    if (!ImGui::Begin("Memory", open)) { ImGui::End(); return; }

    auto& reg = Engine::Registry();

    // Per-category rollups.
    if (ImGui::CollapsingHeader("By Category", ImGuiTreeNodeFlags_DefaultOpen)) {
        if (ImGui::BeginTable("mem_cat", 4,
                ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg)) {
            ImGui::TableSetupColumn("Category");
            ImGui::TableSetupColumn("Used");
            ImGui::TableSetupColumn("Peak");
            ImGui::TableSetupColumn("Capacity");
            ImGui::TableHeadersRow();
            for (uint8_t i = 0; i < (uint8_t)Engine::MemCategory::Count; ++i) {
                auto cat = (Engine::MemCategory)i;
                Engine::AllocatorStats s = reg.SumByCategory(cat);
                if (s.Capacity == 0 && s.Used == 0) continue;
                ImGui::TableNextRow();
                ImGui::TableNextColumn(); ImGui::TextUnformatted(Engine::ToString(cat));
                ImGui::TableNextColumn(); ImGui::Text("%zu", s.Used);
                ImGui::TableNextColumn(); ImGui::Text("%zu", s.Peak);
                ImGui::TableNextColumn(); ImGui::Text("%zu", s.Capacity);
            }
            ImGui::EndTable();
        }
    }

    // Per-allocator detail.
    if (ImGui::CollapsingHeader("Allocators", ImGuiTreeNodeFlags_DefaultOpen)) {
        if (ImGui::BeginTable("mem_alloc", 6,
                ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg)) {
            ImGui::TableSetupColumn("Name");
            ImGui::TableSetupColumn("Category");
            ImGui::TableSetupColumn("Used");
            ImGui::TableSetupColumn("Peak");
            ImGui::TableSetupColumn("Capacity");
            ImGui::TableSetupColumn("Alloc/Free");
            ImGui::TableHeadersRow();
            reg.ForEach([](Engine::IAllocator* a) {
                const Engine::AllocatorStats& s = a->Stats();
                ImGui::TableNextRow();
                ImGui::TableNextColumn(); ImGui::TextUnformatted(a->Name());
                ImGui::TableNextColumn(); ImGui::TextUnformatted(Engine::ToString(a->Category()));
                ImGui::TableNextColumn(); ImGui::Text("%zu", s.Used);
                ImGui::TableNextColumn(); ImGui::Text("%zu", s.Peak);
                ImGui::TableNextColumn(); ImGui::Text("%zu", s.Capacity);
                ImGui::TableNextColumn(); ImGui::Text("%llu/%llu",
                    (unsigned long long)s.AllocCount, (unsigned long long)s.FreeCount);
            });
            ImGui::EndTable();
        }
    }

    ImGui::End();
}
```

- [ ] **Step 3: Add the panel source to the editor CMake**

In `src/editor/CMakeLists.txt`, under the `#ImGui` group, add the source. Change:

```cmake
    #ImGui
    src/rendering/imgui/ImGuiRenderer.cpp
    src/rendering/imgui/imgui_nvrhi.cpp
    src/rendering/imgui/registered_font.cpp
    src/rendering/imgui/MeshPreviewRenderer.cpp
```

to:

```cmake
    #ImGui
    src/rendering/imgui/ImGuiRenderer.cpp
    src/rendering/imgui/imgui_nvrhi.cpp
    src/rendering/imgui/registered_font.cpp
    src/rendering/imgui/MeshPreviewRenderer.cpp
    src/rendering/imgui/MemoryPanel.cpp
```

- [ ] **Step 4: Call the panel from ImGuiRenderer**

In `src/editor/src/rendering/imgui/ImGuiRenderer.cpp`, add the include near the other panel/local includes:

```cpp
#include "MemoryPanel.h"
```

Find where other ImGui windows are drawn inside the render method (search for `ImGui::Begin` calls in that file). Add, alongside them:

```cpp
    static bool s_ShowMemoryPanel = true;
    DrawMemoryPanel(&s_ShowMemoryPanel);
```

(A `static` local toggle keeps this self-contained. If the editor has a "Windows"/"View" menu with `ImGui::MenuItem` toggles, also add `ImGui::MenuItem("Memory", nullptr, &s_ShowMemoryPanel);` there — optional.)

- [ ] **Step 5: Build the editor — verify it compiles and links**

Run (configure first — CMakeLists changed):
```
cmake --preset msvc-win64-vs2026-enterprise
cmake --build --preset msvc-win64-vs2026-enterprise --target editor
```
Expected: editor builds and links cleanly.

- [ ] **Step 6: Commit**

```bash
git add src/editor/src/rendering/imgui/MemoryPanel.h src/editor/src/rendering/imgui/MemoryPanel.cpp src/editor/CMakeLists.txt src/editor/src/rendering/imgui/ImGuiRenderer.cpp
git commit -m "feat(editor): ImGui Memory panel reading the Engine allocator registry"
```

---

### Task 7: Full build + verification

Final integration check across all targets.

**Files:** none (verification only).

- [ ] **Step 1: Clean configure + build everything**

Run:
```
cmake --preset msvc-win64-vs2026-enterprise
cmake --build --preset msvc-win64-vs2026-enterprise
```
Expected: `Engine`, `ecs`, `game`, `editor`, `runtime`, `test_ecs`, `test_alloc` all build. `Engine.dll` sits next to `editor.exe` in `bin/Debug/`.

- [ ] **Step 2: Run both test suites**

Run:
```
./out/build/msvc-win64-vs2026-enterprise/bin/Debug/test_alloc.exe
./out/build/msvc-win64-vs2026-enterprise/bin/Debug/test_ecs.exe
```
Expected: `All allocator tests passed.` and `All ECS tests passed.`

- [ ] **Step 3: Editor smoke test (user-driven)**

Launch the editor. Confirm:
- It starts and renders the scene (camera + entities visible, no regression).
- The "Memory" window appears and lists the `Frame` allocator under category `FrameTransient`, with `Used` rising during a frame and `Capacity` = 16 MB.

This step requires the running UI; report explicitly that it must be smoke-tested rather than claiming success from a compile.

- [ ] **Step 4: Final commit (if any verification fixups were needed)**

If steps 1–3 required fixes, commit them:

```bash
git add -A
git commit -m "fix(engine): allocator toolkit verification fixups"
```

---

## Self-Review

**1. Spec coverage:**
- Engine SHARED lib + ENGINE_API → Task 1. ✓
- Header-only toolkit (Stats, Category, IAllocator, MemUtil, Arena, Pool) → Tasks 1–4. ✓
- Per-allocator category tag + identity → Tasks 3, 4 (ctor takes MemCategory + name). ✓
- AllocatorRegistry (locked, non-owning, single instance, SumByCategory) → Task 2. ✓
- Explicit registration (Renderer registers FrameAllocator) → Task 5. ✓
- FrameAllocator = ArenaAllocator subclass, zero render churn → Task 5. ✓
- Arena fixed (loud error) → Task 3. ✓
- Pool growable chained slabs, stable pointers → Task 4. ✓
- Editor ImGui Memory panel, Engine has no ImGui dep → Task 6. ✓
- test_alloc target mirroring test_ecs → Tasks 1–4. ✓
- Output to RUNTIME_DIR (no copy step) → Task 1 CMake. ✓
- ECS churn deferred → not in plan (correct). ✓

**2. Placeholder scan:** The only "placeholder" is the deliberate temporary `AllocatorRegistry.cpp` stub in Task 1 Step 4, explicitly replaced in Task 2 Step 3 (so the SHARED target compiles before the registry exists). No TBD/TODO/"handle edge cases" left.

**3. Type consistency:** `MemCategory`, `AllocatorStats`, `IAllocator` (Allocate/Deallocate/Stats/Category/Name) are defined in Tasks 1–2 and used unchanged in Tasks 3–6. Arena/Pool ctors both take `(... , MemCategory, const char*)`. `Registry()` / `AllocatorRegistry` names consistent across Tasks 2, 5, 6. `FrameAllocator` keeps its name and `MB(16)` default. Arena provides every `FrameAllocator` member listed in Task 5 Step 1.

**Note for executor:** clang-cl/MSVC may emit C4251 on `AllocatorRegistry`'s exported class with `std::mutex`/`std::vector` members — this matches the existing `ECS_API ComponentStore` pattern (all methods out-of-line, shared CRT) and is safe. Do not "fix" it by inlining the methods.
