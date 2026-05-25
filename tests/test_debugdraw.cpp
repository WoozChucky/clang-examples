#include <cstdio>
#include <cmath>
#include <vector>
#include <glm/glm.hpp>

#include "DebugDraw.h"

static int g_Failures = 0;
#define EXPECT(cond)                                                     \
    do {                                                                 \
        if (!(cond)) {                                                   \
            std::fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); \
            ++g_Failures;                                                \
        }                                                                \
    } while (0)

static bool hasVert(const std::vector<DebugVertex>& v, glm::vec3 p) {
    for (auto& x : v) if (std::abs(x.Position.x-p.x)<1e-4f && std::abs(x.Position.y-p.y)<1e-4f
                          && std::abs(x.Position.z-p.z)<1e-4f) return true;
    return false;
}

static void T00_line()
{
    std::vector<DebugVertex> v;
    DebugAppendLine(v, {0,0,0}, {1,2,3}, {1,1,1,1});
    EXPECT(v.size() == 2);
    EXPECT(v[0].Position == glm::vec3(0,0,0));
    EXPECT(v[1].Position == glm::vec3(1,2,3));
    EXPECT(v[0].Color == glm::vec4(1,1,1,1));
}

static void T01_box_24_verts_all_corners()
{
    std::vector<DebugVertex> v;
    DebugAppendBox(v, {-1,-1,-1}, {1,1,1}, {0,1,0,1});
    EXPECT(v.size() == 24);
    EXPECT(hasVert(v, {-1,-1,-1}));
    EXPECT(hasVert(v, { 1, 1, 1}));
    EXPECT(hasVert(v, { 1,-1, 1}));
    EXPECT(v[0].Color == glm::vec4(0,1,0,1));
}

static void T02_sphere_segment_count()
{
    std::vector<DebugVertex> v;
    DebugAppendSphere(v, {0,0,0}, 2.0f, {1,0,0,1}, 8);
    EXPECT(v.size() == static_cast<size_t>(3 * 8 * 2));
}

static void T03_frustum_identity_is_ndc_cube()
{
    std::vector<DebugVertex> v;
    DebugAppendFrustum(v, glm::mat4(1.0f), {1,1,0,1});
    EXPECT(v.size() == 24);
    EXPECT(hasVert(v, {-1,-1,0}));
    EXPECT(hasVert(v, { 1, 1,1}));
}

// Expected vertex count: lines per direction = 2*floor(halfExtent/step)+1, two
// directions, 2 verts per line.
static size_t gridExpectedVerts(float halfExtent, float step) {
    const int n = static_cast<int>(halfExtent / step);
    const int linesPerDir = 2 * n + 1;
    return static_cast<size_t>(2 * linesPerDir * 2);
}

// Return the color of the first vertex at position p (or a sentinel if absent).
static glm::vec4 colorAt(const std::vector<DebugVertex>& v, glm::vec3 p) {
    for (auto& x : v)
        if (std::abs(x.Position.x-p.x)<1e-4f && std::abs(x.Position.y-p.y)<1e-4f
            && std::abs(x.Position.z-p.z)<1e-4f) return x.Color;
    return glm::vec4(-1.0f);
}

static void T04_grid_vertex_count()
{
    std::vector<DebugVertex> v;
    DebugAppendGrid(v, {0,0,0}, 5.0f, 1.0f, {0.35f,0.35f,0.35f,1}, {1,0,0,1}, {0,0,1,1});
    EXPECT(v.size() == gridExpectedVerts(5.0f, 1.0f)); // 2 * (2*5+1) * 2 = 44
    // All grid verts are on Y=0.
    bool allY0 = true;
    for (auto& x : v) if (std::abs(x.Position.y) > 1e-5f) allY0 = false;
    EXPECT(allY0);
}

static void T05_grid_camera_snapped()
{
    std::vector<DebugVertex> v;
    // Camera at non-grid-aligned XZ; step 1 => snapped center = (round(3.4), round(-7.8)) = (3, -8).
    DebugAppendGrid(v, {3.4f, 2.0f, -7.8f}, 5.0f, 1.0f, {0.35f,0.35f,0.35f,1}, {1,0,0,1}, {0,0,1,1});
    // Patch spans X in [3-5, 3+5] = [-2, 8], Z in [-8-5, -8+5] = [-13, -3].
    // Min-X line (gx=-2) spans Z [-13,-3]: its endpoints exist.
    EXPECT(hasVert(v, {-2.0f, 0.0f, -3.0f}));
    EXPECT(hasVert(v, { 8.0f, 0.0f, -3.0f}));
    // Unsnapped raw min-X (3.4-5 = -1.6) must NOT be a line position.
    EXPECT(!hasVert(v, {-1.6f, 0.0f, -3.0f}));
}

static void T06_grid_axis_colors()
{
    const glm::vec4 grid{0.35f,0.35f,0.35f,1}, xCol{1,0,0,1}, zCol{0,0,1,1};
    std::vector<DebugVertex> v;
    DebugAppendGrid(v, {0,0,0}, 5.0f, 1.0f, grid, xCol, zCol);
    // World Z axis runs along X==0 -> that line uses axisZColor. Endpoint (0,0,5).
    EXPECT(colorAt(v, {0.0f, 0.0f, 5.0f}) == zCol);
    // World X axis runs along Z==0 -> that line uses axisXColor. Endpoint (5,0,0).
    EXPECT(colorAt(v, {5.0f, 0.0f, 0.0f}) == xCol);
    // An off-axis grid line (gx=2) uses gridColor. Endpoint (2,0,5).
    EXPECT(colorAt(v, {2.0f, 0.0f, 5.0f}) == grid);
}

int main()
{
    T00_line();
    T01_box_24_verts_all_corners();
    T02_sphere_segment_count();
    T03_frustum_identity_is_ndc_cube();
    T04_grid_vertex_count();
    T05_grid_camera_snapped();
    T06_grid_axis_colors();

    if (g_Failures == 0) { std::printf("All debug draw tests passed.\n"); return 0; }
    std::printf("%d debug draw test(s) FAILED.\n", g_Failures);
    return 1;
}
