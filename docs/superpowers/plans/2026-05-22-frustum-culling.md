# CPU Frustum Culling Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Skip CPU submission of meshes whose world-space AABB is fully outside the camera frustum, in `MeshRenderPass` (Engine → both `editor.exe` and `runtime.exe`), with per-frame render stats and an editor cull on/off toggle.

**Architecture:** A header-only `Frustum.h` (pure GLM: plane extraction + AABB transform + visibility test, unit-tested via `test_frustum`). An Engine-exported `RenderStats`/`CullingSettings` singleton pair (cross-DLL single instance, render-thread-only, no locks). `MeshRenderPass` extracts the frustum from the VP it already renders with, gates the collection loop, and writes stats. A dedicated editor `RenderStatsPanel` shows counts + the toggle.

**Tech Stack:** C++23, GLM (depth `[0,1]`, RH), NVRHI, custom ECS, ImGui (editor only), CMake presets.

**Spec:** `docs/superpowers/specs/2026-05-22-frustum-culling-design.md`

**Conventions for every task:**
- Build preset: `msvc-win64-vs2026-community`. Build dir: `out/build/msvc-win64-vs2026-community`. Binaries: `out/build/msvc-win64-vs2026-community/bin/Debug/`.
- When a task **adds a new file to a target or a new target**, re-run `cmake --preset msvc-win64-vs2026-community` before building so CMake picks it up.
- No `GAME_API_VERSION` bump. `ecs`/`game` source unchanged.
- Commit author is the repo default (`Nuno Silva <nuno.levezinho@live.com.pt>`) — a plain `git commit` is correct. Never stage `.claude/`. Stage only the files each step names.

---

### Task 1: `Frustum.h` + `test_frustum` (pure-GLM culling math, TDD)

**Files:**
- Create: `src/engine/src/rendering/Frustum.h`
- Create: `tests/test_frustum.cpp`
- Modify: `tests/CMakeLists.txt` (append a `test_frustum` target after the `test_alloc` block, currently ends at line 39)

- [ ] **Step 1: Write the failing test**

Create `tests/test_frustum.cpp`. Self-contained (no `lib.h`, no Engine link — `EXPECT` prints via `fprintf`):

