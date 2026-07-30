// SPDX-License-Identifier: LGPL-2.1-or-later

#include <qtav/android_opengl_video_renderer.h>

extern "C" __attribute__((visibility("default")))
int qtav_android_opengl_install_consumer()
{
    qtav::AndroidOpenGLVideoRenderer renderer;
    const qtav::VideoRenderCapabilities capabilities =
        renderer.capabilities();
    return capabilities.customViewport && capabilities.rotation
        ? 0
        : 1;
}
