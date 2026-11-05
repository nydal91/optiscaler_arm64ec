#include <pch.h>
#include <Config.h>
#include "MathUtils.h"
#include <upscalers/IFeature.h>
#include "SGSR2Constants.h"

using namespace OptiMath;

namespace
{
// Per-pixel motion cancels this anywhere that moves, so it only applies to static pixels
constexpr float MinLerpContribution = 0.3f;
} // namespace

SGSR2Constants SGSR2ConstantBuilder::Build(NVSDK_NGX_Parameter* InParameters, const SGSR2FrameInfo& frame)
{
    auto& cfg = *Config::Instance();

    float jitterX = 0.0f;
    float jitterY = 0.0f;
    InParameters->Get(NVSDK_NGX_Parameter_Jitter_Offset_X, &jitterX);
    InParameters->Get(NVSDK_NGX_Parameter_Jitter_Offset_Y, &jitterY);

    float mvScaleX = 1.0f;
    float mvScaleY = 1.0f;
    if (InParameters->Get(NVSDK_NGX_Parameter_MV_Scale_X, &mvScaleX) != NVSDK_NGX_Result_Success ||
        InParameters->Get(NVSDK_NGX_Parameter_MV_Scale_Y, &mvScaleY) != NVSDK_NGX_Result_Success)
    {
        LOG_WARN("Can't get motion vector scales!");
    }

    float preExposure = 1.0f;
    if (InParameters->Get(NVSDK_NGX_Parameter_DLSS_Pre_Exposure, &preExposure) != NVSDK_NGX_Result_Success ||
        preExposure <= 0.0f)
    {
        preExposure = 1.0f;
    }

    float verticalFov = GetRadiansFromDeg(60.0f);
    if (cfg.FsrVerticalFov.has_value())
    {
        verticalFov = GetRadiansFromDeg(cfg.FsrVerticalFov.value());
    }
    else if (cfg.FsrHorizontalFov.value_or_default() > 0.0f)
    {
        verticalFov = GetVerticalFovFromHorizontal(GetRadiansFromDeg(cfg.FsrHorizontalFov.value()),
                                                   static_cast<float>(frame.targetWidth),
                                                   static_cast<float>(frame.targetHeight));
    }

    const float renderW = static_cast<float>(frame.renderWidth);
    const float renderH = static_cast<float>(frame.renderHeight);
    const float displayW = static_cast<float>(frame.displayWidth);
    const float displayH = static_cast<float>(frame.displayHeight);

    SGSR2Constants constants {};
    constants.renderSize[0] = frame.renderWidth;
    constants.renderSize[1] = frame.renderHeight;
    constants.displaySize[0] = frame.displayWidth;
    constants.displaySize[1] = frame.displayHeight;
    constants.renderSizeRcp[0] = 1.0f / renderW;
    constants.renderSizeRcp[1] = 1.0f / renderH;
    constants.displaySizeRcp[0] = 1.0f / displayW;
    constants.displaySizeRcp[1] = 1.0f / displayH;
    constants.jitterOffset[0] = jitterX;
    constants.jitterOffset[1] = jitterY;

    const float motionSizeW = frame.lowResMV ? renderW : displayW;
    const float motionSizeH = frame.lowResMV ? renderH : displayH;
    constants.motionUvScale[0] = mvScaleX / motionSizeW;
    constants.motionUvScale[1] = mvScaleY / motionSizeH;
    constants.motionTexelScale[0] = motionSizeW / renderW;
    constants.motionTexelScale[1] = motionSizeH / renderH;

    if (frame.jitteredMV && !frame.reset)
    {
        constants.jitterCancel[0] = (_prevJitter[0] - jitterX) / renderW;
        constants.jitterCancel[1] = (_prevJitter[1] - jitterY) / renderH;
    }

    constants.depthSizeRcp[0] = 1.0f / static_cast<float>(frame.depthWidth);
    constants.depthSizeRcp[1] = 1.0f / static_cast<float>(frame.depthHeight);

    // History is tonemapped relative to the previous exposure
    constants.preExposure =
        _prevPreExposure > 0.0f && !frame.reset ? std::clamp(_prevPreExposure / preExposure, 0.25f, 4.0f) : 1.0f;
    constants.cameraFovAngleHor = std::tan(verticalFov * 0.5f) * renderW / renderH;
    constants.minLerpContribution = MinLerpContribution;

    // NGX has no camera matrices to tell if the camera moved. Always taking all 9 samples costs a little more than
    // upstream's moving camera path, and minLerpContribution is cancelled per pixel by the motion vectors
    constants.sameCamera = 1;
    constants.reset = frame.reset ? 1 : 0;
    constants.depthInverted = frame.depthInverted ? 1 : 0;

    _prevJitter[0] = jitterX;
    _prevJitter[1] = jitterY;
    _prevPreExposure = preExposure;

    return constants;
}
