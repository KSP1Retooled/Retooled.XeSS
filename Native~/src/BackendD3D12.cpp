#include "Backend.h"

#include <dxgi.h>

#include "IUnityGraphicsD3D12.h"
#include "IUnityInterface.h"

namespace
{
    class BackendD3D12 final : public Backend
    {
    public:
        BackendD3D12(const XessLibrary* xess, IUnityGraphicsD3D12v8* unity) : Backend(xess), unity(unity) {}

        xess_result_t Create(xess_context_handle_t* handle) override
        {
            return xess->D3D12CreateContext(unity->GetDevice(), handle);
        }

        xess_result_t BuildPipelines(xess_context_handle_t handle, uint32_t flags) override
        {
            return xess->D3D12BuildPipelines(handle, nullptr, false, flags);
        }

        xess_result_t Init(xess_context_handle_t handle, const InitRequest& request) override
        {
            xess_d3d12_init_params_t init{};
            init.outputResolution = {request.width, request.height};
            init.qualitySetting = static_cast<xess_quality_settings_t>(request.quality);
            init.initFlags = request.flags;
            return xess->D3D12Init(handle, &init);
        }

        xess_result_t Execute(xess_context_handle_t handle, const ExecuteRequest& request) override
        {
            UnityGraphicsD3D12RecordingState recording{};
            if (!unity->CommandRecordingState(&recording) || recording.commandList == nullptr)
                return XESS_RESULT_ERROR_WRONG_CALL_ORDER;

            auto* color = static_cast<ID3D12Resource*>(request.color);
            auto* velocity = static_cast<ID3D12Resource*>(request.velocity);
            auto* depth = static_cast<ID3D12Resource*>(request.depth);
            auto* output = static_cast<ID3D12Resource*>(request.output);
            unity->RequestResourceState(color, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
            unity->RequestResourceState(velocity, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
            unity->RequestResourceState(depth, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
            unity->RequestResourceState(output, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);

            xess_d3d12_execute_params_t execute{};
            execute.pColorTexture = color;
            execute.pVelocityTexture = velocity;
            execute.pDepthTexture = depth;
            execute.pOutputTexture = output;
            execute.jitterOffsetX = request.jitterX;
            execute.jitterOffsetY = request.jitterY;
            execute.exposureScale = 1.0f;
            execute.resetHistory = request.resetHistory;
            execute.inputWidth = request.inputWidth;
            execute.inputHeight = request.inputHeight;
            xess_result_t result = xess->D3D12Execute(handle, recording.commandList, &execute);

            // Unity adds a UAV barrier before anything reads the output.
            unity->NotifyResourceState(output, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, true);
            return result;
        }

        void ConfigureExecuteEvent(int eventId) override
        {
            // XeSS binds its own descriptor heap and pipeline, so Unity has to restore its bindings afterwards.
            UnityD3D12PluginEventConfig config{};
            config.graphicsQueueAccess = kUnityD3D12GraphicsQueueAccess_DontCare;
            config.flags = kUnityD3D12EventConfigFlag_ModifiesCommandBuffersState;
            config.ensureActiveRenderTextureIsBound = false;
            unity->ConfigureEvent(eventId, &config);
        }

    private:
        IUnityGraphicsD3D12v8* unity;
    };
}

std::unique_ptr<Backend> CreateD3D12Backend(IUnityInterfaces* interfaces)
{
    auto* unity = UNITY_GET_INTERFACE(interfaces, IUnityGraphicsD3D12v8);
    const XessLibrary* xess = LoadXessLibrary();
    if (unity == nullptr || xess == nullptr || xess->D3D12CreateContext == nullptr)
        return nullptr;
    return std::make_unique<BackendD3D12>(xess, unity);
}
