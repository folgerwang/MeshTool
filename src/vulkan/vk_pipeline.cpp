#include "vk_pipeline.h"
#include "vk_renderer.h"
#include <fstream>
#include <stdexcept>
#include <cassert>
#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

// ---------------------------------------------------------------------------
// Public
// ---------------------------------------------------------------------------

void VulkanPipelineManager::Init(VulkanContext* ctx, VkDescriptorSetLayout texLayout)
{
    m_ctx = ctx;

    // --- Pipeline layout (push constants + the bindless texture set) ---
    VkPushConstantRange pushRange = {};
    pushRange.stageFlags = VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT;
    pushRange.offset = 0;
    pushRange.size = sizeof(PushConstants); // 176 bytes (2 mat4 + 3 vec4)

    VkPipelineLayoutCreateInfo layoutInfo = {};
    layoutInfo.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
    layoutInfo.setLayoutCount = 1;
    layoutInfo.pSetLayouts = &texLayout;
    layoutInfo.pushConstantRangeCount = 1;
    layoutInfo.pPushConstantRanges = &pushRange;

    if (vkCreatePipelineLayout(m_ctx->device, &layoutInfo, nullptr, &m_pipelineLayout) != VK_SUCCESS)
        throw std::runtime_error("Failed to create pipeline layout");

    // --- Create pipelines ---
    CreatePipeline(PIPELINE_BASIC_LIGHT,
                   "shaders/basiclight.vert.spv",
                   "shaders/basiclight.frag.spv",
                   VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST,
                   /*enableBlend=*/false,
                   /*enableDepth=*/true);

    CreatePipeline(PIPELINE_BASIC_NOLIGHT,
                   "shaders/basicnolight.vert.spv",
                   "shaders/basicnolight.frag.spv",
                   VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST,
                   /*enableBlend=*/false,
                   /*enableDepth=*/true);

    CreatePipeline(PIPELINE_SCREEN_QUAD,
                   "shaders/screenquad.vert.spv",
                   "shaders/screenquad.frag.spv",
                   VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST,
                   /*enableBlend=*/true,
                   /*enableDepth=*/false);

    CreatePipeline(PIPELINE_BASIC_LIGHT_STRIP,
                   "shaders/basiclight.vert.spv",
                   "shaders/basiclight.frag.spv",
                   VK_PRIMITIVE_TOPOLOGY_TRIANGLE_STRIP,
                   /*enableBlend=*/false,
                   /*enableDepth=*/true);

    CreatePipeline(PIPELINE_BOX_VISIBLE, "shaders/selectionbox.vert.spv", "shaders/selectionbox.frag.spv",
                   VK_PRIMITIVE_TOPOLOGY_LINE_LIST, false, true, false);
    CreatePipeline(PIPELINE_BOX_HIDDEN, "shaders/selectionbox.vert.spv", "shaders/selectionbox.frag.spv",
                   VK_PRIMITIVE_TOPOLOGY_LINE_LIST, false, true, false);

    // Drawn after everything opaque, back to front: tests depth so buildings
    // in front hide it, but does not write it, so what is behind still shows.
    CreatePipeline(PIPELINE_GLASS,
                   "shaders/basiclight.vert.spv",
                   "shaders/glass.frag.spv",
                   VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST,
                   /*enableBlend=*/true,
                   /*enableDepth=*/true,
                   /*depthWrite=*/false);
}

void VulkanPipelineManager::Shutdown()
{
    if (!m_ctx) return;

    for (int i = 0; i < PIPELINE_COUNT; ++i) {
        if (m_pipelines[i] != VK_NULL_HANDLE) {
            vkDestroyPipeline(m_ctx->device, m_pipelines[i], nullptr);
            m_pipelines[i] = VK_NULL_HANDLE;
        }
    }

    if (m_pipelineLayout != VK_NULL_HANDLE) {
        vkDestroyPipelineLayout(m_ctx->device, m_pipelineLayout, nullptr);
        m_pipelineLayout = VK_NULL_HANDLE;
    }

    m_ctx = nullptr;
}

