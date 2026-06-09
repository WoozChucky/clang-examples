#include <cstdio>
#include <string>
#include "MeshLoader.h"

static int g_Failures = 0;
#define EXPECT(cond) do { if(!(cond)){ std::fprintf(stderr,"FAIL %s:%d: %s\n",__FILE__,__LINE__,#cond); ++g_Failures; } } while(0)

#ifndef MESHLOADER_TEST_ASSETS_DIR
#define MESHLOADER_TEST_ASSETS_DIR "assets"
#endif
static std::string asset(const char* rel) { return std::string(MESHLOADER_TEST_ASSETS_DIR) + "/" + rel; }

static void T01_fox_gltf()
{
    MeshLoader::LoadedModel m; std::string err;
    const bool ok = MeshLoader::LoadModel(asset("models/Fox.gltf").c_str(), m, err);
    EXPECT(ok);
    EXPECT(m.vertices.size() > 0);
    EXPECT(m.indices.size() > 0);
    for (uint32_t idx : m.indices) EXPECT(idx < m.vertices.size());
    EXPECT(m.hasSkeleton);
    EXPECT(m.skinning.size() == m.vertices.size());
    EXPECT(m.clips.size() > 0);
}

static void T02_multi_submesh()
{
    // scene.gltf is a genuine multi-submesh asset (17 primitives). cube-textured-multiple.obj,
    // despite its name, is a single object/material under assimp -> 1 submesh, so it cannot
    // exercise the cross-submesh global-index path; scene.gltf does.
    MeshLoader::LoadedModel m; std::string err;
    const bool ok = MeshLoader::LoadModel(asset("models/scene.gltf").c_str(), m, err);
    EXPECT(ok);
    EXPECT(m.subMeshes.size() > 1);
    EXPECT(m.vertices.size() > 0);
    for (uint32_t idx : m.indices) EXPECT(idx < m.vertices.size());
}

int main()
{
    T01_fox_gltf();
    T02_multi_submesh();
    if (g_Failures == 0) { std::printf("All meshloader tests passed.\n"); return 0; }
    std::fprintf(stderr, "%d meshloader test(s) failed.\n", g_Failures);
    return 1;
}
