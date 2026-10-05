// Copyright Citra Emulator Project / Azahar Emulator Project
// Licensed under GPLv2 or any later version
// Refer to the license.txt file included.

#include <SDL.h>

#include <imgui.h>

#include "citra_uwp/ui/virtual_keyboard.h"

namespace UWP {

VirtualKeyboard& GetVirtualKeyboard() {
    static VirtualKeyboard keyboard;
    return keyboard;
}

bool VirtualKeyboard::InputText(const char* label, char* buffer, std::size_t buffer_size,
                                const char* hint, bool password) {
    ImGuiInputTextFlags flags = ImGuiInputTextFlags_None;
    if (password) {
        flags |= ImGuiInputTextFlags_Password;
    }

    return (hint != nullptr) ? ImGui::InputTextWithHint(label, hint, buffer, buffer_size, flags)
                             : ImGui::InputText(label, buffer, buffer_size, flags);
}

bool VirtualKeyboard::InputTextMultiline(const char* label, char* buffer, std::size_t buffer_size,
                                         const ImVec2& size) {
    return ImGui::InputTextMultiline(label, buffer, buffer_size, size, ImGuiInputTextFlags_None);
}

bool VirtualKeyboard::IsOpen() const {
    return SDL_IsTextInputShown() == SDL_TRUE;
}

} // namespace UWP
