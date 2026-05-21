# ECS `Each` Iteration Primitive Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Replace the heap-allocating `ECS::View<Components...>()` with a zero-allocation `Each<Components...>(callback)` primitive, migrate all 5 callers, and remove `View`.

**Architecture:** `Each` is a header-only `const` template on `ECS` that iterates active entities and invokes a callback per full match — passing the queried components by const ref if the callback accepts them (`if constexpr`), else just the `EntityId`. ECS stays allocator-agnostic (no `Engine.dll` dependency). Collect-needing render sites fill a `FrameAllocator` arena buffer sized to `GetEntityCount()` via the callback.

**Tech Stack:** C++23, custom `test_ecs` harness (no GPU), NVRHI render passes, the `FrameAllocator` arena.

**Spec:** `docs/superpowers/specs/2026-05-21-ecs-each-iteration-design.md`

**Model guidance:** Dispatch **all** implementer + reviewer subagents on **Opus 4.7**. Highest-risk: Task 1 (the `if constexpr` + tuple/apply template) and Task 5 (removing `View` + proving zero `View<` references remain).

**Branch:** Already on `allocator-toolkit` (stacking). Do not switch branches.

**Build/ABI note (important):** `Each`/`View` are inline header templates in `ECS.h` — they are NOT part of the `ecs.cpp` X-macro explicit instantiations, so `ecs.dll`'s ABI does not change. Adding `Each` and removing `View` do **NOT** change `GameState` layout or any export signature → **do NOT bump `GAME_API_VERSION`.** But `ECS.h` is shared, so `editor`, `game`, and `test_ecs` all recompile. Build preset: `msvc-win64-vs2026-community` (enterprise not installed).

**Ordering rationale:** `View` is kept until Task 5 so every intermediate task builds green; callers migrate off it in Tasks 2-4, then Task 5 deletes the now-unused method.

---

## File Structure

- **Modify** `src/common/include/ECS.h` — add `Each` + `#include <tuple>` (Task 1); remove `View` + update the example comment (Task 5).
- **Modify** `tests/test_ecs.cpp` — `Each` unit tests (Task 1).
- **Modify** `src/game/src/game.cpp` — migrate TextRotation + DayNight systems (Task 2).
- **Modify** `src/editor/src/rendering/passes/MeshRenderPass.cpp` — migrate lights + batching (Task 3).
- **Modify** `src/editor/src/rendering/passes/UiRenderPass.cpp` — migrate text list (Task 4).

## Conventions for every task

- Build: `cmake --build --preset msvc-win64-vs2026-community --target <test_ecs|game|editor>`. Run ECS tests: `./out/build/msvc-win64-vs2026-community/bin/Debug/test_ecs.exe` (expect `All ECS tests passed.`).
- Use the Bash tool. Author identity personal `Nuno Silva <nuno.levezinho@live.com.pt>` (already configured). Never stage the untracked `.claude/` directory.

---

### Task 1: Add `Each` to ECS + unit tests (keep `View`)

**Files:**
- Modify: `src/common/include/ECS.h`
- Modify: `tests/test_ecs.cpp`

- [ ] **Step 1: Write the failing tests**

In `tests/test_ecs.cpp`, add these tests before `main` (the harness defines `EXPECT`/`EXPECT_EQ`; `ECS`, `TransformComponent`, `MeshComponent`, `EntityId`, `INVALID_ENTITY` are already available via the existing includes):

