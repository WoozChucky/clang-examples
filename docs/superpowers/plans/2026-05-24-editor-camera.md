# Editor Camera (Free-Look Viewport Camera) Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Give the editor its own free-look viewport camera, independent of the game camera, with an Edit/Play toggle that decides which camera drives the viewport (and picking/outline).

**Architecture:** The editor camera is editor-owned (RenderThread, in the ImGui overlay). It publishes its matrices through a new `ApplicationContext` channel (`atomic<bool> EditorCameraActive` + `Seqlock<CameraView>`). `Renderer` resolves the active camera once per frame — editor override when active, else the snapshot's `WorldCameraComponent` — and the three world-space passes read that resolved camera (no interface change). Picking mirrors the same choice. The fly math lives in a pure, unit-tested `EditorCamera` class.

**Tech Stack:** C++23, NVRHI render passes, custom ECS, Dear ImGui, GLM (depth `[0,1]`, right-handed), CMake presets.

**Spec:** `docs/superpowers/specs/2026-05-24-editor-camera-design.md`

**Conventions (every task):**
- Build preset `msvc-win64-vs2026-community`; build dir `out/build/msvc-win64-vs2026-community`; exes in `bin/Debug/`.
- **No `GAME_API_VERSION` bump** (no `GameState`/export/ECS-component change; the editor camera is editor state, not an ECS component). `ApplicationContext.h` is shared, so changes there rebuild engine/editor/runtime/game.
- Commit author is the repo default (`Nuno Silva <nuno.levezinho@live.com.pt>`) — plain `git commit`, no `-c`/`--author`. Never stage `.claude/`. Stage only the files each step names. After each commit, `git log -1 --format='%an <%ae>'` must show the personal email.
- Refinement vs spec: `CameraView` goes in its own tiny header `src/common/include/CameraView.h` (the spec said "in `ApplicationContext.h`"). This keeps `EditorCamera` + its unit test free of the heavy `ApplicationContext.h` include graph. `ApplicationContext.h` includes the new header.
- GLM is right-handed, depth `[0,1]` project-wide; use `glm::lookAtRH` + `glm::perspectiveRH_ZO` (matches `tests/test_picking.cpp`).

---

### Task 1: `CameraView` header + `ApplicationContext` override channel

**Files:**
- Create: `src/common/include/CameraView.h`
- Modify: `src/common/include/ApplicationContext.h` (include + two members)

This task is data + atomics only (no behavior yet), so it has no unit test — verification is a clean compile.

- [ ] **Step 1: Create the `CameraView` header**

`src/common/include/CameraView.h`:
```cpp
#pragma once

#include <glm/mat4x4.hpp>
#include <glm/vec3.hpp>

// A resolved camera the render passes consume: world-space View/Projection + eye position.
// Trivially copyable so it can ride a Seqlock (single-writer lock-free publish).
struct CameraView {
    glm::mat4 View{1.0f};
    glm::mat4 Projection{1.0f};
    glm::vec3 Position{0.0f};
};
```

- [ ] **Step 2: Include it + add the channel in `ApplicationContext.h`**

Add the include next to the existing includes (after `#include "Seqlock.h"`):
```cpp
#include "CameraView.h"
```
Then, inside `struct ApplicationContext`, immediately after the `SelectedEntity` member (the `std::atomic<uint64_t> SelectedEntity{INVALID_ENTITY};` block), add:
```cpp
    // Editor free-look camera override (editor only). The overlay (RenderThread) writes both each
    // frame; Renderer (RenderThread, earlier in the next frame) reads them -> 1-frame lag, like
    // SelectedEntity. Runtime has no overlay, never writes them, so EditorCameraActive stays false
    // and rendering uses the game's WorldCameraComponent unchanged.
    std::atomic<bool>   EditorCameraActive{false};
    Seqlock<CameraView> EditorCamera{};
```

- [ ] **Step 3: Build to verify it compiles**

```
cmake --build out/build/msvc-win64-vs2026-community --target Engine
```
Expected: builds clean. (The `Seqlock<CameraView>` instantiation compiles only if `CameraView` is trivially copyable — the `static_assert` in `Seqlock.h` is the check.)

- [ ] **Step 4: Commit**

```bash
git add src/common/include/CameraView.h src/common/include/ApplicationContext.h
git commit -m "feat: add CameraView + editor-camera override channel to ApplicationContext"
```

---

### Task 2: Renderer resolves the active camera; world passes read it

**Files:**
- Modify: `src/engine/src/rendering/Renderer.h` (member + accessor)
- Modify: `src/engine/src/rendering/Renderer.cpp` (resolve before the pass loop)
- Modify: `src/engine/src/rendering/passes/MeshRenderPass.cpp:360-363`
- Modify: `src/engine/src/rendering/passes/PrimitiveRenderPass.cpp:263-266`
- Modify: `src/engine/src/rendering/passes/OutlineRenderPass.cpp:113-118`

