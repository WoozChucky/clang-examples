#pragma once

#include <string>
#include <vector>

#include "Engine.h"
#include "lib.h"

struct ShaderBlob {
    std::vector<uint8_t> data;
};

// The idea is this function will compile shader source code into a binary blob
// suitable for the specified RendererAPI (e.g., DXIL for DirectX12, and SPIR-V for Vulkan).
// It returns the compiled shader blob on success, or an empty blob on failure,
// with the error message populated in outErrorMessage.

ENGINE_API ShaderBlob CompileShader(
    RendererAPI api,
    std::string_view sourceCode,
    const char* entryPoint,
    const char* targetName,
    std::string& outErrorMessage);
