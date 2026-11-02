#include "engine.hpp"

#include <cmath>
#include <cstdint>
#include <cstdio>

#include "shaders/compose.frag.h"
#include "shaders/down.frag.h"
#include "shaders/fullscreen.vert.h"
#include "shaders/luma.frag.h"
#include "shaders/me.frag.h"
#include "shaders/select.frag.h"

namespace rp6fg {

namespace {

struct LumaPC { float invDst[2]; float linearIn; };
struct DownPC { float invDst[2]; };
struct MePC {
    int32_t levelSize[2];
    int32_t parentGrid[2];
    int32_t tempGrid[2];
    float scale;
    float lambda;
    int32_t parentShift;
    int32_t flags;
};
struct SelectPC { float fullSize[2]; int32_t grid[2]; float t; int32_t pad; int32_t lumaSize[2]; };
struct ComposePC { float invFull[2]; float t; int32_t mode; float fallback[2]; };

constexpr VkImageUsageFlags kLumaUsage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT |
    VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_SAMPLE_BLOCK_MATCH_BIT_QCOM |
    VK_IMAGE_USAGE_TRANSFER_DST_BIT;
constexpr VkImageUsageFlags kFieldUsage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT |
    VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;

} // namespace

bool Engine::createImage(Image& img, uint32_t w, uint32_t h, VkFormat fmt, VkImageUsageFlags usage) {
    VkImageCreateInfo ici{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
    ici.imageType = VK_IMAGE_TYPE_2D;
    ici.format = fmt;
    ici.extent = {w, h, 1};
    ici.mipLevels = 1;
    ici.arrayLayers = 1;
    ici.samples = VK_SAMPLE_COUNT_1_BIT;
    ici.tiling = VK_IMAGE_TILING_OPTIMAL;
    ici.usage = usage;
    ici.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    if (vk->CreateImage(m_dev, &ici, nullptr, &img.image)) return false;

    VkMemoryRequirements req;
    vk->GetImageMemoryRequirements(m_dev, img.image, &req);
    uint32_t type = UINT32_MAX;
    for (uint32_t i = 0; i < m_memProps.memoryTypeCount; i++) {
        if ((req.memoryTypeBits & (1u << i)) &&
            (m_memProps.memoryTypes[i].propertyFlags & VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT)) {
            type = i;
            break;
        }
    }
    if (type == UINT32_MAX) return false;
    VkMemoryAllocateInfo mai{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
    mai.allocationSize = req.size;
    mai.memoryTypeIndex = type;
    if (vk->AllocateMemory(m_dev, &mai, nullptr, &img.mem)) return false;
    if (vk->BindImageMemory(m_dev, img.image, img.mem, 0)) return false;

    VkImageViewCreateInfo vci{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
    vci.image = img.image;
    vci.viewType = VK_IMAGE_VIEW_TYPE_2D;
    vci.format = fmt;
    vci.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    if (vk->CreateImageView(m_dev, &vci, nullptr, &img.view)) return false;
    img.w = w;
    img.h = h;
    return true;
}

void Engine::destroyImage(Image& img) {
    if (img.fb) vk->DestroyFramebuffer(m_dev, img.fb, nullptr);
    if (img.view) vk->DestroyImageView(m_dev, img.view, nullptr);
    if (img.image) vk->DestroyImage(m_dev, img.image, nullptr);
    if (img.mem) vk->FreeMemory(m_dev, img.mem, nullptr);
    img = Image{};
}

VkRenderPass Engine::createRenderPass(const std::vector<VkFormat>& fmts, VkImageLayout initial, VkImageLayout final_) {
    std::vector<VkAttachmentDescription> att(fmts.size());
    std::vector<VkAttachmentReference> refs(fmts.size());
    for (size_t i = 0; i < fmts.size(); i++) {
        att[i] = {};
        att[i].format = fmts[i];
        att[i].samples = VK_SAMPLE_COUNT_1_BIT;
        att[i].loadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;  // every pass overwrites the whole target
        att[i].storeOp = VK_ATTACHMENT_STORE_OP_STORE;
        att[i].stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
        att[i].stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
        att[i].initialLayout = initial;
        att[i].finalLayout = final_;
        // Internal targets stay GENERAL in the subpass too: no per-pass transitions.
        refs[i] = {static_cast<uint32_t>(i),
                   final_ == VK_IMAGE_LAYOUT_GENERAL ? VK_IMAGE_LAYOUT_GENERAL : VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL};
    }
    VkSubpassDescription sub{};
    sub.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
    sub.colorAttachmentCount = static_cast<uint32_t>(refs.size());
    sub.pColorAttachments = refs.data();
    // Make the initial layout transition wait at COLOR_ATTACHMENT_OUTPUT, the
    // stage a swapchain acquire semaphore is waited on.
    VkSubpassDependency dep{};
    dep.srcSubpass = VK_SUBPASS_EXTERNAL;
    dep.dstSubpass = 0;
    dep.srcStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
    dep.dstStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
    dep.dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
    VkRenderPassCreateInfo rci{VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO};
    rci.attachmentCount = static_cast<uint32_t>(att.size());
    rci.pAttachments = att.data();
    rci.subpassCount = 1;
    rci.pSubpasses = &sub;
    rci.dependencyCount = 1;
    rci.pDependencies = &dep;
    VkRenderPass rp = VK_NULL_HANDLE;
    vk->CreateRenderPass(m_dev, &rci, nullptr, &rp);
    return rp;
}

VkFramebuffer Engine::createFramebuffer(VkRenderPass rp, const std::vector<VkImageView>& views, uint32_t w, uint32_t h) {
    VkFramebufferCreateInfo fci{VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO};
    fci.renderPass = rp;
    fci.attachmentCount = static_cast<uint32_t>(views.size());
    fci.pAttachments = views.data();
    fci.width = w;
    fci.height = h;
    fci.layers = 1;
    VkFramebuffer fb = VK_NULL_HANDLE;
    vk->CreateFramebuffer(m_dev, &fci, nullptr, &fb);
    return fb;
}

bool Engine::createPipe(Pipe& p, const uint32_t* spv, size_t spvSize, int samplers, VkRenderPass rp, int attachments,
                        std::vector<int32_t> spec) {
    std::vector<VkDescriptorSetLayoutBinding> b(samplers);
    for (int i = 0; i < samplers; i++)
        b[i] = {static_cast<uint32_t>(i), VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1,
                VK_SHADER_STAGE_FRAGMENT_BIT, nullptr};
    VkDescriptorSetLayoutCreateInfo dci{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
    dci.bindingCount = static_cast<uint32_t>(b.size());
    dci.pBindings = b.data();
    if (vk->CreateDescriptorSetLayout(m_dev, &dci, nullptr, &p.setLayout)) return false;

    VkPushConstantRange pcr{VK_SHADER_STAGE_FRAGMENT_BIT, 0, 64};
    VkPipelineLayoutCreateInfo lci{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
    lci.setLayoutCount = 1;
    lci.pSetLayouts = &p.setLayout;
    lci.pushConstantRangeCount = 1;
    lci.pPushConstantRanges = &pcr;
    if (vk->CreatePipelineLayout(m_dev, &lci, nullptr, &p.layout)) return false;

    VkShaderModuleCreateInfo smci{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
    smci.codeSize = spvSize;
    smci.pCode = spv;
    VkShaderModule frag;
    if (vk->CreateShaderModule(m_dev, &smci, nullptr, &frag)) return false;

    VkPipelineShaderStageCreateInfo st[2]{};
    st[0].sType = st[1].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    st[0].stage = VK_SHADER_STAGE_VERTEX_BIT;
    st[0].module = m_vert;
    st[0].pName = "main";
    st[1].stage = VK_SHADER_STAGE_FRAGMENT_BIT;
    st[1].module = frag;
    st[1].pName = "main";
    std::vector<VkSpecializationMapEntry> sme(spec.size());
    for (size_t i = 0; i < spec.size(); i++)
        sme[i] = {static_cast<uint32_t>(i), static_cast<uint32_t>(i * sizeof(int32_t)), sizeof(int32_t)};
    VkSpecializationInfo si{static_cast<uint32_t>(spec.size()), sme.data(), spec.size() * sizeof(int32_t), spec.data()};
    if (!spec.empty()) st[1].pSpecializationInfo = &si;

    VkPipelineVertexInputStateCreateInfo vi{VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO};
    VkPipelineInputAssemblyStateCreateInfo ia{VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO};
    ia.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
    VkPipelineViewportStateCreateInfo vp{VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO};
    vp.viewportCount = 1;
    vp.scissorCount = 1;
    VkPipelineRasterizationStateCreateInfo rs{VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO};
    rs.polygonMode = VK_POLYGON_MODE_FILL;
    rs.cullMode = VK_CULL_MODE_NONE;
    rs.lineWidth = 1.0f;
    VkPipelineMultisampleStateCreateInfo ms{VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO};
    ms.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;
    std::vector<VkPipelineColorBlendAttachmentState> cba(attachments);
    for (auto& a : cba) {
        a = {};
        a.colorWriteMask = 0xF;
    }
    VkPipelineColorBlendStateCreateInfo cb{VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO};
    cb.attachmentCount = static_cast<uint32_t>(cba.size());
    cb.pAttachments = cba.data();
    VkDynamicState dyn[] = {VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR};
    VkPipelineDynamicStateCreateInfo ds{VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO};
    ds.dynamicStateCount = 2;
    ds.pDynamicStates = dyn;

    VkGraphicsPipelineCreateInfo gci{VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO};
    if (m_cfg.captureStats)
        gci.flags = VK_PIPELINE_CREATE_CAPTURE_STATISTICS_BIT_KHR |
                    VK_PIPELINE_CREATE_CAPTURE_INTERNAL_REPRESENTATIONS_BIT_KHR;
    gci.stageCount = 2;
    gci.pStages = st;
    gci.pVertexInputState = &vi;
    gci.pInputAssemblyState = &ia;
    gci.pViewportState = &vp;
    gci.pRasterizationState = &rs;
    gci.pMultisampleState = &ms;
    gci.pColorBlendState = &cb;
    gci.pDynamicState = &ds;
    gci.layout = p.layout;
    gci.renderPass = rp;
    VkResult r = vk->CreateGraphicsPipelines(m_dev, VK_NULL_HANDLE, 1, &gci, nullptr, &p.pipeline);
    vk->DestroyShaderModule(m_dev, frag, nullptr);
    return r == VK_SUCCESS;
}

VkDescriptorSet Engine::allocSet(const Pipe& p, const std::vector<std::pair<VkImageView, VkSampler>>& images) {
    VkDescriptorSetAllocateInfo ai{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
    ai.descriptorPool = m_pool;
    ai.descriptorSetCount = 1;
    ai.pSetLayouts = &p.setLayout;
    VkDescriptorSet set = VK_NULL_HANDLE;
    if (vk->AllocateDescriptorSets(m_dev, &ai, &set)) return VK_NULL_HANDLE;
    std::vector<VkDescriptorImageInfo> info(images.size());
    std::vector<VkWriteDescriptorSet> w(images.size());
    for (size_t i = 0; i < images.size(); i++) {
        info[i] = {images[i].second, images[i].first, VK_IMAGE_LAYOUT_GENERAL};
        w[i] = {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
        w[i].dstSet = set;
        w[i].dstBinding = static_cast<uint32_t>(i);
        w[i].descriptorCount = 1;
        w[i].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        w[i].pImageInfo = &info[i];
    }
    vk->UpdateDescriptorSets(m_dev, static_cast<uint32_t>(w.size()), w.data(), 0, nullptr);
    return set;
}

bool Engine::init(VkDevice dev, const DeviceFns* fns,
                  const VkPhysicalDeviceMemoryProperties& memProps, const EngineConfig& cfg) {
    m_dev = dev;
    vk = fns;
    m_memProps = memProps;
    m_cfg = cfg;

    uint32_t w = (cfg.width + 1) / 2, h = (cfg.height + 1) / 2;
    for (int k = 0; k < kLevels; k++) {
        m_lw[k] = w;
        m_lh[k] = h;
        // The coarsest level uses 4x4 blocks: same reach, 4x the fragments to
        // keep every shader core busy on what is otherwise a latency-bound pass.
        m_bs[k] = k == kLevels - 1 ? 4 : 8;
        m_gw[k] = (w + m_bs[k] - 1) / m_bs[k];
        m_gh[k] = (h + m_bs[k] - 1) / m_bs[k];
        w = (w + 1) / 2;
        h = (h + 1) / 2;
    }
    if (m_lw[kLevels - 1] < m_bs[kLevels - 1] || m_lh[kLevels - 1] < m_bs[kLevels - 1]) return false;

    VkSamplerCreateInfo sci{VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO};
    sci.magFilter = sci.minFilter = VK_FILTER_LINEAR;
    sci.mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST;
    sci.addressModeU = sci.addressModeV = sci.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    if (vk->CreateSampler(m_dev, &sci, nullptr, &m_linear)) return false;
    sci.magFilter = sci.minFilter = VK_FILTER_NEAREST;
    if (vk->CreateSampler(m_dev, &sci, nullptr, &m_nearest)) return false;
    sci.flags = VK_SAMPLER_CREATE_IMAGE_PROCESSING_BIT_QCOM;
    sci.unnormalizedCoordinates = VK_TRUE;
    if (vk->CreateSampler(m_dev, &sci, nullptr, &m_blockMatch)) return false;

    for (int p = 0; p < 2; p++)
        for (int k = 0; k < kLevels; k++)
            if (!createImage(m_luma[p][k], m_lw[k], m_lh[k], VK_FORMAT_R8_UNORM, kLumaUsage)) return false;
    for (int k = 1; k < kLevels; k++) {
        if (!createImage(m_fieldF[k], m_gw[k], m_gh[k], VK_FORMAT_R16G16B16A16_SFLOAT, kFieldUsage)) return false;
        if (!createImage(m_fieldB[k], m_gw[k], m_gh[k], VK_FORMAT_R16G16B16A16_SFLOAT, kFieldUsage)) return false;
    }
    for (int p = 0; p < 2; p++) {
        if (!createImage(m_field0F[p], m_gw[0], m_gh[0], VK_FORMAT_R16G16B16A16_SFLOAT, kFieldUsage)) return false;
        if (!createImage(m_field0B[p], m_gw[0], m_gh[0], VK_FORMAT_R16G16B16A16_SFLOAT, kFieldUsage)) return false;
    }
    if (!createImage(m_sel, m_lw[0], m_lh[0], VK_FORMAT_R16G16B16A16_SFLOAT, kFieldUsage)) return false;
    for (int p = 0; p < 2; p++)
        if (!createImage(m_color[p], cfg.width, cfg.height, cfg.colorFormat,
                         VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT)) return false;

    m_rpR8 = createRenderPass({VK_FORMAT_R8_UNORM}, VK_IMAGE_LAYOUT_GENERAL, VK_IMAGE_LAYOUT_GENERAL);
    m_rpField2 = createRenderPass({VK_FORMAT_R16G16B16A16_SFLOAT, VK_FORMAT_R16G16B16A16_SFLOAT},
                                  VK_IMAGE_LAYOUT_GENERAL, VK_IMAGE_LAYOUT_GENERAL);
    m_rpSel = createRenderPass({VK_FORMAT_R16G16B16A16_SFLOAT}, VK_IMAGE_LAYOUT_GENERAL, VK_IMAGE_LAYOUT_GENERAL);
    m_rpOut = createRenderPass({cfg.colorFormat}, VK_IMAGE_LAYOUT_UNDEFINED, cfg.targetFinalLayout);
    if (!m_rpR8 || !m_rpField2 || !m_rpSel || !m_rpOut) return false;

    for (int p = 0; p < 2; p++)
        for (int k = 0; k < kLevels; k++)
            m_luma[p][k].fb = createFramebuffer(m_rpR8, {m_luma[p][k].view}, m_lw[k], m_lh[k]);
    for (int k = 1; k < kLevels; k++)
        m_meFb[0][k] = m_meFb[1][k] =
            createFramebuffer(m_rpField2, {m_fieldF[k].view, m_fieldB[k].view}, m_gw[k], m_gh[k]);
    for (int p = 0; p < 2; p++)
        m_meFb[p][0] = createFramebuffer(m_rpField2, {m_field0F[p].view, m_field0B[p].view}, m_gw[0], m_gh[0]);
    m_sel.fb = createFramebuffer(m_rpSel, {m_sel.view}, m_lw[0], m_lh[0]);

    VkShaderModuleCreateInfo smci{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
    smci.codeSize = sizeof(fullscreen_vert);
    smci.pCode = fullscreen_vert;
    if (vk->CreateShaderModule(m_dev, &smci, nullptr, &m_vert)) return false;

    if (!createPipe(m_pLuma, luma_frag, sizeof(luma_frag), 1, m_rpR8, 1)) return false;
    if (!createPipe(m_pDown, down_frag, sizeof(down_frag), 1, m_rpR8, 1)) return false;
    // spec constants: {radius, block size, candidate count}. Three candidates at
    // L0 measured -2.1 dB for -10% me0 time, so every level keeps six.
    if (!createPipe(m_pMe, me_frag, sizeof(me_frag), 6, m_rpField2, 2, {1, 8, 6})) return false;
    if (!createPipe(m_pMeCoarse, me_frag, sizeof(me_frag), 6, m_rpField2, 2,
                    {cfg.coarseRadius, static_cast<int32_t>(m_bs[kLevels - 1]), 6})) return false;
    if (!createPipe(m_pSelect, select_frag, sizeof(select_frag), 4, m_rpSel, 1)) return false;
    if (!createPipe(m_pCompose, compose_frag, sizeof(compose_frag), 3, m_rpOut, 1)) return false;

    VkDescriptorPoolSize ps{VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1024};
    VkDescriptorPoolCreateInfo pci{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
    pci.maxSets = 256;
    pci.poolSizeCount = 1;
    pci.pPoolSizes = &ps;
    if (vk->CreateDescriptorPool(m_dev, &pci, nullptr, &m_pool)) return false;

    for (int p = 0; p < 2; p++) {
        const int q = 1 - p;
        for (int k = 1; k < kLevels; k++)
            m_dsDown[p][k] = allocSet(m_pDown, {{m_luma[p][k - 1].view, m_linear}});
        for (int k = 0; k < kLevels; k++) {
            // The coarsest level has no parent; bind any valid image, the shader ignores it.
            VkImageView pf = k + 1 < kLevels ? m_fieldF[k + 1].view : m_field0F[q].view;
            VkImageView pb = k + 1 < kLevels ? m_fieldB[k + 1].view : m_field0B[q].view;
            m_dsMe[p][k] = allocSet(m_pMe, {{m_luma[q][k].view, m_blockMatch},
                                            {m_luma[p][k].view, m_blockMatch},
                                            {pf, m_nearest},
                                            {pb, m_nearest},
                                            {m_field0F[q].view, m_nearest},
                                            {m_field0B[q].view, m_nearest}});
        }
        m_dsSelect[p] = allocSet(m_pSelect, {{m_luma[q][0].view, m_blockMatch},
                                             {m_luma[p][0].view, m_blockMatch},
                                             {m_field0F[p].view, m_nearest},
                                             {m_field0B[p].view, m_nearest}});
        m_dsLuma[p] = allocSet(m_pLuma, {{m_color[p].view, m_linear}});
        m_dsCompose[p] = allocSet(m_pCompose, {{m_color[q].view, m_linear}, {m_color[p].view, m_linear},
                                               {m_sel.view, m_nearest}});
    }
    return true;
}

int Engine::addTarget(VkImageView view) {
    m_targets.push_back(createFramebuffer(m_rpOut, {view}, m_cfg.width, m_cfg.height));
    return static_cast<int>(m_targets.size()) - 1;
}

void Engine::destroy() {
    if (!vk) return;
    for (auto fb : m_targets) vk->DestroyFramebuffer(m_dev, fb, nullptr);
    m_targets.clear();
    for (int k = 1; k < kLevels; k++)
        if (m_meFb[0][k]) vk->DestroyFramebuffer(m_dev, m_meFb[0][k], nullptr);
    for (int p = 0; p < 2; p++)
        if (m_meFb[p][0]) vk->DestroyFramebuffer(m_dev, m_meFb[p][0], nullptr);
    for (int p = 0; p < 2; p++)
        for (int k = 0; k < kLevels; k++) destroyImage(m_luma[p][k]);
    for (int k = 1; k < kLevels; k++) {
        destroyImage(m_fieldF[k]);
        destroyImage(m_fieldB[k]);
    }
    for (int p = 0; p < 2; p++) {
        destroyImage(m_field0F[p]);
        destroyImage(m_field0B[p]);
    }
    destroyImage(m_sel);
    destroyImage(m_color[0]);
    destroyImage(m_color[1]);
    for (Pipe* p : {&m_pLuma, &m_pDown, &m_pMe, &m_pMeCoarse, &m_pSelect, &m_pCompose}) {
        if (p->pipeline) vk->DestroyPipeline(m_dev, p->pipeline, nullptr);
        if (p->layout) vk->DestroyPipelineLayout(m_dev, p->layout, nullptr);
        if (p->setLayout) vk->DestroyDescriptorSetLayout(m_dev, p->setLayout, nullptr);
        *p = Pipe{};
    }
    if (m_pool) vk->DestroyDescriptorPool(m_dev, m_pool, nullptr);
    if (m_vert) vk->DestroyShaderModule(m_dev, m_vert, nullptr);
    for (VkRenderPass rp : {m_rpR8, m_rpField2, m_rpSel, m_rpOut})
        if (rp) vk->DestroyRenderPass(m_dev, rp, nullptr);
    for (VkSampler s : {m_linear, m_nearest, m_blockMatch})
        if (s) vk->DestroySampler(m_dev, s, nullptr);
    vk = nullptr;
}

void Engine::barrier(VkCommandBuffer cmd) {
    VkMemoryBarrier mb{VK_STRUCTURE_TYPE_MEMORY_BARRIER};
    mb.srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT | VK_ACCESS_TRANSFER_WRITE_BIT;
    mb.dstAccessMask = VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT |
                       VK_ACCESS_TRANSFER_READ_BIT | VK_ACCESS_TRANSFER_WRITE_BIT;
    VkPipelineStageFlags stages = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT |
                                  VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT | VK_PIPELINE_STAGE_TRANSFER_BIT;
    vk->CmdPipelineBarrier(cmd, stages, stages, 0, 1, &mb, 0, nullptr, 0, nullptr);
}

void Engine::beginPass(VkCommandBuffer cmd, VkRenderPass rp, VkFramebuffer fb, uint32_t w, uint32_t h,
                       const Pipe& p, VkDescriptorSet set, const void* pc, uint32_t pcSize) {
    VkRenderPassBeginInfo rbi{VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO};
    rbi.renderPass = rp;
    rbi.framebuffer = fb;
    rbi.renderArea = {{0, 0}, {w, h}};
    vk->CmdBeginRenderPass(cmd, &rbi, VK_SUBPASS_CONTENTS_INLINE);
    VkViewport vp{0.0f, 0.0f, static_cast<float>(w), static_cast<float>(h), 0.0f, 1.0f};
    VkRect2D sc{{0, 0}, {w, h}};
    vk->CmdSetViewport(cmd, 0, 1, &vp);
    vk->CmdSetScissor(cmd, 0, 1, &sc);
    vk->CmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, p.pipeline);
    vk->CmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, p.layout, 0, 1, &set, 0, nullptr);
    vk->CmdPushConstants(cmd, p.layout, VK_SHADER_STAGE_FRAGMENT_BIT, 0, pcSize, pc);
    vk->CmdDraw(cmd, 3, 1, 0, 0);
}

void Engine::endPass(VkCommandBuffer cmd, const char* name) {
    vk->CmdEndRenderPass(cmd);
    if (onPass) onPass(cmd, name);
}

void Engine::recordInit(VkCommandBuffer cmd) {
    std::vector<VkImageMemoryBarrier> b;
    auto add = [&](const Image& img) {
        VkImageMemoryBarrier ib{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
        ib.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
        ib.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        ib.newLayout = VK_IMAGE_LAYOUT_GENERAL;
        ib.srcQueueFamilyIndex = ib.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        ib.image = img.image;
        ib.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
        b.push_back(ib);
    };
    for (int p = 0; p < 2; p++)
        for (int k = 0; k < kLevels; k++) add(m_luma[p][k]);
    for (int k = 1; k < kLevels; k++) {
        add(m_fieldF[k]);
        add(m_fieldB[k]);
    }
    for (int p = 0; p < 2; p++) {
        add(m_field0F[p]);
        add(m_field0B[p]);
    }
    add(m_sel);
    add(m_color[0]);
    add(m_color[1]);
    vk->CmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0,
                           0, nullptr, 0, nullptr, static_cast<uint32_t>(b.size()), b.data());
    // Zero pyramids and level-0 fields: read (as previous frame / temporal
    // candidates) before they are first written.
    VkClearColorValue zero{};
    VkImageSubresourceRange range{VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    for (int p = 0; p < 2; p++)
        for (int k = 0; k < kLevels; k++)
            vk->CmdClearColorImage(cmd, m_luma[p][k].image, VK_IMAGE_LAYOUT_GENERAL, &zero, 1, &range);
    for (int p = 0; p < 2; p++) {
        vk->CmdClearColorImage(cmd, m_field0F[p].image, VK_IMAGE_LAYOUT_GENERAL, &zero, 1, &range);
        vk->CmdClearColorImage(cmd, m_field0B[p].image, VK_IMAGE_LAYOUT_GENERAL, &zero, 1, &range);
    }
    barrier(cmd);
}

void Engine::recordCopy(VkCommandBuffer cmd, VkImage src, VkImageLayout srcLayout, int parity) {
    barrier(cmd);
    VkImageCopy region{};
    region.srcSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
    region.dstSubresource = region.srcSubresource;
    region.extent = {m_cfg.width, m_cfg.height, 1};
    vk->CmdCopyImage(cmd, src, srcLayout, m_color[parity].image, VK_IMAGE_LAYOUT_GENERAL, 1, &region);
    if (onPass) onPass(cmd, "copy");
}

void Engine::recordAnalyze(VkCommandBuffer cmd, int parity) {
    const int p = parity;
    barrier(cmd);

    LumaPC lpc{{1.0f / m_lw[0], 1.0f / m_lh[0]}, m_cfg.linearIn ? 1.0f : 0.0f};
    beginPass(cmd, m_rpR8, m_luma[p][0].fb, m_lw[0], m_lh[0], m_pLuma, m_dsLuma[p], &lpc, sizeof(lpc));
    endPass(cmd, "luma");
    for (int k = 1; k < kLevels; k++) {
        barrier(cmd);
        DownPC dpc{{1.0f / m_lw[k], 1.0f / m_lh[k]}};
        beginPass(cmd, m_rpR8, m_luma[p][k].fb, m_lw[k], m_lh[k], m_pDown, m_dsDown[p][k], &dpc, sizeof(dpc));
        endPass(cmd, "down");
    }
    // Motion estimation always runs; on the very first frame the previous
    // pyramid is the zero-cleared one and the result is simply never used.
    for (int k = kLevels - 1; k >= 0; k--) {
        barrier(cmd);
        MePC mpc{};
        mpc.parentGrid[0] = mpc.parentGrid[1] = 1;  // keeps the (unused) parent fetch in bounds
        mpc.levelSize[0] = static_cast<int32_t>(m_lw[k]);
        mpc.levelSize[1] = static_cast<int32_t>(m_lh[k]);
        if (k + 1 < kLevels) {
            mpc.parentGrid[0] = static_cast<int32_t>(m_gw[k + 1]);
            mpc.parentGrid[1] = static_cast<int32_t>(m_gh[k + 1]);
            mpc.parentShift = 2 * m_bs[k + 1] / m_bs[k] == 2 ? 1 : 0;
        }
        mpc.tempGrid[0] = static_cast<int32_t>(m_gw[0]);
        mpc.tempGrid[1] = static_cast<int32_t>(m_gh[0]);
        mpc.scale = static_cast<float>(2 << k);
        mpc.lambda = m_cfg.lambda;
        mpc.flags = (k + 1 < kLevels ? 1 : 0) | 2 | (k == 0 ? 4 : 0);
        static const char* names[kLevels] = {"me0", "me1", "me2", "me3"};
        const Pipe& pipe = k == kLevels - 1 ? m_pMeCoarse : m_pMe;
        beginPass(cmd, m_rpField2, m_meFb[p][k], m_gw[k], m_gh[k], pipe, m_dsMe[p][k], &mpc, sizeof(mpc));
        endPass(cmd, names[k]);
    }
}

void Engine::recordGenerate(VkCommandBuffer cmd, int target, int parity, float t, int mode) {
    const int p = parity;
    barrier(cmd);
    SelectPC spc{{static_cast<float>(m_cfg.width), static_cast<float>(m_cfg.height)},
                 {static_cast<int32_t>(m_gw[0]), static_cast<int32_t>(m_gh[0])}, t, 0,
                 {static_cast<int32_t>(m_lw[0]), static_cast<int32_t>(m_lh[0])}};
    beginPass(cmd, m_rpSel, m_sel.fb, m_lw[0], m_lh[0], m_pSelect, m_dsSelect[p], &spc, sizeof(spc));
    endPass(cmd, "select");
    barrier(cmd);
    ComposePC cpc{{1.0f / m_cfg.width, 1.0f / m_cfg.height}, t, mode, {m_cfg.fallbackLo, m_cfg.fallbackHi}};
    beginPass(cmd, m_rpOut, m_targets[target], m_cfg.width, m_cfg.height, m_pCompose, m_dsCompose[p], &cpc, sizeof(cpc));
    endPass(cmd, "compose");
}

} // namespace rp6fg
