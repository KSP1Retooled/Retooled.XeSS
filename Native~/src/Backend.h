#pragma once

#include <cstdint>
#include <memory>

#include "XessLibrary.h"

#include <string>

struct IUnityInterfaces;

// Writes to the Unity log with an "[XeSS] " prefix, from any thread. The levels are Unity's UnityLogType values.
void PluginLog(int type, const std::string& message);
constexpr int LogError = 0;
constexpr int LogWarning = 2;
constexpr int LogInfo = 3;

struct InitRequest
{
    uint32_t width;
    uint32_t height;
    int32_t quality;
    uint32_t flags;
    float velocityScaleX;
    float velocityScaleY;
};

// Shared with C# as XessNative.ExecuteRequest, field for field. The textures are Unity's native texture pointers.
struct ExecuteRequest
{
    void* color;
    void* velocity;
    void* depth;
    void* output;
    float jitterX;
    float jitterY;
    uint32_t resetHistory;
    uint32_t inputWidth;
    uint32_t inputHeight;
};

// Runs the SDK on one graphics API. Every method is called on the render thread.
class Backend
{
public:
    explicit Backend(const XessLibrary* xess) : xess(xess) {}
    virtual ~Backend() = default;

    Backend(const Backend&) = delete;
    Backend& operator=(const Backend&) = delete;

    // The library that created this backend's contexts, for the calls that take only a context.
    const XessLibrary* const xess;

    virtual xess_result_t Create(xess_context_handle_t* handle) = 0;

    // Starts building pipelines in the background. Backends that build them during init return success.
    virtual xess_result_t BuildPipelines(xess_context_handle_t handle, uint32_t flags) = 0;

    // Whether GetPipelineBuildStatus applies. If not, pipelines count as built once the context exists.
    virtual bool HasPipelineBuild() const { return true; }

    virtual xess_result_t Init(xess_context_handle_t handle, const InitRequest& request) = 0;

    // Records the upscale into the command list Unity is recording, or runs it directly.
    virtual xess_result_t Execute(xess_context_handle_t handle, const ExecuteRequest& request) = 0;

    // Called once for the event ID that executes, before any event runs.
    virtual void ConfigureExecuteEvent(int eventId) {}

    // Called when a context goes away, so a backend can drop what it made for that context's textures.
    virtual void ForgetTextures() {}
};

// Each returns null if the API's interface or the SDK runtime is unavailable.
std::unique_ptr<Backend> CreateD3D12Backend(IUnityInterfaces* interfaces);
std::unique_ptr<Backend> CreateVulkanBackend(IUnityInterfaces* interfaces);

// XeSS's own Direct3D 11 runtime on Intel Arc, and CreateD3D11On12Backend everywhere else.
std::unique_ptr<Backend> CreateD3D11Backend(IUnityInterfaces* interfaces);

// XeSS on a Direct3D 12 device next to Unity's Direct3D 11 device, synchronized with shared fences.
std::unique_ptr<Backend> CreateD3D11On12Backend(IUnityInterfaces* interfaces);

// Hooks Unity's Vulkan instance and device creation to add what XeSS needs. It only takes effect if called from
// UnityPluginLoad before Unity creates the device, so the plugin must load on startup.
void InterceptVulkanInitialization(IUnityInterfaces* interfaces);

// Whether Unity's Vulkan device was created through the hooks, with XeSS's extensions and features.
bool VulkanDeviceReady();
