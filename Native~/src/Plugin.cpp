#include <algorithm>
#include <climits>
#include <cstdint>
#include <cstring>
#include <deque>
#include <mutex>
#include <string>
#include <vector>

#include "Backend.h"

#include "IUnityGraphics.h"
#include "IUnityInterface.h"
#include "IUnityLog.h"

namespace
{
    // A result that has not been produced yet. It is outside the range of xess_result_t.
    constexpr int32_t NotRun = INT32_MAX;

    // Execute requests that pile up past this, because their events never ran, are dropped oldest first.
    constexpr size_t MaxQueuedExecutes = 4;

    enum class Stage : int32_t
    {
        Pending = 0,
        Created = 1,
        Initialized = 2,
        Failed = -1,
    };

    enum Event
    {
        CreateEvent = 0,
        InitEvent = 1,
        PollEvent = 2,
        DumpEvent = 3,
        DestroyEvent = 4,
        ExecuteEvent = 5,
    };

    // Shared with C# as XessNative.Status, field for field.
    struct Status
    {
        Stage stage = Stage::Pending;
        int32_t createResult = NotRun;
        int32_t buildResult = NotRun;
        int32_t pipelineResult = NotRun;
        int32_t initResult = NotRun;
        int32_t driverResult = NotRun;
        int32_t profilingResult = NotRun;
        int32_t dumpResult = NotRun;
        int32_t executeResult = NotRun;
        int32_t executeCount = 0;
        int32_t xefxMajor = 0;
        int32_t xefxMinor = 0;
        int32_t xefxPatch = 0;
    };

    struct ProfilingRecord
    {
        std::string name;
        double seconds;
    };

    // The main thread sets the requests and reads the status under the mutex. Only the render thread uses the handle.
    struct Context
    {
        std::mutex mutex;
        Status status;
        InitRequest init{};
        std::deque<ExecuteRequest> executes;
        std::string dumpPath;
        xess_dump_parameters_t dump{};
        std::vector<ProfilingRecord> profiling;

        xess_context_handle_t handle = nullptr;
        bool profilingEnabled = false;
    };

    IUnityInterfaces* interfaces;
    IUnityGraphics* graphics;
    IUnityLog* unityLog;
    UnityGfxRenderer renderer = kUnityGfxRendererNull;
    xess_logging_level_t logLevel = XESS_LOGGING_LEVEL_WARNING;

    // Created with the first context, so that the SDK runtime is only loaded once a game asks for XeSS.
    std::unique_ptr<Backend> backend;
    bool backendTried;

    void Log(UnityLogType type, const std::string& message)
    {
        if (unityLog != nullptr)
            unityLog->Log(type, ("[XeSS] " + message).c_str(), "", 0);
    }
}

void PluginLog(int type, const std::string& message)
{
    Log(static_cast<UnityLogType>(type), message);
}

namespace
{

    // XeSS may call this from its own threads. Unity's log accepts messages from any thread.
    void OnXessLog(const char* message, xess_logging_level_t level)
    {
        UnityLogType type = level >= XESS_LOGGING_LEVEL_ERROR ? kUnityLogTypeError
            : level == XESS_LOGGING_LEVEL_WARNING ? kUnityLogTypeWarning
            : kUnityLogTypeLog;
        Log(type, message);
    }

    Backend* GetBackend()
    {
        if (backendTried)
            return backend.get();

        backendTried = true;
        if (renderer == kUnityGfxRendererD3D12)
            backend = CreateD3D12Backend(interfaces);
        else if (renderer == kUnityGfxRendererVulkan)
            backend = CreateVulkanBackend(interfaces);
        else if (renderer == kUnityGfxRendererD3D11)
            backend = CreateD3D11Backend(interfaces);
        if (backend != nullptr)
            backend->ConfigureExecuteEvent(ExecuteEvent);
        return backend.get();
    }

    void UNITY_INTERFACE_API OnGraphicsDeviceEvent(UnityGfxDeviceEventType type)
    {
        if (type == kUnityGfxDeviceEventInitialize)
        {
            renderer = graphics->GetRenderer();
        }
        else if (type == kUnityGfxDeviceEventShutdown)
        {
            renderer = kUnityGfxRendererNull;
            backend.reset();
            backendTried = false;
        }
    }

    void Fail(Context& context, int32_t Status::*field, int32_t result)
    {
        std::lock_guard lock(context.mutex);
        context.status.*field = result;
        context.status.stage = Stage::Failed;
    }