This is the one shared-code change. **Hard invariant:** when `EditorCameraActive` is false (always, in `runtime.exe`, and in Play mode), the resolved camera must equal the snapshot's `WorldCameraComponent` (or identity when absent) — byte-identical to today. No unit test (render-path glue); verified by the regression suite staying green + the runtime/editor smoke in Final Verification.

- [ ] **Step 1: Add the member + accessor in `Renderer.h`**

In `class ENGINE_API Renderer`, in the `public:` section after `ApplicationContext* GetAppContext() const { return m_AppContext; }` (line 113), add:
```cpp
    // The camera the world passes render with this frame: editor override when active, else the
    // game's WorldCameraComponent. Resolved once at the top of Render(), before the pass loop.
    const CameraView& GetActiveCamera() const { return m_ActiveCamera; }
```
And in the `private:` data section (after `ApplicationContext* m_AppContext;`, line 135), add:
```cpp
    CameraView m_ActiveCamera{};
```
(`CameraView` is visible via `ApplicationContext.h`, already included at `Renderer.h:8`.)

- [ ] **Step 2: Resolve the active camera in `Renderer.cpp` before the pass loop**

In `Renderer::Render`, immediately before the `// Render all passes into the scene buffer` comment + `for (auto& pass : m_RenderPasses)` loop (around `Renderer.cpp:202-203`), insert:
```cpp
                // Resolve the camera the world passes use this frame. Editor override (set by the
                // ImGui overlay last frame) wins when active; otherwise the game's WorldCameraComponent
                // from the snapshot. Runtime never sets EditorCameraActive -> always the game camera.
                {
                    CameraView active{}; // identity V/P, zero pos: matches the passes' old null fallback
                    if (m_AppContext && m_AppContext->EditorCameraActive.load(std::memory_order_relaxed)) {
                        active = m_AppContext->EditorCamera.load();
                    } else if (world) {
                        if (const auto* cam = world->GetSingleton<WorldCameraComponent>())
                            active = { cam->View, cam->Projection, cam->Position };
                    }
                    m_ActiveCamera = active;
                }
```
(`world` and `m_AppContext` are both in scope here; `WorldCameraComponent` is visible via the ECS headers `ApplicationContext.h` pulls in.)

- [ ] **Step 3: MeshRenderPass reads the resolved camera**

In `src/engine/src/rendering/passes/MeshRenderPass.cpp`, replace lines 360-363:
```cpp
        glm::mat4 V(1.0f), P(1.0f); glm::vec3 camPos(0.0f);
        if (const auto* cam = world ? world->GetSingleton<WorldCameraComponent>() : nullptr) {
            V = cam->View; P = cam->Projection; camPos = cam->Position;
        }
```
with:
```cpp
        const CameraView& cam = m_Renderer->GetActiveCamera();
        const glm::mat4 V = cam.View, P = cam.Projection;
        const glm::vec3 camPos = cam.Position;
```

- [ ] **Step 4: PrimitiveRenderPass reads the resolved camera**

In `src/engine/src/rendering/passes/PrimitiveRenderPass.cpp`, replace lines 263-266:
```cpp
    glm::mat4 V(1.0f), P(1.0f); glm::vec3 camPos(0.0f);
    if (const auto* cam = world ? world->GetSingleton<WorldCameraComponent>() : nullptr) {
        V = cam->View; P = cam->Projection; camPos = cam->Position;
    }
```
with:
```cpp
    const CameraView& cam = m_Renderer->GetActiveCamera();
    const glm::mat4 V = cam.View, P = cam.Projection;
    const glm::vec3 camPos = cam.Position;
```
(If `camPos` is unused later in this pass and the compiler warns, drop the `camPos` line — but keep it if the existing code referenced `camPos`. The original declared it, so keep parity.)

- [ ] **Step 5: OutlineRenderPass reads the resolved camera**

In `src/engine/src/rendering/passes/OutlineRenderPass.cpp`, replace lines 113-118:
```cpp
    glm::mat4 V(1.0f), P(1.0f);
    if (const auto* cam = world->GetSingleton<WorldCameraComponent>())
    {
        V = cam->View;
        P = cam->Projection;
    }
```
with:
```cpp
    const CameraView& camv = m_Renderer->GetActiveCamera();
    const glm::mat4 V = camv.View;
    const glm::mat4 P = camv.Projection;
```
(Outline uses only View/Projection — no `camPos`.)

- [ ] **Step 6: Build engine, editor, runtime + run the full test suite (regression)**

```
cmake --build out/build/msvc-win64-vs2026-community --target editor
cmake --build out/build/msvc-win64-vs2026-community --target runtime
cmake --build out/build/msvc-win64-vs2026-community --target test_ecs test_alloc test_frustum test_input test_picking
./out/build/msvc-win64-vs2026-community/bin/Debug/test_ecs.exe
./out/build/msvc-win64-vs2026-community/bin/Debug/test_alloc.exe
./out/build/msvc-win64-vs2026-community/bin/Debug/test_frustum.exe
./out/build/msvc-win64-vs2026-community/bin/Debug/test_input.exe
./out/build/msvc-win64-vs2026-community/bin/Debug/test_picking.exe
```
Expected: all build clean; each test prints its `All ... passed.` line (test_alloc prints one intentional ERROR line before its pass).

