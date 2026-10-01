#include "vk_texture_manager.h"
#include "vk_renderer.h"
#include "vk_buffer.h"
#include "../../include/coretexture.h"

#include <cstring>
#include <stdexcept>
#include <algorithm>

// OpenGL internal format constants
static const uint32_t GL_RGB8  = 0x8051;
static const uint32_t GL_RGBA8 = 0x8058;
static const uint32_t GL_BGRA  = 0x80E1;
static const uint32_t GL_COMPRESSED_RGB_S3TC_DXT1  = 0x83F0;
static const uint32_t GL_COMPRESSED_RGBA_S3TC_DXT1 = 0x83F1;
static const uint32_t GL_COMPRESSED_RGBA_S3TC_DXT3 = 0x83F2;
static const uint32_t GL_COMPRESSED_RGBA_S3TC_DXT5 = 0x83F3;
static const uint32_t GL_COMPRESSED_RGB8_ETC2      = 0x9274;

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

static bool IsCompressedFormat(VkFormat format)
{
    switch (format)
    {
        case VK_FORMAT_BC1_RGB_UNORM_BLOCK:
        case VK_FORMAT_BC1_RGBA_UNORM_BLOCK:
        case VK_FORMAT_BC2_UNORM_BLOCK:
        case VK_FORMAT_BC3_UNORM_BLOCK:
        case VK_FORMAT_ETC2_R8G8B8_UNORM_BLOCK:
            return true;
        default:
            return false;
    }
}

void VulkanTextureManager::Init(VulkanContext* ctx, VkDescriptorSetLayout texLayout)
{
    m_ctx = ctx;
    m_textureLayout = texLayout;
}

void VulkanTextureManager::Shutdown()
{
    if (!m_ctx)
        return;

    for (auto& tex : m_textures)
    {
        if (tex.sampler != VK_NULL_HANDLE)
            vkDestroySampler(m_ctx->device, tex.sampler, nullptr);
        if (tex.view != VK_NULL_HANDLE)
            vkDestroyImageView(m_ctx->device, tex.view, nullptr);
        if (tex.image != VK_NULL_HANDLE)
            vkDestroyImage(m_ctx->device, tex.image, nullptr);
        if (tex.memory != VK_NULL_HANDLE)
            vkFreeMemory(m_ctx->device, tex.memory, nullptr);
    }
    m_textures.clear();
}

VkFormat VulkanTextureManager::MapGLFormatToVulkan(uint32_t internalFormat, uint32_t format, uint32_t type)
{
    switch (internalFormat)
    {
        case GL_RGBA8:
            return VK_FORMAT_R8G8B8A8_UNORM;
        case GL_RGB8:
            return VK_FORMAT_R8G8B8A8_UNORM; // will expand to 4 channels during upload
        case GL_BGRA:
            return VK_FORMAT_B8G8R8A8_UNORM;
        case GL_COMPRESSED_RGB_S3TC_DXT1:
            return VK_FORMAT_BC1_RGB_UNORM_BLOCK;
        case GL_COMPRESSED_RGBA_S3TC_DXT1:
            return VK_FORMAT_BC1_RGBA_UNORM_BLOCK;
        case GL_COMPRESSED_RGBA_S3TC_DXT3:
            return VK_FORMAT_BC2_UNORM_BLOCK;
        case GL_COMPRESSED_RGBA_S3TC_DXT5:
            return VK_FORMAT_BC3_UNORM_BLOCK;
        case GL_COMPRESSED_RGB8_ETC2:
            return VK_FORMAT_ETC2_R8G8B8_UNORM_BLOCK;
        default:
            return VK_FORMAT_R8G8B8A8_UNORM;
    }
}

