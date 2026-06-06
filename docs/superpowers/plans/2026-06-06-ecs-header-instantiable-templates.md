# ECS Header-Instantiable Templates (Boundary Piece 1) Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Let `Game.dll` (and any consumer) define and use its own ECS component/singleton types with no `ecs.dll` rebuild or editor restart, by moving the ECS template machinery into the header while keeping built-ins explicitly instantiated in `ecs.dll`.

**Architecture:** ECS storage is already a runtime `type_index → IComponentArray` map, so the *only* gate is that the templated method bodies (`ComponentStore`/`ECS` `<T>` methods, `ComponentArray<T>::Clone`, the COW array pool) live in `ecs.cpp` and are emitted only for the X-macro set. We move those bodies into `ECS.h`, drop the class-level `__declspec` from the `ComponentArray` template (export comes solely from the explicit instantiation), and export the array-pool counters from `ecs.dll` so stats stay unified. Built-ins keep one exported copy in `ecs.dll` via the existing `extern template` decls; a type absent from the X-macro instantiates locally in whatever module names it.

**Tech Stack:** C++23, MSVC (`msvc-win64-vs2026-community` preset), CMake, `ecs.dll` (`src/ecs`), `ECS.h` (`src/common/include`), `test_ecs` unit target.

**Scope:** This is Piece 1 of the engine/game boundary spec (`docs/superpowers/specs/2026-06-06-engine-game-boundary-design.md`). Pieces 2–5 (game-owned states, raw input, registered serialization, generic inspector editing) are separate plans. This plan does **not** add serialization or editor support for game types — only storage/snapshot/COW.

---

## Background: why a "failing test" here is a link error

`test_ecs.exe` is a separate target that links `ecs.dll`. Today, referencing a component type that is **not** in `ECS_FOR_EACH_REGISTERED_COMPONENT` (ECS.h:374–408) from `test_ecs.cpp` compiles but **fails to link** — `ComponentStore::AddComponent<T>` / `ECS::AddComponent<T>` bodies live only in `ecs.cpp` and are emitted only for the registered set, so the linker reports an unresolved external. That link failure is the Task 1 "red". After the refactor the bodies are header-visible, the type instantiates locally in the `test_ecs` TU, and the same test links and passes.

## Files

- **Modify `src/common/include/ECS.h`** — primary change. Move into the header: the COW array-pool machinery (in a new named `ecs::detail` namespace), `ComponentArray<T>::Clone()`, and the `ComponentStore`/`ECS` templated method bodies. Drop the class-level `ECS_API` on the `ComponentArray` template. Declare an exported `ecs::detail::ArrayPoolCounters()`.
- **Modify `src/ecs/src/ecs.cpp`** — remove the moved template bodies + the anonymous-namespace pool; keep the explicit instantiations (built-ins) and define the now-exported `ecs::detail::ArrayPoolCounters()` + `ArrayPoolCountersT`. `GetComponentArrayPoolStats()` reads it.
- **Modify `tests/test_ecs.cpp`** — add a component type defined **in the test TU** (outside `ecs.dll`) and exercise add/get/has/remove, snapshot isolation + COW divergence, and pool-balance.
- No `tests/CMakeLists.txt` change — `test_ecs` already links `ecs` (tests/CMakeLists.txt:1–9).

## Type/symbol contract (used across tasks — keep names exact)

- `namespace ecs::detail` holds: `struct ArrayPoolCountersT`, `ECS_API ArrayPoolCountersT& ArrayPoolCounters()`, `template<class T> class ComponentArrayPool`, `template<class T> ComponentArrayPool<T>& GetArrayPool()`, `template<class T> std::shared_ptr<ComponentArray<T>> MakePooledClone(const ComponentArray<T>&)`.
- `ArrayPoolCountersT` keeps fields/methods `Free, InUse, Created, Reuses` (atomics) + `OnCreate/OnReuse/OnRecycle` exactly as today (ecs.cpp:11–19).
- `ComponentArray` class template loses its `ECS_API` attribute (declared `class ComponentArray final : public IComponentArray`). Built-ins are still exported by the unchanged `template class ECS_API ComponentArray<T>;` block in ecs.cpp.

---

### Task 1: Header-instantiable refactor (out-of-DLL type works)

This is one atomic ABI change: the codebase will not compile/link cleanly at intermediate sub-steps, so all edits land in a single commit. Do the sub-steps in order, then build once.

**Files:**
- Test: `tests/test_ecs.cpp`
- Modify: `src/common/include/ECS.h`
- Modify: `src/ecs/src/ecs.cpp`

