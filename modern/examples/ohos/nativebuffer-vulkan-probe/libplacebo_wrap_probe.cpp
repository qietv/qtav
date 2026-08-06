// SPDX-License-Identifier: CC0-1.0

#include "libplacebo_wrap_probe.h"

namespace ohos_native_buffer_probe {

bool wrapWithLibplacebo(void* user, const ImportedImage& image)
{
    auto* context = static_cast<LibplaceboContext*>(user);
    if (!context || !context->gpu || !context->log
        || image.format == VK_FORMAT_UNDEFINED) {
        return false;
    }

    pl_vulkan_wrap_params params {};
    params.image = image.image;
    params.width = static_cast<int>(image.width);
    params.height = static_cast<int>(image.height);
    params.format = image.format;
    params.usage = image.usage;
    params.debug_tag = "OHOS forced VkFormat probe";
    pl_tex texture = pl_vulkan_wrap(context->gpu, &params);
    if (!texture) {
        context->log("pl_vulkan_wrap", VK_ERROR_FORMAT_NOT_SUPPORTED);
        return false;
    }

    const bool hasTwoPlanes = texture->params.format
        && texture->params.format->num_planes == 2
        && texture->planes[0]
        && texture->planes[1];
    context->log(
        "pl_vulkan_wrap(two explicit planes)",
        hasTwoPlanes ? VK_SUCCESS : VK_ERROR_FORMAT_NOT_SUPPORTED);
    pl_tex_destroy(context->gpu, &texture);
    return hasTwoPlanes;
}

} // namespace ohos_native_buffer_probe
