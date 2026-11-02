// Device-level Vulkan entry points used by the engine and the layer.
// Loaded through vkGetDeviceProcAddr so the same code runs both standalone
// (loader) and inside the layer (next link in the chain).
#pragma once
#include <vulkan/vulkan.h>

#define RP6FG_DEVICE_FNS(X)            \
    X(DestroyDevice)                   \
    X(GetDeviceQueue)                  \
    X(DeviceWaitIdle)                  \
    X(QueueSubmit)                     \
    X(QueueWaitIdle)                   \
    X(CreateImage)                     \
    X(DestroyImage)                    \
    X(GetImageMemoryRequirements)      \
    X(AllocateMemory)                  \
    X(FreeMemory)                      \
    X(BindImageMemory)                 \
    X(CreateBuffer)                    \
    X(DestroyBuffer)                   \
    X(GetBufferMemoryRequirements)     \
    X(BindBufferMemory)                \
    X(MapMemory)                       \
    X(UnmapMemory)                     \
    X(CreateImageView)                 \
    X(DestroyImageView)                \
    X(CreateSampler)                   \
    X(DestroySampler)                  \
    X(CreateShaderModule)              \
    X(DestroyShaderModule)             \
    X(CreateDescriptorSetLayout)       \
    X(DestroyDescriptorSetLayout)      \
    X(CreatePipelineLayout)            \
    X(DestroyPipelineLayout)           \
    X(CreateGraphicsPipelines)         \
    X(DestroyPipeline)                 \
    X(CreateDescriptorPool)            \
    X(DestroyDescriptorPool)           \
    X(AllocateDescriptorSets)          \
    X(UpdateDescriptorSets)            \
    X(CreateRenderPass)                \
    X(DestroyRenderPass)               \
    X(CreateFramebuffer)               \
    X(DestroyFramebuffer)              \
    X(CreateCommandPool)               \
    X(DestroyCommandPool)              \
    X(ResetCommandPool)                \
    X(AllocateCommandBuffers)          \
    X(FreeCommandBuffers)              \
    X(BeginCommandBuffer)              \
    X(EndCommandBuffer)                \
    X(ResetCommandBuffer)              \
    X(CreateSemaphore)                 \
    X(DestroySemaphore)                \
    X(CreateFence)                     \
    X(DestroyFence)                    \
    X(WaitForFences)                   \
    X(ResetFences)                     \
    X(GetFenceStatus)                  \
    X(CreateQueryPool)                 \
    X(DestroyQueryPool)                \
    X(GetQueryPoolResults)             \
    X(CmdBindPipeline)                 \
    X(CmdBindDescriptorSets)           \
    X(CmdPushConstants)                \
    X(CmdDraw)                         \
    X(CmdPipelineBarrier)              \
    X(CmdBeginRenderPass)              \
    X(CmdEndRenderPass)                \
    X(CmdCopyImage)                    \
    X(CmdClearColorImage)              \
    X(CmdCopyImageToBuffer)            \
    X(CmdCopyBufferToImage)            \
    X(CmdSetViewport)                  \
    X(CmdSetScissor)                   \
    X(CmdWriteTimestamp)               \
    X(CmdResetQueryPool)

struct DeviceFns {
#define RP6FG_DECL(name) PFN_vk##name name = nullptr;
    RP6FG_DEVICE_FNS(RP6FG_DECL)
#undef RP6FG_DECL

    // Returns false if any entry point is missing.
    bool load(PFN_vkGetDeviceProcAddr gdpa, VkDevice dev) {
        bool ok = true;
#define RP6FG_LOAD(name) \
    name = reinterpret_cast<PFN_vk##name>(gdpa(dev, "vk" #name)); ok &= name != nullptr;
        RP6FG_DEVICE_FNS(RP6FG_LOAD)
#undef RP6FG_LOAD
        return ok;
    }
};