- [ ] **Step 1: Add the failing test (defines a type outside ecs.dll)**

The test harness (tests/test_ecs.cpp:23–35) is custom: `static int g_Failures`, an `EXPECT(cond)` macro and `EXPECT_EQ(a,b)`/`EXPECT_NE(a,b)`, tests written as `static void TXX_name()` and called from `main()` (test_ecs.cpp:1048+). Use that harness — **not** `assert`/`printf`.

Add this type + test function near the other component tests (anywhere above `main()`):

```cpp
// A component type intentionally NOT in ECS_FOR_EACH_REGISTERED_COMPONENT.
// Proves a consumer (this test TU, outside ecs.dll) can define + use its own
// component with no ecs.dll registration. Before Piece 1 this fails to LINK.
struct ExternProbeComponent {
    int   Value = 0;
    float Ratio = 0.0f;
};

static void TX01_extern_component_basic()
{
    ECS world;
    const EntityId e = world.CreateEntity();

    world.AddComponent<ExternProbeComponent>(e, ExternProbeComponent{ 42, 1.5f });
    EXPECT(world.HasComponent<ExternProbeComponent>(e));

    const ExternProbeComponent* got = world.GetComponent<ExternProbeComponent>(e);
    EXPECT(got != nullptr);
    EXPECT_EQ(got->Value, 42);
    EXPECT_EQ(got->Ratio, 1.5f);

    world.RemoveComponent<ExternProbeComponent>(e);
    EXPECT(!world.HasComponent<ExternProbeComponent>(e));
}
```

Register it in `main()` (test_ecs.cpp) by adding `TX01_extern_component_basic();` after the last existing test call (after `T61_duplicate_invalid_src_is_noop();`, near line 1106).

- [ ] **Step 2: Build and confirm the LINK failure (red)**

Run:
```
cmake --build --preset msvc-win64-vs2026-community --target test_ecs
```
Expected: **link error** — `unresolved external symbol` for `ComponentStore::AddComponent<ExternProbeComponent>` (and/or `ECS::AddComponent<ExternProbeComponent>`). This confirms the gate. (If it links already, stop — an assumption is wrong.)

- [ ] **Step 3: Move the array-pool machinery into `ECS.h` under `ecs::detail`**

In `src/ecs/src/ecs.cpp`, **delete** the anonymous-namespace block at lines 6–68 (the `ArrayPoolCountersT` struct, `ArrayPoolCounters()`, `ComponentArrayPool<T>`, `GetArrayPool<T>()`, `MakePooledClone<T>`) and the out-of-line `ComponentArray<T>::Clone()` at lines 72–75.

In `src/common/include/ECS.h`, immediately **after** the `ComponentArray` class definition and its `extern template` block (after line 590), add:

```cpp
// ---- COW array recycle pool (header so consumer-defined component types
// instantiate their own pool locally; built-in T's pools live in ecs.dll via
// the explicit instantiations). Counters are a single exported instance so the
// editor Memory panel aggregates every module's pools. ----
namespace ecs::detail {

struct ArrayPoolCountersT {
    std::atomic<size_t>   Free{0};
    std::atomic<size_t>   InUse{0};
    std::atomic<size_t>   Created{0};
    std::atomic<uint64_t> Reuses{0};
    void OnCreate()  noexcept { ++InUse; ++Created; }
    void OnReuse()   noexcept { --Free;  ++InUse; ++Reuses; }
    void OnRecycle() noexcept { ++Free;  --InUse; }
};

// Single instance, defined + exported from ecs.dll so all modules share counts.
ECS_API ArrayPoolCountersT& ArrayPoolCounters();

template<typename T>
class ComponentArrayPool {
public:
    ComponentArray<T>* Acquire() {
        std::lock_guard<std::mutex> lock(m_Mutex);
        if (!m_Free.empty()) {
            ComponentArray<T>* a = m_Free.back();
            m_Free.pop_back();
            ArrayPoolCounters().OnReuse();
            return a;
        }
        ArrayPoolCounters().OnCreate();
        return new ComponentArray<T>();
    }
    void Recycle(ComponentArray<T>* a) noexcept {
        std::lock_guard<std::mutex> lock(m_Mutex);
        m_Free.push_back(a);
        ArrayPoolCounters().OnRecycle();
    }
    ~ComponentArrayPool() { for (ComponentArray<T>* a : m_Free) delete a; }
private:
    std::mutex m_Mutex;
    std::vector<ComponentArray<T>*> m_Free;
};

template<typename T>
ComponentArrayPool<T>& GetArrayPool() { static ComponentArrayPool<T> pool; return pool; }

template<typename T>
std::shared_ptr<ComponentArray<T>> MakePooledClone(const ComponentArray<T>& src) {
    std::shared_ptr<ComponentArray<T>> arr(
        GetArrayPool<T>().Acquire(),
        [](ComponentArray<T>* p) noexcept { GetArrayPool<T>().Recycle(p); });
    arr->CopyFrom(src);
    return arr;
}

} // namespace ecs::detail

template<typename T>
inline std::shared_ptr<IComponentArray> ComponentArray<T>::Clone() const {
    return ecs::detail::MakePooledClone(*this);
}
```

