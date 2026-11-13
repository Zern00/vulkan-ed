#include <vulkan/vulkan.h>
#include <vulkan/vk_enum_string_helper.h>

#include <iostream>
#include <cstdlib>
#include <stdexcept>
#include <vector>
#include <optional>
#include <algorithm>
#include <cstdint>
#include <cstring>
#include <array>
#include <utility>
#include <filesystem>
#include <fstream>
#include <format>
#include <cmath>
#include <source_location>

static VKAPI_ATTR VkBool32 VKAPI_CALL DebugCallback(VkDebugUtilsMessageSeverityFlagBitsEXT,
                                                    VkDebugUtilsMessageTypeFlagsEXT,
                                                    const VkDebugUtilsMessengerCallbackDataEXT* callbackData, void*) {
    std::cerr << "[Vulkan Validation] " << callbackData->pMessage << '\n';
    return VK_FALSE;
}

inline void CheckVkResult(VkResult result, const char* functionName,
                          std::source_location location = std::source_location::current()) {
    if (result != VK_SUCCESS) {
        throw std::runtime_error(std::format("{}:{} [{}] {} error {}", location.file_name(), location.line(),
                                             location.function_name(), functionName, string_VkResult(result)));
    }
}

#define VK_CHECK(call) CheckVkResult((call), #call)

std::optional<uint32_t> FindQueueFamily(VkPhysicalDevice device) {
    uint32_t count = 0;
    vkGetPhysicalDeviceQueueFamilyProperties(device, &count, nullptr);
    std::vector<VkQueueFamilyProperties> families(count);
    vkGetPhysicalDeviceQueueFamilyProperties(device, &count, families.data());
    for (uint32_t i = 0; i < count; ++i) {
        if (families[i].queueCount > 0 && (families[i].queueFlags & VK_QUEUE_COMPUTE_BIT)) {
            return i;
        }
    }
    return std::nullopt;
}

VkPhysicalDevice FindPhysicalDevice(VkInstance instance) {
    uint32_t count = 0;
    VK_CHECK(vkEnumeratePhysicalDevices(instance, &count, nullptr));
    std::vector<VkPhysicalDevice> devices(count);
    VK_CHECK(vkEnumeratePhysicalDevices(instance, &count, devices.data()));
    VkPhysicalDevice selected = VK_NULL_HANDLE;
    for (VkPhysicalDevice device : devices) {
        VkPhysicalDeviceProperties properties{};
        vkGetPhysicalDeviceProperties(device, &properties);
        if (properties.apiVersion < VK_API_VERSION_1_3 || !FindQueueFamily(device)) {
            continue;
        }
        VkPhysicalDeviceVulkan13Features features13{};
        features13.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES;
        VkPhysicalDeviceVulkan12Features features12{};
        features12.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES;
        features12.pNext = &features13;
        VkPhysicalDeviceFeatures2 features{};
        features.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2;
        features.pNext = &features12;
        vkGetPhysicalDeviceFeatures2(device, &features);
        if (!(features13.synchronization2 && features12.timelineSemaphore)) {
            continue;
        }
        if (properties.deviceType == VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU) {
            return device;
        }
        if (selected == VK_NULL_HANDLE) {
            selected = device;
        }
    }
    if (selected == VK_NULL_HANDLE) {
        throw std::runtime_error("No Vulkan 1.3 device with the required features and queue");
    }
    return selected;
}

void PrintMemoryProperties(VkPhysicalDevice physicalDevice) {
    VkPhysicalDeviceMemoryProperties memory{};
    vkGetPhysicalDeviceMemoryProperties(physicalDevice, &memory);
    std::cout << std::format("Memory: {} types, {} heaps\n", memory.memoryTypeCount, memory.memoryHeapCount);
    const std::array<std::pair<VkMemoryPropertyFlags, const char*>, 4> flags{
        {{VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, "device-local"},
         {VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT, "host-visible"},
         {VK_MEMORY_PROPERTY_HOST_COHERENT_BIT, "coherent"},
         {VK_MEMORY_PROPERTY_HOST_CACHED_BIT, "cached"}}};
    for (uint32_t i = 0; i < memory.memoryTypeCount; ++i) {
        std::cout << std::format("  Type {} (heap {}):", i, memory.memoryTypes[i].heapIndex);
        for (const auto& [flag, name] : flags) {
            if (memory.memoryTypes[i].propertyFlags & flag) {
                std::cout << ' ' << name;
            }
        }
        std::cout << '\n';
    }
    std::cout << '\n';
}

