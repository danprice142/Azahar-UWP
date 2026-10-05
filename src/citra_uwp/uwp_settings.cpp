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
