#include "vk_buffer.h"
#include "vk_renderer.h"

#include <cstring>
#include <stdexcept>

namespace vkutil {

static uint32_t FindMemoryType(VulkanContext& ctx, uint32_t typeFilter, VkMemoryPropertyFlags properties)
{
    VkPhysicalDeviceMemoryProperties memProps;
    vkGetPhysicalDeviceMemoryProperties(ctx.physicalDevice, &memProps);

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

Buffer CreateBuffer(VulkanContext& ctx, VkDeviceSize size,
                    VkBufferUsageFlags usage, VkMemoryPropertyFlags properties)
{
    Buffer result;
    result.size = size;

    VkBufferCreateInfo bufferInfo{};
    bufferInfo.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
    bufferInfo.size = size;
    bufferInfo.usage = usage;
    bufferInfo.sharingMode = VK_SHARING_MODE_EXCLUSIVE;

    if (vkCreateBuffer(ctx.device, &bufferInfo, nullptr, &result.buffer) != VK_SUCCESS)
    {
        throw std::runtime_error("failed to create buffer");
    }

    VkMemoryRequirements memReqs;
    vkGetBufferMemoryRequirements(ctx.device, result.buffer, &memReqs);

    VkMemoryAllocateInfo allocInfo{};
    allocInfo.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
    allocInfo.allocationSize = memReqs.size;
    allocInfo.memoryTypeIndex = FindMemoryType(ctx, memReqs.memoryTypeBits, properties);

    if (vkAllocateMemory(ctx.device, &allocInfo, nullptr, &result.memory) != VK_SUCCESS)
    {
        throw std::runtime_error("failed to allocate buffer memory");
    }

    vkBindBufferMemory(ctx.device, result.buffer, result.memory, 0);

    return result;
}

void DestroyBuffer(VulkanContext& ctx, Buffer& buffer)
{
    if (buffer.buffer != VK_NULL_HANDLE)
    {
        vkDestroyBuffer(ctx.device, buffer.buffer, nullptr);
        buffer.buffer = VK_NULL_HANDLE;
    }
    if (buffer.memory != VK_NULL_HANDLE)
    {
        vkFreeMemory(ctx.device, buffer.memory, nullptr);
        buffer.memory = VK_NULL_HANDLE;
    }
    buffer.size = 0;
}

void CopyToBuffer(VulkanContext& ctx, Buffer& dst, const void* data, VkDeviceSize size)
{
    void* mapped;
    vkMapMemory(ctx.device, dst.memory, 0, size, 0, &mapped);
    memcpy(mapped, data, static_cast<size_t>(size));
    vkUnmapMemory(ctx.device, dst.memory);
}

static void CopyBufferToBuffer(VulkanContext& ctx, VkBuffer src, VkBuffer dst, VkDeviceSize size)
{
    // Begin single-time command buffer
    VkCommandBufferAllocateInfo allocInfo{};
    allocInfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
    allocInfo.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    allocInfo.commandPool = ctx.commandPool;
    allocInfo.commandBufferCount = 1;

    VkCommandBuffer cmd;
    vkAllocateCommandBuffers(ctx.device, &allocInfo, &cmd);

    VkCommandBufferBeginInfo beginInfo{};
    beginInfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
    beginInfo.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    vkBeginCommandBuffer(cmd, &beginInfo);

    VkBufferCopy copyRegion{};
    copyRegion.size = size;
    vkCmdCopyBuffer(cmd, src, dst, 1, &copyRegion);

    vkEndCommandBuffer(cmd);

    VkSubmitInfo submitInfo{};
    submitInfo.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
    submitInfo.commandBufferCount = 1;
    submitInfo.pCommandBuffers = &cmd;

    vkQueueSubmit(ctx.graphicsQueue, 1, &submitInfo, VK_NULL_HANDLE);
    vkQueueWaitIdle(ctx.graphicsQueue);

    vkFreeCommandBuffers(ctx.device, ctx.commandPool, 1, &cmd);
}

Buffer CreateDeviceLocalBuffer(VulkanContext& ctx, VkBufferUsageFlags usage,
                               const void* data, VkDeviceSize size)
{
    // Create staging buffer
    Buffer staging = CreateBuffer(ctx, size,
        VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
        VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);

    CopyToBuffer(ctx, staging, data, size);

    // Create device-local buffer
    Buffer result = CreateBuffer(ctx, size,
        usage | VK_BUFFER_USAGE_TRANSFER_DST_BIT,
        VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);

    CopyBufferToBuffer(ctx, staging.buffer, result.buffer, size);

    DestroyBuffer(ctx, staging);

    return result;
}

} // namespace vkutil
