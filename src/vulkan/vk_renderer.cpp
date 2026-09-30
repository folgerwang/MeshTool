#include "vk_renderer.h"

#define GLFW_INCLUDE_VULKAN
#include <GLFW/glfw3.h>

#include <stdexcept>
#include <set>
#include <algorithm>
#include <cstring>

#ifdef _DEBUG
static const bool kEnableValidationLayers = true;
#else
static const bool kEnableValidationLayers = false;
#endif

static const char* kValidationLayers[] = {
    "VK_LAYER_KHRONOS_validation"
};

static const char* kDeviceExtensions[] = {
    VK_KHR_SWAPCHAIN_EXTENSION_NAME
};

// ---------- Debug messenger callback ----------
static VKAPI_ATTR VkBool32 VKAPI_CALL DebugCallback(
    VkDebugUtilsMessageSeverityFlagBitsEXT messageSeverity,
    VkDebugUtilsMessageTypeFlagsEXT messageType,
    const VkDebugUtilsMessengerCallbackDataEXT* pCallbackData,
    void* pUserData)
{
    if (messageSeverity >= VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT)
    {
        fprintf(stderr, "Vulkan validation: %s\n", pCallbackData->pMessage);
    }
    return VK_FALSE;
}

// ---------- VulkanRenderer ----------

VulkanRenderer::VulkanRenderer() = default;

VulkanRenderer::~VulkanRenderer()
{
    Shutdown();
}

bool VulkanRenderer::Init(GLFWwindow* window)
{
    m_window = window;

    try
    {
        CreateInstance();
        CreateSurface(window);
        PickPhysicalDevice();
        CreateLogicalDevice();
        CreateSwapchain();
        CreateImageViews();
        CreateRenderPass();
        CreateDepthResources();
        CreateFramebuffers();
        CreateCommandPool();
        CreateCommandBuffers();
        CreateDescriptorPool();
        CreateSyncObjects();
    }
    catch (const std::exception& e)
    {
        fprintf(stderr, "VulkanRenderer::Init failed: %s\n", e.what());
        return false;
    }

    return true;
}

void VulkanRenderer::Shutdown()
{
    if (m_ctx.device == VK_NULL_HANDLE)
        return;

    vkDeviceWaitIdle(m_ctx.device);

    CleanupSwapchain();

    for (size_t i = 0; i < VulkanContext::MAX_FRAMES_IN_FLIGHT; i++)
    {
        vkDestroySemaphore(m_ctx.device, m_ctx.imageAvailableSemaphores[i], nullptr);
        vkDestroySemaphore(m_ctx.device, m_ctx.renderFinishedSemaphores[i], nullptr);
        vkDestroyFence(m_ctx.device, m_ctx.inFlightFences[i], nullptr);
    }

    if (m_ctx.descriptorPool != VK_NULL_HANDLE)
        vkDestroyDescriptorPool(m_ctx.device, m_ctx.descriptorPool, nullptr);

    if (m_ctx.commandPool != VK_NULL_HANDLE)
        vkDestroyCommandPool(m_ctx.device, m_ctx.commandPool, nullptr);

    vkDestroyDevice(m_ctx.device, nullptr);
    m_ctx.device = VK_NULL_HANDLE;

    if (m_ctx.surface != VK_NULL_HANDLE)
        vkDestroySurfaceKHR(m_ctx.instance, m_ctx.surface, nullptr);

    vkDestroyInstance(m_ctx.instance, nullptr);
    m_ctx.instance = VK_NULL_HANDLE;
}

// ---------- Frame management ----------

