#include <pch.h>
#include <Config.h>
#include <State.h>
#include <misc/DeferredRelease.h>
#include "SGSR2Passes_Vk.h"

#include "precompile/sgsr2_convert_Vk.h"
#include "precompile/sgsr2_upscale_Vk.h"

namespace
{
constexpr VkFormat MotionDepthClipFormat = VK_FORMAT_R16G16B16A16_SFLOAT;
constexpr VkFormat ColorLumaFormat = VK_FORMAT_R32_UINT;
constexpr VkFormat HistoryFormat = VK_FORMAT_R16G16B16A16_SFLOAT;

constexpr uint32_t ConstantsBinding = 0;
constexpr uint32_t LinearSamplerBinding = 1;
constexpr uint32_t PointSamplerBinding = 2;
constexpr uint32_t FirstSourceBinding = 3;
constexpr uint32_t FirstTargetBinding = 6;

void GlobalBarrier(VkCommandBuffer cmdBuffer, VkPipelineStageFlags srcStage, VkAccessFlags srcAccess,
                   VkPipelineStageFlags dstStage, VkAccessFlags dstAccess)
{
    VkMemoryBarrier barrier { VK_STRUCTURE_TYPE_MEMORY_BARRIER };
    barrier.srcAccessMask = srcAccess;
    barrier.dstAccessMask = dstAccess;
    vkCmdPipelineBarrier(cmdBuffer, srcStage, dstStage, 0, 1, &barrier, 0, nullptr, 0, nullptr);
}
} // namespace

SGSR2Passes_Vk::SGSR2Passes_Vk(std::string InName, VkDevice InDevice, VkPhysicalDevice InPhysicalDevice)
    : Shader_Vk(InName, InDevice, InPhysicalDevice)
{
    if (InDevice == VK_NULL_HANDLE)
    {
        LOG_ERROR("InDevice is nullptr!");
        return;
    }

    _maxFramesInFlight = FramesInFlight * SetsPerFrame;

    // The base class owns the linear one
    CreateSampler(VK_FILTER_NEAREST);
    _pointSampler = _textureSampler;
    _textureSampler = VK_NULL_HANDLE;
    CreateSampler(VK_FILTER_LINEAR);

    if (_pointSampler == VK_NULL_HANDLE || _textureSampler == VK_NULL_HANDLE)
        return;

    static_assert(sizeof(SGSR2Constants) <= ConstantsStride);
    CreateConstantBuffer(ConstantsStride * FramesInFlight);

    if (_mappedConstantBuffer == nullptr)
        return;

    std::vector<VkDescriptorSetLayoutBinding> bindings = {
        CreateBinding(ConstantsBinding, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER),
        CreateBinding(LinearSamplerBinding, VK_DESCRIPTOR_TYPE_SAMPLER),
        CreateBinding(PointSamplerBinding, VK_DESCRIPTOR_TYPE_SAMPLER)
    };

    for (uint32_t i = FirstSourceBinding; i < FirstTargetBinding; i++)
        bindings.push_back(CreateBinding(i, VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE));

    for (uint32_t i = FirstTargetBinding; i < FirstTargetBinding + 2; i++)
        bindings.push_back(CreateBinding(i, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE));

    CreateLayouts(bindings);

    if (_pipelineLayout == VK_NULL_HANDLE)
        return;

    const std::vector<VkDescriptorPoolSize> poolSizes = {
        { VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, _maxFramesInFlight },
        { VK_DESCRIPTOR_TYPE_SAMPLER, 2 * _maxFramesInFlight },
        { VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, 3 * _maxFramesInFlight },
        { VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 2 * _maxFramesInFlight },
    };

    CreateDescriptorPool(poolSizes, _maxFramesInFlight);

    if (_descriptorPool == VK_NULL_HANDLE)
        return;

    CreateDescriptorSets(_descriptorSetLayout, _descriptorPool, _descriptorSets);

    if (_descriptorSets.empty() || _descriptorSets[0] == VK_NULL_HANDLE)
        return;

    const std::vector<char> convertCode(sgsr2_convert_spv, sgsr2_convert_spv + sizeof(sgsr2_convert_spv));
    const std::vector<char> upscaleCode(sgsr2_upscale_spv, sgsr2_upscale_spv + sizeof(sgsr2_upscale_spv));

    if (!CreateComputePipeline(_device, _pipelineLayout, &_pipeline, convertCode) ||
        !CreateComputePipeline(_device, _pipelineLayout, &_upscalePipeline, upscaleCode))
    {
        LOG_ERROR("[{0}] Failed to create compute pipelines", _name);
        return;
    }

    _init = true;
}

