// Copyright Citra Emulator Project / Azahar Emulator Project
// Licensed under GPLv2 or any later version
// Refer to the license.txt file included.

#include <utility>

#include <imgui.h>

#include "citra_uwp/ui/popups.h"
#include "citra_uwp/ui/widgets.h"
#include "common/settings.h"

namespace UWP {

void SecondaryScreenWindow::Draw(unsigned int texture, int width, int height) {
    if (Settings::values.layout_option.GetValue() != Settings::LayoutOption::SeparateWindows) {
        return;
    }
    if (!is_open) {
        return;
    }

    // Opens beside the main screen the first time, then remembers wherever the user drags it.
    ImGui::SetNextWindowSize(ImVec2(400.0f, 340.0f), ImGuiCond_FirstUseEver);
    if (ImGui::Begin("Secondary Screen", nullptr)) {
        Widgets::HandleGamepadBack(is_open);
        if (texture != 0 && width > 0 && height > 0) {
            // Letterbox to the 3DS aspect ratio, like the desktop secondary window.
            const ImVec2 available = ImGui::GetContentRegionAvail();
            const float aspect = static_cast<float>(width) / static_cast<float>(height);
            float draw_width = available.x;
            float draw_height = draw_width / aspect;
            if (draw_height > available.y) {
                draw_height = available.y;
                draw_width = draw_height * aspect;
            }
            ImGui::SetCursorPosX(ImGui::GetCursorPosX() + (available.x - draw_width) * 0.5f);
            ImGui::Image(static_cast<ImTextureID>(static_cast<std::uintptr_t>(texture)),
                         ImVec2(draw_width, draw_height));
        } else {
            ImGui::TextDisabled("Nothing is running.");
        }
    }
    ImGui::End();
}

void DiscordPresencePopup::SetPresence(std::string title, std::string details) {
    game_title = std::move(title);
    game_details = std::move(details);
}

void DiscordPresencePopup::Draw() {
    if (!is_open) {
        return;
    }

    const ImGuiViewport* viewport = ImGui::GetMainViewport();
    ImGui::SetNextWindowPos(ImVec2(viewport->WorkPos.x + viewport->WorkSize.x * 0.5f,
                                   viewport->WorkPos.y + viewport->WorkSize.y * 0.5f),
                            ImGuiCond_Appearing, ImVec2(0.5f, 0.5f));
    ImGui::SetNextWindowSize(ImVec2(520.0f, 0.0f), ImGuiCond_Appearing);

    if (ImGui::Begin("Rich Presence", nullptr, ImGuiWindowFlags_NoSavedSettings)) {
        Widgets::HandleGamepadBack(is_open);
        ImGui::TextWrapped(
            "Discord's client is reached over a named pipe, which an app container cannot open, "
            "so presence cannot be published from this platform. This is what would be shared:");
        ImGui::Spacing();
        ImGui::Separator();
        ImGui::Spacing();

        ImGui::TextDisabled("Playing");
        ImGui::SameLine();
        ImGui::TextUnformatted(game_title.c_str());

        ImGui::TextDisabled("Details");
        ImGui::SameLine();
        ImGui::TextUnformatted(game_details.c_str());

        ImGui::Spacing();
        ImGui::Separator();
        ImGui::Spacing();
        if (ImGui::Button("Close", ImVec2(120.0f, 0.0f))) {
            is_open = false;
        }
    }
    ImGui::End();
}

} // namespace UWP
