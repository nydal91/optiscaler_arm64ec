#pragma once
#include <cstdint>

struct NVSDK_NGX_Parameter;

constexpr uint32_t SGSR2ThreadGroupSize = 8;

// Must match the Params cbuffer in sgsr2_common.hlsli
struct SGSR2Constants
{
    uint32_t renderSize[2];
    uint32_t displaySize[2];
    float renderSizeRcp[2];
    float displaySizeRcp[2];
    float jitterOffset[2];
    float motionUvScale[2];
    float motionTexelScale[2];
    float jitterCancel[2];
    float depthSizeRcp[2];
    float preExposure;
    float cameraFovAngleHor;
    float minLerpContribution;
    uint32_t sameCamera;
    uint32_t reset;
    uint32_t depthInverted;
};

struct SGSR2FrameInfo
{
    uint32_t renderWidth;
    uint32_t renderHeight;
    uint32_t displayWidth;
    uint32_t displayHeight;
    uint32_t depthWidth;
    uint32_t depthHeight;
    uint32_t targetWidth;
    uint32_t targetHeight;
    bool lowResMV;
    bool jitteredMV;
    bool depthInverted;
    bool reset;
};

// Keeps the values the next frame's constants depend on
class SGSR2ConstantBuilder
{
    float _prevJitter[2] = {};
    float _prevPreExposure = 0.0f;

  public:
    SGSR2Constants Build(NVSDK_NGX_Parameter* InParameters, const SGSR2FrameInfo& frame);
};
