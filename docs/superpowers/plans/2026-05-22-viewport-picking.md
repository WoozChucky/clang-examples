# Viewport Mouse-Picking Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Click an entity in the editor Viewport to select it (ray vs world-AABB → set the Inspector's selectedEntity → gizmo/editors follow); empty click deselects.

**Architecture:** Pure GLM ray/AABB math (`Picking.h`) + a shared `ModelMatrix(TransformComponent)` (`TransformMath.h`); an editor `ViewportPicker` ray-casts visible mesh entities' world AABBs (reusing `Frustum.h::TransformAABB` + `MeshSystem::GetMeshBounds`); `ImGuiRenderer` detects the Viewport click and sets the inspector's selection. Editor/render-thread only; no GameThread or ECS-command involvement.

**Tech Stack:** C++23, GLM (depth `[0,1]`, RH), custom ECS, Dear ImGui + ImGuizmo, CMake presets.

**Spec:** `docs/superpowers/specs/2026-05-22-viewport-picking-design.md`

**Conventions (every task):**
- Build preset `msvc-win64-vs2026-community`; build dir `out/build/msvc-win64-vs2026-community`; exes in `bin/Debug/`.
- New `.cpp`/target → `cmake --preset msvc-win64-vs2026-community` before building.
- No `GAME_API_VERSION` bump; `game`/ECS layout unchanged.
- Commit author is the repo default (`Nuno Silva <nuno.levezinho@live.com.pt>`) — plain `git commit`. Never stage `.claude/`. Stage only the files each step names. After each commit, `git log -1 --format='%an <%ae>'` must show the personal email.

---

### Task 1: Shared `ModelMatrix(TransformComponent)` (DRY)

Behavior-preserving extraction of the `T*Rz*Ry*Rx*S` formula into one helper, adopted by the mesh pass and the inspector gizmo. (The picker uses it in Task 3.)

**Files:**
- Create: `src/common/include/TransformMath.h`
- Modify: `src/engine/src/rendering/passes/MeshRenderPass.cpp`
- Modify: `src/editor/src/rendering/imgui/EcsInspectorPanel.cpp`

- [ ] **Step 1: Create `src/common/include/TransformMath.h`**
```cpp
#pragma once
#include <glm/glm.hpp>
#include <glm/gtc/matrix_transform.hpp>
#include "ECS.h" // TransformComponent

// The single source of truth for an entity's world matrix: T * Rz * Ry * Rx * S
// (translate * rotZ * rotY * rotX * scale), Euler radians from TransformComponent::Rotation.
inline glm::mat4 ModelMatrix(const TransformComponent& t)
{
    glm::mat4 T  = glm::translate(glm::mat4(1.0f), t.Position);
    glm::mat4 Rx = glm::rotate(glm::mat4(1.0f), t.Rotation.x, glm::vec3(1.f, 0.f, 0.f));
    glm::mat4 Ry = glm::rotate(glm::mat4(1.0f), t.Rotation.y, glm::vec3(0.f, 1.f, 0.f));
    glm::mat4 Rz = glm::rotate(glm::mat4(1.0f), t.Rotation.z, glm::vec3(0.f, 0.f, 1.f));
    glm::mat4 S  = glm::scale(glm::mat4(1.0f), t.Scale);
    return T * Rz * Ry * Rx * S;
}
```

- [ ] **Step 2: Adopt in `MeshRenderPass.cpp`**
Add `#include "TransformMath.h"` with the other includes. DELETE the file-static
`static glm::mat4 BuildWorldMatrix(const TransformComponent& t) { ... }` definition. Replace its
two call sites: the cull-gate `BuildWorldMatrix(transform)` → `ModelMatrix(transform)`, and the
instance-loop `glm::mat4 M = BuildWorldMatrix(*transform);` → `glm::mat4 M = ModelMatrix(*transform);`.
(grep `BuildWorldMatrix` to confirm zero remain.)

- [ ] **Step 3: Adopt in `EcsInspectorPanel.cpp`**
Add `#include "TransformMath.h"`. Find the gizmo's inline world-matrix build from `editTransform`:
```cpp
                                glm::mat4 T = glm::translate(glm::mat4(1.0f), editTransform.Position);
                                glm::mat4 Rx = glm::rotate(glm::mat4(1.0f), editTransform.Rotation.x, glm::vec3(1.f, 0.f, 0.f));
                                glm::mat4 Ry = glm::rotate(glm::mat4(1.0f), editTransform.Rotation.y, glm::vec3(0.f, 1.f, 0.f));
                                glm::mat4 Rz = glm::rotate(glm::mat4(1.0f), editTransform.Rotation.z, glm::vec3(0.f, 0.f, 1.f));
                                glm::mat4 S  = glm::scale(glm::mat4(1.0f), editTransform.Scale);
                                glm::mat4 M  = T * Rz * Ry * Rx * S;
```
Replace that entire block with:
```cpp
                                glm::mat4 M = ModelMatrix(editTransform);
```
(Match the actual indentation/variable names in the file; the input is `editTransform`. If the
local names differ, adapt — the result must be `glm::mat4 M = ModelMatrix(editTransform);` feeding
the existing `DecomposeMatrixToComponents`/`EditTransform` calls unchanged.)

