#include "Backend.h"

#include <cstdio>
#include <string>

#include <d3d11_4.h>
#include <dxgi1_4.h>
#include <wrl/client.h>

#include "IUnityGraphics.h"
#include "IUnityGraphicsD3D11.h"
#include "IUnityInterface.h"

using Microsoft::WRL::ComPtr;

namespace
{
    // Command allocators in flight. The render thread waits for the oldest before reusing it.
    constexpr int FramesInFlight = 3;

    // Unity creates its render textures typeless so that it can view them in more than one format. XeSS views its
    // textures in their own format, so the shared copies use the typed format a shader would read.
    DXGI_FORMAT Typed(DXGI_FORMAT format)
    {
        switch (format)
        {
        case DXGI_FORMAT_R8G8B8A8_TYPELESS: return DXGI_FORMAT_R8G8B8A8_UNORM;
        case DXGI_FORMAT_B8G8R8A8_TYPELESS: return DXGI_FORMAT_B8G8R8A8_UNORM;
        case DXGI_FORMAT_R16G16_TYPELESS: return DXGI_FORMAT_R16G16_FLOAT;
        case DXGI_FORMAT_R16G16B16A16_TYPELESS: return DXGI_FORMAT_R16G16B16A16_FLOAT;
        case DXGI_FORMAT_R32_TYPELESS: return DXGI_FORMAT_R32_FLOAT;
        default: return format;
        }
    }

    // A texture made on Unity's Direct3D 11 device and opened on the Direct3D 12 device.
    struct SharedTexture
    {
        ComPtr<ID3D12Resource> d3d12;
        ComPtr<ID3D11Texture2D> d3d11;
        DXGI_FORMAT format = DXGI_FORMAT_UNKNOWN;
        UINT width = 0;
        UINT height = 0;
    };

    // Runs XeSS on a Direct3D 12 device next to Unity's Direct3D 11 device, for GPUs that XeSS's own Direct3D 11
    // runtime doesn't support. Each upscale copies Unity's inputs into shared textures and signals a shared fence.
    // The Direct3D 12 queue waits for it, runs XeSS and signals again, and Unity's context waits for that before it
    // copies the result back.
    class BackendD3D11On12 final : public Backend
    {
    public:
        BackendD3D11On12(const XessLibrary* xess, ComPtr<ID3D11Device5> device11, ComPtr<ID3D11DeviceContext4> context11,
            ComPtr<ID3D12Device> device12)
            : Backend(xess), device11(std::move(device11)), context11(std::move(context11)), device12(std::move(device12))
        {
        }