    void Create(Context& context)
    {
        Backend* api = GetBackend();
        if (api == nullptr)
        {
            bool unsupported = renderer != kUnityGfxRendererD3D12 && renderer != kUnityGfxRendererVulkan
                && renderer != kUnityGfxRendererD3D11;
            if (!unsupported)
                Log(kUnityLogTypeError, "cannot load the XeSS runtime or set up its device");
            Fail(context, &Status::createResult,
                unsupported ? XESS_RESULT_ERROR_UNSUPPORTED : XESS_RESULT_ERROR_CANT_LOAD_LIBRARY);
            return;
        }

        xess_result_t result = api->Create(&context.handle);
        if (result != XESS_RESULT_SUCCESS)
        {
            context.handle = nullptr;
            Fail(context, &Status::createResult, result);
            return;
        }

        const XessLibrary* xess = api->xess;
        xess->SetLoggingCallback(context.handle, logLevel, OnXessLog);
        xess_result_t driver = xess->IsOptimalDriver(context.handle);
        xess_version_t xefx{};
        xess->GetIntelXeFXVersion(context.handle, &xefx);

        uint32_t flags;
        {
            std::lock_guard lock(context.mutex);
            flags = context.init.flags;
        }
        xess_result_t build = api->BuildPipelines(context.handle, flags);

        std::lock_guard lock(context.mutex);
        context.status.createResult = result;
        context.status.driverResult = driver;
        context.status.buildResult = build;
        if (!api->HasPipelineBuild())
            context.status.pipelineResult = XESS_RESULT_SUCCESS;
        context.status.xefxMajor = xefx.major;
        context.status.xefxMinor = xefx.minor;
        context.status.xefxPatch = xefx.patch;
        context.status.stage = build < XESS_RESULT_SUCCESS ? Stage::Failed : Stage::Created;
    }

    void Init(Context& context)
    {
        InitRequest init;
        {
            std::lock_guard lock(context.mutex);
            init = context.init;
        }

        xess_result_t result = backend->Init(context.handle, init);
        if (result != XESS_RESULT_SUCCESS)
        {
            Fail(context, &Status::initResult, result);
            return;
        }

        const XessLibrary* xess = backend->xess;
        xess->SetVelocityScale(context.handle, init.velocityScaleX, init.velocityScaleY);
        context.profilingEnabled = (init.flags & XESS_DEBUG_ENABLE_PROFILING) != 0;
        int32_t profiling = NotRun;
        if (context.profilingEnabled)
        {
            xess_profiling_data_t* data = nullptr;
            profiling = xess->GetProfilingData(context.handle, &data);
            context.profilingEnabled = profiling == XESS_RESULT_SUCCESS;
        }

        std::lock_guard lock(context.mutex);
        context.status.initResult = result;
        context.status.profilingResult = profiling;
        context.status.stage = Stage::Initialized;
    }

    void Poll(Context& context)
    {
        const XessLibrary* xess = backend->xess;

        // After init the SDK answers the pipeline query with a wrong call order error, so the last answer stands.
        bool building;
        {
            std::lock_guard lock(context.mutex);
            building = context.status.stage == Stage::Created && backend->HasPipelineBuild();
        }
        int32_t pipeline = building ? xess->GetPipelineBuildStatus(context.handle) : NotRun;

        // Draining the profiling data every poll keeps the SDK's buffers from growing. Only the newest frame is kept.
        std::vector<ProfilingRecord> records;
        bool haveRecords = false;
        if (context.profilingEnabled)
        {
            xess_profiling_data_t* data = nullptr;
            if (xess->GetProfilingData(context.handle, &data) == XESS_RESULT_SUCCESS && data != nullptr
                && data->frame_count > 0)
            {
                const xess_profiled_frame_data_t& frame = data->frames[data->frame_count - 1];
                for (uint64_t i = 0; i < frame.gpu_duration_record_count; i++)
                    records.push_back({frame.gpu_duration_names[i], frame.gpu_duration_values[i]});
                haveRecords = true;
            }
        }

        std::lock_guard lock(context.mutex);
        if (building)
            context.status.pipelineResult = pipeline;
        if (haveRecords)
            context.profiling = std::move(records);
    }

    void Dump(Context& context)
    {
        xess_dump_parameters_t dump;
        std::string path;
        {
            std::lock_guard lock(context.mutex);
            dump = context.dump;
            path = context.dumpPath;
        }
        dump.path = path.c_str();

        xess_result_t result = backend->xess->StartDump(context.handle, &dump);
        std::lock_guard lock(context.mutex);
        context.status.dumpResult = result;
    }

    void Execute(Context& context)
    {
        ExecuteRequest request;
        {
            std::lock_guard lock(context.mutex);
            if (context.executes.empty() || context.status.stage != Stage::Initialized)
                return;
            request = context.executes.front();
            context.executes.pop_front();
        }

        xess_result_t result = backend->Execute(context.handle, request);
        std::lock_guard lock(context.mutex);
        context.status.executeResult = result;
        context.status.executeCount++;
    }

    void Destroy(Context* context)
    {
        if (context->handle != nullptr)
        {
            backend->ForgetTextures();
            backend->xess->DestroyContext(context->handle);
        }
        delete context;
    }

