#include <pch.h>
#include <Config.h>
#include <State.h>
#include "SGSR2Feature_Vk.h"
#include <nvsdk_ngx_vk.h>

SGSR2FeatureVk::SGSR2FeatureVk(unsigned int InHandleId, NVSDK_NGX_Parameter* InParameters)
    : IFeature(InHandleId, InParameters), IFeature_Vk(InHandleId, InParameters)
{
    _initParameters = SetInitParameters(InParameters);
    _moduleLoaded = true;
}

bool SGSR2FeatureVk::InitInternal(VkCommandBuffer InCmdBuffer, NVSDK_NGX_Parameter* InParameters)
{
    LOG_FUNC();

    if (IsInited())
        return true;

    if (Device == VK_NULL_HANDLE)
    {
        LOG_ERROR("Device is null!");
        return false;
    }

    auto& cfg = *Config::Instance();

    float outputScale = 1.0f;
    if (cfg.OutputScalingEnabled.value_or_default() && (LowResMV() || RenderWidth() == DisplayWidth()))
    {
        outputScale = std::clamp(cfg.OutputScalingMultiplier.value_or_default(), 0.5f, 3.0f);
        cfg.OutputScalingMultiplier.set_volatile_value(outputScale);
    }

    _targetWidth = static_cast<unsigned int>(DisplayWidth() * outputScale);
    _targetHeight = static_cast<unsigned int>(DisplayHeight() * outputScale);

    _passes = std::make_unique<SGSR2Passes_Vk>("SGSR2", Device, PhysicalDevice);

    if (!_passes->IsInit())
    {
        LOG_ERROR("Can't create the SGSR 2 passes");
        _passes.reset();
        return false;
    }

    SetInit(true);
    return true;
}

bool SGSR2FeatureVk::EvaluateInternal(VkCommandBuffer InCmdBuffer, NVSDK_NGX_Parameter* InParameters)
{
    LOG_FUNC();

    if (!IsInited() || _passes == nullptr)
        return false;

    NVSDK_NGX_Resource_VK* paramColor = nullptr;
    NVSDK_NGX_Resource_VK* paramVelocity = nullptr;
    NVSDK_NGX_Resource_VK* paramDepth = nullptr;
    NVSDK_NGX_Resource_VK* paramOutput = nullptr;

    InParameters->Get(NVSDK_NGX_Parameter_Color, (void**) &paramColor);
    InParameters->Get(NVSDK_NGX_Parameter_MotionVectors, (void**) &paramVelocity);
    InParameters->Get(NVSDK_NGX_Parameter_Depth, (void**) &paramDepth);
    InParameters->Get(NVSDK_NGX_Parameter_Output, (void**) &paramOutput);

    _hasColor = paramColor != nullptr;
    _hasMV = paramVelocity != nullptr;
    _hasDepth = paramDepth != nullptr;
    _hasOutput = paramOutput != nullptr;

    if (!_hasColor || !_hasMV || !_hasDepth || !_hasOutput)
    {
        LOG_ERROR("Missing input! color: {}, motion: {}, depth: {}, output: {}", _hasColor, _hasMV, _hasDepth,
                  _hasOutput);
        return false;
    }

    const auto& color = paramColor->Resource.ImageViewInfo;
    const auto& velocity = paramVelocity->Resource.ImageViewInfo;
    const auto& depth = paramDepth->Resource.ImageViewInfo;
    const auto& output = paramOutput->Resource.ImageViewInfo;

    unsigned int renderWidth = 0;
    unsigned int renderHeight = 0;
    GetRenderResolution(InParameters, &renderWidth, &renderHeight);

    if (renderWidth == 0 || renderHeight == 0)
    {
        LOG_ERROR("Invalid render resolution: {}x{}", renderWidth, renderHeight);
        return false;
    }

    const uint32_t displayWidth = output.Width;
    const uint32_t displayHeight = output.Height;

    bool recreated = false;
    if (!_passes->EnsureTextures(renderWidth, renderHeight, displayWidth, displayHeight, recreated))
        return false;

    if (recreated)
    {
        LOG_INFO("Render {}x{}, display {}x{}, formats color: {}, motion: {}, depth: {}, output: {}", renderWidth,
                 renderHeight, displayWidth, displayHeight, static_cast<int>(color.Format),
                 static_cast<int>(velocity.Format), static_cast<int>(depth.Format), static_cast<int>(output.Format));
    }

    unsigned int resetFlag = 0;
    InParameters->Get(NVSDK_NGX_Parameter_Reset, &resetFlag);

    SGSR2FrameInfo frame {};
    frame.renderWidth = renderWidth;
    frame.renderHeight = renderHeight;
    frame.displayWidth = displayWidth;
    frame.displayHeight = displayHeight;
    frame.depthWidth = depth.Width;
    frame.depthHeight = depth.Height;
    frame.targetWidth = TargetWidth();
    frame.targetHeight = TargetHeight();
    frame.lowResMV = LowResMV();
    frame.jitteredMV = JitteredMV();
    frame.depthInverted = DepthInverted();
    frame.reset = resetFlag == 1 || recreated || _frameCount == 0;

    const auto constants = _constantBuilder.Build(InParameters, frame);

    const SGSR2Passes_Vk::Inputs inputs { color.ImageView, depth.ImageView, velocity.ImageView, output.ImageView };
    const auto result = _passes->Dispatch(InCmdBuffer, inputs, constants);

    _frameCount++;

    return result;
}
