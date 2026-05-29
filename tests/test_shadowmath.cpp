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

static glm::vec3 proj_ndc(const glm::mat4& vp, const glm::vec3& p) {
    glm::vec4 c = vp * glm::vec4(p, 1.0f);
    return glm::vec3(c) / c.w;
}

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

static void T04_sun_up_gate()
{
    EXPECT(IsSunUp(glm::vec3(0, -1, 0)));
    EXPECT(IsSunUp(glm::normalize(glm::vec3(0, -0.5f, 0.5f))));
    EXPECT(!IsSunUp(glm::vec3(0, 1, 0)));
    EXPECT(!IsSunUp(glm::vec3(0, 0, 1)));
}

static void T05_center_projects_to_ndc_origin()
{
    const glm::vec3 center(3, 1, -2);
    const glm::mat4 vp = ComputeLightViewProj(center, 5.0f, glm::vec3(0, -1, 0));
    const glm::vec3 n = proj_ndc(vp, center);
    EXPECT(std::abs(n.x) < 1e-3f);
    EXPECT(std::abs(n.y) < 1e-3f);
    EXPECT(n.z > 0.0f && n.z < 1.0f);
}

static void T06_scene_fits_inside_light_frustum()
{
    const glm::vec3 center(0, 0, 0);
    const float r = 4.0f;
    const glm::mat4 vp = ComputeLightViewProj(center, r, glm::normalize(glm::vec3(0.3f, -1, 0.2f)));
    const float h = r / std::sqrt(3.0f);
    for (int i = 0; i < 8; ++i) {
        glm::vec3 corner(center.x + ((i&1)?h:-h), center.y + ((i&2)?h:-h), center.z + ((i&4)?h:-h));
        glm::vec3 n = proj_ndc(vp, corner);
        EXPECT(n.x >= -1.001f && n.x <= 1.001f);
        EXPECT(n.y >= -1.001f && n.y <= 1.001f);
        EXPECT(n.z >= -0.001f && n.z <= 1.001f);
    }
}

static void T07_straight_down_no_nan()
{
    const glm::mat4 vp = ComputeLightViewProj(glm::vec3(0), 2.0f, glm::vec3(0, -1, 0));
    const glm::vec3 n = proj_ndc(vp, glm::vec3(0));
    EXPECT(!std::isnan(n.x) && !std::isnan(n.y) && !std::isnan(n.z));
}

// CameraForward: a lookAtRH camera at (0,10,20) looking at origin yields normalize(target - eye).
static void T08_camera_forward()
{
    const glm::vec3 eye(0, 10, 20), target(0, 0, 0);
    const glm::mat4 view = glm::lookAtRH(eye, target, glm::vec3(0, 1, 0));
    const glm::vec3 fwd = CameraForward(view);
    const glm::vec3 expected = glm::normalize(target - eye);
    EXPECT(near(fwd.x, expected.x));
    EXPECT(near(fwd.y, expected.y));
    EXPECT(near(fwd.z, expected.z));
    EXPECT(near(glm::length(fwd), 1.0f));
}

// GroundFocus: a downward ray hits y=0 at the expected point; an upward ray uses the fallback.
static void T09_ground_focus()
{
    const glm::vec3 hit = GroundFocus(glm::vec3(2, 10, 2), glm::vec3(0, -1, 0), 99.0f);
    EXPECT(near(hit.x, 2.0f) && near(hit.y, 0.0f) && near(hit.z, 2.0f));
    // Upward ray never hits the ground -> fallback eye + forward*fallbackDist = (0,5,0)+(0,1,0)*7.
    const glm::vec3 up = GroundFocus(glm::vec3(0, 5, 0), glm::vec3(0, 1, 0), 7.0f);
    EXPECT(near(up.x, 0.0f) && near(up.y, 12.0f) && near(up.z, 0.0f));
}

int main()
{
    T00_nearextend_zero_is_identity_change();
    T01_nearextend_captures_caster_toward_sun();
    T02_frustum_slice_sphere();
    T03_snap_to_texel_grid();
    T04_sun_up_gate();
    T05_center_projects_to_ndc_origin();
    T06_scene_fits_inside_light_frustum();
    T07_straight_down_no_nan();
    T08_camera_forward();
    T09_ground_focus();
    if (g_Failures == 0) { std::printf("All shadow-math tests passed.\n"); return 0; }
    std::fprintf(stderr, "%d shadow-math test(s) failed.\n", g_Failures);
    return 1;
}
