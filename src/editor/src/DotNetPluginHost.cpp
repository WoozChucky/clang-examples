#include "DotNetPluginHost.h"

#include "DotNetPluginHost.h"
#include <Windows.h>
#include <iostream>
#include <filesystem>

#include "lib.h"

// Platform-specific helpers
#ifdef _WIN32
    #define STR(s) L##s
    #define CHAR_T wchar_t
    #define DIR_SEPARATOR L'\\'
    #define MAX_PATH_LEN 260
#else
    #define STR(s) s
    #define CHAR_T char
    #define DIR_SEPARATOR '/'
    #define MAX_PATH_LEN 4096
#endif

DotNetPluginHost::~DotNetPluginHost() {
    Shutdown();
}

bool DotNetPluginHost::Initialize(const char* runtimeConfigPath) {
    if (!LoadHostFxr()) {
        SM_WARN("Failed to load hostfxr");
        return false;
    }

    // Convert runtime config path to wide string on Windows
    std::wstring configPathWide;
    size_t len = strlen(runtimeConfigPath);
    configPathWide.resize(len);
    size_t convertedChars = 0;
    mbstowcs_s(&convertedChars, &configPathWide[0], len + 1, runtimeConfigPath, len);

    // Initialize runtime with config
    hostfxr_initialize_parameters params{};
    params.size = sizeof(hostfxr_initialize_parameters);

    int rc = m_InitFxr(configPathWide.c_str(), &params, &m_Context);
    if (rc != 0 || m_Context == nullptr) {
        SM_WARN("Failed to initialize hostfxr. Return code: %x", rc);
        return false;
    }

    // Get delegate for loading assemblies
    void* loadAssemblyPtr = nullptr;
    rc = m_GetDelegate(
        m_Context,
        hdt_load_assembly_and_get_function_pointer,
        &loadAssemblyPtr
    );

    if (rc != 0 || loadAssemblyPtr == nullptr) {
        SM_WARN("Failed to get load_assembly delegate. Return code: %x", rc);
        return false;
    }

    m_LoadAssemblyFunc = reinterpret_cast<load_assembly_and_get_function_pointer_fn>(loadAssemblyPtr);

    SM_TRACE("DotNetPluginHost initialized successfully");
    return true;
}

void DotNetPluginHost::Shutdown() {
    m_PluginUpdateFuncs.clear();

    if (m_CloseFxr && m_Context) {
        m_CloseFxr(m_Context);
        m_Context = nullptr;
    }

    if (m_HostFxrLib) {
        FreeLibrary(static_cast<HMODULE>(m_HostFxrLib));
        m_HostFxrLib = nullptr;
    }

    m_InitFxr = nullptr;
    m_GetDelegate = nullptr;
    m_CloseFxr = nullptr;
    m_LoadAssemblyFunc = nullptr;
}

bool DotNetPluginHost::LoadPlugin(const char* assemblyPath) {
    if (!m_LoadAssemblyFunc) {
        std::cerr << "Plugin host not initialized" << std::endl;
        return false;
    }

    // Convert assembly path to wide string
    std::wstring assemblyPathWide;
    size_t len = strlen(assemblyPath);
    assemblyPathWide.resize(len);
    size_t convertedChars = 0;
    mbstowcs_s(&convertedChars, &assemblyPathWide[0], len + 1, assemblyPath, len);

    // Get absolute path
    std::filesystem::path fullPath = std::filesystem::absolute(assemblyPathWide);
    if (!std::filesystem::exists(fullPath)) {
        std::cerr << "ERROR: Assembly not found at: " << fullPath << std::endl;
        return false;
    }

    // Load the plugin assembly and get the Update function
    // Expected C# signature: public static void Update(double deltaTime)
    const CHAR_T* typeName = L"MyPlugin.PluginEntry, ClassLibrary";
    const CHAR_T* methodName = L"Update";

    void* updateFunc = nullptr;
    int rc = m_LoadAssemblyFunc(
        fullPath.c_str(),
        typeName,
        methodName,
        UNMANAGEDCALLERSONLY_METHOD,
        nullptr,
        &updateFunc
    );

    if (rc != 0 || updateFunc == nullptr) {
        std::cerr << "Failed to load plugin: " << assemblyPath
                  << " Return code: " << std::hex << rc << std::endl;
        return false;
    }

    m_PluginUpdateFuncs.push_back(updateFunc);
    std::cout << "Plugin loaded successfully: " << assemblyPath << std::endl;
    return true;
}

