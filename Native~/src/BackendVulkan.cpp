#include "Backend.h"

#include <cstring>
#include <string>
#include <unordered_map>
#include <vector>

#include "IUnityGraphics.h"
#include "IUnityGraphicsVulkan.h"
#include "IUnityInterface.h"
#include "IUnityLog.h"

namespace
{
    // Instance and device creation

    // Unity creates its Vulkan instance and device through these, so XeSS's extensions and features can be added.
    PFN_vkGetInstanceProcAddr loaderGetInstanceProcAddr;
    PFN_vkCreateInstance loaderCreateInstance;
    PFN_vkCreateDevice loaderCreateDevice;
    VkInstance createdInstance;
    bool deviceReady;

    void Log(UnityLogType type, const std::string& message)
    {
        PluginLog(type, message);
    }

    void AddMissing(std::vector<const char*>& names, uint32_t count, const char* const* added)
    {
        for (uint32_t i = 0; i < count; i++)
        {
            bool present = false;
            for (const char* name : names)
                present = present || std::strcmp(name, added[i]) == 0;
            if (!present)
                names.push_back(added[i]);
        }
    }

    VKAPI_ATTR VkResult VKAPI_CALL CreateInstance(
        const VkInstanceCreateInfo* info, const VkAllocationCallbacks* allocator, VkInstance* instance)
    {
        const XessLibrary* xess = LoadXessLibrary();
        uint32_t count = 0;
        const char* const* required = nullptr;
        uint32_t minVersion = 0;
        if (xess == nullptr || xess->VKGetRequiredInstanceExtensions(&count, &required, &minVersion) != XESS_RESULT_SUCCESS)
            return loaderCreateInstance(info, allocator, instance);

        std::vector<const char*> extensions(info->ppEnabledExtensionNames,
            info->ppEnabledExtensionNames + info->enabledExtensionCount);
        AddMissing(extensions, count, required);

        VkApplicationInfo application = info->pApplicationInfo != nullptr ? *info->pApplicationInfo : VkApplicationInfo{
            VK_STRUCTURE_TYPE_APPLICATION_INFO};
        if (application.apiVersion < minVersion)
            application.apiVersion = minVersion;

        VkInstanceCreateInfo patched = *info;
        patched.pApplicationInfo = &application;
        patched.enabledExtensionCount = static_cast<uint32_t>(extensions.size());
        patched.ppEnabledExtensionNames = extensions.data();
        VkResult result = loaderCreateInstance(&patched, allocator, instance);
        if (result != VK_SUCCESS)
        {
            Log(kUnityLogTypeWarning, "the Vulkan instance refused XeSS's extensions; XeSS is unavailable");
            return loaderCreateInstance(info, allocator, instance);
        }
        createdInstance = *instance;
        return result;
    }

    VKAPI_ATTR VkResult VKAPI_CALL CreateDevice(VkPhysicalDevice physicalDevice, const VkDeviceCreateInfo* info,
        const VkAllocationCallbacks* allocator, VkDevice* device)
    {
        const XessLibrary* xess = LoadXessLibrary();
        uint32_t count = 0;
        const char* const* required = nullptr;
        if (createdInstance == VK_NULL_HANDLE || xess == nullptr
            || xess->VKGetRequiredDeviceExtensions(createdInstance, physicalDevice, &count, &required) != XESS_RESULT_SUCCESS)
            return loaderCreateDevice(physicalDevice, info, allocator, device);

        std::vector<const char*> extensions(info->ppEnabledExtensionNames,
            info->ppEnabledExtensionNames + info->enabledExtensionCount);
        AddMissing(extensions, count, required);

        // The SDK patches a features chain and can't use pEnabledFeatures, so those move into a features2 struct.
        VkPhysicalDeviceFeatures2 features2{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2};
        void* chain = const_cast<void*>(info->pNext);
        if (info->pEnabledFeatures != nullptr)
        {
            features2.features = *info->pEnabledFeatures;
            features2.pNext = chain;
            chain = &features2;
        }
        if (xess->VKGetRequiredDeviceFeatures(createdInstance, physicalDevice, &chain) != XESS_RESULT_SUCCESS)
            return loaderCreateDevice(physicalDevice, info, allocator, device);

        VkDeviceCreateInfo patched = *info;
        patched.pNext = chain;
        patched.pEnabledFeatures = nullptr;
        patched.enabledExtensionCount = static_cast<uint32_t>(extensions.size());
        patched.ppEnabledExtensionNames = extensions.data();
        VkResult result = loaderCreateDevice(physicalDevice, &patched, allocator, device);
        if (result != VK_SUCCESS)
        {
            Log(kUnityLogTypeWarning, "the Vulkan device refused XeSS's extensions or features; XeSS is unavailable");
            return loaderCreateDevice(physicalDevice, info, allocator, device);
        }
        deviceReady = true;
        return result;
    }

    VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL GetInstanceProcAddr(VkInstance instance, const char* name)
    {
        if (std::strcmp(name, "vkCreateInstance") == 0)
        {
            loaderCreateInstance = reinterpret_cast<PFN_vkCreateInstance>(loaderGetInstanceProcAddr(instance, name));
            return reinterpret_cast<PFN_vkVoidFunction>(&CreateInstance);
        }
        if (std::strcmp(name, "vkCreateDevice") == 0)
        {
            loaderCreateDevice = reinterpret_cast<PFN_vkCreateDevice>(loaderGetInstanceProcAddr(instance, name));
            return reinterpret_cast<PFN_vkVoidFunction>(&CreateDevice);
        }
        return loaderGetInstanceProcAddr(instance, name);
    }

    PFN_vkGetInstanceProcAddr UNITY_INTERFACE_API InterceptInitialization(PFN_vkGetInstanceProcAddr getInstanceProcAddr, void*)
    {
        loaderGetInstanceProcAddr = getInstanceProcAddr;
        return &GetInstanceProcAddr;
    }

    // Upscaling

    class BackendVulkan final : public Backend
    {
    public:
        BackendVulkan(const XessLibrary* xess, IUnityGraphicsVulkanV2* unity) : Backend(xess), unity(unity)
        {
            instance = unity->Instance();
            auto getDeviceProcAddr = reinterpret_cast<PFN_vkGetDeviceProcAddr>(
                instance.getInstanceProcAddr(instance.instance, "vkGetDeviceProcAddr"));
            createImageView = reinterpret_cast<PFN_vkCreateImageView>(getDeviceProcAddr(instance.device, "vkCreateImageView"));
            destroyImageView = reinterpret_cast<PFN_vkDestroyImageView>(getDeviceProcAddr(instance.device, "vkDestroyImageView"));
        }

        ~BackendVulkan() override
        {
            for (auto& [image, view] : views)
                destroyImageView(instance.device, view, nullptr);
            for (const Retired& retired : retiredViews)
                destroyImageView(instance.device, retired.view, nullptr);
        }

        xess_result_t Create(xess_context_handle_t* handle) override
        {
            if (!deviceReady)
            {
                Log(kUnityLogTypeWarning,
                    "the Vulkan device was created without XeSS's extensions, because the plugin loaded too late");
                return XESS_RESULT_ERROR_UNSUPPORTED;
            }
            return xess->VKCreateContext(instance.instance, instance.physicalDevice, instance.device, handle);
        }

        xess_result_t BuildPipelines(xess_context_handle_t handle, uint32_t flags) override
        {
            return xess->VKBuildPipelines(handle, VK_NULL_HANDLE, false, flags);
        }

        xess_result_t Init(xess_context_handle_t handle, const InitRequest& request) override
        {
            xess_vk_init_params_t init{};
            init.outputResolution = {request.width, request.height};
            init.qualitySetting = static_cast<xess_quality_settings_t>(request.quality);
            init.initFlags = request.flags;
            return xess->VKInit(handle, &init);
        }

        xess_result_t Execute(xess_context_handle_t handle, const ExecuteRequest& request) override
        {
            constexpr VkPipelineStageFlags stage = VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT;
            constexpr VkAccessFlags read = VK_ACCESS_SHADER_READ_BIT;
            UnityVulkanImage color{};
            UnityVulkanImage velocity{};
            UnityVulkanImage depth{};
            UnityVulkanImage output{};
            if (!Access(request.color, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, stage, read, color)
                || !Access(request.velocity, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, stage, read, velocity)
                || !Access(request.depth, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, stage, read, depth)
                || !Access(request.output, VK_IMAGE_LAYOUT_GENERAL, stage, read | VK_ACCESS_SHADER_WRITE_BIT, output))
                return XESS_RESULT_ERROR_INVALID_ARGUMENT;

            // Accessing textures may record barriers, so the recording state is read afterwards.
            UnityVulkanRecordingState recording{};
            if (!unity->CommandRecordingState(&recording, kUnityVulkanGraphicsQueueAccess_DontCare))
                return XESS_RESULT_ERROR_WRONG_CALL_ORDER;
            DestroyRetiredViews(recording.safeFrameNumber);
            currentFrame = recording.currentFrameNumber;

            xess_vk_execute_params_t execute{};
            execute.colorTexture = ViewInfo(color);
            execute.velocityTexture = ViewInfo(velocity);
            execute.depthTexture = ViewInfo(depth);
            execute.outputTexture = ViewInfo(output);
            execute.jitterOffsetX = request.jitterX;
            execute.jitterOffsetY = request.jitterY;
            execute.exposureScale = 1.0f;
            execute.resetHistory = request.resetHistory;
            execute.inputWidth = request.inputWidth;
            execute.inputHeight = request.inputHeight;
            return xess->VKExecute(handle, recording.commandBuffer, &execute);
        }

