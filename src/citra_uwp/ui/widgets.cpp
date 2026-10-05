// Copyright Citra Emulator Project / Azahar Emulator Project
// Licensed under GPLv2 or any later version
// Refer to the license.txt file included.

#include "citra_uwp/ui/widgets.h"

namespace UWP::Widgets {

namespace {
bool popup_owned_input_at_frame_start = false;
}

void Tooltip(const char* text) {
    if (text == nullptr) {
        return;
    }
    // ImGuiHoveredFlags_ForTooltip also covers gamepad focus.
    if (ImGui::IsItemHovered(ImGuiHoveredFlags_ForTooltip)) {
        ImGui::SetTooltip("%s", text);
    }
}

void GroupHeader(const char* label) {
    ImGui::Spacing();
    ImGui::TextDisabled("%s", label);
    ImGui::Separator();
    ImGui::Spacing();
}

void LatchPopupInputOwner() {
    popup_owned_input_at_frame_start = ImGui::IsPopupOpen("", ImGuiPopupFlags_AnyPopup);
}

void HandleGamepadBack(bool& is_open) {
    if (!ImGui::IsWindowFocused(ImGuiFocusedFlags_RootAndChildWindows)) {
        return;
    }
    // A modal popup is submitted by its parent window, which still reports itself focused.
    if (popup_owned_input_at_frame_start || ImGui::IsPopupOpen("", ImGuiPopupFlags_AnyPopup)) {
        return;
    }
    // An active item (open combo or edited field) owns B.
    if (ImGui::IsAnyItemActive()) {
        return;
    }
    if (ImGui::IsKeyPressed(ImGuiKey_GamepadFaceRight, /*repeat=*/false)) {
        is_open = false;
    }
}

void SetNextWindowFullArea() {
    const ImGuiViewport* viewport = ImGui::GetMainViewport();
    ImGui::SetNextWindowPos(viewport->WorkPos);
    ImGui::SetNextWindowSize(viewport->WorkSize);
}

bool Combo(const char* label, int& value, std::span<const char* const> items, const char* tooltip) {
    const int count = static_cast<int>(items.size());
    // A value outside the list (e.g. from a newer config) shows nothing selected, as in Qt.
    const char* preview = (value >= 0 && value < count) ? items[value] : "";

    bool changed = false;
    if (ImGui::BeginCombo(label, preview)) {
        for (int i = 0; i < count; ++i) {
            const bool selected = (value == i);
            if (ImGui::Selectable(items[i], selected)) {
                value = i;
                changed = true;
            }
            if (selected) {
                // Keeps gamepad navigation anchored on the current entry when the list opens.
                ImGui::SetItemDefaultFocus();
            }
        }
        ImGui::EndCombo();
    }
    Tooltip(tooltip);
    return changed;
}

} // namespace UWP::Widgets