- [ ] **Step 4: Build + regression**
```
cmake --build out/build/msvc-win64-vs2026-community --target Engine
cmake --build out/build/msvc-win64-vs2026-community --target editor
cmake --build out/build/msvc-win64-vs2026-community --target runtime
cmake --build out/build/msvc-win64-vs2026-community --target test_ecs
cmake --build out/build/msvc-win64-vs2026-community --target test_alloc
./out/build/msvc-win64-vs2026-community/bin/Debug/test_ecs.exe
./out/build/msvc-win64-vs2026-community/bin/Debug/test_alloc.exe
```
Expected: all clean; both tests pass. Behavior unchanged (identical formula) — scene + gizmo render exactly as before.

- [ ] **Step 5: Commit**
```bash
git add src/common/include/TransformMath.h src/engine/src/rendering/passes/MeshRenderPass.cpp src/editor/src/rendering/imgui/EcsInspectorPanel.cpp
git commit -m "refactor: extract shared ModelMatrix(TransformComponent); adopt in mesh pass + inspector"
```

---

### Task 2: `Picking.h` ray/AABB math + `test_picking`

**Files:**
- Create: `src/common/include/Picking.h`
- Create: `tests/test_picking.cpp`
- Modify: `tests/CMakeLists.txt` (append a `test_picking` target)

- [ ] **Step 1: Write the failing test** — create `tests/test_picking.cpp` (self-contained, like `test_frustum.cpp`):
```cpp
#include <cstdio>
#include <cmath>
#include <glm/glm.hpp>
#include <glm/gtc/matrix_transform.hpp>

#include "Picking.h"

static int g_Failures = 0;
#define EXPECT(cond)                                                     \
    do {                                                                 \
        if (!(cond)) {                                                   \
            std::fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); \
            ++g_Failures;                                                \
        }                                                                \
    } while (0)

static void T00_smoke() { EXPECT(1 + 1 == 2); }

// Ray from origin straight down -Z hits a box ahead, with tHit ~= 9.
static void T01_aabb_hit_ahead()
{
    Ray r; r.Origin = {0,0,0}; r.Dir = {0,0,-1};
    float t = -1.0f;
    EXPECT(RayIntersectsAABB(r, {-1,-1,-11}, {1,1,-9}, t) == true);
    EXPECT(std::abs(t - 9.0f) < 1e-3f);
}

// A box behind the ray origin is not hit.
static void T02_aabb_behind_miss()
{
    Ray r; r.Origin = {0,0,0}; r.Dir = {0,0,-1};
    float t = -1.0f;
    EXPECT(RayIntersectsAABB(r, {-1,-1,9}, {1,1,11}, t) == false);
}

// A box off the ray axis (ray parallel to the X slab, origin outside it) is not hit.
static void T03_aabb_offaxis_miss()
{
    Ray r; r.Origin = {0,0,0}; r.Dir = {0,0,-1};
    float t = -1.0f;
    EXPECT(RayIntersectsAABB(r, {5,-1,-11}, {6,1,-9}, t) == false);
}

// Origin inside the box -> tHit == 0.
static void T04_aabb_inside_origin()
{
    Ray r; r.Origin = {0,0,0}; r.Dir = {0,0,-1};
    float t = -1.0f;
    EXPECT(RayIntersectsAABB(r, {-1,-1,-1}, {1,1,1}, t) == true);
    EXPECT(std::abs(t) < 1e-6f);
}

// Two boxes along the ray: the nearer one has the smaller tHit.
static void T05_nearest_ordering()
{
    Ray r; r.Origin = {0,0,0}; r.Dir = {0,0,-1};
    float tNear = 0.0f, tFar = 0.0f;
    EXPECT(RayIntersectsAABB(r, {-1,-1,-11}, {1,1,-9},  tNear) == true);
    EXPECT(RayIntersectsAABB(r, {-1,-1,-21}, {1,1,-19}, tFar)  == true);
    EXPECT(tNear < tFar);
}

// Center-of-viewport click yields a ray pointing into the scene (~ -Z for this camera).
static void T10_screen_center_ray()
{
    const glm::mat4 V = glm::lookAtRH(glm::vec3(0,0,0), glm::vec3(0,0,-1), glm::vec3(0,1,0));
    const glm::mat4 P = glm::perspectiveRH_ZO(glm::radians(60.0f), 1.0f, 0.1f, 100.0f);
    // Viewport 1000x1000 at screen origin; click dead center.
    Ray r = ScreenPointToRay(500.0f, 500.0f, 0.0f, 0.0f, 1000.0f, 1000.0f, V, P);
    EXPECT(r.Dir.z < 0.0f);
    EXPECT(std::abs(r.Dir.x) < 1e-3f);
    EXPECT(std::abs(r.Dir.y) < 1e-3f);
    // That ray hits a unit box at (0,0,-10).
    float t = -1.0f;
    EXPECT(RayIntersectsAABB(r, {-1,-1,-11}, {1,1,-9}, t) == true);
}

int main()
{
    T00_smoke();
    T01_aabb_hit_ahead();
    T02_aabb_behind_miss();
    T03_aabb_offaxis_miss();
    T04_aabb_inside_origin();
    T05_nearest_ordering();
    T10_screen_center_ray();

    if (g_Failures == 0) { std::printf("All picking tests passed.\n"); return 0; }
    std::printf("%d picking test(s) FAILED.\n", g_Failures);
    return 1;
}
```

