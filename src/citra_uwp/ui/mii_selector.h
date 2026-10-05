// Copyright Citra Emulator Project / Azahar Emulator Project
// Licensed under GPLv2 or any later version
// Refer to the license.txt file included.

#pragma once

#include <atomic>
#include <string>
#include <vector>

#include "core/frontend/applets/mii_selector.h"

namespace UWP {

/// Xbox/gamepad-driven implementation of the Mii Selector applet.
class MiiSelectorUwp final : public Frontend::MiiSelector {
public:
    void Setup(const Frontend::MiiSelectorConfig& config) override;

    void Draw();

    bool IsOpen() const {
        return open.load(std::memory_order_acquire);
    }

private:
    std::atomic_bool open{false};
    std::atomic_bool popup_pending{false};
    int selected = 0;
    std::vector<Mii::MiiData> miis;
    std::vector<std::string> names;
};

} // namespace UWP
