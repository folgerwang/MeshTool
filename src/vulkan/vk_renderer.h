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

    // Utility
    uint32_t FindMemoryType(uint32_t typeFilter, VkMemoryPropertyFlags properties);
    VkCommandBuffer BeginSingleTimeCommands();
    void EndSingleTimeCommands(VkCommandBuffer cmd);

private:
    VulkanContext m_ctx;
    GLFWwindow* m_window = nullptr;
    bool m_framebufferResized = false;

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
