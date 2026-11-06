#include <pch.h>
#include <Config.h>
#include <State.h>
#include "SGSR1Feature_Vk.h"
#include <nvsdk_ngx_vk.h>

SGSR1FeatureVk::SGSR1FeatureVk(unsigned int InHandleId, NVSDK_NGX_Parameter* InParameters)
    : IFeature(InHandleId, InParameters), IFeature_Vk(InHandleId, InParameters)
{
    _initParameters = SetInitParameters(InParameters);
    _moduleLoaded = true;
}

bool SGSR1FeatureVk::InitInternal(VkCommandBuffer InCmdBuffer, NVSDK_NGX_Parameter* InParameters)
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

    _pass = std::make_unique<SGSR1Pass_Vk>("SGSR1", Device, PhysicalDevice);

    if (!_pass->IsInit())
    {
        LOG_ERROR("Can't create the SGSR 1 pass");
        _pass.reset();
        return false;
    }

    SetInit(true);
    return true;
}

bool SGSR1FeatureVk::EvaluateInternal(VkCommandBuffer InCmdBuffer, NVSDK_NGX_Parameter* InParameters)
{
    LOG_FUNC();

    if (!IsInited() || _pass == nullptr)
        return false;

    NVSDK_NGX_Resource_VK* paramColor = nullptr;
    NVSDK_NGX_Resource_VK* paramOutput = nullptr;

    InParameters->Get(NVSDK_NGX_Parameter_Color, (void**) &paramColor);
    InParameters->Get(NVSDK_NGX_Parameter_Output, (void**) &paramOutput);

    _hasColor = paramColor != nullptr;
    _hasOutput = paramOutput != nullptr;

    if (!_hasColor || !_hasOutput)
    {
        LOG_ERROR("Missing input! color: {}, output: {}", _hasColor, _hasOutput);
        return false;
    }

    const auto& color = paramColor->Resource.ImageViewInfo;
    const auto& output = paramOutput->Resource.ImageViewInfo;

    unsigned int renderWidth = 0;
    unsigned int renderHeight = 0;
    GetRenderResolution(InParameters, &renderWidth, &renderHeight);

    if (renderWidth == 0 || renderHeight == 0)
    {
        LOG_ERROR("Invalid render resolution: {}x{}", renderWidth, renderHeight);
        return false;
    }

    if (_frameCount == 0)
    {
        LOG_INFO("Render {}x{}, color {}x{} format {}, output {}x{} format {}, HDR: {}", renderWidth, renderHeight,
                 color.Width, color.Height, static_cast<int>(color.Format), output.Width, output.Height,
                 static_cast<int>(output.Format), IsHdr());
    }

    const SGSR1FrameInfo frame { renderWidth,  renderHeight,  color.Width, color.Height,
                                 output.Width, output.Height, IsHdr() };
    const auto constants = BuildSGSR1Constants(InParameters, frame);

    const auto result = _pass->Dispatch(InCmdBuffer, color.ImageView, output.ImageView, constants);

    _frameCount++;

    return result;
}
