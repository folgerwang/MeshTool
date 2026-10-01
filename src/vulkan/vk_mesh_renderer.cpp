#include "vk_mesh_renderer.h"
#include "vk_pipeline.h"
#include "vk_renderer.h"
#include "vk_buffer.h"
#include "vk_texture_manager.h"
#include "meshdata.h"
#include "coremath.h"

#include <cstring>
#include <stdexcept>

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------

static uint32_t FindMemoryType(VkPhysicalDevice physDevice, uint32_t typeFilter, VkMemoryPropertyFlags properties)
{
    VkPhysicalDeviceMemoryProperties memProps;
    vkGetPhysicalDeviceMemoryProperties(physDevice, &memProps);
    for (uint32_t i = 0; i < memProps.memoryTypeCount; i++) {
        if ((typeFilter & (1 << i)) && (memProps.memoryTypes[i].propertyFlags & properties) == properties)
            return i;
    }
    throw std::runtime_error("Failed to find suitable memory type");
}

static void CreateBufferAndMemory(VulkanContext* ctx,
                                  VkDeviceSize size,
                                  VkBufferUsageFlags usage,
                                  VkMemoryPropertyFlags memProps,
                                  VkBuffer& outBuffer,
                                  VkDeviceMemory& outMemory)
{
    VkBufferCreateInfo bufInfo = {};
    bufInfo.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
    bufInfo.size = size;
    bufInfo.usage = usage;
    bufInfo.sharingMode = VK_SHARING_MODE_EXCLUSIVE;

    if (vkCreateBuffer(ctx->device, &bufInfo, nullptr, &outBuffer) != VK_SUCCESS)
        throw std::runtime_error("Failed to create buffer");

    VkMemoryRequirements memReqs;
    vkGetBufferMemoryRequirements(ctx->device, outBuffer, &memReqs);

    VkMemoryAllocateInfo allocInfo = {};
    allocInfo.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
    allocInfo.allocationSize = memReqs.size;
    allocInfo.memoryTypeIndex = FindMemoryType(ctx->physicalDevice, memReqs.memoryTypeBits, memProps);

    if (vkAllocateMemory(ctx->device, &allocInfo, nullptr, &outMemory) != VK_SUCCESS)
        throw std::runtime_error("Failed to allocate buffer memory");

    vkBindBufferMemory(ctx->device, outBuffer, outMemory, 0);
}

static void UploadBufferData(VulkanContext* ctx,
                             VkBuffer buffer,
                             VkDeviceMemory memory,
                             const void* data,
                             VkDeviceSize size)
{
    void* mapped = nullptr;
    vkMapMemory(ctx->device, memory, 0, size, 0, &mapped);
    memcpy(mapped, data, static_cast<size_t>(size));
    vkUnmapMemory(ctx->device, memory);
}

static void DestroyBufferPair(VkDevice device, VkBuffer& buffer, VkDeviceMemory& memory)
{
    if (buffer != VK_NULL_HANDLE) {
        vkDestroyBuffer(device, buffer, nullptr);
        buffer = VK_NULL_HANDLE;
    }
    if (memory != VK_NULL_HANDLE) {
        vkFreeMemory(device, memory, nullptr);
        memory = VK_NULL_HANDLE;
    }
}

// ---------------------------------------------------------------------------
// Public
// ---------------------------------------------------------------------------

void VulkanMeshRenderer::Init(VulkanContext* ctx, VulkanTextureManager* texMgr, VulkanPipelineManager* pipeMgr)
{
    m_ctx = ctx;
    m_texMgr = texMgr;
    m_pipeMgr = pipeMgr;

    const uint8_t white[4] = { 255, 255, 255, 255 };
    m_whiteTex = m_texMgr->UploadRGBA(white, 1, 1);
}

void VulkanMeshRenderer::DestroyMeshGPU(MeshGPUData& gpu)
{
    VkDevice dev = m_ctx->device;
    DestroyBufferPair(dev, gpu.vertexBuffer, gpu.vertexMemory);
    DestroyBufferPair(dev, gpu.uvBuffer, gpu.uvMemory);
    DestroyBufferPair(dev, gpu.colorBuffer, gpu.colorMemory);
    for (auto& dc : gpu.drawCalls)
        DestroyBufferPair(dev, dc.indexBuffer, dc.indexMemory);
    gpu.drawCalls.clear();
    gpu.uploaded = false;
}

