#include <pch.h>
#include <Config.h>
#include <Util.h>
#include "SGSR1Feature_Dx11.h"

#include <shaders/Shader_Dx11.h>

#include "generated/sgsr1_upscale_cso.h"

SGSR1FeatureDx11::SGSR1FeatureDx11(unsigned int InHandleId, NVSDK_NGX_Parameter* InParameters)
    : IFeature(InHandleId, InParameters), IFeature_Dx11(InHandleId, InParameters)
{
    _initParameters = SetInitParameters(InParameters);
    _moduleLoaded = true;
}

bool SGSR1FeatureDx11::InitInternal(ID3D11DeviceContext* InContext, NVSDK_NGX_Parameter* InParameters)
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

    auto hr = Device->CreateComputeShader(sgsr1_upscale_cso, sizeof(sgsr1_upscale_cso), nullptr, &_upscaleShader);
    if (FAILED(hr))
    {
        LOG_ERROR("Upscale shader error: {:x}", static_cast<unsigned long>(hr));
        return false;
    }

    D3D11_BUFFER_DESC bufferDesc {};
    bufferDesc.ByteWidth = (sizeof(SGSR1Constants) + 15) & ~15u;
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
    samplerDesc.Filter = D3D11_FILTER_MIN_MAG_MIP_LINEAR;
    samplerDesc.AddressU = D3D11_TEXTURE_ADDRESS_CLAMP;
    samplerDesc.AddressV = D3D11_TEXTURE_ADDRESS_CLAMP;
    samplerDesc.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
    samplerDesc.ComparisonFunc = D3D11_COMPARISON_NEVER;
    samplerDesc.MaxLOD = D3D11_FLOAT32_MAX;

    hr = Device->CreateSamplerState(&samplerDesc, &_linearClamp);
    if (FAILED(hr))
    {
        LOG_ERROR("Sampler error: {:x}", static_cast<unsigned long>(hr));
        return false;
    }

    SetInit(true);
    return true;
}

bool SGSR1FeatureDx11::EvaluateInternal(ID3D11DeviceContext* InContext, NVSDK_NGX_Parameter* InParameters)
{
    LOG_FUNC();

    if (!IsInited())
        return false;

    ID3D11Resource* paramColor = nullptr;
    ID3D11Resource* paramOutput = nullptr;

    const auto getResource = [InParameters](const char* name, ID3D11Resource** resource)
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

    ComPtr<ID3D11Texture2D> colorTexture;
    ComPtr<ID3D11Texture2D> outputTexture;

    if (FAILED(paramColor->QueryInterface(IID_PPV_ARGS(&colorTexture))) ||
        FAILED(paramOutput->QueryInterface(IID_PPV_ARGS(&outputTexture))))
    {
        LOG_ERROR("Inputs must be 2D textures");
        return false;
    }

    D3D11_TEXTURE2D_DESC colorDesc {};
    D3D11_TEXTURE2D_DESC outputDesc {};

    colorTexture->GetDesc(&colorDesc);
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

    unsigned int outputWidth = 0;
    unsigned int outputHeight = 0;
    GetOutputExtent(outputDesc.Width, outputDesc.Height, &outputWidth, &outputHeight);

    const SGSR1FrameInfo frame { renderWidth, renderHeight, colorDesc.Width, colorDesc.Height,
                                 outputWidth, outputHeight, IsHdr() };
    const auto constants = BuildSGSR1Constants(InParameters, frame);

    D3D11_SHADER_RESOURCE_VIEW_DESC colorSrvDesc {};
    colorSrvDesc.Format = Shader_Dx11::TranslateTypelessFormats(colorDesc.Format);
    colorSrvDesc.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
    colorSrvDesc.Texture2D.MipLevels = 1;

    ComPtr<ID3D11ShaderResourceView> colorSrv;
    auto hr = Device->CreateShaderResourceView(colorTexture.Get(), &colorSrvDesc, &colorSrv);
    if (FAILED(hr))
    {
        LOG_ERROR("Color format {} error: {:x}", static_cast<int>(colorDesc.Format), static_cast<unsigned long>(hr));
        return false;
    }

    D3D11_UNORDERED_ACCESS_VIEW_DESC outputUavDesc {};
    outputUavDesc.Format = Shader_Dx11::TranslateTypelessFormats(outputDesc.Format);
    outputUavDesc.ViewDimension = D3D11_UAV_DIMENSION_TEXTURE2D;

    ComPtr<ID3D11UnorderedAccessView> outputUav;
    hr = Device->CreateUnorderedAccessView(outputTexture.Get(), &outputUavDesc, &outputUav);
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
    ID3D11SamplerState* samplers[] = { _linearClamp.Get() };
    ID3D11ShaderResourceView* srvs[] = { colorSrv.Get() };
    ID3D11UnorderedAccessView* uavs[] = { outputUav.Get() };

    InContext->CSSetShader(_upscaleShader.Get(), nullptr, 0);
    InContext->CSSetConstantBuffers(0, 1, constantBuffers);
    InContext->CSSetSamplers(0, 1, samplers);
    InContext->CSSetShaderResources(0, 1, srvs);
    InContext->CSSetUnorderedAccessViews(0, 1, uavs, nullptr);
    InContext->Dispatch((outputWidth + SGSR1ThreadGroupSize - 1) / SGSR1ThreadGroupSize,
                        (outputHeight + SGSR1ThreadGroupSize - 1) / SGSR1ThreadGroupSize, 1);

    ID3D11ShaderResourceView* const nullSrvs[1] = {};
    ID3D11UnorderedAccessView* const nullUavs[1] = {};
    InContext->CSSetUnorderedAccessViews(0, 1, nullUavs, nullptr);
    InContext->CSSetShaderResources(0, 1, nullSrvs);

    // The base class restores everything but the shader itself
    InContext->CSSetShader(previousShader.Get(), nullptr, 0);

    return true;
}
