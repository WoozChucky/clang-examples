#pragma once

#include "Engine.h"
#include "DotNetPluginHost.h"

class ENGINE_API DotNetPluginManager {
private:
    struct PluginInstance {
        std::string Name;
        void* Handle;  // IntPtr from C#
        void* InitializeFunc;
        void* UpdateFunc;
        void* ShutdownFunc;
    };

    DotNetPluginHost m_Host;
    std::vector<PluginInstance> m_Plugins;

    // Function pointers to PluginDiscovery methods
    void* m_LoadPluginFunc = nullptr;
    void* m_InitializeFunc = nullptr;
    void* m_UpdateFunc = nullptr;
    void* m_ShutdownFunc = nullptr;
    void* m_GetPluginTypesFunc = nullptr;

public:
    bool Initialize(const char* runtimeConfigPath);
    bool LoadPluginsFromDirectory(const std::string& pluginDirectory);
    void UpdateAll(double deltaTime, void* reserved = nullptr);
    void ShutdownAll();
};
