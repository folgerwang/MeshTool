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
    VkSampler sampler = VK_NULL_HANDLE;           // shared, owned by the manager
    VkDescriptorSet descriptorSet = VK_NULL_HANDLE;   // UI images only (ImGui binds sets)
    VkDescriptorPool pool = VK_NULL_HANDLE;       // the pool descriptorSet came from
    uint32_t slot = UINT32_MAX;                   // index in the bindless texture array
    uint32_t width = 0, height = 0;
};

class VulkanTextureManager {
public:
    void Init(VulkanContext* ctx, VkDescriptorSetLayout texLayout);
    void Shutdown();

    // Upload a texture from Texture2DInfo (handles compressed formats)
    uint32_t UploadTexture(const core::Texture2DInfo* info);

    // Get texture by handle (nullptr if unknown or released)
    VulkanTexture* GetTexture(uint32_t handle);

    // Frees a texture's GPU resources; the handle is not reused. The caller
    // must make sure the GPU is idle.
    void ReleaseTexture(uint32_t handle);

    // Upload raw RGBA pixels (for icons, UI images)
    uint32_t UploadRGBA(const uint8_t* pixels, uint32_t width, uint32_t height);

    // Get ImGui-compatible texture ID (VkDescriptorSet); UI images only
    void* GetImTextureID(uint32_t handle);

    VkDescriptorSetLayout GetTextureLayout() const { return m_textureLayout; }

    // Bindless: every texture sits in one descriptor array; meshes bind the
    // set once and pick their texture by slot (push constant).
    VkDescriptorSetLayout GetBindlessLayout() const { return m_bindlessLayout; }
    VkDescriptorSet GetBindlessSet() const { return m_bindlessSet; }
    // Slot of a texture, or UINT32_MAX if unknown / released.
    uint32_t GetSlot(uint32_t handle) const;

private:
    VulkanContext* m_ctx = nullptr;
    VkDescriptorSetLayout m_textureLayout = VK_NULL_HANDLE;
    std::vector<VulkanTexture> m_textures;

    // Texture descriptor sets come from pools of the manager's own, another
    // added whenever the last is full (merged captures hold many thousand
    // textures). Samplers are shared: devices cap how many may exist.
    std::vector<VkDescriptorPool> m_pools;
    VkSampler m_repeatSampler = VK_NULL_HANDLE;   // captured textures
    VkSampler m_clampSampler = VK_NULL_HANDLE;    // UI images
    VkDescriptorSet AllocateSet(VkDescriptorPool& pool);
    void DestroyTexture(VulkanTexture& tex);

    VkDescriptorSetLayout m_bindlessLayout = VK_NULL_HANDLE;
    VkDescriptorPool m_bindlessPool = VK_NULL_HANDLE;
    VkDescriptorSet m_bindlessSet = VK_NULL_HANDLE;
    uint32_t m_bindlessCapacity = 0;
    uint32_t m_nextSlot = 0;
    std::vector<uint32_t> m_freeSlots;   // released slots, reused
    void InitBindless();
    // Gives tex a slot and points it at tex's view (throws when full).
    void AssignSlot(VulkanTexture& tex);

    VkFormat MapGLFormatToVulkan(uint32_t internalFormat, uint32_t format, uint32_t type);
};
