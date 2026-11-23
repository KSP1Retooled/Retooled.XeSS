#pragma once

#include <windows.h>
#include <d3d11.h>
#include <d3d12.h>
#include <vulkan/vulkan.h>

#include "xess/xess.h"
#include "xess/xess_d3d11.h"
#include "xess/xess_d3d12.h"
#include "xess/xess_debug.h"
#include "xess/xess_vk.h"

// The SDK runtime is loaded by path at first use, so that games which never select XeSS don't load it, and so that it
// is found next to this plugin rather than through the process's DLL search path.
//
// libxess.dll serves Direct3D 12 and Vulkan, and libxess_dx11.dll serves Direct3D 11. Both export the functions that
// take only a context, but each only accepts its own contexts, so a backend calls the library that created its context.
struct XessLibrary
{
    decltype(&xessGetVersion) GetVersion;
    decltype(&xessGetIntelXeFXVersion) GetIntelXeFXVersion;
    decltype(&xessDestroyContext) DestroyContext;
    decltype(&xessSetLoggingCallback) SetLoggingCallback;
    decltype(&xessIsOptimalDriver) IsOptimalDriver;
    decltype(&xessGetPipelineBuildStatus) GetPipelineBuildStatus;
    decltype(&xessSetVelocityScale) SetVelocityScale;
    decltype(&xessStartDump) StartDump;
    decltype(&xessGetProfilingData) GetProfilingData;

    // Null in libxess_dx11.dll.
    decltype(&xessD3D12CreateContext) D3D12CreateContext;
    decltype(&xessD3D12BuildPipelines) D3D12BuildPipelines;
    decltype(&xessD3D12Init) D3D12Init;
    decltype(&xessD3D12Execute) D3D12Execute;
    decltype(&xessVKGetRequiredInstanceExtensions) VKGetRequiredInstanceExtensions;
    decltype(&xessVKGetRequiredDeviceExtensions) VKGetRequiredDeviceExtensions;
    decltype(&xessVKGetRequiredDeviceFeatures) VKGetRequiredDeviceFeatures;
    decltype(&xessVKCreateContext) VKCreateContext;
    decltype(&xessVKBuildPipelines) VKBuildPipelines;
    decltype(&xessVKInit) VKInit;
    decltype(&xessVKExecute) VKExecute;

    // Null in libxess.dll.
    decltype(&xessD3D11CreateContext) D3D11CreateContext;
    decltype(&xessD3D11Init) D3D11Init;
    decltype(&xessD3D11Execute) D3D11Execute;
};

// Each returns null if the DLL or one of the functions every library has is missing. Safe to call from any thread.
const XessLibrary* LoadXessLibrary();
const XessLibrary* LoadXessD3D11Library();
