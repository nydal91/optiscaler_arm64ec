#pragma once
#include <upscalers/IFeature_Dx12.h>

#include "SGSR2Constants.h"
#include "SGSR2Passes_Dx12.h"

// Snapdragon Game Super Resolution 2, 2-pass compute variant
class SGSR2FeatureDx12 : public IFeature_Dx12
{
  private:
    std::unique_ptr<SGSR2Passes_Dx12> _passes;
    SGSR2ConstantBuilder _constantBuilder;

  public:
    SGSR2FeatureDx12(unsigned int InHandleId, NVSDK_NGX_Parameter* InParameters);

    bool InitInternal(ID3D12GraphicsCommandList* InCommandList, NVSDK_NGX_Parameter* InParameters) override;
    bool EvaluateInternal(ID3D12GraphicsCommandList* InCommandList, NVSDK_NGX_Parameter* InParameters) override;

    feature_version Version() override { return { 2, 0, 0 }; }
    Upscaler GetUpscalerType() const final { return Upscaler::SGSR2; }
    bool IsWithDx12() override { return false; }
};
