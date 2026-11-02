// Frame generation engine: luma pyramid, hardware block-match motion
// estimation, per-pixel vector selection and compositing. Records into
// command buffers supplied by the caller; owns only its internal images.
#pragma once
#include "vkfns.hpp"

#include <functional>
#include <vector>

namespace rp6fg {

struct EngineConfig {
    uint32_t width = 0;
    uint32_t height = 0;
    VkFormat colorFormat = VK_FORMAT_UNDEFINED;
    bool linearIn = false;            // colour views return linear values (sRGB formats)
    VkImageLayout targetFinalLayout = VK_IMAGE_LAYOUT_GENERAL;
    float lambda = 0.004f;            // vector smoothness penalty (SAD units per level pixel)
    int coarseRadius = 4;             // full search radius on the coarsest level
    bool captureStats = false;
    float fallbackLo = 1e9f, fallbackHi = 2e9f;  // select cost (4x4 SAD) where compose fades to a plain blend        // keep pipeline statistics / ir3 disassembly (bench)
};

struct Image {
    VkImage image = VK_NULL_HANDLE;
    VkDeviceMemory mem = VK_NULL_HANDLE;
    VkImageView view = VK_NULL_HANDLE;
    VkFramebuffer fb = VK_NULL_HANDLE;
    uint32_t w = 0, h = 0;
};

class Engine {
public:
    static constexpr int kLevels = 4;

    bool init(VkDevice dev, const DeviceFns* fns,
              const VkPhysicalDeviceMemoryProperties& memProps, const EngineConfig& cfg);
    void destroy();

    int addTarget(VkImageView view);

    // Stateless recording so command buffers can be recorded once and replayed.
    // `parity` selects the "current" colour/pyramid/field set (alternate per
    // real frame); the other set holds the previous frame. Per real frame:
    // recordCopy (grab the app's image; it can be released right after),
    // recordAnalyze, then recordGenerate once per generated frame.
    void recordInit(VkCommandBuffer cmd);
    void recordCopy(VkCommandBuffer cmd, VkImage src, VkImageLayout srcLayout, int parity);
    void recordAnalyze(VkCommandBuffer cmd, int parity);
    void recordGenerate(VkCommandBuffer cmd, int target, int parity, float t, int mode = 0);

    const EngineConfig& config() const { return m_cfg; }
    VkImage colorImage(int parity) const { return m_color[parity].image; }
    std::vector<std::pair<const char*, VkPipeline>> pipelines() const {
        return {{"luma", m_pLuma.pipeline}, {"down", m_pDown.pipeline}, {"me", m_pMe.pipeline}, {"me_coarse", m_pMeCoarse.pipeline},
                {"select", m_pSelect.pipeline}, {"compose", m_pCompose.pipeline}};
    }

    // Called after each pass is recorded (used for GPU timestamps).
    std::function<void(VkCommandBuffer, const char*)> onPass;

private:
    struct Pipe {
        VkDescriptorSetLayout setLayout = VK_NULL_HANDLE;
        VkPipelineLayout layout = VK_NULL_HANDLE;
        VkPipeline pipeline = VK_NULL_HANDLE;
    };

    bool createImage(Image& img, uint32_t w, uint32_t h, VkFormat fmt, VkImageUsageFlags usage);
    void destroyImage(Image& img);
    VkRenderPass createRenderPass(const std::vector<VkFormat>& fmts, VkImageLayout initial, VkImageLayout final_);
    VkFramebuffer createFramebuffer(VkRenderPass rp, const std::vector<VkImageView>& views, uint32_t w, uint32_t h);
    bool createPipe(Pipe& p, const uint32_t* spv, size_t spvSize, int samplers, VkRenderPass rp, int attachments,
                    std::vector<int32_t> spec = {});
    VkDescriptorSet allocSet(const Pipe& p, const std::vector<std::pair<VkImageView, VkSampler>>& images);
    void beginPass(VkCommandBuffer cmd, VkRenderPass rp, VkFramebuffer fb, uint32_t w, uint32_t h,
                   const Pipe& p, VkDescriptorSet set, const void* pc, uint32_t pcSize);
    void endPass(VkCommandBuffer cmd, const char* name);
    void barrier(VkCommandBuffer cmd);

    VkDevice m_dev = VK_NULL_HANDLE;
    const DeviceFns* vk = nullptr;
    VkPhysicalDeviceMemoryProperties m_memProps{};
    EngineConfig m_cfg;

    uint32_t m_lw[kLevels]{}, m_lh[kLevels]{};  // pyramid level sizes
    uint32_t m_gw[kLevels]{}, m_gh[kLevels]{};  // block grid sizes
    uint32_t m_bs[kLevels]{};                    // ME block size per level

    Image m_luma[2][kLevels];
    Image m_fieldF[kLevels], m_fieldB[kLevels];  // levels >= 1
    Image m_field0F[2], m_field0B[2];            // level 0, double-buffered for temporal candidates
    VkFramebuffer m_meFb[2][kLevels]{};
    Image m_sel;
    Image m_color[2];  // copies of the app's frames: [parity] = current, [1 - parity] = previous

    VkSampler m_linear = VK_NULL_HANDLE, m_nearest = VK_NULL_HANDLE, m_blockMatch = VK_NULL_HANDLE;
    VkRenderPass m_rpR8 = VK_NULL_HANDLE, m_rpField2 = VK_NULL_HANDLE, m_rpSel = VK_NULL_HANDLE, m_rpOut = VK_NULL_HANDLE;
    VkShaderModule m_vert = VK_NULL_HANDLE;
    Pipe m_pLuma, m_pDown, m_pMe, m_pMeCoarse, m_pSelect, m_pCompose;
    VkDescriptorPool m_pool = VK_NULL_HANDLE;

    VkDescriptorSet m_dsDown[2][kLevels]{};
    VkDescriptorSet m_dsMe[2][kLevels]{};
    VkDescriptorSet m_dsSelect[2]{};
    VkDescriptorSet m_dsLuma[2]{}, m_dsCompose[2]{};
    std::vector<VkFramebuffer> m_targets;

};

} // namespace rp6fg