- [ ] **Step 7: Commit**

```bash
git add src/engine/src/rendering/Renderer.h src/engine/src/rendering/Renderer.cpp src/engine/src/rendering/passes/MeshRenderPass.cpp src/engine/src/rendering/passes/PrimitiveRenderPass.cpp src/engine/src/rendering/passes/OutlineRenderPass.cpp
git commit -m "feat: Renderer resolves active camera; world passes read it (no-op fallback = game camera)"
```

---

### Task 3: `EditorCamera` controller (pure math) + `test_editorcam`

**Files:**
- Create: `src/editor/src/rendering/EditorCamera.h`
- Create: `src/editor/src/rendering/EditorCamera.cpp`
- Create: `tests/test_editorcam.cpp`
- Modify: `tests/CMakeLists.txt` (new `test_editorcam` target)
- Modify: `src/editor/CMakeLists.txt` (compile `EditorCamera.cpp` into the editor)

TDD: write the test first, watch it fail to build, implement, watch it pass. The math is pure (GLM only, no ImGui/NVRHI), so it is fully unit-testable.

- [ ] **Step 1: Write the header (interface the test compiles against)**

`src/editor/src/rendering/EditorCamera.h`:
```cpp
#pragma once

#include <glm/glm.hpp>
#include "CameraView.h"

// Per-frame fly inputs, filled by the editor overlay from ImGui IO. Pure data.
struct EditorCameraInput {
    float MouseDX = 0.0f, MouseDY = 0.0f; // pixels this frame
    float Wheel   = 0.0f;                 // notches this frame
    bool  Look = false;                   // RMB held: mouse-look + WASD fly
    bool  Pan  = false;                   // MMB held: screen-space pan
    bool  Orbit = false;                  // Alt+LMB held: orbit the pivot
    bool  W=false, A=false, S=false, D=false, Q=false, E=false; // movement keys held
    bool  Frame = false;                  // F pressed this frame: frame the pivot
    bool  HasPivot = false;               // a selected entity exists (for orbit/frame)
    glm::vec3 PivotCenter{0.0f};          // selection AABB center (world)
    float     PivotRadius = 1.0f;         // selection bounding radius (world)
    float DeltaSeconds = 0.0f;
};

// Editor-owned free-look camera. Session-only state; no ImGui/NVRHI dependency (unit-testable).
class EditorCamera {
public:
    void Update(const EditorCameraInput& in);
    CameraView ToCameraView(float aspect) const; // perspectiveRH_ZO(Fov, aspect, Near, Far)

    glm::vec3 Forward() const;
    glm::vec3 Right() const;
    glm::vec3 Up() const;

    void FrameSelection(const glm::vec3& center, float radius);
    void OrbitAround(const glm::vec3& pivot, float dYaw, float dPitch);

    // Accessors for tests.
    glm::vec3 GetPosition() const { return m_Position; }
    float     GetYaw()   const { return m_Yaw; }
    float     GetPitch() const { return m_Pitch; }
    float     GetFlySpeed() const { return m_FlySpeed; }

private:
    glm::vec3 m_Position{0.0f, 5.0f, 10.0f}; // matches the game free-look default
    float m_Yaw   = 0.0f;   // radians around +Y; 0 => forward = -Z
    float m_Pitch = 0.0f;   // radians; +up; clamped +/- 89 deg
    float m_Fov   = glm::radians(60.0f);
    float m_FlySpeed = 7.5f; // units/sec; wheel-adjustable while looking

    static constexpr float kNear = 0.1f;
    static constexpr float kFar  = 1000.0f;
};
```

- [ ] **Step 2: Write the failing test**

