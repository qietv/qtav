// SPDX-License-Identifier: LGPL-2.1-or-later

#include "vulkan_ycbcr_identity.h"

#include <array>
#include <cstdlib>
#include <fstream>
#include <iostream>
#include <iterator>
#include <string>

namespace {

enum class Swizzle {
    Identity,
    R,
    G,
    B,
    A,
};

struct ComponentMapping {
    Swizzle r;
    Swizzle g;
    Swizzle b;
    Swizzle a;
};

constexpr bool operator==(
    const ComponentMapping& left,
    const ComponentMapping& right) noexcept
{
    return left.r == right.r && left.g == right.g
        && left.b == right.b && left.a == right.a;
}

void expect(bool condition, const char* message)
{
    if (!condition) {
        std::cerr << message << '\n';
        std::exit(1);
    }
}

std::string readSource(const char* path, const char* message)
{
    std::ifstream input(path);
    expect(input.good(), message);
    return {
        std::istreambuf_iterator<char> { input },
        std::istreambuf_iterator<char> {},
    };
}

void testPlatformIdentityMapping(const char* platform)
{
    constexpr ComponentMapping driverMapping {
        Swizzle::B,
        Swizzle::G,
        Swizzle::R,
        Swizzle::A,
    };
    constexpr ComponentMapping rawIdentity =
        qtav::detail::vulkanRawIdentityComponents(
            driverMapping);
    if (!(rawIdentity == driverMapping)) {
        std::cerr << platform
                  << " raw identity sampler changed the driver component mapping\n";
        std::exit(1);
    }
}

void testSharedNormalization()
{
    constexpr std::array<int, 3> vulkanRaw { 30, 10, 20 };
    constexpr std::array<int, 3> expectedYcbcr { 10, 20, 30 };
    constexpr auto normalized =
        qtav::detail::normalizeVulkanRawYcbcrSample(vulkanRaw);
    static_assert(
        normalized[0] == expectedYcbcr[0]
        && normalized[1] == expectedYcbcr[1]
        && normalized[2] == expectedYcbcr[2]);

    const std::string source = readSource(
        QTAV_VULKAN_EXTERNAL_NORMALIZER_SHADER,
        "Could not open the Vulkan normalization shader");
    const std::string expected = std::string("sampleValue.")
        + qtav::detail::VulkanRawYcbcrShaderSwizzle;
    expect(
        source.find(expected) != std::string::npos,
        "The Vulkan normalization shader no longer applies the raw "
        "YCbCr swizzle");
}

void testPlatformCallSites()
{
    const std::string android = readSource(
        QTAV_MEDIACODEC_VULKAN_SOURCE,
        "Could not open the MediaCodec Vulkan interop source");
    expect(
        android.find(
            "key.components = detail::vulkanRawIdentityComponents")
            != std::string::npos,
        "Android raw identity sampler bypasses the shared component "
        "mapping contract");
    expect(
        android.find("explicitSwizzle") == std::string::npos,
        "Android raw identity sampler pre-rotates the component mapping");

    const std::string ohos = readSource(
        QTAV_OHCODEC_VULKAN_SOURCE,
        "Could not open the OHCodec Vulkan interop source");
    expect(
        ohos.find(
            "identityInfo.components = detail::vulkanRawIdentityComponents")
            != std::string::npos,
        "OHOS raw identity sampler bypasses the shared component mapping "
        "contract");
}

} // namespace

int main()
{
    testPlatformIdentityMapping("Android");
    testPlatformIdentityMapping("OHOS");
    testSharedNormalization();
    testPlatformCallSites();
    return 0;
}