```cpp
#include <cstdio>
#include <cmath>
#include <glm/glm.hpp>
#include <glm/gtc/matrix_transform.hpp>

#include "Frustum.h"

static int g_Failures = 0;
#define EXPECT(cond)                                                     \
    do {                                                                 \
        if (!(cond)) {                                                   \
            std::fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); \
            ++g_Failures;                                                \
        }                                                                \
    } while (0)

// Camera at origin looking down -Z, RH; 60-deg vertical FOV, square aspect, near 0.1 far 100.
static glm::mat4 TestVP()
{
    const glm::mat4 V = glm::lookAtRH(glm::vec3(0, 0, 0), glm::vec3(0, 0, -1), glm::vec3(0, 1, 0));
    const glm::mat4 P = glm::perspectiveRH_ZO(glm::radians(60.0f), 1.0f, 0.1f, 100.0f);
    return P * V;
}

// Small AABB centered at c with half-size h.
static void Box(glm::vec3 c, float h, glm::vec3& mn, glm::vec3& mx)
{
    mn = c - glm::vec3(h);
    mx = c + glm::vec3(h);
}

static void T00_smoke() { EXPECT(1 + 1 == 2); }

static void T01_point_ahead_visible()
{
    const Frustum f = ExtractFrustum(TestVP());
    glm::vec3 mn, mx; Box({0, 0, -10}, 0.1f, mn, mx);
    EXPECT(IsAABBVisible(f, mn, mx) == true);
}

static void T02_behind_camera_culled()
{
    const Frustum f = ExtractFrustum(TestVP());
    glm::vec3 mn, mx; Box({0, 0, 10}, 0.1f, mn, mx); // +Z is behind the camera
    EXPECT(IsAABBVisible(f, mn, mx) == false);
}

static void T03_beyond_far_culled()
{
    const Frustum f = ExtractFrustum(TestVP());
    glm::vec3 mn, mx; Box({0, 0, -200}, 0.1f, mn, mx); // far plane is 100
    EXPECT(IsAABBVisible(f, mn, mx) == false);
}

static void T04_left_right_planes()
{
    const Frustum f = ExtractFrustum(TestVP());
    glm::vec3 mn, mx;
    // At z=-10, half-width = 10*tan(30deg) ~= 5.77. x=5 inside, x=8 outside.
    Box({5, 0, -10}, 0.1f, mn, mx);  EXPECT(IsAABBVisible(f, mn, mx) == true);
    Box({8, 0, -10}, 0.1f, mn, mx);  EXPECT(IsAABBVisible(f, mn, mx) == false);
    Box({-8, 0, -10}, 0.1f, mn, mx); EXPECT(IsAABBVisible(f, mn, mx) == false);
}

static void T05_top_bottom_planes()
{
    const Frustum f = ExtractFrustum(TestVP());
    glm::vec3 mn, mx;
    Box({0, 8, -10}, 0.1f, mn, mx);  EXPECT(IsAABBVisible(f, mn, mx) == false);
    Box({0, -8, -10}, 0.1f, mn, mx); EXPECT(IsAABBVisible(f, mn, mx) == false);
}

static void T06_straddling_visible()
{
    const Frustum f = ExtractFrustum(TestVP());
    glm::vec3 mn, mx; Box({0, 0, -10}, 4.0f, mn, mx); // big box overlapping the view
    EXPECT(IsAABBVisible(f, mn, mx) == true);
}

static void T10_transform_identity()
{
    glm::vec3 mn, mx;
    TransformAABB(glm::mat4(1.0f), {-1, -2, -3}, {1, 2, 3}, mn, mx);
    EXPECT(std::abs(mn.x + 1) < 1e-5f && std::abs(mn.y + 2) < 1e-5f && std::abs(mn.z + 3) < 1e-5f);
    EXPECT(std::abs(mx.x - 1) < 1e-5f && std::abs(mx.y - 2) < 1e-5f && std::abs(mx.z - 3) < 1e-5f);
}

static void T11_transform_translate_into_view()
{
    const Frustum f = ExtractFrustum(TestVP());
    const glm::mat4 m = glm::translate(glm::mat4(1.0f), {0, 0, -10});
    glm::vec3 mn, mx;
    TransformAABB(m, {-1, -1, -1}, {1, 1, 1}, mn, mx);
    EXPECT(IsAABBVisible(f, mn, mx) == true);
}

static void T12_transform_translate_behind()
{
    const Frustum f = ExtractFrustum(TestVP());
    const glm::mat4 m = glm::translate(glm::mat4(1.0f), {0, 0, 50}); // behind camera
    glm::vec3 mn, mx;
    TransformAABB(m, {-1, -1, -1}, {1, 1, 1}, mn, mx);
    EXPECT(IsAABBVisible(f, mn, mx) == false);
}

static void T13_transform_rotate_scale_enlarges()
{
    // Unit cube, scale 2, rotate 45deg about Z. World extent per axis grows past the
    // local half-extent (1) -> abs-3x3 method produced an enlarged world AABB.
    glm::mat4 m = glm::rotate(glm::mat4(1.0f), glm::radians(45.0f), {0, 0, 1});
    m = m * glm::scale(glm::mat4(1.0f), glm::vec3(2.0f));
    glm::vec3 mn, mx;
    TransformAABB(m, {-1, -1, -1}, {1, 1, 1}, mn, mx);
    EXPECT((mx.x - mn.x) > 4.0f); // 2*2*(cos45+sin45) ~= 5.66 > local-scaled 4
    EXPECT((mx.z - mn.z) > 3.9f); // z just scaled by 2 -> ~4
}

int main()
{
    T00_smoke();
    T01_point_ahead_visible();
    T02_behind_camera_culled();
    T03_beyond_far_culled();
    T04_left_right_planes();
    T05_top_bottom_planes();
    T06_straddling_visible();
    T10_transform_identity();
    T11_transform_translate_into_view();
    T12_transform_translate_behind();
    T13_transform_rotate_scale_enlarges();

    if (g_Failures == 0) { std::printf("All frustum tests passed.\n"); return 0; }
    std::printf("%d frustum test(s) FAILED.\n", g_Failures);
    return 1;
}
```

