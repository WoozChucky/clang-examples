#include <cstdio>
#include <nlohmann/json.hpp>

#include "AaModeMigration.h" // ResolveAaMode

static int g_Failures = 0;
#define EXPECT(cond)                                                     \
    do {                                                                 \
        if (!(cond)) {                                                   \
            std::fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); \
            ++g_Failures;                                                \
        }                                                                \
    } while (0)

// New aaMode key wins outright.
static void T00_aamode_key_wins()
{
    nlohmann::json r = { {"aaMode", 2}, {"fxaa", true} };
    EXPECT(ResolveAaMode(r) == 2); // SMAA, ignores legacy fxaa
}

// Legacy fxaa bool maps when aaMode absent: true -> FXAA(1), false -> Off(0).
static void T01_legacy_fxaa_maps()
{
    EXPECT(ResolveAaMode(nlohmann::json{ {"fxaa", true} })  == 1);
    EXPECT(ResolveAaMode(nlohmann::json{ {"fxaa", false} }) == 0);
}

// Neither key present -> default FXAA(1).
static void T02_default_is_fxaa()
{
    EXPECT(ResolveAaMode(nlohmann::json::object()) == 1);
}

// Out-of-range aaMode falls back (ignored -> legacy/default path).
static void T03_out_of_range_aamode_falls_back()
{
    EXPECT(ResolveAaMode(nlohmann::json{ {"aaMode", 7} })               == 1); // -> default
    EXPECT(ResolveAaMode(nlohmann::json{ {"aaMode", 7}, {"fxaa", false} }) == 0); // -> legacy
}

int main()
{
    T00_aamode_key_wins();
    T01_legacy_fxaa_maps();
    T02_default_is_fxaa();
    T03_out_of_range_aamode_falls_back();
    if (g_Failures == 0) { std::printf("All AA-mode tests passed.\n"); return 0; }
    std::fprintf(stderr, "%d AA-mode test(s) failed.\n", g_Failures);
    return 1;
}
