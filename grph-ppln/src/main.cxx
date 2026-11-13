#include <vulkan/vulkan.h>
#include <vulkan/vk_enum_string_helper.h>

#include <SDL3/SDL.h>
#include <SDL3/SDL_vulkan.h>

#include <iostream>
#include <cstdlib>
#include <stdexcept>
#include <vector>
#include <optional>
#include <algorithm>
#include <cstdint>
#include <cstring>
#include <array>
#include <filesystem>
#include <fstream>
#include <format>
#include <cmath>
#include <source_location>

VkExtent2D ChooseExtent(const VkSurfaceCapabilitiesKHR& capabilities, uint32_t pixelWidth, uint32_t pixelHeight) {
    if (capabilities.currentExtent.width != UINT32_MAX) {
        return capabilities.currentExtent;
    }

    VkExtent2D extent{pixelWidth, pixelHeight};

    extent.width = std::clamp(extent.width, capabilities.minImageExtent.width, capabilities.maxImageExtent.width);
    extent.height = std::clamp(extent.height, capabilities.minImageExtent.height, capabilities.maxImageExtent.height);

    return extent;
}

static VKAPI_ATTR VkBool32 VKAPI_CALL DebugCallback(VkDebugUtilsMessageSeverityFlagBitsEXT,
                                                    VkDebugUtilsMessageTypeFlagsEXT,
                                                    const VkDebugUtilsMessengerCallbackDataEXT* callbackData, void*) {
    std::cerr << "[Vulkan Validation] " << callbackData->pMessage << '\n';
    return VK_FALSE;
}

inline void CheckVulkanResult(VkResult result, const char* functionName,
                              std::source_location loc = std::source_location::current()) {
    if (result == VK_SUCCESS) {
        return;
    }

    throw std::runtime_error(std::format("{}:{} [{}] {} error {}", loc.file_name(), loc.line(), loc.function_name(),
                                         functionName, string_VkResult(result)));
}

#define VK_CHECK(call) CheckVulkanResult(call, #call)

std::optional<uint32_t> FindQueueFamily(VkPhysicalDevice device, VkSurfaceKHR surface) {
    uint32_t count = 0;
    vkGetPhysicalDeviceQueueFamilyProperties(device, &count, nullptr);
    std::vector<VkQueueFamilyProperties> families(count);
    vkGetPhysicalDeviceQueueFamilyProperties(device, &count, families.data());
    for (uint32_t i = 0; i < count; ++i) {
        if (families[i].queueCount > 0 && (families[i].queueFlags & VK_QUEUE_GRAPHICS_BIT)) {

            VkBool32 present = VK_FALSE;
            VK_CHECK(vkGetPhysicalDeviceSurfaceSupportKHR(device, i, surface, &present));
            if (!present) {
                continue;
            }
            return i;
        }
    }
    return std::nullopt;
}

