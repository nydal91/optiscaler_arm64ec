#include <pch.h>
#include <Config.h>
#include <State.h>
#include "SGSR2Feature_Dx12.h"

SGSR2FeatureDx12::SGSR2FeatureDx12(unsigned int InHandleId, NVSDK_NGX_Parameter* InParameters)
    : IFeature(InHandleId, InParameters), IFeature_Dx12(InHandleId, InParameters)
{
    _initParameters = SetInitParameters(InParameters);
    _moduleLoaded = true;
}

bool SGSR2FeatureDx12::InitInternal(ID3D12GraphicsCommandList* InCommandList, NVSDK_NGX_Parameter* InParameters)
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

    _passes = std::make_unique<SGSR2Passes_Dx12>("SGSR2", Device);

    if (!_passes->IsInit())
    {
        LOG_ERROR("Can't create the SGSR 2 passes");
        _passes.reset();
        return false;
    }

    SetInit(true);
    return true;
}

bool SGSR2FeatureDx12::EvaluateInternal(ID3D12GraphicsCommandList* InCommandList, NVSDK_NGX_Parameter* InParameters)
{
    LOG_FUNC();

    if (!IsInited() || _passes == nullptr)
        return false;

    auto& cfg = *Config::Instance();
    auto& state = State::Instance();

    ID3D12Resource* paramColor = nullptr;
    ID3D12Resource* paramVelocity = nullptr;
    ID3D12Resource* paramDepth = nullptr;
    ID3D12Resource* paramOutput = nullptr;

    const auto getResource = [InParameters](const char* name, ID3D12Resource** resource)
    {
        if (InParameters->Get(name, resource) != NVSDK_NGX_Result_Success)
            InParameters->Get(name, (void**) resource);
    };

    getResource(NVSDK_NGX_Parameter_Color, &paramColor);
    getResource(NVSDK_NGX_Parameter_MotionVectors, &paramVelocity);
    getResource(NVSDK_NGX_Parameter_Depth, &paramDepth);
    getResource(NVSDK_NGX_Parameter_Output, &paramOutput);

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

    const auto colorDesc = paramColor->GetDesc();
    const auto velocityDesc = paramVelocity->GetDesc();
    const auto depthDesc = paramDepth->GetDesc();
    const auto outputDesc = paramOutput->GetDesc();

    if ((colorDesc.Flags | velocityDesc.Flags | depthDesc.Flags) & D3D12_RESOURCE_FLAG_DENY_SHADER_RESOURCE)
    {
        LOG_ERROR("Inputs can't be read by a shader, color: {:x}, motion: {:x}, depth: {:x}",
                  static_cast<int>(colorDesc.Flags), static_cast<int>(velocityDesc.Flags),
                  static_cast<int>(depthDesc.Flags));
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

    const auto displayWidth = static_cast<uint32_t>(outputDesc.Width);
    const auto displayHeight = static_cast<uint32_t>(outputDesc.Height);

    bool recreated = false;
    if (!_passes->EnsureTextures(paramOutput, renderWidth, renderHeight, displayWidth, displayHeight, recreated))
        return false;

    if (recreated)
    {
        LOG_INFO("Render {}x{}, display {}x{}, formats color: {}, motion: {}, depth: {}, output: {}", renderWidth,
                 renderHeight, displayWidth, displayHeight, static_cast<int>(colorDesc.Format),
                 static_cast<int>(velocityDesc.Format), static_cast<int>(depthDesc.Format),
                 static_cast<int>(outputDesc.Format));
    }

    unsigned int resetFlag = 0;
    InParameters->Get(NVSDK_NGX_Parameter_Reset, &resetFlag);

    SGSR2FrameInfo frame {};
    frame.renderWidth = renderWidth;
    frame.renderHeight = renderHeight;
    frame.displayWidth = displayWidth;
    frame.displayHeight = displayHeight;
    frame.depthWidth = static_cast<uint32_t>(depthDesc.Width);
    frame.depthHeight = depthDesc.Height;
    frame.targetWidth = TargetWidth();
    frame.targetHeight = TargetHeight();
    frame.lowResMV = LowResMV();
    frame.jitteredMV = JitteredMV();
    frame.depthInverted = DepthInverted();
    frame.reset = resetFlag == 1 || recreated || _frameCount == 0;

    const auto constants = _constantBuilder.Build(InParameters, frame);

    const bool isUnreal = state.NVNGX_Engine == NVSDK_NGX_ENGINE_TYPE_UNREAL ||
                          state.gameEngine == GameEngineType::Unreal || state.gameQuirks & GameQuirk::ForceUnrealEngine;

    constexpr auto ReadState = D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;

    // Unreal doesn't move the inputs to the state NGX asks for
    if (!cfg.ColorResourceBarrier.has_value() && isUnreal)
        cfg.ColorResourceBarrier.set_volatile_value(D3D12_RESOURCE_STATE_RENDER_TARGET);

    if (!cfg.MVResourceBarrier.has_value() && isUnreal)
        cfg.MVResourceBarrier.set_volatile_value(D3D12_RESOURCE_STATE_UNORDERED_ACCESS);

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
        transition(paramVelocity, cfg.MVResourceBarrier, ReadState, undo);
        transition(paramDepth, cfg.DepthResourceBarrier, ReadState, undo);
        transition(paramOutput, cfg.OutputResourceBarrier, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, undo);
    };

    transitionAll(false);

    const SGSR2Passes_Dx12::Inputs inputs { paramColor, paramDepth, paramVelocity, paramOutput };
    const auto result = _passes->Dispatch(InCommandList, inputs, constants);

    transitionAll(true);

    _frameCount++;

    return result;
}
