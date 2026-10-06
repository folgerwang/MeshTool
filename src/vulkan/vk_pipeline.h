#pragma once
#include <vulkan/vulkan.h>
#include <string>
#include <vector>

struct VulkanContext;

enum PipelineType {
    PIPELINE_BASIC_LIGHT = 0,
    PIPELINE_BASIC_NOLIGHT,
    PIPELINE_SCREEN_QUAD,
    PIPELINE_BASIC_LIGHT_STRIP,   // Same as BASIC_LIGHT but with triangle strip topology
    PIPELINE_GLASS,               // Translucent, reflective facades: blended, no depth writes
    PIPELINE_COUNT
};

struct PushConstants {
    float viewProjMatrix[16];
    float modelMatrix[16];
    float boxColor[4];
    float screenPosition[4];
    int32_t textureIndex[4];   // x = slot in the bindless texture array
};

class VulkanPipelineManager {
public:
    void Init(VulkanContext* ctx, VkDescriptorSetLayout texLayout);
    void Shutdown();

    VkPipeline GetPipeline(PipelineType type) { return m_pipelines[type]; }
    VkPipelineLayout GetLayout() { return m_pipelineLayout; }

private:
    VulkanContext* m_ctx = nullptr;
    VkPipelineLayout m_pipelineLayout = VK_NULL_HANDLE;
    VkPipeline m_pipelines[PIPELINE_COUNT] = {};

    VkShaderModule CreateShaderModule(const std::vector<char>& code);
    void CreatePipeline(PipelineType type, const std::string& vertPath, const std::string& fragPath,
                        VkPrimitiveTopology topology, bool enableBlend, bool enableDepth,
                        bool depthWrite = true);
    std::vector<char> ReadSPIRV(const std::string& filename);
};