    void UNITY_INTERFACE_API OnRenderEvent(int event, void* data)
    {
        auto* context = static_cast<Context*>(data);
        if (event == DestroyEvent)
        {
            Destroy(context);
            return;
        }
        if (event == CreateEvent)
        {
            Create(*context);
            return;
        }

        // Later requests on a context that failed, or was never created, have nothing to act on.
        if (context->handle == nullptr)
            return;

        switch (event)
        {
        case InitEvent:
            Init(*context);
            break;
        case PollEvent:
            Poll(*context);
            break;
        case DumpEvent:
            Dump(*context);
            break;
        case ExecuteEvent:
            Execute(*context);
            break;
        default:
            break;
        }
    }
}

extern "C"
{
    UNITY_INTERFACE_EXPORT void UNITY_INTERFACE_API UnityPluginLoad(IUnityInterfaces* unityInterfaces)
    {
        interfaces = unityInterfaces;
        graphics = UNITY_GET_INTERFACE(interfaces, IUnityGraphics);
        unityLog = UNITY_GET_INTERFACE(interfaces, IUnityLog);
        graphics->RegisterDeviceEventCallback(OnGraphicsDeviceEvent);
        InterceptVulkanInitialization(interfaces);

        // A plugin loaded after the device was created misses the initialize event.
        OnGraphicsDeviceEvent(kUnityGfxDeviceEventInitialize);
    }

    UNITY_INTERFACE_EXPORT void UNITY_INTERFACE_API UnityPluginUnload()
    {
        graphics->UnregisterDeviceEventCallback(OnGraphicsDeviceEvent);
    }

    UNITY_INTERFACE_EXPORT int RetooledXeSS_GetRenderer()
    {
        return renderer;
    }

    // Whether XeSS can run on Unity's Vulkan device. Meaningful once Unity has created the device.
    UNITY_INTERFACE_EXPORT int RetooledXeSS_IsVulkanDeviceReady()
    {
        return VulkanDeviceReady() ? 1 : 0;
    }

    UNITY_INTERFACE_EXPORT int RetooledXeSS_GetSdkVersion(int* major, int* minor, int* patch)
    {
        const XessLibrary* xess = LoadXessLibrary();
        if (xess == nullptr)
            return XESS_RESULT_ERROR_CANT_LOAD_LIBRARY;

        xess_version_t version{};
        xess_result_t result = xess->GetVersion(&version);
        *major = version.major;
        *minor = version.minor;
        *patch = version.patch;
        return result;
    }

    // Applies to contexts created afterwards.
    UNITY_INTERFACE_EXPORT void RetooledXeSS_SetLogLevel(int level)
    {
        logLevel = static_cast<xess_logging_level_t>(level);
    }

    UNITY_INTERFACE_EXPORT UnityRenderingEventAndData RetooledXeSS_GetEventFunction()
    {
        return OnRenderEvent;
    }

    // The context is freed by its destroy event on the render thread.
    UNITY_INTERFACE_EXPORT Context* RetooledXeSS_NewContext()
    {
        return new Context();
    }

    UNITY_INTERFACE_EXPORT void RetooledXeSS_SetInitParams(
        Context* context, uint32_t width, uint32_t height, int quality, uint32_t flags, float velocityScaleX,
        float velocityScaleY)
    {
        std::lock_guard lock(context->mutex);
        context->init = {width, height, quality, flags, velocityScaleX, velocityScaleY};
    }

    // Each execute event takes the oldest queued request, so queue exactly one per event that will run.
    UNITY_INTERFACE_EXPORT void RetooledXeSS_QueueExecute(Context* context, const ExecuteRequest* request)
    {
        std::lock_guard lock(context->mutex);
        context->executes.push_back(*request);
        while (context->executes.size() > MaxQueuedExecutes)
            context->executes.pop_front();
    }

    UNITY_INTERFACE_EXPORT void RetooledXeSS_SetDumpParams(
        Context* context, const char* path, uint32_t firstFrame, uint32_t frameCount, uint32_t elements)
    {
        std::lock_guard lock(context->mutex);
        context->dumpPath = path;
        context->dump = {};
        context->dump.frame_idx = firstFrame;
        context->dump.frame_count = frameCount;
        context->dump.dump_elements_mask = elements;
    }

    UNITY_INTERFACE_EXPORT void RetooledXeSS_GetStatus(Context* context, Status* status)
    {
        std::lock_guard lock(context->mutex);
        *status = context->status;
    }

    // Returns the number of records from the newest profiled frame. The name is truncated to fit the buffer.
    UNITY_INTERFACE_EXPORT int RetooledXeSS_GetProfilingRecord(
        Context* context, int index, char* name, int nameCapacity, double* seconds)
    {
        std::lock_guard lock(context->mutex);
        int count = static_cast<int>(context->profiling.size());
        if (index < 0 || index >= count || nameCapacity <= 0)
            return count;

        const ProfilingRecord& record = context->profiling[index];
        size_t length = std::min(record.name.size(), static_cast<size_t>(nameCapacity - 1));
        std::memcpy(name, record.name.data(), length);
        name[length] = '\0';
        *seconds = record.seconds;
        return count;
    }
}
