#pragma once
#include <shaders/Shader_Vk.h>

#include "SGSR2Constants.h"

// The Convert and Upscale passes, which share a pipeline layout and swap two history textures
class SGSR2Passes_Vk : public Shader_Vk
{
  public:
    struct Inputs
    {
        VkImageView color = VK_NULL_HANDLE;
        VkImageView depth = VK_NULL_HANDLE;
        VkImageView velocity = VK_NULL_HANDLE;
        VkImageView output = VK_NULL_HANDLE;
    };

  private:
    struct Texture
    {
        VkImage image = VK_NULL_HANDLE;
        VkDeviceMemory memory = VK_NULL_HANDLE;
        VkImageView view = VK_NULL_HANDLE;
    };

    // Every frame needs its own descriptor sets and constants as the GPU can still be reading the previous ones
    static constexpr uint32_t FramesInFlight = 8;
    static constexpr uint32_t SetsPerFrame = 2;

    // Vulkan's largest allowed minUniformBufferOffsetAlignment
    static constexpr VkDeviceSize ConstantsStride = 256;

    VkPipeline _upscalePipeline = VK_NULL_HANDLE;
    VkSampler _pointSampler = VK_NULL_HANDLE;

    // All of them stay in VK_IMAGE_LAYOUT_GENERAL
    Texture _motionDepthClip {};
    Texture _colorLuma {};
    Texture _history[2] = {};

    uint32_t _renderWidth = 0;
    uint32_t _renderHeight = 0;
    uint32_t _displayWidth = 0;
    uint32_t _displayHeight = 0;
    uint32_t _frame = 0;
    uint32_t _historyIndex = 0;
    bool _texturesPrepared = false;

    bool CreateTexture(Texture& texture, uint32_t width, uint32_t height, VkFormat format);
    void ReleaseTexture(Texture& texture);
    void ReleaseTextures();
    void PrepareTextures(VkCommandBuffer InCmdList);
    void UpdateDescriptorSet(VkDescriptorSet set, VkDeviceSize constantsOffset, VkImageView source0,
                             VkImageView source1, VkImageView source2, VkImageView target0, VkImageView target1);

  public:
    bool EnsureTextures(uint32_t renderWidth, uint32_t renderHeight, uint32_t displayWidth, uint32_t displayHeight,
                        bool& recreated);

    // Inputs have to be readable by compute shaders and the output in VK_IMAGE_LAYOUT_GENERAL
    bool Dispatch(VkCommandBuffer InCmdList, const Inputs& inputs, const SGSR2Constants& constants);

    SGSR2Passes_Vk(std::string InName, VkDevice InDevice, VkPhysicalDevice InPhysicalDevice);

    ~SGSR2Passes_Vk();
};
