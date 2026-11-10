#include <pch.h>
#include <Config.h>
#include <State.h>
#include "SGSR1Feature_Dx12.h"

SGSR1FeatureDx12::SGSR1FeatureDx12(unsigned int InHandleId, NVSDK_NGX_Parameter* InParameters)
    : IFeature(InHandleId, InParameters), IFeature_Dx12(InHandleId, InParameters)
{
    _initParameters = SetInitParameters(InParameters);
    _moduleLoaded = true;
}

bool SGSR1FeatureDx12::InitInternal(ID3D12GraphicsCommandList* InCommandList, NVSDK_NGX_Parameter* InParameters)
{
    LOG_FUNC();

    if (IsInited())
        return true;

    if (Device == nullptr)
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

    _pass = std::make_unique<SGSR1Pass_Dx12>("SGSR1", Device);

    if (!_pass->IsInit())
    {
        LOG_ERROR("Can't create the SGSR 1 pass");
        _pass.reset();
        return false;
    }

    SetInit(true);
    return true;
}

bool SGSR1FeatureDx12::EvaluateInternal(ID3D12GraphicsCommandList* InCommandList, NVSDK_NGX_Parameter* InParameters)
{
    LOG_FUNC();

    if (!IsInited() || _pass == nullptr)
        return false;

    auto& cfg = *Config::Instance();
    auto& state = State::Instance();

    ID3D12Resource* paramColor = nullptr;
    ID3D12Resource* paramOutput = nullptr;

    const auto getResource = [InParameters](const char* name, ID3D12Resource** resource)
    {
        if (InParameters->Get(name, resource) != NVSDK_NGX_Result_Success)
            InParameters->Get(name, (void**) resource);
    };

    getResource(NVSDK_NGX_Parameter_Color, &paramColor);
    getResource(NVSDK_NGX_Parameter_Output, &paramOutput);

    _hasColor = paramColor != nullptr;
    _hasOutput = paramOutput != nullptr;

    if (!_hasColor || !_hasOutput)
    {
        LOG_ERROR("Missing input! color: {}, output: {}", _hasColor, _hasOutput);
        return false;
    }

    const auto colorDesc = paramColor->GetDesc();
    const auto outputDesc = paramOutput->GetDesc();

    if (colorDesc.Flags & D3D12_RESOURCE_FLAG_DENY_SHADER_RESOURCE)
    {
        LOG_ERROR("Color can't be read by a shader, flags: {:x}", static_cast<int>(colorDesc.Flags));
        return false;
    }

    if (!(outputDesc.Flags & D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS))
    {
        LOG_ERROR("Output has no unordered access, flags: {:x}", static_cast<int>(outputDesc.Flags));
        return false;
    }

    unsigned int renderWidth = 0;
    unsigned int renderHeight = 0;
    GetRenderResolution(InParameters, &renderWidth, &renderHeight);

    if (renderWidth == 0 || renderHeight == 0)
    {
        LOG_ERROR("Invalid render resolution: {}x{}", renderWidth, renderHeight);
        return false;
    }

    unsigned int outputWidth = 0;
    unsigned int outputHeight = 0;
    GetOutputExtent(static_cast<unsigned int>(outputDesc.Width), outputDesc.Height, &outputWidth, &outputHeight);

    const SGSR1FrameInfo frame { renderWidth,  renderHeight, static_cast<uint32_t>(colorDesc.Width),
                                 colorDesc.Height, outputWidth,  outputHeight, IsHdr() };
    const auto constants = BuildSGSR1Constants(InParameters, frame);

    const bool isUnreal = state.NVNGX_Engine == NVSDK_NGX_ENGINE_TYPE_UNREAL ||
                          state.gameEngine == GameEngineType::Unreal || state.gameQuirks & GameQuirk::ForceUnrealEngine;

    constexpr auto ReadState = D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;

    // Unreal doesn't move the inputs to the state NGX asks for
    if (!cfg.ColorResourceBarrier.has_value() && isUnreal)
        cfg.ColorResourceBarrier.set_volatile_value(D3D12_RESOURCE_STATE_RENDER_TARGET);

    const auto transition = [&](ID3D12Resource* resource, auto& barrier, D3D12_RESOURCE_STATES target, bool undo)
    {
        if (!barrier.has_value())
            return;

        const auto configured = static_cast<D3D12_RESOURCE_STATES>(barrier.value());
        ResourceBarrier(InCommandList, resource, undo ? target : configured, undo ? configured : target);
    };

    const auto transitionAll = [&](bool undo)
    {
        transition(paramColor, cfg.ColorResourceBarrier, ReadState, undo);
        transition(paramOutput, cfg.OutputResourceBarrier, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, undo);
    };

    transitionAll(false);

    const auto result = _pass->Dispatch(InCommandList, paramColor, paramOutput, constants);

    transitionAll(true);

    _frameCount++;

    return result;
}
