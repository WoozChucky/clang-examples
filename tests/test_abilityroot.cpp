#include <cstdio>
#include "AbilityRoot.h"

static int g_Failures = 0;
#define EXPECT(c) do{ if(!(c)){ std::fprintf(stderr,"FAIL %s:%d: %s\n",__FILE__,__LINE__,#c); ++g_Failures; } }while(0)

int main() {
    EXPECT(ShouldRootMovement("Attack", 0.0f));
    EXPECT(ShouldRootMovement("Attack", 0.59f));
    EXPECT(!ShouldRootMovement("Attack", 0.6f));   // window end exclusive
    EXPECT(!ShouldRootMovement("Attack", 0.9f));
    EXPECT(!ShouldRootMovement("Idle", 0.1f));
    EXPECT(!ShouldRootMovement("Walk", 0.1f));
    if (g_Failures) { std::fprintf(stderr, "test_abilityroot: %d FAILURES\n", g_Failures); return 1; }
    std::printf("All ability-root tests passed.\n");
    return 0;
}