- [ ] **Step 2: Add the `test_frustum` target**

Append to `tests/CMakeLists.txt` (after the `test_alloc` block that ends at line 39):

```cmake
add_executable(test_frustum
    test_frustum.cpp
)

target_link_libraries(test_frustum PRIVATE
    glm::glm
)

target_include_directories(test_frustum PRIVATE
    ${CMAKE_SOURCE_DIR}/src/engine/src/rendering
)

target_compile_definitions(test_frustum PRIVATE
    GLM_FORCE_DEPTH_ZERO_TO_ONE
    GLM_FORCE_RIGHT_HANDED
    GLM_ENABLE_EXPERIMENTAL
)

set_target_properties(test_frustum PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY "${RUNTIME_DIR}"
    FOLDER Tests
)
```

- [ ] **Step 3: Reconfigure + build to verify it FAILS to compile**

Run:
```
cmake --preset msvc-win64-vs2026-community
cmake --build out/build/msvc-win64-vs2026-community --target test_frustum
```
Expected: **build error** — `Frustum.h` not found / `ExtractFrustum` etc. undefined.

- [ ] **Step 4: Implement `Frustum.h`**

Create `src/engine/src/rendering/Frustum.h`:

```cpp
#pragma once
#include <cmath>
#include <glm/glm.hpp>

// Six frustum planes, each (n.x, n.y, n.z, d) with an inward-pointing unit normal:
// a point p is inside the half-space iff dot(n, p) + d >= 0.
struct Frustum { glm::vec4 Planes[6]; };

// Gribb-Hartmann extraction from a GLM (column-major) view-projection where clip = VP * v.
// rowI(m) = vec4(m[0][I], m[1][I], m[2][I], m[3][I]) (0-based). Depth-[0,1] (ZO) variant:
//   left   = row3 + row0     right = row3 - row0
//   bottom = row3 + row1     top   = row3 - row1
//   near   = row2            far   = row3 - row2
// near = row2 is the ZO-specific bit (the OpenGL [-1,1] form is row3 + row2).
inline Frustum ExtractFrustum(const glm::mat4& m)
{
    const glm::vec4 row0(m[0][0], m[1][0], m[2][0], m[3][0]);
    const glm::vec4 row1(m[0][1], m[1][1], m[2][1], m[3][1]);
    const glm::vec4 row2(m[0][2], m[1][2], m[2][2], m[3][2]);
    const glm::vec4 row3(m[0][3], m[1][3], m[2][3], m[3][3]);

    Frustum f;
    f.Planes[0] = row3 + row0; // left
    f.Planes[1] = row3 - row0; // right
    f.Planes[2] = row3 + row1; // bottom
    f.Planes[3] = row3 - row1; // top
    f.Planes[4] = row2;        // near
    f.Planes[5] = row3 - row2; // far

    for (glm::vec4& p : f.Planes)
    {
        const float len = glm::length(glm::vec3(p));
        if (len > 0.0f)
            p /= len;
    }
    return f;
}

// Transform a local-space AABB by m into a (looser) world-space AABB. Center+extents
// method: world center = m * center; world extent_i = sum_j |R[i][j]| * localExtent_j,
// where R is the upper-left 3x3 (R[i][j] math = a[j][i] in column-major GLM). No 8-corner loop.
inline void TransformAABB(const glm::mat4& m, glm::vec3 localMin, glm::vec3 localMax,
                          glm::vec3& outMin, glm::vec3& outMax)
{
    const glm::vec3 center  = 0.5f * (localMin + localMax);
    const glm::vec3 extents = 0.5f * (localMax - localMin);
    const glm::vec3 wCenter = glm::vec3(m * glm::vec4(center, 1.0f));
    const glm::mat3 a(m);
    glm::vec3 wExtents;
    for (int k = 0; k < 3; ++k)
        wExtents[k] = std::abs(a[0][k]) * extents.x
                    + std::abs(a[1][k]) * extents.y
                    + std::abs(a[2][k]) * extents.z;
    outMin = wCenter - wExtents;
    outMax = wCenter + wExtents;
}

// p-vertex test: the AABB is fully outside iff its positive vertex (the corner farthest
// along a plane's normal) is behind that plane. Returns true if possibly visible.
inline bool IsAABBVisible(const Frustum& f, glm::vec3 worldMin, glm::vec3 worldMax)
{
    for (const glm::vec4& plane : f.Planes)
    {
        const glm::vec3 n(plane);
        const glm::vec3 p(
            n.x >= 0.0f ? worldMax.x : worldMin.x,
            n.y >= 0.0f ? worldMax.y : worldMin.y,
            n.z >= 0.0f ? worldMax.z : worldMin.z);
        if (glm::dot(n, p) + plane.w < 0.0f)
            return false;
    }
    return true;
}
```

