#include <cstdio>
#include <cmath>
#include <glm/glm.hpp>

#include "SsaoMath.h" // MakeHemisphereKernel, IsOccluded, RangeWeight, SsaoKernel

static int g_Failures = 0;
#define EXPECT(cond)                                                     \
    do {                                                                 \
        if (!(cond)) {                                                   \
            std::fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); \
            ++g_Failures;                                                \
        }                                                                \
    } while (0)

static bool near_eq(float a, float b, float e = 1e-4f) { return std::fabs(a - b) < e; }

// Kernel: all samples in the +Z hemisphere, within the unit sphere, biased toward the origin.
static void T00_kernel_hemisphere()
{
    SsaoKernel k = MakeHemisphereKernel();
    EXPECT(k.Count == 16);
    float maxLen = 0.0f, sumLen = 0.0f;
    for (int i = 0; i < k.Count; ++i) {
        EXPECT(k.Samples[i].z >= 0.0f);                 // +Z hemisphere
        const float len = glm::length(k.Samples[i]);
        EXPECT(len <= 1.0001f);                          // within unit sphere
        maxLen = glm::max(maxLen, len); sumLen += len;
    }
    EXPECT(maxLen > 0.5f);                                // not all crammed at origin
    EXPECT(sumLen / k.Count < 0.8f);                     // bias -> avg pulled inward
}

// Occlusion: occluded when the stored surface is closer to the camera than the sample by > bias.
// View-space Z is negative (RH, -Z forward), so "closer" = larger (less negative) Z.
static void T01_occluded_when_surface_in_front()
{
    EXPECT(IsOccluded(/*sampleViewZ=*/-5.0f, /*occluderViewZ=*/-4.0f, /*bias=*/0.025f) == true);
    EXPECT(IsOccluded(-5.0f, -6.0f, 0.025f) == false);   // occluder behind sample
    EXPECT(IsOccluded(-5.0f, -5.01f, 0.025f) == false);  // within bias -> self-occlusion guard
}

// Range falloff: 1 at the shaded point, 0 at/beyond radius.
static void T02_range_falloff()
{
    EXPECT(near_eq(RangeWeight(/*dist=*/0.0f, /*radius=*/1.0f), 1.0f));
    EXPECT(near_eq(RangeWeight(2.0f, 1.0f), 0.0f));      // beyond radius
    const float mid = RangeWeight(0.5f, 1.0f);
    EXPECT(mid > 0.0f && mid < 1.0f);
}

int main()
{
    T00_kernel_hemisphere();
    T01_occluded_when_surface_in_front();
    T02_range_falloff();
    if (g_Failures == 0) { std::printf("All ssao-math tests passed.\n"); return 0; }
    std::fprintf(stderr, "%d ssao-math test(s) failed.\n", g_Failures);
    return 1;
}