VkPhysicalDevice FindPhysicalDevice(VkInstance instance, VkSurfaceKHR surface) {
    uint32_t count = 0;
    VK_CHECK(vkEnumeratePhysicalDevices(instance, &count, nullptr));
    std::vector<VkPhysicalDevice> devices(count);
    VK_CHECK(vkEnumeratePhysicalDevices(instance, &count, devices.data()));
    VkPhysicalDevice selected = VK_NULL_HANDLE;
    for (VkPhysicalDevice device : devices) {
        VkPhysicalDeviceProperties properties{};
        vkGetPhysicalDeviceProperties(device, &properties);
        if (properties.apiVersion < VK_API_VERSION_1_3 || !FindQueueFamily(device, surface)) {
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
        if (!(features13.synchronization2 && features13.dynamicRendering)) {
            continue;
        }

        uint32_t extensionCount = 0;
        VK_CHECK(vkEnumerateDeviceExtensionProperties(device, nullptr, &extensionCount, nullptr));
        std::vector<VkExtensionProperties> extensions(extensionCount);
        VK_CHECK(vkEnumerateDeviceExtensionProperties(device, nullptr, &extensionCount, extensions.data()));
        const bool swapchainSupported = std::any_of(extensions.begin(), extensions.end(), [](const auto& extension) {
            return std::strcmp(extension.extensionName, VK_KHR_SWAPCHAIN_EXTENSION_NAME) == 0;
        });
        if (!swapchainSupported) {
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

uint32_t FindMemoryType(VkPhysicalDevice physicalDevice, uint32_t typeBits, VkMemoryPropertyFlags required) {
    VkPhysicalDeviceMemoryProperties props{};
    vkGetPhysicalDeviceMemoryProperties(physicalDevice, &props);

    for (uint32_t i = 0; i < props.memoryTypeCount; ++i) {
        if ((typeBits & (1u << i)) && (props.memoryTypes[i].propertyFlags & required) == required) {
            return i;
        }
    }

    throw std::runtime_error("No suitable memory type");
}

std::vector<uint32_t> ReadShaderFile(const std::filesystem::path& path) {
    std::ifstream file(path, std::ios::binary | std::ios::ate);
    if (!file.is_open()) {
        throw std::runtime_error("Failed to open SPIR-V file");
    }

    size_t fileSize = file.tellg();
    if (fileSize <= 0 || fileSize % sizeof(uint32_t) != 0) {
        throw std::runtime_error("Invalid SPIR-V file size");
    }

    std::vector<uint32_t> code(fileSize / sizeof(uint32_t));
    file.seekg(0, std::ios::beg);
    if (!file.read(reinterpret_cast<char*>(code.data()), static_cast<std::streamsize>(fileSize))) {
        throw std::runtime_error("Failed to read SPIR-V");
    }

    return code;
}

bool RecreateSwapchain(SDL_Window* window, VkPhysicalDevice mainPhysicalDevice, VkDevice device, VkSurfaceKHR surface,
                       VkFormat depthFormat, VkSampleCountFlagBits msaaSamples, VkSwapchainKHR& swapchain,
                       VkExtent2D& extent, VkSurfaceFormatKHR& mainSurfaceFormat, std::vector<VkImage>& swapchainImages,
                       std::vector<VkImageView>& imageViews, std::vector<VkSemaphore>& renderFinished,
                       std::vector<VkImage>& depthFrameImages, std::vector<VkDeviceMemory>& depthFrameMemories,
                       std::vector<VkImageView>& depthFrameViews, std::vector<VkImage>& msaaColorImages,
                       std::vector<VkDeviceMemory>& msaaColorMemories, std::vector<VkImageView>& msaaColorViews) {
    int pixelWidth = 0;
    int pixelHeight = 0;
    if (!SDL_GetWindowSizeInPixels(window, &pixelWidth, &pixelHeight)) {
        throw std::runtime_error(SDL_GetError());
    }
    if (pixelWidth <= 0 || pixelHeight <= 0 || (SDL_GetWindowFlags(window) & SDL_WINDOW_MINIMIZED)) {
        return false;
    }

    VK_CHECK(vkDeviceWaitIdle(device));

    VkSurfaceCapabilitiesKHR capabilities{};
    VK_CHECK(vkGetPhysicalDeviceSurfaceCapabilitiesKHR(mainPhysicalDevice, surface, &capabilities));
    VkExtent2D newExtent =
        ChooseExtent(capabilities, static_cast<uint32_t>(pixelWidth), static_cast<uint32_t>(pixelHeight));
    if (newExtent.width == 0 || newExtent.height == 0) {
        return false;
    }

    uint32_t formatCount = 0;
    VK_CHECK(vkGetPhysicalDeviceSurfaceFormatsKHR(mainPhysicalDevice, surface, &formatCount, nullptr));
    std::vector<VkSurfaceFormatKHR> formats(formatCount);
    VK_CHECK(vkGetPhysicalDeviceSurfaceFormatsKHR(mainPhysicalDevice, surface, &formatCount, formats.data()));

    std::optional<VkSurfaceFormatKHR> newSurfaceFormat;
    for (const auto& format : formats) {
        if (format.format == mainSurfaceFormat.format || format.format == VK_FORMAT_UNDEFINED) {
            newSurfaceFormat = VkSurfaceFormatKHR{mainSurfaceFormat.format, format.colorSpace};
            if (format.colorSpace == mainSurfaceFormat.colorSpace) {
                break;
            }
        }
    }
    if (!newSurfaceFormat) {
        throw std::runtime_error("Surface format is incompatible with the existing graphics pipeline");
    }
    if (!(capabilities.supportedUsageFlags & VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT) ||
        !(capabilities.supportedCompositeAlpha & VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR)) {
        throw std::runtime_error("Surface no longer supports the current swapchain configuration");
    }

    uint32_t imageCount = capabilities.minImageCount + 1;
    if (capabilities.maxImageCount > 0) {
        imageCount = std::min(imageCount, capabilities.maxImageCount);
    }

    VkSwapchainCreateInfoKHR swapchainInfo{};
    swapchainInfo.sType = VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR;
    swapchainInfo.surface = surface;
    swapchainInfo.minImageCount = imageCount;
    swapchainInfo.imageFormat = newSurfaceFormat->format;
    swapchainInfo.imageColorSpace = newSurfaceFormat->colorSpace;
    swapchainInfo.imageExtent = newExtent;
    swapchainInfo.imageArrayLayers = 1;
    swapchainInfo.imageUsage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;
    swapchainInfo.imageSharingMode = VK_SHARING_MODE_EXCLUSIVE;
    swapchainInfo.preTransform = capabilities.currentTransform;
    swapchainInfo.compositeAlpha = VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR;
    swapchainInfo.presentMode = VK_PRESENT_MODE_FIFO_KHR;
    swapchainInfo.clipped = VK_TRUE;
    swapchainInfo.oldSwapchain = swapchain;

    VkSwapchainKHR newSwapchain = VK_NULL_HANDLE;
    VK_CHECK(vkCreateSwapchainKHR(device, &swapchainInfo, nullptr, &newSwapchain));

    for (VkSemaphore semaphore : renderFinished) {
        vkDestroySemaphore(device, semaphore, nullptr);
    }
    for (VkImageView view : imageViews) {
        vkDestroyImageView(device, view, nullptr);
    }
    for (size_t i = 0; i < depthFrameImages.size(); ++i) {
        vkDestroyImageView(device, depthFrameViews[i], nullptr);
        vkDestroyImage(device, depthFrameImages[i], nullptr);
        vkFreeMemory(device, depthFrameMemories[i], nullptr);
    }
    for (size_t i = 0; i < msaaColorImages.size(); ++i) {
        vkDestroyImageView(device, msaaColorViews[i], nullptr);
        vkDestroyImage(device, msaaColorImages[i], nullptr);
        vkFreeMemory(device, msaaColorMemories[i], nullptr);
    }
    vkDestroySwapchainKHR(device, swapchain, nullptr);

    swapchain = newSwapchain;
    extent = newExtent;
    mainSurfaceFormat = newSurfaceFormat.value();

    uint32_t swapchainImageCount = 0;
    VK_CHECK(vkGetSwapchainImagesKHR(device, swapchain, &swapchainImageCount, nullptr));
    swapchainImages.resize(swapchainImageCount);
    VK_CHECK(vkGetSwapchainImagesKHR(device, swapchain, &swapchainImageCount, swapchainImages.data()));
    imageViews.resize(swapchainImageCount);
    renderFinished.resize(swapchainImageCount);

    VkSemaphoreCreateInfo semaphoreInfo{};
    semaphoreInfo.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO;
    for (size_t i = 0; i < swapchainImages.size(); ++i) {
        VkImageViewCreateInfo viewInfo{};
        viewInfo.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
        viewInfo.image = swapchainImages[i];
        viewInfo.viewType = VK_IMAGE_VIEW_TYPE_2D;
        viewInfo.format = mainSurfaceFormat.format;
        viewInfo.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        viewInfo.subresourceRange.levelCount = 1;
        viewInfo.subresourceRange.layerCount = 1;
        VK_CHECK(vkCreateImageView(device, &viewInfo, nullptr, &imageViews[i]));
        VK_CHECK(vkCreateSemaphore(device, &semaphoreInfo, nullptr, &renderFinished[i]));
    }

    auto createAttachment = [&](VkFormat format, VkImageUsageFlags usage, VkImageAspectFlags aspect, VkImage& image,
                                VkDeviceMemory& memory, VkImageView& view) {
        VkImageCreateInfo imageInfo{};
        imageInfo.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
        imageInfo.imageType = VK_IMAGE_TYPE_2D;
        imageInfo.format = format;
        imageInfo.extent = {extent.width, extent.height, 1};
        imageInfo.mipLevels = 1;
        imageInfo.arrayLayers = 1;
        imageInfo.samples = msaaSamples;
        imageInfo.tiling = VK_IMAGE_TILING_OPTIMAL;
        imageInfo.usage = usage;
        imageInfo.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
        imageInfo.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        VK_CHECK(vkCreateImage(device, &imageInfo, nullptr, &image));

        VkMemoryRequirements requirements{};
        vkGetImageMemoryRequirements(device, image, &requirements);
        VkMemoryAllocateInfo allocInfo{};
        allocInfo.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
        allocInfo.allocationSize = requirements.size;
        allocInfo.memoryTypeIndex =
            FindMemoryType(mainPhysicalDevice, requirements.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
        VK_CHECK(vkAllocateMemory(device, &allocInfo, nullptr, &memory));
        VK_CHECK(vkBindImageMemory(device, image, memory, 0));

        VkImageViewCreateInfo viewInfo{};
        viewInfo.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
        viewInfo.image = image;
        viewInfo.viewType = VK_IMAGE_VIEW_TYPE_2D;
        viewInfo.format = format;
        viewInfo.subresourceRange.aspectMask = aspect;
        viewInfo.subresourceRange.levelCount = 1;
        viewInfo.subresourceRange.layerCount = 1;
        VK_CHECK(vkCreateImageView(device, &viewInfo, nullptr, &view));
    };
    for (size_t i = 0; i < depthFrameImages.size(); ++i) {
        createAttachment(depthFormat, VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT, VK_IMAGE_ASPECT_DEPTH_BIT,
                         depthFrameImages[i], depthFrameMemories[i], depthFrameViews[i]);
    }
    for (size_t i = 0; i < msaaColorImages.size(); ++i) {
        createAttachment(mainSurfaceFormat.format, VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT, VK_IMAGE_ASPECT_COLOR_BIT,
                         msaaColorImages[i], msaaColorMemories[i], msaaColorViews[i]);
    }

    std::cout << std::format("Swapchain recreated: {}x{}, images: {}\n", extent.width, extent.height,
                             swapchainImageCount);
    return true;
}

VkPipeline CreateGraphicsPipeline(VkDevice device, VkFormat colorFormat, VkFormat depthFormat,
                                  VkSampleCountFlagBits msaaSamples, VkPipelineLayout& pipelineLayout) {
    std::vector<uint32_t> vertexCode = ReadShaderFile(VERTEX_SHADER_PATH);
    VkShaderModuleCreateInfo vertexModuleInfo{};
    vertexModuleInfo.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;
    vertexModuleInfo.codeSize = vertexCode.size() * sizeof(uint32_t);
    vertexModuleInfo.pCode = vertexCode.data();

    VkShaderModule vertexModule = VK_NULL_HANDLE;
    VK_CHECK(vkCreateShaderModule(device, &vertexModuleInfo, nullptr, &vertexModule));

    std::vector<uint32_t> fragmentCode = ReadShaderFile(FRAGMENT_SHADER_PATH);
    VkShaderModuleCreateInfo fragmentModuleInfo{};
    fragmentModuleInfo.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;
    fragmentModuleInfo.codeSize = fragmentCode.size() * sizeof(uint32_t);
    fragmentModuleInfo.pCode = fragmentCode.data();

    VkShaderModule fragmentModule = VK_NULL_HANDLE;
    VK_CHECK(vkCreateShaderModule(device, &fragmentModuleInfo, nullptr, &fragmentModule));

    VkPipelineShaderStageCreateInfo vertexStage{};
    vertexStage.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    vertexStage.stage = VK_SHADER_STAGE_VERTEX_BIT;
    vertexStage.module = vertexModule;
    vertexStage.pName = "main";

    VkPipelineShaderStageCreateInfo fragmentStage{};
    fragmentStage.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    fragmentStage.stage = VK_SHADER_STAGE_FRAGMENT_BIT;
    fragmentStage.module = fragmentModule;
    fragmentStage.pName = "main";

    std::array<VkPipelineShaderStageCreateInfo, 2> stageInfos = {vertexStage, fragmentStage};

    VkPipelineLayoutCreateInfo pipelineLayoutInfo{};
    pipelineLayoutInfo.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
    pipelineLayoutInfo.setLayoutCount = 0;
    pipelineLayoutInfo.pushConstantRangeCount = 0;

    pipelineLayout = VK_NULL_HANDLE;
    VK_CHECK(vkCreatePipelineLayout(device, &pipelineLayoutInfo, nullptr, &pipelineLayout));

    VkPipelineVertexInputStateCreateInfo vertexInputInfo{};
    vertexInputInfo.sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO;
    vertexInputInfo.vertexBindingDescriptionCount = 0;
    vertexInputInfo.vertexAttributeDescriptionCount = 0;

    VkPipelineInputAssemblyStateCreateInfo inputAssemblyInfo{};
    inputAssemblyInfo.sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO;
    inputAssemblyInfo.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
    inputAssemblyInfo.primitiveRestartEnable = VK_FALSE;

    VkDynamicState dynamicStates[] = {VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR};

    VkPipelineDynamicStateCreateInfo dynamicInfo{};
    dynamicInfo.sType = VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO;
    dynamicInfo.dynamicStateCount = 2;
    dynamicInfo.pDynamicStates = dynamicStates;

    VkPipelineViewportStateCreateInfo viewportInfo{};
    viewportInfo.sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO;
    viewportInfo.viewportCount = 1;
    viewportInfo.scissorCount = 1;
    viewportInfo.pScissors = nullptr;
    viewportInfo.pViewports = nullptr;

    VkPipelineRasterizationStateCreateInfo pipelineRasterizationInfo{};
    pipelineRasterizationInfo.sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO;
    pipelineRasterizationInfo.depthClampEnable = VK_FALSE;
    pipelineRasterizationInfo.rasterizerDiscardEnable = VK_FALSE;
    pipelineRasterizationInfo.polygonMode = VK_POLYGON_MODE_FILL;
    pipelineRasterizationInfo.cullMode = VK_CULL_MODE_NONE;
    pipelineRasterizationInfo.frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE;
    pipelineRasterizationInfo.depthBiasEnable = VK_FALSE;
    pipelineRasterizationInfo.lineWidth = 1.0f;

    VkPipelineMultisampleStateCreateInfo pipelineMultisampleInfo{};
    pipelineMultisampleInfo.sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO;
    pipelineMultisampleInfo.rasterizationSamples = msaaSamples;
    pipelineMultisampleInfo.sampleShadingEnable = VK_FALSE;
    pipelineMultisampleInfo.minSampleShading = 0.0f;
    pipelineMultisampleInfo.pSampleMask = nullptr;
    pipelineMultisampleInfo.alphaToCoverageEnable = VK_FALSE;
    pipelineMultisampleInfo.alphaToOneEnable = VK_FALSE;

    VkPipelineColorBlendAttachmentState blendAttachment{};
    blendAttachment.blendEnable = VK_FALSE;
    blendAttachment.colorWriteMask =
        VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT | VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT;

    VkPipelineColorBlendStateCreateInfo pipelineBlendingInfo{};
    pipelineBlendingInfo.sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO;
    pipelineBlendingInfo.logicOpEnable = VK_FALSE;
    pipelineBlendingInfo.attachmentCount = 1;
    pipelineBlendingInfo.pAttachments = &blendAttachment;

    VkPipelineRenderingCreateInfo pipelineRenderingInfo{};
    pipelineRenderingInfo.sType = VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO;
    pipelineRenderingInfo.viewMask = 0;
    pipelineRenderingInfo.colorAttachmentCount = 1;
    pipelineRenderingInfo.pColorAttachmentFormats = &colorFormat;
    pipelineRenderingInfo.depthAttachmentFormat = depthFormat;
    pipelineRenderingInfo.stencilAttachmentFormat = VK_FORMAT_UNDEFINED;

    VkPipelineDepthStencilStateCreateInfo depthState{};
    depthState.sType = VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO;
    depthState.depthTestEnable = VK_TRUE;
    depthState.depthWriteEnable = VK_TRUE;
    depthState.depthCompareOp = VK_COMPARE_OP_LESS;
    depthState.depthBoundsTestEnable = VK_FALSE;
    depthState.stencilTestEnable = VK_FALSE;

    VkGraphicsPipelineCreateInfo graphicsPipelineInfo{};
    graphicsPipelineInfo.sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO;
    graphicsPipelineInfo.stageCount = static_cast<uint32_t>(stageInfos.size());
    graphicsPipelineInfo.pStages = stageInfos.data();
    graphicsPipelineInfo.pVertexInputState = &vertexInputInfo;
    graphicsPipelineInfo.pInputAssemblyState = &inputAssemblyInfo;
    graphicsPipelineInfo.layout = pipelineLayout;
    graphicsPipelineInfo.pViewportState = &viewportInfo;
    graphicsPipelineInfo.pRasterizationState = &pipelineRasterizationInfo;
    graphicsPipelineInfo.pMultisampleState = &pipelineMultisampleInfo;
    graphicsPipelineInfo.pColorBlendState = &pipelineBlendingInfo;
    graphicsPipelineInfo.pNext = &pipelineRenderingInfo;
    graphicsPipelineInfo.pDynamicState = &dynamicInfo;
    graphicsPipelineInfo.pDepthStencilState = &depthState;

    VkPipeline graphicsPipeline = VK_NULL_HANDLE;
    VK_CHECK(vkCreateGraphicsPipelines(device, nullptr, 1, &graphicsPipelineInfo, nullptr, &graphicsPipeline));
    vkDestroyShaderModule(device, vertexModule, nullptr);
    vkDestroyShaderModule(device, fragmentModule, nullptr);

    return graphicsPipeline;
}

constexpr int WIDTH = 1280;
constexpr int HEIGHT = 720;
constexpr int FramesInFlight = 3;

SDL_Window* CreateWindow() {
    if (!SDL_Init(SDL_INIT_VIDEO)) {
        throw std::runtime_error(SDL_GetError());
    }

    uint64_t windowFlags = SDL_WINDOW_VULKAN | SDL_WINDOW_BORDERLESS | SDL_WINDOW_RESIZABLE | SDL_WINDOW_UTILITY;
    SDL_Window* window = SDL_CreateWindow("Vulkan - depth and MSAA", WIDTH, HEIGHT, windowFlags);

    if (!window) {
        throw std::runtime_error(SDL_GetError());
    }

    return window;
}

void Run() {
    SDL_Window* window = CreateWindow();

    VkApplicationInfo appInfo{};
    appInfo.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO;
    appInfo.pApplicationName = "Screen test";
    appInfo.pEngineName = "No engine";
    appInfo.apiVersion = VK_API_VERSION_1_3;
    appInfo.engineVersion = VK_MAKE_API_VERSION(0, 1, 0, 0);
    appInfo.applicationVersion = VK_MAKE_API_VERSION(0, 1, 0, 0);

    VkDebugUtilsMessengerCreateInfoEXT debugInfo{};
    debugInfo.sType = VK_STRUCTURE_TYPE_DEBUG_UTILS_MESSENGER_CREATE_INFO_EXT;
    debugInfo.messageSeverity =
        VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT | VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT;
    debugInfo.messageType = VK_DEBUG_UTILS_MESSAGE_TYPE_GENERAL_BIT_EXT |
                            VK_DEBUG_UTILS_MESSAGE_TYPE_PERFORMANCE_BIT_EXT |
                            VK_DEBUG_UTILS_MESSAGE_TYPE_VALIDATION_BIT_EXT;
    debugInfo.pfnUserCallback = DebugCallback;

    std::vector<const char*> layers = {"VK_LAYER_KHRONOS_validation"};

    uint32_t sdlExtensionsCount = 0;
    auto sdlExtensions = SDL_Vulkan_GetInstanceExtensions(&sdlExtensionsCount);
    if (!sdlExtensions) {
        throw std::runtime_error(SDL_GetError());
    }

    std::vector<const char*> instanceExtensions(sdlExtensions, sdlExtensions + sdlExtensionsCount);
    instanceExtensions.push_back(VK_EXT_DEBUG_UTILS_EXTENSION_NAME);

    VkInstanceCreateInfo instanceInfo{};
    instanceInfo.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO;
    instanceInfo.pNext = &debugInfo;
    instanceInfo.pApplicationInfo = &appInfo;
    instanceInfo.enabledExtensionCount = instanceExtensions.size();
    instanceInfo.ppEnabledExtensionNames = instanceExtensions.data();
    instanceInfo.enabledLayerCount = layers.size();
    instanceInfo.ppEnabledLayerNames = layers.data();

    VkInstance instance = VK_NULL_HANDLE;
    VK_CHECK(vkCreateInstance(&instanceInfo, nullptr, &instance));

    VkDebugUtilsMessengerEXT debugMessenger = VK_NULL_HANDLE;

    auto createDebugMessenger = reinterpret_cast<PFN_vkCreateDebugUtilsMessengerEXT>(
        vkGetInstanceProcAddr(instance, "vkCreateDebugUtilsMessengerEXT"));

    if (!createDebugMessenger) {
        throw std::runtime_error("vkCreateDebugUtilsMessengerEXT not found");
    }

    VK_CHECK(createDebugMessenger(instance, &debugInfo, nullptr, &debugMessenger));

    VkSurfaceKHR surface = VK_NULL_HANDLE;
    if (!SDL_Vulkan_CreateSurface(window, instance, nullptr, &surface)) {
        throw std::runtime_error(SDL_GetError());
    }

    auto mainPhysicalDevice = FindPhysicalDevice(instance, surface);

    uint32_t familyCount = 0;
    vkGetPhysicalDeviceQueueFamilyProperties(mainPhysicalDevice, &familyCount, nullptr);
    std::vector<VkQueueFamilyProperties> families(familyCount);
    vkGetPhysicalDeviceQueueFamilyProperties(mainPhysicalDevice, &familyCount, families.data());
    size_t graphicsFamilyIndex = families.size();
    for (size_t i = 0; i < families.size(); ++i) {
        if (families[i].queueCount > 0 && (families[i].queueFlags & VK_QUEUE_GRAPHICS_BIT) != 0) {
            VkBool32 canPresent = VK_FALSE;
            VK_CHECK(vkGetPhysicalDeviceSurfaceSupportKHR(mainPhysicalDevice, i, surface, &canPresent));
            if (canPresent == VK_TRUE) {
                graphicsFamilyIndex = i;
                break;
            }
        }
    }
    if (graphicsFamilyIndex == families.size()) {
        throw std::runtime_error("Failed to find graphics and present queue family");
    }

    VkPhysicalDeviceProperties mainPhysicalDeviceProperties{};
    vkGetPhysicalDeviceProperties(mainPhysicalDevice, &mainPhysicalDeviceProperties);
    std::cout << std::format("Selected GPU: {}\n\n", mainPhysicalDeviceProperties.deviceName);

    VkPhysicalDeviceVulkan13Features supported13{};
    supported13.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES;

    VkPhysicalDeviceFeatures2 supportedFeatures{};
    supportedFeatures.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2;
    supportedFeatures.pNext = &supported13;

    vkGetPhysicalDeviceFeatures2(mainPhysicalDevice, &supportedFeatures);
    if (supported13.dynamicRendering != VK_TRUE) {
        throw std::runtime_error("Dynamic Rendering is unsupported");
    }

    if (supported13.synchronization2 != VK_TRUE) {
        throw std::runtime_error("Synchronization2 is unsupported");
    }

    float priority = 1.0f;
    VkDeviceQueueCreateInfo deviceQueueInfo{};
    deviceQueueInfo.sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO;
    deviceQueueInfo.queueCount = 1;
    deviceQueueInfo.pQueuePriorities = &priority;
    deviceQueueInfo.queueFamilyIndex = graphicsFamilyIndex;

    uint32_t countExtensions = 0;
    VK_CHECK(vkEnumerateDeviceExtensionProperties(mainPhysicalDevice, nullptr, &countExtensions, nullptr));
    std::vector<VkExtensionProperties> availableExtensions(countExtensions);
    VK_CHECK(vkEnumerateDeviceExtensionProperties(mainPhysicalDevice, nullptr, &countExtensions,
                                                  availableExtensions.data()));

    {
        bool isOk = false;
        for (auto& ext : availableExtensions) {
            if (std::strcmp(ext.extensionName, VK_KHR_SWAPCHAIN_EXTENSION_NAME) == 0) {
                isOk = true;
            }
        }
        if (!isOk) {
            throw std::runtime_error("VK_KHR_SWAPCHAIN_EXTENSION_NAME not found");
        }
    }

    const char* deviceExtensions[] = {VK_KHR_SWAPCHAIN_EXTENSION_NAME};

    VkPhysicalDeviceVulkan13Features enabled13{};
    enabled13.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES;
    enabled13.dynamicRendering = VK_TRUE;
    enabled13.synchronization2 = VK_TRUE;
    VkDeviceCreateInfo deviceInfo{};
    deviceInfo.sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO;
    deviceInfo.pNext = &enabled13;
    deviceInfo.queueCreateInfoCount = 1;
    deviceInfo.pQueueCreateInfos = &deviceQueueInfo;
    deviceInfo.enabledExtensionCount = 1;
    deviceInfo.ppEnabledExtensionNames = deviceExtensions;

    VkDevice device = VK_NULL_HANDLE;
    VK_CHECK(vkCreateDevice(mainPhysicalDevice, &deviceInfo, nullptr, &device));

    VkSurfaceCapabilitiesKHR surfaceCapabilities{};
    VK_CHECK(vkGetPhysicalDeviceSurfaceCapabilitiesKHR(mainPhysicalDevice, surface, &surfaceCapabilities));
    std::cout << std::format("Surface capabilities:\n"
                             "    Min images: {}\n"
                             "    Max images: {}\n"
                             "    Current extent: {}x{}\n\n",
                             surfaceCapabilities.minImageCount, surfaceCapabilities.maxImageCount,
                             surfaceCapabilities.currentExtent.width, surfaceCapabilities.currentExtent.height);

    uint32_t imageCount = surfaceCapabilities.minImageCount + 1;
    if (surfaceCapabilities.maxImageCount > 0) {
        imageCount = std::min(imageCount, surfaceCapabilities.maxImageCount);
    }

    int pixelWidth = 0;
    int pixelHeight = 0;

    while (pixelWidth <= 0 || pixelHeight <= 0) {
        if (!SDL_GetWindowSizeInPixels(window, &pixelWidth, &pixelHeight)) {
            throw std::runtime_error(SDL_GetError());
        }
    }
    VkExtent2D extent =
        ChooseExtent(surfaceCapabilities, static_cast<uint32_t>(pixelWidth), static_cast<uint32_t>(pixelHeight));

    uint32_t formatCount = 0;
    VK_CHECK(vkGetPhysicalDeviceSurfaceFormatsKHR(mainPhysicalDevice, surface, &formatCount, nullptr));
    std::vector<VkSurfaceFormatKHR> formats(formatCount);
    VK_CHECK(vkGetPhysicalDeviceSurfaceFormatsKHR(mainPhysicalDevice, surface, &formatCount, formats.data()));

    std::optional<VkSurfaceFormatKHR> checkSurfaceFormat;
    for (const auto& format : formats) {
        if (format.format == VK_FORMAT_B8G8R8A8_SRGB && format.colorSpace == VK_COLOR_SPACE_SRGB_NONLINEAR_KHR) {
            checkSurfaceFormat = format;
        }
    }

    if (!checkSurfaceFormat) {
        throw std::runtime_error("SurfaceFormat not found");
    }

    VkSurfaceFormatKHR mainSurfaceFormat = checkSurfaceFormat.value();

    uint32_t modeCount = 0;
    VK_CHECK(vkGetPhysicalDeviceSurfacePresentModesKHR(mainPhysicalDevice, surface, &modeCount, nullptr));
    std::vector<VkPresentModeKHR> modes(modeCount);
    VK_CHECK(vkGetPhysicalDeviceSurfacePresentModesKHR(mainPhysicalDevice, surface, &modeCount, modes.data()));

    {
        bool isOk = false;
        for (auto& m : modes) {
            if (m == VK_PRESENT_MODE_FIFO_KHR) {
                isOk = true;
            }
        }
        if (!isOk) {
            throw std::runtime_error("VK_KHR_SWAPCHAIN_EXTENSION_NAME not found");
        }
    }

    if (!(surfaceCapabilities.supportedUsageFlags & VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT)) {
        throw std::runtime_error("Surface does not support color attachments");
    }

    if (!(surfaceCapabilities.supportedCompositeAlpha & VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR)) {
        throw std::runtime_error("Opaque composite alpha is unsupported");
    }

    VkSwapchainCreateInfoKHR swapchainInfo{};
    swapchainInfo.sType = VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR;
    swapchainInfo.surface = surface;
    swapchainInfo.minImageCount = imageCount;
    swapchainInfo.imageFormat = mainSurfaceFormat.format;
    swapchainInfo.imageColorSpace = mainSurfaceFormat.colorSpace;
    swapchainInfo.imageExtent = extent;
    swapchainInfo.imageArrayLayers = 1;
    swapchainInfo.imageUsage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;
    swapchainInfo.imageSharingMode = VK_SHARING_MODE_EXCLUSIVE;
    swapchainInfo.preTransform = surfaceCapabilities.currentTransform;
    swapchainInfo.compositeAlpha = VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR;
    swapchainInfo.presentMode = VK_PRESENT_MODE_FIFO_KHR;
    swapchainInfo.clipped = VK_TRUE;
    swapchainInfo.oldSwapchain = VK_NULL_HANDLE;

    VkSwapchainKHR swapchain = VK_NULL_HANDLE;
    VK_CHECK(vkCreateSwapchainKHR(device, &swapchainInfo, nullptr, &swapchain));

    uint32_t swapchainImageCount = 0;
    VK_CHECK(vkGetSwapchainImagesKHR(device, swapchain, &swapchainImageCount, nullptr));
    std::vector<VkImage> swapchainImages(swapchainImageCount);
    VK_CHECK(vkGetSwapchainImagesKHR(device, swapchain, &swapchainImageCount, swapchainImages.data()));
    std::cout << std::format("Swapchain:\n"
                             "    Format: {}\n"
                             "    Color space: {}\n"
                             "    Present mode: {}\n"
                             "    Image extent: {}x{}\n"
                             "    Requested image count: {}\n"
                             "    Actual image count: {}\n\n",
                             string_VkFormat(swapchainInfo.imageFormat),
                             string_VkColorSpaceKHR(swapchainInfo.imageColorSpace),
                             string_VkPresentModeKHR(swapchainInfo.presentMode), swapchainInfo.imageExtent.width,
                             swapchainInfo.imageExtent.height, swapchainInfo.minImageCount, swapchainImageCount);

    VkFormat depthFormat = VK_FORMAT_D32_SFLOAT;

    {
        VkFormatProperties props{};
        vkGetPhysicalDeviceFormatProperties(mainPhysicalDevice, depthFormat, &props);
        if (!(props.optimalTilingFeatures & VK_FORMAT_FEATURE_DEPTH_STENCIL_ATTACHMENT_BIT)) {
            throw std::runtime_error("Selected depth format is unsupported");
        }
    }

    VkImageFormatProperties colorImageProperties{};
    VK_CHECK(vkGetPhysicalDeviceImageFormatProperties(mainPhysicalDevice, mainSurfaceFormat.format, VK_IMAGE_TYPE_2D,
                                                      VK_IMAGE_TILING_OPTIMAL, VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT, 0,
                                                      &colorImageProperties));

    VkImageFormatProperties depthImageProperties{};
    VK_CHECK(vkGetPhysicalDeviceImageFormatProperties(
        mainPhysicalDevice, depthFormat, VK_IMAGE_TYPE_2D, VK_IMAGE_TILING_OPTIMAL,
        VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT, 0, &depthImageProperties));

    VkSampleCountFlags supportedSamples = mainPhysicalDeviceProperties.limits.framebufferColorSampleCounts &
                                          mainPhysicalDeviceProperties.limits.framebufferDepthSampleCounts &
                                          colorImageProperties.sampleCounts & depthImageProperties.sampleCounts;
    VkSampleCountFlagBits msaaSamples = VK_SAMPLE_COUNT_1_BIT;
    if (supportedSamples & VK_SAMPLE_COUNT_4_BIT) {
        msaaSamples = VK_SAMPLE_COUNT_4_BIT;
    } else if (supportedSamples & VK_SAMPLE_COUNT_2_BIT) {
        msaaSamples = VK_SAMPLE_COUNT_2_BIT;
    }

    std::cout << std::format("MSAA samples: {}\n", static_cast<uint32_t>(msaaSamples));

    std::vector<VkImageCreateInfo> depthFrameInfos(FramesInFlight);
    for (VkImageCreateInfo& depthImageInfo : depthFrameInfos) {
        depthImageInfo.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
        depthImageInfo.imageType = VK_IMAGE_TYPE_2D;
        depthImageInfo.tiling = VK_IMAGE_TILING_OPTIMAL;
        depthImageInfo.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        depthImageInfo.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
        depthImageInfo.usage = VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT;
        depthImageInfo.samples = msaaSamples;
        depthImageInfo.format = depthFormat;
        depthImageInfo.extent = {extent.width, extent.height, 1};
        depthImageInfo.arrayLayers = 1;
        depthImageInfo.mipLevels = 1;
    }

    std::vector<VkImage> depthFrameImages(FramesInFlight, VK_NULL_HANDLE);
    for (size_t i = 0; i < FramesInFlight; ++i) {
        VK_CHECK(vkCreateImage(device, &depthFrameInfos[i], nullptr, &depthFrameImages[i]));
    }

    std::vector<VkDeviceMemory> depthFrameMemories(FramesInFlight, VK_NULL_HANDLE);
    for (size_t i = 0; i < FramesInFlight; ++i) {
        VkMemoryRequirements requirements{};
        vkGetImageMemoryRequirements(device, depthFrameImages[i], &requirements);

        VkMemoryAllocateInfo depthAllocInfo{};
        depthAllocInfo.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
        depthAllocInfo.allocationSize = requirements.size;
        depthAllocInfo.memoryTypeIndex =
            FindMemoryType(mainPhysicalDevice, requirements.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
        VK_CHECK(vkAllocateMemory(device, &depthAllocInfo, nullptr, &depthFrameMemories[i]));
        VK_CHECK(vkBindImageMemory(device, depthFrameImages[i], depthFrameMemories[i], 0));
    }

    std::vector<VkImageView> depthFrameViews(FramesInFlight, VK_NULL_HANDLE);
    for (size_t i = 0; i < FramesInFlight; ++i) {
        VkImageViewCreateInfo viewInfo{};
        viewInfo.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
        viewInfo.image = depthFrameImages[i];
        viewInfo.viewType = VK_IMAGE_VIEW_TYPE_2D;
        viewInfo.format = depthFormat;
        viewInfo.subresourceRange.aspectMask = VK_IMAGE_ASPECT_DEPTH_BIT;
        viewInfo.subresourceRange.baseMipLevel = 0;
        viewInfo.subresourceRange.levelCount = 1;
        viewInfo.subresourceRange.baseArrayLayer = 0;
        viewInfo.subresourceRange.layerCount = 1;

        VK_CHECK(vkCreateImageView(device, &viewInfo, nullptr, &depthFrameViews[i]));
    }

    std::vector<VkImage> msaaColorImages(msaaSamples == VK_SAMPLE_COUNT_1_BIT ? 0 : FramesInFlight, VK_NULL_HANDLE);
    std::vector<VkDeviceMemory> msaaColorMemories(msaaColorImages.size(), VK_NULL_HANDLE);
    std::vector<VkImageView> msaaColorViews(msaaColorImages.size(), VK_NULL_HANDLE);
    for (size_t i = 0; i < msaaColorImages.size(); ++i) {
        VkImageCreateInfo msaaImageInfo{};
        msaaImageInfo.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
        msaaImageInfo.imageType = VK_IMAGE_TYPE_2D;
        msaaImageInfo.format = mainSurfaceFormat.format;
        msaaImageInfo.extent = {extent.width, extent.height, 1};
        msaaImageInfo.mipLevels = 1;
        msaaImageInfo.arrayLayers = 1;
        msaaImageInfo.samples = msaaSamples;
        msaaImageInfo.tiling = VK_IMAGE_TILING_OPTIMAL;
        msaaImageInfo.usage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;
        msaaImageInfo.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
        msaaImageInfo.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        VK_CHECK(vkCreateImage(device, &msaaImageInfo, nullptr, &msaaColorImages[i]));

        VkMemoryRequirements requirements{};
        vkGetImageMemoryRequirements(device, msaaColorImages[i], &requirements);

        VkMemoryAllocateInfo msaaAllocInfo{};
        msaaAllocInfo.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
        msaaAllocInfo.allocationSize = requirements.size;
        msaaAllocInfo.memoryTypeIndex =
            FindMemoryType(mainPhysicalDevice, requirements.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
        VK_CHECK(vkAllocateMemory(device, &msaaAllocInfo, nullptr, &msaaColorMemories[i]));
        VK_CHECK(vkBindImageMemory(device, msaaColorImages[i], msaaColorMemories[i], 0));

        VkImageViewCreateInfo viewInfo{};
        viewInfo.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
        viewInfo.image = msaaColorImages[i];
        viewInfo.viewType = VK_IMAGE_VIEW_TYPE_2D;
        viewInfo.format = mainSurfaceFormat.format;
        viewInfo.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        viewInfo.subresourceRange.levelCount = 1;
        viewInfo.subresourceRange.layerCount = 1;
        VK_CHECK(vkCreateImageView(device, &viewInfo, nullptr, &msaaColorViews[i]));
    }

    std::vector<VkImageView> imageViews;
    for (VkImage image : swapchainImages) {
        VkImageViewCreateInfo viewInfo{};
        viewInfo.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
        viewInfo.image = image;
        viewInfo.viewType = VK_IMAGE_VIEW_TYPE_2D;
        viewInfo.format = mainSurfaceFormat.format;
        viewInfo.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        viewInfo.subresourceRange.baseMipLevel = 0;
        viewInfo.subresourceRange.levelCount = 1;
        viewInfo.subresourceRange.baseArrayLayer = 0;
        viewInfo.subresourceRange.layerCount = 1;

        VkImageView view = VK_NULL_HANDLE;
        VK_CHECK(vkCreateImageView(device, &viewInfo, nullptr, &view));
        imageViews.push_back(view);
    }

    std::cout << std::format("Image views created: {}\n", imageViews.size());

    VkPipelineLayout pipelineLayout = VK_NULL_HANDLE;
    VkPipeline graphicsPipeline =
        CreateGraphicsPipeline(device, mainSurfaceFormat.format, depthFormat, msaaSamples, pipelineLayout);

    VkQueue graphicsQueue = VK_NULL_HANDLE;
    vkGetDeviceQueue(device, graphicsFamilyIndex, 0, &graphicsQueue);

    VkCommandPoolCreateInfo commandPoolInfo{};
    commandPoolInfo.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
    commandPoolInfo.queueFamilyIndex = static_cast<uint32_t>(graphicsFamilyIndex);
    commandPoolInfo.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;

    VkCommandPool commandPool = VK_NULL_HANDLE;
    VK_CHECK(vkCreateCommandPool(device, &commandPoolInfo, nullptr, &commandPool));

    VkCommandBufferAllocateInfo commandBufferAllocInfo{};
    commandBufferAllocInfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
    commandBufferAllocInfo.commandPool = commandPool;
    commandBufferAllocInfo.commandBufferCount = FramesInFlight;
    commandBufferAllocInfo.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;

    std::vector<VkCommandBuffer> commandBuffers(FramesInFlight, VK_NULL_HANDLE);
    VK_CHECK(vkAllocateCommandBuffers(device, &commandBufferAllocInfo, commandBuffers.data()));

    VkViewport viewport{};
    viewport.x = 0.0f;
    viewport.y = 0.0f;
    viewport.width = static_cast<float>(extent.width);
    viewport.height = static_cast<float>(extent.height);
    viewport.minDepth = 0.0f;
    viewport.maxDepth = 1.0f;

    VkRect2D scissor{};
    scissor.offset = {0, 0};
    scissor.extent = extent;

    VkCommandBufferBeginInfo commandBufferBeginInfo{};
    commandBufferBeginInfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;

    VkSemaphoreCreateInfo semaphoreInfo{};
    semaphoreInfo.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO;

    std::vector<VkSemaphore> imageAvailable(FramesInFlight, VK_NULL_HANDLE);
    for (size_t i = 0; i < FramesInFlight; ++i) {
        VK_CHECK(vkCreateSemaphore(device, &semaphoreInfo, nullptr, &imageAvailable[i]));
    }

    std::vector<VkSemaphore> renderFinished(swapchainImageCount);
    for (auto& semaphore : renderFinished) {
        VK_CHECK(vkCreateSemaphore(device, &semaphoreInfo, nullptr, &semaphore));
    }

    VkFenceCreateInfo fenceInfo{};
    fenceInfo.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;
    fenceInfo.flags = VK_FENCE_CREATE_SIGNALED_BIT;

    std::vector<VkFence> frameFences(FramesInFlight, VK_NULL_HANDLE);
    for (size_t i = 0; i < FramesInFlight; ++i) {
        VK_CHECK(vkCreateFence(device, &fenceInfo, nullptr, &frameFences[i]));
    }

    bool swapchainNeedsRecreation = false;
    uint32_t imageIndex = 0;
    uint32_t frameIndex = 0;
    bool running = true;
    while (running) {
        SDL_Event event;

        while (SDL_PollEvent(&event)) {
            if (event.type == SDL_EVENT_QUIT) {
                running = false;
            }
            if (event.type == SDL_EVENT_WINDOW_PIXEL_SIZE_CHANGED || event.type == SDL_EVENT_WINDOW_MINIMIZED ||
                event.type == SDL_EVENT_WINDOW_RESTORED) {
                swapchainNeedsRecreation = true;
            }
        }

        if (!running) {
            break;
        }
        if (swapchainNeedsRecreation) {
            if (!RecreateSwapchain(window, mainPhysicalDevice, device, surface, depthFormat, msaaSamples, swapchain,
                                   extent, mainSurfaceFormat, swapchainImages, imageViews, renderFinished,
                                   depthFrameImages, depthFrameMemories, depthFrameViews, msaaColorImages,
                                   msaaColorMemories, msaaColorViews)) {
                SDL_Delay(10);
                continue;
            }
            swapchainImageCount = static_cast<uint32_t>(swapchainImages.size());
            viewport.width = static_cast<float>(extent.width);
            viewport.height = static_cast<float>(extent.height);
            scissor.extent = extent;
            swapchainNeedsRecreation = false;
        }
        frameIndex = (frameIndex + 1) % FramesInFlight;
        VK_CHECK(vkWaitForFences(device, 1, &frameFences[frameIndex], VK_TRUE, UINT64_MAX));
        VkResult acquireResult = vkAcquireNextImageKHR(device, swapchain, UINT64_MAX, imageAvailable[frameIndex],
                                                       VK_NULL_HANDLE, &imageIndex);
        if (acquireResult == VK_ERROR_OUT_OF_DATE_KHR) {
            swapchainNeedsRecreation = true;
            continue;
        }
        if (acquireResult != VK_SUBOPTIMAL_KHR) {
            VK_CHECK(acquireResult);
        }

        VK_CHECK(vkResetCommandBuffer(commandBuffers[frameIndex], 0));

        VkRenderingAttachmentInfo colorAttachment{};
        colorAttachment.sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO;
        colorAttachment.imageView = imageViews[imageIndex];
        colorAttachment.imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
        colorAttachment.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
        colorAttachment.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
        if (msaaSamples != VK_SAMPLE_COUNT_1_BIT) {
            colorAttachment.imageView = msaaColorViews[frameIndex];
            colorAttachment.resolveMode = VK_RESOLVE_MODE_AVERAGE_BIT;
            colorAttachment.resolveImageView = imageViews[imageIndex];
            colorAttachment.resolveImageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
            colorAttachment.storeOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
        }
        colorAttachment.clearValue.color = {{0.03f, 0.04f, 0.07f, 1.0f}};

        VkRenderingAttachmentInfo depthAttachment{};
        depthAttachment.sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO;
        depthAttachment.imageView = depthFrameViews[frameIndex];
        depthAttachment.imageLayout = VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL;
        depthAttachment.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
        depthAttachment.storeOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
        depthAttachment.clearValue.depthStencil = {1.0f, 0};

        VkRenderingInfo renderingInfo{};
        renderingInfo.sType = VK_STRUCTURE_TYPE_RENDERING_INFO;
        renderingInfo.renderArea.offset = {0, 0};
        renderingInfo.renderArea.extent = extent;
        renderingInfo.layerCount = 1;
        renderingInfo.viewMask = 0;
        renderingInfo.colorAttachmentCount = 1;
        renderingInfo.pColorAttachments = &colorAttachment;
        renderingInfo.pDepthAttachment = &depthAttachment;
        renderingInfo.pStencilAttachment = nullptr;

        VK_CHECK(vkBeginCommandBuffer(commandBuffers[frameIndex], &commandBufferBeginInfo));

        VkImageMemoryBarrier2 depthBarrier{};
        depthBarrier.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2;
        depthBarrier.srcStageMask = VK_PIPELINE_STAGE_2_NONE;
        depthBarrier.srcAccessMask = VK_ACCESS_2_NONE;
        depthBarrier.dstStageMask =
            VK_PIPELINE_STAGE_2_EARLY_FRAGMENT_TESTS_BIT | VK_PIPELINE_STAGE_2_LATE_FRAGMENT_TESTS_BIT;
        depthBarrier.dstAccessMask =
            VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_READ_BIT | VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;
        depthBarrier.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        depthBarrier.newLayout = VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL;
        depthBarrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        depthBarrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        depthBarrier.image = depthFrameImages[frameIndex];
        depthBarrier.subresourceRange.aspectMask = VK_IMAGE_ASPECT_DEPTH_BIT;
        depthBarrier.subresourceRange.levelCount = 1;
        depthBarrier.subresourceRange.layerCount = 1;

        VkImageMemoryBarrier2 imageBarrier{};
        imageBarrier.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2;
        imageBarrier.srcStageMask = VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT;
        imageBarrier.dstStageMask = VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT;
        imageBarrier.dstAccessMask = VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT;
        imageBarrier.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        imageBarrier.newLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
        imageBarrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        imageBarrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        imageBarrier.image = swapchainImages[imageIndex];
        imageBarrier.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        imageBarrier.subresourceRange.levelCount = 1;
        imageBarrier.subresourceRange.layerCount = 1;

        VkDependencyInfo dependencyInfo{};
        dependencyInfo.sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO;
        dependencyInfo.imageMemoryBarrierCount = 1;
        dependencyInfo.pImageMemoryBarriers = &depthBarrier;
        vkCmdPipelineBarrier2(commandBuffers[frameIndex], &dependencyInfo);

        dependencyInfo.pImageMemoryBarriers = &imageBarrier;
        vkCmdPipelineBarrier2(commandBuffers[frameIndex], &dependencyInfo);

        VkImageMemoryBarrier2 msaaBarrier = imageBarrier;
        if (msaaSamples != VK_SAMPLE_COUNT_1_BIT) {
            msaaBarrier.srcStageMask = VK_PIPELINE_STAGE_2_NONE;
            msaaBarrier.image = msaaColorImages[frameIndex];
            dependencyInfo.pImageMemoryBarriers = &msaaBarrier;
            vkCmdPipelineBarrier2(commandBuffers[frameIndex], &dependencyInfo);
        }

        vkCmdBeginRendering(commandBuffers[frameIndex], &renderingInfo);
        vkCmdBindPipeline(commandBuffers[frameIndex], VK_PIPELINE_BIND_POINT_GRAPHICS, graphicsPipeline);
        vkCmdSetViewport(commandBuffers[frameIndex], 0, 1, &viewport);
        vkCmdSetScissor(commandBuffers[frameIndex], 0, 1, &scissor);

        vkCmdDraw(commandBuffers[frameIndex], 3, 2, 0, 0);
        vkCmdEndRendering(commandBuffers[frameIndex]);

        imageBarrier.srcAccessMask = VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT;
        imageBarrier.dstStageMask = VK_PIPELINE_STAGE_2_NONE;
        imageBarrier.dstAccessMask = VK_ACCESS_2_NONE;
        imageBarrier.oldLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
        imageBarrier.newLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;
        dependencyInfo.pImageMemoryBarriers = &imageBarrier;
        vkCmdPipelineBarrier2(commandBuffers[frameIndex], &dependencyInfo);
        VK_CHECK(vkEndCommandBuffer(commandBuffers[frameIndex]));

        VkSemaphoreSubmitInfo waitInfo{};
        waitInfo.sType = VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO;
        waitInfo.semaphore = imageAvailable[frameIndex];
        waitInfo.stageMask = VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT;

        VkSemaphoreSubmitInfo signalInfo{};
        signalInfo.sType = VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO;
        signalInfo.semaphore = renderFinished[imageIndex];
        signalInfo.stageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;

        VkCommandBufferSubmitInfo commandSubmitInfo{};
        commandSubmitInfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_SUBMIT_INFO;
        commandSubmitInfo.commandBuffer = commandBuffers[frameIndex];

        VkSubmitInfo2 submitInfo{};
        submitInfo.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO_2;
        submitInfo.waitSemaphoreInfoCount = 1;
        submitInfo.pWaitSemaphoreInfos = &waitInfo;
        submitInfo.commandBufferInfoCount = 1;
        submitInfo.pCommandBufferInfos = &commandSubmitInfo;
        submitInfo.signalSemaphoreInfoCount = 1;
        submitInfo.pSignalSemaphoreInfos = &signalInfo;
        VK_CHECK(vkResetFences(device, 1, &frameFences[frameIndex]));
        VK_CHECK(vkQueueSubmit2(graphicsQueue, 1, &submitInfo, frameFences[frameIndex]));

        VkPresentInfoKHR presentInfo{};
        presentInfo.sType = VK_STRUCTURE_TYPE_PRESENT_INFO_KHR;
        presentInfo.waitSemaphoreCount = 1;
        presentInfo.pWaitSemaphores = &renderFinished[imageIndex];
        presentInfo.swapchainCount = 1;
        presentInfo.pSwapchains = &swapchain;
        presentInfo.pImageIndices = &imageIndex;

        VkResult presentResult = vkQueuePresentKHR(graphicsQueue, &presentInfo);
        if (presentResult == VK_ERROR_OUT_OF_DATE_KHR || presentResult == VK_SUBOPTIMAL_KHR) {
            swapchainNeedsRecreation = true;
        } else {
            VK_CHECK(presentResult);
        }
        swapchainNeedsRecreation = swapchainNeedsRecreation || acquireResult == VK_SUBOPTIMAL_KHR;
        SDL_Delay(1);
    }

    VK_CHECK(vkDeviceWaitIdle(device));
    for (VkFence fence : frameFences) {
        vkDestroyFence(device, fence, nullptr);
    }
    for (VkSemaphore semaphore : imageAvailable) {
        vkDestroySemaphore(device, semaphore, nullptr);
    }
    for (VkSemaphore semaphore : renderFinished) {
        vkDestroySemaphore(device, semaphore, nullptr);
    }
    vkDestroyCommandPool(device, commandPool, nullptr);
    vkDestroyPipeline(device, graphicsPipeline, nullptr);
    vkDestroyPipelineLayout(device, pipelineLayout, nullptr);

    for (VkImageView view : imageViews) {
        vkDestroyImageView(device, view, nullptr);
    }

    for (size_t i = 0; i < FramesInFlight; ++i) {
        vkDestroyImageView(device, depthFrameViews[i], nullptr);
        vkDestroyImage(device, depthFrameImages[i], nullptr);
        vkFreeMemory(device, depthFrameMemories[i], nullptr);
    }

    for (size_t i = 0; i < msaaColorImages.size(); ++i) {
        vkDestroyImageView(device, msaaColorViews[i], nullptr);
        vkDestroyImage(device, msaaColorImages[i], nullptr);
        vkFreeMemory(device, msaaColorMemories[i], nullptr);
    }

    vkDestroySwapchainKHR(device, swapchain, nullptr);
    vkDestroyDevice(device, nullptr);
    vkDestroySurfaceKHR(instance, surface, nullptr);

    auto destroyDebugMessenger = reinterpret_cast<PFN_vkDestroyDebugUtilsMessengerEXT>(
        vkGetInstanceProcAddr(instance, "vkDestroyDebugUtilsMessengerEXT"));

    if (destroyDebugMessenger) {
        destroyDebugMessenger(instance, debugMessenger, nullptr);
    }

    vkDestroyInstance(instance, nullptr);
    SDL_DestroyWindow(window);
    SDL_Quit();
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