- [ ] **Step 5: Build + run to verify PASS**

Run:
```
cmake --build out/build/msvc-win64-vs2026-community --target test_frustum
./out/build/msvc-win64-vs2026-community/bin/Debug/test_frustum.exe
```
Expected: `All frustum tests passed.` (exit 0).

- [ ] **Step 6: Commit**

```bash
git add src/engine/src/rendering/Frustum.h tests/test_frustum.cpp tests/CMakeLists.txt
git commit -m "feat: add frustum culling math (Frustum.h) + test_frustum"
```

---

### Task 2: `RenderStats` Engine-exported singleton

**Files:**
- Create: `src/engine/src/rendering/RenderStats.h`
- Create: `src/engine/src/rendering/RenderStats.cpp`
- Modify: `src/engine/CMakeLists.txt:23` (add `src/rendering/RenderStats.cpp` after `src/rendering/StagingBufferPool.cpp`)

- [ ] **Step 1: Create `RenderStats.h`**

```cpp
#pragma once
#include <cstdint>

#include "Engine.h"

// Per-frame mesh render counters, written by MeshRenderPass once at the end of each frame.
struct RenderStats {
    uint32_t MeshEntitiesTotal  = 0; // entities with Visible==true considered this frame
    uint32_t MeshEntitiesDrawn  = 0; // entries actually submitted (Total - Culled)
    uint32_t MeshEntitiesCulled = 0; // rejected by the frustum test
    uint32_t InstancesDrawn     = 0; // sum of per-batch instance counts emitted
    uint32_t BatchesDrawn       = 0; // draw batches (runs) issued
};

struct CullingSettings { bool Enabled = true; };

// Single instances DEFINED in RenderStats.cpp and exported from Engine.dll so the mesh pass
// (Engine.dll) and the editor panel (editor.exe) share ONE copy each. A header-inline
// function-local static would give every module its own copy (the staging-pool bug).
// Both are touched only on the RenderThread (mesh pass writes stats / reads the toggle;
// the ImGui overlay later in the same frame reads stats / writes the toggle) -> no locks.
ENGINE_API RenderStats&     GetRenderStats();
ENGINE_API CullingSettings& GetCullingSettings();
```

- [ ] **Step 2: Create `RenderStats.cpp`**

```cpp
#include "RenderStats.h"

RenderStats& GetRenderStats()
{
    static RenderStats s;
    return s;
}

CullingSettings& GetCullingSettings()
{
    static CullingSettings s;
    return s;
}
```

- [ ] **Step 3: Add the source to the Engine target**

In `src/engine/CMakeLists.txt`, after line 23 (`src/rendering/StagingBufferPool.cpp`) add:
```cmake
    src/rendering/RenderStats.cpp
```

- [ ] **Step 4: Reconfigure + build the Engine target**

Run:
```
cmake --preset msvc-win64-vs2026-community
cmake --build out/build/msvc-win64-vs2026-community --target Engine
```
Expected: builds clean.

- [ ] **Step 5: Verify the symbols are exported once from Engine.dll**

Run (Developer PowerShell, or any shell with `dumpbin` on PATH):
```
dumpbin /exports out/build/msvc-win64-vs2026-community/bin/Debug/Engine.dll | findstr GetRenderStats
dumpbin /exports out/build/msvc-win64-vs2026-community/bin/Debug/Engine.dll | findstr GetCullingSettings
```
Expected: one matching exported entry for each (mangled name containing `GetRenderStats` / `GetCullingSettings`). If `dumpbin` is unavailable, skip this step — Task 4's panel build + the GUI smoke confirm the single-instance wiring.

