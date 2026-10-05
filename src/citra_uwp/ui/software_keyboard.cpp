// Copyright Citra Emulator Project / Azahar Emulator Project
// Licensed under GPLv2 or any later version
// Refer to the license.txt file included.

#include <imgui.h>

#include "citra_uwp/ui/software_keyboard.h"
#include "citra_uwp/ui/virtual_keyboard.h"
#include "common/logging/log.h"

namespace UWP {

namespace {

const char* DescribeValidationError(Frontend::ValidationError error) {
    using Frontend::ValidationError;
    switch (error) {
    case ValidationError::MaxDigitsExceeded:
        return "Too many digits.";
    case ValidationError::AtSignNotAllowed:
        return "The @ sign is not allowed here.";
    case ValidationError::PercentNotAllowed:
        return "The % sign is not allowed here.";
    case ValidationError::BackslashNotAllowed:
        return "The \\ sign is not allowed here.";
    case ValidationError::ProfanityNotAllowed:
        return "That text is not allowed.";
    case ValidationError::CallbackFailed:
        return "The application rejected this input.";
    case ValidationError::FixedLengthRequired:
        return "The input must have the exact required length.";
    case ValidationError::MaxLengthExceeded:
        return "The input is too long.";
    case ValidationError::BlankInputNotAllowed:
        return "Blank input is not allowed.";
    case ValidationError::EmptyInputNotAllowed:
        return "Empty input is not allowed.";
    default:
        return "The input is not valid.";
    }
}

} // namespace

void SoftwareKeyboardUwp::Execute(const Frontend::KeyboardConfig& config_) {
    std::scoped_lock lock{state_mutex};
    // CallbackResult::Continue calls ShowError() and then Execute() again; keep the rejected
    // text.
    const bool reopening_after_callback =
        open.load(std::memory_order_acquire) && !error_text.empty();
    SoftwareKeyboard::Execute(config_);
    if (!reopening_after_callback) {
        buffer.fill('\0');
        error_text.clear();
    }
    focus_input_pending = true;
    popup_pending.store(true, std::memory_order_relaxed);
    open.store(true, std::memory_order_release);
    LOG_INFO(Frontend, "Software keyboard requested (buttons {})",
             static_cast<int>(config.button_config));
}

void SoftwareKeyboardUwp::ShowError(const std::string& error) {
    std::scoped_lock lock{state_mutex};
    // Raised when the app-side callback rejects the text; reopen with the message.
    error_text = error;
    focus_input_pending = true;
    popup_pending.store(true, std::memory_order_relaxed);
    open.store(true, std::memory_order_release);
}

void SoftwareKeyboardUwp::Submit(u8 button, bool is_ok) {
    const Frontend::ValidationError error =
        Finalize(is_ok ? std::string{buffer.data()} : std::string{}, button);
    if (is_ok && error != Frontend::ValidationError::None) {
        error_text = DescribeValidationError(error);
        focus_input_pending = true;
        return;
    }
    open.store(false, std::memory_order_release);
    ImGui::CloseCurrentPopup();
}

void SoftwareKeyboardUwp::Draw() {
    if (!open.load(std::memory_order_acquire)) {
        return;
    }
    std::scoped_lock lock{state_mutex};
    if (!open.load(std::memory_order_acquire)) {
        return;
    }
    if (popup_pending.exchange(false, std::memory_order_acq_rel)) {
        ImGui::OpenPopup("Keyboard");
    }
    if (!ImGui::BeginPopupModal("Keyboard", nullptr,
                                ImGuiWindowFlags_AlwaysAutoResize |
                                    ImGuiWindowFlags_NoSavedSettings)) {
        return;
    }

    if (!config.hint_text.empty()) {
        ImGui::TextDisabled("%s", config.hint_text.c_str());
    }
    if (focus_input_pending) {
        ImGui::SetKeyboardFocusHere();
        focus_input_pending = false;
    }
    bool text_changed = false;
    if (config.multiline_mode) {
        text_changed = GetVirtualKeyboard().InputTextMultiline(
            "##swkbd_text", buffer.data(), buffer.size(), ImVec2(520.0f, 180.0f));
    } else {
        text_changed = GetVirtualKeyboard().InputText("##swkbd_text", buffer.data(), buffer.size());
    }
    if (text_changed) {
        // Clear a stale error once the user starts correcting the value.
        error_text.clear();
    }
    if (!error_text.empty()) {
        ImGui::TextColored(ImVec4(1.0f, 0.4f, 0.4f, 1.0f), "%s", error_text.c_str());
    }
    ImGui::Spacing();

    // button_text uses hardware slots ([0] Cancel, [1] I Forgot, [2] OK); result indices are
    // compact.
    const auto button_label = [this](std::size_t index, const char* fallback) {
        return config.button_text.size() > index && !config.button_text[index].empty()
                   ? config.button_text[index].c_str()
                   : fallback;
    };
    // A Single/None keyboard has no cancel button on hardware either, so B does nothing there.
    const bool back_pressed =
        !ImGui::IsAnyItemActive() && ImGui::IsKeyPressed(ImGuiKey_GamepadFaceRight, false);
    switch (config.button_config) {
    case Frontend::ButtonConfig::None:
        if (ImGui::Button("Done", ImVec2(160.0f, 0.0f))) {
            Submit(static_cast<u8>(Frontend::ButtonConfig::None), true);
        }
        break;
    case Frontend::ButtonConfig::Single:
        if (ImGui::Button(button_label(2, Frontend::SWKBD_BUTTON_OKAY), ImVec2(160.0f, 0.0f))) {
            Submit(0, true);
        }
        break;
    case Frontend::ButtonConfig::Dual:
        if (ImGui::Button(button_label(0, Frontend::SWKBD_BUTTON_CANCEL), ImVec2(160.0f, 0.0f)) ||
            back_pressed) {
            Submit(0, false);
        }
        ImGui::SameLine();
        if (ImGui::Button(button_label(2, Frontend::SWKBD_BUTTON_OKAY), ImVec2(160.0f, 0.0f))) {
            Submit(1, true);
        }
        break;
    case Frontend::ButtonConfig::Triple:
        if (ImGui::Button(button_label(0, Frontend::SWKBD_BUTTON_CANCEL), ImVec2(160.0f, 0.0f)) ||
            back_pressed) {
            Submit(0, false);
        }
        ImGui::SameLine();
        if (ImGui::Button(button_label(1, Frontend::SWKBD_BUTTON_FORGOT), ImVec2(160.0f, 0.0f))) {
            Submit(1, false);
        }
        ImGui::SameLine();
        if (ImGui::Button(button_label(2, Frontend::SWKBD_BUTTON_OKAY), ImVec2(160.0f, 0.0f))) {
            Submit(2, true);
        }
        break;
    }

    ImGui::EndPopup();
}

} // namespace UWP
