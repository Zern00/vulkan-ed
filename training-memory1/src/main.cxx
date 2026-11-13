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
#include <format>

void CheckVkResult(VkResult result, const char* operation) {
    if (result != VK_SUCCESS) {
        throw std::runtime_error(std::format("{} failed: {}", operation, string_VkResult(result)));
    }
}

#define VK_CHECK(call) CheckVkResult((call), #call)

uint32_t FindMemoryType(VkPhysicalDevice physicalDevice, uint32_t memoryTypeBits,
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
    throw std::runtime_error("Failed to find host-visible, host-coherent memory for the buffer");
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

static VKAPI_ATTR VkBool32 VKAPI_CALL DebugCallback(VkDebugUtilsMessageSeverityFlagBitsEXT,
                                                    VkDebugUtilsMessageTypeFlagsEXT,
                                                    const VkDebugUtilsMessengerCallbackDataEXT* callbackData, void*) {
    std::cerr << "[Vulkan Validation] " << callbackData->pMessage << '\n';
    return VK_FALSE;
}

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

void Run() {
    VkApplicationInfo appInfo{};
    appInfo.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO;
    appInfo.pEngineName = "No engine";
    appInfo.pApplicationName = "Check memory";
    appInfo.apiVersion = VK_API_VERSION_1_3;
    appInfo.applicationVersion = VK_MAKE_VERSION(1, 0, 0);
    appInfo.engineVersion = VK_MAKE_VERSION(1, 0, 0);

    const char* layers[] = {"VK_LAYER_KHRONOS_validation"};

    const char* extensions[] = {VK_EXT_DEBUG_UTILS_EXTENSION_NAME};

    VkDebugUtilsMessengerCreateInfoEXT debugInfo{};
    debugInfo.sType = VK_STRUCTURE_TYPE_DEBUG_UTILS_MESSENGER_CREATE_INFO_EXT;

    debugInfo.messageSeverity =
        VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT | VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT;
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
    const VkPhysicalDevice mainPhysicalDevice = FindPhysicalDevice(instance);

    VkPhysicalDeviceProperties mainPhysicalDeviceProp;
    vkGetPhysicalDeviceProperties(mainPhysicalDevice, &mainPhysicalDeviceProp);
    std::cout << "Selected device : " << mainPhysicalDeviceProp.deviceName << "\n";

    PrintMemoryProperties(mainPhysicalDevice);
    const uint32_t graphicsFamilyIndex = FindQueueFamily(mainPhysicalDevice).value();

    float priority = 1.0f;
    VkDeviceQueueCreateInfo deviceQueueInfo{};
    deviceQueueInfo.sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO;
    deviceQueueInfo.queueCount = 1;
    deviceQueueInfo.pQueuePriorities = &priority;
    deviceQueueInfo.queueFamilyIndex = graphicsFamilyIndex;

    VkPhysicalDeviceTimelineSemaphoreFeatures supportedTimeline{};
    supportedTimeline.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_TIMELINE_SEMAPHORE_FEATURES;

    VkPhysicalDeviceFeatures2 features{};
    features.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2;
    features.pNext = &supportedTimeline;

    vkGetPhysicalDeviceFeatures2(mainPhysicalDevice, &features);

    if (!supportedTimeline.timelineSemaphore) {
        throw std::runtime_error("Timeline semaphores not supported");
    }

    VkPhysicalDeviceTimelineSemaphoreFeatures timelineFeatures{};
    timelineFeatures.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_TIMELINE_SEMAPHORE_FEATURES;
    timelineFeatures.timelineSemaphore = VK_TRUE;

    VkPhysicalDeviceSynchronization2Features synchronization2Features{};
    synchronization2Features.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SYNCHRONIZATION_2_FEATURES;
    synchronization2Features.synchronization2 = VK_TRUE;
    synchronization2Features.pNext = &timelineFeatures;

    VkDeviceCreateInfo deviceInfo{};
    deviceInfo.sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO;
    deviceInfo.pNext = &synchronization2Features;
    deviceInfo.queueCreateInfoCount = 1;
    deviceInfo.pQueueCreateInfos = &deviceQueueInfo;

    VkDevice device = VK_NULL_HANDLE;
    VK_CHECK(vkCreateDevice(mainPhysicalDevice, &deviceInfo, nullptr, &device));

    struct Vertex {
        double pos[2];
        double color[3];
    };
    const std::array<Vertex, 3> vertices = {
        {{{0.0f, -0.5f}, {1.0f, 0.0f, 0.0f}}, {{0.5f, 0.5f}, {0.0f, 1.0f, 0.0f}}, {{-0.5f, 0.5f}, {0.0f, 0.0f, 1.0f}}}};

    const VkDeviceSize stagingBufferSize = sizeof(vertices);

    VkBufferCreateInfo stagingBufferInfo{};
    stagingBufferInfo.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
    stagingBufferInfo.size = stagingBufferSize;
    stagingBufferInfo.usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT;
    stagingBufferInfo.sharingMode = VK_SHARING_MODE_EXCLUSIVE;

    VkBuffer stagingBuffer = VK_NULL_HANDLE;
    VK_CHECK(vkCreateBuffer(device, &stagingBufferInfo, nullptr, &stagingBuffer));

    VkMemoryRequirements stagingMemoryRequirements{};
    vkGetBufferMemoryRequirements(device, stagingBuffer, &stagingMemoryRequirements);

    VkMemoryAllocateInfo stagingAllocationInfo{};
    stagingAllocationInfo.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
    stagingAllocationInfo.allocationSize = stagingMemoryRequirements.size;
    stagingAllocationInfo.memoryTypeIndex =
        FindMemoryType(mainPhysicalDevice, stagingMemoryRequirements.memoryTypeBits,
                       VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);

    VkDeviceMemory stagingBufferMemory = VK_NULL_HANDLE;
    VK_CHECK(vkAllocateMemory(device, &stagingAllocationInfo, nullptr, &stagingBufferMemory));

    VK_CHECK(vkBindBufferMemory(device, stagingBuffer, stagingBufferMemory, 0));

    void* mappedData = nullptr;
    VK_CHECK(vkMapMemory(device, stagingBufferMemory, 0, stagingBufferSize, 0, &mappedData));
    std::memcpy(mappedData, vertices.data(), static_cast<size_t>(stagingBufferSize));
    if (std::memcmp(mappedData, vertices.data(), static_cast<size_t>(stagingBufferSize)) != 0) {
        throw std::runtime_error("Vertex buffer data mismatch!");
    }
    vkUnmapMemory(device, stagingBufferMemory);
    std::cout << "Copied " << vertices.size() << " vertices (" << stagingBufferSize
              << " bytes) into the staging buffer; data verified.\n";

    VkBufferCreateInfo vertexBufferCreateInfo{};
    vertexBufferCreateInfo.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
    vertexBufferCreateInfo.size = stagingBufferSize;
    vertexBufferCreateInfo.usage =
        VK_BUFFER_USAGE_TRANSFER_SRC_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT | VK_BUFFER_USAGE_VERTEX_BUFFER_BIT;
    vertexBufferCreateInfo.sharingMode = VK_SHARING_MODE_EXCLUSIVE;

    VkBuffer vertexBuffer = VK_NULL_HANDLE;
    VK_CHECK(vkCreateBuffer(device, &vertexBufferCreateInfo, nullptr, &vertexBuffer));

    VkMemoryRequirements vertexBufferMemoryRequirements{};
    vkGetBufferMemoryRequirements(device, vertexBuffer, &vertexBufferMemoryRequirements);

    VkMemoryAllocateInfo vertexMemoryAllocInfo{};
    vertexMemoryAllocInfo.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
    vertexMemoryAllocInfo.allocationSize = vertexBufferMemoryRequirements.size;
    vertexMemoryAllocInfo.memoryTypeIndex = FindMemoryType(
        mainPhysicalDevice, vertexBufferMemoryRequirements.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);

    VkDeviceMemory vertexBufferMemory = VK_NULL_HANDLE;

    VK_CHECK(vkAllocateMemory(device, &vertexMemoryAllocInfo, nullptr, &vertexBufferMemory));

    VK_CHECK(vkBindBufferMemory(device, vertexBuffer, vertexBufferMemory, 0));

    VkBufferCreateInfo readbackBufferCreateInfo{};
    readbackBufferCreateInfo.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
    readbackBufferCreateInfo.size = stagingBufferSize;
    readbackBufferCreateInfo.usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT;
    readbackBufferCreateInfo.sharingMode = VK_SHARING_MODE_EXCLUSIVE;

    VkBuffer readbackBuffer = VK_NULL_HANDLE;
    VK_CHECK(vkCreateBuffer(device, &readbackBufferCreateInfo, nullptr, &readbackBuffer));

    VkMemoryRequirements readbackBufferMemoryRequiments{};
    vkGetBufferMemoryRequirements(device, readbackBuffer, &readbackBufferMemoryRequiments);

    VkMemoryAllocateInfo readbackMemoryAllocInfo{};
    readbackMemoryAllocInfo.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
    readbackMemoryAllocInfo.allocationSize = readbackBufferMemoryRequiments.size;
    readbackMemoryAllocInfo.memoryTypeIndex = FindMemoryType(
        mainPhysicalDevice, readbackBufferMemoryRequiments.memoryTypeBits, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT);

    VkDeviceMemory readbackBufferMemory = VK_NULL_HANDLE;

    VK_CHECK(vkAllocateMemory(device, &readbackMemoryAllocInfo, nullptr, &readbackBufferMemory));

    VK_CHECK(vkBindBufferMemory(device, readbackBuffer, readbackBufferMemory, 0));

    VkQueue copyQueue = VK_NULL_HANDLE;
    vkGetDeviceQueue(device, deviceQueueInfo.queueFamilyIndex, 0, &copyQueue);

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
    commandBufferAllocInfo.commandBufferCount = 2;

    std::array<VkCommandBuffer, 2> commandBuffers{};
    VK_CHECK(vkAllocateCommandBuffers(device, &commandBufferAllocInfo, commandBuffers.data()));

    VkCommandBufferBeginInfo beginInfo{};
    beginInfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;

    beginInfo.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;

    VK_CHECK(vkBeginCommandBuffer(commandBuffers[0], &beginInfo));
    VkBufferCopy region{};
    region.srcOffset = 0;
    region.dstOffset = 0;
    region.size = stagingBufferSize;
    vkCmdCopyBuffer(commandBuffers[0], stagingBuffer, vertexBuffer, 1, &region);

    VK_CHECK(vkEndCommandBuffer(commandBuffers[0]));

    VK_CHECK(vkBeginCommandBuffer(commandBuffers[1], &beginInfo));
    vkCmdCopyBuffer(commandBuffers[1], vertexBuffer, readbackBuffer, 1, &region);

    VkMemoryBarrier2 hostBarrier{};
    hostBarrier.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER_2;
    hostBarrier.srcStageMask = VK_PIPELINE_STAGE_2_COPY_BIT;
    hostBarrier.srcAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT;
    hostBarrier.dstStageMask = VK_PIPELINE_STAGE_2_HOST_BIT;
    hostBarrier.dstAccessMask = VK_ACCESS_2_HOST_READ_BIT;

    VkDependencyInfo hostDependencyInfo{};
    hostDependencyInfo.sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO;
    hostDependencyInfo.memoryBarrierCount = 1;
    hostDependencyInfo.pMemoryBarriers = &hostBarrier;

    vkCmdPipelineBarrier2(commandBuffers[1], &hostDependencyInfo);
    VK_CHECK(vkEndCommandBuffer(commandBuffers[1]));

    VkSemaphoreTypeCreateInfo typesInfo{};
    typesInfo.sType = VK_STRUCTURE_TYPE_SEMAPHORE_TYPE_CREATE_INFO;
    typesInfo.initialValue = 0;
    typesInfo.semaphoreType = VK_SEMAPHORE_TYPE_TIMELINE;

    VkSemaphoreCreateInfo semaphoreInfo{};
    semaphoreInfo.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO;
    semaphoreInfo.pNext = &typesInfo;

    VkSemaphore copySemaphore = VK_NULL_HANDLE;
    VK_CHECK(vkCreateSemaphore(device, &semaphoreInfo, nullptr, &copySemaphore));

    VkSemaphoreSubmitInfo semaphoreSubmitInfoA{};
    semaphoreSubmitInfoA.sType = VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO;
    semaphoreSubmitInfoA.semaphore = copySemaphore;
    semaphoreSubmitInfoA.value = 1;
    semaphoreSubmitInfoA.stageMask = VK_PIPELINE_STAGE_2_COPY_BIT;

    VkCommandBufferSubmitInfo commandBufferSubmitInfo{};
    commandBufferSubmitInfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_SUBMIT_INFO;
    commandBufferSubmitInfo.commandBuffer = commandBuffers[0];

    VkSubmitInfo2 submitInfo{};
    submitInfo.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO_2;

    submitInfo.commandBufferInfoCount = 1;
    submitInfo.pCommandBufferInfos = &commandBufferSubmitInfo;
    submitInfo.signalSemaphoreInfoCount = 1;
    submitInfo.pSignalSemaphoreInfos = &semaphoreSubmitInfoA;

    VkCommandBufferSubmitInfo readbackCommandBufferSubmitInfo{};
    readbackCommandBufferSubmitInfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_SUBMIT_INFO;
    readbackCommandBufferSubmitInfo.commandBuffer = commandBuffers[1];

    VkSemaphoreSubmitInfo waitB{};
    waitB.sType = VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO;
    waitB.semaphore = copySemaphore;
    waitB.value = 1;
    waitB.stageMask = VK_PIPELINE_STAGE_2_COPY_BIT;

    VkSemaphoreSubmitInfo signalB{};
    signalB.sType = VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO;
    signalB.semaphore = copySemaphore;
    signalB.value = 2;
    signalB.stageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;

    VkSubmitInfo2 readbackSubmitInfo{};
    readbackSubmitInfo.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO_2;
    readbackSubmitInfo.waitSemaphoreInfoCount = 1;
    readbackSubmitInfo.pWaitSemaphoreInfos = &waitB;
    readbackSubmitInfo.signalSemaphoreInfoCount = 1;
    readbackSubmitInfo.pSignalSemaphoreInfos = &signalB;
    readbackSubmitInfo.commandBufferInfoCount = 1;
    readbackSubmitInfo.pCommandBufferInfos = &readbackCommandBufferSubmitInfo;

    VK_CHECK(vkQueueSubmit2(copyQueue, 1, &submitInfo, VK_NULL_HANDLE));

    VK_CHECK(vkQueueSubmit2(copyQueue, 1, &readbackSubmitInfo, VK_NULL_HANDLE));

    uint64_t targetValue = 2;

    VkSemaphoreWaitInfo waitInfo{};
    waitInfo.sType = VK_STRUCTURE_TYPE_SEMAPHORE_WAIT_INFO;
    waitInfo.semaphoreCount = 1;
    waitInfo.pSemaphores = &copySemaphore;
    waitInfo.pValues = &targetValue;
    VK_CHECK(vkWaitSemaphores(device, &waitInfo, UINT64_MAX));

    void* readbackData = nullptr;
    VK_CHECK(vkMapMemory(device, readbackBufferMemory, 0, VK_WHOLE_SIZE, 0, &readbackData));

    VkMappedMemoryRange range{};
    range.sType = VK_STRUCTURE_TYPE_MAPPED_MEMORY_RANGE;
    range.memory = readbackBufferMemory;
    range.offset = 0;
    range.size = VK_WHOLE_SIZE;

    VK_CHECK(vkInvalidateMappedMemoryRanges(device, 1, &range));

    if (std::memcmp(readbackData, vertices.data(), static_cast<size_t>(stagingBufferSize)) != 0) {
        throw std::runtime_error("Readback buffer data mismatch!");
    }

    vkUnmapMemory(device, readbackBufferMemory);
    std::cout << "Readback matches the original vertices.\n";
    vkDestroyBuffer(device, stagingBuffer, nullptr);
    vkFreeMemory(device, stagingBufferMemory, nullptr);

    std::cout << "Timeline transfer complete.\n";
    vkDestroyCommandPool(device, commandPool, nullptr);
    vkDestroySemaphore(device, copySemaphore, nullptr);

    vkDestroyBuffer(device, vertexBuffer, nullptr);
    vkFreeMemory(device, vertexBufferMemory, nullptr);

    vkDestroyBuffer(device, readbackBuffer, nullptr);
    vkFreeMemory(device, readbackBufferMemory, nullptr);

    vkDestroyDevice(device, nullptr);

    auto destroyDebugMessenger = reinterpret_cast<PFN_vkDestroyDebugUtilsMessengerEXT>(
        vkGetInstanceProcAddr(instance, "vkDestroyDebugUtilsMessengerEXT"));

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