- [ ] **Step 2: Add the `test_picking` target** — append to `tests/CMakeLists.txt`:
```cmake
add_executable(test_picking
    test_picking.cpp
)

target_link_libraries(test_picking PRIVATE
    glm::glm
)

target_include_directories(test_picking PRIVATE
    ${CMAKE_SOURCE_DIR}/src/common/include
)

target_compile_definitions(test_picking PRIVATE
    GLM_FORCE_DEPTH_ZERO_TO_ONE
    GLM_FORCE_RIGHT_HANDED
    GLM_ENABLE_EXPERIMENTAL
)

set_target_properties(test_picking PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY "${RUNTIME_DIR}"
    FOLDER Tests
)
```
(`Picking.h` is header-only pure GLM; the test links only `glm::glm` and includes `src/common/include`, mirroring `test_frustum`.)

- [ ] **Step 3: Reconfigure + build → expect FAIL (header missing)**
```
cmake --preset msvc-win64-vs2026-community
cmake --build out/build/msvc-win64-vs2026-community --target test_picking
```
Expected: build error — cannot open `Picking.h`.

- [ ] **Step 4: Create `src/common/include/Picking.h`**
```cpp
#pragma once
#include <algorithm>
#include <cmath>
#include <limits>
#include <glm/glm.hpp>

struct Ray { glm::vec3 Origin; glm::vec3 Dir; };

// World-space ray from a click at (mouseX,mouseY) screen coords inside the Viewport image rect
// [vpMinX,vpMinY, vpW x vpH]. ImGui Y is top-down -> NDC Y flipped. Unprojects near (z=0) and
// far (z=1) — depth [0,1] (ZO) — through inverse(proj*view).
inline Ray ScreenPointToRay(float mouseX, float mouseY,
                            float vpMinX, float vpMinY, float vpW, float vpH,
                            const glm::mat4& view, const glm::mat4& proj)
{
    const float ndcX = 2.0f * (mouseX - vpMinX) / vpW - 1.0f;
    const float ndcY = 1.0f - 2.0f * (mouseY - vpMinY) / vpH;
    const glm::mat4 invVP = glm::inverse(proj * view);
    glm::vec4 nearP = invVP * glm::vec4(ndcX, ndcY, 0.0f, 1.0f);
    glm::vec4 farP  = invVP * glm::vec4(ndcX, ndcY, 1.0f, 1.0f);
    const glm::vec3 n = glm::vec3(nearP) / nearP.w;
    const glm::vec3 f = glm::vec3(farP)  / farP.w;
    Ray r;
    r.Origin = n;
    r.Dir = glm::normalize(f - n);
    return r;
}

// Slab test. Returns true and the entry distance tHit (clamped >= 0; 0 if origin is inside)
// when the ray hits the AABB. A box entirely behind the ray returns false.
inline bool RayIntersectsAABB(const Ray& r, glm::vec3 aabbMin, glm::vec3 aabbMax, float& tHit)
{
    float tmin = 0.0f;
    float tmax = std::numeric_limits<float>::max();
    for (int i = 0; i < 3; ++i)
    {
        const float o = r.Origin[i];
        const float d = r.Dir[i];
        if (std::abs(d) < 1e-8f)
        {
            if (o < aabbMin[i] || o > aabbMax[i])
                return false; // parallel to this slab and outside it
        }
        else
        {
            const float inv = 1.0f / d;
            float t1 = (aabbMin[i] - o) * inv;
            float t2 = (aabbMax[i] - o) * inv;
            if (t1 > t2) std::swap(t1, t2);
            if (t1 > tmin) tmin = t1;
            if (t2 < tmax) tmax = t2;
            if (tmin > tmax) return false;
        }
    }
    tHit = tmin;
    return true;
}
```