`tests/test_editorcam.cpp`:
```cpp
#include <cstdio>
#include <cmath>
#include <glm/glm.hpp>
#include <glm/gtc/matrix_transform.hpp>

#include "EditorCamera.h"

static int g_Failures = 0;
#define EXPECT(cond)                                                     \
    do {                                                                 \
        if (!(cond)) {                                                   \
            std::fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); \
            ++g_Failures;                                                \
        }                                                                \
    } while (0)

static void T00_default_forward_is_minus_z()
{
    EditorCamera c;
    const glm::vec3 f = c.Forward();
    EXPECT(std::abs(f.x) < 1e-4f);
    EXPECT(std::abs(f.y) < 1e-4f);
    EXPECT(f.z < -0.99f);            // looks down -Z by default
    const glm::vec3 r = c.Right();
    EXPECT(r.x > 0.99f);             // right is +X
}

static void T01_look_yaw_rotates_forward()
{
    EditorCamera c;
    EditorCameraInput in{};
    in.Look = true; in.MouseDX = 100.0f; in.DeltaSeconds = 0.016f;
    c.Update(in);
    // Yaw changed; forward is no longer pure -Z.
    EXPECT(std::abs(c.GetYaw()) > 1e-3f);
    EXPECT(std::abs(c.Forward().x) > 1e-3f);
}

static void T02_pitch_clamps_at_89_deg()
{
    EditorCamera c;
    EditorCameraInput in{};
    in.Look = true; in.MouseDY = -100000.0f; in.DeltaSeconds = 0.016f; // slam look up
    c.Update(in);
    EXPECT(c.GetPitch() <= glm::radians(89.0f) + 1e-3f);
    EXPECT(c.GetPitch() >= glm::radians(89.0f) - 1e-1f); // hit the clamp
}

static void T03_wasd_moves_along_basis()
{
    EditorCamera c;
    const glm::vec3 p0 = c.GetPosition();
    EditorCameraInput in{};
    in.Look = true; in.W = true; in.DeltaSeconds = 1.0f; // forward 1 sec
    c.Update(in);
    const glm::vec3 d = c.GetPosition() - p0;
    // Moved along forward (-Z), magnitude ~ FlySpeed.
    EXPECT(d.z < -1.0f);
    EXPECT(std::abs(glm::length(d) - c.GetFlySpeed()) < 1e-3f);
}

static void T04_wasd_ignored_without_look()
{
    EditorCamera c;
    const glm::vec3 p0 = c.GetPosition();
    EditorCameraInput in{};
    in.W = true; in.DeltaSeconds = 1.0f; // no Look held
    c.Update(in);
    EXPECT(glm::length(c.GetPosition() - p0) < 1e-5f); // unchanged
}

static void T05_wheel_dollies_when_not_looking()
{
    EditorCamera c;
    const glm::vec3 p0 = c.GetPosition();
    EditorCameraInput in{};
    in.Wheel = 1.0f; // scroll, no RMB
    c.Update(in);
    const glm::vec3 d = c.GetPosition() - p0;
    EXPECT(d.z < 0.0f);              // dollied forward (-Z)
    EXPECT(glm::length(d) > 1e-3f);
}

static void T06_orbit_preserves_distance()
{
    EditorCamera c;
    const glm::vec3 pivot{0.0f, 0.0f, 0.0f};
    const float d0 = glm::length(c.GetPosition() - pivot);
    c.OrbitAround(pivot, glm::radians(30.0f), glm::radians(10.0f));
    const float d1 = glm::length(c.GetPosition() - pivot);
    EXPECT(std::abs(d0 - d1) < 1e-3f);     // distance to pivot preserved
}

static void T07_frame_centers_pivot()
{
    EditorCamera c;
    const glm::vec3 center{3.0f, 1.0f, -2.0f};
    c.FrameSelection(center, 2.0f);
    // The pivot projects to ~screen center (NDC origin) and is in front of the camera.
    const CameraView cv = c.ToCameraView(1.0f);
    glm::vec4 clip = cv.Projection * cv.View * glm::vec4(center, 1.0f);
    EXPECT(clip.w > 0.0f);                          // in front
    const glm::vec3 ndc = glm::vec3(clip) / clip.w;
    EXPECT(std::abs(ndc.x) < 1e-2f);
    EXPECT(std::abs(ndc.y) < 1e-2f);
    EXPECT(ndc.z > 0.0f && ndc.z < 1.0f);           // within depth [0,1]
}

static void T08_tocameraview_view_looks_at_target()
{
    EditorCamera c; // at (0,5,10) looking -Z
    const CameraView cv = c.ToCameraView(1.6f);
    // A point straight ahead maps to view-space -Z (in front).
    glm::vec4 vp = cv.View * glm::vec4(0.0f, 5.0f, 0.0f, 1.0f); // 10 units ahead along -Z
    EXPECT(vp.z < 0.0f);
    EXPECT(std::abs(vp.x) < 1e-3f);
    EXPECT(std::abs(vp.y) < 1e-3f);
}

int main()
{
    T00_default_forward_is_minus_z();
    T01_look_yaw_rotates_forward();
    T02_pitch_clamps_at_89_deg();
    T03_wasd_moves_along_basis();
    T04_wasd_ignored_without_look();
    T05_wheel_dollies_when_not_looking();
    T06_orbit_preserves_distance();
    T07_frame_centers_pivot();
    T08_tocameraview_view_looks_at_target();

    if (g_Failures == 0) { std::printf("All editor camera tests passed.\n"); return 0; }
    std::printf("%d editor camera test(s) FAILED.\n", g_Failures);
    return 1;
}
```

- [ ] **Step 3: Wire the `test_editorcam` CMake target**

In `tests/CMakeLists.txt`, after the `test_picking` block (ends at line ~105), append:
```cmake
add_executable(test_editorcam
    test_editorcam.cpp
    ${CMAKE_SOURCE_DIR}/src/editor/src/rendering/EditorCamera.cpp
)

target_link_libraries(test_editorcam PRIVATE
    glm::glm
)

target_include_directories(test_editorcam PRIVATE
    ${CMAKE_SOURCE_DIR}/src/common/include
    ${CMAKE_SOURCE_DIR}/src/editor/src/rendering
)

target_compile_definitions(test_editorcam PRIVATE
    GLM_FORCE_DEPTH_ZERO_TO_ONE
    GLM_FORCE_RIGHT_HANDED
    GLM_ENABLE_EXPERIMENTAL
)

set_target_properties(test_editorcam PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY "${RUNTIME_DIR}"
    FOLDER Tests
)
```