```cpp
static void TE01_each_visits_matching_entities()
{
    ECS w;
    EntityId a = w.CreateEntity(); w.AddComponent(a, TransformComponent{});
    EntityId b = w.CreateEntity(); w.AddComponent(b, TransformComponent{}); w.AddComponent(b, MeshComponent{});
    EntityId c = w.CreateEntity(); w.AddComponent(c, MeshComponent{});
    (void)a; (void)c;

    int count = 0; EntityId seen = INVALID_ENTITY;
    w.Each<TransformComponent, MeshComponent>([&](EntityId e){ ++count; seen = e; });
    EXPECT_EQ(count, 1);
    EXPECT_EQ(seen, b);
}

static void TE02_each_components_form()
{
    ECS w;
    EntityId a = w.CreateEntity();
    w.AddComponent(a, TransformComponent{{1.0f, 2.0f, 3.0f}, {}, {1, 1, 1}});
    w.AddComponent(a, MeshComponent{ .MeshId = 42, .Visible = true });

    int count = 0; uint32_t meshId = 0; float px = 0.0f;
    w.Each<TransformComponent, MeshComponent>(
        [&](EntityId, const TransformComponent& t, const MeshComponent& m){
            ++count; meshId = m.MeshId; px = t.Position.x;
        });
    EXPECT_EQ(count, 1);
    EXPECT_EQ(meshId, 42u);
    EXPECT_EQ(px, 1.0f);
}

static void TE03_each_zero_matches()
{
    ECS w;
    EntityId a = w.CreateEntity(); w.AddComponent(a, TransformComponent{});
    (void)a;
    int count = 0;
    w.Each<MeshComponent>([&](EntityId){ ++count; });
    EXPECT_EQ(count, 0);
}

static void TE04_each_single_component()
{
    ECS w;
    w.AddComponent(w.CreateEntity(), TransformComponent{});
    w.AddComponent(w.CreateEntity(), TransformComponent{});
    w.CreateEntity(); // no components
    int count = 0;
    w.Each<TransformComponent>([&](EntityId){ ++count; });
    EXPECT_EQ(count, 2);
}

static void TE05_each_does_not_change_entity_count()
{
    ECS w;
    w.AddComponent(w.CreateEntity(), TransformComponent{});
    size_t before = w.GetEntityCount();
    w.Each<TransformComponent>([&](EntityId){});
    EXPECT_EQ(w.GetEntityCount(), before);
}
```

Register them in `main` (after the existing test calls):

```cpp
    TE01_each_visits_matching_entities();
    TE02_each_components_form();
    TE03_each_zero_matches();
    TE04_each_single_component();
    TE05_each_does_not_change_entity_count();
```

- [ ] **Step 2: Run to verify failure**

Run:
```
cmake --build --preset msvc-win64-vs2026-community --target test_ecs
```
Expected: compile error — `Each` is not a member of `ECS`.

- [ ] **Step 3: Implement `Each`**

In `src/common/include/ECS.h`, add `#include <tuple>` to the include block at the top (alongside `<vector>`, `<unordered_map>`, etc.).

Then add the `Each` template to the `ECS` class, immediately **above** the existing `View` method (leave `View` in place for now). The `View` method currently reads:

```cpp
    // Iterate entities with specific components (simple view)
    template<typename... Components>
    [[nodiscard]] std::vector<EntityId> View() const {
```

Insert before that comment:

```cpp
    /**
     * @brief Zero-allocation iteration over entities that have all Components.
     *        If the callback accepts (EntityId, const Components&...), each
     *        queried component is passed by const reference (guaranteed non-null
     *        on a full match); otherwise the callback is invoked with (EntityId).
     * @threading const; safe on snapshots. Mutation still goes through
     *            Modify/MutateArray (the refs here are read-only).
     */
    template<typename... Components, typename F>
    void Each(F&& fn) const {
        for (EntityId entity : m_EntityStore.GetActiveEntities()) {
            if constexpr (std::is_invocable_v<F, EntityId, const Components&...>) {
                const std::tuple ptrs{ GetComponent<Components>(entity)... };
                const bool all = std::apply([](auto*... p){ return (p && ...); }, ptrs);
                if (all) std::apply([&](auto*... p){ fn(entity, *p...); }, ptrs);
            } else {
                if ((HasComponent<Components>(entity) && ...)) fn(entity);
            }
        }
    }

```

- [ ] **Step 4: Run to verify pass**

Run:
```
cmake --build --preset msvc-win64-vs2026-community --target test_ecs
./out/build/msvc-win64-vs2026-community/bin/Debug/test_ecs.exe
```
Expected: `All ECS tests passed.`

- [ ] **Step 5: Commit**

```bash
git add src/common/include/ECS.h tests/test_ecs.cpp
git commit -m "feat(ecs): zero-alloc Each<Components...> iteration primitive + tests"
```

---

### Task 2: Migrate game.dll systems to `Each`

Both systems iterate then `Modify` inside — they don't read the queried components, so use the **entity-only** form.

**Files:**
- Modify: `src/game/src/game.cpp`

- [ ] **Step 1: Migrate TextRotationSystem**

In `src/game/src/game.cpp`, change:

