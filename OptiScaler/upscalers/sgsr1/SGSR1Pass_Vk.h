#pragma once
#include <shaders/Shader_Vk.h>

#include "SGSR1Constants.h"

class SGSR1Pass_Vk : public Shader_Vk
{
    // Every frame needs its own descriptor set and constants as the GPU can still be reading the previous ones
    static constexpr uint32_t FramesInFlight = 8;

    // Vulkan's largest allowed minUniformBufferOffsetAlignment
    static constexpr VkDeviceSize ConstantsStride = 256;

  public:
    // The color has to be readable by compute shaders and the output in VK_IMAGE_LAYOUT_GENERAL
    bool Dispatch(VkCommandBuffer InCmdList, VkImageView color, VkImageView output, const SGSR1Constants& constants);

    SGSR1Pass_Vk(std::string InName, VkDevice InDevice, VkPhysicalDevice InPhysicalDevice);
};
