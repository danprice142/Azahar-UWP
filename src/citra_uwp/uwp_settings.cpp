// Copyright Citra Emulator Project / Azahar Emulator Project
// Licensed under GPLv2 or any later version
// Refer to the license.txt file included.

#include "citra_uwp/uwp_settings.h"

#include <algorithm>
#include <SDL.h>

#include "common/param_package.h"
#include "input_common/udp/udp.h"

namespace UWP {

Values::Values() {
    const auto local_button = [](int button) {
        Common::ParamPackage params{{"engine", "sdl"}, {"api", "controller"}};
        params.Set("guid", "0");
        params.Set("port", 0);
        params.Set("maptype", "all");
        params.Set("button", button);
        return params.Serialize();
    };
    in_game_menu_chord[0] =
        InputCommon::CemuhookUDP::GenerateAddonButtonParam(local_button(SDL_CONTROLLER_BUTTON_BACK),
                                                           InputCommon::CemuhookUDP::Button::Share)
            .Serialize();
    in_game_menu_chord[1] =
        InputCommon::CemuhookUDP::GenerateAddonButtonParam(
            local_button(SDL_CONTROLLER_BUTTON_START), InputCommon::CemuhookUDP::Button::Options)
            .Serialize();

    // Desktop frontend defaults.
    const auto key = [this](ControllerHotkey hotkey, const char* chord) {
        keyboard_hotkeys[static_cast<std::size_t>(hotkey)] = chord;
    };
    key(ControllerHotkey::TogglePause, "F4");
    key(ControllerHotkey::StopEmulation, "F5");
    key(ControllerHotkey::RestartEmulation, "F6");
    key(ControllerHotkey::CaptureScreenshot, "Ctrl+P");
    key(ControllerHotkey::SwapScreens, "F9");
    key(ControllerHotkey::ToggleUprightScreens, "F8");
    key(ControllerHotkey::CycleLayout, "F10");
    key(ControllerHotkey::ToggleMute, "Ctrl+M");
    key(ControllerHotkey::IncreaseSpeed, "+");
    key(ControllerHotkey::DecreaseSpeed, "-");
    key(ControllerHotkey::Increase3D, "Ctrl++");
    key(ControllerHotkey::Decrease3D, "Ctrl+-");
    key(ControllerHotkey::LoadFile, "Ctrl+O");
    key(ControllerHotkey::Exit, "Ctrl+Q");
    key(ControllerHotkey::LoadAmiibo, "F2");
    key(ControllerHotkey::RemoveAmiibo, "F3");
    key(ControllerHotkey::SaveToOldestSlot, "Ctrl+C");
    key(ControllerHotkey::LoadFromNewestSlot, "Ctrl+V");
    key(ControllerHotkey::ToggleFilterBar, "Ctrl+F");
    key(ControllerHotkey::ToggleStatusBar, "Ctrl+S");
    key(ControllerHotkey::ToggleCustomTextures, "F7");
    key(ControllerHotkey::BrowsePublicRooms, "Ctrl+B");
    key(ControllerHotkey::CreateRoom, "Ctrl+N");
    key(ControllerHotkey::LeaveRoom, "Ctrl+L");
    key(ControllerHotkey::ShowCurrentRoom, "Ctrl+R");
}

Values values;

void AddRecentFile(std::string path) {
    if (path.empty()) {
        return;
    }
    auto& files = values.recent_files;
    std::erase(files, path);
    files.insert(files.begin(), std::move(path));
    if (files.size() > MaxRecentFiles) {
        files.resize(MaxRecentFiles);
    }
}

} // namespace UWP