```cpp
        for (EntityId e : ctx.world.View<TextComponent, TransformComponent>()) {
            ctx.world.Modify<TransformComponent>(e, [&](auto& transform) {
                constexpr float TWO_PI = 6.28318530718f;
                transform.Rotation.z = fmodf(
                    transform.Rotation.z + glm::radians(180.0f) * static_cast<float>(ctx.dt),
                    TWO_PI);
            });
            ctx.world.Modify<TextComponent>(e, [&](auto& text) {
                const auto time = static_cast<float>(ctx.dt);
                const float red = (sinf(time) + 1.0f) / 2.0f;
                const float green = (cosf(ctx.gameTime) + 1.0f) / 2.0f;
                const float blue = 1.0f - red;
                text.Color = glm::vec4(red, green, blue, 1.0f);
            });
        }
```

to:

```cpp
        ctx.world.Each<TextComponent, TransformComponent>([&](EntityId e) {
            ctx.world.Modify<TransformComponent>(e, [&](auto& transform) {
                constexpr float TWO_PI = 6.28318530718f;
                transform.Rotation.z = fmodf(
                    transform.Rotation.z + glm::radians(180.0f) * static_cast<float>(ctx.dt),
                    TWO_PI);
            });
            ctx.world.Modify<TextComponent>(e, [&](auto& text) {
                const auto time = static_cast<float>(ctx.dt);
                const float red = (sinf(time) + 1.0f) / 2.0f;
                const float green = (cosf(ctx.gameTime) + 1.0f) / 2.0f;
                const float blue = 1.0f - red;
                text.Color = glm::vec4(red, green, blue, 1.0f);
            });
        });
```

- [ ] **Step 2: Migrate DayNightSystem**

Change the loop header:

```cpp
        for (EntityId sun : ctx.world.View<SunMarker, LightningComponent>()) {
            ctx.world.Modify<LightningComponent>(sun, [&](auto& l) {
```

to:

```cpp
        ctx.world.Each<SunMarker, LightningComponent>([&](EntityId sun) {
            ctx.world.Modify<LightningComponent>(sun, [&](auto& l) {
```

and change that loop's closing `}` to `});` (the `for` body becomes the `Each` lambda body). **Preserve the `DO NOT REMOVE THIS COMMENTED OUT CODE` block inside the `Modify` lambda exactly as-is** — do not delete or alter it.

- [ ] **Step 3: Build game (and ecs) to verify**

Run:
```
cmake --build --preset msvc-win64-vs2026-community --target game
```
Expected: builds cleanly. (No `GAME_API_VERSION` bump — `Game.h`/`GameState` are untouched.)

- [ ] **Step 4: Commit**

```bash
git add src/game/src/game.cpp
git commit -m "refactor(game): migrate TextRotation/DayNight systems to ECS Each"
```

---

### Task 3: Migrate MeshRenderPass to `Each`

**Files:**
- Modify: `src/editor/src/rendering/passes/MeshRenderPass.cpp`

- [ ] **Step 1: Migrate the point-light gather (components form)**

Change:

```cpp
        for (EntityId entity : world->View<TransformComponent, LightningComponent>()) {
            const auto* transform = world->GetComponent<TransformComponent>(entity);
            const auto* lightning = world->GetComponent<LightningComponent>(entity);
            if (!transform || !lightning) continue;

            if (lightning->Type == LightningType::Directional)
            {
                lightningDirection = lightning->Direction;
                lightningColor = lightning->Color;
                // keep scanning for points in case; do not break
            }
            else if (lightning->Type == LightningType::Point)
            {
                if (pointLights && pointLightCount < m_MaxPointLights)
                {
                    PointLightCPU pl{};
                    pl.Position = glm::vec4(transform->Position, 1.0f);
                    pl.Color = lightning->Color;
                    pl.Intensity = lightning->Intensity;
                    pl.Range = lightning->Range;
                    pointLights[pointLightCount++] = pl;
                }
            }
        }
```

to:

