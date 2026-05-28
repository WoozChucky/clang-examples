#include <cstdio>
#include <cmath>
#include <glm/glm.hpp>

#include "Atmosphere.h" // SunDirectionFromAngles

static int g_Failures = 0;
#define EXPECT(cond)                                                     \
    do {                                                                 \
        if (!(cond)) {                                                   \
            std::fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); \
            ++g_Failures;                                                \
        }                                                                \
    } while (0)

static bool near(float a, float b) { return std::fabs(a - b) < 1e-3f; }

// Engine convention: sun light direction points FROM the sun toward the scene,
// so an overhead noon sun points straight down (dir.y == -1).
static void T00_overhead_points_down()
{
    const glm::vec3 d = SunDirectionFromAngles(90.0f, 0.0f);
    EXPECT(near(d.x, 0.0f));
    EXPECT(near(d.y, -1.0f));
    EXPECT(near(d.z, 0.0f));
}

// At the horizon (elevation 0) the vertical component is zero.
static void T01_horizon_is_flat()
{
    const glm::vec3 d = SunDirectionFromAngles(0.0f, 0.0f);
    EXPECT(near(d.y, 0.0f));
    EXPECT(near(glm::length(d), 1.0f)); // always unit length
}

// Azimuth rotates the horizontal component around +Y.
static void T02_azimuth_rotates_horizontal()
{
    const glm::vec3 d = SunDirectionFromAngles(0.0f, 90.0f);
    EXPECT(near(d.x, 1.0f)); // sin(90deg) on x
    EXPECT(near(d.z, 0.0f)); // cos(90deg) on z
}

// Elevation is clamped to [0,90]: out-of-range stays unit and finite.
static void T03_elevation_clamped()
{
    const glm::vec3 d = SunDirectionFromAngles(140.0f, 0.0f);
    EXPECT(near(glm::length(d), 1.0f));
    EXPECT(near(d.y, -1.0f)); // clamped to 90 -> straight down
}

int main()
{
    T00_overhead_points_down();
    T01_horizon_is_flat();
    T02_azimuth_rotates_horizontal();
    T03_elevation_clamped();
    if (g_Failures == 0) { std::printf("All atmosphere tests passed.\n"); return 0; }
    std::fprintf(stderr, "%d atmosphere test(s) failed.\n", g_Failures);
    return 1;
}
