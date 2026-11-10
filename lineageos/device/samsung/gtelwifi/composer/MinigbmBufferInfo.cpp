// SPDX-License-Identifier: Apache-2.0
#include "bufferinfo/BufferInfo.h"
#include "bufferinfo/BufferInfoGetter.h"

#include <cstring>

namespace android::drm_hwcomposer {
namespace {
// Private gralloc0 ABI from minigbm-upstream/cros_gralloc/gralloc0/gralloc0.cc.
// GET_BUFFER_INFO returns borrowed fds, retained by GrallocBufferHandle.
struct MinigbmInfo {
  uint32_t drm_fourcc;
  int num_fds;
  int fds[4];
  uint64_t modifier;
  uint32_t offset[4];
  uint32_t stride[4];
};
class MinigbmBufferInfo : public LegacyBufferInfoGetter {
 public:
  int ValidateGralloc() override {
    return gralloc_->perform &&
                   std::strstr(gralloc_->common.name, "CrOS") != nullptr
               ? 0 : -EINVAL;
  }
  auto GetBoInfo(buffer_handle_t handle) -> std::optional<BufferInfo> override {
    MinigbmInfo info{};
    BufferInfo bi{};
    constexpr int kGetDimensions = 2;
    constexpr int kGetBufferInfo = 4;
    if (gralloc_->perform(gralloc_, kGetDimensions, handle, &bi.width, &bi.height) ||
        gralloc_->perform(gralloc_, kGetBufferInfo, handle, &info) ||
        info.num_fds < 1 || info.num_fds > kBufferMaxPlanes)
      return {};
    bi.format = info.drm_fourcc;
    for (int i = 0; i < kBufferMaxPlanes; ++i) bi.prime_fds[i] = -1;
    for (int i = 0; i < info.num_fds; ++i) {
      if (info.fds[i] < 0 || info.stride[i] == 0) return {};
      bi.prime_fds[i] = info.fds[i];
      bi.pitches[i] = info.stride[i];
      bi.offsets[i] = info.offset[i];
      bi.modifiers[i] = info.modifier;
    }
    return bi;
  }
};
}  // namespace
LEGACY_BUFFER_INFO_GETTER(MinigbmBufferInfo);
}  // namespace android::drm_hwcomposer
