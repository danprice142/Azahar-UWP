// Copyright Citra Emulator Project / Azahar Emulator Project
// Licensed under GPLv2 or any later version
// Refer to the license.txt file included.

#pragma once

#include <array>
#include <atomic>
#include <mutex>
#include <string>

#include "core/frontend/applets/swkbd.h"

namespace UWP {

/// Interactive frontend for the emulated 3DS software-keyboard applet.
class SoftwareKeyboardUwp final : public Frontend::SoftwareKeyboard {
public:
    void Execute(const Frontend::KeyboardConfig& config) override;
    void ShowError(const std::string& error) override;

    void Draw();

    bool IsOpen() const {
        return open.load(std::memory_order_acquire);
    }

private:
    void Submit(u8 button, bool is_ok);

    mutable std::mutex state_mutex;
    std::atomic_bool open{false};
    std::atomic_bool popup_pending{false};
    bool focus_input_pending = false;
    std::array<char, 4096> buffer{};
    std::string error_text;
};

} // namespace UWP