// ---------------------------------------------------------------------------
// Private
// ---------------------------------------------------------------------------

// .spv files are generated into <build>/shaders and copied next to the exe.
// Look there first, then fall back to paths relative to the working directory
// (run.bat starts MeshTool from the repo root).
static std::string ResolveSPIRVPath(const std::string& filename)
{
    std::vector<std::string> candidates;
#ifdef _WIN32
    char exePath[MAX_PATH] = {};
    if (GetModuleFileNameA(nullptr, exePath, MAX_PATH))
    {
        std::string dir(exePath);
        size_t slash = dir.find_last_of("\\/");
        if (slash != std::string::npos)
            candidates.push_back(dir.substr(0, slash + 1) + filename);
    }
#endif
    candidates.push_back(filename);
    candidates.push_back("build/" + filename);

    for (const auto& c : candidates)
    {
        std::ifstream probe(c, std::ios::binary);
        if (probe.is_open())
            return c;
    }
    return filename;
}

std::vector<char> VulkanPipelineManager::ReadSPIRV(const std::string& requested)
{
    const std::string filename = ResolveSPIRVPath(requested);
    std::ifstream file(filename, std::ios::ate | std::ios::binary);
    if (!file.is_open())
        throw std::runtime_error("Failed to open SPIR-V file: " + requested +
                                 " (looked next to the exe, in the working dir and in build/)");

    size_t fileSize = static_cast<size_t>(file.tellg());
    std::vector<char> buffer(fileSize);
    file.seekg(0);
    file.read(buffer.data(), fileSize);
    return buffer;
}

VkShaderModule VulkanPipelineManager::CreateShaderModule(const std::vector<char>& code)
{
    VkShaderModuleCreateInfo createInfo = {};
    createInfo.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;
    createInfo.codeSize = code.size();
    createInfo.pCode = reinterpret_cast<const uint32_t*>(code.data());

    VkShaderModule shaderModule;
    if (vkCreateShaderModule(m_ctx->device, &createInfo, nullptr, &shaderModule) != VK_SUCCESS)
        throw std::runtime_error("Failed to create shader module");

    return shaderModule;
}

