// SPDX-License-Identifier: Apache-2.0
#include "backend/GenericBackend.h"
#include "bufferinfo/BufferInfoGetter.h"
#include "compositor/ICompositorDisplay.h"
#include "compositor/FlatteningController.h"
#include "hwc/HwcLayer.h"

#include <android-base/properties.h>

namespace android::drm_hwcomposer {
namespace {
class SprdPlanner : public CompositionPlanner {
 public:
  auto ValidateDisplay(const ICompositorDisplay* display) -> ValidatedComposition override {
    const auto layers = display->GetOrderLayersByZPos();
    auto fallback = GetFlattenedComposition(layers, FlattenReason::kNone);
    if (layers.empty() || display->CtmByGpu() ||
        !android::base::GetBoolProperty("vendor.gtelwifi.overlay", true))
      return fallback;
    const auto* flat = display->GetFlatCon();
    if (flat && flat->ShouldFlatten()) return fallback;

    // Only the lowest Android layer may use the opaque IMG plane. All layers
    // above it form one premultiplied client target on OSD, never another hole.
    const auto* bottom = layers.front();
    const auto& data = bottom->GetLayerData();
    if (bottom->GetSfType() != CompositionType::kDevice ||
        !bottom->IsLayerUsableAsDevice() || !data.bi ||
        data.bi->blend_mode != BufferBlendMode::kNone || data.pi.alpha != kAlphaOpaque ||
        data.pi.transform.rotate90 || data.pi.transform.hflip || data.pi.transform.vflip)
      return fallback;
    switch (data.bi->format) {
      case DRM_FORMAT_XRGB8888: case DRM_FORMAT_XBGR8888:
      case DRM_FORMAT_ARGB8888: case DRM_FORMAT_ABGR8888: break;
      default: return fallback;
    }
    const auto [width, height] = display->GetSize();
    const auto src = data.pi.source_crop.f_rect.value_or(
        FRect{0, 0, float(data.bi->width), float(data.bi->height)});
    const auto dst = data.pi.display_frame.i_rect.value_or(
        IRect{0, 0, int32_t(width), int32_t(height)});
    if (src.Width() != dst.Width() || src.Height() != dst.Height() ||
        src.left != std::floor(src.left) || src.top != std::floor(src.top))
      return fallback;

    ValidatedComposition candidate{};
    for (const auto* layer : layers)
      candidate.composition_types[layer] = layer == bottom ? CompositionType::kDevice
                                                         : CompositionType::kClient;
    // TEST_ONLY checks the real pitch, crops, placement and both plane limits.
    if (display->TestComposition(candidate)) return candidate;
    return fallback;
  }
};
class SprdBackend : public GenericBackend {
 public:
  SprdBackend() : GenericBackend("sprd-dispc") {}
  std::unique_ptr<CompositionPlanner> CreateCompositionPlanner() override {
    return std::make_unique<SprdPlanner>();
  }
  std::unique_ptr<BufferInfoGetter> CreateBufferInfoGetter() override {
    return LegacyBufferInfoGetter::CreateInstance();
  }
};
SprdBackend backend;
}  // namespace
}  // namespace android::drm_hwcomposer
