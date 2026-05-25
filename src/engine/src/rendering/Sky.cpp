#include "Sky.h"

SkySettings& GetSkySettings()
{
    static SkySettings s;
    return s;
}
