#include <cstdio>
#include <string>
#include "AssetKey.h"

static int g_Failures = 0;
#define EXPECT(cond) do { if(!(cond)){ std::fprintf(stderr,"FAIL %s:%d: %s\n",__FILE__,__LINE__,#cond); ++g_Failures; } } while(0)

int main() {
    EXPECT(AssetKeyHash("models/tree.obj") == AssetKeyHash("models/tree.obj"));
    EXPECT(AssetKeyHash("models/tree.obj") != AssetKeyHash("models/rock.obj"));
    EXPECT(AssetKeyHash("textures/bark.png") != AssetKeyHash("models/tree.obj"));
    EXPECT(AssetKeyHash("models/tree.obj") != kMissingAssetHandle);
    EXPECT(NormalizeAssetKey("assets\\models\\tree.obj") == "models/tree.obj");
    EXPECT(NormalizeAssetKey("assets/models/tree.obj") == "models/tree.obj");
    EXPECT(NormalizeAssetKey("models/tree.obj") == "models/tree.obj");
    if (g_Failures == 0) std::printf("All asset-key tests passed.\n");
    return g_Failures ? 1 : 0;
}
