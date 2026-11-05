#include <pch.h>
#include <Config.h>
#include <Util.h>
#include "SGSR2Feature_Dx11.h"

#include <shaders/Shader_Dx11.h>

#include "generated/sgsr2_convert_cso.h"
#include "generated/sgsr2_upscale_cso.h"

SGSR2FeatureDx11::SGSR2FeatureDx11(unsigned int InHandleId, NVSDK_NGX_Parameter* InParameters)
    : IFeature(InHandleId, InParameters), IFeature_Dx11(InHandleId, InParameters)
{
    _initParameters = SetInitParameters(InParameters);
    _moduleLoaded = true;
}

bool SGSR2FeatureDx11::InitInternal(ID3D11DeviceContext* InContext, NVSDK_NGX_Parameter* InParameters)
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

    auto hr = Device->CreateComputeShader(sgsr2_convert_cso, sizeof(sgsr2_convert_cso), nullptr, &_convertShader);
    if (FAILED(hr))
    {
        LOG_ERROR("Convert shader error: {:x}", static_cast<unsigned long>(hr));
        return false;
    }

    hr = Device->CreateComputeShader(sgsr2_upscale_cso, sizeof(sgsr2_upscale_cso), nullptr, &_upscaleShader);
    if (FAILED(hr))
    {
        LOG_ERROR("Upscale shader error: {:x}", static_cast<unsigned long>(hr));
        return false;
    }

    D3D11_BUFFER_DESC bufferDesc {};
    bufferDesc.ByteWidth = (sizeof(SGSR2Constants) + 15) & ~15u;
    bufferDesc.Usage = D3D11_USAGE_DYNAMIC;
    bufferDesc.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
    bufferDesc.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;

    hr = Device->CreateBuffer(&bufferDesc, nullptr, &_constantBuffer);
    if (FAILED(hr))
    {
        LOG_ERROR("Constant buffer error: {:x}", static_cast<unsigned long>(hr));
        return false;
    }

    D3D11_SAMPLER_DESC samplerDesc {};
    samplerDesc.AddressU = D3D11_TEXTURE_ADDRESS_CLAMP;
    samplerDesc.AddressV = D3D11_TEXTURE_ADDRESS_CLAMP;
    samplerDesc.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
    samplerDesc.ComparisonFunc = D3D11_COMPARISON_NEVER;
    samplerDesc.MaxLOD = D3D11_FLOAT32_MAX;

    samplerDesc.Filter = D3D11_FILTER_MIN_MAG_MIP_LINEAR;
    hr = Device->CreateSamplerState(&samplerDesc, &_linearClamp);

    if (SUCCEEDED(hr))
    {
        samplerDesc.Filter = D3D11_FILTER_MIN_MAG_MIP_POINT;
        hr = Device->CreateSamplerState(&samplerDesc, &_pointClamp);
    }

    if (FAILED(hr))
    {
        LOG_ERROR("Sampler error: {:x}", static_cast<unsigned long>(hr));
        return false;
    }

    SetInit(true);
    return true;
}

bool SGSR2FeatureDx11::CreateRenderTexture(uint32_t width, uint32_t height, DXGI_FORMAT format, RenderTexture& out)
{
    out = {};

    D3D11_TEXTURE2D_DESC desc {};
    desc.Width = width;
    desc.Height = height;
    desc.MipLevels = 1;
    desc.ArraySize = 1;
    desc.Format = format;
    desc.SampleDesc.Count = 1;
    desc.Usage = D3D11_USAGE_DEFAULT;
    desc.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_UNORDERED_ACCESS;

    auto hr = Device->CreateTexture2D(&desc, nullptr, &out.texture);
    if (SUCCEEDED(hr))
        hr = Device->CreateShaderResourceView(out.texture.Get(), nullptr, &out.srv);
    if (SUCCEEDED(hr))
        hr = Device->CreateUnorderedAccessView(out.texture.Get(), nullptr, &out.uav);

    if (FAILED(hr))
    {
        LOG_ERROR("{}x{} format {} error: {:x}", width, height, static_cast<int>(format),
                  static_cast<unsigned long>(hr));
        return false;
    }

    return true;
}

bool SGSR2FeatureDx11::EnsureBuffers(uint32_t renderWidth, uint32_t renderHeight, uint32_t displayWidth,
                                     uint32_t displayHeight, bool& recreated)
{
    recreated = false;

    if (renderWidth != _bufferRenderWidth || renderHeight != _bufferRenderHeight)
    {
        if (!CreateRenderTexture(renderWidth, renderHeight, DXGI_FORMAT_R16G16B16A16_FLOAT, _motionDepthClip) ||
            !CreateRenderTexture(renderWidth, renderHeight, DXGI_FORMAT_R32_UINT, _colorLuma))
        {
            _bufferRenderWidth = 0;
            return false;
        }

        _bufferRenderWidth = renderWidth;
        _bufferRenderHeight = renderHeight;
        recreated = true;
    }

    if (displayWidth != _bufferDisplayWidth || displayHeight != _bufferDisplayHeight)
    {
        if (!CreateRenderTexture(displayWidth, displayHeight, DXGI_FORMAT_R16G16B16A16_FLOAT, _history[0]) ||
            !CreateRenderTexture(displayWidth, displayHeight, DXGI_FORMAT_R16G16B16A16_FLOAT, _history[1]))
        {
            _bufferDisplayWidth = 0;
            return false;
        }

        _bufferDisplayWidth = displayWidth;
        _bufferDisplayHeight = displayHeight;
        recreated = true;
    }

    return true;
}