- [ ] **Step 6: Commit**

```bash
git add src/engine/src/rendering/RenderStats.h src/engine/src/rendering/RenderStats.cpp src/engine/CMakeLists.txt
git commit -m "feat: add Engine-exported RenderStats + CullingSettings singletons"
```

---

### Task 3: Wire culling + stats into `MeshRenderPass`

**Files:**
- Modify: `src/engine/src/rendering/passes/MeshRenderPass.cpp` (includes; new file-local `BuildWorldMatrix`; frustum extract after `:362`; cull gate in the collection lambda `:381-391`; reuse helper at `:453-459`; stats write near the end of `Render()`)

No new unit test (this is ECS+renderer integration). Verification = Engine/editor/runtime build green, `test_ecs`/`test_alloc` regression green, plus the user GUI smoke at the end.

- [ ] **Step 1: Add includes**

Near the top of `src/engine/src/rendering/passes/MeshRenderPass.cpp`, with the other rendering includes, add:
```cpp
#include "Frustum.h"
#include "RenderStats.h"
```
(Both headers live in `src/engine/src/rendering`, the parent of this `passes/` dir. The pass already includes sibling rendering headers, e.g. `MeshSystem`/`MaterialSystem`; if the include does not resolve, use `#include "../Frustum.h"` / `#include "../RenderStats.h"` to match the existing relative-include style in this file.)

- [ ] **Step 2: Add the file-local `BuildWorldMatrix` helper**

In the anonymous/file-static region near the top of the `.cpp` (above the `MeshRenderPass::Render` definition; same place as other file-local helpers like `BuildBatchRuns`), add:
```cpp
static glm::mat4 BuildWorldMatrix(const TransformComponent& t)
{
    glm::mat4 T  = glm::translate(glm::mat4(1.0f), t.Position);
    glm::mat4 Rx = glm::rotate(glm::mat4(1.0f), t.Rotation.x, glm::vec3(1.f, 0.f, 0.f));
    glm::mat4 Ry = glm::rotate(glm::mat4(1.0f), t.Rotation.y, glm::vec3(0.f, 1.f, 0.f));
    glm::mat4 Rz = glm::rotate(glm::mat4(1.0f), t.Rotation.z, glm::vec3(0.f, 0.f, 1.f));
    glm::mat4 S  = glm::scale(glm::mat4(1.0f), t.Scale);
    return T * Rz * Ry * Rx * S;
}
```

- [ ] **Step 3: Reuse the helper in the instance loop**

Replace the inline matrix block at `MeshRenderPass.cpp:453-459`:
```cpp
                // Build world transform: T * Rz * Ry * Rx * S
                glm::mat4 T = glm::translate(glm::mat4(1.0f), transform->Position);
                glm::mat4 Rx = glm::rotate(glm::mat4(1.0f), transform->Rotation.x, glm::vec3(1.f, 0.f, 0.f));
                glm::mat4 Ry = glm::rotate(glm::mat4(1.0f), transform->Rotation.y, glm::vec3(0.f, 1.f, 0.f));
                glm::mat4 Rz = glm::rotate(glm::mat4(1.0f), transform->Rotation.z, glm::vec3(0.f, 0.f, 1.f));
                glm::mat4 S = glm::scale(glm::mat4(1.0f), transform->Scale);
                glm::mat4 M = T * Rz * Ry * Rx * S;
```
with:
```cpp
                // Build world transform (shared with the cull test so they cannot diverge)
                glm::mat4 M = BuildWorldMatrix(*transform);
```

- [ ] **Step 4: Extract the frustum + read the toggle (once per frame)**

Immediately after `perFrame.VP = P * V;` (`MeshRenderPass.cpp:362`), add:
```cpp
        const Frustum cullFrustum = ExtractFrustum(perFrame.VP);
        const bool cullEnabled = GetCullingSettings().Enabled;
        MeshSystem* meshSystem = m_Renderer->GetMeshSystem();
        uint32_t culledCount = 0;
```
(`m_Renderer->GetMeshSystem()` is the same accessor used at `:419`. If its return type name differs, use `auto* meshSystem = m_Renderer->GetMeshSystem();`.)