```cpp
        world->Each<TransformComponent, LightningComponent>(
            [&](EntityId, const TransformComponent& transform, const LightningComponent& lightning)
        {
            if (lightning.Type == LightningType::Directional)
            {
                lightningDirection = lightning.Direction;
                lightningColor = lightning.Color;
                // keep scanning for points in case; do not break
            }
            else if (lightning.Type == LightningType::Point)
            {
                if (pointLights && pointLightCount < m_MaxPointLights)
                {
                    PointLightCPU pl{};
                    pl.Position = glm::vec4(transform.Position, 1.0f);
                    pl.Color = lightning.Color;
                    pl.Intensity = lightning.Intensity;
                    pl.Range = lightning.Range;
                    pointLights[pointLightCount++] = pl;
                }
            }
        });
```

- [ ] **Step 2: Migrate the batching collect (components form, sized to GetEntityCount)**

Change:

```cpp
        auto meshEnts = world->View<TransformComponent, MeshComponent>();
        auto* entries = frameAllocator->AllocateArray<BatchEntry>(meshEnts.size());
        uint32_t entryCount = 0;
        if (entries)
        {
            for (EntityId entity : meshEnts)
            {
                const auto* meshComp = world->GetComponent<MeshComponent>(entity);
                if (!meshComp || !meshComp->Visible)
                    continue;

                const auto* materialComp = world->GetComponent<MaterialComponent>(entity);
                uint32_t materialId = materialComp ? materialComp->MaterialId : MaterialSystem::MissingMaterial;

                entries[entryCount++] = BatchEntry{ meshComp->MeshId, materialId, entity };
            }
        }
        else if (meshEnts.size() > 0)
        {
            char warn[128];
            snprintf(warn, sizeof(warn),
                     "MeshRenderPass: frame arena exhausted, dropped %zu mesh entities (no meshes drawn)",
                     meshEnts.size());
            SM_WARN(warn);
        }
```

to:

```cpp
        auto* entries = frameAllocator->AllocateArray<BatchEntry>(world->GetEntityCount());
        uint32_t entryCount = 0;
        if (entries)
        {
            world->Each<TransformComponent, MeshComponent>(
                [&](EntityId e, const TransformComponent&, const MeshComponent& meshComp)
            {
                if (!meshComp.Visible)
                    return;

                const auto* materialComp = world->GetComponent<MaterialComponent>(e);
                uint32_t materialId = materialComp ? materialComp->MaterialId : MaterialSystem::MissingMaterial;

                entries[entryCount++] = BatchEntry{ meshComp.MeshId, materialId, e };
            });
        }
        else if (world->GetEntityCount() > 0)
        {
            char warn[128];
            snprintf(warn, sizeof(warn),
                     "MeshRenderPass: frame arena exhausted, dropped up to %zu entities (no meshes drawn)",
                     world->GetEntityCount());
            SM_WARN(warn);
        }
```

- [ ] **Step 3: Build the editor to verify**

Run:
```
cmake --build --preset msvc-win64-vs2026-community --target editor
```
Expected: builds + links cleanly.

- [ ] **Step 4: Commit**

```bash
git add src/editor/src/rendering/passes/MeshRenderPass.cpp
git commit -m "refactor(render): migrate MeshRenderPass lights+batching to ECS Each"
```

---

### Task 4: Migrate UiRenderPass text list to `Each` + arena buffer

**Files:**
- Modify: `src/editor/src/rendering/passes/UiRenderPass.cpp`

- [ ] **Step 1: Fill the text-entity list from an arena buffer via Each**

Change:

```cpp
        // 1) Gather unique font sizes used by text entities (arena-backed dedup)
        auto textEnts = world->View<TransformComponent, TextComponent>();
        auto* usedFontSizes = frameAllocator->AllocateArray<size_t>(textEnts.size());
        uint32_t fontSizeCount = 0;
        if (usedFontSizes) {
            for (EntityId entity : textEnts) {
                const auto* text = world->GetComponent<TextComponent>(entity);
                if (text) {
                    const size_t fontSize = text->FontSize;
                    bool found = false;
                    for (uint32_t k = 0; k < fontSizeCount; ++k) {
                        if (usedFontSizes[k] == fontSize) {
                            found = true;
                            break;
                        }
                    }
                    if (!found) {
                        usedFontSizes[fontSizeCount++] = fontSize;
                    }
                }
            }
        }
```

to:

