// SPDX-License-Identifier: LGPL-2.1-or-later
#pragma once

#include <native_window/external_window.h>

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>

namespace qtav::ohos_example {

class VVCValidationSession final {
public:
    VVCValidationSession();
    ~VVCValidationSession();

    VVCValidationSession(const VVCValidationSession&) = delete;
    VVCValidationSession& operator=(const VVCValidationSession&) = delete;

    bool start(
        const std::uint8_t* media,
        std::size_t size,
        OHNativeWindow* window);
    void setSurface(OHNativeWindow* window);
    void clearSurface();
    void setForeground(bool foreground);
    void stop();

    bool active() const noexcept;
    std::string status() const;

private:
    class Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace qtav::ohos_example