bool VulkanRenderer::BeginFrame()
{
    vkWaitForFences(m_ctx.device, 1, &m_ctx.inFlightFences[m_ctx.currentFrame],
                    VK_TRUE, UINT64_MAX);

    VkResult result = vkAcquireNextImageKHR(
        m_ctx.device, m_ctx.swapchain, UINT64_MAX,
        m_ctx.imageAvailableSemaphores[m_ctx.currentFrame],
        VK_NULL_HANDLE, &m_ctx.imageIndex);

    if (result == VK_ERROR_OUT_OF_DATE_KHR)
    {
        int w, h;
        glfwGetFramebufferSize(m_window, &w, &h);
        RecreateSwapchain(w, h);
        return false;
    }
    if (result != VK_SUCCESS && result != VK_SUBOPTIMAL_KHR)
    {
        throw std::runtime_error("failed to acquire swapchain image");
    }

    vkResetFences(m_ctx.device, 1, &m_ctx.inFlightFences[m_ctx.currentFrame]);

    VkCommandBuffer cmd = m_ctx.commandBuffers[m_ctx.currentFrame];
    vkResetCommandBuffer(cmd, 0);

    VkCommandBufferBeginInfo beginInfo{};
    beginInfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
    vkBeginCommandBuffer(cmd, &beginInfo);

    VkRenderPassBeginInfo rpInfo{};
    rpInfo.sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO;
    rpInfo.renderPass = m_ctx.renderPass;
    rpInfo.framebuffer = m_ctx.framebuffers[m_ctx.imageIndex];
    rpInfo.renderArea.offset = {0, 0};
    rpInfo.renderArea.extent = m_ctx.swapchainExtent;

    VkClearValue clearValues[2];
    clearValues[0].color = {{0.04f, 0.04f, 0.06f, 1.0f}};
    clearValues[1].depthStencil = {1.0f, 0};
    rpInfo.clearValueCount = 2;
    rpInfo.pClearValues = clearValues;

    vkCmdBeginRenderPass(cmd, &rpInfo, VK_SUBPASS_CONTENTS_INLINE);

    VkViewport viewport{};
    viewport.x = 0.0f;
    viewport.y = 0.0f;
    viewport.width = static_cast<float>(m_ctx.swapchainExtent.width);
    viewport.height = static_cast<float>(m_ctx.swapchainExtent.height);
    viewport.minDepth = 0.0f;
    viewport.maxDepth = 1.0f;
    vkCmdSetViewport(cmd, 0, 1, &viewport);

    VkRect2D scissor{};
    scissor.offset = {0, 0};
    scissor.extent = m_ctx.swapchainExtent;
    vkCmdSetScissor(cmd, 0, 1, &scissor);

    return true;
}

void VulkanRenderer::EndFrame()
{
    VkCommandBuffer cmd = m_ctx.commandBuffers[m_ctx.currentFrame];

    vkCmdEndRenderPass(cmd);
    vkEndCommandBuffer(cmd);

    VkSubmitInfo submitInfo{};
    submitInfo.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;

    VkSemaphore waitSemaphores[] = {m_ctx.imageAvailableSemaphores[m_ctx.currentFrame]};
    VkPipelineStageFlags waitStages[] = {VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT};
    submitInfo.waitSemaphoreCount = 1;
    submitInfo.pWaitSemaphores = waitSemaphores;
    submitInfo.pWaitDstStageMask = waitStages;
    submitInfo.commandBufferCount = 1;
    submitInfo.pCommandBuffers = &cmd;

    VkSemaphore signalSemaphores[] = {m_ctx.renderFinishedSemaphores[m_ctx.currentFrame]};
    submitInfo.signalSemaphoreCount = 1;
    submitInfo.pSignalSemaphores = signalSemaphores;

    if (vkQueueSubmit(m_ctx.graphicsQueue, 1, &submitInfo,
                      m_ctx.inFlightFences[m_ctx.currentFrame]) != VK_SUCCESS)
    {
        throw std::runtime_error("failed to submit draw command buffer");
    }

    VkPresentInfoKHR presentInfo{};
    presentInfo.sType = VK_STRUCTURE_TYPE_PRESENT_INFO_KHR;
    presentInfo.waitSemaphoreCount = 1;
    presentInfo.pWaitSemaphores = signalSemaphores;
    presentInfo.swapchainCount = 1;
    presentInfo.pSwapchains = &m_ctx.swapchain;
    presentInfo.pImageIndices = &m_ctx.imageIndex;

    VkResult result = vkQueuePresentKHR(m_ctx.presentQueue, &presentInfo);

    if (result == VK_ERROR_OUT_OF_DATE_KHR || result == VK_SUBOPTIMAL_KHR || m_framebufferResized)
    {
        m_framebufferResized = false;
        int w, h;
        glfwGetFramebufferSize(m_window, &w, &h);
        RecreateSwapchain(w, h);
    }
    else if (result != VK_SUCCESS)
    {
        throw std::runtime_error("failed to present swapchain image");
    }

    m_ctx.currentFrame = (m_ctx.currentFrame + 1) % VulkanContext::MAX_FRAMES_IN_FLIGHT;
}

