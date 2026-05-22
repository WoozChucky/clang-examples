#pragma once
#include <string>
#include <vector>
#include <nethost.h>
#include <hostfxr.h>
#include <coreclr_delegates.h>

class DotNetPluginHost {
public:
    DotNetPluginHost() = default;
    ~DotNetPluginHost();

    bool Initialize(const char* runtimeConfigPath);
    void Shutdown();
    bool LoadPlugin(const char* assemblyPath);
    void UpdatePlugins(double deltaTime);

    bool LoadFunction(const char* assemblyPath, const wchar_t* typeName, const wchar_t* methodName, void** functionPtr);

private:
    hostfxr_handle m_Context = nullptr;
    void* m_HostFxrLib = nullptr;
    load_assembly_and_get_function_pointer_fn m_LoadAssemblyFunc = nullptr;
    std::vector<void*> m_PluginUpdateFuncs;

    bool LoadHostFxr();
    hostfxr_initialize_for_runtime_config_fn m_InitFxr = nullptr;
    hostfxr_get_runtime_delegate_fn m_GetDelegate = nullptr;
    hostfxr_close_fn m_CloseFxr = nullptr;
};