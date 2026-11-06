#include <pch.h>
#include "SGSR1Pass_Vk.h"

#include "precompile/sgsr1_upscale_Vk.h"

namespace
{
constexpr uint32_t ConstantsBinding = 0;
constexpr uint32_t SamplerBinding = 1;
constexpr uint32_t ColorBinding = 2;
constexpr uint32_t OutputBinding = 3;
} // namespace

SGSR1Pass_Vk::SGSR1Pass_Vk(std::string InName, VkDevice InDevice, VkPhysicalDevice InPhysicalDevice)
    : Shader_Vk(InName, InDevice, InPhysicalDevice)
{
    if (InDevice == VK_NULL_HANDLE)
    {
        LOG_ERROR("InDevice is nullptr!");
        return;
    }

    _maxFramesInFlight = FramesInFlight;

    CreateSampler(VK_FILTER_LINEAR);

    if (_textureSampler == VK_NULL_HANDLE)
        return;

    static_assert(sizeof(SGSR1Constants) <= ConstantsStride);
    CreateConstantBuffer(ConstantsStride * FramesInFlight);

    if (_mappedConstantBuffer == nullptr)
        return;

    CreateLayouts({ CreateBinding(ConstantsBinding, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER),
                    CreateBinding(SamplerBinding, VK_DESCRIPTOR_TYPE_SAMPLER),
                    CreateBinding(ColorBinding, VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE),
                    CreateBinding(OutputBinding, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE) });

    if (_pipelineLayout == VK_NULL_HANDLE)
        return;

    CreateDescriptorPool({ { VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, FramesInFlight },
                           { VK_DESCRIPTOR_TYPE_SAMPLER, FramesInFlight },
                           { VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, FramesInFlight },
                           { VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, FramesInFlight } },
                         FramesInFlight);

    if (_descriptorPool == VK_NULL_HANDLE)
        return;

    CreateDescriptorSets(_descriptorSetLayout, _descriptorPool, _descriptorSets);

    if (_descriptorSets.empty() || _descriptorSets[0] == VK_NULL_HANDLE)
        return;

    const std::vector<char> shaderCode(sgsr1_upscale_spv, sgsr1_upscale_spv + sizeof(sgsr1_upscale_spv));

    if (!CreateComputePipeline(_device, _pipelineLayout, &_pipeline, shaderCode))
    {
        LOG_ERROR("[{0}] Failed to create the compute pipeline", _name);
        return;
    }

    _init = true;
}

bool SGSR1Pass_Vk::Dispatch(VkCommandBuffer InCmdList, VkImageView color, VkImageView output,
                            const SGSR1Constants& constants)
{
    if (!_init || InCmdList == VK_NULL_HANDLE)
        return false;

    _currentSetIndex = (_currentSetIndex + 1) % FramesInFlight;

    const VkDeviceSize constantsOffset = _currentSetIndex * ConstantsStride;
    memcpy(static_cast<char*>(_mappedConstantBuffer) + constantsOffset, &constants, sizeof(constants));

    const auto set = _descriptorSets[_currentSetIndex];

    const VkDescriptorBufferInfo constantsInfo { _constantBuffer, constantsOffset, sizeof(SGSR1Constants) };
    const VkDescriptorImageInfo samplerInfo { _textureSampler, VK_NULL_HANDLE, VK_IMAGE_LAYOUT_UNDEFINED };
    const VkDescriptorImageInfo colorInfo { VK_NULL_HANDLE, color, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL };
    const VkDescriptorImageInfo outputInfo { VK_NULL_HANDLE, output, VK_IMAGE_LAYOUT_GENERAL };

    const VkWriteDescriptorSet writes[] = {
        { VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, nullptr, set, ConstantsBinding, 0, 1,
          VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, nullptr, &constantsInfo, nullptr },
        { VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, nullptr, set, SamplerBinding, 0, 1, VK_DESCRIPTOR_TYPE_SAMPLER,
          &samplerInfo, nullptr, nullptr },
        { VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, nullptr, set, ColorBinding, 0, 1, VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE,
          &colorInfo, nullptr, nullptr },
        { VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, nullptr, set, OutputBinding, 0, 1, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE,
          &outputInfo, nullptr, nullptr },
    };

    vkUpdateDescriptorSets(_device, static_cast<uint32_t>(std::size(writes)), writes, 0, nullptr);

    vkCmdBindDescriptorSets(InCmdList, VK_PIPELINE_BIND_POINT_COMPUTE, _pipelineLayout, 0, 1, &set, 0, nullptr);
    vkCmdBindPipeline(InCmdList, VK_PIPELINE_BIND_POINT_COMPUTE, _pipeline);
    vkCmdDispatch(InCmdList, (constants.displaySize[0] + SGSR1ThreadGroupSize - 1) / SGSR1ThreadGroupSize,
                  (constants.displaySize[1] + SGSR1ThreadGroupSize - 1) / SGSR1ThreadGroupSize, 1);

    VkMemoryBarrier barrier { VK_STRUCTURE_TYPE_MEMORY_BARRIER };
    barrier.srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
    barrier.dstAccessMask = VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT;
    vkCmdPipelineBarrier(InCmdList, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0, 1,
                         &barrier, 0, nullptr, 0, nullptr);

    return true;
}