void VulkanMeshRenderer::BindTextureOrWhite(VkCommandBuffer cmd, uint32_t texHandle)
{
    VulkanTexture* tex = (texHandle != 0xFFFFFFFF) ? m_texMgr->GetTexture(texHandle) : nullptr;
    if (!tex || tex->descriptorSet == VK_NULL_HANDLE)
        tex = m_texMgr->GetTexture(m_whiteTex);
    if (tex && tex->descriptorSet != VK_NULL_HANDLE)
        vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS,
                                m_pipeMgr->GetLayout(), 0, 1,
                                &tex->descriptorSet, 0, nullptr);
}

void VulkanMeshRenderer::Shutdown()
{
    if (!m_ctx) return;

    VkDevice dev = m_ctx->device;

    // Destroy all uploaded mesh GPU data
    for (auto& pair : m_meshGPU)
        DestroyMeshGPU(pair.second);
    m_meshGPU.clear();

    // Destroy quad geometry
    DestroyBufferPair(dev, m_quadVertBuffer, m_quadVertMemory);
    DestroyBufferPair(dev, m_quadIdxBuffer, m_quadIdxMemory);

    m_ctx = nullptr;
    m_texMgr = nullptr;
    m_pipeMgr = nullptr;
}

void VulkanMeshRenderer::EnsureUploaded(MeshData* mesh)
{
    if (!mesh) return;

    auto it = m_meshGPU.find(mesh);
    if (it != m_meshGPU.end() && it->second.uploaded)
        return;

    MeshGPUData& gpu = m_meshGPU[mesh];
    UploadMesh(mesh, gpu);
}

void VulkanMeshRenderer::ReleaseMesh(MeshData* mesh)
{
    auto it = m_meshGPU.find(mesh);
    if (it == m_meshGPU.end())
        return;
    DestroyMeshGPU(it->second);
    m_meshGPU.erase(it);
}

void VulkanMeshRenderer::DrawBatchMeshes(VkCommandBuffer cmd,
                                          const std::vector<BatchMeshData*>& batches,
                                          const float* viewProjMatrix,
                                          const MeshDrawFrame& frame,
                                          bool culling)
{
    for (const auto* batch : batches) {
        if (!batch) continue;
        for (const auto* group : batch->group_meshes) {
            if (!group) continue;
            for (auto* mesh : group->meshes) {
                if (!mesh) continue;
                EnsureUploaded(mesh);
                DrawMesh(cmd, mesh, viewProjMatrix, frame);
            }
        }
    }
}

void VulkanMeshRenderer::DrawMesh(VkCommandBuffer cmd, MeshData* mesh, const float* viewProjMatrix, const MeshDrawFrame& frame)
{
    if (!mesh) return;

    auto it = m_meshGPU.find(mesh);
    if (it == m_meshGPU.end() || !it->second.uploaded)
        return;

    const MeshGPUData& gpu = it->second;

    // Build push constants
    PushConstants pc = {};
    memcpy(pc.viewProjMatrix, viewProjMatrix, 16 * sizeof(float));

    // Model matrix (column-major): uniform scale, then the mesh's offset from
    // the reference position. The subtraction is done in double so large
    // world coordinates keep their precision.
    const float s = frame.scale;
    pc.modelMatrix[0]  = s;
    pc.modelMatrix[5]  = s;
    pc.modelMatrix[10] = s;
    pc.modelMatrix[12] = static_cast<float>((mesh->translation.x - frame.refPos[0]) * s);
    pc.modelMatrix[13] = static_cast<float>((mesh->translation.y - frame.refPos[1]) * s);
    pc.modelMatrix[14] = static_cast<float>((mesh->translation.z - frame.refPos[2]) * s);
    pc.modelMatrix[15] = 1.0f;

    // Set boxColor: w > 0.5 signals "use texture"
    bool hasTexture = (mesh->tex_id != 0xFFFFFFFF) && m_texMgr->GetTexture(mesh->tex_id);
    pc.boxColor[0] = 0.75f;
    pc.boxColor[1] = 0.75f;
    pc.boxColor[2] = 0.75f;
    pc.boxColor[3] = hasTexture ? 1.0f : 0.0f;

    vkCmdPushConstants(cmd, m_pipeMgr->GetLayout(),
                       VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT,
                       0, sizeof(PushConstants), &pc);

    BindTextureOrWhite(cmd, hasTexture ? mesh->tex_id : 0xFFFFFFFF);

    if (gpu.vertexBuffer == VK_NULL_HANDLE)
        return;

    // Bind vertex buffers (UploadMesh guarantees all three exist)
    VkBuffer vbs[3] = { gpu.vertexBuffer, gpu.uvBuffer, gpu.colorBuffer };
    VkDeviceSize offsets[3] = { 0, 0, 0 };
    vkCmdBindVertexBuffers(cmd, 0, 3, vbs, offsets);

    // Draw each draw call
    for (size_t i = 0; i < gpu.drawCalls.size(); ++i) {
        const MeshGPUData::DrawCallGPU& dcGPU = gpu.drawCalls[i];
        const DrawCallInfo& dcInfo = mesh->draw_call_list[i];

        if (!dcInfo.is_drawable() || dcGPU.indexCount == 0)
            continue;

        // Select pipeline based on primitive type
        PipelineType pipeType;
        if (dcInfo.get_primitive_type() == kGlTriangleStrip)
            pipeType = PIPELINE_BASIC_LIGHT_STRIP;
        else
            pipeType = PIPELINE_BASIC_LIGHT;

        vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, m_pipeMgr->GetPipeline(pipeType));

        // Bind index buffer and draw
        vkCmdBindIndexBuffer(cmd, dcGPU.indexBuffer, 0, dcGPU.indexType);
        vkCmdDrawIndexed(cmd, dcGPU.indexCount, 1, 0, 0, 0);
    }
}