Add `#include <mutex>` and `#include <atomic>` to `ECS.h`'s include block (lines 2–16) if not already present.

In `src/ecs/src/ecs.cpp`, add the single exported definition (e.g. just below the remaining includes):
```cpp
namespace ecs::detail {
ArrayPoolCountersT& ArrayPoolCounters() { static ArrayPoolCountersT c; return c; }
} // namespace ecs::detail
```
And update `GetComponentArrayPoolStats()` (was ecs.cpp:261–269) to read it:
```cpp
ComponentArrayPoolStats GetComponentArrayPoolStats() {
    const auto& c = ecs::detail::ArrayPoolCounters();
    return ComponentArrayPoolStats{
        c.Free.load(std::memory_order_relaxed),
        c.InUse.load(std::memory_order_relaxed),
        c.Created.load(std::memory_order_relaxed),
        c.Reuses.load(std::memory_order_relaxed)
    };
}
```

- [ ] **Step 4: Drop the class-level `ECS_API` on the `ComponentArray` template**

In `src/common/include/ECS.h:435`, change:
```cpp
template<typename T>
class ECS_API ComponentArray final : public IComponentArray {
```
to:
```cpp
template<typename T>
class ComponentArray final : public IComponentArray {
```
Rationale: a class-level attribute can't differ per-`T`, and `dllimport` on a template the consumer must instantiate itself (game types) is wrong. Export of built-ins comes solely from the unchanged `template class ECS_API ComponentArray<T>;` explicit-instantiation block (ecs.cpp:79–81); import of built-ins in consumers comes from the unchanged `extern template class ECS_API ComponentArray<T>;` block (ECS.h:586–590). Leave both of those blocks exactly as they are.

- [ ] **Step 5: Move the `ComponentStore` templated method bodies into `ECS.h`**

In `src/ecs/src/ecs.cpp`, delete the `ComponentStore` template definitions (lines 85–133: `MutateArray`, `GetArray`, `AddComponent`, `RemoveComponent`, `HasComponent`, `GetComponent`). **Keep** the explicit-instantiation block (ecs.cpp:137–145).

In `src/common/include/ECS.h`, replace the in-class method *declarations* on `ComponentStore` (lines 609–660: `AddComponent`, `RemoveComponent`, `GetComponent`, `HasComponent`, `MutateArray`, `GetArray`) with inline definitions. `RegisterComponent`/`GetComponentArray` already have inline bodies — leave them. The bodies (verbatim from the old ecs.cpp, now inline in the class):

```cpp
    template<typename T>
    void AddComponent(EntityId entity, T component) { MutateArray<T>().Add(entity, component); }

    template<typename T>
    void RemoveComponent(EntityId entity) { MutateArray<T>().Remove(entity); }

    template<typename T>
    const T* GetComponent(EntityId entity) const {
        const auto componentArray = GetComponentArray<T>();
        return componentArray ? componentArray->Get(entity) : nullptr;
    }

    template<typename T>
    [[nodiscard]] bool HasComponent(EntityId entity) const {
        auto array = GetComponentArray<T>();
        return array && array->Has(entity);
    }

    template<typename T>
    ComponentArray<T>& MutateArray() {
        AssertOwnerThread();
        const auto typeIndex = std::type_index(typeid(T));
        auto& slot = m_ComponentArrays[typeIndex];
        if (!slot) slot = std::make_shared<ComponentArray<T>>();
        if (m_DirtyThisTick.insert(typeIndex).second) {
            slot = ecs::detail::MakePooledClone<T>(static_cast<const ComponentArray<T>&>(*slot));
        }
        return static_cast<ComponentArray<T>&>(*slot);
    }

    template<typename T>
    const ComponentArray<T>* GetArray() const {
        const auto typeIndex = std::type_index(typeid(T));
        const auto it = m_ComponentArrays.find(typeIndex);
        return it == m_ComponentArrays.end()
            ? nullptr
            : static_cast<const ComponentArray<T>*>(it->second.get());
    }
```

