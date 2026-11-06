#pragma once

#include <pch.h>

#include <vulkan/vulkan.h>

#include <nvsdk_ngx.h>

// Vulkan doesn't allow compute work inside a render pass, but Unity hands its plugin events a secondary command
// buffer that continues one. Drivers are free to drop that work, Turnip does it silently. So an Evaluate made on such a
// command buffer is kept and runs on the primary command buffer that executes it, right after its render pass ends.
class VkDeferredEvaluate
{
  public:
    static void OnBeginCommandBuffer(VkCommandBuffer commandBuffer, const VkCommandBufferBeginInfo* pBeginInfo);

    // True when the command buffer continues a render pass of the primary command buffer executing it
    static bool ContinuesRenderPass(VkCommandBuffer commandBuffer);

    // Keeps a copy of the parameters, returns false when they can't be copied
    static bool Defer(VkCommandBuffer commandBuffer, const NVSDK_NGX_Handle* handle, NVSDK_NGX_Parameter* parameters);

    static void OnExecuteCommands(VkCommandBuffer primary, uint32_t count, const VkCommandBuffer* pCommandBuffers);
    static void OnRenderPassEnded(VkCommandBuffer primary, PFN_vkCmdPipelineBarrier cmdPipelineBarrier);

    // The command buffer's recording is gone, so are the evaluates it held
    static void Forget(VkCommandBuffer commandBuffer);
    static void ForgetFeature(unsigned int handleId);
};