- [ ] **Step 5: Build + run → expect PASS**
```
cmake --build out/build/msvc-win64-vs2026-community --target test_picking
./out/build/msvc-win64-vs2026-community/bin/Debug/test_picking.exe
```
Expected: `All picking tests passed.` (exit 0).

- [ ] **Step 6: Commit**
```bash
git add src/common/include/Picking.h tests/test_picking.cpp tests/CMakeLists.txt
git commit -m "feat: add Picking.h ray/AABB math + test_picking"
```

---

### Task 3: `ViewportPicker` (editor ray-cast over entities)

**Files:**
- Create: `src/editor/src/rendering/imgui/ViewportPicker.h`
- Create: `src/editor/src/rendering/imgui/ViewportPicker.cpp`
- Modify: `src/editor/CMakeLists.txt`

- [ ] **Step 1: Create `ViewportPicker.h`**
```cpp
#pragma once
#include "ECS.h" // EntityId
struct EditorContext;

// Ray-casts the click (screen coords) against every visible mesh entity's world AABB and returns
// the nearest hit, or INVALID_ENTITY if none. Reads camera/entities/bounds/viewport-rect from ctx.
EntityId PickEntity(const EditorContext& ctx, float mouseX, float mouseY);
```

- [ ] **Step 2: Create `ViewportPicker.cpp`**
```cpp
#include "ViewportPicker.h"
#include "EditorContext.h"

#include <limits>
#include "ECS.h"
#include "Picking.h"
#include "TransformMath.h"
#include "Frustum.h"        // TransformAABB
#include "MeshSystem.h"     // GetMeshBounds

EntityId PickEntity(const EditorContext& ctx, float mouseX, float mouseY)
{
    // Pick against the SAME ECS that was rendered (ctx.World) for both the camera and the entities,
    // so the ray hits exactly what's on screen (ctx.World and ctx.WorldSnapshot can differ by a frame).
    if (!ctx.World || !ctx.MeshSys || ctx.ViewportW == 0 || ctx.ViewportH == 0)
        return INVALID_ENTITY;

    const auto* cam = ctx.World->GetSingleton<WorldCameraComponent>();
    if (!cam)
        return INVALID_ENTITY;

    const Ray ray = ScreenPointToRay(mouseX, mouseY,
                                     ctx.ViewportMinX, ctx.ViewportMinY,
                                     static_cast<float>(ctx.ViewportW), static_cast<float>(ctx.ViewportH),
                                     cam->View, cam->Projection);

    EntityId best = INVALID_ENTITY;
    float bestT = std::numeric_limits<float>::max();

    ctx.World->Each<TransformComponent, MeshComponent>(
        [&](EntityId e, const TransformComponent& t, const MeshComponent& m)
    {
        if (!m.Visible)
            return;
        const auto bounds = ctx.MeshSys->GetMeshBounds(m.MeshId);
        if (!bounds.valid)
            return;

        glm::vec3 wMin, wMax;
        TransformAABB(ModelMatrix(t), bounds.min, bounds.max, wMin, wMax);

        float tHit = 0.0f;
        if (RayIntersectsAABB(ray, wMin, wMax, tHit) && tHit < bestT)
        {
            bestT = tHit;
            best = e;
        }
    });

    return best;
}
```
(Verify against the codebase before finalizing: `ctx.World` is `const ECS*`; `ECS::GetSingleton<WorldCameraComponent>()` returns a pointer (used the same way in `MeshRenderPass`); `WorldCameraComponent` has `View`/`Projection`; `ECS::Each<...>` is `const`; `MeshSystem::GetMeshBounds` returns `{min,max,valid}`; `EditorContext` exposes `World`, `WorldSnapshot`, `MeshSys`, `ViewportMinX/MinY/W/H`. `ctx.MeshSys->GetMeshBounds` may need a non-const `MeshSystem*` — it is (`EditorContext::MeshSys` is `MeshSystem*`).)

- [ ] **Step 3: Add to editor CMake** — in `src/editor/CMakeLists.txt`, after the last `src/rendering/imgui/*.cpp` source line, add:
```cmake
    src/rendering/imgui/ViewportPicker.cpp
```

