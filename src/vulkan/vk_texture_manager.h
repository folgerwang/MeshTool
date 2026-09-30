#pragma once
#include <vulkan/vulkan.h>
#include <vector>
#include <map>
#include <cstdint>

struct VulkanContext;
namespace core { struct Texture2DInfo; }

struct VulkanTexture {
    VkImage image = VK_NULL_HANDLE;
    VkDeviceMemory memory = VK_NULL_HANDLE;
    VkImageView view = VK_NULL_HANDLE;
    VkSampler sampler = VK_NULL_HANDLE;
    VkDescriptorSet descriptorSet = VK_NULL_HANDLE;
    uint32_t width = 0, height = 0;
};

class VulkanTextureManager {
public:
    void Init(VulkanContext* ctx, VkDescriptorSetLayout texLayout);
    void Shutdown();

    // Upload a texture from Texture2DInfo (handles compressed formats)
    uint32_t UploadTexture(const core::Texture2DInfo* info);

    // Get texture by handle
    VulkanTexture* GetTexture(uint32_t handle);

    // Upload raw RGBA pixels (for icons, UI images)
    uint32_t UploadRGBA(const uint8_t* pixels, uint32_t width, uint32_t height);

    // Get ImGui-compatible texture ID (VkDescriptorSet)
    void* GetImTextureID(uint32_t handle);

    VkDescriptorSetLayout GetTextureLayout() const { return m_textureLayout; }

private:
    VulkanContext* m_ctx = nullptr;
    VkDescriptorSetLayout m_textureLayout = VK_NULL_HANDLE;
    std::vector<VulkanTexture> m_textures;

    VkFormat MapGLFormatToVulkan(uint32_t internalFormat, uint32_t format, uint32_t type);
};
