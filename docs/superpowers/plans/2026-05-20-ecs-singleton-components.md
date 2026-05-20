# ECS Singleton Components + Input/Camera Systems — Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.
>
> **MODEL NOTE:** Two tasks are high-risk — dispatch their implementer + reviewer with **Opus 4.7** (`model: opus`):
> - **Task 1** (singleton mechanism): touches the carefully-built COW `ComponentStore`, `CreateSnapshot`, and `Clear` semantics, plus a reserved hidden entity.
> - **Task 6** (snapshot + render-pass camera migration): moves the camera off the Seqlock onto the ECS snapshot across 4 render files + changes `ImGuiRenderer::Render`'s signature.
> Other tasks may use a standard model.

**Goal:** Add a singleton-component facility to the ECS (`SetSingleton`/`GetSingleton`/`ModifySingleton` over a hidden reserved entity) and migrate input, world+UI cameras, day/night config, and game-owned quit out of `GameState` into ECS singletons consumed by engine-side input drain + Simulation-phase systems; render reads the camera from the world snapshot.

**Architecture:** A singleton is a component on one reserved ECS entity (hidden from gameplay iteration, preserved across `Clear` + `CreateSnapshot`). The engine drains raw input into `InputStateComponent`; systems (`FreeLookCameraSystem`, `QuitRequestSystem`, etc.) read it and write `WorldCameraComponent`/`AppControlComponent`; the renderer reads `WorldCameraComponent`/`UICameraComponent` from the per-tick world snapshot.

**Tech Stack:** C++23, CMake preset `msvc-win64-vs2026-community`, MSVC/VS2026; existing `ecs.dll` (COW ECS), `game.dll` (SHARED, hot-reload), `editor.exe`, `test_ecs.exe`; GLM; the Systems layer from the prior feature.

**Spec:** `docs/superpowers/specs/2026-05-20-ecs-singleton-components-design.md` — read first.

> **Testing:** The singleton mechanism is unit-tested in `test_ecs.exe` (Task 3). Input/camera/quit systems need input events + GPU → manual smoke (Task 8).

---

## File Structure

**Modified:**
- `src/common/include/ECS.h` — `m_SingletonEntity` member + ctor; `SetSingleton`/`GetSingleton`/`ModifySingleton`; `EntityStore::CreateReserved`; the 7 singleton component structs + X-macro entries + `#include "Input.h"`.
- `src/ecs/src/ecs.cpp` — ECS ctor reserves the entity; `CreateSnapshot` preserves `m_SingletonEntity`; `Clear` preserves singletons (destroy gameplay entities only); explicit instantiations cover the new types.
- `tests/test_ecs.cpp` — singleton-mechanism unit tests.
- `src/editor/src/threading/GameThread.{h,cpp}` — engine input drain → `InputStateComponent`; seed `InputStateComponent`/`ViewportComponent`; resize → `ViewportComponent` + `UICameraComponent`; quit reads `AppControlComponent`; `PublishSnapshot` drops camera; pass world to ImGui.
- `src/game/src/game.cpp` — `FreeLookCameraSystem`, `QuitRequestSystem`, `DebugSpawnSystem`; `DayNightSystem` reads config; `GameRegisterSystems` list; `GameUpdate` seeds singletons + drops per-tick input/camera; remove `DrainInput`/`HandleCameraMovement`/`HandleFreeLook`.
- `src/game/include/game.h` — remove migrated `GameState` fields; bump `GAME_API_VERSION` 4→5.
- `src/common/include/ApplicationContext.h` — `SimulationSnapshot` drops `GameCamera`/`UICamera`.
- `src/editor/src/rendering/passes/MeshRenderPass.cpp`, `PrimitiveRenderPass.cpp`, `UiRenderPass.cpp` — read camera from `world` snapshot.
- `src/editor/src/rendering/imgui/ImGuiRenderer.{h,cpp}` + `src/editor/src/rendering/Renderer.cpp` — `ImGuiRenderer::Render` gains `const ECS* world`; gizmo reads camera singleton.

---

## Build / run

- ecs: `cmake --build --preset msvc-win64-vs2026-community --target ecs`
- tests: `cmake --build --preset msvc-win64-vs2026-community --target test_ecs && ./out/build/msvc-win64-vs2026-community/bin/Debug/test_ecs.exe`
- all in-scope: `cmake --build --preset msvc-win64-vs2026-community --target ecs editor game test_ecs`
- reconfigure (CMake change): `cmake --preset msvc-win64-vs2026-community`

`runtime.exe` is broken on `main` (pre-existing) — do not build it.

---

## Task 1: Singleton mechanism in ecs.dll **(Opus — high-risk)**

Reserved hidden entity + sugar + snapshot/Clear semantics. Define the singleton component **types** in Task 2; this task adds only the mechanism (templates compile without concrete registered types; the unit tests in Task 3 exercise it with the Task 2 types).