SGSR2Passes_Vk::~SGSR2Passes_Vk()
{
    if (State::Instance().isShuttingDown)
        return;

    ReleaseTextures();

    // Command buffers in flight can still reference these
    DeferredRelease::Enqueue(
        [device = _device, pipeline = _upscalePipeline, sampler = _pointSampler]()
        {
            if (pipeline != VK_NULL_HANDLE)
                vkDestroyPipeline(device, pipeline, nullptr);

            if (sampler != VK_NULL_HANDLE)
                vkDestroySampler(device, sampler, nullptr);
        },
        (uint64_t) _device);
}

bool SGSR2Passes_Vk::CreateTexture(Texture& texture, uint32_t width, uint32_t height, VkFormat format)
{
    VkImageCreateInfo imageInfo { VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO };
    imageInfo.imageType = VK_IMAGE_TYPE_2D;
    imageInfo.extent = { width, height, 1 };
    imageInfo.mipLevels = 1;
    imageInfo.arrayLayers = 1;
    imageInfo.format = format;
    imageInfo.tiling = VK_IMAGE_TILING_OPTIMAL;
    imageInfo.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    imageInfo.usage = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
    imageInfo.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    imageInfo.samples = VK_SAMPLE_COUNT_1_BIT;

    if (vkCreateImage(_device, &imageInfo, nullptr, &texture.image) != VK_SUCCESS)
    {
        LOG_ERROR("[{0}] Failed to create a {1}x{2} texture", _name, width, height);
        return false;
    }

    VkMemoryRequirements requirements;
    vkGetImageMemoryRequirements(_device, texture.image, &requirements);

    VkMemoryAllocateInfo allocInfo { VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO };
    allocInfo.allocationSize = requirements.size;
    allocInfo.memoryTypeIndex =
        FindMemoryType(_physicalDevice, requirements.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);

    if (vkAllocateMemory(_device, &allocInfo, nullptr, &texture.memory) != VK_SUCCESS ||
        vkBindImageMemory(_device, texture.image, texture.memory, 0) != VK_SUCCESS)
    {
        LOG_ERROR("[{0}] Failed to allocate a {1}x{2} texture", _name, width, height);
        return false;
    }

    VkImageViewCreateInfo viewInfo { VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO };
    viewInfo.image = texture.image;
    viewInfo.viewType = VK_IMAGE_VIEW_TYPE_2D;
    viewInfo.format = format;
    viewInfo.subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 };

    if (vkCreateImageView(_device, &viewInfo, nullptr, &texture.view) != VK_SUCCESS)
    {
        LOG_ERROR("[{0}] Failed to create a {1}x{2} texture view", _name, width, height);
        return false;
    }

    return true;
}

void SGSR2Passes_Vk::ReleaseTexture(Texture& texture)
{
    if (texture.image == VK_NULL_HANDLE && texture.memory == VK_NULL_HANDLE)
        return;

    // Command buffers in flight can still reference it
    DeferredRelease::Enqueue(
        [device = _device, texture]()
        {
            if (texture.view != VK_NULL_HANDLE)
                vkDestroyImageView(device, texture.view, nullptr);

            if (texture.image != VK_NULL_HANDLE)
                vkDestroyImage(device, texture.image, nullptr);

            if (texture.memory != VK_NULL_HANDLE)
                vkFreeMemory(device, texture.memory, nullptr);
        },
        (uint64_t) _device);

    texture = {};
}

