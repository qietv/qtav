// SPDX-License-Identifier: LGPL-2.1-or-later
#pragma once

#include <string>

#include <qtav/frame.h>
#include <qtav/vulkan_video_renderer.h>

namespace qtav::test {

bool runVulkanOffscreenRendererChecks(
    BorrowedVulkanDevice device,
    const VideoFrame& frame,
    std::string& error);

} // namespace qtav::test
