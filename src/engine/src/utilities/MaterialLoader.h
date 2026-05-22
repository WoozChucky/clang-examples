#pragma once

#include <vector>
#include <string>
#include <cstdint>

// MaterialLoader utility for loading image files into RGBA8 pixel data
// Uses stb_image library to support PNG, JPG, BMP, TGA, and other common formats
namespace MaterialLoader
{
    // Load an image file and return RGBA8 pixel data
    // Returns true on success, false on failure
    // Outputs:
    //   - pixels: RGBA8 pixel data (4 bytes per pixel, packed as uint32_t)
    //   - width: image width in pixels
    //   - height: image height in pixels
    //   - error: error message if loading fails
    bool LoadMaterialFromFile(
        const char* filePath,
        std::vector<uint32_t>& pixels,
        uint32_t& width,
        uint32_t& height,
        std::string& error
    );
}
