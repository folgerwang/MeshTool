#pragma once
#include <vulkan/vulkan.h>
#include <cstdint>
#include <vector>
#include <map>

struct VulkanContext;
class VulkanTextureManager;
class VulkanPipelineManager;
struct MeshData;
struct GroupMeshData;
struct BatchMeshData;
struct DrawCallInfo;

namespace core {
    template<class T> class matrix4;
    typedef matrix4<float> matrix4f;
}

// Per-mesh GPU resources
struct MeshGPUData {
    VkBuffer vertexBuffer = VK_NULL_HANDLE;
    VkDeviceMemory vertexMemory = VK_NULL_HANDLE;
    VkBuffer uvBuffer = VK_NULL_HANDLE;
    VkDeviceMemory uvMemory = VK_NULL_HANDLE;
    VkBuffer colorBuffer = VK_NULL_HANDLE;
    VkDeviceMemory colorMemory = VK_NULL_HANDLE;

    struct DrawCallGPU {
        VkBuffer indexBuffer = VK_NULL_HANDLE;
        VkDeviceMemory indexMemory = VK_NULL_HANDLE;
        uint32_t indexCount = 0;
        VkIndexType indexType = VK_INDEX_TYPE_UINT32;
    };
    std::vector<DrawCallGPU> drawCalls;
    bool uploaded = false;
};

class VulkanMeshRenderer {
public:
    void Init(VulkanContext* ctx, VulkanTextureManager* texMgr, VulkanPipelineManager* pipeMgr);
    void Shutdown();

    // Upload mesh data to GPU (lazy - called on first draw)
    void EnsureUploaded(MeshData* mesh);

    // Draw functions
    void DrawBatchMeshes(VkCommandBuffer cmd, const std::vector<BatchMeshData*>& batches,
                         const float* viewProjMatrix, bool culling);
    void DrawMesh(VkCommandBuffer cmd, MeshData* mesh, const float* viewProjMatrix);
    void DrawQuad(VkCommandBuffer cmd, float x, float y, float w, float h, uint32_t texHandle);

private:
    VulkanContext* m_ctx = nullptr;
    VulkanTextureManager* m_texMgr = nullptr;
    VulkanPipelineManager* m_pipeMgr = nullptr;

    std::map<MeshData*, MeshGPUData> m_meshGPU;

    // Screen quad geometry (uploaded once)
    VkBuffer m_quadVertBuffer = VK_NULL_HANDLE;
    VkDeviceMemory m_quadVertMemory = VK_NULL_HANDLE;
    VkBuffer m_quadIdxBuffer = VK_NULL_HANDLE;
    VkDeviceMemory m_quadIdxMemory = VK_NULL_HANDLE;

    void UploadMesh(MeshData* mesh, MeshGPUData& gpu);
    void UploadQuadGeometry();
};