void VulkanRenderer::RecreateSwapchain(int width, int height)
{
    // Handle minimization
    while (width == 0 || height == 0)
    {
        glfwGetFramebufferSize(m_window, &width, &height);
        glfwWaitEvents();
    }

    vkDeviceWaitIdle(m_ctx.device);

    CleanupSwapchain();

    CreateSwapchain();
    CreateImageViews();
    CreateDepthResources();
    CreateFramebuffers();
}

// ---------- Utility ----------

uint32_t VulkanRenderer::FindMemoryType(uint32_t typeFilter, VkMemoryPropertyFlags properties)
{
    VkPhysicalDeviceMemoryProperties memProps;
    vkGetPhysicalDeviceMemoryProperties(m_ctx.physicalDevice, &memProps);

    for (uint32_t i = 0; i < memProps.memoryTypeCount; i++)
    {
        if ((typeFilter & (1 << i)) &&
            (memProps.memoryTypes[i].propertyFlags & properties) == properties)
        {
            return i;
        }
    }

    throw std::runtime_error("failed to find suitable memory type");
}

VkCommandBuffer VulkanRenderer::BeginSingleTimeCommands()
{
    VkCommandBufferAllocateInfo allocInfo{};
    allocInfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
    allocInfo.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    allocInfo.commandPool = m_ctx.commandPool;
    allocInfo.commandBufferCount = 1;

    VkCommandBuffer cmd;
    vkAllocateCommandBuffers(m_ctx.device, &allocInfo, &cmd);

    VkCommandBufferBeginInfo beginInfo{};
    beginInfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
    beginInfo.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    vkBeginCommandBuffer(cmd, &beginInfo);

    return cmd;
}

void VulkanRenderer::EndSingleTimeCommands(VkCommandBuffer cmd)
{
    vkEndCommandBuffer(cmd);

    VkSubmitInfo submitInfo{};
    submitInfo.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
    submitInfo.commandBufferCount = 1;
    submitInfo.pCommandBuffers = &cmd;

    vkQueueSubmit(m_ctx.graphicsQueue, 1, &submitInfo, VK_NULL_HANDLE);
    vkQueueWaitIdle(m_ctx.graphicsQueue);

    vkFreeCommandBuffers(m_ctx.device, m_ctx.commandPool, 1, &cmd);
}

// ---------- Initialization helpers ----------

void VulkanRenderer::CreateInstance()
{
    VkApplicationInfo appInfo{};
    appInfo.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO;
    appInfo.pApplicationName = "MeshTool";
    appInfo.applicationVersion = VK_MAKE_VERSION(1, 0, 0);
    appInfo.pEngineName = "MeshTool";
    appInfo.engineVersion = VK_MAKE_VERSION(1, 0, 0);
    appInfo.apiVersion = VK_API_VERSION_1_2;

    // Gather required extensions from GLFW
    uint32_t glfwExtCount = 0;
    const char** glfwExts = glfwGetRequiredInstanceExtensions(&glfwExtCount);

    std::vector<const char*> extensions(glfwExts, glfwExts + glfwExtCount);

    if (kEnableValidationLayers)
    {
        extensions.push_back(VK_EXT_DEBUG_UTILS_EXTENSION_NAME);
    }

    VkInstanceCreateInfo createInfo{};
    createInfo.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO;
    createInfo.pApplicationInfo = &appInfo;
    createInfo.enabledExtensionCount = static_cast<uint32_t>(extensions.size());
    createInfo.ppEnabledExtensionNames = extensions.data();

    VkDebugUtilsMessengerCreateInfoEXT debugCreateInfo{};
    if (kEnableValidationLayers)
    {
        createInfo.enabledLayerCount = 1;
        createInfo.ppEnabledLayerNames = kValidationLayers;

        debugCreateInfo.sType = VK_STRUCTURE_TYPE_DEBUG_UTILS_MESSENGER_CREATE_INFO_EXT;
        debugCreateInfo.messageSeverity =
            VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT |
            VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT;
        debugCreateInfo.messageType =
            VK_DEBUG_UTILS_MESSAGE_TYPE_GENERAL_BIT_EXT |
            VK_DEBUG_UTILS_MESSAGE_TYPE_VALIDATION_BIT_EXT |
            VK_DEBUG_UTILS_MESSAGE_TYPE_PERFORMANCE_BIT_EXT;
        debugCreateInfo.pfnUserCallback = DebugCallback;
        createInfo.pNext = &debugCreateInfo;
    }
    else
    {
        createInfo.enabledLayerCount = 0;
    }

    if (vkCreateInstance(&createInfo, nullptr, &m_ctx.instance) != VK_SUCCESS)
    {
        throw std::runtime_error("failed to create Vulkan instance");
    }
}

