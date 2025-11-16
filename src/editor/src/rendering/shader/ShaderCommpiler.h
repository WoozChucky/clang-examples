#pragma once

#include <d3dcompiler.h>
#include <nvrhi/nvrhi.h>
#include <string>

#include "lib.h"

#include <wrl.h>
#include <d3dcompiler.h>
#include <dxcapi.h>

using Microsoft::WRL::ComPtr;

struct ShaderBlob {
    std::vector<uint8_t> data;
};

// The idea is this function will compile shader source code into a binary blob
// suitable for the specified RendererAPI (e.g., DXIL for DirectX12, and SPIR-V for Vulkan).
// It returns the compiled shader blob on success, or an empty blob on failure,
// with the error message populated in outErrorMessage.

inline ShaderBlob CompileShader(
    RendererAPI api,
    std::string_view sourceCode,
    const char* entryPoint,
    const char* targetName,
    std::string& outErrorMessage) {

    ShaderBlob shaderBlob;

    outErrorMessage.clear();

    // Create DXC instances
    ComPtr<IDxcUtils> utils;
    ComPtr<IDxcCompiler3> compiler;

    if (FAILED(DxcCreateInstance(CLSID_DxcUtils, IID_PPV_ARGS(&utils)))) {
        outErrorMessage = "Failed to create DXC utils instance";
        return shaderBlob;
    }
    if (FAILED(DxcCreateInstance(CLSID_DxcCompiler, IID_PPV_ARGS(&compiler)))) {
        outErrorMessage = "Failed to create DXC compiler instance";
        return shaderBlob;
    }

    // Create source blob
    ComPtr<IDxcBlobEncoding> sourceBlob;
    if (FAILED(utils->CreateBlob(sourceCode.data(),
                             static_cast<UINT32>(sourceCode.size()),
                                 CP_UTF8, &sourceBlob))) {
        outErrorMessage = "Failed to create source blob";
        return shaderBlob;
    }

    // Build arguments
    std::vector<LPCWSTR> arguments;

    // Entry point
    std::wstring wEntryPoint = std::wstring(entryPoint, entryPoint + strlen(entryPoint));
    arguments.push_back(L"-E");
    arguments.push_back(wEntryPoint.c_str());

    // Target profile
    std::wstring wTargetName = std::wstring(targetName, targetName + strlen(targetName));
    arguments.push_back(L"-T");
    arguments.push_back(wTargetName.c_str());

    // Debug flags
#if defined(_DEBUG)
    arguments.push_back(DXC_ARG_DEBUG);
    arguments.push_back(DXC_ARG_SKIP_OPTIMIZATIONS);
#endif

    // SPIR-V generation for Vulkan
    if (api == RendererAPI::Vulkan) {
        arguments.push_back(L"-spirv");
        arguments.push_back(L"-fspv-target-env=vulkan1.2");
    }

    // Compile
    DxcBuffer sourceBuffer = {};
    sourceBuffer.Ptr = sourceBlob->GetBufferPointer();
    sourceBuffer.Size = sourceBlob->GetBufferSize();
    sourceBuffer.Encoding = CP_UTF8;

    ComPtr<IDxcResult> result;
    HRESULT hr = compiler->Compile(&sourceBuffer, arguments.data(),
                                    static_cast<UINT32>(arguments.size()),
                                    nullptr, IID_PPV_ARGS(&result));

    if (SUCCEEDED(hr)) {
        result->GetStatus(&hr);
    }

    // Get errors
    ComPtr<IDxcBlobUtf8> errors;
    if (SUCCEEDED(result->GetOutput(DXC_OUT_ERRORS, IID_PPV_ARGS(&errors), nullptr)) &&
        errors && errors->GetStringLength() > 0) {
        outErrorMessage = std::string(errors->GetStringPointer(), errors->GetStringLength());
    }

    if (FAILED(hr)) {
        return shaderBlob;
    }

    // Get compiled shader
    ComPtr<IDxcBlob> compiledShader;
    if (SUCCEEDED(result->GetOutput(DXC_OUT_OBJECT, IID_PPV_ARGS(&compiledShader), nullptr))) {
        shaderBlob.data.resize(compiledShader->GetBufferSize());
        memcpy(shaderBlob.data.data(), compiledShader->GetBufferPointer(),
               compiledShader->GetBufferSize());
    }

    return shaderBlob;
}
