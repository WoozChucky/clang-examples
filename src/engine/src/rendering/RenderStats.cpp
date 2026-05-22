#include "RenderStats.h"

RenderStats& GetRenderStats()
{
    static RenderStats s;
    return s;
}

CullingSettings& GetCullingSettings()
{
    static CullingSettings s;
    return s;
}
