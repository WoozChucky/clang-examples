#pragma once

#include "renderer.h"

void load_font(const char* filePath, int fontSize, FontAtlas& outAtlas, const nvrhi::DeviceHandle& device);