void SGSR2Passes_Vk::ReleaseTextures()
{
    ReleaseTexture(_motionDepthClip);
    ReleaseTexture(_colorLuma);
    ReleaseTexture(_history[0]);
    ReleaseTexture(_history[1]);
}

bool SGSR2Passes_Vk::EnsureTextures(uint32_t renderWidth, uint32_t renderHeight, uint32_t displayWidth,
                                    uint32_t displayHeight, bool& recreated)
{
    recreated = renderWidth != _renderWidth || renderHeight != _renderHeight || displayWidth != _displayWidth ||
                displayHeight != _displayHeight;

    if (!recreated)
        return true;

    ReleaseTextures();
    _renderWidth = 0;
    _texturesPrepared = false;

    if (!CreateTexture(_motionDepthClip, renderWidth, renderHeight, MotionDepthClipFormat) ||
        !CreateTexture(_colorLuma, renderWidth, renderHeight, ColorLumaFormat) ||
        !CreateTexture(_history[0], displayWidth, displayHeight, HistoryFormat) ||
        !CreateTexture(_history[1], displayWidth, displayHeight, HistoryFormat))
    {
        ReleaseTextures();
        return false;
    }

    _renderWidth = renderWidth;
    _renderHeight = renderHeight;
    _displayWidth = displayWidth;
    _displayHeight = displayHeight;

    return true;
}

void SGSR2Passes_Vk::PrepareTextures(VkCommandBuffer InCmdList)
{
    const VkImageSubresourceRange range { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 };
    const VkImage images[] = { _motionDepthClip.image, _colorLuma.image, _history[0].image, _history[1].image };

    for (auto image : images)
        SetImageLayout(InCmdList, image, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_GENERAL, range);

    // The first frame reads the previous history's alpha even though it's a reset
    GlobalBarrier(InCmdList, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_ACCESS_SHADER_WRITE_BIT,
                  VK_PIPELINE_STAGE_TRANSFER_BIT, VK_ACCESS_TRANSFER_WRITE_BIT);

    const VkClearColorValue clearColor {};
    vkCmdClearColorImage(InCmdList, _history[0].image, VK_IMAGE_LAYOUT_GENERAL, &clearColor, 1, &range);
    vkCmdClearColorImage(InCmdList, _history[1].image, VK_IMAGE_LAYOUT_GENERAL, &clearColor, 1, &range);

    GlobalBarrier(InCmdList, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_ACCESS_TRANSFER_WRITE_BIT,
                  VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT);

    _texturesPrepared = true;
}

void SGSR2Passes_Vk::UpdateDescriptorSet(VkDescriptorSet set, VkDeviceSize constantsOffset, VkImageView source0,
                                         VkImageView source1, VkImageView source2, VkImageView target0,
                                         VkImageView target1)
{
    // The passes' own textures stay in GENERAL, the game's are in the layouts NGX expects
    const auto sourceLayout = [this](VkImageView view)
    {
        const bool own = view == _motionDepthClip.view || view == _colorLuma.view || view == _history[0].view ||
                         view == _history[1].view;
        return own ? VK_IMAGE_LAYOUT_GENERAL : VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    };

    const VkDescriptorBufferInfo constantsInfo { _constantBuffer, constantsOffset, sizeof(SGSR2Constants) };
    const VkDescriptorImageInfo linearInfo { _textureSampler, VK_NULL_HANDLE, VK_IMAGE_LAYOUT_UNDEFINED };
    const VkDescriptorImageInfo pointInfo { _pointSampler, VK_NULL_HANDLE, VK_IMAGE_LAYOUT_UNDEFINED };
    const VkDescriptorImageInfo sources[] = {
        { VK_NULL_HANDLE, source0, sourceLayout(source0) },
        { VK_NULL_HANDLE, source1, sourceLayout(source1) },
        { VK_NULL_HANDLE, source2, sourceLayout(source2) },
    };
    const VkDescriptorImageInfo targets[] = {
        { VK_NULL_HANDLE, target0, VK_IMAGE_LAYOUT_GENERAL },
        { VK_NULL_HANDLE, target1, VK_IMAGE_LAYOUT_GENERAL },
    };

    const VkWriteDescriptorSet writes[] = {
        { VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, nullptr, set, ConstantsBinding, 0, 1,
          VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, nullptr, &constantsInfo, nullptr },
        { VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, nullptr, set, LinearSamplerBinding, 0, 1, VK_DESCRIPTOR_TYPE_SAMPLER,
          &linearInfo, nullptr, nullptr },
        { VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, nullptr, set, PointSamplerBinding, 0, 1, VK_DESCRIPTOR_TYPE_SAMPLER,
          &pointInfo, nullptr, nullptr },
        { VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, nullptr, set, FirstSourceBinding, 0, 3,
          VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, sources, nullptr, nullptr },
        { VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, nullptr, set, FirstTargetBinding, 0, 2,
          VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, targets, nullptr, nullptr },
    };

    vkUpdateDescriptorSets(_device, static_cast<uint32_t>(std::size(writes)), writes, 0, nullptr);
}