void VulkanPipelineManager::CreatePipeline(PipelineType type,
                                           const std::string& vertPath,
                                           const std::string& fragPath,
                                           VkPrimitiveTopology topology,
                                           bool enableBlend,
                                           bool enableDepth,
                                           bool depthWrite)
{
    // --- Shader stages ---
    auto vertCode = ReadSPIRV(vertPath);
    auto fragCode = ReadSPIRV(fragPath);

    VkShaderModule vertModule = CreateShaderModule(vertCode);
    VkShaderModule fragModule = CreateShaderModule(fragCode);

    VkPipelineShaderStageCreateInfo vertStageInfo = {};
    vertStageInfo.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    vertStageInfo.stage = VK_SHADER_STAGE_VERTEX_BIT;
    vertStageInfo.module = vertModule;
    vertStageInfo.pName = "main";

    VkPipelineShaderStageCreateInfo fragStageInfo = {};
    fragStageInfo.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    fragStageInfo.stage = VK_SHADER_STAGE_FRAGMENT_BIT;
    fragStageInfo.module = fragModule;
    fragStageInfo.pName = "main";

    VkPipelineShaderStageCreateInfo shaderStages[] = { vertStageInfo, fragStageInfo };

    // --- Vertex input ---
    // Binding 0: position vec3f, stride 12
    // Binding 1: texcoord vec2f, stride 8
    // Binding 2: color vec4ub (R8G8B8A8_UNORM), stride 4
    VkVertexInputBindingDescription bindings[3] = {};
    bindings[0].binding = 0;
    bindings[0].stride = 12; // sizeof(vec3f)
    bindings[0].inputRate = VK_VERTEX_INPUT_RATE_VERTEX;
    bindings[1].binding = 1;
    bindings[1].stride = 8;  // sizeof(vec2f)
    bindings[1].inputRate = VK_VERTEX_INPUT_RATE_VERTEX;
    bindings[2].binding = 2;
    bindings[2].stride = 4;  // sizeof(uint32_t) RGBA8
    bindings[2].inputRate = VK_VERTEX_INPUT_RATE_VERTEX;

    VkVertexInputAttributeDescription attrs[3] = {};
    // location 0 = position (vec3, binding 0)
    attrs[0].binding = 0;
    attrs[0].location = 0;
    attrs[0].format = VK_FORMAT_R32G32B32_SFLOAT;
    attrs[0].offset = 0;
    // location 1 = texcoord (vec2, binding 1)
    attrs[1].binding = 1;
    attrs[1].location = 1;
    attrs[1].format = VK_FORMAT_R32G32_SFLOAT;
    attrs[1].offset = 0;
    // location 2 = color (rgba8 unorm, binding 2)
    attrs[2].binding = 2;
    attrs[2].location = 2;
    attrs[2].format = VK_FORMAT_R8G8B8A8_UNORM;
    attrs[2].offset = 0;

    // Decide how many bindings/attributes this pipeline actually uses
    uint32_t bindingCount = 3;
    uint32_t attrCount = 3;

    if (type == PIPELINE_SCREEN_QUAD) {
        // Screen quad only uses position (vec2) at binding 0
        bindings[0].stride = 8; // sizeof(vec2f)
        attrs[0].format = VK_FORMAT_R32G32_SFLOAT;
        bindingCount = 1;
        attrCount = 1;
    } else if (type == PIPELINE_BASIC_NOLIGHT) {
        // No-light only uses position at binding 0
        bindingCount = 1;
        attrCount = 1;
    }

    VkPipelineVertexInputStateCreateInfo vertexInputInfo = {};
    vertexInputInfo.sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO;
    vertexInputInfo.vertexBindingDescriptionCount = bindingCount;
    vertexInputInfo.pVertexBindingDescriptions = bindings;
    vertexInputInfo.vertexAttributeDescriptionCount = attrCount;
    vertexInputInfo.pVertexAttributeDescriptions = attrs;

    // --- Input assembly ---
    VkPipelineInputAssemblyStateCreateInfo inputAssembly = {};
    inputAssembly.sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO;
    inputAssembly.topology = topology;
    inputAssembly.primitiveRestartEnable = (topology == VK_PRIMITIVE_TOPOLOGY_TRIANGLE_STRIP) ? VK_TRUE : VK_FALSE;

    // --- Viewport / scissor (dynamic) ---
    VkPipelineViewportStateCreateInfo viewportState = {};
    viewportState.sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO;
    viewportState.viewportCount = 1;
    viewportState.scissorCount = 1;

    // --- Rasterizer ---
    VkPipelineRasterizationStateCreateInfo rasterizer = {};
    rasterizer.sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO;
    rasterizer.depthClampEnable = VK_FALSE;
    rasterizer.rasterizerDiscardEnable = VK_FALSE;
    rasterizer.polygonMode = VK_POLYGON_MODE_FILL;
    rasterizer.lineWidth = 1.0f;
    // No culling: captured meshes keep GL's winding, which Vulkan's Y-down clip
    // space flips, and the planar camera may view them from either side.
    rasterizer.cullMode = VK_CULL_MODE_NONE;
    rasterizer.frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE;
    rasterizer.depthBiasEnable = VK_FALSE;

    // --- Multisampling ---
    VkPipelineMultisampleStateCreateInfo multisampling = {};
    multisampling.sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO;
    multisampling.sampleShadingEnable = VK_FALSE;
    multisampling.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;

    // --- Depth / stencil ---
    VkPipelineDepthStencilStateCreateInfo depthStencil = {};
    depthStencil.sType = VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO;
    depthStencil.depthTestEnable = enableDepth ? VK_TRUE : VK_FALSE;
    depthStencil.depthWriteEnable = enableDepth && depthWrite ? VK_TRUE : VK_FALSE;
    depthStencil.depthCompareOp = type == PIPELINE_BOX_HIDDEN ? VK_COMPARE_OP_GREATER :
        type == PIPELINE_BOX_VISIBLE ? VK_COMPARE_OP_LESS_OR_EQUAL : VK_COMPARE_OP_LESS;
    depthStencil.depthBoundsTestEnable = VK_FALSE;
    depthStencil.stencilTestEnable = VK_FALSE;

    // --- Color blending ---
    VkPipelineColorBlendAttachmentState colorBlendAttachment = {};
    colorBlendAttachment.colorWriteMask =
        VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT |
        VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT;

    if (enableBlend) {
        colorBlendAttachment.blendEnable = VK_TRUE;
        colorBlendAttachment.srcColorBlendFactor = VK_BLEND_FACTOR_SRC_ALPHA;
        colorBlendAttachment.dstColorBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
        colorBlendAttachment.colorBlendOp = VK_BLEND_OP_ADD;
        colorBlendAttachment.srcAlphaBlendFactor = VK_BLEND_FACTOR_ONE;
        colorBlendAttachment.dstAlphaBlendFactor = VK_BLEND_FACTOR_ZERO;
        colorBlendAttachment.alphaBlendOp = VK_BLEND_OP_ADD;
    } else {
        colorBlendAttachment.blendEnable = VK_FALSE;
    }

    VkPipelineColorBlendStateCreateInfo colorBlending = {};
    colorBlending.sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO;
    colorBlending.logicOpEnable = VK_FALSE;
    colorBlending.attachmentCount = 1;
    colorBlending.pAttachments = &colorBlendAttachment;

    // --- Dynamic state ---
    VkDynamicState dynamicStates[] = {
        VK_DYNAMIC_STATE_VIEWPORT,
        VK_DYNAMIC_STATE_SCISSOR
    };

    VkPipelineDynamicStateCreateInfo dynamicState = {};
    dynamicState.sType = VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO;
    dynamicState.dynamicStateCount = 2;
    dynamicState.pDynamicStates = dynamicStates;

    // --- Graphics pipeline ---
    if (type == PIPELINE_BOX_VISIBLE || type == PIPELINE_BOX_HIDDEN)
    {
        vertexInputInfo.vertexBindingDescriptionCount = 0;
        vertexInputInfo.vertexAttributeDescriptionCount = 0;
    }

    VkGraphicsPipelineCreateInfo pipelineInfo = {};
    pipelineInfo.sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO;
    pipelineInfo.stageCount = 2;
    pipelineInfo.pStages = shaderStages;
    pipelineInfo.pVertexInputState = &vertexInputInfo;
    pipelineInfo.pInputAssemblyState = &inputAssembly;
    pipelineInfo.pViewportState = &viewportState;
    pipelineInfo.pRasterizationState = &rasterizer;
    pipelineInfo.pMultisampleState = &multisampling;
    pipelineInfo.pDepthStencilState = &depthStencil;
    pipelineInfo.pColorBlendState = &colorBlending;
    pipelineInfo.pDynamicState = &dynamicState;
    pipelineInfo.layout = m_pipelineLayout;
    pipelineInfo.renderPass = m_ctx->renderPass;
    pipelineInfo.subpass = 0;
    pipelineInfo.basePipelineHandle = VK_NULL_HANDLE;

    if (vkCreateGraphicsPipelines(m_ctx->device, VK_NULL_HANDLE, 1, &pipelineInfo, nullptr, &m_pipelines[type]) != VK_SUCCESS)
        throw std::runtime_error("Failed to create graphics pipeline");

    // Shader modules can be destroyed after pipeline creation
    vkDestroyShaderModule(m_ctx->device, fragModule, nullptr);
    vkDestroyShaderModule(m_ctx->device, vertModule, nullptr);
}