- [ ] **Step 4: Reconfigure + build editor**
```
cmake --preset msvc-win64-vs2026-community
cmake --build out/build/msvc-win64-vs2026-community --target editor
```
Expected: clean build (compiles; not yet called).

- [ ] **Step 5: Commit**
```bash
git add src/editor/src/rendering/imgui/ViewportPicker.h src/editor/src/rendering/imgui/ViewportPicker.cpp src/editor/CMakeLists.txt
git commit -m "feat: add ViewportPicker ray-cast entity selection"
```

---

### Task 4: Wire picking into the Viewport click

**Files:**
- Modify: `src/editor/src/rendering/imgui/EcsInspectorPanel.h` (public setter)
- Modify: `src/editor/src/rendering/imgui/ImGuiRenderer.cpp` (include + click handler)

- [ ] **Step 1: Add the selection setter to `EcsInspectorPanel.h`**
In the `public:` section (next to `Draw`), add:
```cpp
    void SetSelectedEntity(EntityId e) { selectedEntity = e; }
```

- [ ] **Step 2: Wire the click handler in `ImGuiRenderer::Render`**
Add `#include "ViewportPicker.h"` with the other imgui-layer includes. Then, immediately AFTER the
existing block that publishes the viewport rect into `ctx` (the lines
`ctx.ViewportDrawList = m_ViewportDrawList; ctx.ViewportMinX = ...; ... ctx.ViewportH = m_LastViewportH;`),
add:
```cpp
        // Viewport pick: left-click selects the entity under the cursor (edit mode). Skip when
        // over/using a gizmo (that click manipulates the gizmo). Empty space -> deselect.
        if (m_ViewportHovered && ImGui::IsMouseClicked(ImGuiMouseButton_Left)
            && !ImGuizmo::IsOver() && !ImGuizmo::IsUsing()) {
            m_EcsInspector.SetSelectedEntity(PickEntity(ctx, io.MousePos.x, io.MousePos.y));
        }
```
(`m_ViewportHovered` is the member set this frame in the Viewport block; `io` is the in-scope
`ImGuiIO&` already declared in `Render`; `m_EcsInspector` is the existing inspector member;
`ImGuizmo.h` is already included. `ctx.Viewport*` are set just above, so `PickEntity` reads
current values. The gizmo's own `EditTransform` runs later when the inspector draws — `IsUsing()`
reflects last frame's drag state, which is the correct guard for starting a new pick.)

- [ ] **Step 3: Build editor, run tests**
```
cmake --build out/build/msvc-win64-vs2026-community --target editor
cmake --build out/build/msvc-win64-vs2026-community --target test_ecs
cmake --build out/build/msvc-win64-vs2026-community --target test_alloc
cmake --build out/build/msvc-win64-vs2026-community --target test_picking
./out/build/msvc-win64-vs2026-community/bin/Debug/test_ecs.exe
./out/build/msvc-win64-vs2026-community/bin/Debug/test_alloc.exe
./out/build/msvc-win64-vs2026-community/bin/Debug/test_picking.exe
```
Expected: clean build; all three test exes pass.

- [ ] **Step 4: Commit**
```bash
git add src/editor/src/rendering/imgui/EcsInspectorPanel.h src/editor/src/rendering/imgui/ImGuiRenderer.cpp
git commit -m "feat: select entity by clicking it in the Viewport"
```

---

## Final verification (after all tasks)

- [ ] Full build: `cmake --build out/build/msvc-win64-vs2026-community` — all targets green.
- [ ] `test_ecs` / `test_alloc` / `test_frustum` / `test_input` / `test_picking` all print their `All ... passed.` lines.
- [ ] **GUI smoke (user-run, surface to the user — do not self-approve):** click a model in the Viewport → it's selected (Inspector highlights it, gizmo appears on it); click a different model → selection moves; click empty space → deselected (gizmo/editor gone); clicking a gizmo handle drags the gizmo (doesn't re-pick); Inspector-list selection still works; orbit the camera, then clicking still hits the right object; `runtime.exe` unaffected (renders identically — `ModelMatrix` adoption is behavior-preserving).

## Notes / non-goals
- No `GAME_API_VERSION` bump; editor/render-thread only; no ECS command (entity *manipulation* still flows through the inspector→ECSCommand path).
- Ray-vs-AABB only (bounding box, not triangles; nearest `tHit` wins). GPU entity-ID-buffer picking is the future precision upgrade. No multi-select/box-select. Play-mode (cursor-lock) clicks go to the game, not the picker.
