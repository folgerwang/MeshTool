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

// Camera-relative placement: a mesh is drawn at
// (vertex + mesh->translation - refPos) * scale.
struct MeshDrawFrame {
    double refPos[3] = { 0.0, 0.0, 0.0 };
    float  scale = 1.0f;

    // Segmented scenes: colour meshes by object class instead of texture, and
    // skip classes switched off (indexed by ObjectClass; nullptr = all shown).
    bool        classColors = false;
    bool        buildingColors = false;   // with classColors: one colour per building
    const bool* classVisible = nullptr;

    // Debug view of a clicked object: its meshes in selColor; with
    // isolateSelection everything else is hidden. selMesh set = that one mesh
    // (scene not segmented), else the meshes of selGroup with object_id selObject.
    const GroupMeshData* selGroup = nullptr;
    int32_t              selObject = -1;
    const MeshData*      selMesh = nullptr;
    bool                 isolateSelection = false;
    bool                 selActual = false;   // selection drawn as captured (textured), not highlighted
    float                selColor[3] = { 1.0f, 0.85f, 0.1f };

    bool IsSelected(const GroupMeshData* group, const MeshData* mesh) const;
};

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

    // Frees a mesh's GPU buffers before the MeshData is deleted. The caller
    // must make sure the GPU is idle (no frame in flight uses them).
    void ReleaseMesh(MeshData* mesh);

    // Draw functions
    void DrawBatchMeshes(VkCommandBuffer cmd, const std::vector<BatchMeshData*>& batches,
                         const float* viewProjMatrix, const MeshDrawFrame& frame, bool culling);
    // flatColor: draw untextured in this RGB colour instead (nullptr = textured).
    void DrawMesh(VkCommandBuffer cmd, MeshData* mesh, const float* viewProjMatrix, const MeshDrawFrame& frame,
                  const float* flatColor = nullptr);
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

    // 1x1 white texture bound when a mesh has none: the lit pipeline's fragment
    // shader always declares the sampler, so set 0 must be bound for every draw.
    uint32_t m_whiteTex = 0xFFFFFFFF;

    void UploadMesh(MeshData* mesh, MeshGPUData& gpu);
    void UploadQuadGeometry();
    void BindTextureOrWhite(VkCommandBuffer cmd, uint32_t texHandle);
    void DestroyMeshGPU(MeshGPUData& gpu);
};