void DotNetPluginHost::UpdatePlugins(double deltaTime) {
    for (void* updateFunc : m_PluginUpdateFuncs) {
        if (updateFunc) {
            // Call the plugin's Update method
            // Signature: void (*)(double)
            using UpdateFn = void (*)(double);
            reinterpret_cast<UpdateFn>(updateFunc)(deltaTime);
        }
    }
}

bool DotNetPluginHost::LoadFunction(const char* assemblyPath, const wchar_t* typeName, const wchar_t* methodName, void** functionPtr) {
    if (!m_LoadAssemblyFunc) {
        std::cerr << "Plugin host not initialized" << std::endl;
        return false;
    }

    // Convert assembly path to wide string
    std::wstring assemblyPathWide;
    size_t len = strlen(assemblyPath);
    assemblyPathWide.resize(len);
    size_t convertedChars = 0;
    mbstowcs_s(&convertedChars, &assemblyPathWide[0], len + 1, assemblyPath, len);

    std::filesystem::path fullPath = std::filesystem::absolute(assemblyPathWide);

    if (!std::filesystem::exists(fullPath)) {
        std::cerr << "Assembly not found: " << fullPath << std::endl;
        return false;
    }

    int rc = m_LoadAssemblyFunc(
        fullPath.c_str(),
        typeName,
        methodName,
        UNMANAGEDCALLERSONLY_METHOD,
        nullptr,
        functionPtr
    );

    return rc == 0 && *functionPtr != nullptr;
}

bool DotNetPluginHost::LoadHostFxr() {
    // Try to find hostfxr.dll next to the executable first
    CHAR_T buffer[MAX_PATH_LEN];
    size_t bufferSize = sizeof(buffer) / sizeof(CHAR_T);

    int rc = get_hostfxr_path(buffer, &bufferSize, nullptr);
    if (rc != 0) {
        std::cerr << "Failed to find hostfxr. Return code: " << std::hex << rc << std::endl;

        // Fallback: try to load from local directory
        m_HostFxrLib = LoadLibraryW(L"hostfxr.dll");
        if (!m_HostFxrLib) {
            std::cerr << "Failed to load hostfxr.dll from local directory" << std::endl;
            return false;
        }
    } else {
        m_HostFxrLib = LoadLibraryW(buffer);
        if (!m_HostFxrLib) {
            std::wcerr << L"Failed to load hostfxr from: " << buffer << std::endl;
            return false;
        }
    }

    // Get function pointers
    m_InitFxr = reinterpret_cast<hostfxr_initialize_for_runtime_config_fn>(
        GetProcAddress(static_cast<HMODULE>(m_HostFxrLib), "hostfxr_initialize_for_runtime_config")
    );

    m_GetDelegate = reinterpret_cast<hostfxr_get_runtime_delegate_fn>(
        GetProcAddress(static_cast<HMODULE>(m_HostFxrLib), "hostfxr_get_runtime_delegate")
    );

    m_CloseFxr = reinterpret_cast<hostfxr_close_fn>(
        GetProcAddress(static_cast<HMODULE>(m_HostFxrLib), "hostfxr_close")
    );

    if (!m_InitFxr || !m_GetDelegate || !m_CloseFxr) {
        std::cerr << "Failed to get hostfxr function pointers" << std::endl;
        return false;
    }

    return true;
}
