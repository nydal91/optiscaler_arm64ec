#include <pch.h>

#include "Vulkan_DeferredEvaluate.h"

#include <NVNGX_Parameter.h>
#include <nvsdk_ngx_vk.h>

#include <algorithm>
#include <atomic>
#include <list>
#include <memory>
#include <mutex>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace
{
struct DeferredCall
{
    NVSDK_NGX_Handle handle {};
    std::unique_ptr<NVNGX_Parameters> parameters;
    // The caller's resource structs don't have to outlive the call
    std::list<NVSDK_NGX_Resource_VK> resources;
};

using DeferredCalls = std::vector<DeferredCall>;

std::mutex mutex;
std::unordered_set<VkCommandBuffer> renderPassContinuing;
std::unordered_map<VkCommandBuffer, DeferredCalls> recorded; // By secondary, until it's executed
std::unordered_map<VkCommandBuffer, DeferredCalls> executed; // By primary, until its render pass ends

// Render pass hooks are hot, this keeps them from taking the lock when nothing is waiting
std::atomic<size_t> waiting = 0;

const char* const resourceKeys[] = {
    NVSDK_NGX_Parameter_Color,           NVSDK_NGX_Parameter_MotionVectors,
    NVSDK_NGX_Parameter_Depth,           NVSDK_NGX_Parameter_Output,
    NVSDK_NGX_Parameter_ExposureTexture, NVSDK_NGX_Parameter_DLSS_Input_Bias_Current_Color_Mask,
    "FSR.transparencyAndComposition",    "FSR.reactive"
};

// mutex is held
void Discard(VkCommandBuffer commandBuffer)
{
    for (auto map : { &recorded, &executed })
    {
        if (auto it = map->find(commandBuffer); it != map->end())
        {
            waiting -= it->second.size();
            map->erase(it);
        }
    }
}
} // namespace

void VkDeferredEvaluate::OnBeginCommandBuffer(VkCommandBuffer commandBuffer, const VkCommandBufferBeginInfo* pBeginInfo)
{
    std::scoped_lock lock(mutex);

    if (pBeginInfo != nullptr && (pBeginInfo->flags & VK_COMMAND_BUFFER_USAGE_RENDER_PASS_CONTINUE_BIT) != 0)
        renderPassContinuing.insert(commandBuffer);
    else
        renderPassContinuing.erase(commandBuffer);

    Discard(commandBuffer);
}

bool VkDeferredEvaluate::ContinuesRenderPass(VkCommandBuffer commandBuffer)
{
    std::scoped_lock lock(mutex);
    return renderPassContinuing.contains(commandBuffer);
}

bool VkDeferredEvaluate::Defer(VkCommandBuffer commandBuffer, const NVSDK_NGX_Handle* handle,
                               NVSDK_NGX_Parameter* parameters)
{
    // Only OptiScaler's own tables carry this
    int ownTable = 0;
    if (parameters == nullptr || parameters->Get("OptiScaler", &ownTable) != NVSDK_NGX_Result_Success)
    {
        LOG_ERROR("Parameters of {:X} can't be copied", (size_t) commandBuffer);
        return false;
    }

    auto source = static_cast<NVNGX_Parameters*>(parameters);

    DeferredCall call {};
    call.handle = *handle;
    call.parameters = std::make_unique<NVNGX_Parameters>(source->Api, true);
    call.parameters->CopyValuesFrom(*source);

    for (auto key : resourceKeys)
    {
        NVSDK_NGX_Resource_VK* resource = nullptr;
        if (source->Get(key, (void**) &resource) != NVSDK_NGX_Result_Success || resource == nullptr)
            continue;

        call.parameters->Set(key, (void*) &call.resources.emplace_back(*resource));
    }

    std::scoped_lock lock(mutex);
    recorded[commandBuffer].push_back(std::move(call));
    waiting++;

    return true;
}

void VkDeferredEvaluate::OnExecuteCommands(VkCommandBuffer primary, uint32_t count,
                                           const VkCommandBuffer* pCommandBuffers)
{
    if (waiting == 0)
        return;

    std::scoped_lock lock(mutex);

    for (uint32_t i = 0; i < count; i++)
    {
        auto it = recorded.find(pCommandBuffers[i]);
        if (it == recorded.end())
            continue;

        auto& calls = executed[primary];
        std::move(it->second.begin(), it->second.end(), std::back_inserter(calls));
        recorded.erase(it);
    }
}

void VkDeferredEvaluate::OnRenderPassEnded(VkCommandBuffer primary, PFN_vkCmdPipelineBarrier cmdPipelineBarrier)
{
    if (waiting == 0)
        return;

    DeferredCalls calls;
    {
        std::scoped_lock lock(mutex);

        auto it = executed.find(primary);
        if (it == executed.end())
            return;

        calls = std::move(it->second);
        executed.erase(it);
        waiting -= calls.size();
    }

    // Render pass dependencies don't have to cover compute, and whatever reads the output was recorded without seeing
    // the upscaler's writes
    VkMemoryBarrier barrier { VK_STRUCTURE_TYPE_MEMORY_BARRIER };
    barrier.srcAccessMask = VK_ACCESS_MEMORY_WRITE_BIT;
    barrier.dstAccessMask = VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT;

    const auto fullBarrier = [&]
    {
        cmdPipelineBarrier(primary, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0, 1,
                           &barrier, 0, nullptr, 0, nullptr);
    };

    fullBarrier();

    for (auto& call : calls)
    {
        auto result = NVSDK_NGX_VULKAN_EvaluateFeature(primary, &call.handle, call.parameters.get(), nullptr);
        if (result != NVSDK_NGX_Result_Success)
            LOG_ERROR("Deferred evaluate of {} failed: {:X}", call.handle.Id, (UINT) result);
    }

    fullBarrier();
}

void VkDeferredEvaluate::Forget(VkCommandBuffer commandBuffer)
{
    std::scoped_lock lock(mutex);

    renderPassContinuing.erase(commandBuffer);
    Discard(commandBuffer);
}

void VkDeferredEvaluate::ForgetFeature(unsigned int handleId)
{
    if (waiting == 0)
        return;

    std::scoped_lock lock(mutex);

    for (auto map : { &recorded, &executed })
    {
        for (auto& [commandBuffer, calls] : *map)
        {
            waiting -= std::erase_if(calls, [&](const DeferredCall& call) { return call.handle.Id == handleId; });
        }
    }
}