```cpp
        // Collect text entities into an arena buffer (entity-only Each), reused below.
        auto* textEnts = frameAllocator->AllocateArray<EntityId>(world->GetEntityCount());
        uint32_t textCount = 0;
        if (textEnts) {
            world->Each<TransformComponent, TextComponent>([&](EntityId e){ textEnts[textCount++] = e; });
        }

        // 1) Gather unique font sizes used by text entities (arena-backed dedup)
        auto* usedFontSizes = frameAllocator->AllocateArray<size_t>(textCount);
        uint32_t fontSizeCount = 0;
        if (usedFontSizes) {
            for (uint32_t ti = 0; ti < textCount; ++ti) {
                const EntityId entity = textEnts[ti];
                const auto* text = world->GetComponent<TextComponent>(entity);
                if (text) {
                    const size_t fontSize = text->FontSize;
                    bool found = false;
                    for (uint32_t k = 0; k < fontSizeCount; ++k) {
                        if (usedFontSizes[k] == fontSize) {
                            found = true;
                            break;
                        }
                    }
                    if (!found) {
                        usedFontSizes[fontSizeCount++] = fontSize;
                    }
                }
            }
        }
```

- [ ] **Step 2: Update the two inner per-font loops to index the buffer**

Inside the `for (uint32_t fsIdx ...)` font loop there are two loops that iterate `world->View<TransformComponent, TextComponent>()` (the glyph-count loop and the instance-generation loop — earlier migrated to a cached `textEnts` vector). Change BOTH loop headers from:

```cpp
            for (EntityId entity : textEnts) {
```

to:

```cpp
            for (uint32_t ti = 0; ti < textCount; ++ti) {
                const EntityId entity = textEnts[ti];
```

For each, ensure the loop's closing brace still balances (you replaced one `for (...) {` line with a `for (...) {` + a declaration line — same single opening brace). The loop bodies (which use `entity` and `world->GetComponent<TextComponent>(entity)`) are otherwise unchanged.

- [ ] **Step 3: Build the editor to verify**

Run:
```
cmake --build --preset msvc-win64-vs2026-community --target editor
```
Expected: builds + links cleanly.

- [ ] **Step 4: Commit**

```bash
git add src/editor/src/rendering/passes/UiRenderPass.cpp
git commit -m "refactor(render): migrate UiRenderPass text list to ECS Each + arena buffer"
```

---

### Task 5: Remove `View` + update the example comment + verify

**Files:**
- Modify: `src/common/include/ECS.h`

- [ ] **Step 1: Confirm no `View<` callers remain**

Search `src/` for `View<`. Expected: matches only inside `src/common/include/ECS.h` — the `View` method definition itself and the example-usage comment block. NO matches in `game.cpp`, `MeshRenderPass.cpp`, `UiRenderPass.cpp`, or `test_ecs.cpp`. If any production caller still uses `View<`, STOP and migrate it before continuing.

- [ ] **Step 2: Delete the `View` method**

In `src/common/include/ECS.h`, delete the entire `View` method:

```cpp
    // Iterate entities with specific components (simple view)
    template<typename... Components>
    [[nodiscard]] std::vector<EntityId> View() const {
        std::vector<EntityId> result;
        for (EntityId entity : m_EntityStore.GetActiveEntities()) {
            if (HasComponents<Components...>(entity)) {
                result.push_back(entity);
            }
        }
        return result;
    }
```

(Leave `Each`, `HasComponents`, and `GetComponents` intact.)

- [ ] **Step 3: Update the example-usage comment block**

In the large example comment block lower in `ECS.h`, update the `View` examples to `Each`. Replace the block:

```cpp
// Iterate all entities with specific components (simple system, read-only)
for (EntityId entity : world.View<TransformComponent, MeshComponent>()) {
    const auto* transform = world.GetComponent<TransformComponent>(entity);
    const auto* mesh = world.GetComponent<MeshComponent>(entity);
    // Render mesh at transform position (read only)
}
```

with:

```cpp
// Iterate all entities with specific components (read-only)
world.Each<TransformComponent, MeshComponent>(
    [](EntityId entity, const TransformComponent& transform, const MeshComponent& mesh) {
        // Render mesh at transform position (read only)
    });
```

If any other `View<` example remains in the comment block, convert it to the equivalent `Each` form (or delete it). After this step, `View<` must appear **nowhere** in `src/`.

- [ ] **Step 4: Verify no `View<` references remain**

Search `src/` for `View<` again. Expected: zero matches.

- [ ] **Step 5: Rebuild all affected targets + run ECS tests**