void VulkanRenderer::CreateSurface(GLFWwindow* window)
{
    if (glfwCreateWindowSurface(m_ctx.instance, window, nullptr, &m_ctx.surface) != VK_SUCCESS)
    {
        throw std::runtime_error("failed to create window surface");
    }
}

void VulkanRenderer::PickPhysicalDevice()
{
    uint32_t deviceCount = 0;
    vkEnumeratePhysicalDevices(m_ctx.instance, &deviceCount, nullptr);
    if (deviceCount == 0)
    {
        throw std::runtime_error("no GPUs with Vulkan support found");
    }

    std::vector<VkPhysicalDevice> devices(deviceCount);
    vkEnumeratePhysicalDevices(m_ctx.instance, &deviceCount, devices.data());

    // Prefer discrete GPU
    VkPhysicalDevice fallback = VK_NULL_HANDLE;
    for (auto& dev : devices)
    {
        VkPhysicalDeviceProperties props;
        vkGetPhysicalDeviceProperties(dev, &props);

        // Check queue families
        uint32_t queueFamilyCount = 0;
        vkGetPhysicalDeviceQueueFamilyProperties(dev, &queueFamilyCount, nullptr);
        std::vector<VkQueueFamilyProperties> queueFamilies(queueFamilyCount);
        vkGetPhysicalDeviceQueueFamilyProperties(dev, &queueFamilyCount, queueFamilies.data());

        bool hasGraphics = false, hasPresent = false;
        for (uint32_t i = 0; i < queueFamilyCount; i++)
        {
            if (queueFamilies[i].queueFlags & VK_QUEUE_GRAPHICS_BIT)
            {
                m_ctx.graphicsFamily = i;
                hasGraphics = true;
            }

            VkBool32 presentSupport = false;
            vkGetPhysicalDeviceSurfaceSupportKHR(dev, i, m_ctx.surface, &presentSupport);
            if (presentSupport)
            {
                m_ctx.presentFamily = i;
                hasPresent = true;
            }
        }

        if (!hasGraphics || !hasPresent)
            continue;

        // Check swapchain extension support
        uint32_t extCount;
        vkEnumerateDeviceExtensionProperties(dev, nullptr, &extCount, nullptr);
        std::vector<VkExtensionProperties> availableExts(extCount);
        vkEnumerateDeviceExtensionProperties(dev, nullptr, &extCount, availableExts.data());

        bool swapchainSupported = false;
        for (auto& ext : availableExts)
        {
            if (strcmp(ext.extensionName, VK_KHR_SWAPCHAIN_EXTENSION_NAME) == 0)
            {
                swapchainSupported = true;
                break;
            }
        }

        if (!swapchainSupported)
            continue;

        if (fallback == VK_NULL_HANDLE)
            fallback = dev;

        if (props.deviceType == VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU)
        {
            m_ctx.physicalDevice = dev;
            return;
        }
    }

    if (fallback != VK_NULL_HANDLE)
    {
        m_ctx.physicalDevice = fallback;
        return;
    }

    throw std::runtime_error("failed to find a suitable GPU");
}

