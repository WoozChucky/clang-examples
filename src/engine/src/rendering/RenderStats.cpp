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

DebugDrawSettings& GetDebugDrawSettings()
{
    static DebugDrawSettings s;
    return s;
}

ShadowSettings& GetShadowSettings()
{
    static ShadowSettings s;
    return s;
}

AntiAliasingSettings& GetAntiAliasingSettings()
{
    static AntiAliasingSettings s;
    return s;
}