**Files:** `src/common/include/ECS.h`, `src/ecs/src/ecs.cpp`

- [ ] **Step 1: `EntityStore::CreateReserved` (ECS.h)**

In `class EntityStore` (ECS.h), add a public method that allocates an id WITHOUT adding it to the active list (so it's invisible to `GetActiveEntities`):
```cpp
    // Allocates an id not tracked in m_ActiveEntities (used for the ECS singleton
    // entity). Not returned by GetActiveEntities()/GetEntityCount(), not recycled.
    EntityId CreateReserved() {
        return m_NextEntityId++;
    }

    // Removes all ACTIVE (gameplay) entities; leaves m_NextEntityId untouched so a
    // previously-reserved id stays valid and is never re-handed-out.
    void ClearActive() {
        m_ActiveEntities.clear();
        m_FreeEntities.clear();
        // NOTE: do NOT reset m_NextEntityId — keeps reserved ids stable + monotonic.
    }
```
Keep the existing `Clear()` as-is (still used elsewhere if any; `ECS::Clear` will switch to the gameplay-only path below).

- [ ] **Step 2: ECS gains the reserved singleton entity + sugar (ECS.h)**

In `class ECS`, add a private member and a constructor that reserves the singleton entity FIRST (so it's id 1, gameplay ids start at 2):
```cpp
public:
    ECS() { m_SingletonEntity = m_EntityStore.CreateReserved(); }
```
Add the typed sugar in the public section (header-inline; they wrap the already-exported templated methods):
```cpp
    // ----- Singleton components (one reserved hidden entity) -----
    template<typename T> void SetSingleton(T value) { AddComponent<T>(m_SingletonEntity, std::move(value)); }
    template<typename T> [[nodiscard]] const T* GetSingleton() const { return GetComponent<T>(m_SingletonEntity); }
    template<typename T, typename F> void ModifySingleton(F&& fn) { Modify<T>(m_SingletonEntity, std::forward<F>(fn)); }
    [[nodiscard]] EntityId SingletonEntity() const { return m_SingletonEntity; }
```
Add the private member (near `m_EntityStore`/`m_ComponentStore`):
```cpp
    EntityId m_SingletonEntity = INVALID_ENTITY;
```

- [ ] **Step 3: `CreateSnapshot` preserves the singleton entity (ecs.cpp)**

`CreateSnapshot` does `make_shared<ECS>()` (whose ctor reserves a NEW singleton id), then overwrites `m_EntityStore` by value copy. Add a line to copy `m_SingletonEntity` so the snapshot's `GetSingleton` resolves the same id:
```cpp
std::shared_ptr<const ECS> ECS::CreateSnapshot() {
    auto snap = std::make_shared<ECS>();
    snap->m_EntityStore = m_EntityStore;                     // value copy
    snap->m_SingletonEntity = m_SingletonEntity;             // preserve reserved id
    snap->m_ComponentStore.CopyArraysFrom(m_ComponentStore); // shallow shared_ptr copy
    m_ComponentStore.ClearDirty();
    return snap;
}
```

- [ ] **Step 4: `Clear` preserves singletons (ecs.cpp)**

Replace the current `ECS::Clear` (which wipes everything) with a gameplay-only clear that keeps the reserved entity + its singleton components:
```cpp
void ECS::Clear() {
    // Destroy only gameplay (active) entities + their components. The reserved
    // singleton entity is not in the active list, so its singleton components
    // (input, cameras, app-control, config) survive world loads + reloads.
    const std::vector<EntityId> active = m_EntityStore.GetActiveEntities(); // copy (DestroyEntity mutates)
    for (const EntityId e : active) {
        m_ComponentStore.RemoveAllComponents(e);
    }
    m_EntityStore.ClearActive();
}
```
Do NOT call `m_ComponentStore.Cleanup()` here (that would wipe the singleton arrays too). `RemoveAllComponents` only touches arrays holding the given gameplay entity; singleton component arrays (held solely by the reserved entity) are untouched.

> Note for the implementer: confirm `ComponentStore::RemoveAllComponents` removes the entity from every array it appears in (it does — it iterates arrays, COW-clones on first write, and `Remove`s the entity). Singleton component types are only ever on `m_SingletonEntity`, so no gameplay entity appears in them and they're left intact.

- [ ] **Step 5: Build ecs**

```
cmake --build --preset msvc-win64-vs2026-community --target ecs
```
Expected: clean. (No concrete singleton types yet; the templates compile. `INVALID_ENTITY` is defined in ECS.h.)

- [ ] **Step 6: Commit**

```bash
git add src/common/include/ECS.h src/ecs/src/ecs.cpp
git commit -m "ECS: reserved singleton entity + Set/Get/ModifySingleton, snapshot+Clear preserve it"
```

---

## Task 2: Singleton component types

**Files:** `src/common/include/ECS.h`, `src/ecs/src/ecs.cpp` (instantiations are macro-driven — no manual edit if the X-macro covers them)

- [ ] **Step 1: Include Input.h for KEY_LAST**

Near the top includes of `ECS.h`, add (if not already pulled in): `#include "Input.h"`.

- [ ] **Step 2: Define the 7 singleton component structs**

In `ECS.h`, alongside the existing component catalog (e.g. after `SunMarker`), add:
```cpp
struct InputStateComponent {
    bool    KeysDown[KEY_LAST + 1] = {};
    bool    Pressed[KEY_LAST + 1]  = {};   // pressed this tick (cleared each drain)
    double  MouseX = 0.0, MouseY = 0.0;
    double  MouseDX = 0.0, MouseDY = 0.0;
    int32_t Wheel = 0;
};
struct WorldCameraComponent {
    glm::mat4 View{1.0f};
    glm::mat4 Projection{1.0f};
    glm::vec3 Position{0.0f};
};
struct UICameraComponent {
    glm::mat4 View{1.0f};
    glm::mat4 Projection{1.0f};
};
struct FreeLookControlComponent {
    glm::vec3 Position{0.0f, 5.0f, 10.0f};
    float Yaw = 0.0f;       // rotation.y
    float Pitch = 0.0f;     // rotation.x
    float Fov = glm::radians(80.0f);
    float MoveSpeed = 7.5f;
    float Sensitivity = 0.002f;
    bool  MouseAimEnabled = false;
};
struct DayNightConfigComponent { float CycleSeconds = 10.0f; };
struct AppControlComponent     { bool  QuitRequested = false; };
struct ViewportComponent       { uint32_t Width = 1920; uint32_t Height = 1080; };
```

- [ ] **Step 3: Register all 7 in the X-macro**

Append to `ECS_FOR_EACH_REGISTERED_COMPONENT`:
```cpp
    X(InputStateComponent) \
    X(WorldCameraComponent) \
    X(UICameraComponent) \
    X(FreeLookControlComponent) \
    X(DayNightConfigComponent) \
    X(AppControlComponent) \
    X(ViewportComponent)
```
(Mind the line-continuation backslashes; the last X-line has no trailing backslash.)

- [ ] **Step 4: Build ecs + all dependents**

```
cmake --build --preset msvc-win64-vs2026-community --target ecs editor game test_ecs
```
Expected: clean. The macro-driven explicit instantiations in `ecs.cpp` now cover the new types; `editor`/`game` see them via `ECS.h`. **Do NOT** add `ECSCommands`/`WorldManager` handling — singletons are not inspector-edited or serialized.

- [ ] **Step 5: Commit**

```bash
git add src/common/include/ECS.h
git commit -m "ECS: add 7 singleton component types + register them"
```

---

## Task 3: Singleton-mechanism unit tests

**Files:** `tests/test_ecs.cpp`

- [ ] **Step 1: Add tests above `int main()`** (uses the existing `EXPECT`/`EXPECT_EQ`/`EXPECT_NE` macros; `ECS.h` already included):

```cpp
// --- Singleton component tests ---

static void TSG01_set_get_roundtrip()
{
    ECS world;
    EXPECT_EQ(world.GetSingleton<DayNightConfigComponent>(), nullptr);   // unset → null
    world.SetSingleton(DayNightConfigComponent{ 42.0f });
    const auto* c = world.GetSingleton<DayNightConfigComponent>();
    EXPECT_NE(c, nullptr);
    if (c) EXPECT_EQ(c->CycleSeconds, 42.0f);
}

static void TSG02_modify_singleton()
{
    ECS world;
    world.SetSingleton(AppControlComponent{ false });
    world.ModifySingleton<AppControlComponent>([](AppControlComponent& a){ a.QuitRequested = true; });
    const auto* a = world.GetSingleton<AppControlComponent>();
    EXPECT_NE(a, nullptr);
    if (a) EXPECT(a->QuitRequested);
}

static void TSG03_singleton_entity_hidden()
{
    ECS world;
    world.SetSingleton(ViewportComponent{ 800, 600 });
    EXPECT_EQ(world.GetEntityCount(), 0u);                 // reserved entity not counted
    EXPECT_EQ(world.GetActiveEntities().size(), 0u);       // not iterated by gameplay
    const EntityId e = world.CreateEntity();               // first gameplay id
    EXPECT_NE(e, world.SingletonEntity());                 // distinct from reserved
    EXPECT_EQ(world.GetEntityCount(), 1u);                 // only the gameplay entity
}

static void TSG04_snapshot_preserves_singletons()
{
    ECS world;
    world.SetSingleton(DayNightConfigComponent{ 7.0f });
    std::shared_ptr<const ECS> snap = world.CreateSnapshot();
    const auto* c = snap->GetSingleton<DayNightConfigComponent>();
    EXPECT_NE(c, nullptr);
    if (c) EXPECT_EQ(c->CycleSeconds, 7.0f);
}

static void TSG05_clear_preserves_singletons_removes_gameplay()
{
    ECS world;
    world.SetSingleton(DayNightConfigComponent{ 3.0f });
    const EntityId e = world.CreateEntity();
    world.AddComponent(e, TransformComponent{{1,2,3},{},{1,1,1}});
    EXPECT_EQ(world.GetEntityCount(), 1u);

    world.Clear();

    EXPECT_EQ(world.GetEntityCount(), 0u);                       // gameplay gone
    const auto* c = world.GetSingleton<DayNightConfigComponent>();
    EXPECT_NE(c, nullptr);                                       // singleton survives
    if (c) EXPECT_EQ(c->CycleSeconds, 3.0f);
}
```

- [ ] **Step 2: Register in `main()`** after the last existing test call:
```cpp
    TSG01_set_get_roundtrip();
    TSG02_modify_singleton();
    TSG03_singleton_entity_hidden();
    TSG04_snapshot_preserves_singletons();
    TSG05_clear_preserves_singletons_removes_gameplay();
```

- [ ] **Step 3: Build + run**

```
cmake --build --preset msvc-win64-vs2026-community --target test_ecs
./out/build/msvc-win64-vs2026-community/bin/Debug/test_ecs.exe
```
Expected: `All ECS tests passed.`

- [ ] **Step 4: Commit**

```bash
git add tests/test_ecs.cpp
git commit -m "Add singleton-mechanism unit tests (set/get/modify, hidden, snapshot, clear)"
```

---

## Task 4: Engine-side input drain

**Files:** `src/editor/src/threading/GameThread.{h,cpp}`

- [ ] **Step 1: Seed input + viewport singletons at startup**

In `GameThread::RunLoop`, after the world is loaded and BEFORE the main `while` loop (a good spot is right after the default-world load block, before the initial `m_GameLib.LoadOrReload`), seed the engine-owned singletons:
```cpp
    gameState.World.SetSingleton(InputStateComponent{});
    gameState.World.SetSingleton(ViewportComponent{
        m_AppContext->Settings.windowWidth, m_AppContext->Settings.windowHeight });
```

- [ ] **Step 2: Add an engine drain helper + call it each tick before the scheduler**

Add a private method to `GameThread` (`GameThread.h`):
```cpp
    void DrainInputToSingleton(GameState& state);
```
Implement in `GameThread.cpp` (translates raw events; no keybind meaning, no mouse-aim gating):
```cpp
void GameThread::DrainInputToSingleton(GameState& state) {
    // Ensure the component exists (Modify no-ops if absent; Clear keeps it, but be safe).
    if (!state.World.GetSingleton<InputStateComponent>()) {
        state.World.SetSingleton(InputStateComponent{});
    }
    double prevX = 0.0, prevY = 0.0;
    if (const auto* in = state.World.GetSingleton<InputStateComponent>()) { prevX = in->MouseX; prevY = in->MouseY; }

    state.World.ModifySingleton<InputStateComponent>([&](InputStateComponent& s) {
        std::memset(s.Pressed, 0, sizeof(s.Pressed));   // per-tick
        s.Wheel = 0;
        InputEvent ev{};
        while (m_AppContext->InputRing.Pop(ev)) {
            if (ev.Type == Input::InputEventType::Key) {
                const int k = static_cast<int>(ev.KeyEvent.Key);
                if (k >= 0 && k <= KEY_LAST) {
                    if (ev.KeyEvent.Action == Input::PRESS || ev.KeyEvent.Action == Input::REPEAT) s.KeysDown[k] = true;
                    if (ev.KeyEvent.Action == Input::RELEASE) s.KeysDown[k] = false;
                    if (ev.KeyEvent.Action == Input::PRESS) s.Pressed[k] = true;
                }
            } else if (ev.Type == Input::InputEventType::MouseMove) {
                s.MouseX = ev.MouseMoveEvent.X;
                s.MouseY = ev.MouseMoveEvent.Y;
            } else if (ev.Type == Input::InputEventType::MouseWheel) {
                s.Wheel = static_cast<int32_t>(ev.MouseScrollEvent.OffsetY);
            }
        }
        s.MouseDX = s.MouseX - prevX;
        s.MouseDY = s.MouseY - prevY;
    });
}
```
(Confirm the exact `InputEvent`/`InputEventType`/action enum + field names against `Input.h` / the old `DrainInput` in `game.cpp`, and match them — the old code is the reference. Use the same ring `m_AppContext->InputRing` the platform thread fills. `<cstring>` for `memset`.)

Call it in the tick, immediately BEFORE `m_GameLib.Update(&gameState)`:
```cpp
            DrainInputToSingleton(gameState);
            if (m_GameLib.IsValid()) { m_GameLib.Update(&gameState); }
            { SystemContext sysCtx{...}; m_Scheduler.Run(sysCtx); }   // existing
```

- [ ] **Step 3: Build editor**

```
cmake --build --preset msvc-win64-vs2026-community --target editor
```
Expected: clean. (The old `game.cpp` `DrainInput` still exists at this point but is now redundant; it is removed in Task 5. Both writing to different stores is harmless temporarily.)

- [ ] **Step 4: Commit**

```bash
git add src/editor/src/threading/GameThread.h src/editor/src/threading/GameThread.cpp
git commit -m "GameThread: drain raw input into InputStateComponent singleton each tick"
```

---

## Task 5: Game systems (camera, quit, F12) + GameUpdate cleanup

**Files:** `src/game/src/game.cpp`

Read the current `game.cpp` first — `GameUpdate` (state machine, spawn), `HandleCameraMovement`, `HandleFreeLook`, `DrainInput`, the existing `DayNightSystem`/`TextRotationSystem`, and `GameRegisterSystems`.

- [ ] **Step 1: Add the new systems** (anonymous namespace, near the existing systems). The camera math mirrors `Camera.h::get_view_matrix` (`(Rz*Ry*Rx)*translate(-pos)` with negated angles) and `get_projection_matrix` (`perspectiveRH_ZO`), and the movement mirrors the old `HandleCameraMovement`/`HandleFreeLook`:

```cpp
class FreeLookCameraSystem final : public ISystem {
public:
    void Update(SystemContext& ctx) override {
        const auto* in = ctx.world.GetSingleton<InputStateComponent>();
        const auto* vp = ctx.world.GetSingleton<ViewportComponent>();
        if (!in || !ctx.world.GetSingleton<FreeLookControlComponent>()) return;

        const float dt = static_cast<float>(ctx.dt);
        ctx.world.ModifySingleton<FreeLookControlComponent>([&](FreeLookControlComponent& c) {
            using namespace Input;
            // Toggle mouse-aim on T press
            if (in->Pressed[KEY_T]) c.MouseAimEnabled = !c.MouseAimEnabled;

            // Look (only when mouse-aim on)
            if (c.MouseAimEnabled) {
                c.Yaw   -= static_cast<float>(in->MouseDX) * c.Sensitivity;
                c.Pitch -= static_cast<float>(in->MouseDY) * c.Sensitivity;
                const float lim = glm::radians(89.0f);
                c.Pitch = glm::clamp(c.Pitch, -lim, lim);
            }

            // Movement basis from yaw/pitch (matches Camera.h convention)
            const float cp = cosf(c.Pitch), sp = sinf(c.Pitch);
            const float cy = cosf(c.Yaw),   sy = sinf(c.Yaw);
            const glm::vec3 forward = glm::normalize(glm::vec3(-sy, sp*cy, -cp*cy));
            const glm::vec3 right   = glm::normalize(glm::cross(forward, glm::vec3(0,1,0)));
            const float spd = c.MoveSpeed * dt;
            if (in->KeysDown[KEY_W]) c.Position += forward * spd;
            if (in->KeysDown[KEY_S]) c.Position -= forward * spd;
            if (in->KeysDown[KEY_A]) c.Position -= right   * spd;
            if (in->KeysDown[KEY_D]) c.Position += right   * spd;
            if (in->KeysDown[KEY_SPACE])      c.Position.y += spd;
            if (in->KeysDown[KEY_LEFT_SHIFT]) c.Position.y -= spd;
            const float yawSpeed = glm::radians(120.0f) * dt;
            if (in->KeysDown[KEY_Q]) c.Yaw += yawSpeed;
            if (in->KeysDown[KEY_E]) c.Yaw -= yawSpeed;

            // Zoom (FOV) via wheel
            if (in->Wheel != 0) {
                const float step = glm::radians(2.0f);
                c.Fov = glm::clamp(c.Fov - step * static_cast<float>(in->Wheel),
                                   glm::radians(20.0f), glm::radians(179.99f));
            }
        });

        const auto* c = ctx.world.GetSingleton<FreeLookControlComponent>();
        const float aspect = (vp && vp->Height) ? float(vp->Width) / float(vp->Height) : 16.0f/9.0f;
        const glm::mat4 T  = glm::translate(glm::mat4(1.0f), -c->Position);
        const glm::mat4 Rx = glm::rotate(glm::mat4(1.0f), -c->Pitch, {1,0,0});
        const glm::mat4 Ry = glm::rotate(glm::mat4(1.0f), -c->Yaw,   {0,1,0});
        ctx.world.ModifySingleton<WorldCameraComponent>([&](WorldCameraComponent& w) {
            w.View       = (Ry * Rx) * T;                                  // roll = 0
            w.Projection = glm::perspectiveRH_ZO(c->Fov, aspect, 0.1f, 1000.0f);
            w.Position   = c->Position;
        });
    }
    const char* Name() const override { return "FreeLookCameraSystem"; }
    SystemPhase Phase() const override { return SystemPhase::Simulation; }
};

class QuitRequestSystem final : public ISystem {
public:
    explicit QuitRequestSystem(int quitKey) : m_QuitKey(quitKey) {}
    void Update(SystemContext& ctx) override {
        const auto* in = ctx.world.GetSingleton<InputStateComponent>();
        if (in && m_QuitKey >= 0 && m_QuitKey <= KEY_LAST && in->Pressed[m_QuitKey]) {
            ctx.world.ModifySingleton<AppControlComponent>([](AppControlComponent& a){ a.QuitRequested = true; });
        }
    }
    const char* Name() const override { return "QuitRequestSystem"; }
    SystemPhase Phase() const override { return SystemPhase::PostSimulation; }
private:
    int m_QuitKey;
};

class DebugSpawnSystem final : public ISystem {
public:
    void Update(SystemContext& ctx) override {
        const auto* in = ctx.world.GetSingleton<InputStateComponent>();
        if (in && in->Pressed[Input::KEY_F12]) {
            const auto id = ctx.world.CreateEntity();
            SM_TRACE("DebugSpawnSystem: spawned entity %llu", (unsigned long long)id);
        }
    }
    const char* Name() const override { return "DebugSpawnSystem"; }
    SystemPhase Phase() const override { return SystemPhase::Simulation; }
};
```
(Verify the `Input::` key constants + `using namespace Input;` against `Input.h` — the old `game.cpp` uses these exact names. Adjust `KEY_*` references to match.)

- [ ] **Step 2: `DayNightSystem` reads config**

Change `DayNightSystem::Update` to read the singleton instead of the hardcoded constant:
```cpp
        float kDayNightCycleSeconds = 10.0f;
        if (const auto* cfg = ctx.world.GetSingleton<DayNightConfigComponent>()) kDayNightCycleSeconds = cfg->CycleSeconds;
```
(Keep the rest of the body, including the `DO NOT REMOVE` reference comment, unchanged.)

- [ ] **Step 3: Register the systems**

In `GameRegisterSystems`:
```cpp
void GameRegisterSystems(SystemScheduler* s) {
    if (!s) return;
    s->Register(std::make_unique<FreeLookCameraSystem>());
    s->Register(std::make_unique<TextRotationSystem>());
    s->Register(std::make_unique<DayNightSystem>());
    s->Register(std::make_unique<DebugSpawnSystem>());
    s->Register(std::make_unique<QuitRequestSystem>(Input::KEY_ESCAPE));
}
```

- [ ] **Step 4: Seed game singletons in `GameUpdate` Uninitialized; remove per-tick input/camera**

In `GameUpdate`'s `Uninitialized` case, after setting `StateId = MainMenu`, seed the game-owned singletons (replaces the old `g_GameState->GameCamera.position = (0,5,10)` etc.):
```cpp
        g_GameState->World.SetSingleton(FreeLookControlComponent{});            // defaults incl. pos (0,5,10)
        g_GameState->World.SetSingleton(WorldCameraComponent{});
        g_GameState->World.SetSingleton(AppControlComponent{});
        g_GameState->World.SetSingleton(DayNightConfigComponent{});
```
Then DELETE from `game.cpp`:
- the call(s) to `DrainInput`, `HandleCameraMovement` (and `HandleFreeLook`) in `GameUpdate`;
- the `DrainInput`, `HandleCameraMovement`, `HandleFreeLook` function definitions + their forward declarations;
- the `IsKeyPressedThisFrame`/`IsKeyDown` helpers + `gKeysPressedThisFrame`/`gMouseWheel` file-statics (now obsolete);
- the F12-spawn block in `GameUpdate` (now `DebugSpawnSystem`);
- any `g_GameState->QuitRequested`/`KeysDown`/`MouseAimEnabled`/`MouseX`/`GameCamera` references.

The `MainMenu` case stays `break;` (text/day-night are systems). The `Uninitialized` default-scene spawn (entities + SunMarker) stays.

- [ ] **Step 5: Build game**

```
cmake --build --preset msvc-win64-vs2026-community --target game
```
Expected: clean (it still references `GameState` fields that exist until Task 7; just don't reference the ones you deleted usages of). If the build complains about now-unused `GameState` fields, that's fine — they're removed in Task 7.

- [ ] **Step 6: Commit**

```bash
git add src/game/src/game.cpp
git commit -m "game: camera/quit/spawn systems; day-night reads config; drop per-tick input/camera"
```

---

## Task 6: Snapshot + render camera migration **(Opus — high-risk)**

**Files:** `src/common/include/ApplicationContext.h`, `src/editor/src/threading/GameThread.cpp`, `src/editor/src/rendering/passes/MeshRenderPass.cpp`, `PrimitiveRenderPass.cpp`, `UiRenderPass.cpp`, `src/editor/src/rendering/imgui/ImGuiRenderer.{h,cpp}`, `src/editor/src/rendering/Renderer.cpp`

- [ ] **Step 1: Drop camera from `SimulationSnapshot`**

In `ApplicationContext.h`, remove `PerspectiveCamera3D GameCamera;` and `OrthographicCamera2D UICamera;` from `struct SimulationSnapshot` (keep `Tick`/`Timestamp`/TPS/`ObjectX`/`ObjectVX`/`FrameStats`).

- [ ] **Step 2: `PublishSnapshot` + init snapshot stop copying the camera**

In `GameThread.cpp` `PublishSnapshot`, delete `snap.GameCamera = state.GameCamera;` and `snap.UICamera = state.UICamera;`. In the `GameThread` ctor's initial `SimulationSnapshot init{}` publish, remove any camera field sets (the fields no longer exist).

- [ ] **Step 3: Engine maintains `UICameraComponent` + `ViewportComponent` on resize; reads quit from `AppControlComponent`**

Concrete, deterministic approach — maintain both singletons each tick from `m_AppContext->Settings` (the authoritative window size; if the existing resize path updates `Settings`, the UI camera follows automatically; if not, it stays at the startup size, which is acceptable for v1). Add this in the tick (e.g. just before `PublishSnapshot`), and seed `UICameraComponent` at startup (Task 4 Step 1 area: also `gameState.World.SetSingleton(UICameraComponent{});`):
```cpp
    // Keep ViewportComponent + UI camera in sync with the window each tick.
    const uint32_t vw = m_AppContext->Settings.windowWidth;
    const uint32_t vh = m_AppContext->Settings.windowHeight;
    gameState.World.ModifySingleton<ViewportComponent>([&](ViewportComponent& v){ v.Width = vw; v.Height = vh; });
    gameState.World.ModifySingleton<UICameraComponent>([&](UICameraComponent& ui){
        ui.Projection = glm::orthoRH_ZO(0.0f, float(vw), float(vh), 0.0f, -1.0f, 1.0f);
        ui.View = glm::mat4(1.0f);
    });
```
(`ModifySingleton` no-ops if the component is absent, so the startup `SetSingleton` seeds are required first. This per-tick recompute is cheap — two 1-element COW clones — and removes any dependency on a Platform→Game resize signal. If a later feature wants event-driven resize, it can replace this; for now it's correct and self-contained.)

Replace the post-tick quit check. Where the tick currently reads `gameState.QuitRequested`, read the singleton instead:
```cpp
    if (const auto* app = gameState.World.GetSingleton<AppControlComponent>(); app && app->QuitRequested) {
        m_Running.store(false, std::memory_order_relaxed);
        m_AppContext->ShutdownRequested.store(true, std::memory_order_relaxed);
        break;
    }
```

- [ ] **Step 4: Render passes read the camera from the world snapshot**

`MeshRenderPass.cpp` (≈356-357) and `PrimitiveRenderPass.cpp` (≈263-264, 301) — replace `snapshot.GameCamera.get_view_matrix()/get_projection_matrix()/position` with reads from the `world` snapshot the pass already receives:
```cpp
    glm::mat4 V(1.0f), P(1.0f); glm::vec3 camPos(0.0f);
    if (const auto* cam = world ? world->GetSingleton<WorldCameraComponent>() : nullptr) {
        V = cam->View; P = cam->Projection; camPos = cam->Position;
    }
```
Use `V`/`P`/`camPos` in place of the old expressions (e.g. `PrimitiveRenderPass` `pf.CameraPos = glm::vec4(camPos, 0.0f);`). Null-check guards the pre-seed frame.

`UiRenderPass.cpp` (≈279-282) — build its ortho from `world->GetSingleton<UICameraComponent>()->Projection` instead of however it currently derives the screen matrix (read the current code there; replace the manual ortho with the singleton's `Projection`, falling back to identity if null).

- [ ] **Step 5: ImGuiRenderer gizmo reads the camera singleton**

`ImGuiRenderer::Render` currently takes `(framebuffer, deltaTime, SimulationSnapshot& snapshot, gpuMs)` and uses `snapshot.GameCamera` for the gizmo. Add a `const ECS* world` parameter:
- `ImGuiRenderer.h`: change the `Render` declaration to include `const ECS* world` (forward-declare `class ECS;` if needed).
- `ImGuiRenderer.cpp`: read `world->GetSingleton<WorldCameraComponent>()` for the gizmo's view/projection (replace the `snapshot.GameCamera.get_*` calls); null-guard.
- `Renderer.cpp`: the call site `m_ImGuiRenderer->Render(frameBuffer, deltaTime, snapshot, secs)` → pass the world snapshot the `Renderer::Render` already has (`world`): `m_ImGuiRenderer->Render(frameBuffer, deltaTime, snapshot, secs, world)` (match the new signature param order).

- [ ] **Step 6: Build all in-scope**

```
cmake --build --preset msvc-win64-vs2026-community --target ecs editor game test_ecs
```
Expected: clean. Grep to confirm no `snapshot.GameCamera` / `snapshot.UICamera` remain: `grep -rn "snapshot.GameCamera\|snapshot.UICamera" src/editor` → empty.

- [ ] **Step 7: Commit**

```bash
git add src/common/include/ApplicationContext.h src/editor/src/threading/GameThread.cpp src/editor/src/rendering/passes/MeshRenderPass.cpp src/editor/src/rendering/passes/PrimitiveRenderPass.cpp src/editor/src/rendering/passes/UiRenderPass.cpp src/editor/src/rendering/imgui/ImGuiRenderer.h src/editor/src/rendering/imgui/ImGuiRenderer.cpp src/editor/src/rendering/Renderer.cpp
git commit -m "Render reads camera from ECS world snapshot; drop camera from SimulationSnapshot; engine quit via AppControlComponent"
```

---

## Task 7: GameState cleanup + version bump

**Files:** `src/game/include/game.h`

- [ ] **Step 1: Remove migrated fields + bump version**

In `struct GameState`, delete: `KeysDown[]`, `MouseAimEnabled`, `MouseX`, `MouseY`, `GameCamera`, `UICamera`, `DayNightCycleSeconds`, `QuitRequested`, and `PlatformInput` (the ring ptr — engine drains `ApplicationContext::InputRing` directly now). Keep `StateId`, `GameTime`, `DeltaTime`, `TargetTPS`, `ActualTPS`, `Settings`, `GameOutputHandle`, `World`, `WorldLoaded`.

Bump:
```cpp
#define GAME_API_VERSION 5u
```
(`#include "Camera.h"` in game.h can be dropped if nothing else there uses it — check.)

- [ ] **Step 2: Build all in-scope**

```
cmake --build --preset msvc-win64-vs2026-community --target ecs editor game test_ecs
```
Expected: clean. Any remaining references to the removed fields are compile errors — fix them (they should all have been migrated in Tasks 4-6; if the engine still set `gameState.GameCamera` anywhere, remove it). Grep: `grep -rn "\.GameCamera\|->GameCamera\|QuitRequested\|KeysDown\|MouseAimEnabled\|DayNightCycleSeconds\|PlatformInput" src/editor src/game` → only legitimate singleton/usages remain (no `GameState.` field hits).

- [ ] **Step 3: Commit**

```bash
git add src/game/include/game.h
git commit -m "GameState: drop migrated fields (input/camera/quit/config); bump API to 5"
```

---

## Task 8: Verify — tests + manual smoke

**Files:** none.

- [ ] **Step 1: Build all + ECS tests**

```
cmake --build --preset msvc-win64-vs2026-community --target ecs editor game test_ecs
./out/build/msvc-win64-vs2026-community/bin/Debug/test_ecs.exe
```
Expected: clean; `All ECS tests passed.` (incl. the 5 new `TSG0x`).

- [ ] **Step 2: Manual smoke (user-driven; GUI)**

Launch `editor.exe`. Confirm:
1. Camera moves: WASD/Q/E/Space/Shift; `T` toggles mouse-aim then mouse looks; wheel zooms (FreeLookCameraSystem).
2. `ESC` quits the editor (QuitRequestSystem → AppControlComponent → GameThread observes).
3. Text still spins + day/night cycle still runs.
4. `F12` spawns an entity (log line).
5. Resize the window → 3D + UI stay correct (ViewportComponent/UICameraComponent + aspect).

- [ ] **Step 3: Manual smoke — hot-reload + world load**

Edit a system constant (e.g. `MoveSpeed` default), rebuild `game` → hot-reload re-registers; camera/input keep working; the camera does NOT jump (FreeLookControlComponent survived in the ECS). Load/save a world (if exposed) → input/camera keep working (Clear preserved singletons). Clean exit (no crash).

- [ ] **Step 4: Commit any fixes** (only if smoke surfaces them).

---

## Task 9: Wrap up

- [ ] **Step 1:** `git status` + `git log --oneline origin/main..HEAD`; confirm authors are personal email (`git log --format=%ae origin/main..HEAD | grep -c vinci-energies` → 0).
- [ ] **Step 2:** Push the branch + open a PR (or push `main` per the recent flow). Summarize: singleton mechanism + 7 components, engine input drain, camera resolved-component + FreeLookCameraSystem (swappable), UI camera, day/night config, game-owned quit, render reads camera from the world snapshot, GameState slimmed + API→5, 5 new unit tests. Hand the URL/SHA back.

---

## Out-of-scope reminders

Iso/top-down camera itself; full key→action rebinding layer; moving Settings/timing into singletons; turning GameUpdate's state machine/spawn into systems; inspecting singletons in ImGui; `View<>()` allocation optimization; parallel systems.