void VulkanRenderer::CreateLogicalDevice()
{
    std::set<uint32_t> uniqueFamilies = {m_ctx.graphicsFamily, m_ctx.presentFamily};

    std::vector<VkDeviceQueueCreateInfo> queueCreateInfos;
    float queuePriority = 1.0f;

    for (uint32_t family : uniqueFamilies)
    {
        VkDeviceQueueCreateInfo queueInfo{};
        queueInfo.sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO;
        queueInfo.queueFamilyIndex = family;
        queueInfo.queueCount = 1;
        queueInfo.pQueuePriorities = &queuePriority;
        queueCreateInfos.push_back(queueInfo);
    }

    VkPhysicalDeviceFeatures deviceFeatures{};
    deviceFeatures.samplerAnisotropy = VK_TRUE;
    deviceFeatures.textureCompressionBC = VK_TRUE;
    deviceFeatures.textureCompressionETC2 = VK_FALSE; // may not be available on desktop

    VkDeviceCreateInfo createInfo{};
    createInfo.sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO;
    createInfo.queueCreateInfoCount = static_cast<uint32_t>(queueCreateInfos.size());
    createInfo.pQueueCreateInfos = queueCreateInfos.data();
    createInfo.pEnabledFeatures = &deviceFeatures;
    createInfo.enabledExtensionCount = 1;
    createInfo.ppEnabledExtensionNames = kDeviceExtensions;

    if (kEnableValidationLayers)
    {
        createInfo.enabledLayerCount = 1;
        createInfo.ppEnabledLayerNames = kValidationLayers;
    }
    else
    {
        createInfo.enabledLayerCount = 0;
    }

    if (vkCreateDevice(m_ctx.physicalDevice, &createInfo, nullptr, &m_ctx.device) != VK_SUCCESS)
    {
        throw std::runtime_error("failed to create logical device");
    }

    vkGetDeviceQueue(m_ctx.device, m_ctx.graphicsFamily, 0, &m_ctx.graphicsQueue);
    vkGetDeviceQueue(m_ctx.device, m_ctx.presentFamily, 0, &m_ctx.presentQueue);
}

void VulkanRenderer::CreateSwapchain()
{
    VkSurfaceCapabilitiesKHR caps;
    vkGetPhysicalDeviceSurfaceCapabilitiesKHR(m_ctx.physicalDevice, m_ctx.surface, &caps);

    // Choose surface format: prefer B8G8R8A8_UNORM (NOT SRGB)
    // ImGui outputs colors in sRGB space already, so using an SRGB swapchain
    // would double-gamma-correct and wash out all colors to grey.
    uint32_t formatCount;
    vkGetPhysicalDeviceSurfaceFormatsKHR(m_ctx.physicalDevice, m_ctx.surface, &formatCount, nullptr);
    std::vector<VkSurfaceFormatKHR> formats(formatCount);
    vkGetPhysicalDeviceSurfaceFormatsKHR(m_ctx.physicalDevice, m_ctx.surface, &formatCount, formats.data());

    VkSurfaceFormatKHR chosenFormat = formats[0];
    for (auto& f : formats)
    {
        if (f.format == VK_FORMAT_B8G8R8A8_UNORM &&
            f.colorSpace == VK_COLOR_SPACE_SRGB_NONLINEAR_KHR)
        {
            chosenFormat = f;
            break;
        }
    }

    // Choose present mode: prefer MAILBOX
    uint32_t presentModeCount;
    vkGetPhysicalDeviceSurfacePresentModesKHR(m_ctx.physicalDevice, m_ctx.surface, &presentModeCount, nullptr);
    std::vector<VkPresentModeKHR> presentModes(presentModeCount);
    vkGetPhysicalDeviceSurfacePresentModesKHR(m_ctx.physicalDevice, m_ctx.surface, &presentModeCount, presentModes.data());

    VkPresentModeKHR chosenMode = VK_PRESENT_MODE_FIFO_KHR;
    for (auto& mode : presentModes)
    {
        if (mode == VK_PRESENT_MODE_MAILBOX_KHR)
        {
            chosenMode = mode;
            break;
        }
    }

    // Choose extent
    VkExtent2D extent;
    if (caps.currentExtent.width != UINT32_MAX)
    {
        extent = caps.currentExtent;
    }
    else
    {
        int w, h;
        glfwGetFramebufferSize(m_window, &w, &h);
        extent.width = std::clamp(static_cast<uint32_t>(w),
                                  caps.minImageExtent.width, caps.maxImageExtent.width);
        extent.height = std::clamp(static_cast<uint32_t>(h),
                                   caps.minImageExtent.height, caps.maxImageExtent.height);
    }

    uint32_t imageCount = caps.minImageCount + 1;
    if (caps.maxImageCount > 0 && imageCount > caps.maxImageCount)
        imageCount = caps.maxImageCount;

    VkSwapchainCreateInfoKHR createInfo{};
    createInfo.sType = VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR;
    createInfo.surface = m_ctx.surface;
    createInfo.minImageCount = imageCount;
    createInfo.imageFormat = chosenFormat.format;
    createInfo.imageColorSpace = chosenFormat.colorSpace;
    createInfo.imageExtent = extent;
    createInfo.imageArrayLayers = 1;
    createInfo.imageUsage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;

    uint32_t queueFamilyIndices[] = {m_ctx.graphicsFamily, m_ctx.presentFamily};
    if (m_ctx.graphicsFamily != m_ctx.presentFamily)
    {
        createInfo.imageSharingMode = VK_SHARING_MODE_CONCURRENT;
        createInfo.queueFamilyIndexCount = 2;
        createInfo.pQueueFamilyIndices = queueFamilyIndices;
    }
    else
    {
        createInfo.imageSharingMode = VK_SHARING_MODE_EXCLUSIVE;
    }

    createInfo.preTransform = caps.currentTransform;
    createInfo.compositeAlpha = VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR;
    createInfo.presentMode = chosenMode;
    createInfo.clipped = VK_TRUE;
    createInfo.oldSwapchain = VK_NULL_HANDLE;

    if (vkCreateSwapchainKHR(m_ctx.device, &createInfo, nullptr, &m_ctx.swapchain) != VK_SUCCESS)
    {
        throw std::runtime_error("failed to create swapchain");
    }

    m_ctx.swapchainFormat = chosenFormat.format;
    m_ctx.swapchainExtent = extent;

    vkGetSwapchainImagesKHR(m_ctx.device, m_ctx.swapchain, &imageCount, nullptr);
    m_ctx.swapchainImages.resize(imageCount);
    vkGetSwapchainImagesKHR(m_ctx.device, m_ctx.swapchain, &imageCount, m_ctx.swapchainImages.data());
}

