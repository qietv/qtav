// SPDX-License-Identifier: LGPL-2.1-or-later
#pragma once

#include <string>

#include <qtav/frame.h>

namespace qtav::test {

bool runOpenGLOffscreenRendererChecks(
    const VideoFrame& softwareFrame,
    const VideoFrame& hdrFrame,
    std::string& error);

} // namespace qtav::test
