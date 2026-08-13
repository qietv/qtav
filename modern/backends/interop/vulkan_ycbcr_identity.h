// SPDX-License-Identifier: LGPL-2.1-or-later

#pragma once

#include <array>

namespace qtav::detail {

// VkSamplerYcbcrConversion applies the driver-provided component mapping
// before the selected model. RGB_IDENTITY therefore exposes Vulkan's raw
// (Cr, Y, Cb) convention after this mapping. The shared normalization shader
// performs the one required conversion to libplacebo's (Y, Cb, Cr) order.
template <typename ComponentMapping>
constexpr ComponentMapping vulkanRawIdentityComponents(
    const ComponentMapping& components) noexcept
{
    return components;
}

template <typename Component>
constexpr std::array<Component, 3> normalizeVulkanRawYcbcrSample(
    const std::array<Component, 3>& sample) noexcept
{
    return { sample[1], sample[2], sample[0] };
}

inline constexpr char VulkanRawYcbcrShaderSwizzle[] = "gbr";

} // namespace qtav::detail