Leave the `extern template ... ComponentStore::...` block (ECS.h:702–712) unchanged — it still suppresses local instantiation of the built-in `T`s in consumers and imports them from `ecs.dll`.

- [ ] **Step 6: Move the `ECS` templated method bodies into `ECS.h`**

In `src/ecs/src/ecs.cpp`, delete the `ECS::` template definitions (lines 149–177: `AddComponent`, `RemoveComponent`, `HasComponent`, `GetComponent`, `GetArray`, `MutateArray`). **Keep** the explicit-instantiation block (ecs.cpp:284–292).

In `src/common/include/ECS.h`, inside the `ECS` class (after line 813), give those methods inline bodies (find their existing declarations in the `ECS` class and replace with definitions; they simply forward to `m_ComponentStore`):

```cpp
    template<typename T>
    void AddComponent(EntityId entity, T component) { m_ComponentStore.AddComponent<T>(entity, std::move(component)); }

    template<typename T>
    void RemoveComponent(EntityId entity) { m_ComponentStore.RemoveComponent<T>(entity); }

    template<typename T>
    [[nodiscard]] bool HasComponent(EntityId entity) const { return m_ComponentStore.HasComponent<T>(entity); }

    template<typename T>
    const T* GetComponent(EntityId entity) const { return m_ComponentStore.GetComponent<T>(entity); }

    template<typename T>
    const ComponentArray<T>* GetArray() const { return m_ComponentStore.GetArray<T>(); }

    template<typename T>
    ComponentArray<T>& MutateArray() { return m_ComponentStore.MutateArray<T>(); }
```
Leave the `ECS_EXTERN_ECS_METHODS` block (ECS.h:984–992, guarded by `#ifndef ECS_EXPORTS`) **unchanged**. `extern template` declarations legally coexist with a now-visible inline body: for built-in `T` the consumer suppresses local instantiation and imports from `ecs.dll`; for game `T` (no extern decl) the visible body instantiates locally. In `ecs.dll`'s own TU the extern block is guarded out and the explicit instantiations (ecs.cpp:284–292) are the single definition, sourced from the header body.

- [ ] **Step 7: Build `test_ecs` — the red test now passes (green)**

Run:
```
cmake --build --preset msvc-win64-vs2026-community --target ecs --target test_ecs
./out/build/msvc-win64-vs2026-community/bin/Debug/test_ecs.exe
```
Expected: links cleanly and ends with `All ECS tests passed.` (`g_Failures == 0`; the harness prints only the final summary, not per-test lines).
Watch for MSVC `C4910`/`LNK` attribute warnings — the existing `#ifdef ECS_EXPORTS` guards (ECS.h:586, 702) should keep them suppressed. If a new C4910 appears, confirm Step 4 removed the class-level attribute (it must be gone, not duplicated).

- [ ] **Step 8: Commit**

```bash
git add src/common/include/ECS.h src/ecs/src/ecs.cpp tests/test_ecs.cpp
git commit -m "refactor(ecs): header-instantiable component templates so consumers define their own types"
```

---

### Task 2: Snapshot isolation + COW + pool-balance for an out-of-DLL type

Proves the harder paths — the snapshot deep-copy and copy-on-write clone work for a type instantiated outside `ecs.dll`, and the pool counters stay balanced across the module boundary.

**Files:**
- Test: `tests/test_ecs.cpp`

- [ ] **Step 1: Write the failing test**

Add to `tests/test_ecs.cpp` (reuses `ExternProbeComponent` from Task 1) and register both in `main()` after `TX01_extern_component_basic();`:

```cpp
static void TX02_extern_snapshot_cow()
{
    ECS world;
    const EntityId e = world.CreateEntity();
    world.AddComponent<ExternProbeComponent>(e, ExternProbeComponent{ 7, 0.5f });

    // Snapshot, then mutate the master. The snapshot must NOT see the change (COW).
    std::shared_ptr<const ECS> snap = world.CreateSnapshot();
    world.MutateArray<ExternProbeComponent>().Get(e)->Value = 999;

    const ExternProbeComponent* snapComp = snap->GetComponent<ExternProbeComponent>(e);
    EXPECT(snapComp != nullptr);
    EXPECT_EQ(snapComp->Value, 7);                                  // snapshot retains pre-mutation value
    EXPECT_EQ(world.GetComponent<ExternProbeComponent>(e)->Value, 999); // master diverged
}

static void TX03_extern_pool_balance()
{
    // Acquire+release cycles must leave InUse balanced (deleter recycles across
    // the module boundary). Compare InUse before/after a scope that clones.
    const size_t inUseBefore = GetComponentArrayPoolStats().InUse;
    {
        ECS world;
        const EntityId e = world.CreateEntity();
        world.AddComponent<ExternProbeComponent>(e, ExternProbeComponent{ 1, 1.0f });
        std::shared_ptr<const ECS> snap = world.CreateSnapshot();
        world.MutateArray<ExternProbeComponent>().Get(e)->Value = 2; // forces a clone from the pool
    } // snap + world drop here; pooled arrays recycle via deleters
    const size_t inUseAfter = GetComponentArrayPoolStats().InUse;
    EXPECT_EQ(inUseAfter, inUseBefore);                            // no leak / double-free
}
```

