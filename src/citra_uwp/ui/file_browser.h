// Copyright Citra Emulator Project / Azahar Emulator Project
// Licensed under GPLv2 or any later version
// Refer to the license.txt file included.

#pragma once

#include <string>
#include <vector>

namespace UWP {

/**
 * A gamepad-navigable file picker shared by every pick-a-file action; the caller consumes the
 * choice through TakeResult.
 */
class FileBrowser {
public:
    /// Extensions include the dot and are matched case-insensitively; an empty list shows every
    /// file.
    void Open(std::string dialog_title, std::vector<std::string> match_extensions,
              std::string start_dir = {});

    void Draw();

    /// Returns the picked path exactly once after a pick, empty otherwise.
    std::string TakeResult();

    bool IsOpen() const {
        return is_open;
    }

private:
    struct Entry {
        std::string name;
        bool is_directory;
    };

    void SetDirectory(std::string dir);
    bool MatchesFilter(const std::string& name) const;

    bool is_open = false;
    bool focus_pending = false;
    std::string title;
    std::vector<std::string> extensions;
    std::string current_dir;
    std::vector<Entry> entries;
    std::string result;
    std::string last_dir;
};

} // namespace UWP
