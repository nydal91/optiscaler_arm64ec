#pragma once
#include <upscalers/IFeature_Dx11.h>

#include "SGSR1Constants.h"

#include <wrl/client.h>

// Snapdragon Game Super Resolution 1, a spatial upscaler with its own sharpening
class SGSR1FeatureDx11 : public IFeature_Dx11
{
  private:
    template <typename T> using ComPtr = Microsoft::WRL::ComPtr<T>;

    ComPtr<ID3D11ComputeShader> _upscaleShader;
    ComPtr<ID3D11Buffer> _constantBuffer;
    ComPtr<ID3D11SamplerState> _linearClamp;
    ComPtr<ID3D11Texture2D> _colorCopy;

  public:
    SGSR1FeatureDx11(unsigned int InHandleId, NVSDK_NGX_Parameter* InParameters);

    bool InitInternal(ID3D11DeviceContext* InContext, NVSDK_NGX_Parameter* InParameters) override;
    bool EvaluateInternal(ID3D11DeviceContext* InContext, NVSDK_NGX_Parameter* InParameters) override;

    feature_version Version() override { return { 1, 0, 0 }; }
    Upscaler GetUpscalerType() const final { return Upscaler::SGSR1; }
    bool IsWithDx12() override { return false; }
};