        void ConfigureExecuteEvent(int eventId) override
        {
            // XeSS dispatches compute work, which can't happen inside a render pass, and binds its own state.
            UnityVulkanPluginEventConfig config{};
            config.renderPassPrecondition = kUnityVulkanRenderPass_EnsureOutside;
            config.graphicsQueueAccess = kUnityVulkanGraphicsQueueAccess_DontCare;
            config.flags = kUnityVulkanEventConfigFlag_EnsurePreviousFrameSubmission
                | kUnityVulkanEventConfigFlag_ModifiesCommandBuffersState;
            unity->ConfigureEvent(eventId, &config);
        }

        // The views may still be in use by frames in flight, so they wait until Unity says those frames are done.
        void ForgetTextures() override
        {
            for (auto& [image, view] : views)
                retiredViews.push_back({view, currentFrame});
            views.clear();
        }

    private:
        struct Retired
        {
            VkImageView view;
            unsigned long long frame;
        };

        bool Access(void* texture, VkImageLayout layout, VkPipelineStageFlags stage, VkAccessFlags access, UnityVulkanImage& image)
        {
            VkImageSubresource subresource{VK_IMAGE_ASPECT_COLOR_BIT, 0, 0};
            return unity->AccessTexture(texture, &subresource, layout, stage, access,
                kUnityVulkanResourceAccess_PipelineBarrier, &image);
        }

        xess_vk_image_view_info ViewInfo(const UnityVulkanImage& image)
        {
            xess_vk_image_view_info info{};
            info.image = image.image;
            info.imageView = View(image);
            info.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
            info.format = image.format;
            info.width = image.extent.width;
            info.height = image.extent.height;
            return info;
        }

        VkImageView View(const UnityVulkanImage& image)
        {
            auto found = views.find(image.image);
            if (found != views.end())
                return found->second;

            VkImageViewCreateInfo create{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
            create.image = image.image;
            create.viewType = VK_IMAGE_VIEW_TYPE_2D;
            create.format = image.format;
            create.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
            VkImageView view = VK_NULL_HANDLE;
            createImageView(instance.device, &create, nullptr, &view);
            views.emplace(image.image, view);
            return view;
        }

        void DestroyRetiredViews(unsigned long long safeFrame)
        {
            std::erase_if(retiredViews, [&](const Retired& retired) {
                if (retired.frame > safeFrame)
                    return false;
                destroyImageView(instance.device, retired.view, nullptr);
                return true;
            });
        }

        IUnityGraphicsVulkanV2* unity;
        UnityVulkanInstance instance;
        PFN_vkCreateImageView createImageView;
        PFN_vkDestroyImageView destroyImageView;
        std::unordered_map<VkImage, VkImageView> views;
        std::vector<Retired> retiredViews;
        unsigned long long currentFrame = 0;
    };
}

void InterceptVulkanInitialization(IUnityInterfaces* interfaces)
{
    // Registering fails once Unity has created its device, whatever the API. The Vulkan backend reports that case
    // when a context is created.
    auto* unity = UNITY_GET_INTERFACE(interfaces, IUnityGraphicsVulkanV2);
    if (unity != nullptr)
        unity->AddInterceptInitialization(&InterceptInitialization, nullptr, 0);
}

bool VulkanDeviceReady()
{
    return deviceReady;
}

std::unique_ptr<Backend> CreateVulkanBackend(IUnityInterfaces* interfaces)
{
    auto* unity = UNITY_GET_INTERFACE(interfaces, IUnityGraphicsVulkanV2);
    const XessLibrary* xess = LoadXessLibrary();
    if (unity == nullptr || xess == nullptr || xess->VKCreateContext == nullptr)
        return nullptr;
    return std::make_unique<BackendVulkan>(xess, unity);
}
