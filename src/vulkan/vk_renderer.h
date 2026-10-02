#pragma once
#include <vulkan/vulkan.h>
#include <vector>
#include <cstdint>

struct GLFWwindow;

struct VulkanContext
{
    VkInstance instance = VK_NULL_HANDLE;
    VkPhysicalDevice physicalDevice = VK_NULL_HANDLE;
    VkDevice device = VK_NULL_HANDLE;
    VkQueue graphicsQueue = VK_NULL_HANDLE;
    VkQueue presentQueue = VK_NULL_HANDLE;
    uint32_t graphicsFamily = 0;
    uint32_t presentFamily = 0;

    VkSurfaceKHR surface = VK_NULL_HANDLE;
    VkSwapchainKHR swapchain = VK_NULL_HANDLE;
    VkFormat swapchainFormat;
    VkExtent2D swapchainExtent;
    std::vector<VkImage> swapchainImages;
    std::vector<VkImageView> swapchainImageViews;

    VkRenderPass renderPass = VK_NULL_HANDLE;
    std::vector<VkFramebuffer> framebuffers;

    VkImage depthImage = VK_NULL_HANDLE;
    VkDeviceMemory depthMemory = VK_NULL_HANDLE;
    VkImageView depthView = VK_NULL_HANDLE;

    VkCommandPool commandPool = VK_NULL_HANDLE;
    std::vector<VkCommandBuffer> commandBuffers;

    VkDescriptorPool descriptorPool = VK_NULL_HANDLE;

    // Synchronization
    static const int MAX_FRAMES_IN_FLIGHT = 2;
    std::vector<VkSemaphore> imageAvailableSemaphores;
    std::vector<VkSemaphore> renderFinishedSemaphores;
    std::vector<VkFence> inFlightFences;
    uint32_t currentFrame = 0;
    uint32_t imageIndex = 0;
};

class VulkanRenderer
{
public:
    VulkanRenderer();
    ~VulkanRenderer();

    bool Init(GLFWwindow* window);
    void Shutdown();

    bool BeginFrame();  // returns false if swapchain needs recreation
    void EndFrame();

    void RecreateSwapchain(int width, int height);

    VulkanContext& GetContext() { return m_ctx; }
    VkCommandBuffer GetCurrentCommandBuffer() { return m_ctx.commandBuffers[m_ctx.currentFrame]; }

    // Screenshot: the next EndFrame copies the finished frame (UI included)
    // to memory; TakeReadback returns it as tightly packed RGB rows.
    void RequestReadback() { m_readbackRequested = true; }
    bool ReadbackSupported() const { return m_readbackSupported; }
    bool TakeReadback(std::vector<uint8_t>& rgb, int& width, int& height);

    // Utility
    uint32_t FindMemoryType(uint32_t typeFilter, VkMemoryPropertyFlags properties);
    VkCommandBuffer BeginSingleTimeCommands();
    void EndSingleTimeCommands(VkCommandBuffer cmd);

private:
    VulkanContext m_ctx;
    GLFWwindow* m_window = nullptr;
    bool m_framebufferResized = false;
    bool m_readbackRequested = false;
    bool m_readbackSupported = false;
    bool m_readbackReady = false;
    std::vector<uint8_t> m_readbackRgb;
    int m_readbackW = 0, m_readbackH = 0;

    void CreateInstance();
    void CreateSurface(GLFWwindow* window);
    void PickPhysicalDevice();
    void CreateLogicalDevice();
    void CreateSwapchain();
    void CreateImageViews();
    void CreateRenderPass();
    void CreateDepthResources();
    void CreateFramebuffers();
    void CreateCommandPool();
    void CreateCommandBuffers();
    void CreateDescriptorPool();
    void CreateSyncObjects();

    void CleanupSwapchain();
};