- [ ] **Step 5: Add the cull gate to the collection lambda**

Replace the collection lambda body at `MeshRenderPass.cpp:381-391`:
```cpp
            world->Each<TransformComponent, MeshComponent>(
                [&](EntityId e, const TransformComponent&, const MeshComponent& meshComp)
            {
                if (!meshComp.Visible)
                    return;

                const auto* materialComp = world->GetComponent<MaterialComponent>(e);
                uint32_t materialId = materialComp ? materialComp->MaterialId : MaterialSystem::MissingMaterial;

                entries[entryCount++] = BatchEntry{ meshComp.MeshId, materialId, e };
            });
```
with (note the now-named `transform` param):
```cpp
            world->Each<TransformComponent, MeshComponent>(
                [&](EntityId e, const TransformComponent& transform, const MeshComponent& meshComp)
            {
                if (!meshComp.Visible)
                    return;

                if (cullEnabled && meshSystem)
                {
                    const auto bounds = meshSystem->GetMeshBounds(meshComp.MeshId);
                    if (bounds.valid) // invalid/unloaded bounds -> never cull
                    {
                        glm::vec3 wMin, wMax;
                        TransformAABB(BuildWorldMatrix(transform), bounds.min, bounds.max, wMin, wMax);
                        if (!IsAABBVisible(cullFrustum, wMin, wMax))
                        {
                            ++culledCount;
                            return;
                        }
                    }
                }

                const auto* materialComp = world->GetComponent<MaterialComponent>(e);
                uint32_t materialId = materialComp ? materialComp->MaterialId : MaterialSystem::MissingMaterial;

                entries[entryCount++] = BatchEntry{ meshComp.MeshId, materialId, e };
            });
```

- [ ] **Step 6: Accumulate instance/batch counts in the render-run loop**

Before the `for (uint32_t r = 0; r < runCount; ++r)` loop (`:413`), add an accumulator:
```cpp
        uint32_t instancesDrawn = 0;
```
Inside that loop, after the instances for a run have been built and the draw is issued (i.e. after `instanceOut` is final for the run — right after the existing `if (instanceOut == 0) { ... continue; }` guard near `:477-479`), add:
```cpp
            instancesDrawn += instanceOut;
```
(`runCount` already holds the batch count; no separate counter needed.)

- [ ] **Step 7: Write the stats once at the end of `Render()`**

At the end of the `MeshRenderPass::Render` body, after the run loop and before the function returns, add:
```cpp
        RenderStats& rs = GetRenderStats();
        rs.MeshEntitiesDrawn  = entryCount;
        rs.MeshEntitiesCulled = culledCount;
        rs.MeshEntitiesTotal  = entryCount + culledCount;
        rs.InstancesDrawn     = instancesDrawn;
        rs.BatchesDrawn       = runCount;
```

- [ ] **Step 8: Build Engine + runtime, run regression tests**

Run:
```
cmake --build out/build/msvc-win64-vs2026-community --target Engine
cmake --build out/build/msvc-win64-vs2026-community --target runtime
cmake --build out/build/msvc-win64-vs2026-community --target test_ecs
cmake --build out/build/msvc-win64-vs2026-community --target test_alloc
./out/build/msvc-win64-vs2026-community/bin/Debug/test_ecs.exe
./out/build/msvc-win64-vs2026-community/bin/Debug/test_alloc.exe
```
Expected: all build clean; `All ECS tests passed.` and `All allocator tests passed.`

- [ ] **Step 9: Commit**

```bash
git add src/engine/src/rendering/passes/MeshRenderPass.cpp
git commit -m "feat: frustum-cull mesh entities + record render stats in MeshRenderPass"
```

---

### Task 4: `RenderStatsPanel` (editor) + wiring

**Files:**
- Create: `src/editor/src/rendering/imgui/RenderStatsPanel.h`
- Create: `src/editor/src/rendering/imgui/RenderStatsPanel.cpp`
- Modify: `src/editor/src/rendering/imgui/ImGuiRenderer.cpp` (`#include` near `:22`; draw call near `:481-482`)
- Modify: `src/editor/CMakeLists.txt:11` (add `src/rendering/imgui/RenderStatsPanel.cpp` after `MemoryPanel.cpp`)

