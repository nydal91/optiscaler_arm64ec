#pragma once
#include <upscalers/IFeature_Vk.h>

#include "SGSR2Constants.h"
#include "SGSR2Passes_Vk.h"

// Snapdragon Game Super Resolution 2, 2-pass compute variant
class SGSR2FeatureVk : public IFeature_Vk
{
  private:
    std::unique_ptr<SGSR2Passes_Vk> _passes;
    SGSR2ConstantBuilder _constantBuilder;

  protected:
    bool InitInternal(VkCommandBuffer InCmdBuffer, NVSDK_NGX_Parameter* InParameters) override;
    bool EvaluateInternal(VkCommandBuffer InCmdBuffer, NVSDK_NGX_Parameter* InParameters) override;

  public:
    SGSR2FeatureVk(unsigned int InHandleId, NVSDK_NGX_Parameter* InParameters);

    feature_version Version() override { return { 2, 0, 0 }; }
    Upscaler GetUpscalerType() const final { return Upscaler::SGSR2; }
    bool IsWithDx12() final { return false; }
};