void VulkanMeshRenderer::DrawQuad(VkCommandBuffer cmd, float x, float y, float w, float h, uint32_t texHandle)
{
    // Lazily upload quad geometry
    if (m_quadVertBuffer == VK_NULL_HANDLE)
        UploadQuadGeometry();

    // Bind screen quad pipeline
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, m_pipeMgr->GetPipeline(PIPELINE_SCREEN_QUAD));

    // Push constants with screen position
    PushConstants pc = {};
    // Identity matrices (not used by screen quad shader but must be present)
    pc.modelMatrix[0] = 1.0f; pc.modelMatrix[5] = 1.0f;
    pc.modelMatrix[10] = 1.0f; pc.modelMatrix[15] = 1.0f;
    pc.viewProjMatrix[0] = 1.0f; pc.viewProjMatrix[5] = 1.0f;
    pc.viewProjMatrix[10] = 1.0f; pc.viewProjMatrix[15] = 1.0f;
    pc.screenPosition[0] = x;
    pc.screenPosition[1] = y;
    pc.screenPosition[2] = w;
    pc.screenPosition[3] = h;

    vkCmdPushConstants(cmd, m_pipeMgr->GetLayout(),
                       VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT,
                       0, sizeof(PushConstants), &pc);

    // Bind texture
    VulkanTexture* tex = m_texMgr->GetTexture(texHandle);
    if (tex && tex->descriptorSet != VK_NULL_HANDLE) {
        vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS,
                                m_pipeMgr->GetLayout(), 0, 1,
                                &tex->descriptorSet, 0, nullptr);
    }

    // Bind quad vertex buffer and draw
    VkDeviceSize offset = 0;
    vkCmdBindVertexBuffers(cmd, 0, 1, &m_quadVertBuffer, &offset);
    vkCmdBindIndexBuffer(cmd, m_quadIdxBuffer, 0, VK_INDEX_TYPE_UINT16);
    vkCmdDrawIndexed(cmd, 6, 1, 0, 0, 0);
}

// ---------------------------------------------------------------------------
// Private
// ---------------------------------------------------------------------------

