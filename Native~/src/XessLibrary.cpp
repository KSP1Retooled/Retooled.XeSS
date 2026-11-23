#include "XessLibrary.h"

#include <mutex>
#include <string>

namespace
{
    std::wstring PluginDirectory()
    {
        HMODULE self = nullptr;
        GetModuleHandleExW(
            GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
            reinterpret_cast<LPCWSTR>(&PluginDirectory),
            &self);

        std::wstring path(MAX_PATH, L'\0');
        while (true)
        {
            DWORD length = GetModuleFileNameW(self, path.data(), static_cast<DWORD>(path.size()));
            if (length < path.size())
            {
                path.resize(length);
                break;
            }
            path.resize(path.size() * 2);
        }
        return path.substr(0, path.find_last_of(L"\\/") + 1);
    }

    template <typename T>
    bool Resolve(HMODULE module, const char* name, T& function)
    {
        function = reinterpret_cast<T>(GetProcAddress(module, name));
        return function != nullptr;
    }

    bool Load(const wchar_t* name, XessLibrary& library)
    {
        // The altered search path lets the runtime find the DLLs it depends on in its own folder.
        std::wstring path = PluginDirectory() + name;
        HMODULE module = LoadLibraryExW(path.c_str(), nullptr, LOAD_WITH_ALTERED_SEARCH_PATH);
        if (module == nullptr)
            return false;

        bool common = Resolve(module, "xessGetVersion", library.GetVersion)
            && Resolve(module, "xessGetIntelXeFXVersion", library.GetIntelXeFXVersion)
            && Resolve(module, "xessDestroyContext", library.DestroyContext)
            && Resolve(module, "xessSetLoggingCallback", library.SetLoggingCallback)
            && Resolve(module, "xessIsOptimalDriver", library.IsOptimalDriver)
            && Resolve(module, "xessGetPipelineBuildStatus", library.GetPipelineBuildStatus)
            && Resolve(module, "xessSetVelocityScale", library.SetVelocityScale)
            && Resolve(module, "xessStartDump", library.StartDump)
            && Resolve(module, "xessGetProfilingData", library.GetProfilingData);

        Resolve(module, "xessD3D12CreateContext", library.D3D12CreateContext);
        Resolve(module, "xessD3D12BuildPipelines", library.D3D12BuildPipelines);
        Resolve(module, "xessD3D12Init", library.D3D12Init);
        Resolve(module, "xessD3D12Execute", library.D3D12Execute);
        Resolve(module, "xessVKGetRequiredInstanceExtensions", library.VKGetRequiredInstanceExtensions);
        Resolve(module, "xessVKGetRequiredDeviceExtensions", library.VKGetRequiredDeviceExtensions);
        Resolve(module, "xessVKGetRequiredDeviceFeatures", library.VKGetRequiredDeviceFeatures);
        Resolve(module, "xessVKCreateContext", library.VKCreateContext);
        Resolve(module, "xessVKBuildPipelines", library.VKBuildPipelines);
        Resolve(module, "xessVKInit", library.VKInit);
        Resolve(module, "xessVKExecute", library.VKExecute);
        Resolve(module, "xessD3D11CreateContext", library.D3D11CreateContext);
        Resolve(module, "xessD3D11Init", library.D3D11Init);
        Resolve(module, "xessD3D11Execute", library.D3D11Execute);
        return common;
    }

    XessLibrary main;
    bool mainLoaded;
    std::once_flag mainOnce;

    XessLibrary d3d11;
    bool d3d11Loaded;
    std::once_flag d3d11Once;
}

const XessLibrary* LoadXessLibrary()
{
    std::call_once(mainOnce, [] { mainLoaded = Load(L"libxess.dll", main); });
    return mainLoaded ? &main : nullptr;
}

const XessLibrary* LoadXessD3D11Library()
{
    std::call_once(d3d11Once, [] { d3d11Loaded = Load(L"libxess_dx11.dll", d3d11); });
    return d3d11Loaded ? &d3d11 : nullptr;
}
