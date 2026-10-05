// Copyright Citra Emulator Project / Azahar Emulator Project
// Licensed under GPLv2 or any later version
// Refer to the license.txt file included.

#pragma once

#include <string>

namespace UWP {

/// Floating secondary-screen surface used by the Separate Windows layout.
class SecondaryScreenWindow {
public:
    void Draw(unsigned int texture, int width, int height);

    bool IsOpen() const {
        return is_open;
    }

    void SetOpen(bool open) {
        is_open = open;
    }

private:
    bool is_open = true;
};

/// Displays the rich-presence payload that cannot be published from the app container.
class DiscordPresencePopup {
public:
    void Draw();

    void Open() {
        is_open = true;
    }

    bool IsOpen() const {
        return is_open;
    }

    void SetPresence(std::string title, std::string details);

private:
    bool is_open = false;
    std::string game_title = "Not playing";
    std::string game_details = "Idle";
};

} // namespace UWP