- [ ] **Step 2: Build + run — expect PASS (machinery already landed in Task 1)**

Run:
```
cmake --build --preset msvc-win64-vs2026-community --target test_ecs
./out/build/msvc-win64-vs2026-community/bin/Debug/test_ecs.exe
```
Expected: ends with `All ECS tests passed.` (`g_Failures == 0`).
(These pass immediately because Task 1 made the machinery header-instantiable. If `TX02` fails — snapshot sees `999` — the COW clone isn't firing for the extern type; revisit Step 5/Step 3 of Task 1. If `TX03` fails, the pool deleter or the exported counters are wrong; revisit Step 3 of Task 1.)

- [ ] **Step 3: Commit**

```bash
git add tests/test_ecs.cpp
git commit -m "test(ecs): snapshot COW + pool balance for a component type defined outside ecs.dll"
```

---

### Task 3: Full rebuild, regression, and Memory-panel sanity

The refactor changed `ECS.h` (an `ecs.dll`/common header), so every consumer recompiles. Confirm the whole tree builds and behaves, and that the editor's array-pool stats still read non-garbage through the exported counter.

**Files:**
- None (build + run verification; commit only if a fixup is needed).

- [ ] **Step 1: Full clean build**

Run:
```
cmake --build --preset msvc-win64-vs2026-community
```
Expected: `ecs`, `Engine`, `game`, `editor`, `runtime`, and all `test_*` targets build with no errors. Watch specifically for `C4910` (dllexport+extern) or `LNK2005` (duplicate symbol — would mean a built-in `T` got instantiated in two modules; if so, a `extern template` decl was dropped by mistake).

- [ ] **Step 2: Run the ECS + allocator + serialization suites**

Run:
```
./out/build/msvc-win64-vs2026-community/bin/Debug/test_ecs.exe
./out/build/msvc-win64-vs2026-community/bin/Debug/test_alloc.exe
./out/build/msvc-win64-vs2026-community/bin/Debug/test_worldserial.exe
./out/build/msvc-win64-vs2026-community/bin/Debug/test_collision.exe
./out/build/msvc-win64-vs2026-community/bin/Debug/test_navmesh.exe
```
Expected: `All ECS tests passed.`, `All allocator tests passed.`, and each of the others prints its own pass line / exits 0. (These four exercise built-in components through the refactored paths.) Report any non-zero exit with its output.

- [ ] **Step 3: Manual Memory-panel sanity (editor)**

Launch the editor:
```
./out/build/msvc-win64-vs2026-community/bin/Debug/editor.exe
```
Open the **Memory** panel and confirm the "ComponentArray pool" stats (Free/InUse/Created/Reuses) show plausible non-zero, non-garbage values that change as the scene runs — proving the exported `ecs::detail::ArrayPoolCounters()` is the single instance the running ECS feeds (MemoryPanel.cpp:106 reads `GetComponentArrayPoolStats()`). Close the editor.

- [ ] **Step 4: Commit any fixups (if Steps 1–3 required edits)**

```bash
git add -A
git commit -m "fix(ecs): resolve header-instantiation fallout from full rebuild"
```
If no fixups were needed, skip this step.

---

## Done criteria

- A component type defined outside `ecs.dll` can be added/queried/removed, survives snapshot with COW isolation, and leaves the array pool balanced (Tasks 1–2).
- Full tree builds; ECS/alloc/worldserial/collision/navmesh suites green; editor Memory panel pool stats live (Task 3).
- No `ecs.dll` source change is required to *use* a new game-side component — only this one-time refactor. (Future game components arrive in `Game.dll` with no rebuild of `ecs.dll`, proving the boundary goal.)

## Next plans (not this one)

Piece 2 (game-owned `GameStateId`), Piece 3 (raw input to game), Piece 4 (registered serialization), Piece 5 (generic inspector editing) — each its own spec-derived plan, written when this lands. Piece 4 builds directly on the header-instantiable machinery from this plan.