void VulkanRenderer::CreateImageViews()
{
    m_ctx.swapchainImageViews.resize(m_ctx.swapchainImages.size());

    for (size_t i = 0; i < m_ctx.swapchainImages.size(); i++)
    {
        VkImageViewCreateInfo viewInfo{};
        viewInfo.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
        viewInfo.image = m_ctx.swapchainImages[i];
        viewInfo.viewType = VK_IMAGE_VIEW_TYPE_2D;
        viewInfo.format = m_ctx.swapchainFormat;
        viewInfo.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        viewInfo.subresourceRange.baseMipLevel = 0;
        viewInfo.subresourceRange.levelCount = 1;
        viewInfo.subresourceRange.baseArrayLayer = 0;
        viewInfo.subresourceRange.layerCount = 1;

        if (vkCreateImageView(m_ctx.device, &viewInfo, nullptr,
                              &m_ctx.swapchainImageViews[i]) != VK_SUCCESS)
        {
            throw std::runtime_error("failed to create image view");
        }
    }
}

void VulkanRenderer::CreateRenderPass()
{
    VkAttachmentDescription colorAttachment{};
    colorAttachment.format = m_ctx.swapchainFormat;
    colorAttachment.samples = VK_SAMPLE_COUNT_1_BIT;
    colorAttachment.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
    colorAttachment.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
    colorAttachment.stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
    colorAttachment.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
    colorAttachment.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    colorAttachment.finalLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;

    VkAttachmentDescription depthAttachment{};
    depthAttachment.format = VK_FORMAT_D32_SFLOAT;
    depthAttachment.samples = VK_SAMPLE_COUNT_1_BIT;
    depthAttachment.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
    depthAttachment.storeOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
    depthAttachment.stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
    depthAttachment.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
    depthAttachment.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    depthAttachment.finalLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;

    VkAttachmentReference colorRef{};
    colorRef.attachment = 0;
    colorRef.layout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;

    VkAttachmentReference depthRef{};
    depthRef.attachment = 1;
    depthRef.layout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;

    VkSubpassDescription subpass{};
    subpass.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
    subpass.colorAttachmentCount = 1;
    subpass.pColorAttachments = &colorRef;
    subpass.pDepthStencilAttachment = &depthRef;

    VkSubpassDependency dependency{};
    dependency.srcSubpass = VK_SUBPASS_EXTERNAL;
    dependency.dstSubpass = 0;
    dependency.srcStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT |
                              VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT;
    dependency.srcAccessMask = 0;
    dependency.dstStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT |
                              VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT;
    dependency.dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT |
                               VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;

    VkAttachmentDescription attachments[] = {colorAttachment, depthAttachment};

    VkRenderPassCreateInfo rpInfo{};
    rpInfo.sType = VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO;
    rpInfo.attachmentCount = 2;
    rpInfo.pAttachments = attachments;
    rpInfo.subpassCount = 1;
    rpInfo.pSubpasses = &subpass;
    rpInfo.dependencyCount = 1;
    rpInfo.pDependencies = &dependency;

    if (vkCreateRenderPass(m_ctx.device, &rpInfo, nullptr, &m_ctx.renderPass) != VK_SUCCESS)
    {
        throw std::runtime_error("failed to create render pass");
    }
}