void VulkanMeshRenderer::UploadMesh(MeshData* mesh, MeshGPUData& gpu)
{
    VkMemoryPropertyFlags hostVisible = VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT;

    // --- Vertex positions (binding 0) ---
    if (mesh->vertex_list && mesh->num_vertex > 0) {
        VkDeviceSize size = static_cast<VkDeviceSize>(mesh->num_vertex) * sizeof(core::vec3f);
        CreateBufferAndMemory(m_ctx, size,
                              VK_BUFFER_USAGE_VERTEX_BUFFER_BIT,
                              hostVisible,
                              gpu.vertexBuffer, gpu.vertexMemory);
        UploadBufferData(m_ctx, gpu.vertexBuffer, gpu.vertexMemory, mesh->vertex_list.get(), size);
    }

    // --- UV coordinates (binding 1) and vertex colors (binding 2) ---
    // The lit pipelines declare all three bindings, so a mesh without UVs or
    // colors still gets a zero-filled buffer for each.
    if (mesh->num_vertex > 0) {
        VkDeviceSize uvSize = static_cast<VkDeviceSize>(mesh->num_vertex) * sizeof(core::vec2f);
        std::vector<uint8_t> zeros;
        if (!mesh->uv_list)
            zeros.assign(static_cast<size_t>(uvSize), 0);
        CreateBufferAndMemory(m_ctx, uvSize,
                              VK_BUFFER_USAGE_VERTEX_BUFFER_BIT,
                              hostVisible,
                              gpu.uvBuffer, gpu.uvMemory);
        UploadBufferData(m_ctx, gpu.uvBuffer, gpu.uvMemory,
                         mesh->uv_list ? static_cast<const void*>(mesh->uv_list.get()) : zeros.data(), uvSize);

        VkDeviceSize colorSize = static_cast<VkDeviceSize>(mesh->num_vertex) * sizeof(uint32_t);
        if (!mesh->color_list)
            zeros.assign(static_cast<size_t>(colorSize), 0);
        CreateBufferAndMemory(m_ctx, colorSize,
                              VK_BUFFER_USAGE_VERTEX_BUFFER_BIT,
                              hostVisible,
                              gpu.colorBuffer, gpu.colorMemory);
        UploadBufferData(m_ctx, gpu.colorBuffer, gpu.colorMemory,
                         mesh->color_list ? static_cast<const void*>(mesh->color_list.get()) : zeros.data(), colorSize);
    }

    // --- Index buffers (one per draw call) ---
    gpu.drawCalls.resize(mesh->draw_call_list.size());
    for (size_t i = 0; i < mesh->draw_call_list.size(); ++i) {
        const DrawCallInfo& dc = mesh->draw_call_list[i];
        MeshGPUData::DrawCallGPU& dcGPU = gpu.drawCalls[i];

        int indexCount = dc.get_index_count();
        if (indexCount <= 0)
            continue;

        dcGPU.indexCount = static_cast<uint32_t>(indexCount);

        // Determine index type and size
        uint32_t glIndexType = dc.get_index_type();
        if (glIndexType == kGlUInt) {
            dcGPU.indexType = VK_INDEX_TYPE_UINT32;
        } else {
            dcGPU.indexType = VK_INDEX_TYPE_UINT16;
        }

        uint32_t indexSize = (dcGPU.indexType == VK_INDEX_TYPE_UINT32) ? 4 : 2;
        VkDeviceSize bufSize = static_cast<VkDeviceSize>(indexCount) * indexSize;

        CreateBufferAndMemory(m_ctx, bufSize,
                              VK_BUFFER_USAGE_INDEX_BUFFER_BIT,
                              hostVisible,
                              dcGPU.indexBuffer, dcGPU.indexMemory);
        UploadBufferData(m_ctx, dcGPU.indexBuffer, dcGPU.indexMemory, dc.get_index_buffer(), bufSize);
    }

    gpu.uploaded = true;
}

void VulkanMeshRenderer::UploadQuadGeometry()
{
    // Full-screen quad: 4 vertices (vec2 positions that double as UVs)
    // (0,0), (1,0), (1,1), (0,1)
    float vertices[] = {
        0.0f, 0.0f,
        1.0f, 0.0f,
        1.0f, 1.0f,
        0.0f, 1.0f,
    };

    uint16_t indices[] = {
        0, 1, 2,
        0, 2, 3,
    };

    VkMemoryPropertyFlags hostVisible = VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT;

    // Vertex buffer
    VkDeviceSize vertSize = sizeof(vertices);
    CreateBufferAndMemory(m_ctx, vertSize,
                          VK_BUFFER_USAGE_VERTEX_BUFFER_BIT,
                          hostVisible,
                          m_quadVertBuffer, m_quadVertMemory);
    UploadBufferData(m_ctx, m_quadVertBuffer, m_quadVertMemory, vertices, vertSize);

    // Index buffer
    VkDeviceSize idxSize = sizeof(indices);
    CreateBufferAndMemory(m_ctx, idxSize,
                          VK_BUFFER_USAGE_INDEX_BUFFER_BIT,
                          hostVisible,
                          m_quadIdxBuffer, m_quadIdxMemory);
    UploadBufferData(m_ctx, m_quadIdxBuffer, m_quadIdxMemory, indices, idxSize);
}
