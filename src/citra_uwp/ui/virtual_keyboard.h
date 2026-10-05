// Copyright Citra Emulator Project / Azahar Emulator Project
// Licensed under GPLv2 or any later version
// Refer to the license.txt file included.

#pragma once

#include <algorithm>
#include <array>
#include <cstddef>
#include <string>

#include "common/settings.h"

struct ImVec2;

namespace UWP {

/// ImGui text fields backed by SDL's WinRT on-screen keyboard support.
class VirtualKeyboard {
public:
    bool InputText(const char* label, char* buffer, std::size_t buffer_size,
                   const char* hint = nullptr, bool password = false);

    bool InputTextMultiline(const char* label, char* buffer, std::size_t buffer_size,
                            const ImVec2& size);

    template <bool ranged>
    bool InputText(const char* label, Settings::Setting<std::string, ranged>& setting,
                   const char* hint = nullptr, bool password = false) {
        std::array<char, 512> buffer{};
        const std::string& current = setting.GetValue();
        const auto length = std::min(current.size(), buffer.size() - 1);
        std::copy_n(current.begin(), length, buffer.begin());

        if (InputText(label, buffer.data(), buffer.size(), hint, password)) {
            setting.SetValue(std::string{buffer.data()});
            return true;
        }
        return false;
    }

    bool IsOpen() const;
};

VirtualKeyboard& GetVirtualKeyboard();

} // namespace UWP
