#pragma once
#include <upscalers/IFeature_Dx12.h>

#include "SGSR1Constants.h"
#include "SGSR1Pass_Dx12.h"

// Snapdragon Game Super Resolution 1, a spatial upscaler with its own sharpening
class SGSR1FeatureDx12 : public IFeature_Dx12
{
  private:
    std::unique_ptr<SGSR1Pass_Dx12> _pass;

  public:
    SGSR1FeatureDx12(unsigned int InHandleId, NVSDK_NGX_Parameter* InParameters);

    bool InitInternal(ID3D12GraphicsCommandList* InCommandList, NVSDK_NGX_Parameter* InParameters) override;
    bool EvaluateInternal(ID3D12GraphicsCommandList* InCommandList, NVSDK_NGX_Parameter* InParameters) override;

    feature_version Version() override { return { 1, 0, 0 }; }
    Upscaler GetUpscalerType() const final { return Upscaler::SGSR1; }
    bool IsWithDx12() override { return false; }
};
