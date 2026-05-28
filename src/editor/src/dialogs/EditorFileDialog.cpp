#include "EditorFileDialog.h"

#include <vector>
#include <string>

#include <windows.h>
#include <commdlg.h>
#include <shobjidl.h>

namespace EditorFileDialog {
bool Open(char* outPath, size_t outPathSize, const char* filter)
{
    if (!outPath || outPathSize == 0)
        return false;

    // Initialize COM (handle RPC_E_CHANGED_MODE specially so we don't uninit incorrectly)
    bool needUninit = false;
    HRESULT hr = CoInitializeEx(NULL, COINIT_APARTMENTTHREADED);
    if (SUCCEEDED(hr)) {
        needUninit = true;
    } else if (hr == RPC_E_CHANGED_MODE) {
        // COM already initialized with different threading model; proceed but don't call CoUninitialize
        needUninit = false;
    } else {
        return false;
    }

    IFileOpenDialog* pDlg = nullptr;
    hr = CoCreateInstance(CLSID_FileOpenDialog, NULL, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&pDlg));
    if (FAILED(hr) || !pDlg) {
        if (needUninit) CoUninitialize();
        return false;
    }

    // Parse the filter string: pairs of display\0pattern\0 ... terminated by extra \0
    std::vector<std::wstring> holdWide; // keep wide strings alive
    std::vector<COMDLG_FILTERSPEC> specs;
    if (filter && filter[0]) {
        const char* p = filter;
        while (*p) {
            std::string name(p);
            p += name.size() + 1;
            if (!*p) break;
            std::string pattern(p);
            p += pattern.size() + 1;

            // Convert to wide (assume UTF-8 source)
            int n = MultiByteToWideChar(CP_UTF8, 0, name.c_str(), -1, NULL, 0);
            std::wstring wname(n, L'\0');
            MultiByteToWideChar(CP_UTF8, 0, name.c_str(), -1, &wname[0], n);

            n = MultiByteToWideChar(CP_UTF8, 0, pattern.c_str(), -1, NULL, 0);
            std::wstring wpattern(n, L'\0');
            MultiByteToWideChar(CP_UTF8, 0, pattern.c_str(), -1, &wpattern[0], n);

            holdWide.push_back(std::move(wname));
            holdWide.push_back(std::move(wpattern));
            COMDLG_FILTERSPEC spec;
            spec.pszName = holdWide[holdWide.size() - 2].c_str();
            spec.pszSpec = holdWide[holdWide.size() - 1].c_str();
            specs.push_back(spec);
        }
    }

    if (!specs.empty()) {
        pDlg->SetFileTypes(static_cast<UINT>(specs.size()), specs.data());
        pDlg->SetFileTypeIndex(1);
    }

    // Options: require existing file / path and avoid changing current dir (similar to OFN flags used before)
    DWORD options = 0;
    if (SUCCEEDED(pDlg->GetOptions(&options))) {
        options |= FOS_PATHMUSTEXIST | FOS_FILEMUSTEXIST | FOS_NOCHANGEDIR;
        pDlg->SetOptions(options);
    }

    // Show dialog
    hr = pDlg->Show(NULL);
    bool result = false;
    if (SUCCEEDED(hr)) {
        IShellItem* pItem = nullptr;
        hr = pDlg->GetResult(&pItem);
        if (SUCCEEDED(hr) && pItem) {
            PWSTR pszFilePath = nullptr;
            hr = pItem->GetDisplayName(SIGDN_FILESYSPATH, &pszFilePath);
            if (SUCCEEDED(hr) && pszFilePath) {
                // Convert wide path to UTF-8 into outPath
                int needed = WideCharToMultiByte(CP_UTF8, 0, pszFilePath, -1, NULL, 0, NULL, NULL);
                if (needed > 0 && static_cast<size_t>(needed) <= outPathSize) {
                    WideCharToMultiByte(CP_UTF8, 0, pszFilePath, -1, outPath, (int)outPathSize, NULL, NULL);
                    result = true;
                }
                CoTaskMemFree(pszFilePath);
            }
            pItem->Release();
        }
    }

    pDlg->Release();
    if (needUninit) CoUninitialize();
    return result;
}
} // namespace EditorFileDialog