Run:
```
cmake --build --preset msvc-win64-vs2026-community --target test_ecs
cmake --build --preset msvc-win64-vs2026-community --target game
cmake --build --preset msvc-win64-vs2026-community --target editor
./out/build/msvc-win64-vs2026-community/bin/Debug/test_ecs.exe
```
Expected: all build cleanly; `test_ecs.exe` prints `All ECS tests passed.` (`ecs.dll` itself is unchanged — `Each`/`View` are header templates not in `ecs.cpp`'s instantiations — but rebuilding it is harmless.)

- [ ] **Step 6: Commit**

```bash
git add src/common/include/ECS.h
git commit -m "refactor(ecs): remove View(); Each is the sole iteration primitive"
```

---

### Task 6: Full verification

**Files:** none (verification only).

- [ ] **Step 1: Build + unit tests**

Run:
```
cmake --build --preset msvc-win64-vs2026-community --target test_ecs
cmake --build --preset msvc-win64-vs2026-community --target editor
./out/build/msvc-win64-vs2026-community/bin/Debug/test_ecs.exe
./out/build/msvc-win64-vs2026-community/bin/Debug/test_alloc.exe
```
Expected: `All ECS tests passed.` and `All allocator tests passed.` (ignore the stray arena-overflow ERROR line in test_alloc — pre-existing lib.h quirk). (Do not build the whole solution — the legacy `runtime` target is pre-broken and unrelated.)

- [ ] **Step 2: Editor smoke test (user-driven)**

Launch `./out/build/msvc-win64-vs2026-community/bin/Debug/editor.exe`. Confirm the scene renders identically (meshes, instancing, text, day/night light all behave as before) and the text spins/cycles color (TextRotation/DayNight systems still run). No crash. Report that this requires the running UI rather than claiming success from a compile.

---

## Self-Review

**1. Spec coverage:**
- Add `Each` (flexible: components-by-const-ref via `if constexpr`, else entity-only; single fetch via tuple/apply): Task 1. ✓
- `#include <tuple>`: Task 1 Step 3. ✓
- Remove `View`: Task 5. ✓
- Update ECS.h example comment: Task 5 Step 3. ✓
- Migrate game.cpp:20 + :47 (entity-only): Task 2. ✓ (preserves the DO-NOT-REMOVE comment)
- Migrate MeshRenderPass:332 lights (components form): Task 3 Step 1. ✓
- Migrate MeshRenderPass:379 batching, `entries` sized to `GetEntityCount()`: Task 3 Step 2. ✓
- Migrate UiRenderPass:300 text → arena buffer via entity-only Each: Task 4. ✓
- Unit-test `Each` in test_ecs (both forms, zero/single/multi, no-mutate): Task 1. ✓
- Verify zero `View<` remain: Task 5 Steps 1+4. ✓
- No `GAME_API_VERSION` bump: called out in header + Task 2 Step 3. ✓

**2. Placeholder scan:** No TBD/TODO/"handle edge cases". Every code step shows full before/after code.

**3. Type consistency:** `Each<Components..., F>(F&&) const` signature defined in Task 1 and called identically in Tasks 2-4. Callback forms match the `if constexpr` contract: entity-only `[&](EntityId)` in Task 2 + UiRenderPass; components `[&](EntityId, const TransformComponent&, const LightningComponent&)` / `(..., const MeshComponent&)` in Task 3 (matching the queried `<Components...>` order). `BatchEntry{ meshId, materialId, entity }` aggregate matches Task-3 (render-pass) definition. `world->GetEntityCount()` returns `size_t` (used for arena sizing + the `%zu` warn). `textEnts`/`entries`/`pointLights` are arena `T*` from `AllocateArray<T>` (null on overflow/zero) — all null-guarded.

**Note for executor:** `MeshComponent` fields are `uint32_t MeshId; bool Visible;` (designated init `{.MeshId=42,.Visible=true}` in TE02 is valid C++23). The components-form callback param order MUST match the `Each<A,B>` type-list order (e.g. `Each<TransformComponent, MeshComponent>` → `(EntityId, const TransformComponent&, const MeshComponent&)`). In MeshRenderPass batching the `TransformComponent&` param is unnamed (unused — the per-instance loop reads transforms later); that's intentional, not a mistake.