uint32_t FindMemoryTypes(VkPhysicalDevice physicalDevice, uint32_t memoryTypeBits,
                         VkMemoryPropertyFlags requiredProperties) {
    VkPhysicalDeviceMemoryProperties memoryProperties{};
    vkGetPhysicalDeviceMemoryProperties(physicalDevice, &memoryProperties);

    for (uint32_t i = 0; i < memoryProperties.memoryTypeCount; ++i) {
        const bool allowedForBuffer = (memoryTypeBits & (1u << i)) != 0;
        const bool hasRequiredProperties =
            (memoryProperties.memoryTypes[i].propertyFlags & requiredProperties) == requiredProperties;
        if (allowedForBuffer && hasRequiredProperties) {
            return i;
        }
    }
    throw std::runtime_error("Failed to find memory type with required properties for the resource");
}

std::vector<uint32_t> ReadShaderFile(const std::filesystem::path& path) {
    std::ifstream file(path, std::ios::binary | std::ios::ate);
    if (!file) {
        throw std::runtime_error("Failed to open SPIR-V file");
    }

    auto fileSize = file.tellg();
    if (fileSize <= 0 || fileSize % sizeof(uint32_t) != 0) {
        throw std::runtime_error("Invalid SPIR-V file size");
    }

    std::vector<uint32_t> code(static_cast<size_t>(fileSize) / sizeof(uint32_t));
    file.seekg(0, std::ios::beg);
    if (!file.read(reinterpret_cast<char*>(code.data()), static_cast<std::streamsize>(fileSize))) {
        throw std::runtime_error("Failed to read SPIR-V");
    }

    return code;
}

struct alignas(16) Params {
    std::array<float, 4> multiplier;
    std::array<float, 4> bias;
};

static_assert(sizeof(Params) == 32);