- [ ] **Step 4: Reconfigure + build the test → expect FAIL (link/undefined: EditorCamera methods)**

```
cmake --preset msvc-win64-vs2026-community
cmake --build out/build/msvc-win64-vs2026-community --target test_editorcam
```
Expected: link or compile error — `EditorCamera::Update`/`Forward`/`ToCameraView`/`FrameSelection`/`OrbitAround` undefined (only the header exists; `EditorCamera.cpp` is empty/missing).

- [ ] **Step 5: Implement `EditorCamera.cpp`**

`src/editor/src/rendering/EditorCamera.cpp`:
```cpp
#include "EditorCamera.h"

#include <glm/gtc/matrix_transform.hpp>
#include <algorithm>
#include <cmath>

namespace {
constexpr float kLookSens = 0.0035f; // radians per pixel
constexpr float kPanScale = 0.01f;   // world units per pixel
constexpr float kDollyStep = 1.0f;   // world units per wheel notch
constexpr float kPitchLimit = glm::radians(89.0f);
}

glm::vec3 EditorCamera::Forward() const
{
    return glm::normalize(glm::vec3(
        -std::sin(m_Yaw) * std::cos(m_Pitch),
         std::sin(m_Pitch),
        -std::cos(m_Yaw) * std::cos(m_Pitch)));
}

glm::vec3 EditorCamera::Right() const
{
    return glm::normalize(glm::cross(Forward(), glm::vec3(0.0f, 1.0f, 0.0f)));
}

glm::vec3 EditorCamera::Up() const
{
    return glm::normalize(glm::cross(Right(), Forward()));
}

void EditorCamera::OrbitAround(const glm::vec3& pivot, float dYaw, float dPitch)
{
    glm::vec3 offset = m_Position - pivot;
    const float dist = glm::length(offset);
    if (dist < 1e-4f) return;

    float yaw   = std::atan2(offset.x, offset.z);
    float pitch = std::asin(std::clamp(offset.y / dist, -1.0f, 1.0f));
    yaw   += dYaw;
    pitch  = std::clamp(pitch + dPitch, -kPitchLimit, kPitchLimit);

    offset = dist * glm::vec3(std::cos(pitch) * std::sin(yaw),
                              std::sin(pitch),
                              std::cos(pitch) * std::cos(yaw));
    m_Position = pivot + offset;

    // Re-aim at the pivot.
    const glm::vec3 dir = glm::normalize(pivot - m_Position);
    m_Yaw   = std::atan2(-dir.x, -dir.z);
    m_Pitch = std::asin(std::clamp(dir.y, -1.0f, 1.0f));
}

void EditorCamera::FrameSelection(const glm::vec3& center, float radius)
{
    if (radius < 1e-4f) radius = 1.0f;
    const float dist = radius / std::sin(m_Fov * 0.5f); // fit the bounding sphere in the vertical FOV
    m_Position = center - Forward() * dist;             // back off along current forward -> looks at center
}

void EditorCamera::Update(const EditorCameraInput& in)
{
    if (in.Look) {
        m_Yaw   -= in.MouseDX * kLookSens;
        m_Pitch  = std::clamp(m_Pitch - in.MouseDY * kLookSens, -kPitchLimit, kPitchLimit);

        const glm::vec3 fwd = Forward();
        const glm::vec3 right = Right();
        const glm::vec3 worldUp(0.0f, 1.0f, 0.0f);
        const float v = m_FlySpeed * in.DeltaSeconds;
        if (in.W) m_Position += fwd * v;
        if (in.S) m_Position -= fwd * v;
        if (in.D) m_Position += right * v;
        if (in.A) m_Position -= right * v;
        if (in.E) m_Position += worldUp * v;
        if (in.Q) m_Position -= worldUp * v;

        if (in.Wheel != 0.0f)
            m_FlySpeed = std::clamp(m_FlySpeed * (1.0f + 0.1f * in.Wheel), 0.5f, 200.0f);
    } else if (in.Wheel != 0.0f) {
        m_Position += Forward() * (in.Wheel * kDollyStep); // dolly when not looking
    }

    if (in.Pan) {
        m_Position -= Right() * (in.MouseDX * kPanScale);
        m_Position += Up()    * (in.MouseDY * kPanScale);
    }

    if (in.Orbit) {
        const glm::vec3 pivot = in.HasPivot ? in.PivotCenter : glm::vec3(0.0f);
        OrbitAround(pivot, -in.MouseDX * kLookSens, -in.MouseDY * kLookSens);
    }

    if (in.Frame && in.HasPivot)
        FrameSelection(in.PivotCenter, in.PivotRadius);
}

CameraView EditorCamera::ToCameraView(float aspect) const
{
    if (aspect <= 0.0f) aspect = 16.0f / 9.0f;
    CameraView cv;
    cv.View       = glm::lookAtRH(m_Position, m_Position + Forward(), glm::vec3(0.0f, 1.0f, 0.0f));
    cv.Projection = glm::perspectiveRH_ZO(m_Fov, aspect, kNear, kFar);
    cv.Position   = m_Position;
    return cv;
}
```

