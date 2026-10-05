// Copyright Citra Emulator Project / Azahar Emulator Project
// Licensed under GPLv2 or any later version
// Refer to the license.txt file included.

#pragma once

#include <algorithm>
#include <array>
#include <span>
#include <string>

#include <imgui.h>

#include "citra_uwp/ui/virtual_keyboard.h"
#include "common/settings.h"

namespace UWP::Widgets {

/// Thin ImGui wrappers over Setting<T, ranged>/SwitchableSetting<T, ranged> that read and write
/// the setting in place. They are templates because `ranged` is part of the type.

void Tooltip(const char* text);

void GroupHeader(const char* label);

/// Records popup ownership before ImGui processes this frame's navigation input.
void LatchPopupInputOwner();

/// Closes `is_open` on B while the window has focus, deferring to any open popup, combo or field.
void HandleGamepadBack(bool& is_open);

/// Positions the next window to fill the viewport's work area.
void SetNextWindowFullArea();

/// Combo box over a contiguous enum stored as an int. `items` is indexed by the enum value.
bool Combo(const char* label, int& value, std::span<const char* const> items,
           const char* tooltip = nullptr);

template <bool ranged>
bool SettingCheckbox(const char* label, Settings::Setting<bool, ranged>& setting,
                     const char* tooltip = nullptr) {
    bool value = setting.GetValue();
    const bool changed = ImGui::Checkbox(label, &value);
    Tooltip(tooltip);
    if (changed) {
        setting.SetValue(value);
    }
    return changed;
}

/// Slider over any arithmetic setting; bounds are double so call sites can pass plain literals.
template <typename Type, bool ranged>
bool SettingSlider(const char* label, Settings::Setting<Type, ranged>& setting, double min,
                   double max, const char* format = nullptr, const char* tooltip = nullptr) {
    bool changed = false;
    if constexpr (std::is_floating_point_v<Type>) {
        auto value = static_cast<float>(setting.GetValue());
        changed = ImGui::SliderFloat(label, &value, static_cast<float>(min),
                                     static_cast<float>(max), format != nullptr ? format : "%.2f");
        if (changed) {
            setting.SetValue(static_cast<Type>(value));
        }
    } else {
        auto value = static_cast<int>(setting.GetValue());
        changed = ImGui::SliderInt(label, &value, static_cast<int>(min), static_cast<int>(max),
                                   format != nullptr ? format : "%d");
        if (changed) {
            setting.SetValue(static_cast<Type>(value));
        }
    }
    Tooltip(tooltip);
    return changed;
}

template <typename EnumType, bool ranged>
bool SettingEnumCombo(const char* label, Settings::Setting<EnumType, ranged>& setting,
                      std::span<const char* const> items, const char* tooltip = nullptr) {
    int value = static_cast<int>(setting.GetValue());
    if (Combo(label, value, items, tooltip)) {
        setting.SetValue(static_cast<EnumType>(value));
        return true;
    }
    return false;
}

template <bool ranged>
bool SettingInputText(const char* label, Settings::Setting<std::string, ranged>& setting,
                      const char* tooltip = nullptr) {
    // 512 exceeds every string setting in use.
    std::array<char, 512> buffer{};
    const std::string& current = setting.GetValue();
    const auto length = std::min(current.size(), buffer.size() - 1);
    std::copy_n(current.begin(), length, buffer.begin());

    // Routed through the on-screen keyboard; a raw ImGui text field cannot be typed into on a
    // console.
    const bool changed = GetVirtualKeyboard().InputText(label, buffer.data(), buffer.size());
    Tooltip(tooltip);
    if (changed) {
        setting.SetValue(std::string{buffer.data()});
    }
    return changed;
}

} // namespace UWP::Widgets