void CreateHostBuffer(VkDevice device, VkPhysicalDevice mainPhysicalDevice, VkDeviceSize size, VkBufferUsageFlags usage,
                      VkBuffer& buffer, VkDeviceMemory& memory) {
    VkBufferCreateInfo bufferInfo{};
    bufferInfo.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
    bufferInfo.size = size;
    bufferInfo.usage = usage;
    bufferInfo.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    VK_CHECK(vkCreateBuffer(device, &bufferInfo, nullptr, &buffer));

    VkMemoryRequirements requirements{};
    vkGetBufferMemoryRequirements(device, buffer, &requirements);

    VkMemoryAllocateInfo allocationInfo{};
    allocationInfo.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
    allocationInfo.allocationSize = requirements.size;
    allocationInfo.memoryTypeIndex =
        FindMemoryTypes(mainPhysicalDevice, requirements.memoryTypeBits,
                        VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
    VK_CHECK(vkAllocateMemory(device, &allocationInfo, nullptr, &memory));
    VK_CHECK(vkBindBufferMemory(device, buffer, memory, 0));
}

void Run() {
    VkApplicationInfo appInfo{};
    appInfo.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO;
    appInfo.applicationVersion = VK_MAKE_API_VERSION(0, 1, 0, 0);
    appInfo.apiVersion = VK_API_VERSION_1_3;
    appInfo.engineVersion = VK_MAKE_API_VERSION(0, 1, 0, 0);
    appInfo.pEngineName = "No engine";
    appInfo.pApplicationName = "image using test";

    const char* layers[] = {"VK_LAYER_KHRONOS_validation"};

    const char* extensions[] = {VK_EXT_DEBUG_UTILS_EXTENSION_NAME};

    VkDebugUtilsMessengerCreateInfoEXT debugInfo{};
    debugInfo.sType = VK_STRUCTURE_TYPE_DEBUG_UTILS_MESSENGER_CREATE_INFO_EXT;
    debugInfo.messageSeverity =
        VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT | VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT;
    debugInfo.messageType = VK_DEBUG_UTILS_MESSAGE_TYPE_GENERAL_BIT_EXT |
                            VK_DEBUG_UTILS_MESSAGE_TYPE_VALIDATION_BIT_EXT |
                            VK_DEBUG_UTILS_MESSAGE_TYPE_PERFORMANCE_BIT_EXT;
    debugInfo.pfnUserCallback = DebugCallback;

    VkInstanceCreateInfo instanceInfo{};
    instanceInfo.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO;
    instanceInfo.pApplicationInfo = &appInfo;
    instanceInfo.enabledLayerCount = 1;
    instanceInfo.ppEnabledLayerNames = layers;
    instanceInfo.enabledExtensionCount = 1;
    instanceInfo.ppEnabledExtensionNames = extensions;
    instanceInfo.pNext = &debugInfo;

    VkInstance instance = VK_NULL_HANDLE;
    VK_CHECK(vkCreateInstance(&instanceInfo, nullptr, &instance));

    VkDebugUtilsMessengerEXT debugMessenger = VK_NULL_HANDLE;

    auto createDebugMessenger = reinterpret_cast<PFN_vkCreateDebugUtilsMessengerEXT>(
        vkGetInstanceProcAddr(instance, "vkCreateDebugUtilsMessengerEXT"));

    if (!createDebugMessenger) {
        throw std::runtime_error("vkCreateDebugUtilsMessengerEXT not found");
    }

    VK_CHECK(createDebugMessenger(instance, &debugInfo, nullptr, &debugMessenger));

    VkPhysicalDevice mainPhysicalDevice = FindPhysicalDevice(instance);

    VkPhysicalDeviceProperties mainPhysicalDeviceProperties{};
    vkGetPhysicalDeviceProperties(mainPhysicalDevice, &mainPhysicalDeviceProperties);
    std::cout << "Selected device : " << mainPhysicalDeviceProperties.deviceName << "\n";

    PrintMemoryProperties(mainPhysicalDevice);
    const uint32_t computeFamilyIndex = FindQueueFamily(mainPhysicalDevice).value();

    float priority = 1.0f;
    VkDeviceQueueCreateInfo deviceQueueInfo{};
    deviceQueueInfo.sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO;
    deviceQueueInfo.queueCount = 1;
    deviceQueueInfo.pQueuePriorities = &priority;
    deviceQueueInfo.queueFamilyIndex = computeFamilyIndex;

    VkPhysicalDeviceSynchronization2Features sync2Support{};
    sync2Support.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SYNCHRONIZATION_2_FEATURES;

    VkPhysicalDeviceTimelineSemaphoreFeatures timelineSemaphoreSupport{};
    timelineSemaphoreSupport.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_TIMELINE_SEMAPHORE_FEATURES;

    VkPhysicalDeviceFeatures2 features{};
    features.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2;

    features.pNext = &sync2Support;
    sync2Support.pNext = &timelineSemaphoreSupport;

    vkGetPhysicalDeviceFeatures2(mainPhysicalDevice, &features);
    if (sync2Support.synchronization2 != VK_TRUE) {
        throw std::runtime_error("Synchronization2Features is not supported");
    }

    if (timelineSemaphoreSupport.timelineSemaphore != VK_TRUE) {
        throw std::runtime_error("TimelineSemaphoreFeatures  is not supported");
    }

    VkPhysicalDeviceTimelineSemaphoreFeatures timelineFeatures{};
    timelineFeatures.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_TIMELINE_SEMAPHORE_FEATURES;
    timelineFeatures.timelineSemaphore = VK_TRUE;

    VkPhysicalDeviceSynchronization2Features synchronization2Features{};
    synchronization2Features.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SYNCHRONIZATION_2_FEATURES;
    synchronization2Features.synchronization2 = VK_TRUE;
    synchronization2Features.pNext = &timelineFeatures;

    VkPhysicalDeviceFeatures deviceFeatures{};

    VkDeviceCreateInfo deviceInfo{};
    deviceInfo.sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO;
    deviceInfo.pNext = &synchronization2Features;
    deviceInfo.queueCreateInfoCount = 1;
    deviceInfo.pQueueCreateInfos = &deviceQueueInfo;
    deviceInfo.pEnabledFeatures = &deviceFeatures;

    VkDevice device = VK_NULL_HANDLE;
    VK_CHECK(vkCreateDevice(mainPhysicalDevice, &deviceInfo, nullptr, &device));

    constexpr std::array<Params, 2> params{
        {{{1.0f, 1.0f, 1.0f, 1.0f}, {0.0f, 0.0f, 0.0f, 0.0f}}, {{1.0f, 1.0f, 1.0f, 1.0f}, {0.125f, 0.0f, 0.0f, 0.0f}}}};
    const VkDeviceSize uboAlignment = mainPhysicalDeviceProperties.limits.minUniformBufferOffsetAlignment;
    const VkDeviceSize uboStride = (sizeof(Params) + uboAlignment - 1) / uboAlignment * uboAlignment;
    const VkDeviceSize uboSize = uboStride * params.size();

    VkBuffer ubo = VK_NULL_HANDLE;
    VkDeviceMemory uboMemory = VK_NULL_HANDLE;
    CreateHostBuffer(device, mainPhysicalDevice, uboSize, VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT, ubo, uboMemory);

    VkBuffer ssbo = VK_NULL_HANDLE;
    VkDeviceMemory ssboMemory = VK_NULL_HANDLE;
    CreateHostBuffer(device, mainPhysicalDevice, sizeof(std::array<float, 4>), VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, ssbo,
                     ssboMemory);

    void* mappedUbo = nullptr;
    void* mappedSsbo = nullptr;
    VK_CHECK(vkMapMemory(device, uboMemory, 0, uboSize, 0, &mappedUbo));
    for (size_t i = 0; i < params.size(); ++i) {
        std::memcpy(static_cast<uint8_t*>(mappedUbo) + i * uboStride, &params[i], sizeof(Params));
    }
    vkUnmapMemory(device, uboMemory);
    VK_CHECK(vkMapMemory(device, ssboMemory, 0, sizeof(std::array<float, 4>), 0, &mappedSsbo));

    VkQueue computeQueue = VK_NULL_HANDLE;
    vkGetDeviceQueue(device, deviceQueueInfo.queueFamilyIndex, 0, &computeQueue);

    const std::array<VkDescriptorSetLayoutBinding, 2> bindings{
        {{0, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr},
         {1, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr}}};

    VkDescriptorSetLayoutCreateInfo layoutInfo{};
    layoutInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
    layoutInfo.bindingCount = static_cast<uint32_t>(bindings.size());
    layoutInfo.pBindings = bindings.data();

    VkDescriptorSetLayout descriptorLayout = VK_NULL_HANDLE;
    VK_CHECK(vkCreateDescriptorSetLayout(device, &layoutInfo, nullptr, &descriptorLayout));

    const std::array<VkDescriptorPoolSize, 2> poolSizes{
        {{VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC, 1}, {VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1}}};
    VkDescriptorPoolCreateInfo poolInfo{};
    poolInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
    poolInfo.maxSets = 1;
    poolInfo.poolSizeCount = static_cast<uint32_t>(poolSizes.size());
    poolInfo.pPoolSizes = poolSizes.data();

    VkDescriptorPool descriptorPool = VK_NULL_HANDLE;
    VK_CHECK(vkCreateDescriptorPool(device, &poolInfo, nullptr, &descriptorPool));

    VkDescriptorSetAllocateInfo descriptorAllocInfo{};
    descriptorAllocInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
    descriptorAllocInfo.descriptorPool = descriptorPool;
    descriptorAllocInfo.descriptorSetCount = 1;
    descriptorAllocInfo.pSetLayouts = &descriptorLayout;

    VkDescriptorSet descriptorSet = VK_NULL_HANDLE;
    VK_CHECK(vkAllocateDescriptorSets(device, &descriptorAllocInfo, &descriptorSet));

    VkDescriptorBufferInfo uboDescriptor{ubo, 0, sizeof(Params)};
    VkDescriptorBufferInfo ssboDescriptor{ssbo, 0, sizeof(std::array<float, 4>)};
    VkWriteDescriptorSet write{};
    write.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    write.dstSet = descriptorSet;
    write.descriptorCount = 1;
    write.dstBinding = 0;
    write.descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC;
    write.pBufferInfo = &uboDescriptor;
    vkUpdateDescriptorSets(device, 1, &write, 0, nullptr);

    write.dstBinding = 1;
    write.descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
    write.pBufferInfo = &ssboDescriptor;
    vkUpdateDescriptorSets(device, 1, &write, 0, nullptr);

    VkPipelineLayoutCreateInfo pipelineLayoutInfo{};
    pipelineLayoutInfo.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
    pipelineLayoutInfo.setLayoutCount = 1;
    pipelineLayoutInfo.pSetLayouts = &descriptorLayout;
    pipelineLayoutInfo.pushConstantRangeCount = 0;
    pipelineLayoutInfo.pPushConstantRanges = nullptr;

    VkPipelineLayout pipelineLayout = VK_NULL_HANDLE;
    VK_CHECK(vkCreatePipelineLayout(device, &pipelineLayoutInfo, nullptr, &pipelineLayout));

    std::vector<uint32_t> computeShaderCode = ReadShaderFile(IMAGE_SHADER_PATH);
    VkShaderModuleCreateInfo computeModuleInfo{};
    computeModuleInfo.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;
    computeModuleInfo.flags = 0;
    computeModuleInfo.codeSize = computeShaderCode.size() * sizeof(uint32_t);
    computeModuleInfo.pCode = computeShaderCode.data();

    VkShaderModule computeModule = VK_NULL_HANDLE;
    VK_CHECK(vkCreateShaderModule(device, &computeModuleInfo, nullptr, &computeModule));

    VkPipelineShaderStageCreateInfo shaderPipelineStageInfo{};
    shaderPipelineStageInfo.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    shaderPipelineStageInfo.stage = VK_SHADER_STAGE_COMPUTE_BIT;
    shaderPipelineStageInfo.module = computeModule;
    shaderPipelineStageInfo.pName = "main";

    VkComputePipelineCreateInfo computeStageInfo{};
    computeStageInfo.sType = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO;
    computeStageInfo.stage = shaderPipelineStageInfo;
    computeStageInfo.layout = pipelineLayout;
    computeStageInfo.basePipelineHandle = VK_NULL_HANDLE;
    computeStageInfo.basePipelineIndex = -1;

    VkPipeline computePipeline = VK_NULL_HANDLE;
    VK_CHECK(vkCreateComputePipelines(device, VK_NULL_HANDLE, 1, &computeStageInfo, nullptr, &computePipeline));

    VkCommandPoolCreateInfo commandPoolCreateInfo{};
    commandPoolCreateInfo.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
    commandPoolCreateInfo.queueFamilyIndex = deviceQueueInfo.queueFamilyIndex;
    commandPoolCreateInfo.flags =
        VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT | VK_COMMAND_POOL_CREATE_TRANSIENT_BIT;

    VkCommandPool commandPool = VK_NULL_HANDLE;
    VK_CHECK(vkCreateCommandPool(device, &commandPoolCreateInfo, nullptr, &commandPool));

    VkCommandBufferAllocateInfo commandBufferAllocInfo{};
    commandBufferAllocInfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
    commandBufferAllocInfo.commandPool = commandPool;
    commandBufferAllocInfo.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    commandBufferAllocInfo.commandBufferCount = 1;

    VkCommandBuffer commandBuffer = VK_NULL_HANDLE;
    VK_CHECK(vkAllocateCommandBuffers(device, &commandBufferAllocInfo, &commandBuffer));

    VkCommandBufferBeginInfo beginInfo{};
    beginInfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
    beginInfo.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;

    VkCommandBufferSubmitInfo commandBufferSubmitInfo{};
    commandBufferSubmitInfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_SUBMIT_INFO;
    commandBufferSubmitInfo.commandBuffer = commandBuffer;

    VkSubmitInfo2 submitInfo{};
    submitInfo.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO_2;
    submitInfo.commandBufferInfoCount = 1;
    submitInfo.pCommandBufferInfos = &commandBufferSubmitInfo;

    VkFenceCreateInfo fenceInfo{};
    fenceInfo.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;

    VkFence fence = VK_NULL_HANDLE;
    VK_CHECK(vkCreateFence(device, &fenceInfo, nullptr, &fence));

    constexpr std::array<std::array<float, 4>, 2> expected{{{0.25f, 0.5f, 0.75f, 1.0f}, {0.375f, 0.5f, 0.75f, 1.0f}}};

    for (size_t pass = 0; pass < params.size(); ++pass) {
        if (pass == 1) {
            VK_CHECK(vkResetCommandBuffer(commandBuffer, 0));
            VK_CHECK(vkResetFences(device, 1, &fence));
        }
        constexpr std::array<float, 4> unwritten{-1.0f, -1.0f, -1.0f, -1.0f};
        std::memcpy(mappedSsbo, unwritten.data(), sizeof(unwritten));

        VK_CHECK(vkBeginCommandBuffer(commandBuffer, &beginInfo));
        vkCmdBindPipeline(commandBuffer, VK_PIPELINE_BIND_POINT_COMPUTE, computePipeline);
        const uint32_t dynamicOffset = static_cast<uint32_t>(pass * uboStride);
        vkCmdBindDescriptorSets(commandBuffer, VK_PIPELINE_BIND_POINT_COMPUTE, pipelineLayout, 0, 1, &descriptorSet, 1,
                                &dynamicOffset);
        vkCmdDispatch(commandBuffer, 1, 1, 1);

        VkMemoryBarrier2 hostBarrier{};
        hostBarrier.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER_2;
        hostBarrier.srcStageMask = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT;
        hostBarrier.srcAccessMask = VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT;
        hostBarrier.dstStageMask = VK_PIPELINE_STAGE_2_HOST_BIT;
        hostBarrier.dstAccessMask = VK_ACCESS_2_HOST_READ_BIT;

        VkDependencyInfo dependencyInfo{};
        dependencyInfo.sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO;
        dependencyInfo.memoryBarrierCount = 1;
        dependencyInfo.pMemoryBarriers = &hostBarrier;
        vkCmdPipelineBarrier2(commandBuffer, &dependencyInfo);
        VK_CHECK(vkEndCommandBuffer(commandBuffer));

        VK_CHECK(vkQueueSubmit2(computeQueue, 1, &submitInfo, fence));
        VK_CHECK(vkWaitForFences(device, 1, &fence, VK_TRUE, UINT64_MAX));

        std::array<float, 4> result{};
        std::memcpy(result.data(), mappedSsbo, sizeof(result));
        for (size_t i = 0; i < result.size(); ++i) {
            if (!std::isfinite(result[i]) || std::abs(result[i] - expected[pass][i]) > 0.000001f) {
                throw std::runtime_error(std::format("Pass {}: SSBO component {} expected {}, got {}", pass + 1, i,
                                                     expected[pass][i], result[i]));
            }
        }
        std::cout << std::format("Pass {} (UBO offset {}): SSBO = ({}, {}, {}, {}); verified.\n", pass + 1,
                                 dynamicOffset, result[0], result[1], result[2], result[3]);
    }
    std::cout << "Both compute passes verified; dynamic offsets selected different UBO data.\n";

    vkDestroyCommandPool(device, commandPool, nullptr);
    vkDestroyFence(device, fence, nullptr);
    vkDestroyPipeline(device, computePipeline, nullptr);
    vkDestroyPipelineLayout(device, pipelineLayout, nullptr);
    vkDestroyShaderModule(device, computeModule, nullptr);
    vkDestroyDescriptorPool(device, descriptorPool, nullptr);
    vkDestroyDescriptorSetLayout(device, descriptorLayout, nullptr);

    vkUnmapMemory(device, ssboMemory);
    vkDestroyBuffer(device, ubo, nullptr);
    vkFreeMemory(device, uboMemory, nullptr);
    vkDestroyBuffer(device, ssbo, nullptr);
    vkFreeMemory(device, ssboMemory, nullptr);

    auto destroyDebugMessenger = reinterpret_cast<PFN_vkDestroyDebugUtilsMessengerEXT>(
        vkGetInstanceProcAddr(instance, "vkDestroyDebugUtilsMessengerEXT"));

    vkDestroyDevice(device, nullptr);
    if (destroyDebugMessenger) {
        destroyDebugMessenger(instance, debugMessenger, nullptr);
    }
    vkDestroyInstance(instance, nullptr);
}

int main() {
    try {
        Run();
        return EXIT_SUCCESS;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return EXIT_FAILURE;
    }
}