void VulkanRenderer::CreateDepthResources()
{
    VkFormat depthFormat = VK_FORMAT_D32_SFLOAT;

    VkImageCreateInfo imageInfo{};
    imageInfo.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
    imageInfo.imageType = VK_IMAGE_TYPE_2D;
    imageInfo.format = depthFormat;
    imageInfo.extent.width = m_ctx.swapchainExtent.width;
    imageInfo.extent.height = m_ctx.swapchainExtent.height;
    imageInfo.extent.depth = 1;
    imageInfo.mipLevels = 1;
    imageInfo.arrayLayers = 1;
    imageInfo.samples = VK_SAMPLE_COUNT_1_BIT;
    imageInfo.tiling = VK_IMAGE_TILING_OPTIMAL;
    imageInfo.usage = VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT;
    imageInfo.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    imageInfo.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;

    if (vkCreateImage(m_ctx.device, &imageInfo, nullptr, &m_ctx.depthImage) != VK_SUCCESS)
    {
        throw std::runtime_error("failed to create depth image");
    }

    VkMemoryRequirements memReqs;
    vkGetImageMemoryRequirements(m_ctx.device, m_ctx.depthImage, &memReqs);

    VkMemoryAllocateInfo allocInfo{};
    allocInfo.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
    allocInfo.allocationSize = memReqs.size;
    allocInfo.memoryTypeIndex = FindMemoryType(memReqs.memoryTypeBits,
                                               VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);

    if (vkAllocateMemory(m_ctx.device, &allocInfo, nullptr, &m_ctx.depthMemory) != VK_SUCCESS)
    {
        throw std::runtime_error("failed to allocate depth image memory");
    }

    vkBindImageMemory(m_ctx.device, m_ctx.depthImage, m_ctx.depthMemory, 0);

    VkImageViewCreateInfo viewInfo{};
    viewInfo.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
    viewInfo.image = m_ctx.depthImage;
    viewInfo.viewType = VK_IMAGE_VIEW_TYPE_2D;
    viewInfo.format = depthFormat;
    viewInfo.subresourceRange.aspectMask = VK_IMAGE_ASPECT_DEPTH_BIT;
    viewInfo.subresourceRange.baseMipLevel = 0;
    viewInfo.subresourceRange.levelCount = 1;
    viewInfo.subresourceRange.baseArrayLayer = 0;
    viewInfo.subresourceRange.layerCount = 1;

    if (vkCreateImageView(m_ctx.device, &viewInfo, nullptr, &m_ctx.depthView) != VK_SUCCESS)
    {
        throw std::runtime_error("failed to create depth image view");
    }
}

void VulkanRenderer::CreateFramebuffers()
{
    m_ctx.framebuffers.resize(m_ctx.swapchainImageViews.size());

    for (size_t i = 0; i < m_ctx.swapchainImageViews.size(); i++)
    {
        VkImageView attachments[] = {
            m_ctx.swapchainImageViews[i],
            m_ctx.depthView
        };

        VkFramebufferCreateInfo fbInfo{};
        fbInfo.sType = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO;
        fbInfo.renderPass = m_ctx.renderPass;
        fbInfo.attachmentCount = 2;
        fbInfo.pAttachments = attachments;
        fbInfo.width = m_ctx.swapchainExtent.width;
        fbInfo.height = m_ctx.swapchainExtent.height;
        fbInfo.layers = 1;

        if (vkCreateFramebuffer(m_ctx.device, &fbInfo, nullptr,
                                &m_ctx.framebuffers[i]) != VK_SUCCESS)
        {
            throw std::runtime_error("failed to create framebuffer");
        }
    }
}

void VulkanRenderer::CreateCommandPool()
{
    VkCommandPoolCreateInfo poolInfo{};
    poolInfo.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
    poolInfo.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
    poolInfo.queueFamilyIndex = m_ctx.graphicsFamily;

    if (vkCreateCommandPool(m_ctx.device, &poolInfo, nullptr, &m_ctx.commandPool) != VK_SUCCESS)
    {
        throw std::runtime_error("failed to create command pool");
    }
}