SGSR2FeatureDx11::ComPtr<ID3D11ShaderResourceView> SGSR2FeatureDx11::CreateInputSrv(ID3D11Resource* resource)
{
    ComPtr<ID3D11Texture2D> texture;
    if (FAILED(resource->QueryInterface(IID_PPV_ARGS(&texture))))
        return nullptr;

    D3D11_TEXTURE2D_DESC desc {};
    texture->GetDesc(&desc);

    D3D11_SHADER_RESOURCE_VIEW_DESC srvDesc {};
    srvDesc.Format = Shader_Dx11::TranslateTypelessFormats(desc.Format);
    srvDesc.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
    srvDesc.Texture2D.MipLevels = 1;

    ComPtr<ID3D11ShaderResourceView> srv;
    const auto hr = Device->CreateShaderResourceView(texture.Get(), &srvDesc, &srv);
    if (FAILED(hr))
    {
        LOG_ERROR("Format {} error: {:x}", static_cast<int>(desc.Format), static_cast<unsigned long>(hr));
        return nullptr;
    }

    return srv;
}

bool SGSR2FeatureDx11::EvaluateInternal(ID3D11DeviceContext* InContext, NVSDK_NGX_Parameter* InParameters)
{
    LOG_FUNC();

    if (!IsInited())
        return false;

    ID3D11Resource* paramColor = nullptr;
    ID3D11Resource* paramVelocity = nullptr;
    ID3D11Resource* paramDepth = nullptr;
    ID3D11Resource* paramOutput = nullptr;

    const auto getResource = [InParameters](const char* name, ID3D11Resource** resource)
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

    ComPtr<ID3D11Texture2D> colorTexture;
    ComPtr<ID3D11Texture2D> velocityTexture;
    ComPtr<ID3D11Texture2D> depthTexture;
    ComPtr<ID3D11Texture2D> outputTexture;

    if (FAILED(paramColor->QueryInterface(IID_PPV_ARGS(&colorTexture))) ||
        FAILED(paramVelocity->QueryInterface(IID_PPV_ARGS(&velocityTexture))) ||
        FAILED(paramDepth->QueryInterface(IID_PPV_ARGS(&depthTexture))) ||
        FAILED(paramOutput->QueryInterface(IID_PPV_ARGS(&outputTexture))))
    {
        LOG_ERROR("Inputs must be 2D textures");
        return false;
    }

    D3D11_TEXTURE2D_DESC colorDesc {};
    D3D11_TEXTURE2D_DESC velocityDesc {};
    D3D11_TEXTURE2D_DESC depthDesc {};
    D3D11_TEXTURE2D_DESC outputDesc {};

    colorTexture->GetDesc(&colorDesc);
    velocityTexture->GetDesc(&velocityDesc);
    depthTexture->GetDesc(&depthDesc);
    outputTexture->GetDesc(&outputDesc);

    if (!(outputDesc.BindFlags & D3D11_BIND_UNORDERED_ACCESS))
    {
        LOG_ERROR("Output has no unordered access, bind flags: {:x}", outputDesc.BindFlags);
        return false;
    }

    // Scene color isn't guaranteed to be readable by a shader
    if (!(colorDesc.BindFlags & D3D11_BIND_SHADER_RESOURCE))
    {
        D3D11_TEXTURE2D_DESC currentCopy {};
        if (_colorCopy != nullptr)
            _colorCopy->GetDesc(&currentCopy);

        if (currentCopy.Width != colorDesc.Width || currentCopy.Height != colorDesc.Height ||
            currentCopy.Format != colorDesc.Format)
        {
            auto copyDesc = colorDesc;
            copyDesc.BindFlags = D3D11_BIND_SHADER_RESOURCE;
            copyDesc.MiscFlags = 0;

            _colorCopy.Reset();
            const auto hr = Device->CreateTexture2D(&copyDesc, nullptr, &_colorCopy);
            if (FAILED(hr))
            {
                LOG_ERROR("Color copy error: {:x}", static_cast<unsigned long>(hr));
                return false;
            }
        }

        InContext->CopyResource(_colorCopy.Get(), colorTexture.Get());
        colorTexture = _colorCopy;
    }

    unsigned int renderWidth = 0;
    unsigned int renderHeight = 0;
    GetRenderResolution(InParameters, &renderWidth, &renderHeight);

    if (renderWidth == 0 || renderHeight == 0)
    {
        LOG_ERROR("Invalid render resolution: {}x{}", renderWidth, renderHeight);
        return false;
    }

    bool recreated = false;
    if (!EnsureBuffers(renderWidth, renderHeight, outputDesc.Width, outputDesc.Height, recreated))
        return false;

    unsigned int resetFlag = 0;
    InParameters->Get(NVSDK_NGX_Parameter_Reset, &resetFlag);
    const bool reset = resetFlag == 1 || recreated || _frameCount == 0;

    SGSR2FrameInfo frame {};
    frame.renderWidth = renderWidth;
    frame.renderHeight = renderHeight;
    frame.displayWidth = outputDesc.Width;
    frame.displayHeight = outputDesc.Height;
    frame.depthWidth = depthDesc.Width;
    frame.depthHeight = depthDesc.Height;
    frame.targetWidth = TargetWidth();
    frame.targetHeight = TargetHeight();
    frame.lowResMV = LowResMV();
    frame.jitteredMV = JitteredMV();
    frame.depthInverted = DepthInverted();
    frame.reset = reset;

    const auto constants = _constantBuilder.Build(InParameters, frame);

    auto colorSrv = CreateInputSrv(colorTexture.Get());
    auto depthSrv = CreateInputSrv(depthTexture.Get());
    auto velocitySrv = CreateInputSrv(velocityTexture.Get());

    if (colorSrv == nullptr || depthSrv == nullptr || velocitySrv == nullptr)
        return false;

    D3D11_UNORDERED_ACCESS_VIEW_DESC outputUavDesc {};
    outputUavDesc.Format = Shader_Dx11::TranslateTypelessFormats(outputDesc.Format);
    outputUavDesc.ViewDimension = D3D11_UAV_DIMENSION_TEXTURE2D;

    ComPtr<ID3D11UnorderedAccessView> outputUav;
    auto hr = Device->CreateUnorderedAccessView(outputTexture.Get(), &outputUavDesc, &outputUav);
    if (FAILED(hr))
    {
        LOG_ERROR("Output format {} error: {:x}", static_cast<int>(outputDesc.Format), static_cast<unsigned long>(hr));
        return false;
    }

    D3D11_MAPPED_SUBRESOURCE mapped {};
    hr = InContext->Map(_constantBuffer.Get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped);
    if (FAILED(hr))
    {
        LOG_ERROR("Map error: {:x}", static_cast<unsigned long>(hr));
        return false;
    }

    memcpy(mapped.pData, &constants, sizeof(constants));
    InContext->Unmap(_constantBuffer.Get(), 0);

    ComPtr<ID3D11ComputeShader> previousShader;
    InContext->CSGetShader(&previousShader, nullptr, nullptr);

    ID3D11Buffer* constantBuffers[] = { _constantBuffer.Get() };
    ID3D11SamplerState* samplers[] = { _linearClamp.Get(), _pointClamp.Get() };
    InContext->CSSetConstantBuffers(0, 1, constantBuffers);
    InContext->CSSetSamplers(0, 2, samplers);

    ID3D11ShaderResourceView* const nullSrvs[3] = {};
    ID3D11UnorderedAccessView* const nullUavs[2] = {};

    {
        ID3D11ShaderResourceView* srvs[] = { colorSrv.Get(), depthSrv.Get(), velocitySrv.Get() };
        ID3D11UnorderedAccessView* uavs[] = { _motionDepthClip.uav.Get(), _colorLuma.uav.Get() };

        InContext->CSSetShader(_convertShader.Get(), nullptr, 0);
        InContext->CSSetShaderResources(0, 3, srvs);
        InContext->CSSetUnorderedAccessViews(0, 2, uavs, nullptr);
        InContext->Dispatch((renderWidth + SGSR2ThreadGroupSize - 1) / SGSR2ThreadGroupSize,
                            (renderHeight + SGSR2ThreadGroupSize - 1) / SGSR2ThreadGroupSize, 1);
        InContext->CSSetUnorderedAccessViews(0, 2, nullUavs, nullptr);
        InContext->CSSetShaderResources(0, 3, nullSrvs);
    }

    {
        const auto& prevHistory = _history[_historyIndex];
        const auto& nextHistory = _history[_historyIndex ^ 1];

        ID3D11ShaderResourceView* srvs[] = { prevHistory.srv.Get(), _motionDepthClip.srv.Get(), _colorLuma.srv.Get() };
        ID3D11UnorderedAccessView* uavs[] = { outputUav.Get(), nextHistory.uav.Get() };

        InContext->CSSetShader(_upscaleShader.Get(), nullptr, 0);
        InContext->CSSetShaderResources(0, 3, srvs);
        InContext->CSSetUnorderedAccessViews(0, 2, uavs, nullptr);
        InContext->Dispatch((outputDesc.Width + SGSR2ThreadGroupSize - 1) / SGSR2ThreadGroupSize,
                            (outputDesc.Height + SGSR2ThreadGroupSize - 1) / SGSR2ThreadGroupSize, 1);
        InContext->CSSetUnorderedAccessViews(0, 2, nullUavs, nullptr);
        InContext->CSSetShaderResources(0, 3, nullSrvs);
    }

    _historyIndex ^= 1;

    // The base class restores everything but the shader itself
    InContext->CSSetShader(previousShader.Get(), nullptr, 0);

    return true;
}
