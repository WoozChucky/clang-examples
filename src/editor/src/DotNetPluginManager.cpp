#include "DotNetPluginManager.h"
#include <iostream>
#include <filesystem>

#include "lib.h"

bool DotNetPluginManager::Initialize(const char* runtimeConfigPath) {
    if (!m_Host.Initialize(runtimeConfigPath)) {
        return false;
    }

    // Load PluginCore.dll and get PluginDiscovery methods
    const wchar_t* typeName = L"PluginCore.PluginDiscovery, PluginCore";

    if (!m_Host.LoadFunction("plugins/PluginCore.dll", typeName, L"LoadPlugin", &m_LoadPluginFunc)) {
        SM_WARN("Failed to load LoadPlugin function");
        return false;
    }

    if (!m_Host.LoadFunction("plugins/PluginCore.dll", typeName, L"GetPluginTypes", &m_GetPluginTypesFunc)) {
        SM_WARN("Failed to load GetPluginTypes function");
        return false;
    }

    if (!m_Host.LoadFunction("plugins/PluginCore.dll", typeName, L"Initialize", &m_InitializeFunc)) {
        std::cerr << "Failed to load Initialize function" << std::endl;
        SM_WARN("Failed to load Initialize function");
        return false;
    }

    if (!m_Host.LoadFunction("plugins/PluginCore.dll", typeName, L"Update", &m_UpdateFunc)) {
        SM_WARN("Failed to load Update function");
        return false;
    }

    if (!m_Host.LoadFunction("plugins/PluginCore.dll", typeName, L"Shutdown", &m_ShutdownFunc)) {
        SM_WARN("Failed to load Shutdown function");
        return false;
    }

    SM_TRACE("DotNetPluginManager initialized successfully");
    return true;
}

bool DotNetPluginManager::LoadPluginsFromDirectory(const std::string& pluginDirectory) {
    namespace fs = std::filesystem;

    for (const auto& entry : fs::directory_iterator(pluginDirectory)) {
        if (entry.path().extension() != ".dll") continue;
        if (entry.path().filename() == "PluginCore.dll") continue;

        std::string assemblyPath = entry.path().string();
        std::vector<std::string> discoveredTypes;

        // Store the vector pointer in a static variable so the callback can access it
        static std::vector<std::string>* s_CurrentTypes = nullptr;
        s_CurrentTypes = &discoveredTypes;

        // Static callback that matches C# delegate signature
        auto staticCallback = +[](const char* typeName) {
            if (s_CurrentTypes) {
                s_CurrentTypes->push_back(typeName);
            }
        };

        using GetPluginTypesFn = int(*)(const char*, void(*)(const char*));
        auto getTypesFunc = reinterpret_cast<GetPluginTypesFn>(m_GetPluginTypesFunc);

        int typeCount = getTypesFunc(assemblyPath.c_str(), staticCallback);

        s_CurrentTypes = nullptr; // Clean up

        if (typeCount == 0) {
            SM_WARN("No plugins found in: %s", assemblyPath.c_str());
            continue;
        }

        // Load each discovered type
        for (const auto& typeName : discoveredTypes) {
            // Convert to absolute path
            std::string absolutePath = std::filesystem::absolute(assemblyPath).string();

            using LoadPluginFn = void*(*)(const char*, const char*);
            void* handle = reinterpret_cast<LoadPluginFn>(m_LoadPluginFunc)(
                absolutePath.c_str(),
                typeName.c_str()
            );

            if (handle == nullptr) {
                SM_WARN("Failed to load plugin: %s from type %s", typeName.c_str(), assemblyPath.c_str());
                continue;
            }

            PluginInstance plugin;
            plugin.Name = typeName;
            plugin.Handle = handle;
            plugin.InitializeFunc = m_InitializeFunc;
            plugin.UpdateFunc = m_UpdateFunc;
            plugin.ShutdownFunc = m_ShutdownFunc;

            m_Plugins.push_back(plugin);

            using InitializeFn = void(*)(void*);
            reinterpret_cast<InitializeFn>(m_InitializeFunc)(handle);

            SM_TRACE("Loaded plugin: %s", typeName.c_str());
        }
    }

    return !m_Plugins.empty();
}


void DotNetPluginManager::UpdateAll(double deltaTime, void* reserved) {
    using UpdateFn = void (*)(void*, double, void*);
    auto updateFn = reinterpret_cast<UpdateFn>(m_UpdateFunc);

    for (auto& plugin : m_Plugins) {
        updateFn(plugin.Handle, deltaTime, reserved);
    }
}

void DotNetPluginManager::ShutdownAll() {
    using ShutdownFn = void (*)(void*);
    auto shutdownFn = reinterpret_cast<ShutdownFn>(m_ShutdownFunc);

    for (auto& plugin : m_Plugins) {
        shutdownFn(plugin.Handle);
    }

    m_Plugins.clear();
    m_Host.Shutdown();
}
