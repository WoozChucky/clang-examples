#include "ShaderCompiler.h"

#include <filesystem>
#include <fstream>

#include <d3dcompiler.h>
#include <nvrhi/nvrhi.h>
#include <wrl.h>
#include <dxcapi.h>

using Microsoft::WRL::ComPtr;

namespace {
    // Strip any HLSL attributes of the form [[vk::...]] when not compiling for Vulkan/SPIR-V.
    // This allows the same shader source to compile for DX12 where these attributes are unknown.
    static std::string PreprocessShaderSource(RendererAPI api, std::string_view source)
    {
        // Keep Vulkan attributes intact for Vulkan.
        if (api == RendererAPI::Vulkan)
        {
            return std::string(source);
        }

        std::string output;
        output.reserve(source.size());

        const char* s = source.data();
        const size_t n = source.size();

        size_t i = 0;
        while (i < n)
        {
            // Look for an attribute block start.
            if (i + 1 < n && s[i] == '[' && s[i + 1] == '[')
            {
                size_t contentStart = i + 2;
                size_t end = source.find("]]", contentStart);
                if (end == std::string_view::npos)
                {
                    // Malformed attribute, just copy the remaining text and stop.
                    output.append(s + i, n - i);
                    break;
                }

                // Extract the attribute content between [[ and ]]
                std::string_view attrib = source.substr(contentStart, end - contentStart);

                // Trim leading whitespace in the attribute content
                size_t a = 0;
                while (a < attrib.size() && (attrib[a] == ' ' || attrib[a] == '\t' || attrib[a] == '\n' || attrib[a] == '\r'))
                    ++a;

                // If this attribute block contains any vk:: attribute, drop the whole block.
                bool isVulkanAttribute = (attrib.find("vk::", a) != std::string_view::npos);

                if (isVulkanAttribute)
                {
                    // Skip the entire [[...]] block
                    i = end + 2;
                    continue;
                }
                else
                {
                    // Keep non-vk attributes intact
                    output.append(s + i, (end + 2) - i);
                    i = end + 2;
                    continue;
                }
            }

            // Default: copy character
            output.push_back(s[i]);
            ++i;
        }

        return output;
    }
}

ShaderBlob CompileShader(
    RendererAPI api,
    std::string_view sourceCode,
    const char* entryPoint,
    const char* targetName,
    std::string& outErrorMessage) {

    outErrorMessage.clear();
    ShaderBlob shaderBlob;

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

    // Preprocess the shader source for the selected API (strip [[vk::...]] for non-Vulkan)
    std::string preprocessedSource = PreprocessShaderSource(api, sourceCode);

    // Create source blob
    ComPtr<IDxcBlobEncoding> sourceBlob;
    if (FAILED(utils->CreateBlob(preprocessedSource.data(),
                             static_cast<UINT32>(preprocessedSource.size()),
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
    arguments.push_back(DXC_ARG_WARNINGS_ARE_ERRORS);

    // Debug flags
#if defined(_DEBUG)
    arguments.push_back(DXC_ARG_DEBUG);
    arguments.push_back(DXC_ARG_SKIP_OPTIMIZATIONS);
    arguments.push_back(L"-Qembed_debug");
#endif

    // Here for future reference, do not remove
    // arguments.push_back(DXC_ARG_PACK_MATRIX_COLUMN_MAJOR);

    // SPIR-V generation for Vulkan
    if (api == RendererAPI::Vulkan) {
        arguments.push_back(L"-spirv");
        arguments.push_back(L"-fspv-target-env=vulkan1.3");
        // Here for future reference, do not remove
        // arguments.push_back(L"-Zpc");
        // arguments.push_back(L"-WX");
        // arguments.push_back(L"-fvk-use-dx-layout");
    }

    if (std::string(targetName).starts_with("vs_")) {
        // Here for future reference, do not remove
        //arguments.push_back(L"-fvk-invert-y");
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

ShaderBlob CompileGlsl(std::string_view sourceCode,
    const char* entryPoint,
    const char* targetName,
    std::string& outErrorMessage) {

    ShaderBlob shaderBlob;
    outErrorMessage.clear();

    if (sourceCode.empty()) {
        outErrorMessage = "GLSL source is empty";
        return shaderBlob;
    }

    // Create temporary files
    auto tempDir = std::filesystem::temp_directory_path();
    auto srcFile = tempDir / "shader_temp.glsl";
    auto outFile = tempDir / "shader_temp.spv";

    // Write GLSL source to temp file
    std::ofstream ofs(srcFile, std::ios::binary);
    ofs.write(sourceCode.data(), sourceCode.size());
    ofs.close();

    // Build command line
    std::string cmd = "glslangValidator -V ";
    cmd += srcFile.string();
    cmd += " -o " + outFile.string();
    if (targetName && std::strlen(targetName) > 0) {
        // vert / frag
        if (std::string(targetName) == "vs_6_1") {
            cmd += " -S vert";
        }
        else if (std::string(targetName) == "ps_6_1") {
            cmd += " -S frag";
        }
        else if (std::string(targetName) == "cs_6_1") {
            cmd += " -S comp";
        }
    }
    if(entryPoint && std::strlen(entryPoint) > 0) {
        cmd += " -e " + std::string(entryPoint);
    }

    // Run compiler
    int result = std::system(cmd.c_str());
    if(result != 0 || !std::filesystem::exists(outFile)) {
        outErrorMessage = "glslangValidator failed. Make sure it's on PATH.";
        return shaderBlob;
    }

    // Read SPIR-V binary
    std::ifstream ifs(outFile, std::ios::binary | std::ios::ate);
    if (!ifs) {
        outErrorMessage = "Failed to open SPIR-V output file";
        return shaderBlob;
    }
    auto size = ifs.tellg();
    ifs.seekg(0, std::ios::beg);
    shaderBlob.data.resize(static_cast<size_t>(size));
    ifs.read(reinterpret_cast<char*>(shaderBlob.data.data()), size);

    ifs.close();

    // Clean up temp files
    std::filesystem::remove(srcFile);
    std::filesystem::remove(outFile);

    return shaderBlob;
}