uint32_t VulkanTextureManager::UploadTexture(const core::Texture2DInfo* info)
{
    if (!info || info->m_levelCount == 0)
        return UINT32_MAX;

    const auto& mip0 = info->m_mips[0];
    uint32_t width = mip0.m_width;
    uint32_t height = mip0.m_height;

    VkFormat vkFormat = MapGLFormatToVulkan(info->m_internalFormat, info->m_format, info->m_type);
    bool compressed = IsCompressedFormat(vkFormat);
    bool needsRGB2RGBA = (info->m_internalFormat == GL_RGB8);

    // Determine upload data and size
    const void* uploadData = mip0.m_imageData.get();
    VkDeviceSize uploadSize = mip0.m_size;
    std::unique_ptr<uint8_t[]> expandedData;

    if (needsRGB2RGBA)
    {
        // Expand RGB to RGBA
        uint32_t pixelCount = width * height;
        uploadSize = pixelCount * 4;
        expandedData.reset(new uint8_t[uploadSize]);
        const uint8_t* src = reinterpret_cast<const uint8_t*>(mip0.m_imageData.get());
        uint8_t* dst = expandedData.get();
        for (uint32_t p = 0; p < pixelCount; p++)
        {
            dst[p * 4 + 0] = src[p * 3 + 0];
            dst[p * 4 + 1] = src[p * 3 + 1];
            dst[p * 4 + 2] = src[p * 3 + 2];
            dst[p * 4 + 3] = 255;
        }
        uploadData = expandedData.get();
    }

    // Create staging buffer
    vkutil::Buffer staging = vkutil::CreateBuffer(*m_ctx, uploadSize,
        VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
        VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
    vkutil::CopyToBuffer(*m_ctx, staging, uploadData, uploadSize);

    // Create image
    VkImageCreateInfo imageInfo{};
    imageInfo.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
    imageInfo.imageType = VK_IMAGE_TYPE_2D;
    imageInfo.format = vkFormat;
    imageInfo.extent.width = width;
    imageInfo.extent.height = height;
    imageInfo.extent.depth = 1;
    imageInfo.mipLevels = 1;
    imageInfo.arrayLayers = 1;
    imageInfo.samples = VK_SAMPLE_COUNT_1_BIT;
    imageInfo.tiling = VK_IMAGE_TILING_OPTIMAL;
    imageInfo.usage = VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;
    imageInfo.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    imageInfo.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;

    VulkanTexture tex;
    tex.width = width;
    tex.height = height;

    if (vkCreateImage(m_ctx->device, &imageInfo, nullptr, &tex.image) != VK_SUCCESS)
    {
        vkutil::DestroyBuffer(*m_ctx, staging);
        throw std::runtime_error("failed to create texture image");
    }

    // Allocate and bind memory
    VkMemoryRequirements memReqs;
    vkGetImageMemoryRequirements(m_ctx->device, tex.image, &memReqs);

    VkMemoryAllocateInfo allocInfo{};
    allocInfo.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
    allocInfo.allocationSize = memReqs.size;
    allocInfo.memoryTypeIndex = FindMemoryType(*m_ctx, memReqs.memoryTypeBits,
                                               VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);

    if (vkAllocateMemory(m_ctx->device, &allocInfo, nullptr, &tex.memory) != VK_SUCCESS)
    {
        vkutil::DestroyBuffer(*m_ctx, staging);
        throw std::runtime_error("failed to allocate texture image memory");
    }

    vkBindImageMemory(m_ctx->device, tex.image, tex.memory, 0);

    // Transition image layout and copy data using single-time commands
    {
        VkCommandBufferAllocateInfo cmdAllocInfo{};
        cmdAllocInfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
        cmdAllocInfo.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
        cmdAllocInfo.commandPool = m_ctx->commandPool;
        cmdAllocInfo.commandBufferCount = 1;

        VkCommandBuffer cmd;
        vkAllocateCommandBuffers(m_ctx->device, &cmdAllocInfo, &cmd);

        VkCommandBufferBeginInfo beginInfo{};
        beginInfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
        beginInfo.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
        vkBeginCommandBuffer(cmd, &beginInfo);

        // Transition: UNDEFINED -> TRANSFER_DST_OPTIMAL
        VkImageMemoryBarrier barrier{};
        barrier.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
        barrier.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        barrier.newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
        barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        barrier.image = tex.image;
        barrier.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        barrier.subresourceRange.baseMipLevel = 0;
        barrier.subresourceRange.levelCount = 1;
        barrier.subresourceRange.baseArrayLayer = 0;
        barrier.subresourceRange.layerCount = 1;
        barrier.srcAccessMask = 0;
        barrier.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;

        vkCmdPipelineBarrier(cmd,
            VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
            VK_PIPELINE_STAGE_TRANSFER_BIT,
            0, 0, nullptr, 0, nullptr, 1, &barrier);

        // Copy buffer to image
        VkBufferImageCopy region{};
        region.bufferOffset = 0;
        region.bufferRowLength = 0;
        region.bufferImageHeight = 0;
        region.imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        region.imageSubresource.mipLevel = 0;
        region.imageSubresource.baseArrayLayer = 0;
        region.imageSubresource.layerCount = 1;
        region.imageOffset = {0, 0, 0};
        region.imageExtent = {width, height, 1};

        vkCmdCopyBufferToImage(cmd, staging.buffer, tex.image,
                               VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &region);

        // Transition: TRANSFER_DST_OPTIMAL -> SHADER_READ_ONLY_OPTIMAL
        barrier.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
        barrier.newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        barrier.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
        barrier.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;

        vkCmdPipelineBarrier(cmd,
            VK_PIPELINE_STAGE_TRANSFER_BIT,
            VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
            0, 0, nullptr, 0, nullptr, 1, &barrier);

        vkEndCommandBuffer(cmd);

        VkSubmitInfo submitInfo{};
        submitInfo.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
        submitInfo.commandBufferCount = 1;
        submitInfo.pCommandBuffers = &cmd;

        vkQueueSubmit(m_ctx->graphicsQueue, 1, &submitInfo, VK_NULL_HANDLE);
        vkQueueWaitIdle(m_ctx->graphicsQueue);

        vkFreeCommandBuffers(m_ctx->device, m_ctx->commandPool, 1, &cmd);
    }

    vkutil::DestroyBuffer(*m_ctx, staging);

    // Create image view
    VkImageViewCreateInfo viewInfo{};
    viewInfo.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
    viewInfo.image = tex.image;
    viewInfo.viewType = VK_IMAGE_VIEW_TYPE_2D;
    viewInfo.format = vkFormat;
    viewInfo.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    viewInfo.subresourceRange.baseMipLevel = 0;
    viewInfo.subresourceRange.levelCount = 1;
    viewInfo.subresourceRange.baseArrayLayer = 0;
    viewInfo.subresourceRange.layerCount = 1;

    if (vkCreateImageView(m_ctx->device, &viewInfo, nullptr, &tex.view) != VK_SUCCESS)
    {
        throw std::runtime_error("failed to create texture image view");
    }

    // Create sampler
    VkSamplerCreateInfo samplerInfo{};
    samplerInfo.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO;
    samplerInfo.magFilter = VK_FILTER_LINEAR;
    samplerInfo.minFilter = VK_FILTER_LINEAR;
    samplerInfo.mipmapMode = VK_SAMPLER_MIPMAP_MODE_LINEAR;
    samplerInfo.addressModeU = VK_SAMPLER_ADDRESS_MODE_REPEAT;
    samplerInfo.addressModeV = VK_SAMPLER_ADDRESS_MODE_REPEAT;
    samplerInfo.addressModeW = VK_SAMPLER_ADDRESS_MODE_REPEAT;
    samplerInfo.anisotropyEnable = VK_TRUE;
    samplerInfo.maxAnisotropy = 16.0f;
    samplerInfo.borderColor = VK_BORDER_COLOR_INT_OPAQUE_BLACK;
    samplerInfo.unnormalizedCoordinates = VK_FALSE;
    samplerInfo.compareEnable = VK_FALSE;
    samplerInfo.compareOp = VK_COMPARE_OP_ALWAYS;
    samplerInfo.mipLodBias = 0.0f;
    samplerInfo.minLod = 0.0f;
    samplerInfo.maxLod = 0.0f;

    if (vkCreateSampler(m_ctx->device, &samplerInfo, nullptr, &tex.sampler) != VK_SUCCESS)
    {
        throw std::runtime_error("failed to create texture sampler");
    }

    // Create descriptor set
    VkDescriptorSetAllocateInfo dsAllocInfo{};
    dsAllocInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
    dsAllocInfo.descriptorPool = m_ctx->descriptorPool;
    dsAllocInfo.descriptorSetCount = 1;
    dsAllocInfo.pSetLayouts = &m_textureLayout;

    if (vkAllocateDescriptorSets(m_ctx->device, &dsAllocInfo, &tex.descriptorSet) != VK_SUCCESS)
    {
        throw std::runtime_error("failed to allocate texture descriptor set");
    }

    VkDescriptorImageInfo imageDescInfo{};
    imageDescInfo.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    imageDescInfo.imageView = tex.view;
    imageDescInfo.sampler = tex.sampler;

    VkWriteDescriptorSet descriptorWrite{};
    descriptorWrite.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    descriptorWrite.dstSet = tex.descriptorSet;
    descriptorWrite.dstBinding = 0;
    descriptorWrite.dstArrayElement = 0;
    descriptorWrite.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    descriptorWrite.descriptorCount = 1;
    descriptorWrite.pImageInfo = &imageDescInfo;

    vkUpdateDescriptorSets(m_ctx->device, 1, &descriptorWrite, 0, nullptr);

    uint32_t handle = static_cast<uint32_t>(m_textures.size());
    m_textures.push_back(tex);
    return handle;
}

VulkanTexture* VulkanTextureManager::GetTexture(uint32_t handle)
{
    if (handle >= static_cast<uint32_t>(m_textures.size()))
        return nullptr;
    VulkanTexture* tex = &m_textures[handle];
    return tex->image != VK_NULL_HANDLE ? tex : nullptr;
}

void VulkanTextureManager::ReleaseTexture(uint32_t handle)
{
    if (handle >= static_cast<uint32_t>(m_textures.size()))
        return;
    VulkanTexture& tex = m_textures[handle];
    if (tex.descriptorSet != VK_NULL_HANDLE)
        vkFreeDescriptorSets(m_ctx->device, m_ctx->descriptorPool, 1, &tex.descriptorSet);
    if (tex.sampler != VK_NULL_HANDLE) vkDestroySampler(m_ctx->device, tex.sampler, nullptr);
    if (tex.view != VK_NULL_HANDLE)    vkDestroyImageView(m_ctx->device, tex.view, nullptr);
    if (tex.image != VK_NULL_HANDLE)   vkDestroyImage(m_ctx->device, tex.image, nullptr);
    if (tex.memory != VK_NULL_HANDLE)  vkFreeMemory(m_ctx->device, tex.memory, nullptr);
    tex = VulkanTexture{};
}

uint32_t VulkanTextureManager::UploadRGBA(const uint8_t* pixels, uint32_t width, uint32_t height)
{
    VkDeviceSize imageSize = width * height * 4;

    // Staging buffer
    vkutil::Buffer staging = vkutil::CreateBuffer(*m_ctx, imageSize,
        VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
        VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
    vkutil::CopyToBuffer(*m_ctx, staging, pixels, imageSize);

    // Image
    VkImageCreateInfo imgCI{};
    imgCI.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
    imgCI.imageType = VK_IMAGE_TYPE_2D;
    imgCI.format = VK_FORMAT_R8G8B8A8_UNORM;
    imgCI.extent = {width, height, 1};
    imgCI.mipLevels = 1;
    imgCI.arrayLayers = 1;
    imgCI.samples = VK_SAMPLE_COUNT_1_BIT;
    imgCI.tiling = VK_IMAGE_TILING_OPTIMAL;
    imgCI.usage = VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;
    imgCI.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;

    VulkanTexture tex{};
    tex.width = width;
    tex.height = height;
    vkCreateImage(m_ctx->device, &imgCI, nullptr, &tex.image);

    VkMemoryRequirements memReqs;
    vkGetImageMemoryRequirements(m_ctx->device, tex.image, &memReqs);

    VkMemoryAllocateInfo allocInfo{};
    allocInfo.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
    allocInfo.allocationSize = memReqs.size;
    allocInfo.memoryTypeIndex = FindMemoryType(*m_ctx, memReqs.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
    vkAllocateMemory(m_ctx->device, &allocInfo, nullptr, &tex.memory);
    vkBindImageMemory(m_ctx->device, tex.image, tex.memory, 0);

    // Transition & copy
    VkCommandBuffer cmd = m_ctx->commandBuffers[0];
    VkCommandBufferBeginInfo beginInfo{};
    beginInfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
    beginInfo.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    vkResetCommandBuffer(cmd, 0);
    vkBeginCommandBuffer(cmd, &beginInfo);

    VkImageMemoryBarrier barrier{};
    barrier.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
    barrier.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    barrier.newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    barrier.image = tex.image;
    barrier.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    barrier.srcAccessMask = 0;
    barrier.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT,
                         0, 0, nullptr, 0, nullptr, 1, &barrier);

    VkBufferImageCopy region{};
    region.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
    region.imageExtent = {width, height, 1};
    vkCmdCopyBufferToImage(cmd, staging.buffer, tex.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &region);

    barrier.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    barrier.newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    barrier.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    barrier.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
    vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
                         0, 0, nullptr, 0, nullptr, 1, &barrier);

    vkEndCommandBuffer(cmd);
    VkSubmitInfo submitInfo{};
    submitInfo.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
    submitInfo.commandBufferCount = 1;
    submitInfo.pCommandBuffers = &cmd;
    vkQueueSubmit(m_ctx->graphicsQueue, 1, &submitInfo, VK_NULL_HANDLE);
    vkQueueWaitIdle(m_ctx->graphicsQueue);

    vkutil::DestroyBuffer(*m_ctx, staging);

    // View
    VkImageViewCreateInfo viewCI{};
    viewCI.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
    viewCI.image = tex.image;
    viewCI.viewType = VK_IMAGE_VIEW_TYPE_2D;
    viewCI.format = VK_FORMAT_R8G8B8A8_UNORM;
    viewCI.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    vkCreateImageView(m_ctx->device, &viewCI, nullptr, &tex.view);

    // Sampler
    VkSamplerCreateInfo samplerCI{};
    samplerCI.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO;
    samplerCI.magFilter = VK_FILTER_LINEAR;
    samplerCI.minFilter = VK_FILTER_LINEAR;
    samplerCI.addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    samplerCI.addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    samplerCI.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    vkCreateSampler(m_ctx->device, &samplerCI, nullptr, &tex.sampler);

    // Descriptor set
    VkDescriptorSetAllocateInfo dsAlloc{};
    dsAlloc.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
    dsAlloc.descriptorPool = m_ctx->descriptorPool;
    dsAlloc.descriptorSetCount = 1;
    dsAlloc.pSetLayouts = &m_textureLayout;
    vkAllocateDescriptorSets(m_ctx->device, &dsAlloc, &tex.descriptorSet);

    VkDescriptorImageInfo descImgInfo{};
    descImgInfo.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    descImgInfo.imageView = tex.view;
    descImgInfo.sampler = tex.sampler;

    VkWriteDescriptorSet write{};
    write.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    write.dstSet = tex.descriptorSet;
    write.dstBinding = 0;
    write.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    write.descriptorCount = 1;
    write.pImageInfo = &descImgInfo;
    vkUpdateDescriptorSets(m_ctx->device, 1, &write, 0, nullptr);

    uint32_t handle = static_cast<uint32_t>(m_textures.size());
    m_textures.push_back(tex);
    return handle;
}

void* VulkanTextureManager::GetImTextureID(uint32_t handle)
{
    VulkanTexture* tex = GetTexture(handle);
    if (!tex) return nullptr;
    return (void*)tex->descriptorSet;
}