bool SGSR2Passes_Vk::Dispatch(VkCommandBuffer InCmdList, const Inputs& inputs, const SGSR2Constants& constants)
{
    if (!_init || InCmdList == VK_NULL_HANDLE || _motionDepthClip.image == VK_NULL_HANDLE)
        return false;

    if (!_texturesPrepared)
        PrepareTextures(InCmdList);

    _frame = (_frame + 1) % FramesInFlight;

    const VkDeviceSize constantsOffset = _frame * ConstantsStride;
    memcpy(static_cast<char*>(_mappedConstantBuffer) + constantsOffset, &constants, sizeof(constants));

    const auto convertSet = _descriptorSets[_frame * SetsPerFrame];
    const auto upscaleSet = _descriptorSets[_frame * SetsPerFrame + 1];

    const auto& prevHistory = _history[_historyIndex];
    const auto& nextHistory = _history[_historyIndex ^ 1];

    UpdateDescriptorSet(convertSet, constantsOffset, inputs.color, inputs.depth, inputs.velocity, _motionDepthClip.view,
                        _colorLuma.view);
    UpdateDescriptorSet(upscaleSet, constantsOffset, prevHistory.view, _motionDepthClip.view, _colorLuma.view,
                        inputs.output, nextHistory.view);

    vkCmdBindDescriptorSets(InCmdList, VK_PIPELINE_BIND_POINT_COMPUTE, _pipelineLayout, 0, 1, &convertSet, 0, nullptr);
    vkCmdBindPipeline(InCmdList, VK_PIPELINE_BIND_POINT_COMPUTE, _pipeline);
    vkCmdDispatch(InCmdList, (constants.renderSize[0] + SGSR2ThreadGroupSize - 1) / SGSR2ThreadGroupSize,
                  (constants.renderSize[1] + SGSR2ThreadGroupSize - 1) / SGSR2ThreadGroupSize, 1);

    GlobalBarrier(InCmdList, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_ACCESS_SHADER_WRITE_BIT,
                  VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_ACCESS_SHADER_READ_BIT);

    vkCmdBindDescriptorSets(InCmdList, VK_PIPELINE_BIND_POINT_COMPUTE, _pipelineLayout, 0, 1, &upscaleSet, 0, nullptr);
    vkCmdBindPipeline(InCmdList, VK_PIPELINE_BIND_POINT_COMPUTE, _upscalePipeline);
    vkCmdDispatch(InCmdList, (constants.displaySize[0] + SGSR2ThreadGroupSize - 1) / SGSR2ThreadGroupSize,
                  (constants.displaySize[1] + SGSR2ThreadGroupSize - 1) / SGSR2ThreadGroupSize, 1);

    // Covers next frame's passes as well as whatever reads the output
    GlobalBarrier(InCmdList, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_ACCESS_SHADER_WRITE_BIT,
                  VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT);

    _historyIndex ^= 1;

    return true;
}
