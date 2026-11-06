#pragma once
#include <cstdint>

struct NVSDK_NGX_Parameter;

constexpr uint32_t SGSR1ThreadGroupSize = 8;

// Must match the Params cbuffer in sgsr1_upscale.hlsl
struct SGSR1Constants
{
    uint32_t displaySize[2];
    float displaySizeRcp[2];
    float renderSize[2];
    float inputSizeRcp[2];
    float jitterOffset[2];
    uint32_t hdr;
    uint32_t padding;
};

struct SGSR1FrameInfo
{
    uint32_t renderWidth;
    uint32_t renderHeight;
    uint32_t inputWidth;
    uint32_t inputHeight;
    uint32_t displayWidth;
    uint32_t displayHeight;
    bool hdr;
};

SGSR1Constants BuildSGSR1Constants(NVSDK_NGX_Parameter* InParameters, const SGSR1FrameInfo& frame);