        bool Prepare()
        {
            D3D12_COMMAND_QUEUE_DESC queueDesc{};
            queueDesc.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
            if (FAILED(device12->CreateCommandQueue(&queueDesc, IID_PPV_ARGS(&queue))))
                return false;
            for (Frame& frame : frames)
            {
                if (FAILED(device12->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&frame.allocator))))
                    return false;
            }
            if (FAILED(device12->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, frames[0].allocator.Get(), nullptr,
                    IID_PPV_ARGS(&commands))))
                return false;
            commands->Close();

            HANDLE shared = nullptr;
            if (FAILED(device12->CreateFence(0, D3D12_FENCE_FLAG_SHARED, IID_PPV_ARGS(&fence12)))
                || FAILED(device12->CreateSharedHandle(fence12.Get(), nullptr, GENERIC_ALL, nullptr, &shared)))
                return false;
            HRESULT opened = device11->OpenSharedFence(shared, IID_PPV_ARGS(&fence11));
            CloseHandle(shared);
            completion = CreateEventW(nullptr, FALSE, FALSE, nullptr);
            return SUCCEEDED(opened) && completion != nullptr;
        }

        ~BackendD3D11On12() override
        {
            WaitForGpu();
            if (completion != nullptr)
                CloseHandle(completion);
        }

        xess_result_t Create(xess_context_handle_t* handle) override
        {
            return xess->D3D12CreateContext(device12.Get(), handle);
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
            auto* color = static_cast<ID3D11Texture2D*>(request.color);
            auto* velocity = static_cast<ID3D11Texture2D*>(request.velocity);
            auto* depth = static_cast<ID3D11Texture2D*>(request.depth);
            auto* output = static_cast<ID3D11Texture2D*>(request.output);
            if (!Match(sharedColor, color, false) || !Match(sharedVelocity, velocity, false)
                || !Match(sharedDepth, depth, false) || !Match(sharedOutput, output, true))
                return XESS_RESULT_ERROR_DEVICE;

            // Direct3D 11 hands the inputs over. The flush submits the copies and the signal, so the other queue can
            // start before Unity submits the rest of the frame.
            context11->CopyResource(sharedColor.d3d11.Get(), color);
            context11->CopyResource(sharedVelocity.d3d11.Get(), velocity);
            context11->CopyResource(sharedDepth.d3d11.Get(), depth);
            UINT64 inputsReady = ++fenceValue;
            context11->Signal(fence11.Get(), inputsReady);
            context11->Flush();

            Frame& frame = frames[frameIndex];
            frameIndex = (frameIndex + 1) % FramesInFlight;
            WaitForValue(frame.done);
            frame.allocator->Reset();
            commands->Reset(frame.allocator.Get(), nullptr);

            Transition(sharedColor, D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
            Transition(sharedVelocity, D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
            Transition(sharedDepth, D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
            Transition(sharedOutput, D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);

            xess_d3d12_execute_params_t execute{};
            execute.pColorTexture = sharedColor.d3d12.Get();
            execute.pVelocityTexture = sharedVelocity.d3d12.Get();
            execute.pDepthTexture = sharedDepth.d3d12.Get();
            execute.pOutputTexture = sharedOutput.d3d12.Get();
            execute.jitterOffsetX = request.jitterX;
            execute.jitterOffsetY = request.jitterY;
            execute.exposureScale = 1.0f;
            execute.resetHistory = request.resetHistory;
            execute.inputWidth = request.inputWidth;
            execute.inputHeight = request.inputHeight;
            xess_result_t result = xess->D3D12Execute(handle, commands.Get(), &execute);

            // Shared textures go back to the common state for the other device.
            Transition(sharedColor, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_COMMON);
            Transition(sharedVelocity, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_COMMON);
            Transition(sharedDepth, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_COMMON);
            Transition(sharedOutput, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COMMON);
            commands->Close();

            queue->Wait(fence12.Get(), inputsReady);
            ID3D12CommandList* lists[] = {commands.Get()};
            queue->ExecuteCommandLists(1, lists);
            UINT64 outputReady = ++fenceValue;
            queue->Signal(fence12.Get(), outputReady);
            frame.done = outputReady;

            // Unity's later work on this context waits on the GPU for the result, not on the CPU.
            context11->Wait(fence11.Get(), outputReady);
            context11->CopyResource(output, sharedOutput.d3d11.Get());
            return result;
        }

        void ForgetTextures() override
        {
            WaitForGpu();
            sharedColor = {};
            sharedVelocity = {};
            sharedDepth = {};
            sharedOutput = {};
        }

    private:
        struct Frame
        {
            ComPtr<ID3D12CommandAllocator> allocator;
            UINT64 done = 0;
        };

        // Makes the shared texture match Unity's texture in size and format, creating it again if needed.
        bool Match(SharedTexture& shared, ID3D11Texture2D* source, bool unorderedAccess)
        {
            if (source == nullptr)
                return false;

            D3D11_TEXTURE2D_DESC desc{};
            source->GetDesc(&desc);
            DXGI_FORMAT format = Typed(desc.Format);
            if (shared.d3d11 != nullptr && shared.format == format && shared.width == desc.Width
                && shared.height == desc.Height)
                return true;

            WaitForGpu();
            shared = {};

            // Direct3D 11 refuses to open textures that Direct3D 12 shares, so the texture is made on Direct3D 11 and
            // opened on Direct3D 12. The fences order the two devices' access, so no keyed mutex is needed.
            D3D11_TEXTURE2D_DESC create{};
            create.Width = desc.Width;
            create.Height = desc.Height;
            create.MipLevels = 1;
            create.ArraySize = 1;
            create.Format = format;
            create.SampleDesc.Count = 1;
            create.Usage = D3D11_USAGE_DEFAULT;
            create.BindFlags = D3D11_BIND_SHADER_RESOURCE | (unorderedAccess ? D3D11_BIND_UNORDERED_ACCESS : 0);
            create.MiscFlags = D3D11_RESOURCE_MISC_SHARED | D3D11_RESOURCE_MISC_SHARED_NTHANDLE;
            HRESULT created = device11->CreateTexture2D(&create, nullptr, &shared.d3d11);
            if (FAILED(created))
                return Report("create a shared texture", created, format);

            ComPtr<IDXGIResource1> resource;
            HANDLE handle = nullptr;
            HRESULT handled = shared.d3d11.As(&resource);
            if (SUCCEEDED(handled))
                handled = resource->CreateSharedHandle(nullptr, DXGI_SHARED_RESOURCE_READ | DXGI_SHARED_RESOURCE_WRITE,
                    nullptr, &handle);
            if (FAILED(handled))
                return Report("share a texture", handled, format);
            HRESULT opened = device12->OpenSharedHandle(handle, IID_PPV_ARGS(&shared.d3d12));
            CloseHandle(handle);
            if (FAILED(opened))
                return Report("open a shared texture on Direct3D 12", opened, format);

            shared.format = format;
            shared.width = desc.Width;
            shared.height = desc.Height;
            return true;
        }

        // Logs the first failure only, because the upscale retries every frame.
        bool Report(const char* action, HRESULT result, DXGI_FORMAT format)
        {
            if (!reported)
            {
                char hex[16];
                std::snprintf(hex, sizeof(hex), "0x%08lX", static_cast<unsigned long>(result));
                PluginLog(LogError, std::string("cannot ") + action + " (format " + std::to_string(format) + "): " + hex);
                reported = true;
            }
            return false;
        }

        void Transition(const SharedTexture& texture, D3D12_RESOURCE_STATES before, D3D12_RESOURCE_STATES after)
        {
            D3D12_RESOURCE_BARRIER barrier{};
            barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
            barrier.Transition.pResource = texture.d3d12.Get();
            barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
            barrier.Transition.StateBefore = before;
            barrier.Transition.StateAfter = after;
            commands->ResourceBarrier(1, &barrier);
        }

        void WaitForValue(UINT64 value)
        {
            if (value == 0 || fence12->GetCompletedValue() >= value)
                return;
            fence12->SetEventOnCompletion(value, completion);
            WaitForSingleObject(completion, INFINITE);
        }

        void WaitForGpu()
        {
            if (queue == nullptr || fence12 == nullptr)
                return;
            UINT64 value = ++fenceValue;
            queue->Signal(fence12.Get(), value);
            WaitForValue(value);
        }

        ComPtr<ID3D11Device5> device11;
        ComPtr<ID3D11DeviceContext4> context11;
        ComPtr<ID3D12Device> device12;
        ComPtr<ID3D12CommandQueue> queue;
        ComPtr<ID3D12GraphicsCommandList> commands;
        ComPtr<ID3D12Fence> fence12;
        ComPtr<ID3D11Fence> fence11;
        HANDLE completion = nullptr;
        UINT64 fenceValue = 0;
        Frame frames[FramesInFlight];
        int frameIndex = 0;
        SharedTexture sharedColor;
        SharedTexture sharedVelocity;
        SharedTexture sharedDepth;
        SharedTexture sharedOutput;
        bool reported = false;
    };

    // The Direct3D 12 device has to be on the adapter that Unity's device uses, so the shared textures stay in that
    // GPU's memory.
    ComPtr<ID3D12Device> CreateDeviceOnAdapter(ID3D11Device* device11)
    {
        ComPtr<IDXGIDevice> dxgiDevice;
        ComPtr<IDXGIAdapter> adapter;
        if (FAILED(device11->QueryInterface(IID_PPV_ARGS(&dxgiDevice))) || FAILED(dxgiDevice->GetAdapter(&adapter)))
            return nullptr;

        // d3d12.dll is loaded only here, so the plugin still loads where Direct3D 12 is missing.
        HMODULE d3d12 = LoadLibraryW(L"d3d12.dll");
        auto create = d3d12 != nullptr
            ? reinterpret_cast<PFN_D3D12_CREATE_DEVICE>(GetProcAddress(d3d12, "D3D12CreateDevice"))
            : nullptr;
        ComPtr<ID3D12Device> device12;
        if (create == nullptr || FAILED(create(adapter.Get(), D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&device12))))
            return nullptr;
        return device12;
    }
}

std::unique_ptr<Backend> CreateD3D11On12Backend(IUnityInterfaces* interfaces)
{
    auto* unity = UNITY_GET_INTERFACE(interfaces, IUnityGraphicsD3D11);
    const XessLibrary* xess = LoadXessLibrary();
    if (unity == nullptr || xess == nullptr || xess->D3D12CreateContext == nullptr)
        return nullptr;

    // Shared fences need Direct3D 11.4.
    ComPtr<ID3D11Device5> device11;
    ComPtr<ID3D11DeviceContext> immediate;
    ComPtr<ID3D11DeviceContext4> context11;
    if (FAILED(unity->GetDevice()->QueryInterface(IID_PPV_ARGS(&device11))))
        return nullptr;
    device11->GetImmediateContext(&immediate);
    if (FAILED(immediate.As(&context11)))
        return nullptr;

    ComPtr<ID3D12Device> device12 = CreateDeviceOnAdapter(device11.Get());
    if (device12 == nullptr)
        return nullptr;

    auto backend = std::make_unique<BackendD3D11On12>(xess, device11, context11, device12);
    if (!backend->Prepare())
        return nullptr;
    return backend;
}