void VulkanRenderer::CreateCommandBuffers()
{
    m_ctx.commandBuffers.resize(VulkanContext::MAX_FRAMES_IN_FLIGHT);

    VkCommandBufferAllocateInfo allocInfo{};
    allocInfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
    allocInfo.commandPool = m_ctx.commandPool;
    allocInfo.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    allocInfo.commandBufferCount = static_cast<uint32_t>(m_ctx.commandBuffers.size());

    if (vkAllocateCommandBuffers(m_ctx.device, &allocInfo, m_ctx.commandBuffers.data()) != VK_SUCCESS)
    {
        throw std::runtime_error("failed to allocate command buffers");
    }
}

void VulkanRenderer::CreateDescriptorPool()
{
    VkDescriptorPoolSize poolSizes[] = {
        {VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 100},
        {VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1000},
    };

    VkDescriptorPoolCreateInfo poolInfo{};
    poolInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
    poolInfo.flags = VK_DESCRIPTOR_POOL_CREATE_FREE_DESCRIPTOR_SET_BIT;
    poolInfo.maxSets = 1100;
    poolInfo.poolSizeCount = 2;
    poolInfo.pPoolSizes = poolSizes;

    if (vkCreateDescriptorPool(m_ctx.device, &poolInfo, nullptr, &m_ctx.descriptorPool) != VK_SUCCESS)
    {
        throw std::runtime_error("failed to create descriptor pool");
    }
}

void VulkanRenderer::CreateSyncObjects()
{
    m_ctx.imageAvailableSemaphores.resize(VulkanContext::MAX_FRAMES_IN_FLIGHT);
    m_ctx.renderFinishedSemaphores.resize(VulkanContext::MAX_FRAMES_IN_FLIGHT);
    m_ctx.inFlightFences.resize(VulkanContext::MAX_FRAMES_IN_FLIGHT);

    VkSemaphoreCreateInfo semInfo{};
    semInfo.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO;

    VkFenceCreateInfo fenceInfo{};
    fenceInfo.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;
    fenceInfo.flags = VK_FENCE_CREATE_SIGNALED_BIT;

    for (int i = 0; i < VulkanContext::MAX_FRAMES_IN_FLIGHT; i++)
    {
        if (vkCreateSemaphore(m_ctx.device, &semInfo, nullptr, &m_ctx.imageAvailableSemaphores[i]) != VK_SUCCESS ||
            vkCreateSemaphore(m_ctx.device, &semInfo, nullptr, &m_ctx.renderFinishedSemaphores[i]) != VK_SUCCESS ||
            vkCreateFence(m_ctx.device, &fenceInfo, nullptr, &m_ctx.inFlightFences[i]) != VK_SUCCESS)
        {
            throw std::runtime_error("failed to create synchronization objects");
        }
    }
}

void VulkanRenderer::CleanupSwapchain()
{
    if (m_ctx.depthView != VK_NULL_HANDLE)
    {
        vkDestroyImageView(m_ctx.device, m_ctx.depthView, nullptr);
        m_ctx.depthView = VK_NULL_HANDLE;
    }
    if (m_ctx.depthImage != VK_NULL_HANDLE)
    {
        vkDestroyImage(m_ctx.device, m_ctx.depthImage, nullptr);
        m_ctx.depthImage = VK_NULL_HANDLE;
    }
    if (m_ctx.depthMemory != VK_NULL_HANDLE)
    {
        vkFreeMemory(m_ctx.device, m_ctx.depthMemory, nullptr);
        m_ctx.depthMemory = VK_NULL_HANDLE;
    }

    for (auto fb : m_ctx.framebuffers)
        vkDestroyFramebuffer(m_ctx.device, fb, nullptr);
    m_ctx.framebuffers.clear();

    for (auto iv : m_ctx.swapchainImageViews)
        vkDestroyImageView(m_ctx.device, iv, nullptr);
    m_ctx.swapchainImageViews.clear();

    if (m_ctx.swapchain != VK_NULL_HANDLE)
    {
        vkDestroySwapchainKHR(m_ctx.device, m_ctx.swapchain, nullptr);
        m_ctx.swapchain = VK_NULL_HANDLE;
    }
}