- [ ] **Step 6: Add `EditorCamera.cpp` to the editor target**

In `src/editor/CMakeLists.txt`, in the editor source list, after `src/rendering/imgui/ViewportPicker.cpp` (line ~22), add:
```cmake
    src/rendering/EditorCamera.cpp
```

- [ ] **Step 7: Build the test → expect PASS, and build the editor**

```
cmake --build out/build/msvc-win64-vs2026-community --target test_editorcam
./out/build/msvc-win64-vs2026-community/bin/Debug/test_editorcam.exe
cmake --build out/build/msvc-win64-vs2026-community --target editor
```
Expected: `All editor camera tests passed.`; editor builds clean.

- [ ] **Step 8: Commit**

```bash
git add src/editor/src/rendering/EditorCamera.h src/editor/src/rendering/EditorCamera.cpp tests/test_editorcam.cpp tests/CMakeLists.txt src/editor/CMakeLists.txt
git commit -m "feat: add EditorCamera free-look controller + test_editorcam"
```

---

### Task 4: Edit/Play toggle + overlay integration (drive + publish the editor camera)

**Files:**
- Modify: `src/editor/src/rendering/imgui/EditorContext.h` (camera fields for picking)
- Modify: `src/editor/src/rendering/imgui/MainMenuBar.h` (Draw gains an `editMode` ref)
- Modify: `src/editor/src/rendering/imgui/MainMenuBar.cpp` (Edit/Play button)
- Modify: `src/editor/src/rendering/imgui/ImGuiRenderer.h` (members + include)
- Modify: `src/editor/src/rendering/imgui/ImGuiRenderer.cpp` (F6, input, update, publish, routing)

No unit test (ImGui glue). Verified by build + the GUI smoke in Final Verification.

- [ ] **Step 1: Add editor-camera fields to `EditorContext`**

In `src/editor/src/rendering/imgui/EditorContext.h`, add `#include <glm/glm.hpp>` after `#include <memory>`, then add these fields to `struct EditorContext` (after `ViewportW`/`ViewportH`):
```cpp
    // Editor camera (for picking): when active, PickEntity builds the ray from these instead of
    // the game's WorldCameraComponent, matching what the render passes drew this frame.
    bool      EditorCameraActive = false;
    glm::mat4 EditorCamView{1.0f};
    glm::mat4 EditorCamProj{1.0f};
```

- [ ] **Step 2: Let `MainMenuBar::Draw` host the Edit/Play toggle**

In `src/editor/src/rendering/imgui/MainMenuBar.h`, change the method signature:
```cpp
    bool Draw(const EditorContext& ctx, bool& editMode);
```

In `src/editor/src/rendering/imgui/MainMenuBar.cpp`, change the definition to match:
```cpp
bool MainMenuBar::Draw(const EditorContext& ctx, bool& editMode)
```
Then, just before `ImGui::EndMainMenuBar();` (line 123), add a right-aligned toggle button:
```cpp
        // Edit/Play mode toggle (also bound to F6 in ImGuiRenderer). Edit = free editor camera;
        // Play = the game's camera drives the viewport.
        {
            const char* label = editMode ? "Mode: Edit" : "Mode: Play";
            const float bw = ImGui::CalcTextSize(label).x + ImGui::GetStyle().FramePadding.x * 2.0f;
            ImGui::SameLine(ImGui::GetWindowWidth() - bw - 12.0f);
            if (ImGui::Button(label))
                editMode = !editMode;
        }
```

- [ ] **Step 3: Add the camera + mode state to `ImGuiRenderer`**

In `src/editor/src/rendering/imgui/ImGuiRenderer.h`, add the include near the other editor includes (next to `#include "EcsInspectorPanel.h"`):
```cpp
#include "EditorCamera.h"
```
Then add to the class's private members (near `m_EcsInspector`):
```cpp
    EditorCamera m_EditorCamera;
    bool         m_EditMode = true; // default to Edit (free camera) on launch
```

- [ ] **Step 4: Wire the toggle call site**

In `src/editor/src/rendering/imgui/ImGuiRenderer.cpp`, change line 284:
```cpp
        if (m_MenuBar.Draw(ctx)) s_ResetLayout = true;
```
to:
```cpp
        if (m_MenuBar.Draw(ctx, m_EditMode)) s_ResetLayout = true;
```

- [ ] **Step 5: F6 toggles the mode (gated on not typing)**

