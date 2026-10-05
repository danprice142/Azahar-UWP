// Copyright Citra Emulator Project / Azahar Emulator Project
// Licensed under GPLv2 or any later version
// Refer to the license.txt file included.

#include <algorithm>
#include <cctype>
#include <utility>

#include <imgui.h>

#include "citra_uwp/storage_paths.h"
#include "citra_uwp/ui/file_browser.h"
#include "citra_uwp/ui/widgets.h"
#include "citra_uwp/uwp_settings.h"
#include "common/common_paths.h"
#include "common/file_util.h"
#include "common/logging/log.h"

namespace UWP {

namespace {

/// Roots reachable from an app container; there is no drive picker.
struct Root {
    const char* label;
    const char* path;
};

std::vector<Root> KnownRoots() {
    static std::string user_dir; // needs FileUtil, so built lazily
    if (user_dir.empty()) {
        user_dir = FileUtil::GetUserPath(FileUtil::UserPath::UserDir);
    }
    return {
        {"Applications", Storage::DevelopmentApplicationsPath.data()},
        {"DevelopmentFiles", "D:\\DevelopmentFiles\\"},
        {"User dir", user_dir.c_str()},
        {"E:", "E:\\"},
        {"F:", "F:\\"},
        {"G:", "G:\\"},
        {"H:", "H:\\"},
    };
}

std::string ToLower(std::string s) {
    std::transform(s.begin(), s.end(), s.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return s;
}

} // namespace

void FileBrowser::Open(std::string dialog_title, std::vector<std::string> match_extensions,
                       std::string start_dir) {
    title = std::move(dialog_title);
    extensions.clear();
    for (auto& extension : match_extensions) {
        extensions.push_back(ToLower(std::move(extension)));
    }
    result.clear();
    is_open = true;
    focus_pending = true;

    if (start_dir.empty()) {
        start_dir =
            !last_dir.empty() ? last_dir : std::string{Storage::DevelopmentApplicationsPath};
    }
    SetDirectory(std::move(start_dir));
}

bool FileBrowser::MatchesFilter(const std::string& name) const {
    if (extensions.empty()) {
        return true;
    }
    const std::string lower = ToLower(name);
    return std::any_of(extensions.begin(), extensions.end(), [&](const std::string& ext) {
        return lower.size() >= ext.size() &&
               lower.compare(lower.size() - ext.size(), ext.size(), ext) == 0;
    });
}

void FileBrowser::SetDirectory(std::string dir) {
    if (!dir.empty() && dir.back() != '\\' && dir.back() != '/') {
        dir += '\\';
    }
    current_dir = std::move(dir);
    last_dir = current_dir;
    entries.clear();

    FileUtil::ForeachDirectoryEntry(
        nullptr, current_dir,
        [this](u64*, const std::string& directory, const std::string& virtual_name) {
            const bool is_dir = FileUtil::IsDirectory(directory + DIR_SEP + virtual_name);
            if (is_dir || MatchesFilter(virtual_name)) {
                entries.push_back({virtual_name, is_dir});
            }
            return true;
        });

    std::sort(entries.begin(), entries.end(), [](const Entry& a, const Entry& b) {
        if (a.is_directory != b.is_directory) {
            return a.is_directory;
        }
        return ToLower(a.name) < ToLower(b.name);
    });
}

std::string FileBrowser::TakeResult() {
    return std::exchange(result, std::string{});
}

void FileBrowser::Draw() {
    if (!is_open) {
        return;
    }

    const ImGuiViewport* viewport = ImGui::GetMainViewport();
    ImGui::SetNextWindowPos(viewport->GetCenter(), ImGuiCond_Appearing, ImVec2(0.5f, 0.5f));
    ImGui::SetNextWindowSize(ImVec2(viewport->Size.x * 0.6f, viewport->Size.y * 0.7f),
                             ImGuiCond_Appearing);
    if (focus_pending) {
        ImGui::SetNextWindowFocus();
        focus_pending = false;
    }

    bool keep_open = true;
    if (ImGui::Begin(title.c_str(), nullptr,
                     ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoSavedSettings)) {
        Widgets::HandleGamepadBack(keep_open);
        for (const auto& root : KnownRoots()) {
            if (ImGui::SmallButton(root.label)) {
                SetDirectory(root.path);
            }
            ImGui::SameLine();
        }
        if (ImGui::SmallButton("Cancel")) {
            keep_open = false;
        }

        ImGui::TextDisabled("%s", current_dir.c_str());
        ImGui::Separator();

        if (ImGui::BeginChild("##entries", ImVec2(0, 0), ImGuiChildFlags_None)) {
            // ".." unless this already is a drive root like "D:\".
            if (current_dir.size() > 3) {
                if (ImGui::Selectable("[..]")) {
                    std::string parent = current_dir;
                    parent.pop_back(); // trailing separator
                    const std::size_t cut = parent.find_last_of("\\/");
                    if (cut != std::string::npos) {
                        SetDirectory(parent.substr(0, cut + 1));
                    }
                }
            }
            // SetDirectory invalidates `entries`, so iterate by index.
            for (std::size_t i = 0; i < entries.size(); ++i) {
                const Entry& entry = entries[i];
                const std::string label = entry.is_directory ? "[" + entry.name + "]" : entry.name;
                if (ImGui::Selectable(label.c_str())) {
                    if (entry.is_directory) {
                        SetDirectory(current_dir + entry.name);
                        break;
                    }
                    result = current_dir + entry.name;
                    keep_open = false;
                    break;
                }
            }
        }
        ImGui::EndChild();
    }
    ImGui::End();

    if (!keep_open) {
        is_open = false;
    }
}

} // namespace UWP
