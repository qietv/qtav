// SPDX-License-Identifier: CC0-1.0
#pragma once

#include "native_buffer_vulkan_probe.h"

#include <libplacebo/vulkan.h>

namespace ohos_native_buffer_probe {

struct LibplaceboContext {
    pl_gpu gpu = nullptr;
    Log log = nullptr;
};

// Consumer callback for probeNativeBuffer(). It verifies that libplacebo can
// map the explicit VkFormat and create its two plane textures. The full device
// run additionally rendered those planes; this small callback isolates only
// the libplacebo acceptance boundary discussed with Huawei.
bool wrapWithLibplacebo(void* user, const ImportedImage& image);

} // namespace ohos_native_buffer_probe
