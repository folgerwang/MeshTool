#pragma once
#include <vulkan/vulkan.h>
#include <cstdint>

struct VulkanContext;

namespace vkutil {

struct Buffer {
    VkBuffer buffer = VK_NULL_HANDLE;
    VkDeviceMemory memory = VK_NULL_HANDLE;
    VkDeviceSize size = 0;
};

Buffer CreateBuffer(VulkanContext& ctx, VkDeviceSize size, VkBufferUsageFlags usage, VkMemoryPropertyFlags properties);
void DestroyBuffer(VulkanContext& ctx, Buffer& buffer);
void CopyToBuffer(VulkanContext& ctx, Buffer& dst, const void* data, VkDeviceSize size);

// Create device-local buffer with staging upload
Buffer CreateDeviceLocalBuffer(VulkanContext& ctx, VkBufferUsageFlags usage, const void* data, VkDeviceSize size);

} // namespace vkutil