In `ImGuiRenderer.cpp`, right after the menu-bar call (after the line from Step 4), add:
```cpp
        if (ImGui::IsKeyPressed(ImGuiKey_F6) && !io.WantTextInput)
            m_EditMode = !m_EditMode;
```
(`io` is already in scope in this function — it is used later at lines 372/io.MousePos at 343.)

- [ ] **Step 6: Drive the editor camera + publish the override, after the Viewport panel block**

In `ImGuiRenderer.cpp`, immediately after the `ctx.ViewportH = m_LastViewportH;` line (line 337) and before the existing viewport-pick `if` (line 339), insert:
```cpp
        // ---- Editor camera (Edit mode) ----
        const float vpAspect = m_LastViewportH ? float(m_LastViewportW) / float(m_LastViewportH)
                                               : 16.0f / 9.0f;
        if (m_EditMode && (m_ViewportHovered || m_ViewportFocused)) {
            EditorCameraInput cin{};
            cin.MouseDX = io.MouseDelta.x;
            cin.MouseDY = io.MouseDelta.y;
            cin.Wheel   = io.MouseWheel;
            cin.Look    = ImGui::IsMouseDown(ImGuiMouseButton_Right);
            cin.Pan     = ImGui::IsMouseDown(ImGuiMouseButton_Middle);
            cin.Orbit   = io.KeyAlt && ImGui::IsMouseDown(ImGuiMouseButton_Left);
            cin.W = ImGui::IsKeyDown(ImGuiKey_W); cin.S = ImGui::IsKeyDown(ImGuiKey_S);
            cin.A = ImGui::IsKeyDown(ImGuiKey_A); cin.D = ImGui::IsKeyDown(ImGuiKey_D);
            cin.Q = ImGui::IsKeyDown(ImGuiKey_Q); cin.E = ImGui::IsKeyDown(ImGuiKey_E);
            cin.Frame = ImGui::IsKeyPressed(ImGuiKey_F);
            cin.DeltaSeconds = io.DeltaTime;

            // Pivot for orbit/frame = the selected entity's world AABB center + radius.
            const EntityId sel = m_EcsInspector.GetSelectedEntity();
            if (sel != INVALID_ENTITY && world) {
                const auto* tc = world->GetComponent<TransformComponent>(sel);
                const auto* mc = world->GetComponent<MeshComponent>(sel);
                if (tc) {
                    if (mc && m_MeshSystem) {
                        const auto b = m_MeshSystem->GetMeshBounds(mc->MeshId);
                        if (b.valid) {
                            glm::vec3 wMin, wMax;
                            TransformAABB(ModelMatrix(*tc), b.min, b.max, wMin, wMax);
                            cin.PivotCenter = 0.5f * (wMin + wMax);
                            cin.PivotRadius = 0.5f * glm::length(wMax - wMin);
                            cin.HasPivot = true;
                        }
                    }
                    if (!cin.HasPivot) { // no mesh bounds -> pivot on the transform position
                        cin.PivotCenter = tc->Position;
                        cin.PivotRadius = 1.0f;
                        cin.HasPivot = true;
                    }
                }
            }
            m_EditorCamera.Update(cin);
        }

        // Publish the override channel for the Renderer (1-frame lag, like SelectedEntity).
        const CameraView editorCam = m_EditorCamera.ToCameraView(vpAspect);
        if (m_AppContext) {
            m_AppContext->EditorCameraActive.store(m_EditMode, std::memory_order_relaxed);
            m_AppContext->EditorCamera.store(editorCam);
        }
        // Hand the editor camera to picking (consumed in Task 5).
        ctx.EditorCameraActive = m_EditMode;
        ctx.EditorCamView = editorCam.View;
        ctx.EditorCamProj = editorCam.Projection;
```
Required includes at the top of `ImGuiRenderer.cpp` if not already present: `#include "TransformMath.h"` (for `ModelMatrix`), `#include "Frustum.h"` (for `TransformAABB`), `#include "MeshSystem.h"` (for `GetMeshBounds`) — `ViewportPicker.cpp` already uses all three, so copy whichever are missing here. `ctx` is a non-const local `EditorContext` (declared at line 269), so assigning its fields is fine.

- [ ] **Step 7: Input routing — in Edit mode the editor owns viewport input**

In `ImGuiRenderer.cpp`, replace the input-routing block at lines 369-374:
```cpp
        if (m_AppContext) {
            m_AppContext->GameAcceptsMouse.store(m_ViewportHovered && !ImGuizmo::IsUsing(),
                                                 std::memory_order_relaxed);
            m_AppContext->GameAcceptsKeyboard.store(m_ViewportFocused && !io.WantTextInput,
                                                    std::memory_order_relaxed);
        }
```
with:
```cpp
        if (m_AppContext) {
            // In Edit mode the editor camera consumes viewport input, so the game gets none (its
            // free-look would otherwise fight the editor camera). In Play mode keep the prior rule.
            const bool toGameMouse    = !m_EditMode && m_ViewportHovered && !ImGuizmo::IsUsing();
            const bool toGameKeyboard = !m_EditMode && m_ViewportFocused && !io.WantTextInput;
            m_AppContext->GameAcceptsMouse.store(toGameMouse, std::memory_order_relaxed);
            m_AppContext->GameAcceptsKeyboard.store(toGameKeyboard, std::memory_order_relaxed);
        }
```

