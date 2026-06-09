#include <cstdio>
#include <nlohmann/json.hpp>
#include "AoModeMigration.h" // ResolveAoMode

static int g_Failures = 0;
#define EXPECT(cond) do { if(!(cond)){ std::fprintf(stderr,"FAIL %s:%d: %s\n",__FILE__,__LINE__,#cond); ++g_Failures; } } while(0)

static void T00_mode_key_wins() {
    nlohmann::json s = { {"mode", 3}, {"enabled", false} };
    EXPECT(ResolveAoMode(s) == 3); // GTAO, ignores legacy enabled
}
static void T01_legacy_enabled_maps() {
    EXPECT(ResolveAoMode(nlohmann::json{ {"enabled", true} })  == 1);
    EXPECT(ResolveAoMode(nlohmann::json{ {"enabled", false} }) == 0);
}
static void T02_default_is_ssao() {
    EXPECT(ResolveAoMode(nlohmann::json::object()) == 1);
}
static void T03_out_of_range_falls_back() {
    EXPECT(ResolveAoMode(nlohmann::json{ {"mode", 9} })                   == 1);
    EXPECT(ResolveAoMode(nlohmann::json{ {"mode", 9}, {"enabled", false} }) == 0);
}

int main() {
    T00_mode_key_wins(); T01_legacy_enabled_maps(); T02_default_is_ssao(); T03_out_of_range_falls_back();
    if (g_Failures == 0) { std::printf("All AO-mode tests passed.\n"); return 0; }
    std::fprintf(stderr, "%d AO-mode test(s) failed.\n", g_Failures);
    return 1;
}