- [ ] **Step 1: Create `RenderStatsPanel.h`**

```cpp
#pragma once

// Draws the "Render Stats" debug window: per-frame mesh draw/cull counters and the
// frustum-culling on/off toggle. `open` may be null (always draw) or point to a toggle bool.
void DrawRenderStatsPanel(bool* open);
```

- [ ] **Step 2: Create `RenderStatsPanel.cpp`**

```cpp
#include "RenderStatsPanel.h"

#include <imgui.h>

#include "RenderStats.h" // resolved via the editor's Engine PUBLIC include dirs (same as StagingBufferPool.h in MemoryPanel)

void DrawRenderStatsPanel(bool* open)
{
    if (open && !*open) return;
    if (!ImGui::Begin("Render Stats", open)) { ImGui::End(); return; }

    ImGui::Checkbox("Frustum culling", &GetCullingSettings().Enabled);
    ImGui::Separator();

    const RenderStats& s = GetRenderStats();
    ImGui::Text("Mesh entities: %u", s.MeshEntitiesTotal);
    ImGui::Text("  drawn:  %u", s.MeshEntitiesDrawn);
    ImGui::Text("  culled: %u", s.MeshEntitiesCulled);
    ImGui::Text("Instances: %u", s.InstancesDrawn);
    ImGui::Text("Batches:   %u", s.BatchesDrawn);

    ImGui::End();
}
```

- [ ] **Step 3: Wire into `ImGuiRenderer.cpp`**

Add the include next to the Memory panel include (`ImGuiRenderer.cpp:22`):
```cpp
#include "RenderStatsPanel.h"
```
Add the draw call right after the `DrawMemoryPanel(...)` call (`:481-482`):
```cpp
        static bool s_ShowRenderStatsPanel = true;
        DrawRenderStatsPanel(&s_ShowRenderStatsPanel);
```

- [ ] **Step 4: Add the panel source to the editor target**

In `src/editor/CMakeLists.txt`, after line 11 (`src/rendering/imgui/MemoryPanel.cpp`) add:
```cmake
    src/rendering/imgui/RenderStatsPanel.cpp
```

- [ ] **Step 5: Reconfigure + build the editor**

Run:
```
cmake --preset msvc-win64-vs2026-community
cmake --build out/build/msvc-win64-vs2026-community --target editor
```
Expected: builds clean.

- [ ] **Step 6: Commit**

```bash
git add src/editor/src/rendering/imgui/RenderStatsPanel.h src/editor/src/rendering/imgui/RenderStatsPanel.cpp src/editor/src/rendering/imgui/ImGuiRenderer.cpp src/editor/CMakeLists.txt
git commit -m "feat: add editor Render Stats panel with culling toggle"
```

---

## Final verification (after all tasks)

- [ ] Full build: `cmake --build out/build/msvc-win64-vs2026-community` — all targets (`Engine`, `ecs`, `game`, `editor`, `runtime`, `test_ecs`, `test_alloc`, `test_frustum`) green.
- [ ] `test_frustum`, `test_ecs`, `test_alloc` all print their `All ... passed.` line.
- [ ] **GUI smoke (user-run, surface this to the user — do not self-approve):** launch the editor, open the **Render Stats** panel. Pan/rotate the camera so models leave the view → `culled` rises, `drawn` falls, and **on-screen meshes never disappear**. Toggle **Frustum culling** off → `culled` drops to 0, `drawn` jumps to the total, scene unchanged. Launch `runtime.exe` → scene renders identically to before (culling silently active, no panel).

## Notes / non-goals
- No `GAME_API_VERSION` bump; no ECS component, `GameState`, or export-layout change. No ecs.dll rebuild or forced editor restart beyond the normal Engine/editor rebuild.
- Not implemented (YAGNI): hierarchical/BVH culling, frozen-frustum debug mode, caching the world AABB in `BatchEntry` (revisit only if `GetMeshBounds` profiles hot).