- [ ] **Step 8: Build the editor**

```
cmake --build out/build/msvc-win64-vs2026-community --target editor
```
Expected: builds clean.

- [ ] **Step 9: Commit**

```bash
git add src/editor/src/rendering/imgui/EditorContext.h src/editor/src/rendering/imgui/MainMenuBar.h src/editor/src/rendering/imgui/MainMenuBar.cpp src/editor/src/rendering/imgui/ImGuiRenderer.h src/editor/src/rendering/imgui/ImGuiRenderer.cpp
git commit -m "feat: Edit/Play toggle (F6 + menu) drives + publishes the editor camera"
```

---

### Task 5: Picking uses the editor camera in Edit mode

**Files:**
- Modify: `src/editor/src/rendering/imgui/ViewportPicker.cpp:18-25`

The `EditorContext` fields were added + populated in Task 4; this task consumes them. No unit test (uses live ECS + ImGui state); covered by the GUI smoke.

- [ ] **Step 1: Choose the camera in `PickEntity`**

In `src/editor/src/rendering/imgui/ViewportPicker.cpp`, replace lines 18-25:
```cpp
    const auto* cam = ctx.World->GetSingleton<WorldCameraComponent>();
    if (!cam)
        return INVALID_ENTITY;

    const Ray ray = ScreenPointToRay(mouseX, mouseY,
                                     ctx.ViewportMinX, ctx.ViewportMinY,
                                     static_cast<float>(ctx.ViewportW), static_cast<float>(ctx.ViewportH),
                                     cam->View, cam->Projection);
```
with:
```cpp
    glm::mat4 view, proj;
    if (ctx.EditorCameraActive) {
        view = ctx.EditorCamView;
        proj = ctx.EditorCamProj;
    } else {
        const auto* cam = ctx.World->GetSingleton<WorldCameraComponent>();
        if (!cam)
            return INVALID_ENTITY;
        view = cam->View;
        proj = cam->Projection;
    }

    const Ray ray = ScreenPointToRay(mouseX, mouseY,
                                     ctx.ViewportMinX, ctx.ViewportMinY,
                                     static_cast<float>(ctx.ViewportW), static_cast<float>(ctx.ViewportH),
                                     view, proj);
```
(`glm` is already included via `EditorContext.h` / `ECS.h` in this TU.)

- [ ] **Step 2: Build the editor**

```
cmake --build out/build/msvc-win64-vs2026-community --target editor
```
Expected: builds clean.

- [ ] **Step 3: Commit**

```bash
git add src/editor/src/rendering/imgui/ViewportPicker.cpp
git commit -m "feat: viewport picking uses the editor camera in Edit mode"
```

---

## Final verification (after all tasks)

- [ ] Full build: `cmake --build out/build/msvc-win64-vs2026-community` — all targets green (editor, runtime, game, all test_*).
- [ ] All unit tests print their pass line:
```
./out/build/msvc-win64-vs2026-community/bin/Debug/test_ecs.exe
./out/build/msvc-win64-vs2026-community/bin/Debug/test_alloc.exe
./out/build/msvc-win64-vs2026-community/bin/Debug/test_frustum.exe
./out/build/msvc-win64-vs2026-community/bin/Debug/test_input.exe
./out/build/msvc-win64-vs2026-community/bin/Debug/test_picking.exe
./out/build/msvc-win64-vs2026-community/bin/Debug/test_editorcam.exe
```
Expected: `All ... passed.` from each (test_alloc prints one intentional ERROR line first).

- [ ] **GUI smoke (user-run; surface to the user — do not self-approve):**
  - Editor launches in **Edit** mode ("Mode: Edit" button shows). Over the viewport: hold **RMB** + move = look; **WASD** = fly; **Q/E** = down/up; **wheel while RMB held** = fly speed.
  - **MMB-drag** pans; **wheel (no RMB)** dollies; **Alt+LMB drag** orbits the selected entity; **F** frames the selection (no selection → F is a no-op, no crash).
  - **Picking + outline + gizmo** operate through the editor camera in Edit mode (click selects what's under the cursor as seen through the editor camera).
  - Toggle to **Play** (menu button or **F6**): the view snaps to the game camera; viewport input drives the game again (the game's free-look responds). Toggle back → editor camera resumes at its last pose.
  - **`runtime.exe`** renders the scene via the game camera exactly as before — no regression.

## Notes / non-goals
- No `GAME_API_VERSION` bump (editor camera is editor state, not an ECS component).
- Editor camera pose is **session-only** (resets on editor restart); default **Edit** mode on launch.
- Sim **keeps ticking** in Edit mode — the mode controls only camera + input routing; the existing pause is independent.
- Out of scope: persisting the editor camera, pausing on Edit, FOV animation, multiple editor cameras, saving the camera to `world.json`.
