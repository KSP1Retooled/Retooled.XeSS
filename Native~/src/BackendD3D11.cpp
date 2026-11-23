#include "Backend.h"

#include <dxgi.h>

#include "IUnityGraphics.h"
#include "IUnityGraphicsD3D11.h"
#include "IUnityInterface.h"

namespace
{
    // XeSS's own Direct3D 11 runtime, which only supports Intel Arc GPUs. It runs on Unity's immediate context.
    class BackendD3D11 final : public Backend
    {
    public:
        BackendD3D11(const XessLibrary* xess, IUnityGraphicsD3D11* unity) : Backend(xess), unity(unity) {}

        xess_result_t Create(xess_context_handle_t* handle) override
        {
            return xess->D3D11CreateContext(unity->GetDevice(), handle);
        }

        xess_result_t BuildPipelines(xess_context_handle_t, uint32_t) override { return XESS_RESULT_SUCCESS; }

        bool HasPipelineBuild() const override { return false; }

        xess_result_t Init(xess_context_handle_t handle, const InitRequest& request) override
        {
            xess_d3d11_init_params_t init{};
            init.outputResolution = {request.width, request.height};
            init.qualitySetting = static_cast<xess_quality_settings_t>(request.quality);
            init.initFlags = request.flags;
            return xess->D3D11Init(handle, &init);
        }

        xess_result_t Execute(xess_context_handle_t handle, const ExecuteRequest& request) override
        {
            xess_d3d11_execute_params_t execute{};
            execute.pColorTexture = static_cast<ID3D11Resource*>(request.color);
            execute.pVelocityTexture = static_cast<ID3D11Resource*>(request.velocity);
            execute.pDepthTexture = static_cast<ID3D11Resource*>(request.depth);
            execute.pOutputTexture = static_cast<ID3D11Resource*>(request.output);
            execute.jitterOffsetX = request.jitterX;
            execute.jitterOffsetY = request.jitterY;
            execute.exposureScale = 1.0f;
            execute.resetHistory = request.resetHistory;
            execute.inputWidth = request.inputWidth;
            execute.inputHeight = request.inputHeight;
            return xess->D3D11Execute(handle, &execute);
        }

    private:
        IUnityGraphicsD3D11* unity;
    };
}

std::unique_ptr<Backend> CreateD3D11Backend(IUnityInterfaces* interfaces)
{
    // XeSS's Direct3D 11 runtime refuses GPUs other than Intel Arc when a context is created, so a trial context
    // decides between it and the Direct3D 12 device next to Unity's.
    auto* unity = UNITY_GET_INTERFACE(interfaces, IUnityGraphicsD3D11);
    const XessLibrary* xess = LoadXessD3D11Library();
    if (unity != nullptr && xess != nullptr && xess->D3D11CreateContext != nullptr)
    {
        xess_context_handle_t trial = nullptr;
        if (xess->D3D11CreateContext(unity->GetDevice(), &trial) == XESS_RESULT_SUCCESS)
        {
            xess->DestroyContext(trial);
            return std::make_unique<BackendD3D11>(xess, unity);
        }
    }
    return CreateD3D11On12Backend(interfaces);
}
