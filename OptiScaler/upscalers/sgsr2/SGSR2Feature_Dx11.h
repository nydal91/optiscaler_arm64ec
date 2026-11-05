#pragma once
#include <upscalers/IFeature_Dx11.h>

#include "SGSR2Constants.h"

#include <wrl/client.h>

// Snapdragon Game Super Resolution 2, 2-pass compute variant
class SGSR2FeatureDx11 : public IFeature_Dx11
{
  private:
    template <typename T> using ComPtr = Microsoft::WRL::ComPtr<T>;

    struct RenderTexture
    {
        ComPtr<ID3D11Texture2D> texture;
        ComPtr<ID3D11ShaderResourceView> srv;
        ComPtr<ID3D11UnorderedAccessView> uav;
    };

    ComPtr<ID3D11ComputeShader> _convertShader;
    ComPtr<ID3D11ComputeShader> _upscaleShader;
    ComPtr<ID3D11Buffer> _constantBuffer;
    ComPtr<ID3D11SamplerState> _linearClamp;
    ComPtr<ID3D11SamplerState> _pointClamp;

    RenderTexture _motionDepthClip;
    RenderTexture _colorLuma;
    RenderTexture _history[2];
    ComPtr<ID3D11Texture2D> _colorCopy;

    uint32_t _bufferRenderWidth = 0;
    uint32_t _bufferRenderHeight = 0;
    uint32_t _bufferDisplayWidth = 0;
    uint32_t _bufferDisplayHeight = 0;
    uint32_t _historyIndex = 0;

    SGSR2ConstantBuilder _constantBuilder;

    bool CreateRenderTexture(uint32_t width, uint32_t height, DXGI_FORMAT format, RenderTexture& out);
    bool EnsureBuffers(uint32_t renderWidth, uint32_t renderHeight, uint32_t displayWidth, uint32_t displayHeight,
                       bool& recreated);
    ComPtr<ID3D11ShaderResourceView> CreateInputSrv(ID3D11Resource* resource);

  public:
    SGSR2FeatureDx11(unsigned int InHandleId, NVSDK_NGX_Parameter* InParameters);

    bool InitInternal(ID3D11DeviceContext* InContext, NVSDK_NGX_Parameter* InParameters) override;
    bool EvaluateInternal(ID3D11DeviceContext* InContext, NVSDK_NGX_Parameter* InParameters) override;

    feature_version Version() override { return { 2, 0, 0 }; }
    Upscaler GetUpscalerType() const final { return Upscaler::SGSR2; }
    bool IsWithDx12() override { return false; }
};
