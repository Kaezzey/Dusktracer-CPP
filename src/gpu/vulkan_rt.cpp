#include "core/denoise.h"
#include "core/firefly_filter.h"
#define VK_NO_PROTOTYPES
#define NOMINMAX
#include <vulkan/vulkan.h>
#include <windows.h>
#include "core/render_backend.h"
#include "core/render_tiles.h"
#include "core/gpu_scene.h"
#include "pathtrace_spv.h"
#include <array>
#include <chrono>
#include <cstring>
#include <iomanip>
#include <sstream>
#include <stdexcept>
#include <iostream>
#include "pipeline_cache.h"
#include "content_key.h"
#include <algorithm>
#include <mutex>
#include <thread>

namespace dusk_gpu {
namespace {
void check(VkResult result, const char* operation) {
    if (result != VK_SUCCESS) {
        throw std::runtime_error(std::string(operation) + " failed (Vulkan " + std::to_string(result) + ").");
    }
}
// The dispatch table keeps the CPU application independent of the Vulkan loader DLL.
// clang-format off
#define INSTANCE_FUNCTIONS(X) \
    X(vkDestroyInstance) \
    X(vkEnumeratePhysicalDevices) \
    X(vkGetPhysicalDeviceProperties2) \
    X(vkGetPhysicalDeviceFeatures2) \
    X(vkEnumerateDeviceExtensionProperties) \
    X(vkGetPhysicalDeviceQueueFamilyProperties) \
    X(vkGetPhysicalDeviceMemoryProperties) \
    X(vkCreateDevice) \
    X(vkGetDeviceProcAddr)
#define DEVICE_FUNCTIONS(X) \
    X(vkGetBufferMemoryRequirements) \
    X(vkDestroyCommandPool) \
    X(vkDestroyDevice) \
    X(vkGetDeviceQueue) \
    X(vkDeviceWaitIdle) \
    X(vkCreateBuffer) \
    X(vkDestroyBuffer) \
    X(vkAllocateMemory) \
    X(vkFreeMemory) \
    X(vkBindBufferMemory) \
    X(vkMapMemory) \
    X(vkUnmapMemory) \
    X(vkGetBufferDeviceAddress) \
    X(vkCreateCommandPool) \
    X(vkResetCommandPool) \
    X(vkCreateQueryPool) \
    X(vkDestroyQueryPool) \
    X(vkCmdResetQueryPool) \
    X(vkCmdWriteTimestamp) \
    X(vkGetQueryPoolResults) \
    X(vkAllocateCommandBuffers) \
    X(vkBeginCommandBuffer) \
    X(vkEndCommandBuffer) \
    X(vkQueueSubmit) \
    X(vkCreateFence) \
    X(vkDestroyFence) \
    X(vkWaitForFences) \
    X(vkResetFences) \
    X(vkCmdCopyBuffer) \
    X(vkCmdFillBuffer) \
    X(vkCmdPipelineBarrier) \
    X(vkCreateAccelerationStructureKHR) \
    X(vkDestroyAccelerationStructureKHR) \
    X(vkGetAccelerationStructureBuildSizesKHR) \
    X(vkCmdBuildAccelerationStructuresKHR) \
    X(vkGetAccelerationStructureDeviceAddressKHR) \
    X(vkCreateDescriptorSetLayout) \
    X(vkDestroyDescriptorSetLayout) \
    X(vkCreateDescriptorPool) \
    X(vkDestroyDescriptorPool) \
    X(vkAllocateDescriptorSets) \
    X(vkUpdateDescriptorSets) \
    X(vkCreatePipelineLayout) \
    X(vkDestroyPipelineLayout) \
    X(vkCreateShaderModule) \
    X(vkDestroyShaderModule) \
    X(vkCreatePipelineCache) \
    X(vkGetPipelineCacheData) \
    X(vkDestroyPipelineCache) \
    X(vkCreateComputePipelines) \
    X(vkDestroyPipeline) \
    X(vkCmdBindPipeline) \
    X(vkCmdBindDescriptorSets) \
    X(vkCmdPushConstants) \
    X(vkCmdDispatch)
// clang-format on
struct buffer {
    VkBuffer handle{};
    VkDeviceMemory memory{};
    VkDeviceSize size{};
    VkDeviceAddress address{};
    void* mapped{};
};
struct context {
    context() = default;
    context(const context&) = delete;
    context& operator=(const context&) = delete;
    HMODULE library{};
    PFN_vkGetInstanceProcAddr get{};
#define DECLARE(name) PFN_##name name{};
    INSTANCE_FUNCTIONS(DECLARE)
    DEVICE_FUNCTIONS(DECLARE)
#undef DECLARE
    VkInstance instance{};
    VkPhysicalDevice physical{};
    VkDevice device{};
    VkQueue queue{};
    VkCommandPool pool{};
    VkCommandBuffer command{};
    VkFence fence{};
    struct pending_submission {
        VkCommandPool pool{};
        VkCommandBuffer command{};
        VkFence fence{};
        VkQueryPool timestamps{};
        bool pending = false;
        int work = 0;
    };
    std::array<pending_submission, 3> pending{};
    size_t next_submission = 0;
    uint32_t timestamp_bits = 0;
    double submission_ms = 0, busy_ms = 0; // last queued submission; sum of all (DUSK_GPU_TIMINGS)
    int64_t completed_work = 0;
    bool synchronous = false;
    VkDescriptorPool descriptors{};
    VkDescriptorSet descriptor{};
    render_statistics* statistics = nullptr;
    VkDescriptorSetLayout layout{};
    VkPipelineLayout pipelineLayout{};
    VkShaderModule shader{};
    VkPipeline pipeline{};
    VkPipelineCache pipelineCache{};
    VkDebugUtilsMessengerEXT messenger{};
    VkPhysicalDeviceProperties properties{};
    VkPhysicalDeviceMemoryProperties memory{};
    VkPhysicalDeviceAccelerationStructurePropertiesKHR asProperties{
        VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ACCELERATION_STRUCTURE_PROPERTIES_KHR};
    std::vector<buffer> buffers;
    struct acceleration_structure {
        VkAccelerationStructureKHR handle;
        buffer storage;
    };
    std::vector<acceleration_structure> structures;
    std::atomic<int> validationErrors{0};
    static VKAPI_ATTR VkBool32 VKAPI_CALL message(VkDebugUtilsMessageSeverityFlagBitsEXT severity,
                                                  VkDebugUtilsMessageTypeFlagsEXT,
                                                  const VkDebugUtilsMessengerCallbackDataEXT* data,
                                                  void* user) {
        if (severity & VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT) {
            ++static_cast<context*>(user)->validationErrors;
        }
        std::cerr << "[Vulkan validation] " << data->pMessage << '\n';
        return VK_FALSE;
    }
    ~context() {
        if (device && vkDeviceWaitIdle) {
            vkDeviceWaitIdle(device);
        }
        if (device) {
            if (pipeline) {
                vkDestroyPipeline(device, pipeline, nullptr);
            }
            if (pipelineCache) {
                vkDestroyPipelineCache(device, pipelineCache, nullptr);
            }
            if (shader) {
                vkDestroyShaderModule(device, shader, nullptr);
            }
            if (pipelineLayout) {
                vkDestroyPipelineLayout(device, pipelineLayout, nullptr);
            }
            if (descriptors) {
                vkDestroyDescriptorPool(device, descriptors, nullptr);
            }
            if (layout) {
                vkDestroyDescriptorSetLayout(device, layout, nullptr);
            }
            for (auto as : structures) {
                vkDestroyAccelerationStructureKHR(device, as.handle, nullptr);
            }
            for (auto& b : buffers) {
                if (b.mapped) {
                    vkUnmapMemory(device, b.memory);
                }
                if (b.handle) {
                    vkDestroyBuffer(device, b.handle, nullptr);
                }
                if (b.memory) {
                    vkFreeMemory(device, b.memory, nullptr);
                }
            }
            for (auto& slot : pending) {
                if (slot.timestamps) vkDestroyQueryPool(device, slot.timestamps, nullptr);
                if (slot.fence) vkDestroyFence(device, slot.fence, nullptr);
                if (slot.pool) vkDestroyCommandPool(device, slot.pool, nullptr);
            }
            if (fence) {
                vkDestroyFence(device, fence, nullptr);
            }
            if (pool) {
                vkDestroyCommandPool(device, pool, nullptr);
            }
            if (vkDestroyDevice) {
                vkDestroyDevice(device, nullptr);
            }
        }
        if (messenger) {
            reinterpret_cast<PFN_vkDestroyDebugUtilsMessengerEXT>(
                get(instance, "vkDestroyDebugUtilsMessengerEXT"))(instance, messenger, nullptr);
        }
        if (instance && vkDestroyInstance) {
            vkDestroyInstance(instance, nullptr);
        }
        if (library) {
            FreeLibrary(library);
        }
    }
    void open() {
        library = LoadLibraryExW(L"vulkan-1.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
        if (!library) {
            throw std::runtime_error(
                "Vulkan loader is unavailable; install a GPU driver with Vulkan support.");
        }
        get = reinterpret_cast<PFN_vkGetInstanceProcAddr>(GetProcAddress(library, "vkGetInstanceProcAddr"));
        if (!get) {
            throw std::runtime_error("Vulkan loader has no instance entry point.");
        }
        auto create = reinterpret_cast<PFN_vkCreateInstance>(get(nullptr, "vkCreateInstance"));
        VkApplicationInfo app{VK_STRUCTURE_TYPE_APPLICATION_INFO};
        app.pApplicationName = "Dusktracer";
        app.apiVersion = VK_API_VERSION_1_2;
        VkInstanceCreateInfo info{VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO};
        info.pApplicationInfo = &app;
        const char* layer = "VK_LAYER_KHRONOS_validation";
        const char* extension = VK_EXT_DEBUG_UTILS_EXTENSION_NAME;
        bool validation = std::getenv("DUSK_VK_VALIDATION") != nullptr;
        VkDebugUtilsMessengerCreateInfoEXT debug{VK_STRUCTURE_TYPE_DEBUG_UTILS_MESSENGER_CREATE_INFO_EXT};
        debug.messageSeverity =
            VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT | VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT;
        debug.messageType = VK_DEBUG_UTILS_MESSAGE_TYPE_GENERAL_BIT_EXT |
                            VK_DEBUG_UTILS_MESSAGE_TYPE_VALIDATION_BIT_EXT |
                            VK_DEBUG_UTILS_MESSAGE_TYPE_PERFORMANCE_BIT_EXT;
        debug.pfnUserCallback = message;
        debug.pUserData = this;
        if (validation) {
            info.enabledLayerCount = 1;
            info.ppEnabledLayerNames = &layer;
            info.enabledExtensionCount = 1;
            info.ppEnabledExtensionNames = &extension;
            info.pNext = &debug;
        }
        check(create(&info, nullptr, &instance), "Create Vulkan instance");
#define LOAD_INSTANCE(name)                                                                                  \
    name = reinterpret_cast<PFN_##name>(get(instance, #name));                                               \
    if (!name)                                                                                               \
        throw std::runtime_error("Missing Vulkan function " #name);
        INSTANCE_FUNCTIONS(LOAD_INSTANCE)
#undef LOAD_INSTANCE
        if (validation) {
            check(reinterpret_cast<PFN_vkCreateDebugUtilsMessengerEXT>(
                      get(instance, "vkCreateDebugUtilsMessengerEXT"))(instance, &debug, nullptr, &messenger),
                  "Create validation messenger");
        }
    }
    std::vector<VkPhysicalDevice> physicalDevices() {
        uint32_t count = 0;
        check(vkEnumeratePhysicalDevices(instance, &count, nullptr), "Enumerate GPUs");
        std::vector<VkPhysicalDevice> result(count);
        check(vkEnumeratePhysicalDevices(instance, &count, result.data()), "Enumerate GPUs");
        result.resize(count);
        return result;
    }
    render_device describe(VkPhysicalDevice gpu) {
        VkPhysicalDeviceIDProperties id{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ID_PROPERTIES};
        VkPhysicalDeviceProperties2 props{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2};
        props.pNext = &id;
        vkGetPhysicalDeviceProperties2(gpu, &props);
        render_device result;
        result.name = props.properties.deviceName;
        result.vendor = props.properties.vendorID;
        result.discrete = props.properties.deviceType == VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU;
        std::ostringstream identity;
        identity << std::hex << std::setfill('0');
        for (auto c : id.deviceUUID) {
            identity << std::setw(2) << int(c);
        }
        result.id = identity.str();
        if (props.properties.apiVersion < VK_API_VERSION_1_2) {
            result.reason = "Vulkan 1.2 is required.";
            return result;
        }
        uint32_t count = 0;
        check(vkEnumerateDeviceExtensionProperties(gpu, nullptr, &count, nullptr), "Query extensions");
        std::vector<VkExtensionProperties> extensions(count);
        check(vkEnumerateDeviceExtensionProperties(gpu, nullptr, &count, extensions.data()),
              "Query extensions");
        for (const char* required :
             {VK_KHR_ACCELERATION_STRUCTURE_EXTENSION_NAME, VK_KHR_DEFERRED_HOST_OPERATIONS_EXTENSION_NAME,
              VK_KHR_RAY_QUERY_EXTENSION_NAME}) {
            if (std::none_of(extensions.begin(), extensions.end(),
                             [&](const auto& e) { return std::strcmp(e.extensionName, required) == 0; })) {
                result.reason = std::string("Missing ") + required;
                return result;
            }
        }
        VkPhysicalDeviceRayQueryFeaturesKHR ray{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_RAY_QUERY_FEATURES_KHR};
        VkPhysicalDeviceAccelerationStructureFeaturesKHR accel{
            VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ACCELERATION_STRUCTURE_FEATURES_KHR};
        accel.pNext = &ray;
        VkPhysicalDeviceBufferDeviceAddressFeatures address{
            VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_BUFFER_DEVICE_ADDRESS_FEATURES};
        address.pNext = &accel;
        VkPhysicalDeviceFeatures2 features{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2};
        features.pNext = &address;
        vkGetPhysicalDeviceFeatures2(gpu, &features);
        if (!ray.rayQuery || !accel.accelerationStructure || !address.bufferDeviceAddress) {
            result.reason = "Required hardware ray-query features are disabled.";
            return result;
        }
        if (props.properties.limits.maxPerStageDescriptorStorageBuffers < 14) {
            result.reason = "At least 14 compute storage buffers are required.";
            return result;
        }
        uint32_t n = 0;
        vkGetPhysicalDeviceQueueFamilyProperties(gpu, &n, nullptr);
        std::vector<VkQueueFamilyProperties> queues(n);
        vkGetPhysicalDeviceQueueFamilyProperties(gpu, &n, queues.data());
        if (std::none_of(queues.begin(), queues.end(),
                         [](auto q) { return q.queueCount && (q.queueFlags & VK_QUEUE_COMPUTE_BIT); })) {
            result.reason = "No compute queue available.";
            return result;
        }
        result.compatible = true;
        return result;
    }
    void select(const std::string& id) {
        for (auto gpu : physicalDevices()) {
            auto info = describe(gpu);
            if (info.compatible && (id.empty() || info.id == id)) {
                physical = gpu;
                break;
            }
        }
        if (!physical) {
            throw std::runtime_error(
                "The selected GPU is unavailable or does not support Vulkan hardware ray queries.");
        }
        VkPhysicalDeviceProperties2 props{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2};
        props.pNext = &asProperties;
        vkGetPhysicalDeviceProperties2(physical, &props);
        properties = props.properties;
        vkGetPhysicalDeviceMemoryProperties(physical, &memory);
        uint32_t count = 0;
        vkGetPhysicalDeviceQueueFamilyProperties(physical, &count, nullptr);
        std::vector<VkQueueFamilyProperties> queues(count);
        vkGetPhysicalDeviceQueueFamilyProperties(physical, &count, queues.data());
        uint32_t family = 0;
        while (!(queues[family].queueFlags & VK_QUEUE_COMPUTE_BIT)) {
            ++family;
        }
        float priority = 1;
        VkDeviceQueueCreateInfo queueInfo{VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO};
        queueInfo.queueFamilyIndex = family;
        queueInfo.queueCount = 1;
        queueInfo.pQueuePriorities = &priority;
        VkPhysicalDeviceRayQueryFeaturesKHR ray{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_RAY_QUERY_FEATURES_KHR};
        ray.rayQuery = VK_TRUE;
        VkPhysicalDeviceAccelerationStructureFeaturesKHR accel{
            VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ACCELERATION_STRUCTURE_FEATURES_KHR};
        accel.accelerationStructure = VK_TRUE;
        accel.pNext = &ray;
        VkPhysicalDeviceBufferDeviceAddressFeatures address{
            VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_BUFFER_DEVICE_ADDRESS_FEATURES};
        address.bufferDeviceAddress = VK_TRUE;
        address.pNext = &accel;
        const char* extensions[] = {VK_KHR_ACCELERATION_STRUCTURE_EXTENSION_NAME,
                                    VK_KHR_DEFERRED_HOST_OPERATIONS_EXTENSION_NAME,
                                    VK_KHR_RAY_QUERY_EXTENSION_NAME};
        VkDeviceCreateInfo info{VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO};
        info.pNext = &address;
        info.queueCreateInfoCount = 1;
        info.pQueueCreateInfos = &queueInfo;
        info.enabledExtensionCount = 3;
        info.ppEnabledExtensionNames = extensions;
        check(vkCreateDevice(physical, &info, nullptr, &device), "Create GPU device");
#define LOAD_DEVICE(name)                                                                                    \
    name = reinterpret_cast<PFN_##name>(vkGetDeviceProcAddr(device, #name));                                 \
    if (!name)                                                                                               \
        throw std::runtime_error("Missing Vulkan function " #name);
        DEVICE_FUNCTIONS(LOAD_DEVICE)
#undef LOAD_DEVICE
        vkGetDeviceQueue(device, family, 0, &queue);
        VkCommandPoolCreateInfo poolInfo{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
        poolInfo.queueFamilyIndex = family;
        check(vkCreateCommandPool(device, &poolInfo, nullptr, &pool), "Create command pool");
        VkCommandBufferAllocateInfo alloc{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
        alloc.commandPool = pool;
        alloc.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
        alloc.commandBufferCount = 1;
        check(vkAllocateCommandBuffers(device, &alloc, &command), "Allocate command buffer");
        VkFenceCreateInfo f{VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
        check(vkCreateFence(device, &f, nullptr, &fence), "Create submission fence");
        timestamp_bits = queues[family].timestampValidBits;
        for (auto& slot : pending) {
            check(vkCreateCommandPool(device, &poolInfo, nullptr, &slot.pool), "Create queued command pool");
            alloc.commandPool = slot.pool;
            check(vkAllocateCommandBuffers(device, &alloc, &slot.command), "Allocate queued command buffer");
            check(vkCreateFence(device, &f, nullptr, &slot.fence), "Create queued fence");
            if (timestamp_bits) {
                VkQueryPoolCreateInfo query{VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO};
                query.queryType = VK_QUERY_TYPE_TIMESTAMP;
                query.queryCount = 2;
                check(vkCreateQueryPool(device, &query, nullptr, &slot.timestamps), "Create GPU timer");
            }
        }
    }
    buffer allocate(VkDeviceSize bytes, VkBufferUsageFlags usage, bool host = false) {
        auto free = std::find_if(buffers.begin(), buffers.end(), [](const buffer& b) { return !b.handle; });
        const size_t index = size_t(free - buffers.begin());
        if (free == buffers.end()) buffers.push_back({});
        auto& b = buffers[index];
        b.size = std::max<VkDeviceSize>(bytes, 16);
        VkBufferCreateInfo info{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
        info.size = b.size;
        info.usage = usage;
        info.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
        check(vkCreateBuffer(device, &info, nullptr, &b.handle), "Create GPU buffer");
        VkMemoryRequirements req;
        vkGetBufferMemoryRequirements(device, b.handle, &req);
        VkMemoryPropertyFlags flags =
            host ? VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT
                 : VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT;
        auto find = [&](VkMemoryPropertyFlags wanted) {
            uint32_t type = 0;
            while (type < memory.memoryTypeCount &&
                   (!(req.memoryTypeBits & (1u << type)) || (memory.memoryTypes[type].propertyFlags & wanted) != wanted))
                ++type;
            return type;
        };
        // The CPU reads readback buffers. On uncached (write-combined) host
        // memory those reads crawl: hundreds of milliseconds for one 1080p
        // preview. Prefer a host-cached type for them; uploads keep the first.
        uint32_t type = memory.memoryTypeCount;
        if (host && (usage & VK_BUFFER_USAGE_TRANSFER_DST_BIT)) type = find(flags | VK_MEMORY_PROPERTY_HOST_CACHED_BIT);
        if (type == memory.memoryTypeCount) type = find(flags);
        if (type == memory.memoryTypeCount) {
            throw std::runtime_error("No suitable GPU memory type.");
        }
        VkMemoryAllocateFlagsInfo address{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_FLAGS_INFO};
        address.flags = VK_MEMORY_ALLOCATE_DEVICE_ADDRESS_BIT;
        VkMemoryAllocateInfo alloc{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
        alloc.allocationSize = req.size;
        alloc.memoryTypeIndex = type;
        if (usage & VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT) {
            alloc.pNext = &address;
        }
        check(vkAllocateMemory(device, &alloc, nullptr, &b.memory), "Allocate GPU memory");
        check(vkBindBufferMemory(device, b.handle, b.memory, 0), "Bind GPU memory");
        if (host) {
            check(vkMapMemory(device, b.memory, 0, VK_WHOLE_SIZE, 0, &b.mapped),
                  "Map readback/upload memory");
        }
        if (usage & VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT) {
            VkBufferDeviceAddressInfo query{VK_STRUCTURE_TYPE_BUFFER_DEVICE_ADDRESS_INFO};
            query.buffer = b.handle;
            b.address = vkGetBufferDeviceAddress(device, &query);
        }
        return b;
    }
    void finish(pending_submission& slot) {
        if (!slot.pending) return;
        VkResult result;
        do {
            result = vkWaitForFences(device, 1, &slot.fence, VK_TRUE, 50000000);
        } while (result == VK_TIMEOUT);
        check(result, "Wait for queued GPU work");
        if (slot.timestamps) {
            uint64_t ticks[2]{};
            check(vkGetQueryPoolResults(device, slot.timestamps, 0, 2, sizeof(ticks), ticks,
                                       sizeof(uint64_t), VK_QUERY_RESULT_64_BIT | VK_QUERY_RESULT_WAIT_BIT),
                  "Read GPU timer");
            const uint64_t mask = timestamp_bits == 64 ? ~uint64_t(0) : (uint64_t(1) << timestamp_bits) - 1;
            submission_ms = double((ticks[1] - ticks[0]) & mask) * properties.limits.timestampPeriod / 1e6;
            busy_ms += submission_ms;
        }
        completed_work += slot.work;
        slot.pending = false;
        if (validationErrors.load()) throw std::runtime_error("Vulkan validation reported an error.");
    }
    void drain() {
        for (size_t i = 0; i < pending.size(); ++i) finish(pending[(next_submission + i) % pending.size()]);
    }
    template <class F> void enqueue(int work, F&& record) {
        auto& slot = pending[next_submission];
        finish(slot);
        check(vkResetCommandPool(device, slot.pool, 0), "Reset queued command pool");
        VkCommandBufferBeginInfo begin{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
        begin.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
        check(vkBeginCommandBuffer(slot.command, &begin), "Begin queued commands");
        if (slot.timestamps) {
            vkCmdResetQueryPool(slot.command, slot.timestamps, 0, 2);
            vkCmdWriteTimestamp(slot.command, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, slot.timestamps, 0);
        }
        record(slot.command);
        if (slot.timestamps) vkCmdWriteTimestamp(slot.command, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, slot.timestamps, 1);
        check(vkEndCommandBuffer(slot.command), "End queued commands");
        check(vkResetFences(device, 1, &slot.fence), "Reset queued fence");
        VkSubmitInfo info{VK_STRUCTURE_TYPE_SUBMIT_INFO};
        info.commandBufferCount = 1;
        info.pCommandBuffers = &slot.command;
        check(vkQueueSubmit(queue, 1, &info, slot.fence), "Queue tracing work");
        if (statistics) ++statistics->submissions;
        slot.work = work;
        slot.pending = true;
        next_submission = (next_submission + 1) % pending.size();
        if (synchronous) finish(slot);
    }
    template <class F> void submit(F&& record) {
        drain();
        check(vkResetCommandPool(device, pool, 0), "Reset command pool");
        VkCommandBufferBeginInfo begin{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
        begin.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
        check(vkBeginCommandBuffer(command, &begin), "Begin command buffer");
        record(command);
        check(vkEndCommandBuffer(command), "End command buffer");
        check(vkResetFences(device, 1, &fence), "Reset fence");
        VkSubmitInfo submit{VK_STRUCTURE_TYPE_SUBMIT_INFO};
        submit.commandBufferCount = 1;
        submit.pCommandBuffers = &command;
        check(vkQueueSubmit(queue, 1, &submit, fence), "Submit GPU work");
        if (statistics) ++statistics->submissions;
        VkResult result;
        do {
            result = vkWaitForFences(device, 1, &fence, VK_TRUE, 50000000);
        } while (result == VK_TIMEOUT);
        check(result, "Wait for GPU work");
        if (validationErrors.load()) {
            throw std::runtime_error("Vulkan validation reported an error; see the diagnostic log.");
        }
    }
    // Call only after the owning submission's fence has completed.
    void release(buffer resource) {
        if (!resource.handle) return;
        for (auto& owned : buffers) {
            if (owned.handle != resource.handle) {
                continue;
            }
            if (owned.mapped) {
                vkUnmapMemory(device, owned.memory);
            }
            vkDestroyBuffer(device, owned.handle, nullptr);
            vkFreeMemory(device, owned.memory, nullptr);
            owned = {};
            return;
        }
    }
    void barrier(VkCommandBuffer cmd, VkPipelineStageFlags src, VkAccessFlags srcAccess,
                 VkPipelineStageFlags dst, VkAccessFlags dstAccess) {
        VkMemoryBarrier b{VK_STRUCTURE_TYPE_MEMORY_BARRIER};
        b.srcAccessMask = srcAccess;
        b.dstAccessMask = dstAccess;
        vkCmdPipelineBarrier(cmd, src, dst, 0, 1, &b, 0, nullptr, 0, nullptr);
    }
    VkAccelerationStructureKHR buildAS(const VkAccelerationStructureGeometryKHR& geometry,
                                       uint32_t primitives, VkAccelerationStructureTypeKHR type) {
        if (primitives > asProperties.maxPrimitiveCount) {
            throw std::runtime_error("GPU acceleration structure primitive limit exceeded.");
        }
        VkAccelerationStructureBuildGeometryInfoKHR info{
            VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_BUILD_GEOMETRY_INFO_KHR};
        info.type = type;
        info.flags = VK_BUILD_ACCELERATION_STRUCTURE_PREFER_FAST_TRACE_BIT_KHR;
        info.mode = VK_BUILD_ACCELERATION_STRUCTURE_MODE_BUILD_KHR;
        info.geometryCount = 1;
        info.pGeometries = &geometry;
        VkAccelerationStructureBuildSizesInfoKHR sizes{
            VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_BUILD_SIZES_INFO_KHR};
        vkGetAccelerationStructureBuildSizesKHR(device, VK_ACCELERATION_STRUCTURE_BUILD_TYPE_DEVICE_KHR,
                                                &info, &primitives, &sizes);
        auto storage =
            allocate(sizes.accelerationStructureSize, VK_BUFFER_USAGE_ACCELERATION_STRUCTURE_STORAGE_BIT_KHR |
                                                          VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT);
        VkAccelerationStructureCreateInfoKHR create{VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_CREATE_INFO_KHR};
        create.buffer = storage.handle;
        create.size = sizes.accelerationStructureSize;
        create.type = type;
        VkAccelerationStructureKHR result{};
        check(vkCreateAccelerationStructureKHR(device, &create, nullptr, &result),
              "Create acceleration structure");
        structures.push_back({result, storage});
        if (statistics) {
            if (type == VK_ACCELERATION_STRUCTURE_TYPE_BOTTOM_LEVEL_KHR) ++statistics->blas_builds;
            else ++statistics->tlas_builds;
        }
        auto alignment = asProperties.minAccelerationStructureScratchOffsetAlignment;
        auto scratch =
            allocate(sizes.buildScratchSize + alignment,
                     VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT);
        info.dstAccelerationStructure = result;
        info.scratchData.deviceAddress = (scratch.address + alignment - 1) & ~VkDeviceAddress(alignment - 1);
        VkAccelerationStructureBuildRangeInfoKHR range{};
        range.primitiveCount = primitives;
        const auto* ptr = &range;
        submit([&](VkCommandBuffer cmd) {
            barrier(cmd,
                    VK_PIPELINE_STAGE_TRANSFER_BIT | VK_PIPELINE_STAGE_ACCELERATION_STRUCTURE_BUILD_BIT_KHR,
                    VK_ACCESS_TRANSFER_WRITE_BIT | VK_ACCESS_ACCELERATION_STRUCTURE_WRITE_BIT_KHR,
                    VK_PIPELINE_STAGE_ACCELERATION_STRUCTURE_BUILD_BIT_KHR,
                    VK_ACCESS_ACCELERATION_STRUCTURE_READ_BIT_KHR |
                        VK_ACCESS_ACCELERATION_STRUCTURE_WRITE_BIT_KHR);
            vkCmdBuildAccelerationStructuresKHR(cmd, 1, &info, &ptr);
            barrier(cmd, VK_PIPELINE_STAGE_ACCELERATION_STRUCTURE_BUILD_BIT_KHR,
                    VK_ACCESS_ACCELERATION_STRUCTURE_WRITE_BIT_KHR,
                    VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT |
                        VK_PIPELINE_STAGE_ACCELERATION_STRUCTURE_BUILD_BIT_KHR,
                    VK_ACCESS_ACCELERATION_STRUCTURE_READ_BIT_KHR);
        });
        release(scratch);
        return result;
    }
    void release_structure(VkAccelerationStructureKHR handle) {
        auto found = std::find_if(structures.begin(), structures.end(),
                                  [&](const acceleration_structure& as) { return as.handle == handle; });
        if (found == structures.end()) return;
        vkDestroyAccelerationStructureKHR(device, handle, nullptr);
        release(found->storage);
        structures.erase(found);
    }
    VkDeviceAddress asAddress(VkAccelerationStructureKHR as) {
        VkAccelerationStructureDeviceAddressInfoKHR info{
            VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_DEVICE_ADDRESS_INFO_KHR};
        info.accelerationStructure = as;
        return vkGetAccelerationStructureDeviceAddressKHR(device, &info);
    }
};
struct configuration {
    f4 origin, pixel00, deltaU, deltaV, defocusU, defocusV, background, sunDirection, sunRadiance, adaptive;
    i4 image, counts, options;
};
struct point_light {
    f4 position, radiance;
};
struct pixel {
    f4 sum, albedo, normal, variance, indirect;
};
static_assert(sizeof(configuration) == 208 && sizeof(pixel) == 80);
f4 pack(vec3 v, float w = 0) {
    return {float(v.x()), float(v.y()), float(v.z()), w};
}
configuration configure(const camera& cam, const renderer& settings, const snapshot& scene) {
    configuration c{};
    vec3 w = safe_unit_vector(cam.lookfrom - cam.lookat), u = safe_unit_vector(cross(cam.vup, w)),
         v = cross(w, u);
    double height = 2 * std::tan(degrees_to_radians(cam.vfov) / 2) * cam.focus_dist;
    vec3 vu = u * (height * cam.image_width / cam.image_height), vv = -v * height;
    vec3 du = vu / cam.image_width, dv = vv / cam.image_height;
    c.origin = pack(cam.lookfrom, float(cam.defocus_angle));
    c.pixel00 = pack(cam.lookfrom - cam.focus_dist * w - vu / 2 - vv / 2 + .5 * (du + dv));
    c.deltaU = pack(du);
    c.deltaV = pack(dv);
    double radius = cam.focus_dist * std::tan(degrees_to_radians(cam.defocus_angle) / 2);
    c.defocusU = pack(radius * u);
    c.defocusV = pack(radius * v);
    c.background = pack(cam.background,float(scene.instances.size()));
    if(std::getenv("DUSK_GPU_NO_SPHERE_NEE")) c.background.w=0;
    c.sunDirection = pack(safe_unit_vector(cam.sun_dir), float(cam.sun_solid_angle()));
    c.sunRadiance = pack(cam.sun_radiance, cam.use_sun ? 1.f : 0.f);
    c.image = {cam.image_width, cam.image_height, cam.samples_per_pixel, cam.max_depth};
    c.counts = {int(cam.point_lights.size()), int(scene.lights.size()), cam.direct_light_samples,
                cam.sun_shadow_samples};
    const bool opaque = std::all_of(scene.materials.begin(), scene.materials.end(),
                                    [](const material_record& material) { return material.alpha.z != 0; });
    const bool reference = std::getenv("DUSK_GPU_REFERENCE") != nullptr;
    c.options = {cam.enable_mis ? 1 : 0, int(cam.sampling_method), settings.adaptive_sampling ? 1 : 0,
                 reference ? 0 : 2 | 4 | (opaque ? 1 : 0)};
    if(settings.gpu_firefly_filter) c.options.w |= 8;
    const bool caustic_risk = cam.use_sun && std::any_of(scene.materials.begin(),scene.materials.end(),
        [](const material_record& material) { return int(material.base.w)==2; });
    const bool subsurface_risk=std::any_of(scene.materials.begin(),scene.materials.end(),
        [](const material_record& material) { return material.subsurface.x>0 || material.specular_lobes.w>0; });
    c.adaptive = {float(std::max(caustic_risk ? 256 : subsurface_risk ? 128 : 2, settings.adaptive_min_samples)),
                  float(std::max(1, settings.adaptive_check_interval)),
                  float(settings.adaptive_rel_threshold), float(settings.adaptive_abs_threshold)};
    return c;
}
render_result resolve(const pixel* pixels, int w, int h, const renderer& settings, bool final,
                      std::vector<float>* linear, std::atomic<bool>* cancel,
                      const std::function<void(const std::string&)>& status) {
    size_t n = size_t(w) * h;
    std::vector<float> hdr(n * 3), albedo, normal, indirect;
    if(settings.gpu_firefly_filter) indirect.resize(n*3);
    if (final && settings.use_denoiser && settings.denoiser_strength > 0) {
        albedo.resize(n * 3);
        normal.resize(n * 3);
    }
    for (size_t p = 0; p < n; ++p) {
        float count = std::max(1.f, pixels[p].sum.w);
        for (int c = 0; c < 3; ++c) {
            float value = pixels[p].sum[c] / count;
            hdr[p * 3 + c] = std::isfinite(value) ? std::max(0.f, value) : 0;
            if(!indirect.empty()) {
                float energy=pixels[p].indirect[c]/count;
                indirect[p*3+c]=std::isfinite(energy)?std::max(0.f,energy):0;
            }
            if (!albedo.empty()) {
                albedo[p * 3 + c] = pixels[p].albedo[c] / count;
                normal[p * 3 + c] = pixels[p].normal[c] / count;
            }
        }
    }
    if (linear) {
        *linear = hdr; // numerical comparisons precede denoising/exposure
    }
    if(!indirect.empty()) dusk_fireflies::reconstruct(hdr,indirect,w,h,1);
    if (!albedo.empty()) {
        if (status) {
            status("GPU tracing finished. Denoising final image...");
        }
        const auto error = denoise_hdr(hdr, albedo, normal, w, h, settings.denoiser_strength, cancel);
        if (status) {
            if (cancel && cancel->load()) {
                status("Render cancelled.");
            } else if (!error.empty()) {
                status("GPU render complete; denoising failed: " + error);
            } else {
                status("GPU render complete. Final image denoised.");
            }
        }
    } else if (final && status) {
        status("GPU render complete.");
    }
    render_result result;
    result.width = w;
    result.height = h;
    result.pixels.resize(n * 3);
    for (size_t p = 0; p < n; ++p) {
        double r = hdr[p * 3] * settings.exposure, g = hdr[p * 3 + 1] * settings.exposure,
               b = hdr[p * 3 + 2] * settings.exposure;
        double lum = .2126 * r + .7152 * g + .0722 * b, scale = lum < 1e-6 ? 0 : 1 / (1 + lum);
        double rgb[3] = {r, g, b};
        for (int c = 0; c < 3; ++c) {
            result.pixels[p * 3 + c] = uint8_t(256 * std::clamp(linear_to_gamma(rgb[c] * scale), 0.0, .999));
        }
    }
    return result;
}
struct uploaded_scene {
    std::array<buffer, 15> buffers{};
    std::array<content_key, 15> keys{};
    std::array<VkDescriptorBufferInfo, 15> ranges{};
};

void upload_scene(context& c, uploaded_scene& uploaded, const snapshot& scene, const camera& cam,
                  const renderer& settings) {
    const configuration config = configure(cam, settings, scene);
    std::vector<point_light> points;
    for (const auto& l : cam.point_lights) {
        points.push_back({pack(l.position, float(l.range)), pack(l.radiance)});
    }
    // A small uniform mixture preserves support for every light. The CDF is
    // independent of the shading point and costs only a binary search per ray.
    double total_power = 0;
    for (const auto& light : points) total_power += std::max(0., .2126*light.radiance.x + .7152*light.radiance.y + .0722*light.radiance.z);
    double cumulative = 0;
    for (auto& light : points) {
        const double power = std::max(0., .2126*light.radiance.x + .7152*light.radiance.y + .0722*light.radiance.z);
        cumulative += total_power > 0 ? .95*power/total_power + .05/points.size() : 1./points.size();
        light.radiance.w = float(cumulative);
    }
    if (!points.empty()) points.back().radiance.w = 1;
    // Fall back to uniform sampling if float CDF precision cannot retain support.
    for (size_t i = 1; i < points.size(); ++i) {
        if (points[i].radiance.w <= points[i-1].radiance.w) {
            for (size_t j = 0; j < points.size(); ++j) points[j].radiance.w = float(double(j+1)/points.size());
            break;
        }
    }
    struct slice { const void* data; size_t bytes, offset; int binding; };
    std::vector<slice> changed;
    size_t staging_bytes = 0;
    auto append = [&](int binding, const void* data, size_t bytes) {
        if (bytes > c.properties.limits.maxStorageBufferRange) {
            throw std::runtime_error("Scene buffer exceeds this GPU's storage-buffer limit.");
        }
        const auto key = identify_content(data, bytes);
        auto& target = uploaded.buffers[binding];
        if (target.handle && uploaded.keys[binding] == key) return;
        if (!target.handle || target.size != std::max<size_t>(16, bytes)) {
            c.release(target);
            target = c.allocate(bytes, VK_BUFFER_USAGE_TRANSFER_DST_BIT | VK_BUFFER_USAGE_STORAGE_BUFFER_BIT |
                                VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT |
                                VK_BUFFER_USAGE_ACCELERATION_STRUCTURE_BUILD_INPUT_READ_ONLY_BIT_KHR);
        }
        uploaded.keys[binding] = key;
        uploaded.ranges[binding] = {target.handle, 0, target.size};
        changed.push_back({data, bytes, staging_bytes, binding});
        staging_bytes += target.size;
    };
    append(1, scene.geometries.data(), scene.geometries.size() * sizeof(geometry));
    append(2, scene.vertices.data(), scene.vertices.size() * sizeof(vertex));
    append(3, scene.instances.data(), scene.instances.size() * sizeof(instance));
    append(4, scene.material_map.data(), scene.material_map.size() * sizeof(uint32_t));
    append(5, scene.materials.data(), scene.materials.size() * sizeof(material_record));
    append(6, scene.instructions.data(), scene.instructions.size() * sizeof(instruction));
    append(7, scene.textures.data(), scene.textures.size() * sizeof(texture_record));
    append(8, scene.texels.data(), scene.texels.size() * sizeof(uint32_t));
    append(10, &config, sizeof(config));
    append(11, points.data(), points.size() * sizeof(point_light));
    append(12, scene.lights.data(), scene.lights.size() * sizeof(area_light));
    append(13, scene.slots.data(), scene.slots.size() * sizeof(uint32_t));
    if (changed.empty()) return;
    const auto staging = c.allocate(staging_bytes, VK_BUFFER_USAGE_TRANSFER_SRC_BIT, true);
    for (const auto& slice : changed) {
        auto* destination = static_cast<uint8_t*>(staging.mapped) + slice.offset;
        std::memset(destination, 0, size_t(uploaded.buffers[slice.binding].size));
        if (slice.bytes) std::memcpy(destination, slice.data, slice.bytes);
    }
    c.submit([&](VkCommandBuffer cmd) {
        for (const auto& slice : changed) {
            const auto& target = uploaded.buffers[slice.binding];
            const VkBufferCopy copy{slice.offset, 0, target.size};
            c.vkCmdCopyBuffer(cmd, staging.handle, target.handle, 1, &copy);
        }
        c.barrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_ACCESS_TRANSFER_WRITE_BIT,
                  VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT | VK_PIPELINE_STAGE_ACCELERATION_STRUCTURE_BUILD_BIT_KHR,
                  VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_ACCELERATION_STRUCTURE_READ_BIT_KHR);
    });
    if (c.statistics) c.statistics->uploaded_bytes += staging_bytes;
    c.release(staging);
}

std::vector<uint8_t> instance_masks(const snapshot& scene) {
    std::vector<uint8_t> masks(scene.instances.size(), 1);
    for (size_t i = 0; i < scene.instances.size(); ++i) {
        const size_t first = size_t(scene.instances[i].data.y);
        const size_t end = i+1 < scene.instances.size() ? size_t(scene.instances[i+1].data.y) : scene.material_map.size();
        for (size_t j = first; j < end; ++j) {
            if (scene.materials[scene.material_map[j]].alpha.z == 0) {
                masks[i] = 2;
                break;
            }
        }
    }
    return masks;
}

VkAccelerationStructureKHR build_world(context& c, const snapshot& scene, const uploaded_scene& uploaded,
                                       std::atomic<bool>* cancel,
                                       std::vector<VkAccelerationStructureKHR>& blas, const std::vector<uint8_t>& masks) {
    auto cancelled = [&] { return cancel && cancel->load(); };
    VkAabbPositionsKHR sphereBounds{-1, -1, -1, 1, 1, 1};
    auto bounds = c.allocate(sizeof(sphereBounds),
                             VK_BUFFER_USAGE_ACCELERATION_STRUCTURE_BUILD_INPUT_READ_ONLY_BIT_KHR |
                                 VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT,
                             true);
    std::memcpy(bounds.mapped, &sphereBounds, sizeof(sphereBounds));
    const bool reuse_blas = blas.size() == scene.geometries.size();
    blas.resize(scene.geometries.size());
    for (size_t i = 0; !reuse_blas && i < scene.geometries.size(); ++i) {
        if (cancelled()) {
            return {};
        }
        const auto& geom = scene.geometries[i].data;
        VkAccelerationStructureGeometryKHR geometry{VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_KHR};
        // Shadow rays visit every crossing once as a candidate
        // (pathtrace.comp: visibility); none may be reported twice.
        geometry.flags = VK_GEOMETRY_OPAQUE_BIT_KHR | VK_GEOMETRY_NO_DUPLICATE_ANY_HIT_INVOCATION_BIT_KHR;
        if (geom.w) {
            geometry.geometryType = VK_GEOMETRY_TYPE_AABBS_KHR;
            auto& a = geometry.geometry.aabbs;
            a.sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_AABBS_DATA_KHR;
            a.data.deviceAddress = bounds.address;
            a.stride = sizeof(sphereBounds);
        } else {
            geometry.geometryType = VK_GEOMETRY_TYPE_TRIANGLES_KHR;
            auto& t = geometry.geometry.triangles;
            t.sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_TRIANGLES_DATA_KHR;
            t.vertexFormat = VK_FORMAT_R32G32B32_SFLOAT;
            t.vertexData.deviceAddress =
                uploaded.buffers[2].address + size_t(geom.x) * sizeof(vertex);
            t.vertexStride = sizeof(vertex);
            t.maxVertex = std::max(0, geom.z * 3 - 1);
            t.indexType = VK_INDEX_TYPE_NONE_KHR;
        }
        blas[i] = c.buildAS(geometry, geom.z, VK_ACCELERATION_STRUCTURE_TYPE_BOTTOM_LEVEL_KHR);
    }
    if (scene.instances.size() > c.asProperties.maxInstanceCount) {
        throw std::runtime_error("GPU acceleration structure instance limit exceeded.");
    }
    std::vector<VkAccelerationStructureInstanceKHR> instances(scene.instances.size());
    for (size_t i = 0; i < instances.size(); ++i) {
        const auto& source = scene.instances[i];
        auto& target = instances[i];
        std::memcpy(target.transform.matrix, source.rows, sizeof(target.transform.matrix));
        target.instanceCustomIndex = uint32_t(i);
        target.mask = masks[i];
        target.flags = VK_GEOMETRY_INSTANCE_TRIANGLE_FACING_CULL_DISABLE_BIT_KHR;
        // Alpha-tested instances offer their crossings as candidates
        // (pathtrace.comp: traceSurface); ray flags force the rest opaque.
        if (masks[i] == 2) target.flags |= VK_GEOMETRY_INSTANCE_FORCE_NO_OPAQUE_BIT_KHR;
        target.accelerationStructureReference = c.asAddress(blas[source.data.x]);
    }
    auto instanceBuffer = c.allocate(instances.size() * sizeof(VkAccelerationStructureInstanceKHR),
                                     VK_BUFFER_USAGE_ACCELERATION_STRUCTURE_BUILD_INPUT_READ_ONLY_BIT_KHR |
                                         VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT,
                                     true);
    if (!instances.empty()) {
        std::memcpy(instanceBuffer.mapped, instances.data(),
                    instances.size() * sizeof(VkAccelerationStructureInstanceKHR));
    }
    VkAccelerationStructureGeometryKHR top{VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_KHR};
    top.geometryType = VK_GEOMETRY_TYPE_INSTANCES_KHR;
    top.geometry.instances.sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_INSTANCES_DATA_KHR;
    top.geometry.instances.data.deviceAddress = instanceBuffer.address;
    auto tlas = c.buildAS(top, uint32_t(instances.size()), VK_ACCELERATION_STRUCTURE_TYPE_TOP_LEVEL_KHR);
    c.release(bounds);
    c.release(instanceBuffer);
    return tlas;
}

VkDescriptorSet create_pipeline(context& c, buffer output, buffer display,
                                std::array<VkDescriptorBufferInfo, 15> ranges,
                                VkAccelerationStructureKHR tlas) {
    std::array<VkDescriptorSetLayoutBinding, 15> bindings{};
    for (uint32_t i = 0; i < bindings.size(); ++i) {
        bindings[i] = {
            i, i == 0 ? VK_DESCRIPTOR_TYPE_ACCELERATION_STRUCTURE_KHR : VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1,
            VK_SHADER_STAGE_COMPUTE_BIT, nullptr};
    }
    ranges[9] = {output.handle, 0, output.size};
    ranges[14] = {display.handle, 0, display.size};
    if (!c.layout) {
    VkDescriptorSetLayoutCreateInfo layout{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
    layout.bindingCount = uint32_t(bindings.size());
    layout.pBindings = bindings.data();
    check(c.vkCreateDescriptorSetLayout(c.device, &layout, nullptr, &c.layout), "Create descriptor layout");
    VkDescriptorPoolSize poolSizes[] = {{VK_DESCRIPTOR_TYPE_ACCELERATION_STRUCTURE_KHR, 1},
                                        {VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 14}};
    VkDescriptorPoolCreateInfo pool{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
    pool.maxSets = 1;
    pool.poolSizeCount = 2;
    pool.pPoolSizes = poolSizes;
    check(c.vkCreateDescriptorPool(c.device, &pool, nullptr, &c.descriptors), "Create descriptor pool");
    VkDescriptorSetAllocateInfo alloc{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
    alloc.descriptorPool = c.descriptors;
    alloc.descriptorSetCount = 1;
    alloc.pSetLayouts = &c.layout;
    check(c.vkAllocateDescriptorSets(c.device, &alloc, &c.descriptor), "Allocate descriptors");
    }
    const auto descriptor = c.descriptor;
    VkWriteDescriptorSetAccelerationStructureKHR asWrite{
        VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET_ACCELERATION_STRUCTURE_KHR};
    asWrite.accelerationStructureCount = 1;
    asWrite.pAccelerationStructures = &tlas;
    std::array<VkWriteDescriptorSet, 15> writes{};
    for (uint32_t i = 0; i < writes.size(); ++i) {
        auto& write = writes[i];
        write.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        write.dstSet = descriptor;
        write.dstBinding = i;
        write.descriptorCount = 1;
        write.descriptorType = bindings[i].descriptorType;
        if (i == 0) {
            write.pNext = &asWrite;
        } else {
            write.pBufferInfo = &ranges[i];
        }
    }
    c.vkUpdateDescriptorSets(c.device, uint32_t(writes.size()), writes.data(), 0, nullptr);
    if (c.pipeline) {
        if (c.statistics) c.statistics->reused_pipeline = true;
        return descriptor;
    }
    VkPushConstantRange push{VK_SHADER_STAGE_COMPUTE_BIT, 0, 16};
    VkPipelineLayoutCreateInfo pipelineLayout{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
    pipelineLayout.setLayoutCount = 1;
    pipelineLayout.pSetLayouts = &c.layout;
    pipelineLayout.pushConstantRangeCount = 1;
    pipelineLayout.pPushConstantRanges = &push;
    check(c.vkCreatePipelineLayout(c.device, &pipelineLayout, nullptr, &c.pipelineLayout),
          "Create pipeline layout");
    VkShaderModuleCreateInfo shader{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
    shader.codeSize = sizeof(pathtrace_spv);
    shader.pCode = pathtrace_spv;
    check(c.vkCreateShaderModule(c.device, &shader, nullptr, &c.shader), "Load ray-query shader");
    VkComputePipelineCreateInfo pipeline{VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO};
    pipeline.layout = c.pipelineLayout;
    pipeline.stage.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    pipeline.stage.stage = VK_SHADER_STAGE_COMPUTE_BIT;
    pipeline.stage.module = c.shader;
    pipeline.stage.pName = "main";
    const auto shader_fingerprint = pipeline_cache::fingerprint(pathtrace_spv, sizeof(pathtrace_spv));
    auto cached = pipeline_cache::read(c.properties, shader_fingerprint);
    VkPipelineCacheCreateInfo cache_info{VK_STRUCTURE_TYPE_PIPELINE_CACHE_CREATE_INFO};
    cache_info.initialDataSize = cached.size();
    cache_info.pInitialData = cached.empty() ? nullptr : cached.data();
    auto cache_result = c.vkCreatePipelineCache(c.device, &cache_info, nullptr, &c.pipelineCache);
    if (cache_result != VK_SUCCESS && !cached.empty()) {
        cached.clear();
        cache_info.initialDataSize = 0;
        cache_info.pInitialData = nullptr;
        cache_result = c.vkCreatePipelineCache(c.device, &cache_info, nullptr, &c.pipelineCache);
    }
    check(cache_result, "Create pipeline cache");
    check(c.vkCreateComputePipelines(c.device, c.pipelineCache, 1, &pipeline, nullptr, &c.pipeline),
          "Compile ray-query pipeline");
    if (cached.empty()) {
        size_t size = 0;
        if (c.vkGetPipelineCacheData(c.device, c.pipelineCache, &size, nullptr) == VK_SUCCESS && size > 0 &&
            size <= pipeline_cache::max_bytes) {
            cached.resize(size);
            if (c.vkGetPipelineCacheData(c.device, c.pipelineCache, &size, cached.data()) == VK_SUCCESS) {
                cached.resize(size);
                pipeline_cache::write(c.properties, shader_fingerprint, cached);
            }
        }
    }
    return descriptor;
}

struct render_session {
    context gpu;
    std::string device_id;
    bool validation = false;
    uploaded_scene uploaded;
    content_key geometry_key, vertex_key, instance_key, mask_key;
    VkAccelerationStructureKHR tlas{};
    std::vector<VkAccelerationStructureKHR> blas;
    buffer output, readback, convergence, display, display_readback;
};
std::mutex session_mutex;
std::unique_ptr<render_session> retained_session;

// Resetting a session on any exception prevents partially uploaded data or a
// lost device from being reused. The context drains submissions before freeing.
struct session_guard {
    bool committed = false;
    ~session_guard() {
        if (!committed) retained_session.reset();
        else retained_session->gpu.statistics = nullptr;
    }
};

} // namespace

void clear_cache() {
    std::lock_guard<std::mutex> lock(session_mutex);
    retained_session.reset();
}

render_device_list enumerate() {
    context c;
    c.open();
    render_device_list result;
    for (auto device : c.physicalDevices()) {
        result.devices.push_back(c.describe(device));
    }
    if (std::none_of(result.devices.begin(), result.devices.end(), [](auto& d) { return d.compatible; })) {
        result.diagnostic = "No compatible Vulkan ray-tracing GPU was found.";
    }
    return result;
}

render_result render(snapshot scene, camera cam, const renderer& settings, const std::string& device_id,
                     std::atomic<bool>* cancel, render_progress_state* progress,
                     const std::function<void(const render_result&)>& callback, std::vector<float>* linear,
                     const std::function<void(const std::string&)>& status, render_statistics* statistics) {
    auto cancelled = [&] { return cancel && cancel->load(); };
    if (cancelled()) {
        return {};
    }
    if (cam.image_height <= 0) {
        cam.image_height = std::max(1, int(cam.image_width / cam.aspect_ratio));
    }
    if (cam.image_width < 1 || cam.image_height < 1 || cam.image_width > 8192 || cam.image_height > 8192 ||
        cam.samples_per_pixel < 1 || cam.max_depth < 0 || cam.max_depth > 128) {
        throw std::runtime_error(
            "GPU render dimensions, samples, or depth exceed supported limits (8192 pixels, depth 128).");
    }
    using clock = std::chrono::steady_clock;
    auto start = clock::now();
    auto last_stage = start;
    const bool timings = std::getenv("DUSK_GPU_TIMINGS") != nullptr;
    auto timed = [&](const char* stage) {
        if (timings) {
            const auto now = clock::now();
            std::cerr << "GPU " << stage << ": "
                      << std::chrono::duration<double, std::milli>(now - last_stage).count() << " ms\n";
            last_stage = now;
        }
    };
    std::unique_lock<std::mutex> lock(session_mutex);
    session_guard retain;
    const bool validation = std::getenv("DUSK_VK_VALIDATION") != nullptr;
    if (statistics) *statistics = {};
    if (!retained_session || retained_session->device_id != device_id || retained_session->validation != validation) {
        retained_session = std::make_unique<render_session>();
        auto& fresh = *retained_session;
        fresh.device_id = device_id;
        fresh.validation = validation;
        fresh.gpu.open();
        timed("instance");
        fresh.gpu.select(device_id);
    } else if (statistics) statistics->reused_device = true;
    auto& session = *retained_session;
    auto& c = session.gpu;
    c.statistics = statistics;
    timed("device");
    if (cancelled()) {
        return {};
    }
    if (status) {
        status("Preparing GPU scene...");
    }
    auto& uploaded = session.uploaded;
    upload_scene(c, uploaded, scene, cam, settings);
    if (cancelled()) {
        return {};
    }
    timed("upload");
    const auto masks = instance_masks(scene);
    const auto mask_key = identify_content(masks.data(), masks.size());
    const bool rebuild_geometry = session.geometry_key != uploaded.keys[1] || session.vertex_key != uploaded.keys[2];
    if (!session.tlas || rebuild_geometry || session.instance_key != uploaded.keys[3] || session.mask_key != mask_key) {
        c.release_structure(session.tlas);
        session.tlas = {};
        if (rebuild_geometry) {
            for (auto blas : session.blas) c.release_structure(blas);
            session.blas.clear();
        }
        session.tlas = build_world(c, scene, uploaded, cancel, session.blas, masks);
        session.geometry_key = uploaded.keys[1];
        session.vertex_key = uploaded.keys[2];
        session.instance_key = uploaded.keys[3];
        session.mask_key = mask_key;
    }
    const auto tlas = session.tlas;
    timed("acceleration structures");
    if (cancelled()) {
        return {};
    }
    // Upload records are no longer needed after the TLAS build has completed.
    scene = {};
    VkDeviceSize pixelBytes = VkDeviceSize(cam.image_width) * cam.image_height * sizeof(pixel);
    constexpr VkDeviceSize output_header_bytes = 16;
    if (pixelBytes + output_header_bytes > c.properties.limits.maxStorageBufferRange) {
        throw std::runtime_error("Render resolution exceeds this GPU's storage-buffer limit.");
    }
    auto resize_buffer = [&](buffer& target, VkDeviceSize size, VkBufferUsageFlags flags, bool host = false) {
        if (target.handle && target.size == std::max<VkDeviceSize>(size, 16)) return;
        c.release(target);
        target = c.allocate(size, flags, host);
    };
    resize_buffer(session.output, pixelBytes + output_header_bytes, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT |
                  VK_BUFFER_USAGE_TRANSFER_SRC_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT);
    resize_buffer(session.readback, pixelBytes, VK_BUFFER_USAGE_TRANSFER_DST_BIT, true);
    resize_buffer(session.convergence, sizeof(uint32_t), VK_BUFFER_USAGE_TRANSFER_DST_BIT, true);
    const VkDeviceSize display_bytes = VkDeviceSize(cam.image_width) * cam.image_height * sizeof(uint32_t);
    resize_buffer(session.display, display_bytes, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_SRC_BIT);
    resize_buffer(session.display_readback, display_bytes, VK_BUFFER_USAGE_TRANSFER_DST_BIT, true);
    const auto output = session.output, readback = session.readback, convergence = session.convergence;
    c.submit([&](VkCommandBuffer cmd) {
        c.vkCmdFillBuffer(cmd, output.handle, 0, output.size, 0);
        c.barrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_ACCESS_TRANSFER_WRITE_BIT,
                  VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                  VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT);
    });
    auto descriptor = create_pipeline(c, output, session.display, uploaded.ranges, tlas);
    timed("pipeline");
    if (status) {
        status("Tracing on GPU...");
    }
    constexpr int tile = 256;
    int tile_group = 4;
    if(const char* override_tiles=std::getenv("DUSK_GPU_TILE_GROUP"))
        tile_group=std::clamp(std::atoi(override_tiles),1,16);
    // Keep at most three submissions in flight. GPU timestamps tune sample
    // batches toward short dispatches without changing per-pixel RNG sequences.
    int batch_samples = 4;
    c.completed_work = 0;
    c.submission_ms = 0;
    c.busy_ms = 0;
    c.synchronous = std::getenv("DUSK_GPU_SYNCHRONOUS") != nullptr;
    const bool fixed_batch = std::getenv("DUSK_GPU_BATCH_SAMPLES") != nullptr;
    if (const char* override_samples = std::getenv("DUSK_GPU_BATCH_SAMPLES")) {
        batch_samples = std::clamp(std::atoi(override_samples), 1, 4);
    }
    int columns = (cam.image_width + tile - 1) / tile, rows = (cam.image_height + tile - 1) / tile;
    int64_t total = int64_t(columns) * rows * cam.samples_per_pixel;
    if (total > INT32_MAX) {
        throw std::runtime_error("GPU render work count exceeds the progress limit.");
    }
    if (progress) {
        progress->total_tiles = int(total);
        progress->completed_tiles = 0;
        progress->total_scanlines = cam.image_height;
        progress->completed_scanlines = 0;
        progress->image_width = cam.image_width;
        progress->image_height = cam.image_height;
        progress->clear_tiles();
    }
    // Bucket previews refresh only the traced buckets, so they can follow
    // each bucket's samples closely, as Cycles' tiles do.
    double preview_interval = settings.gpu_buckets ? .15 : .75;
    if (const char* interval = std::getenv("DUSK_GPU_PREVIEW_MS")) preview_interval = std::clamp(std::atof(interval), 1., 5000.) / 1000.;
    auto lastReadback = clock::now();
    int completed = 0;
    bool stop = false;
    auto read = [&](bool final) {
        if (!final) {
            c.submit([&](VkCommandBuffer cmd) {
                c.barrier(cmd, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT | VK_PIPELINE_STAGE_TRANSFER_BIT,
                          VK_ACCESS_SHADER_WRITE_BIT | VK_ACCESS_TRANSFER_READ_BIT,
                          VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT);
                c.vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, c.pipeline);
                c.vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, c.pipelineLayout, 0, 1, &descriptor, 0, nullptr);
                uint32_t constants[4]{};
                const float exposure = float(settings.exposure);
                std::memcpy(&constants[0], &exposure, sizeof(exposure));
                c.vkCmdPushConstants(cmd, c.pipelineLayout, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(constants), constants);
                c.vkCmdDispatch(cmd, (cam.image_width + 7) / 8, (cam.image_height + 7) / 8, 1);
                c.barrier(cmd, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_ACCESS_SHADER_WRITE_BIT,
                          VK_PIPELINE_STAGE_TRANSFER_BIT, VK_ACCESS_TRANSFER_READ_BIT);
                const VkBufferCopy copy{0, 0, display_bytes};
                c.vkCmdCopyBuffer(cmd, session.display.handle, session.display_readback.handle, 1, &copy);
                c.barrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_ACCESS_TRANSFER_WRITE_BIT,
                          VK_PIPELINE_STAGE_HOST_BIT, VK_ACCESS_HOST_READ_BIT);
            });
            if (statistics) statistics->preview_readback_bytes += display_bytes;
            render_result image;
            image.width = cam.image_width;
            image.height = cam.image_height;
            image.pixels.resize(size_t(cam.image_width) * cam.image_height * 3);
            const auto* packed = static_cast<const uint32_t*>(session.display_readback.mapped);
            for (size_t p = 0; p < image.pixels.size() / 3; ++p) {
                image.pixels[p*3] = uint8_t(packed[p]);
                image.pixels[p*3+1] = uint8_t(packed[p] >> 8);
                image.pixels[p*3+2] = uint8_t(packed[p] >> 16);
            }
            return image;
        }
        if (statistics) statistics->final_readback_bytes += pixelBytes;
        c.submit([&](VkCommandBuffer cmd) {
            c.barrier(cmd, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_ACCESS_SHADER_WRITE_BIT,
                      VK_PIPELINE_STAGE_TRANSFER_BIT, VK_ACCESS_TRANSFER_READ_BIT);
            VkBufferCopy copy{output_header_bytes, 0, pixelBytes};
            c.vkCmdCopyBuffer(cmd, output.handle, readback.handle, 1, &copy);
            c.barrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_ACCESS_TRANSFER_WRITE_BIT,
                      VK_PIPELINE_STAGE_HOST_BIT, VK_ACCESS_HOST_READ_BIT);
        });
        return resolve(static_cast<const pixel*>(readback.mapped), cam.image_width, cam.image_height,
                       settings, final, final ? linear : nullptr, cancel, status);
    };
    // Host time per scheduling step, reported with DUSK_GPU_TIMINGS.
    double enqueue_ms = 0, preview_ms = 0, converge_ms = 0, readback_ms = 0;
    auto clocked = [&](double& total) {
        struct scope {
            double& total;
            clock::time_point begin = clock::now();
            ~scope() { total += std::chrono::duration<double, std::milli>(clock::now() - begin).count(); }
        };
        return scope{total};
    };
    auto converged_pixels = [&] {
        auto timer = clocked(converge_ms);
        c.submit([&](VkCommandBuffer cmd) {
            c.barrier(cmd, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_ACCESS_SHADER_WRITE_BIT,
                      VK_PIPELINE_STAGE_TRANSFER_BIT, VK_ACCESS_TRANSFER_READ_BIT);
            VkBufferCopy copy{0, 0, sizeof(uint32_t)};
            c.vkCmdCopyBuffer(cmd, output.handle, convergence.handle, 1, &copy);
            c.barrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_ACCESS_TRANSFER_WRITE_BIT,
                      VK_PIPELINE_STAGE_HOST_BIT, VK_ACCESS_HOST_READ_BIT);
        });
        return *static_cast<const uint32_t*>(convergence.mapped);
    };
    // One barrier orders a submission after the previous one. The tiles
    // inside it are independent: no barrier or host wait separates them, so
    // their dispatches overlap and hide each other's long-path tails.
    auto begin_tiles = [&](VkCommandBuffer cmd) {
        c.barrier(cmd, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT | VK_PIPELINE_STAGE_TRANSFER_BIT,
                  VK_ACCESS_SHADER_WRITE_BIT | VK_ACCESS_TRANSFER_READ_BIT,
                  VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                  VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT);
        c.vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, c.pipeline);
        c.vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, c.pipelineLayout, 0, 1,
                                  &descriptor, 0, nullptr);
    };
    // Z workgroups cover consecutive tile indices.
    auto dispatch_tiles = [&](VkCommandBuffer cmd, int sample, int samples, uint32_t first_tile, uint32_t tiles) {
        uint32_t constants[] = {uint32_t(sample), first_tile, UINT32_MAX, uint32_t(samples)};
        c.vkCmdPushConstants(cmd, c.pipelineLayout, VK_SHADER_STAGE_COMPUTE_BIT, 0,
                             sizeof(constants), constants);
        c.vkCmdDispatch(cmd, tile/8, tile/8, tiles);
    };
    auto tune_batch = [&] {
        if (!fixed_batch && c.submission_ms > 0) {
            if (c.submission_ms < 2) batch_samples = std::min(16, batch_samples * 2);
            else if (c.submission_ms > 8) batch_samples = std::max(1, batch_samples / 2);
        }
    };
    // Work credited to buckets that converged before their last sample.
    int64_t skipped = 0;
    auto report = [&] {
        double elapsed = std::chrono::duration<double>(clock::now() - start).count();
        if (progress) {
            progress->completed_tiles = int(c.completed_work + skipped);
            progress->elapsed_seconds = elapsed;
            progress->eta_seconds = elapsed / completed * (total - completed);
        }
    };
    // In bucket mode finished buckets are denoised for the viewport while the
    // next ones trace. The final image is still denoised as one frame.
    const bool buckets = settings.gpu_buckets;
    std::vector<float> bucket_color, bucket_albedo, bucket_normal, bucket_indirect;
    std::unique_ptr<dusk_tiles::denoiser> bucket_denoiser;
    if (buckets && callback && settings.use_denoiser && settings.progressive_denoise && settings.denoiser_strength > 0) {
        const size_t values = size_t(cam.image_width) * cam.image_height * 3;
        bucket_color.assign(values, 0.f);
        bucket_albedo.assign(values, 0.f);
        bucket_normal.assign(values, 0.f);
        if (settings.gpu_firefly_filter) bucket_indirect.assign(values, 0.f);
        // Half the cores: OIDN must not starve the thread feeding the GPU.
        const int threads = std::max(1, int(std::thread::hardware_concurrency()) / 2);
        bucket_denoiser = std::make_unique<dusk_tiles::denoiser>(
            cam.image_width, cam.image_height, tile, bucket_color.data(), bucket_albedo.data(), bucket_normal.data(),
            bucket_indirect.empty() ? nullptr : bucket_indirect.data(), settings.denoiser_strength, settings.exposure,
            threads, progress, cancel);
    }
    auto preview = [&] {
        auto timer = clocked(preview_ms);
        auto image = read(false);
        if (bucket_denoiser) bucket_denoiser->overlay(image.pixels);
        callback(image);
        lastReadback = clock::now();
    };
    // Bucket mode keeps a frame on the host and refreshes only the buckets
    // being traced: the display pass and readback cover just their pixels.
    render_result frame;
    if (buckets && callback) {
        frame.width = cam.image_width;
        frame.height = cam.image_height;
        frame.pixels.assign(size_t(cam.image_width) * cam.image_height * 3, 0);
    }
    auto preview_buckets = [&](const std::vector<dusk_tiles::rect>& rects) {
        auto timer = clocked(preview_ms);
        std::vector<VkBufferCopy> copies;
        VkDeviceSize bytes = 0;
        for (const auto& r : rects) {
            for (int y = r.y0; y < r.y1; ++y) {
                const VkDeviceSize offset = (VkDeviceSize(y) * cam.image_width + r.x0) * sizeof(uint32_t);
                copies.push_back({offset, offset, VkDeviceSize(r.width()) * sizeof(uint32_t)});
                bytes += VkDeviceSize(r.width()) * sizeof(uint32_t);
            }
        }
        c.submit([&](VkCommandBuffer cmd) {
            c.barrier(cmd, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT | VK_PIPELINE_STAGE_TRANSFER_BIT,
                      VK_ACCESS_SHADER_WRITE_BIT | VK_ACCESS_TRANSFER_READ_BIT,
                      VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT);
            c.vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, c.pipeline);
            c.vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, c.pipelineLayout, 0, 1, &descriptor, 0, nullptr);
            const float exposure = float(settings.exposure);
            for (const auto& r : rects) {
                // A zero sample count selects the display pass at this pixel offset.
                uint32_t constants[4]{0, uint32_t(r.x0), uint32_t(r.y0), 0};
                std::memcpy(&constants[0], &exposure, sizeof(exposure));
                c.vkCmdPushConstants(cmd, c.pipelineLayout, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(constants), constants);
                c.vkCmdDispatch(cmd, (r.width() + 7) / 8, (r.height() + 7) / 8, 1);
            }
            c.barrier(cmd, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_ACCESS_SHADER_WRITE_BIT,
                      VK_PIPELINE_STAGE_TRANSFER_BIT, VK_ACCESS_TRANSFER_READ_BIT);
            c.vkCmdCopyBuffer(cmd, session.display.handle, session.display_readback.handle, uint32_t(copies.size()),
                              copies.data());
            c.barrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_ACCESS_TRANSFER_WRITE_BIT,
                      VK_PIPELINE_STAGE_HOST_BIT, VK_ACCESS_HOST_READ_BIT);
        });
        if (statistics) statistics->preview_readback_bytes += bytes;
        const auto* packed = static_cast<const uint32_t*>(session.display_readback.mapped);
        for (const auto& r : rects) {
            for (int y = r.y0; y < r.y1; ++y) {
                for (int x = r.x0; x < r.x1; ++x) {
                    const size_t p = size_t(y) * cam.image_width + x;
                    frame.pixels[p * 3] = uint8_t(packed[p]);
                    frame.pixels[p * 3 + 1] = uint8_t(packed[p] >> 8);
                    frame.pixels[p * 3 + 2] = uint8_t(packed[p] >> 16);
                }
            }
        }
        auto image = frame;
        if (bucket_denoiser) bucket_denoiser->overlay(image.pixels);
        callback(image);
        lastReadback = clock::now();
    };
    auto preview_due = [&] {
        return callback && std::chrono::duration<double>(clock::now() - lastReadback).count() >= preview_interval;
    };
    // Copies finished buckets' accumulators to the host and resolves them as
    // resolve() does, for the bucket denoiser.
    auto read_buckets = [&](const std::vector<dusk_tiles::rect>& finished) {
        auto timer = clocked(readback_ms);
        std::vector<VkBufferCopy> copies;
        VkDeviceSize bytes = 0;
        for (const auto& r : finished) {
            for (int y = r.y0; y < r.y1; ++y) {
                const VkDeviceSize offset = (VkDeviceSize(y) * cam.image_width + r.x0) * sizeof(pixel);
                copies.push_back({output_header_bytes + offset, offset, VkDeviceSize(r.width()) * sizeof(pixel)});
                bytes += VkDeviceSize(r.width()) * sizeof(pixel);
            }
        }
        c.submit([&](VkCommandBuffer cmd) {
            c.barrier(cmd, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_ACCESS_SHADER_WRITE_BIT,
                      VK_PIPELINE_STAGE_TRANSFER_BIT, VK_ACCESS_TRANSFER_READ_BIT);
            c.vkCmdCopyBuffer(cmd, output.handle, readback.handle, uint32_t(copies.size()), copies.data());
            c.barrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_ACCESS_TRANSFER_WRITE_BIT,
                      VK_PIPELINE_STAGE_HOST_BIT, VK_ACCESS_HOST_READ_BIT);
        });
        if (statistics) statistics->preview_readback_bytes += bytes;
        const auto* pixels = static_cast<const pixel*>(readback.mapped);
        for (const auto& r : finished) {
            for (int y = r.y0; y < r.y1; ++y) {
                for (int x = r.x0; x < r.x1; ++x) {
                    const size_t p = size_t(y) * cam.image_width + x;
                    const float count = std::max(1.f, pixels[p].sum.w);
                    for (int k = 0; k < 3; ++k) {
                        const float value = pixels[p].sum[k] / count;
                        bucket_color[p * 3 + k] = std::isfinite(value) ? std::max(0.f, value) : 0;
                        bucket_albedo[p * 3 + k] = pixels[p].albedo[k] / count;
                        bucket_normal[p * 3 + k] = pixels[p].normal[k] / count;
                        if (!bucket_indirect.empty()) {
                            const float energy = pixels[p].indirect[k] / count;
                            bucket_indirect[p * 3 + k] = std::isfinite(energy) ? std::max(0.f, energy) : 0;
                        }
                    }
                }
            }
            bucket_denoiser->finished(r);
        }
    };
    if (buckets) {
        // Each group of buckets takes every sample before the next group
        // starts, like Cycles' GPU tiles. Per-pixel RNG streams depend only on
        // the pixel and sample index, so the image matches progressive mode.
        const auto order = dusk_tiles::make_tiles(cam.image_width, cam.image_height, tile, settings.bucket_order);
        bool every_group_converged = settings.adaptive_sampling;
        for (size_t first = 0; first < order.size() && !stop; first += size_t(tile_group)) {
            const std::vector<dusk_tiles::rect> group(order.begin() + first,
                                                      order.begin() + std::min(order.size(), first + size_t(tile_group)));
            const int count = int(group.size());
            uint32_t group_pixels = 0;
            std::vector<uint32_t> indices;
            for (const auto& r : group) {
                group_pixels += uint32_t(r.width() * r.height());
                indices.push_back(uint32_t((r.y0 / tile) * columns + r.x0 / tile));
                if (progress) progress->begin_tile({r.x0, r.y0, r.width(), r.height(), render_tile_activity::rendering});
            }
            // Consecutive tile indices share one dispatch.
            std::sort(indices.begin(), indices.end());
            std::vector<std::pair<uint32_t, uint32_t>> runs;
            for (uint32_t index : indices) {
                if (!runs.empty() && runs.back().first + runs.back().second == index) ++runs.back().second;
                else runs.push_back({index, 1});
            }
            const uint32_t converged_before = settings.adaptive_sampling ? converged_pixels() : 0;
            int next_check = std::max(16, settings.adaptive_min_samples);
            bool converged = false;
            int sample = 0;
            while (sample < cam.samples_per_pixel) {
                if (cancelled()) {
                    stop = true;
                    break;
                }
                const int samples = std::min(batch_samples, cam.samples_per_pixel - sample);
                {
                    auto timer = clocked(enqueue_ms);
                    c.enqueue(samples * count, [&](VkCommandBuffer cmd) {
                        begin_tiles(cmd);
                        for (const auto& run : runs) dispatch_tiles(cmd, sample, samples, run.first, run.second);
                    });
                }
                completed += samples * count;
                sample += samples;
                report();
                if (preview_due()) preview_buckets(group);
                tune_batch();
                if (settings.adaptive_sampling && sample < cam.samples_per_pixel && sample >= next_check) {
                    next_check = sample + std::max(16, settings.adaptive_check_interval);
                    if (converged_pixels() - converged_before == group_pixels) {
                        converged = true;
                        const int64_t rest = int64_t(cam.samples_per_pixel - sample) * count;
                        skipped += rest;
                        completed += int(rest);
                    }
                }
                if (converged) break;
            }
            if (!converged) every_group_converged = false;
            if (progress) {
                for (const auto& r : group)
                    progress->end_tile({r.x0, r.y0, r.width(), r.height(), render_tile_activity::rendering});
            }
            if (stop) break;
            if (bucket_denoiser) read_buckets(group);
            if (callback) preview_buckets(group);
        }
        if (!stop && every_group_converged && status) status("Adaptive sampling converged.");
    } else {
        int next_convergence_check = std::max(16, settings.adaptive_min_samples);
        for (int sample = 0; sample < cam.samples_per_pixel && !stop;) {
            const int samples = std::min(batch_samples, cam.samples_per_pixel - sample);
            for (int first_tile = 0; first_tile < columns*rows && !stop; first_tile += tile_group) {
                const int tiles=std::min(tile_group,columns*rows-first_tile);
                if (cancelled()) {
                    stop = true;
                    break;
                }
                {
                    auto timer = clocked(enqueue_ms);
                    c.enqueue(samples*tiles, [&](VkCommandBuffer cmd) {
                        begin_tiles(cmd);
                        dispatch_tiles(cmd, sample, samples, uint32_t(first_tile), uint32_t(tiles));
                    });
                }
                completed += samples*tiles;
                report();
                if (preview_due()) preview();
            }
            tune_batch();
            if (!stop && settings.adaptive_sampling && sample + samples >= next_convergence_check) {
                next_convergence_check = sample + samples + std::max(16, settings.adaptive_check_interval);
                if (converged_pixels() == uint32_t(cam.image_width * cam.image_height)) {
                    if (status) {
                        status("Adaptive sampling converged.");
                    }
                    break;
                }
            }
            sample += samples;
        }
    }
    c.drain();
    if (bucket_denoiser) bucket_denoiser->stop();
    if (progress) {
        progress->clear_tiles();
        progress->completed_tiles = int(c.completed_work + skipped);
    }
    if (progress && !stop) {
        progress->completed_tiles = int(total);
        progress->completed_scanlines = cam.image_height;
        progress->eta_seconds = 0;
    }
    if (timings) {
        std::cerr << "GPU busy in tracing submissions: " << c.busy_ms << " ms; host enqueue " << enqueue_ms
                  << " ms, previews " << preview_ms << " ms, convergence " << converge_ms << " ms, bucket readback "
                  << readback_ms << " ms\n";
    }
    timed("tracing");
    // Publish the completed raw preview before final OIDN. Timer throttling can
    // otherwise skip the last batch, leaving the viewport at an older sample count.
    if(callback && !stop) {
        preview();
        stop=cancelled();
    }
    auto result = read(!stop);
    if (stop && bucket_denoiser) bucket_denoiser->overlay(result.pixels);
    timed("resolve");
    retain.committed = true;
    return result;
}
} // namespace dusk_gpu
