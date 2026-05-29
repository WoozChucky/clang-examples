#include <cstdio>
#include <cmath>
#include <glm/glm.hpp>
#include <glm/gtc/matrix_transform.hpp>

#include "ShadowMath.h" // ComputeLightViewProj, FrustumSliceSphere, SnapToTexelGrid

static int g_Failures = 0;
#define EXPECT(cond)                                                     \
    do {                                                                 \
        if (!(cond)) {                                                   \
            std::fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); \
            ++g_Failures;                                                \
        }                                                                \
    } while (0)

static bool near(float a, float b, float e = 1e-3f) { return std::fabs(a - b) < e; }

// nearExtend defaults to 0 and reproduces the pre-change matrix exactly (regression guard).
static void T00_nearextend_zero_is_identity_change()
{
    const glm::vec3 c(5, 2, -3); const float r = 10.0f; const glm::vec3 sun = glm::normalize(glm::vec3(0.3f, -1.0f, 0.2f));
    const glm::mat4 a = ComputeLightViewProj(c, r, sun);
    const glm::mat4 b = ComputeLightViewProj(c, r, sun, 0.0f);
    for (int i = 0; i < 4; ++i) for (int j = 0; j < 4; ++j) EXPECT(near(a[i][j], b[i][j]));
}

// A point just beyond the old near plane (further toward the sun) is OUTSIDE [0,1] depth at
// nearExtend=0 but INSIDE once nearExtend pulls the near plane back.
static void T01_nearextend_captures_caster_toward_sun()
{
    const glm::vec3 c(0, 0, 0); const float r = 10.0f; const glm::vec3 sun = glm::normalize(glm::vec3(0, -1, 0));
    const glm::vec4 caster(0, 25, 0, 1);
    auto depthInRange = [&](const glm::mat4& vp) {
        glm::vec4 cl = vp * caster; cl /= cl.w; return cl.z >= 0.0f && cl.z <= 1.0f;
    };
    EXPECT(!depthInRange(ComputeLightViewProj(c, r, sun, 0.0f)));   // clipped without extend
    EXPECT( depthInRange(ComputeLightViewProj(c, r, sun, 20.0f)));  // captured with extend
}

// FrustumSliceSphere: smaller ShadowDistance -> smaller sphere; positive radius; loose containment.
static void T02_frustum_slice_sphere()
{
    const glm::mat4 view = glm::lookAtRH(glm::vec3(0, 10, 20), glm::vec3(0, 0, 0), glm::vec3(0, 1, 0));
    const glm::mat4 proj = glm::perspectiveRH_ZO(glm::radians(45.0f), 16.0f / 9.0f, 0.1f, 1000.0f);
    const ShadowSphere s80 = FrustumSliceSphere(view, proj, 80.0f);
    const ShadowSphere s40 = FrustumSliceSphere(view, proj, 40.0f);
    EXPECT(s40.radius < s80.radius);
    EXPECT(s80.radius > 0.0f);
    EXPECT(s80.radius >= glm::length(s80.center - glm::vec3(0, 10, 20)) - 80.0f - 1.0f); // loose sanity
}

// SnapToTexelGrid: idempotent; output within one texel of input in the snap plane (sun +Y -> x/z).
static void T03_snap_to_texel_grid()
{
    const glm::vec3 sun = glm::normalize(glm::vec3(0, -1, 0));
    const float r = 10.0f; const uint32_t mapSize = 2048;
    const float texel = (2.0f * r) / mapSize;
    const glm::vec3 base(100.0f, 0.0f, 50.0f);
    const glm::vec3 snapped = SnapToTexelGrid(base, r, sun, mapSize);
    const glm::vec3 snapped2 = SnapToTexelGrid(snapped, r, sun, mapSize);
    EXPECT(near(snapped.x, snapped2.x) && near(snapped.z, snapped2.z));
    EXPECT(std::fabs(snapped.x - base.x) <= texel);
    EXPECT(std::fabs(snapped.z - base.z) <= texel);
}

int main()
{
    T00_nearextend_zero_is_identity_change();
    T01_nearextend_captures_caster_toward_sun();
    T02_frustum_slice_sphere();
    T03_snap_to_texel_grid();
    if (g_Failures == 0) { std::printf("All shadow-math tests passed.\n"); return 0; }
    std::fprintf(stderr, "%d shadow-math test(s) failed.\n", g_Failures);
    return 1;
}
