#include "MaterialLoader.h"

#include "lib.h"

// Use stb_image from assimp contrib (already available in the project)
#define STB_IMAGE_IMPLEMENTATION
#include "stb_image.h"

namespace MaterialLoader
{
    bool LoadMaterialFromFile(
        const char* filePath,
        std::vector<uint32_t>& pixels,
        uint32_t& width,
        uint32_t& height,
        std::string& error)
    {
        if (!filePath || filePath[0] == '\0')
        {
            error = "Invalid file path";
            return false;
        }

        // Load image using stb_image
        // Force 4 channels (RGBA) for consistency with MaterialSystem
        int imageWidth = 0;
        int imageHeight = 0;
        int imageChannels = 0;
        constexpr int desiredChannels = 4; // RGBA

        unsigned char* imageData = stbi_load(filePath, &imageWidth, &imageHeight, &imageChannels, desiredChannels);

        if (!imageData)
        {
            const char* stbiError = stbi_failure_reason();
            error = std::string("Failed to load image: ") + (stbiError ? stbiError : "unknown error");
            return false;
        }

        if (imageWidth <= 0 || imageHeight <= 0)
        {
            error = "Invalid image dimensions";
            stbi_image_free(imageData);
            return false;
        }

        // Convert to vector<uint32_t> (RGBA8 packed)
        width = static_cast<uint32_t>(imageWidth);
        height = static_cast<uint32_t>(imageHeight);
        const size_t pixelCount = width * height;

        pixels.clear();
        pixels.reserve(pixelCount);

        // Pack RGBA bytes into uint32_t (R | G<<8 | B<<16 | A<<24)
        for (size_t i = 0; i < pixelCount; ++i)
        {
            const size_t byteIndex = i * 4;
            const uint32_t r = imageData[byteIndex + 0];
            const uint32_t g = imageData[byteIndex + 1];
            const uint32_t b = imageData[byteIndex + 2];
            const uint32_t a = imageData[byteIndex + 3];
            const uint32_t packed = r | (g << 8) | (b << 16) | (a << 24);
            pixels.push_back(packed);
        }

        // Free stb_image data
        stbi_image_free(imageData);

        SM_TRACE("MaterialLoader: Loaded image '%s' (%ux%u, %d channels -> RGBA)",
                 filePath, width, height, imageChannels);

        return true;
    }
}
