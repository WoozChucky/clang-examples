#include <cstdio>
#include <cmath>
#include <nlohmann/json.hpp>

#include "EditorPreferences.h" // pure mappers + RenderStats structs (via RenderStats.h)

static int g_Failures = 0;
#define EXPECT(cond)                                                     \
    do {                                                                 \
        if (!(cond)) {                                                   \
            std::fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); \
            ++g_Failures;                                                \
        }                                                                \
    } while (0)

static void T00_roundtrip()
{
    CullingSettings   culling;  culling.Enabled = false;          // default true
    DebugDrawSettings debug;    debug.ShowLightGizmos = true; debug.ShowCameraFrustum = true;
                                debug.ShowSelectedAABB = true; debug.Wireframe = true; debug.ShowGrid = true; // defaults all false
    ShadowSettings    shadows;  shadows.Enabled = false; shadows.Bias = 0.0042f; // defaults true / 0.0015

    const nlohmann::json j = EditorPreferences::PrefsToJson(culling, debug, shadows);

    CullingSettings   c2;  DebugDrawSettings d2;  ShadowSettings s2; // fresh defaults
    EditorPreferences::PrefsFromJson(j, c2, d2, s2);

    EXPECT(c2.Enabled == culling.Enabled);
    EXPECT(d2.ShowLightGizmos   == debug.ShowLightGizmos);
    EXPECT(d2.ShowCameraFrustum == debug.ShowCameraFrustum);
    EXPECT(d2.ShowSelectedAABB  == debug.ShowSelectedAABB);
    EXPECT(d2.Wireframe         == debug.Wireframe);
    EXPECT(d2.ShowGrid          == debug.ShowGrid);
    EXPECT(s2.Enabled == shadows.Enabled);
    EXPECT(std::fabs(s2.Bias - shadows.Bias) < 1e-6f);
}

static void T01_missing_keys_leave_defaults()
{
    // Partial/old document: only debugDraw.grid present.
    nlohmann::json j;
    j["debugDraw"]["grid"] = false;

    CullingSettings   culling;            // default Enabled=true
    DebugDrawSettings debug;              // defaults all false
    debug.Wireframe = true;              // sentinel: absent key must leave this true
    ShadowSettings    shadows;            // default Enabled=true, Bias=0.0015

    EditorPreferences::PrefsFromJson(j, culling, debug, shadows);

    EXPECT(culling.Enabled == true);      // absent -> default kept
    EXPECT(debug.ShowGrid == false);      // present -> applied
    EXPECT(debug.Wireframe == true);      // absent -> sentinel kept
    EXPECT(shadows.Enabled == true);      // absent -> default kept
    EXPECT(std::fabs(shadows.Bias - 0.0015f) < 1e-6f); // absent -> default kept
}

int main()
{
    T00_roundtrip();
    T01_missing_keys_leave_defaults();
    if (g_Failures == 0) { std::printf("All editor preferences tests passed.\n"); return 0; }
    std::printf("%d editor preferences test(s) FAILED.\n", g_Failures);
    return 1;
}
